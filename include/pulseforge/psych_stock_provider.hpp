#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace pulseforge::psych_stock {

struct StockProviderDiscovery {
    std::filesystem::path assets_root;
    std::size_t score{};
    std::size_t packs_scanned{};
    std::size_t candidates_scanned{};
    bool scan_truncated{};

    [[nodiscard]] bool found() const noexcept {
        return !assets_root.empty();
    }
};

namespace detail {

[[nodiscard]] inline bool path_is_directory(
    const std::filesystem::path& path
) noexcept {
    std::error_code error;
    return std::filesystem::is_directory(path, error) && !error;
}

[[nodiscard]] inline std::filesystem::path normalize(
    const std::filesystem::path& input
) {
    if (input.empty()) return {};
    std::error_code error;
    auto absolute = std::filesystem::absolute(input, error);
    if (error) {
        error.clear();
        absolute = input;
    }
    auto canonical = std::filesystem::weakly_canonical(absolute, error);
    return error ? absolute.lexically_normal() : canonical.lexically_normal();
}

[[nodiscard]] inline std::string key(const std::filesystem::path& path) {
    const auto utf8 = normalize(path).generic_u8string();
    std::string value;
    value.reserve(utf8.size());
    for (const auto unit : utf8) {
        value.push_back(static_cast<char>(unit));
    }
#if defined(_WIN32)
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
#endif
    while (value.size() > 1U && value.back() == '/') value.pop_back();
    return value;
}

[[nodiscard]] inline bool within(
    const std::filesystem::path& child,
    const std::filesystem::path& parent
) {
    const auto child_key = key(child);
    auto parent_key = key(parent);
    if (child_key.empty() || parent_key.empty()) return false;
    if (child_key == parent_key) return true;
    if (parent_key.back() != '/') parent_key.push_back('/');
    return child_key.size() > parent_key.size()
        && child_key.compare(0U, parent_key.size(), parent_key) == 0;
}

// Source-corpus-backed FNF/Psych stock song sentinels. A custom mod containing
// one or two coincidentally named songs is not enough to become the global
// stock provider; a provider must expose a meaningful baseline set.
inline constexpr std::array<std::string_view, 26U> stock_song_ids{
    "tutorial", "bopeebo", "fresh", "dad-battle", "spookeez", "south",
    "monster", "pico", "philly-nice", "blammed", "satin-panties", "high",
    "milf", "cocoa", "eggnog", "winter-horrorland", "senpai", "roses",
    "thorns", "ugh", "guns", "stress", "darnell", "lit-up", "2hot", "blazin",
};

[[nodiscard]] inline std::optional<std::filesystem::path>
find_installation_root(const std::filesystem::path& seed) {
    if (seed.empty()) return std::nullopt;
    const auto normalized_seed = normalize(seed);
    auto cursor = normalized_seed;
    std::optional<std::filesystem::path> outermost;

    constexpr std::size_t maximum_ancestor_hops = 12U;
    for (std::size_t hop = 0U; hop < maximum_ancestor_hops; ++hop) {
        const auto assets = cursor / "assets";
        const auto mods = cursor / "mods";
        if (path_is_directory(assets) && path_is_directory(mods)
            && (key(cursor) == key(normalized_seed)
                || within(normalized_seed, assets)
                || within(normalized_seed, mods))) {
            // Keep walking so a nested imported engine can still discover the
            // outer PulseForge installation that owns its sibling mod packs.
            outermost = cursor;
        }
        const auto parent = cursor.parent_path();
        if (parent.empty() || parent == cursor) break;
        cursor = parent;
    }
    return outermost;
}

[[nodiscard]] inline std::size_t provider_score(
    const std::filesystem::path& assets
) noexcept {
    if (!path_is_directory(assets)) return 0U;

    std::size_t songs = 0U;
    for (const auto id : stock_song_ids) {
        if (path_is_directory(assets / "songs" / id)) ++songs;
    }
    // Four independent stock songs is intentionally conservative: the source
    // corpus' complete engines contain dozens, while ordinary mods generally
    // contain only their own song set.
    if (songs < 4U) return 0U;

    std::size_t topology = 0U;
    for (const auto& relative : std::array{
             std::filesystem::path{"images"},
             std::filesystem::path{"songs"},
             std::filesystem::path{"sounds"},
             std::filesystem::path{"music"},
             std::filesystem::path{"shared"},
             std::filesystem::path{"characters"},
             std::filesystem::path{"data"},
         }) {
        if (path_is_directory(assets / relative)) ++topology;
    }
    return songs * 100U + topology;
}

inline void append_directory_children(
    const std::filesystem::path& parent,
    std::vector<std::filesystem::path>& output,
    const std::size_t maximum_children,
    bool& truncated
) {
    std::error_code error;
    std::filesystem::directory_iterator iterator(parent, error);
    if (error) return;
    for (const auto& entry : iterator) {
        if (output.size() >= maximum_children) {
            truncated = true;
            break;
        }
        std::error_code type_error;
        if (entry.is_directory(type_error) && !type_error) {
            output.push_back(entry.path());
        }
    }
    std::sort(output.begin(), output.end(), [](const auto& left, const auto& right) {
        return key(left) < key(right);
    });
}

}  // namespace detail

[[nodiscard]] inline bool is_stock_song_id(
    const std::string_view canonical_song_id
) noexcept {
    return std::find(
        detail::stock_song_ids.begin(),
        detail::stock_song_ids.end(),
        canonical_song_id
    ) != detail::stock_song_ids.end();
}

// PULSEFORGE_P1_2_0_STOCK_PROVIDER_DISCOVERY_V1
// Finds ONE complete FNF/Psych stock asset provider under the installation's
// mods/ directory. This deliberately avoids mounting every sibling mod and
// therefore prevents cross-mod asset contamination. Discovery is bounded and
// deterministic; ties are resolved by normalized lexicographic path.
[[nodiscard]] inline StockProviderDiscovery discover_stock_provider(
    const std::filesystem::path& seed,
    std::size_t maximum_packs = 256U,
    std::size_t maximum_children_per_pack = 64U,
    std::size_t maximum_candidates = 1'024U
) {
    StockProviderDiscovery result;
    maximum_packs = std::clamp<std::size_t>(maximum_packs, 1U, 256U);
    maximum_children_per_pack = std::clamp<std::size_t>(
        maximum_children_per_pack, 1U, 64U
    );
    maximum_candidates = std::clamp<std::size_t>(
        maximum_candidates, 1U, 1'024U
    );

    const auto installation = detail::find_installation_root(seed);
    if (!installation.has_value()) return result;

    std::vector<std::filesystem::path> packs;
    detail::append_directory_children(
        *installation / "mods", packs, maximum_packs, result.scan_truncated
    );
    result.packs_scanned = packs.size();

    std::vector<std::filesystem::path> candidates;
    candidates.reserve(std::min<std::size_t>(maximum_candidates, 256U));
    const auto append_candidate = [&](const std::filesystem::path& candidate) {
        if (candidates.size() >= maximum_candidates) {
            result.scan_truncated = true;
            return;
        }
        if (!detail::path_is_directory(candidate)) return;
        const auto candidate_key = detail::key(candidate);
        if (std::none_of(
                candidates.begin(), candidates.end(), [&](const auto& current) {
                    return detail::key(current) == candidate_key;
                })) {
            candidates.push_back(detail::normalize(candidate));
        }
    };

    for (const auto& pack : packs) {
        append_candidate(pack / "assets");
        append_candidate(pack / "bin" / "assets");

        std::vector<std::filesystem::path> children;
        detail::append_directory_children(
            pack, children, maximum_children_per_pack, result.scan_truncated
        );
        for (const auto& child : children) {
            append_candidate(child / "assets");
            append_candidate(child / "bin" / "assets");
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        return detail::key(left) < detail::key(right);
    });
    result.candidates_scanned = candidates.size();

    for (const auto& candidate : candidates) {
        const auto score = detail::provider_score(candidate);
        if (score == 0U) continue;
        if (!result.found() || score > result.score
            || (score == result.score
                && detail::key(candidate) < detail::key(result.assets_root))) {
            result.assets_root = candidate;
            result.score = score;
        }
    }
    return result;
}

// Session/process-local bounded cache. Content roots are immutable during a
// gameplay session, and resolving them for every playSound/precache call would
// otherwise rescan a large mods directory repeatedly.
[[nodiscard]] inline StockProviderDiscovery discover_stock_provider_cached(
    const std::filesystem::path& seed
) {
    const auto installation = detail::find_installation_root(seed);
    if (!installation.has_value()) return {};
    const auto cache_key = detail::key(*installation);

    using CacheEntry = std::pair<std::string, StockProviderDiscovery>;
    static std::mutex cache_mutex;
    static std::vector<CacheEntry> cache;
    constexpr std::size_t maximum_cached_installations = 16U;

    {
        const std::lock_guard lock(cache_mutex);
        const auto found = std::find_if(
            cache.begin(), cache.end(), [&](const auto& entry) {
                return entry.first == cache_key;
            }
        );
        if (found != cache.end()) return found->second;
    }

    auto discovery = discover_stock_provider(seed);
    {
        const std::lock_guard lock(cache_mutex);
        const auto found = std::find_if(
            cache.begin(), cache.end(), [&](const auto& entry) {
                return entry.first == cache_key;
            }
        );
        if (found != cache.end()) return found->second;
        if (cache.size() >= maximum_cached_installations) {
            cache.erase(cache.begin());
        }
        cache.emplace_back(cache_key, discovery);
    }
    return discovery;
}

}  // namespace pulseforge::psych_stock
