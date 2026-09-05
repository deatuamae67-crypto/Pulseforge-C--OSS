#pragma once

#include "pulseforge/psych_stock_provider.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace pulseforge::detail {

// Roots are LOWEST -> HIGHEST precedence, exactly matching RuntimeScene's VFS.
// Discovered Psych roots are fallback layers only; explicit caller roots are
// always emitted after them and therefore remain authoritative.
struct PsychContentRootResolution {
    std::vector<std::filesystem::path> roots;
    std::vector<std::filesystem::path> discovered_fallback_roots;
    // PULSEFORGE_P1_2_0_STOCK_PROVIDER_RESULT_V1
    // A single complete sibling engine may provide stock FNF/Psych content.
    // These roots are always the LOWEST-precedence subset of discovered roots.
    std::vector<std::filesystem::path> stock_fallback_roots;
    std::optional<std::filesystem::path> stock_provider_assets_root;
    std::size_t stock_provider_score{};
    std::size_t stock_provider_packs_scanned{};
    std::size_t stock_provider_candidates_scanned{};
    bool stock_provider_scan_truncated{};
    std::size_t omitted_due_to_limit{};
};

namespace psych_content_roots_detail {

[[nodiscard]] inline bool path_is_directory(
    const std::filesystem::path& path
) noexcept {
    std::error_code error;
    return std::filesystem::is_directory(path, error) && !error;
}

[[nodiscard]] inline std::filesystem::path normalize_path(
    const std::filesystem::path& input
) {
    if (input.empty()) return {};

    std::error_code error;
    auto absolute = std::filesystem::absolute(input, error);
    if (error) {
        absolute = input;
        error.clear();
    }

    auto canonical = std::filesystem::weakly_canonical(absolute, error);
    if (!error) return canonical.lexically_normal();
    return absolute.lexically_normal();
}

// PULSEFORGE_P1_1_7_1_UTF8_PATH_KEY_V1
// Use UTF-8 directly instead of narrowing native Windows paths through the
// active ANSI code page.
[[nodiscard]] inline std::string path_key(
    const std::filesystem::path& path
) {
    const auto utf8 = normalize_path(path).generic_u8string();
    std::string key;
    key.reserve(utf8.size());
    for (const auto unit : utf8) {
        key.push_back(static_cast<char>(unit));
    }
#if defined(_WIN32)
    // Fold ASCII only; never mutate UTF-8 multibyte sequences.
    for (auto& value : key) {
        if (value >= 'A' && value <= 'Z') {
            value = static_cast<char>(value - 'A' + 'a');
        }
    }
#endif
    while (key.size() > 1U && key.back() == '/') key.pop_back();
    return key;
}

[[nodiscard]] inline bool path_is_within(
    const std::filesystem::path& child,
    const std::filesystem::path& parent
) {
    const auto child_key = path_key(child);
    auto parent_key = path_key(parent);
    if (child_key.empty() || parent_key.empty()) return false;
    if (child_key == parent_key) return true;
    if (parent_key.back() != '/') parent_key.push_back('/');
    return child_key.size() > parent_key.size()
        && child_key.compare(0U, parent_key.size(), parent_key) == 0;
}

// Find the nearest credible Psych/FNF distribution. We do not scan arbitrary
// drives: the ancestor must own assets/, and the seed must be that root,
// inside assets/, or inside a sibling mods/.
[[nodiscard]] inline std::optional<std::filesystem::path>
find_distribution_root(const std::filesystem::path& seed) {
    if (seed.empty()) return std::nullopt;

    const auto normalized_seed = normalize_path(seed);
    auto cursor = normalized_seed;

    constexpr std::size_t maximum_ancestor_hops = 10U;
    for (std::size_t hop = 0U; hop < maximum_ancestor_hops; ++hop) {
        const auto assets = cursor / "assets";
        if (path_is_directory(assets)) {
            const bool seed_is_distribution =
                path_key(cursor) == path_key(normalized_seed);
            const bool seed_is_asset_content =
                path_is_within(normalized_seed, assets);
            const auto mods = cursor / "mods";
            const bool seed_is_mod_content =
                path_is_directory(mods) && path_is_within(normalized_seed, mods);

            if (seed_is_distribution || seed_is_asset_content
                || seed_is_mod_content) {
                return cursor;
            }
        }

        const auto parent = cursor.parent_path();
        if (parent.empty() || parent == cursor) break;
        cursor = parent;
    }
    return std::nullopt;
}

inline void append_unique_existing(
    std::vector<std::filesystem::path>& output,
    std::unordered_set<std::string>& keys,
    const std::unordered_set<std::string>& explicit_keys,
    const std::filesystem::path& candidate
) {
    if (!path_is_directory(candidate)) return;
    const auto normalized = normalize_path(candidate);
    const auto key = path_key(normalized);
    if (key.empty() || explicit_keys.contains(key) || keys.contains(key)) return;
    keys.insert(key);
    output.push_back(normalized);
}

inline void append_unique_explicit(
    std::vector<std::filesystem::path>& output,
    std::unordered_set<std::string>& keys,
    const std::filesystem::path& candidate
) {
    if (candidate.empty()) return;
    const auto normalized = normalize_path(candidate);
    const auto key = path_key(normalized);
    if (key.empty() || keys.contains(key)) return;
    keys.insert(key);
    output.push_back(normalized);
}

// PULSEFORGE_P1_2_0_COMPLETE_ASSET_LAYERS_V1
// Source corpus layouts include all of these roots. `assets/data` was
// previously omitted, which made valid `assets/data/characters/...` and other
// descriptors appear missing despite being present on disk.
inline void append_asset_layers(
    std::vector<std::filesystem::path>& output,
    std::unordered_set<std::string>& keys,
    const std::unordered_set<std::string>& explicit_keys,
    const std::filesystem::path& assets
) {
    append_unique_existing(output, keys, explicit_keys, assets);
    append_unique_existing(output, keys, explicit_keys, assets / "data");
    append_unique_existing(output, keys, explicit_keys, assets / "preload");
    append_unique_existing(
        output, keys, explicit_keys, assets / "preload" / "data"
    );
    append_unique_existing(output, keys, explicit_keys, assets / "shared");
    append_unique_existing(
        output, keys, explicit_keys, assets / "shared" / "data"
    );
}

}  // namespace psych_content_roots_detail

// PULSEFORGE_1_0_0_SELECTED_SCRIPT_ROOT_ISOLATION_V1
// Executable script discovery is deliberately narrower than visual/content
// fallback discovery. When the launcher selected a concrete mod or content
// root, only that selection may contribute Lua. This prevents an unrelated
// sibling mod from returning Function_Stop from onStartCountdown() and holding
// the active chart at t=0. Direct CLI launches without a catalog selection keep
// the legacy caller-provided root set.
[[nodiscard]] inline std::vector<std::filesystem::path>
select_psych_executable_roots(
    const std::span<const std::filesystem::path> content_roots,
    const std::filesystem::path& selected_content_root,
    const std::filesystem::path& selected_mod_root
) {
    using namespace psych_content_roots_detail;

    std::vector<std::filesystem::path> result;
    result.reserve(content_roots.size() + 2U);
    std::unordered_set<std::string> keys;
    keys.reserve(content_roots.size() * 2U + 5U);

    const auto append = [&](const std::filesystem::path& candidate) {
        if (candidate.empty()) return;
        const auto normalized = normalize_path(candidate);
        const auto candidate_key = path_key(normalized);
        if (candidate_key.empty() || keys.contains(candidate_key)) return;
        keys.insert(candidate_key);
        result.push_back(normalized);
    };

    if (!selected_mod_root.empty()) {
        append(selected_content_root);
        append(selected_mod_root);
        return result;
    }
    if (!selected_content_root.empty()) {
        append(selected_content_root);
        return result;
    }

    for (const auto& root : content_roots) append(root);
    return result;
}

// Generic Psych/FNF layout expansion.
// Supported companion layers:
//   ONE complete sibling stock provider (lowest precedence), then
//   <dist>/assets
//   <dist>/assets/data
//   <dist>/assets/preload
//   <dist>/assets/preload/data
//   <dist>/assets/shared
//   <dist>/assets/shared/data
// then every explicit caller root.
//
// The stock provider is deliberately a single deterministic complete engine;
// we never layer arbitrary sibling mods over one another. Set
// include_stock_provider=false for executable/script-code discovery where a
// sibling engine must not inject global Lua into the selected mod.
[[nodiscard]] inline PsychContentRootResolution resolve_psych_content_roots(
    const std::span<const std::filesystem::path> explicit_input_roots,
    std::size_t maximum_roots = 64U,
    const bool include_stock_provider = true
) {
    using namespace psych_content_roots_detail;

    maximum_roots = std::clamp<std::size_t>(maximum_roots, 1U, 64U);

    std::vector<std::filesystem::path> explicit_roots;
    explicit_roots.reserve(explicit_input_roots.size());
    std::unordered_set<std::string> explicit_keys;
    explicit_keys.reserve(explicit_input_roots.size() * 2U + 1U);

    for (const auto& root : explicit_input_roots) {
        append_unique_explicit(explicit_roots, explicit_keys, root);
    }

    std::vector<std::filesystem::path> fallback_roots;
    fallback_roots.reserve(explicit_roots.size() * 6U + 6U);
    std::unordered_set<std::string> fallback_keys;
    fallback_keys.reserve(explicit_roots.size() * 12U + 13U);

    PsychContentRootResolution result;
    std::vector<std::filesystem::path> requested_stock_roots;

    if (include_stock_provider) {
        psych_stock::StockProviderDiscovery best;
        for (const auto& explicit_root : explicit_roots) {
            const auto candidate = psych_stock::discover_stock_provider_cached(
                explicit_root
            );
            if (!candidate.found()) {
                best.scan_truncated = best.scan_truncated
                    || candidate.scan_truncated;
                best.packs_scanned = std::max(
                    best.packs_scanned, candidate.packs_scanned
                );
                best.candidates_scanned = std::max(
                    best.candidates_scanned, candidate.candidates_scanned
                );
                continue;
            }
            if (!best.found() || candidate.score > best.score
                || (candidate.score == best.score
                    && psych_stock::detail::key(candidate.assets_root)
                        < psych_stock::detail::key(best.assets_root))) {
                best = candidate;
            }
        }
        if (best.found()) {
            result.stock_provider_assets_root = best.assets_root;
            result.stock_provider_score = best.score;
            result.stock_provider_packs_scanned = best.packs_scanned;
            result.stock_provider_candidates_scanned = best.candidates_scanned;
            result.stock_provider_scan_truncated = best.scan_truncated;
            const auto before = fallback_roots.size();
            append_asset_layers(
                fallback_roots, fallback_keys, explicit_keys, best.assets_root
            );
            requested_stock_roots.assign(
                fallback_roots.begin() + static_cast<std::ptrdiff_t>(before),
                fallback_roots.end()
            );
        } else {
            result.stock_provider_scan_truncated = best.scan_truncated;
            result.stock_provider_packs_scanned = best.packs_scanned;
            result.stock_provider_candidates_scanned = best.candidates_scanned;
        }
    }

    for (const auto& explicit_root : explicit_roots) {
        const auto distribution = find_distribution_root(explicit_root);
        if (!distribution.has_value()) continue;
        append_asset_layers(
            fallback_roots,
            fallback_keys,
            explicit_keys,
            *distribution / "assets"
        );
    }

    // Explicit/high-precedence roots are never sacrificed for optional fallbacks.
    if (explicit_roots.size() >= maximum_roots) {
        const auto first = explicit_roots.end()
            - static_cast<std::ptrdiff_t>(maximum_roots);
        result.roots.assign(first, explicit_roots.end());
        result.omitted_due_to_limit =
            explicit_roots.size() - result.roots.size() + fallback_roots.size();
        return result;
    }

    const auto fallback_budget = maximum_roots - explicit_roots.size();
    const auto fallback_keep = std::min(fallback_budget, fallback_roots.size());
    const auto first = fallback_roots.end()
        - static_cast<std::ptrdiff_t>(fallback_keep);

    result.discovered_fallback_roots.assign(first, fallback_roots.end());
    for (const auto& stock_root : requested_stock_roots) {
        const auto stock_key = path_key(stock_root);
        if (std::any_of(
                result.discovered_fallback_roots.begin(),
                result.discovered_fallback_roots.end(),
                [&](const auto& kept) { return path_key(kept) == stock_key; })) {
            result.stock_fallback_roots.push_back(stock_root);
        }
    }
    result.roots = result.discovered_fallback_roots;
    result.roots.insert(
        result.roots.end(), explicit_roots.begin(), explicit_roots.end()
    );
    result.omitted_due_to_limit = fallback_roots.size() - fallback_keep;
    return result;
}

}  // namespace pulseforge::detail
