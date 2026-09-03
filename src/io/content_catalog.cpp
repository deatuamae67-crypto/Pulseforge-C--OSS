#include "pulseforge/content_catalog.hpp"
#include "pulseforge/content_descriptors.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace pulseforge {
namespace {

using Json = nlohmann::json;

constexpr std::uintmax_t maximum_metadata_bytes = 2U * 1024U * 1024U;
constexpr std::uintmax_t maximum_catalog_cache_bytes =
    128U * 1024U * 1024U;
constexpr std::size_t maximum_catalog_cache_entries = 250'000U;
constexpr std::uint32_t catalog_cache_schema = 1U;

struct LayoutCacheEntry {
    std::uintmax_t size{};
    std::int64_t modified_ticks{};
    ContentLayout layout{ContentLayout::unknown};
};

struct LayoutCache {
    std::unordered_map<std::string, LayoutCacheEntry> loaded;
    std::unordered_map<std::string, LayoutCacheEntry> current;
    std::filesystem::path path;
    std::size_t probe_bytes{};
    bool enabled{};
};

struct IndexedFile {
    std::filesystem::path path;
    std::filesystem::path root;
    std::uintmax_t size{};
    std::size_t root_order{};
    std::string relative_key;
};

struct ModInfo {
    std::string id{"base"};
    std::string name{"Base Game"};
    std::filesystem::path root;
    std::filesystem::path content_root;
    std::optional<std::filesystem::path> manifest_path;
    std::optional<std::filesystem::path> entry_chart;
    std::vector<std::filesystem::path> entry_scripts;
    ContentProfile profile{ContentProfile::unknown};
    std::size_t root_order{};
    std::size_t local_order{};
    std::size_t order{};
    bool enabled{true};
};

struct WeekInfo {
    std::string title;
    std::string song_title;
    std::size_t song_order{};
};

struct ModListEntry {
    std::string folder;
    std::string key;
    std::size_t order{};
    bool enabled{true};
};

struct RootInfo {
    std::filesystem::path path;
    std::size_t order{};
    bool mod_container{};
    std::optional<std::filesystem::path> mods_list_path;
    std::vector<ModListEntry> mods_list;
    std::unordered_map<std::string, std::size_t> mods_list_lookup;
};

[[nodiscard]] char ascii_lower(const char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value - 'A' + 'a');
    }
    return value;
}

[[nodiscard]] std::string lower_ascii(const std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), ascii_lower);
    return result;
}

[[nodiscard]] std::string trim_ascii(const std::string_view value) {
    std::size_t first = 0;
    while (first < value.size()) {
        const char current = value[first];
        if (current != ' ' && current != '\t' && current != '\r'
            && current != '\n') {
            break;
        }
        ++first;
    }
    std::size_t last = value.size();
    while (last > first) {
        const char current = value[last - 1];
        if (current != ' ' && current != '\t' && current != '\r'
            && current != '\n') {
            break;
        }
        --last;
    }
    return std::string(value.substr(first, last - first));
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {
        reinterpret_cast<const char*>(value.data()),
        value.size(),
    };
}

[[nodiscard]] bool equal_ascii_insensitive(
    const std::string_view left,
    const std::string_view right
) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string slugify(const std::string_view text) {
    std::string result;
    result.reserve(text.size());
    bool separator = false;
    for (const char raw : text) {
        const auto value = static_cast<unsigned char>(raw);
        if ((value >= 'a' && value <= 'z')
            || (value >= '0' && value <= '9')) {
            result.push_back(static_cast<char>(value));
            separator = false;
        } else if (value >= 'A' && value <= 'Z') {
            result.push_back(static_cast<char>(value - 'A' + 'a'));
            separator = false;
        } else if (value >= 0x80U) {
            result.push_back(raw);
            separator = false;
        } else if (!result.empty() && !separator) {
            result.push_back('-');
            separator = true;
        }
    }
    while (!result.empty() && result.back() == '-') {
        result.pop_back();
    }
    return result.empty() ? "untitled" : result;
}

[[nodiscard]] std::string display_name(const std::string_view slug) {
    std::string result(slug);
    bool capitalize = true;
    for (char& value : result) {
        if (value == '-' || value == '_') {
            value = ' ';
            capitalize = true;
        } else if (capitalize && value >= 'a' && value <= 'z') {
            value = static_cast<char>(value - 'a' + 'A');
            capitalize = false;
        } else {
            capitalize = false;
        }
    }
    return result.empty() ? "Untitled" : result;
}

[[nodiscard]] std::filesystem::path normalized_absolute(
    const std::filesystem::path& path
) {
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(path, error);
    if (error) {
        error.clear();
        result = std::filesystem::absolute(path, error);
    }
    return error ? path.lexically_normal() : result.lexically_normal();
}

[[nodiscard]] std::string path_key(const std::filesystem::path& path) {
    return lower_ascii(path_utf8(normalized_absolute(path)));
}

[[nodiscard]] bool path_less(
    const std::filesystem::path& left,
    const std::filesystem::path& right
) {
    const auto left_text = path_utf8(left);
    const auto right_text = path_utf8(right);
    const auto left_key = lower_ascii(left_text);
    const auto right_key = lower_ascii(right_text);
    if (left_key != right_key) {
        return left_key < right_key;
    }
    return left_text < right_text;
}

[[nodiscard]] bool contains_parent_reference(
    const std::filesystem::path& path
) {
    return std::any_of(path.begin(), path.end(), [](const auto& component) {
        return component == "..";
    });
}

[[nodiscard]] bool safe_relative_manifest_path(
    const std::filesystem::path& path
) {
    return !path.empty() && !path.is_absolute() && !path.has_root_name()
        && !path.has_root_directory() && !contains_parent_reference(path);
}

[[nodiscard]] bool path_is_within(
    const std::filesystem::path& base,
    const std::filesystem::path& candidate
) {
    const auto canonical_base = normalized_absolute(base);
    const auto canonical_candidate = normalized_absolute(candidate);
    auto base_part = canonical_base.begin();
    auto candidate_part = canonical_candidate.begin();
    for (; base_part != canonical_base.end(); ++base_part, ++candidate_part) {
        if (candidate_part == canonical_candidate.end()
            || !equal_ascii_insensitive(
                path_utf8(*base_part),
                path_utf8(*candidate_part)
            )) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] ContentProfile profile_from_string(
    const std::string_view raw
) {
    const auto value = lower_ascii(trim_ascii(raw));
    if (value == "pulseforge" || value == "native") {
        return ContentProfile::pulseforge;
    }
    if (value == "psych" || value == "psych-engine"
        || value == "psychengine") {
        return ContentProfile::psych;
    }
    if (value == "p-slice" || value == "pslice") {
        return ContentProfile::pslice;
    }
    if (value == "h-slice" || value == "hslice") {
        return ContentProfile::hslice;
    }
    if (value == "js-engine" || value == "js_engine"
        || value == "jsengine") {
        return ContentProfile::js_engine;
    }
    if (value == "denpa" || value == "denpaex") {
        return ContentProfile::denpa;
    }
    if (value == "v-slice" || value == "vslice") {
        return ContentProfile::vslice;
    }
    return ContentProfile::unknown;
}

[[nodiscard]] ContentProfile profile_for_layout(
    const ContentLayout layout
) noexcept {
    switch (layout) {
    case ContentLayout::native:
        return ContentProfile::pulseforge;
    case ContentLayout::psych:
        return ContentProfile::psych;
    case ContentLayout::denpa:
        return ContentProfile::denpa;
    case ContentLayout::vslice:
        return ContentProfile::vslice;
    case ContentLayout::midi:
    case ContentLayout::pfm:
        return ContentProfile::pulseforge;
    case ContentLayout::unknown:
        return ContentProfile::unknown;
    }
    return ContentProfile::unknown;
}

[[nodiscard]] std::optional<std::string> read_bounded_text(
    const std::filesystem::path& path,
    const std::uintmax_t limit
) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > limit
        || size > static_cast<std::uintmax_t>(
            std::numeric_limits<std::streamsize>::max()
        )) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!text.empty()) {
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
    }
    if (!input) {
        return std::nullopt;
    }
    return text;
}

[[nodiscard]] std::optional<std::int64_t> modified_ticks(
    const std::filesystem::path& path
) {
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(path, error);
    if (error) {
        return std::nullopt;
    }
    const auto count = modified.time_since_epoch().count();
    if constexpr (sizeof(count) > sizeof(std::int64_t)) {
        if (count < static_cast<decltype(count)>(
                std::numeric_limits<std::int64_t>::min()
            )
            || count > static_cast<decltype(count)>(
                std::numeric_limits<std::int64_t>::max()
            )) {
            return std::nullopt;
        }
    }
    return static_cast<std::int64_t>(count);
}

[[nodiscard]] std::string_view cached_layout_name(
    const ContentLayout layout
) noexcept {
    switch (layout) {
    case ContentLayout::native:
        return "native";
    case ContentLayout::psych:
        return "psych";
    case ContentLayout::denpa:
        return "denpa";
    case ContentLayout::vslice:
        return "vslice";
    case ContentLayout::midi:
        return "midi";
    case ContentLayout::pfm:
        return "pfm";
    case ContentLayout::unknown:
        return "unknown";
    }
    return "unknown";
}

[[nodiscard]] std::optional<ContentLayout> cached_layout_from_name(
    const std::string_view name
) noexcept {
    if (name == "native") {
        return ContentLayout::native;
    }
    if (name == "psych") {
        return ContentLayout::psych;
    }
    if (name == "denpa") {
        return ContentLayout::denpa;
    }
    if (name == "vslice") {
        return ContentLayout::vslice;
    }
    if (name == "midi") {
        return ContentLayout::midi;
    }
    if (name == "pfm") {
        return ContentLayout::pfm;
    }
    if (name == "unknown") {
        return ContentLayout::unknown;
    }
    return std::nullopt;
}

[[nodiscard]] bool write_atomic_text(
    const std::filesystem::path& destination,
    const std::string_view text
) {
    if (destination.empty()) {
        return false;
    }
    std::error_code error;
    if (std::filesystem::is_symlink(destination, error) && !error) {
        return false;
    }
    error.clear();
    const auto parent = destination.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return false;
        }
    }

    auto temporary = destination;
    temporary += ".tmp-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    {
        std::ofstream output(
            temporary,
            std::ios::binary | std::ios::trunc
        );
        if (!output) {
            return false;
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary, error);
            return false;
        }
    }

#if defined(_WIN32)
    const auto temporary_wide = temporary.wstring();
    const auto destination_wide = destination.wstring();
    if (MoveFileExW(
            temporary_wide.c_str(),
            destination_wide.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
        ) == 0) {
        error.clear();
        std::filesystem::remove(temporary, error);
        return false;
    }
#else
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
#endif
    return true;
}

[[nodiscard]] LayoutCache load_layout_cache(
    const ContentCatalogOptions& options
) {
    LayoutCache cache;
    cache.probe_bytes = options.probe_bytes;
    cache.enabled = options.use_persistent_cache
        && !options.cache_path.empty();
    if (!cache.enabled) {
        return cache;
    }
    cache.path = normalized_absolute(options.cache_path);
    const auto source = read_bounded_text(
        cache.path,
        maximum_catalog_cache_bytes
    );
    if (!source.has_value()) {
        return cache;
    }
    try {
        const auto root = Json::parse(*source);
        if (!root.is_object()
            || root.value("schema", 0U) != catalog_cache_schema
            || root.value("probeBytes", std::size_t{0})
                != options.probe_bytes) {
            return cache;
        }
        const auto entries = root.find("entries");
        if (entries == root.end() || !entries->is_array()) {
            return cache;
        }
        cache.loaded.reserve(std::min<std::size_t>(
            entries->size(),
            maximum_catalog_cache_entries
        ));
        for (const auto& value : *entries) {
            if (cache.loaded.size() >= maximum_catalog_cache_entries
                || !value.is_object()) {
                break;
            }
            const auto path = value.find("path");
            const auto size = value.find("size");
            const auto modified = value.find("mtime");
            const auto layout = value.find("layout");
            if (path == value.end() || !path->is_string()
                || path->get_ref<const std::string&>().empty()
                || path->get_ref<const std::string&>().size() > 32'768U
                || size == value.end() || !size->is_number_unsigned()
                || modified == value.end() || !modified->is_number_integer()
                || layout == value.end() || !layout->is_string()) {
                continue;
            }
            const auto parsed_layout = cached_layout_from_name(
                layout->get_ref<const std::string&>()
            );
            if (!parsed_layout.has_value()) {
                continue;
            }
            cache.loaded.insert_or_assign(
                path->get<std::string>(),
                LayoutCacheEntry{
                    size->get<std::uintmax_t>(),
                    modified->get<std::int64_t>(),
                    *parsed_layout,
                }
            );
        }
    } catch (...) {
        cache.loaded.clear();
    }
    return cache;
}

void store_layout_cache(const LayoutCache& cache) {
    if (!cache.enabled
        || cache.current.size() > maximum_catalog_cache_entries) {
        return;
    }
    std::vector<std::pair<std::string, LayoutCacheEntry>> entries;
    entries.reserve(cache.current.size());
    for (const auto& entry : cache.current) {
        entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    Json root = Json::object();
    root["schema"] = catalog_cache_schema;
    root["probeBytes"] = cache.probe_bytes;
    root["entries"] = Json::array();
    auto& output_entries = root["entries"];
    for (const auto& [path, entry] : entries) {
        output_entries.push_back({
            {"path", path},
            {"size", entry.size},
            {"mtime", entry.modified_ticks},
            {"layout", cached_layout_name(entry.layout)},
        });
    }
    const auto output = root.dump();
    if (output.size() <= maximum_catalog_cache_bytes) {
        (void)write_atomic_text(cache.path, output);
    }
}

[[nodiscard]] std::string read_prefix(
    const std::filesystem::path& path,
    const std::size_t limit
) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::string text(limit, '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(input.gcount()));
    return text;
}

[[nodiscard]] bool contains_token(
    const std::string_view text,
    const std::string_view token
) noexcept {
    return text.find(token) != std::string_view::npos;
}

[[nodiscard]] ContentLayout sniff_layout(
    const IndexedFile& file,
    const ContentCatalogOptions& options
) {
    const auto extension = lower_ascii(path_utf8(file.path.extension()));
    if (extension == ".mid" || extension == ".midi") {
        return ContentLayout::midi;
    }
    if (extension == ".pfm") {
        return ContentLayout::pfm;
    }
    const std::string prefix = read_prefix(file.path, options.probe_bytes);
    if (prefix.empty()) {
        return ContentLayout::unknown;
    }
    if (contains_token(prefix, "pulseforge-pfm-source-v1")) {
        return ContentLayout::pfm;
    }
    if (contains_token(prefix, "pulseforge-chart")) {
        return ContentLayout::native;
    }
    if (contains_token(prefix, "\"header\"")
        && contains_token(prefix, "\"options\"")
        && contains_token(prefix, "\"notes\"")) {
        return ContentLayout::denpa;
    }
    if (contains_token(prefix, "\"sectionNotes\"")
        || (contains_token(prefix, "\"mustHitSection\"")
            && contains_token(prefix, "\"bpm\""))) {
        return ContentLayout::psych;
    }
    const auto filename = lower_ascii(path_utf8(file.path.filename()));
    if ((filename.ends_with("-chart.json") || filename == "chart.json")
        && contains_token(prefix, "\"notes\"")) {
        return ContentLayout::vslice;
    }
    if (contains_token(prefix, "\"song\"")
        && contains_token(prefix, "\"notes\"")
        && contains_token(prefix, "\"bpm\"")) {
        return ContentLayout::psych;
    }
    return ContentLayout::unknown;
}

[[nodiscard]] ContentLayout sniff_layout_cached(
    const IndexedFile& file,
    const ContentCatalogOptions& options,
    LayoutCache& cache,
    std::size_t& hits,
    std::size_t& misses
) {
    if (!cache.enabled) {
        return sniff_layout(file, options);
    }
    const auto modified = modified_ticks(file.path);
    if (!modified.has_value()) {
        ++misses;
        return sniff_layout(file, options);
    }
    const auto key = lower_ascii(path_utf8(file.path));
    const auto current = cache.current.find(key);
    if (current != cache.current.end()
        && current->second.size == file.size
        && current->second.modified_ticks == *modified) {
        ++hits;
        return current->second.layout;
    }
    const auto cached = cache.loaded.find(key);
    if (cached != cache.loaded.end()
        && cached->second.size == file.size
        && cached->second.modified_ticks == *modified) {
        ++hits;
        cache.current.insert_or_assign(key, cached->second);
        return cached->second.layout;
    }
    ++misses;
    const auto layout = sniff_layout(file, options);
    cache.current.insert_or_assign(
        key,
        LayoutCacheEntry{file.size, *modified, layout}
    );
    return layout;
}

[[nodiscard]] bool ignored_catalog_subtree(
    const std::string_view raw_name
) {
    const auto name = lower_ascii(raw_name);
    constexpr std::array<std::string_view, 15> ignored{
        "cache",
        "characters",
        "crash",
        "dialogue",
        "dialoguecharacters",
        "fonts",
        "images",
        "manifest",
        "music",
        "plugins",
        "registry",
        "shaders",
        "sounds",
        "stages",
        "videos",
    };
    return std::find(ignored.begin(), ignored.end(), name) != ignored.end();
}

[[nodiscard]] bool has_path_component(
    const std::filesystem::path& path,
    const std::string_view component
) {
    return std::any_of(path.begin(), path.end(), [&](const auto& part) {
        return equal_ascii_insensitive(path_utf8(part), component);
    });
}

[[nodiscard]] bool could_be_chart(const IndexedFile& file) {
    const auto extension = lower_ascii(path_utf8(file.path.extension()));
    if (extension == ".mid" || extension == ".midi" || extension == ".pfm") {
        return true;
    }
    if (extension != ".json") {
        return false;
    }
    constexpr std::array<std::string_view, 10> descriptor_directories{
        "achievements",
        "characters",
        "credits",
        "dialogue",
        "dialoguecharacters",
        "manifest",
        "menucharacters",
        "registry",
        "stages",
        "weeks",
    };
    if (std::any_of(
            descriptor_directories.begin(),
            descriptor_directories.end(),
            [&](const auto component) {
                return has_path_component(file.path.parent_path(), component);
            }
        )) {
        return false;
    }
    const auto filename = lower_ascii(path_utf8(file.path.filename()));
    constexpr std::array<std::string_view, 12> excluded{
        "events.json",
        "metadata.json",
        "settings.json",
        "mod.json",
        "pack.json",
        "credits.json",
        "achievements.json",
        "dialogue.json",
        "character.json",
        "stage.json",
        "weeks.json",
        "config.json",
    };
    if (std::find(excluded.begin(), excluded.end(), filename)
        != excluded.end()) {
        return false;
    }
    if (filename.ends_with("-metadata.json")
        || filename.ends_with(".pfmeta.json")) {
        return false;
    }
    // Most engines use data/<song>/ or charts/, but recovered collections and
    // several JS/Haxe forks also place difficulty JSON files directly at the
    // mod root. Every remaining JSON is only a bounded candidate: sniff_layout
    // must still find a real chart signature before it is published.
    return true;
}

[[nodiscard]] std::optional<Json> read_json_metadata(
    const std::filesystem::path& path
) {
    const auto text = read_bounded_text(path, maximum_metadata_bytes);
    if (!text.has_value()) {
        return std::nullopt;
    }
    try {
        return Json::parse(*text);
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::string> json_string(
    const Json& object,
    const std::string_view key
) {
    const auto iterator = object.find(std::string(key));
    if (iterator == object.end() || !iterator->is_string()) {
        return std::nullopt;
    }
    const auto value = iterator->get<std::string>();
    return value.empty() ? std::nullopt
                         : std::optional<std::string>(value);
}

[[nodiscard]] std::optional<std::filesystem::path> existing_file(
    const std::filesystem::path& path
) {
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error) && !error) {
        return normalized_absolute(path);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> find_mods_list(
    const std::filesystem::path& root
) {
    std::vector<std::filesystem::path> candidates;
    if (equal_ascii_insensitive(path_utf8(root.filename()), "mods")) {
        candidates.push_back(root.parent_path() / "modsList.txt");
    }
    candidates.push_back(root / "modsList.txt");
    for (const auto& candidate : candidates) {
        if (auto found = existing_file(candidate); found.has_value()) {
            return found;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool ignored_mod_directory(const std::string_view raw_name) {
    const auto name = lower_ascii(raw_name);
    constexpr std::array<std::string_view, 18> ignored{
        "achievements",
        "characters",
        "custom_events",
        "custom_notetypes",
        "data",
        "fonts",
        "images",
        "music",
        "scripts",
        "shaders",
        "songs",
        "sounds",
        "stages",
        "videos",
        "weeks",
        "registry",
        "cache",
        "assets",
    };
    return std::find(ignored.begin(), ignored.end(), name) != ignored.end();
}

[[nodiscard]] std::vector<std::filesystem::path> immediate_directories(
    const std::filesystem::path& root
) {
    std::vector<std::filesystem::path> result;
    std::error_code error;
    std::filesystem::directory_iterator iterator(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        error
    );
    const std::filesystem::directory_iterator end;
    for (; !error && iterator != end; iterator.increment(error)) {
        std::error_code status_error;
        if (!iterator->is_symlink(status_error) && !status_error
            && iterator->is_directory(status_error) && !status_error) {
            result.push_back(normalized_absolute(iterator->path()));
        }
    }
    std::sort(result.begin(), result.end(), path_less);
    return result;
}

void append_unique_path(
    std::vector<std::filesystem::path>& paths,
    const std::filesystem::path& path
) {
    const auto key = path_key(path);
    const auto found = std::find_if(paths.begin(), paths.end(), [&](const auto& item) {
        return path_key(item) == key;
    });
    if (found == paths.end()) {
        paths.push_back(path);
    }
}

[[nodiscard]] std::optional<std::filesystem::path> adjacent_metadata(
    const std::filesystem::path& chart,
    const std::string_view song_id
) {
    const auto parent = chart.parent_path();
    auto musical_stem = path_utf8(chart.filename());
    const auto lower_musical = lower_ascii(musical_stem);
    if (lower_musical.ends_with(".pfm.json")) {
        musical_stem.resize(musical_stem.size() - std::string_view{".pfm.json"}.size());
    } else {
        musical_stem = path_utf8(chart.stem());
    }
    std::vector<std::filesystem::path> candidates{
        parent / (musical_stem + ".pfmeta.json"),
        parent / "metadata.json",
        parent / (std::string(song_id) + "-metadata.json"),
    };
    for (const auto& candidate : candidates) {
        if (auto found = existing_file(candidate); found.has_value()) {
            return found;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<std::filesystem::path> adjacent_scripts(
    const std::filesystem::path& chart
) {
    std::vector<std::filesystem::path> result;
    const auto parent = chart.parent_path();
    auto stem_script = chart;
    stem_script.replace_extension(".lua");
    const std::array candidates{parent / "script.lua", stem_script};
    for (const auto& candidate : candidates) {
        if (auto found = existing_file(candidate); found.has_value()) {
            append_unique_path(result, *found);
        }
    }
    return result;
}

[[nodiscard]] std::string infer_song_id(const std::filesystem::path& chart) {
    const auto parent_name = slugify(path_utf8(chart.parent_path().filename()));
    auto source_name = path_utf8(chart.filename());
    const auto lower_name = lower_ascii(source_name);
    if (lower_name.ends_with(".pfm.json")) {
        source_name.resize(
            source_name.size() - std::string_view{".pfm.json"}.size()
        );
    } else {
        source_name = path_utf8(chart.stem());
    }
    const auto stem = slugify(source_name);
    if (stem == "chart" || stem.ends_with("-chart")) {
        return parent_name;
    }
    if (has_path_component(chart.parent_path(), "data")
        && parent_name != "data") {
        return parent_name;
    }
    constexpr std::array<std::string_view, 6> known_difficulties{
        "easy", "normal", "hard", "erect", "nightmare", "insane",
    };
    for (const auto difficulty : known_difficulties) {
        const auto suffix = '-' + std::string(difficulty);
        if (stem.ends_with(suffix) && stem.size() > suffix.size()) {
            return stem.substr(0U, stem.size() - suffix.size());
        }
    }
    return stem;
}

[[nodiscard]] std::filesystem::path infer_content_root(
    const std::filesystem::path& chart,
    const std::filesystem::path& boundary
) {
    // Packaged engines often place content below bin/assets or assets/shared.
    // The directory immediately above data/charts owns sibling songs,
    // characters, stages, scripts and images.
    auto directory = chart.parent_path();
    while (!directory.empty() && path_is_within(boundary, directory)) {
        const auto name = lower_ascii(path_utf8(directory.filename()));
        if (name == "data" || name == "charts") {
            auto candidate = directory.parent_path();
            if (name == "charts"
                && lower_ascii(path_utf8(candidate.filename())) == "data") {
                candidate = candidate.parent_path();
            }
            if (!candidate.empty() && path_is_within(boundary, candidate)) {
                return candidate;
            }
            break;
        }
        if (path_key(directory) == path_key(boundary)) {
            break;
        }
        const auto parent = directory.parent_path();
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    return boundary;
}

[[nodiscard]] std::string infer_difficulty(
    const std::filesystem::path& chart,
    const std::string_view song_id,
    const ContentLayout layout
) {
    const auto stem = slugify(path_utf8(chart.stem()));
    if (layout == ContentLayout::vslice
        || layout == ContentLayout::midi
        || layout == ContentLayout::pfm
        || stem == "chart"
        || stem.ends_with("-chart") || stem == song_id) {
        return "normal";
    }
    const std::string prefix = std::string(song_id) + '-';
    if (stem.starts_with(prefix) && stem.size() > prefix.size()) {
        return stem.substr(prefix.size());
    }
    constexpr std::array known{
        "easy",
        "normal",
        "hard",
        "erect",
        "nightmare",
        "insane",
    };
    if (std::find(known.begin(), known.end(), stem) != known.end()) {
        return stem;
    }
    return "normal";
}

[[nodiscard]] const ModInfo* nearest_mod(
    const std::filesystem::path& file,
    const std::filesystem::path& root,
    const std::vector<ModInfo>& mods,
    const std::unordered_map<std::string, std::size_t>& mods_by_root
) {
    auto directory = file.parent_path();
    while (!directory.empty()) {
        const auto iterator = mods_by_root.find(path_key(directory));
        if (iterator != mods_by_root.end() && iterator->second < mods.size()) {
            return &mods[iterator->second];
        }
        if (path_key(directory) == path_key(root)) {
            break;
        }
        const auto parent = directory.parent_path();
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    return nullptr;
}

[[nodiscard]] std::string scope_key(
    const std::filesystem::path& file,
    const std::filesystem::path& root,
    const std::vector<ModInfo>& mods,
    const std::unordered_map<std::string, std::size_t>& mods_by_root
) {
    const auto* mod = nearest_mod(file, root, mods, mods_by_root);
    return path_key(mod != nullptr ? mod->root : root);
}

}  // namespace

ContentCatalog ContentCatalog::scan(const ContentCatalogOptions& raw_options) {
    ContentCatalog catalog;
    ContentCatalogOptions options = raw_options;
    options.max_files = std::clamp<std::size_t>(options.max_files, 1, 1'000'000);
    options.max_entries = std::clamp<std::size_t>(options.max_entries, 1, 100'000);
    options.max_diagnostics = std::min<std::size_t>(options.max_diagnostics, 4'096);
    options.max_depth = std::clamp<std::size_t>(options.max_depth, 1, 64);
    options.probe_bytes = std::clamp<std::size_t>(
        options.probe_bytes,
        4U * 1024U,
        2U * 1024U * 1024U
    );
    auto layout_cache = load_layout_cache(options);

    const auto diagnose = [&](const std::filesystem::path& path, std::string message) {
        if (catalog.diagnostics_.size() < options.max_diagnostics) {
            catalog.diagnostics_.push_back({path, std::move(message)});
        }
    };

    std::vector<RootInfo> roots;
    roots.reserve(options.roots.size());
    std::unordered_set<std::string> visited_roots;
    for (const auto& requested_root : options.roots) {
        const auto root = normalized_absolute(requested_root);
        if (!visited_roots.insert(path_key(root)).second) {
            continue;
        }
        std::error_code error;
        if (!std::filesystem::is_directory(root, error) || error) {
            diagnose(root, "content root is not an accessible directory");
            continue;
        }

        RootInfo info;
        info.path = root;
        info.order = roots.size();
        info.mods_list_path = find_mods_list(root);
        info.mod_container = equal_ascii_insensitive(
            path_utf8(root.filename()),
            "mods"
        ) || info.mods_list_path.has_value();

        if (info.mods_list_path.has_value()) {
            const auto text = read_bounded_text(
                *info.mods_list_path,
                maximum_metadata_bytes
            );
            if (!text.has_value()) {
                diagnose(*info.mods_list_path, "modsList.txt could not be read safely");
            } else {
                std::istringstream stream(*text);
                std::string line;
                std::size_t line_number = 0;
                while (std::getline(stream, line)) {
                    ++line_number;
                    if (line_number == 1 && line.size() >= 3
                        && static_cast<unsigned char>(line[0]) == 0xEFU
                        && static_cast<unsigned char>(line[1]) == 0xBBU
                        && static_cast<unsigned char>(line[2]) == 0xBFU) {
                        line.erase(0, 3);
                    }
                    line = trim_ascii(line);
                    if (line.empty() || line.front() == '#') {
                        continue;
                    }
                    const auto separator = line.rfind('|');
                    if (separator == std::string::npos) {
                        diagnose(
                            *info.mods_list_path,
                            "ignored malformed modsList.txt line "
                                + std::to_string(line_number)
                        );
                        continue;
                    }
                    const auto folder = trim_ascii(
                        std::string_view(line).substr(0, separator)
                    );
                    const auto state = trim_ascii(
                        std::string_view(line).substr(separator + 1)
                    );
                    const std::filesystem::path relative(folder);
                    if (!safe_relative_manifest_path(relative)
                        || !relative.parent_path().empty()) {
                        diagnose(
                            *info.mods_list_path,
                            "ignored unsafe modsList.txt folder on line "
                                + std::to_string(line_number)
                        );
                        continue;
                    }
                    if (state != "0" && state != "1") {
                        diagnose(
                            *info.mods_list_path,
                            "ignored invalid modsList.txt state on line "
                                + std::to_string(line_number)
                        );
                        continue;
                    }
                    const auto key = lower_ascii(folder);
                    if (info.mods_list_lookup.find(key)
                        != info.mods_list_lookup.end()) {
                        diagnose(
                            *info.mods_list_path,
                            "ignored duplicate modsList.txt entry on line "
                                + std::to_string(line_number)
                        );
                        continue;
                    }
                    const auto list_index = info.mods_list.size();
                    info.mods_list_lookup.emplace(key, list_index);
                    info.mods_list.push_back({
                        folder,
                        key,
                        list_index,
                        state == "1",
                    });
                }
            }
        }
        roots.push_back(std::move(info));
    }

    std::vector<IndexedFile> files;
    files.reserve(std::min<std::size_t>(options.max_files, 8'192));
    bool scan_limit_reported = false;
    for (const auto& root : roots) {
        std::function<void(const std::filesystem::path&, std::size_t)> visit;
        visit = [&](const std::filesystem::path& directory, const std::size_t depth) {
            if (files.size() >= options.max_files) {
                catalog.truncated_ = true;
                if (!scan_limit_reported) {
                    diagnose(
                        root.path,
                        "file scan limit reached; remaining content was skipped"
                    );
                    scan_limit_reported = true;
                }
                return;
            }

            std::error_code error;
            std::filesystem::directory_iterator iterator(
                directory,
                std::filesystem::directory_options::skip_permission_denied,
                error
            );
            const std::filesystem::directory_iterator end;
            if (error) {
                diagnose(directory, "directory could not be enumerated");
                return;
            }
            std::vector<std::filesystem::path> children;
            for (; iterator != end; iterator.increment(error)) {
                if (error) {
                    diagnose(directory, "directory entry could not be read");
                    error.clear();
                    continue;
                }
                children.push_back(iterator->path());
            }
            std::sort(children.begin(), children.end(), path_less);

            for (const auto& child : children) {
                if (files.size() >= options.max_files) {
                    catalog.truncated_ = true;
                    if (!scan_limit_reported) {
                        diagnose(
                            root.path,
                            "file scan limit reached; remaining content was skipped"
                        );
                        scan_limit_reported = true;
                    }
                    return;
                }
                std::error_code status_error;
                if (std::filesystem::is_symlink(child, status_error)) {
                    continue;
                }
                if (status_error) {
                    diagnose(child, "filesystem entry status could not be read");
                    continue;
                }
                if (std::filesystem::is_directory(child, status_error)) {
                    if (status_error) {
                        diagnose(child, "directory status could not be read");
                        continue;
                    }
                    const auto name = path_utf8(child.filename());
                    if (!name.empty() && name.front() != '.'
                        && !ignored_catalog_subtree(name)
                        && depth < options.max_depth) {
                        visit(child, depth + 1);
                    }
                    continue;
                }
                if (status_error
                    || !std::filesystem::is_regular_file(child, status_error)
                    || status_error) {
                    continue;
                }
                const auto extension = lower_ascii(
                    path_utf8(child.extension())
                );
                if (extension != ".json"
                    && extension != ".mid"
                    && extension != ".midi"
                    && extension != ".pfm") {
                    continue;
                }
                const auto size = std::filesystem::file_size(child, status_error);
                if (status_error) {
                    diagnose(child, "file size could not be read");
                    continue;
                }
                const auto absolute = normalized_absolute(child);
                const auto relative = absolute.lexically_relative(root.path);
                files.push_back({
                    absolute,
                    root.path,
                    size,
                    root.order,
                    lower_ascii(path_utf8(relative)),
                });
            }
        };
        visit(root.path, 0);
    }

    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        if (left.root_order != right.root_order) {
            return left.root_order < right.root_order;
        }
        if (left.relative_key != right.relative_key) {
            return left.relative_key < right.relative_key;
        }
        return path_utf8(left.path) < path_utf8(right.path);
    });

    std::vector<ModInfo> mods;
    for (const auto& root : roots) {
        if (!root.mod_container) {
            ModInfo info;
            info.id = slugify(path_utf8(root.path.filename()));
            info.name = display_name(info.id);
            info.root = root.path;
            info.content_root = root.path;
            info.root_order = root.order;
            mods.push_back(std::move(info));
            continue;
        }

        struct DirectoryCandidate {
            std::filesystem::path path;
            std::string key;
            std::size_t listed_order{};
            bool listed{};
            bool enabled{true};
        };
        std::vector<DirectoryCandidate> candidates;
        for (const auto& directory : immediate_directories(root.path)) {
            const auto name = path_utf8(directory.filename());
            if (name.empty() || name.front() == '.' || ignored_mod_directory(name)) {
                continue;
            }
            DirectoryCandidate candidate;
            candidate.path = directory;
            candidate.key = lower_ascii(name);
            const auto control = root.mods_list_lookup.find(candidate.key);
            if (control != root.mods_list_lookup.end()
                && control->second < root.mods_list.size()) {
                const auto& listed = root.mods_list[control->second];
                candidate.listed = true;
                candidate.listed_order = listed.order;
                candidate.enabled = listed.enabled;
            }
            candidates.push_back(std::move(candidate));
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
            if (left.listed != right.listed) {
                return left.listed;
            }
            if (left.listed && left.listed_order != right.listed_order) {
                return left.listed_order < right.listed_order;
            }
            if (left.key != right.key) {
                return left.key < right.key;
            }
            return path_utf8(left.path) < path_utf8(right.path);
        });
        std::size_t local_order = 0;
        for (auto& candidate : candidates) {
            ModInfo info;
            info.id = slugify(path_utf8(candidate.path.filename()));
            info.name = display_name(info.id);
            info.root = candidate.path;
            info.content_root = root.path;
            info.profile = ContentProfile::psych;
            info.root_order = root.order;
            info.local_order = local_order;
            info.enabled = candidate.enabled;
            ++local_order;
            mods.push_back(std::move(info));
        }
    }

    std::unordered_map<std::string, std::size_t> mods_by_root;
    for (std::size_t index = 0; index < mods.size(); ++index) {
        mods_by_root.insert_or_assign(path_key(mods[index].root), index);
    }

    for (const auto& file : files) {
        const auto filename = lower_ascii(path_utf8(file.path.filename()));
        if (filename != "mod.json" && filename != "pack.json") {
            continue;
        }

        const auto mod_root_key = path_key(file.path.parent_path());
        auto mod_iterator = mods_by_root.find(mod_root_key);
        if (mod_iterator == mods_by_root.end()) {
            ModInfo info;
            info.id = slugify(path_utf8(file.path.parent_path().filename()));
            info.name = display_name(info.id);
            info.root = file.path.parent_path();
            info.content_root = file.root;
            info.root_order = file.root_order;
            info.local_order = mods.size();
            if (file.root_order < roots.size()) {
                const auto& root = roots[file.root_order];
                const auto relative = info.root.lexically_relative(root.path);
                if (!relative.empty()) {
                    const auto first = lower_ascii(path_utf8(*relative.begin()));
                    const auto control = root.mods_list_lookup.find(first);
                    if (control != root.mods_list_lookup.end()
                        && control->second < root.mods_list.size()) {
                        info.enabled = root.mods_list[control->second].enabled;
                        info.local_order = root.mods_list[control->second].order;
                    }
                }
            }
            const auto index = mods.size();
            mods.push_back(std::move(info));
            mods_by_root.emplace(mod_root_key, index);
            mod_iterator = mods_by_root.find(mod_root_key);
        }

        auto& info = mods[mod_iterator->second];
        const auto parsed = read_json_metadata(file.path);
        if (!parsed.has_value() || !parsed->is_object()) {
            diagnose(file.path, "mod manifest is not valid bounded JSON");
            continue;
        }

        const bool existing_is_native = info.manifest_path.has_value()
            && equal_ascii_insensitive(
                path_utf8(info.manifest_path->filename()),
                "mod.json"
            );
        const bool prefer_manifest = !info.manifest_path.has_value()
            || filename == "mod.json" || !existing_is_native;
        if (!prefer_manifest) {
            continue;
        }

        info.manifest_path = file.path;
        info.id = json_string(*parsed, "id").value_or(
            slugify(path_utf8(info.root.filename()))
        );
        info.name = json_string(*parsed, "name").value_or(display_name(info.id));

        info.profile = ContentProfile::unknown;
        constexpr std::array<std::string_view, 3> profile_keys{
            "profile",
            "engine",
            "compatibilityProfile",
        };
        for (const auto key : profile_keys) {
            if (const auto value = json_string(*parsed, key); value.has_value()) {
                info.profile = profile_from_string(*value);
                if (info.profile != ContentProfile::unknown) {
                    break;
                }
            }
        }
        if (info.profile == ContentProfile::unknown) {
            info.profile = filename == "mod.json"
                ? ContentProfile::pulseforge
                : ContentProfile::psych;
        }

        const auto safe_manifest_file = [&](const std::string_view raw_path)
            -> std::optional<std::filesystem::path> {
            const std::filesystem::path relative{std::string(raw_path)};
            if (!safe_relative_manifest_path(relative)) {
                diagnose(file.path, "manifest path is absolute or contains '..'");
                return std::nullopt;
            }
            std::error_code error;
            const auto candidate = std::filesystem::canonical(
                info.root / relative,
                error
            );
            if (error || !std::filesystem::is_regular_file(candidate, error)
                || error) {
                diagnose(file.path, "manifest path does not name a readable file");
                return std::nullopt;
            }
            if (!path_is_within(info.root, candidate)) {
                diagnose(file.path, "manifest path escapes the mod root");
                return std::nullopt;
            }
            return candidate;
        };

        info.entry_chart.reset();
        if (const auto entry = json_string(*parsed, "entryChart"); entry.has_value()) {
            info.entry_chart = safe_manifest_file(*entry);
        }
        info.entry_scripts.clear();
        const auto scripts = parsed->find("scripts");
        if (scripts != parsed->end() && scripts->is_array()) {
            for (const auto& script : *scripts) {
                if (script.is_string()) {
                    if (const auto found = safe_manifest_file(
                            script.get_ref<const std::string&>()
                        ); found.has_value()) {
                        append_unique_path(info.entry_scripts, *found);
                    }
                }
            }
        }
    }

    std::sort(mods.begin(), mods.end(), [](const auto& left, const auto& right) {
        if (left.root_order != right.root_order) {
            return left.root_order < right.root_order;
        }
        if (left.local_order != right.local_order) {
            return left.local_order < right.local_order;
        }
        return path_less(left.root, right.root);
    });
    mods_by_root.clear();
    for (std::size_t index = 0; index < mods.size(); ++index) {
        mods[index].order = index;
        mods_by_root.insert_or_assign(path_key(mods[index].root), index);
        catalog.mods_.push_back({
            mods[index].id,
            mods[index].name,
            mods[index].root,
            mods[index].manifest_path,
            mods[index].profile,
            mods[index].order,
            mods[index].enabled,
        });
    }

    std::unordered_map<std::string, WeekInfo> weeks;
    for (const auto& file : files) {
        if (!has_path_component(file.path.parent_path(), "weeks")
            || !equal_ascii_insensitive(path_utf8(file.path.extension()), ".json")) {
            continue;
        }
        const auto source = read_bounded_text(file.path, maximum_metadata_bytes);
        if (!source.has_value()) {
            continue;
        }
        DescriptorParseOptions descriptor_options;
        descriptor_options.mode = DescriptorParseMode::permissive;
        descriptor_options.limits.max_input_bytes = maximum_metadata_bytes;
        const auto parsed = ContentDescriptorParser::parse_psych_week(
            *source,
            path_utf8(file.path.stem()),
            descriptor_options
        );
        if (!parsed) {
            diagnose(file.path, "week descriptor is not valid bounded JSON");
            continue;
        }
        const auto* mod = nearest_mod(
            file.path,
            file.root,
            mods,
            mods_by_root
        );
        if (mod != nullptr && !mod->enabled) {
            continue;
        }
        const auto title = !parsed.value->display_name.empty()
            ? parsed.value->display_name
            : (!parsed.value->story_name.empty()
                ? parsed.value->story_name
                : display_name(path_utf8(file.path.stem())));
        for (std::size_t song_index = 0;
             song_index < parsed.value->songs.size();
             ++song_index) {
            const auto& song_title = parsed.value->songs[song_index].name;
            if (!song_title.empty()) {
                const auto key = scope_key(
                    file.path,
                    file.root,
                    mods,
                    mods_by_root
                ) + '\n' + slugify(song_title);
                weeks.try_emplace(
                    key,
                    WeekInfo{title, song_title, song_index}
                );
            }
        }
    }

    std::unordered_set<std::string> known_charts;
    const auto add_entry = [&](const IndexedFile& file, const ContentLayout layout) {
        if (catalog.entries_.size() >= options.max_entries) {
            catalog.truncated_ = true;
            return;
        }
        const auto* mod = nearest_mod(
            file.path,
            file.root,
            mods,
            mods_by_root
        );
        if (mod != nullptr && !mod->enabled) {
            return;
        }
        const auto chart_key = path_key(file.path);
        if (!known_charts.insert(chart_key).second) {
            return;
        }
        SongCatalogEntry entry;
        entry.chart_path = file.path;
        entry.chart_size_bytes = file.size;
        entry.layout = layout;
        entry.song_id = infer_song_id(file.path);
        entry.difficulty = infer_difficulty(file.path, entry.song_id, layout);
        entry.title = display_name(entry.song_id);
        const auto week_key = scope_key(
            file.path,
            file.root,
            mods,
            mods_by_root
        ) + '\n' + entry.song_id;
        if (const auto week = weeks.find(week_key); week != weeks.end()) {
            entry.week = week->second.title;
            entry.title = week->second.song_title;
            entry.week_song_order = week->second.song_order;
        }
        if (mod != nullptr) {
            entry.mod_id = mod->id;
            entry.mod_name = mod->name;
            entry.mod_root = mod->root;
            entry.manifest_path = mod->manifest_path;
            entry.profile = mod->profile;
            entry.mod_order = mod->order;
            if (mod->entry_chart.has_value()
                && path_key(*mod->entry_chart) == chart_key) {
                entry.provenance = ContentProvenance::manifest;
                for (const auto& script : mod->entry_scripts) {
                    append_unique_path(entry.script_paths, script);
                }
            }
        } else {
            entry.mod_id = slugify(path_utf8(file.root.filename()));
            entry.mod_name = display_name(entry.mod_id);
            entry.mod_root = file.root;
            entry.profile = profile_for_layout(layout);
            entry.mod_order = file.root_order;
        }
        entry.content_root = infer_content_root(
            file.path,
            mod != nullptr ? mod->root : file.root
        );
        if (entry.profile == ContentProfile::unknown) {
            entry.profile = profile_for_layout(layout);
        }
        if (entry.provenance != ContentProvenance::manifest
            && (has_path_component(file.path.parent_path(), "data")
                || has_path_component(file.path.parent_path(), "charts"))) {
            entry.provenance = ContentProvenance::convention;
        }
        for (const auto& script : adjacent_scripts(file.path)) {
            if (path_is_within(entry.mod_root, script)) {
                append_unique_path(entry.script_paths, script);
            }
        }
        if (!entry.script_paths.empty()) {
            entry.script_path = entry.script_paths.front();
        }
        const auto metadata = adjacent_metadata(file.path, entry.song_id);
        std::vector<std::string> descriptor_difficulties;
        if (metadata.has_value() && path_is_within(entry.mod_root, *metadata)) {
            entry.metadata_path = metadata;
            if (layout == ContentLayout::vslice) {
                if (const auto source = read_bounded_text(
                        *metadata,
                        maximum_metadata_bytes
                    ); source.has_value()) {
                    DescriptorParseOptions descriptor_options;
                    descriptor_options.mode = DescriptorParseMode::permissive;
                    descriptor_options.limits.max_input_bytes =
                        maximum_metadata_bytes;
                    const auto descriptor =
                        ContentDescriptorParser::parse_vslice_song(
                            *source,
                            entry.song_id,
                            descriptor_options
                        );
                    if (descriptor) {
                        if (!descriptor.value->name.empty()) {
                            entry.title = descriptor.value->name;
                        }
                        descriptor_difficulties = descriptor.value->difficulties;
                    } else {
                        diagnose(*metadata, "V-Slice song metadata is invalid");
                    }
                }
            } else if (layout == ContentLayout::midi
                || layout == ContentLayout::pfm) {
                if (const auto source = read_bounded_text(
                        *metadata,
                        maximum_metadata_bytes
                    ); source.has_value()) {
                    try {
                        const auto json = Json::parse(*source);
                        if (const auto title = json.find("title");
                            title != json.end() && title->is_string()
                            && !title->get<std::string>().empty()) {
                            entry.title = title->get<std::string>();
                        }
                        if (const auto difficulty = json.find("difficulty");
                            difficulty != json.end()
                            && difficulty->is_string()
                            && !difficulty->get<std::string>().empty()) {
                            entry.difficulty = difficulty->get<std::string>();
                        }
                    } catch (...) {
                        diagnose(*metadata, "PFM/MIDI metadata sidecar is invalid JSON");
                    }
                }
            }
        }
        if (layout == ContentLayout::pfm
            && !metadata.has_value()
            && lower_ascii(path_utf8(file.path.filename())).ends_with(".pfm.json")) {
            if (const auto source = read_bounded_text(
                    file.path,
                    maximum_metadata_bytes
                ); source.has_value()) {
                try {
                    const auto json = Json::parse(*source);
                    if (const auto embedded = json.find("metadata");
                        embedded != json.end() && embedded->is_object()) {
                        if (const auto title = embedded->find("title");
                            title != embedded->end() && title->is_string()
                            && !title->get<std::string>().empty()) {
                            entry.title = title->get<std::string>();
                        }
                        if (const auto difficulty = embedded->find("difficulty");
                            difficulty != embedded->end()
                            && difficulty->is_string()
                            && !difficulty->get<std::string>().empty()) {
                            entry.difficulty = difficulty->get<std::string>();
                        }
                    }
                } catch (...) {
                    diagnose(file.path, "PFM source metadata is invalid JSON");
                }
            }
        }
        if (layout == ContentLayout::vslice
            && !descriptor_difficulties.empty()) {
            std::unordered_set<std::string> seen_difficulties;
            for (const auto& difficulty : descriptor_difficulties) {
                if (difficulty.empty()
                    || !seen_difficulties.insert(lower_ascii(difficulty)).second) {
                    continue;
                }
                if (catalog.entries_.size() >= options.max_entries) {
                    catalog.truncated_ = true;
                    break;
                }
                auto variant = entry;
                variant.difficulty = difficulty;
                variant.id = variant.mod_id + ':' + variant.song_id + ':'
                    + variant.difficulty;
                catalog.entries_.push_back(std::move(variant));
            }
            return;
        }
        entry.id = entry.mod_id + ':' + entry.song_id + ':' + entry.difficulty;
        catalog.entries_.push_back(std::move(entry));
    };

    for (const auto& mod : mods) {
        if (!mod.enabled || !mod.entry_chart.has_value()) {
            continue;
        }
        std::error_code error;
        const auto size = std::filesystem::file_size(*mod.entry_chart, error);
        if (!error) {
            const IndexedFile file{
                *mod.entry_chart,
                mod.content_root,
                size,
                mod.root_order,
                lower_ascii(path_utf8(
                    mod.entry_chart->lexically_relative(mod.content_root)
                )),
            };
            auto layout = sniff_layout_cached(
                file,
                options,
                layout_cache,
                catalog.cache_hits_,
                catalog.cache_misses_
            );
            if (layout == ContentLayout::unknown) {
                layout = ContentLayout::native;
            }
            add_entry(file, layout);
        }
    }

    for (const auto& file : files) {
        if (!could_be_chart(file)) {
            continue;
        }
        const auto layout = sniff_layout_cached(
            file,
            options,
            layout_cache,
            catalog.cache_hits_,
            catalog.cache_misses_
        );
        if (layout != ContentLayout::unknown) {
            add_entry(file, layout);
        }
    }

    std::sort(catalog.entries_.begin(), catalog.entries_.end(), [](const auto& left, const auto& right) {
        const auto left_title = lower_ascii(left.title);
        const auto right_title = lower_ascii(right.title);
        if (left_title != right_title) {
            return left_title < right_title;
        }
        if (left.mod_order != right.mod_order) {
            return left.mod_order < right.mod_order;
        }
        const auto left_mod = lower_ascii(left.mod_id);
        const auto right_mod = lower_ascii(right.mod_id);
        if (left_mod != right_mod) {
            return left_mod < right_mod;
        }
        const auto left_song = lower_ascii(left.song_id);
        const auto right_song = lower_ascii(right.song_id);
        if (left_song != right_song) {
            return left_song < right_song;
        }
        const auto left_difficulty = lower_ascii(left.difficulty);
        const auto right_difficulty = lower_ascii(right.difficulty);
        if (left_difficulty != right_difficulty) {
            return left_difficulty < right_difficulty;
        }
        const auto left_path = lower_ascii(path_utf8(left.chart_path));
        const auto right_path = lower_ascii(path_utf8(right.chart_path));
        if (left_path != right_path) {
            return left_path < right_path;
        }
        return path_utf8(left.chart_path) < path_utf8(right.chart_path);
    });

    // Preserve every chart version while keeping duplicate names usable in
    // the launcher.  Difficulty variants are intentionally separate groups:
    // two "Hard" charts named X become X-1 and X-2, while an Easy chart keeps
    // the ordinary X label.  The underlying JSON is never renamed or
    // rewritten, so conventional audio lookup continues to use its real song
    // metadata and directory layout.
    std::unordered_map<std::string, std::size_t> title_totals;
    for (const auto& entry : catalog.entries_) {
        ++title_totals[
            lower_ascii(entry.title) + '\n' + lower_ascii(entry.difficulty)
        ];
    }
    std::unordered_map<std::string, std::size_t> title_indices;
    for (auto& entry : catalog.entries_) {
        const auto key =
            lower_ascii(entry.title) + '\n' + lower_ascii(entry.difficulty);
        if (title_totals[key] > 1U) {
            entry.title += '-' + std::to_string(++title_indices[key]);
        }
    }

    std::unordered_map<std::string, std::size_t> id_counts;
    for (auto& entry : catalog.entries_) {
        auto& count = id_counts[lower_ascii(entry.id)];
        ++count;
        if (count > 1) {
            entry.id += '#' + std::to_string(count);
        }
    }

    std::sort(
        catalog.diagnostics_.begin(),
        catalog.diagnostics_.end(),
        [](const auto& left, const auto& right) {
            if (path_less(left.path, right.path)) {
                return true;
            }
            if (path_less(right.path, left.path)) {
                return false;
            }
            return left.message < right.message;
        }
    );
    if (!roots.empty()) {
        store_layout_cache(layout_cache);
    }
    return catalog;
}

std::span<const SongCatalogEntry> ContentCatalog::entries() const noexcept {
    return entries_;
}

std::span<const ModCatalogEntry> ContentCatalog::mods() const noexcept {
    return mods_;
}

std::span<const ContentCatalogDiagnostic> ContentCatalog::diagnostics()
    const noexcept {
    return diagnostics_;
}

bool ContentCatalog::truncated() const noexcept {
    return truncated_;
}

std::size_t ContentCatalog::cache_hits() const noexcept {
    return cache_hits_;
}

std::size_t ContentCatalog::cache_misses() const noexcept {
    return cache_misses_;
}

const SongCatalogEntry* ContentCatalog::find(
    const std::string_view selector,
    const std::string_view difficulty
) const noexcept {
    const SongCatalogEntry* fallback = nullptr;
    for (const auto& entry : entries_) {
        if (!equal_ascii_insensitive(entry.id, selector)
            && !equal_ascii_insensitive(entry.song_id, selector)
            && !equal_ascii_insensitive(entry.title, selector)) {
            continue;
        }
        if (fallback == nullptr) {
            fallback = &entry;
        }
        if (difficulty.empty()
            || equal_ascii_insensitive(entry.difficulty, difficulty)) {
            return &entry;
        }
    }
    return difficulty.empty() ? fallback : nullptr;
}

std::string_view to_string(const ContentLayout layout) noexcept {
    switch (layout) {
    case ContentLayout::native:
        return "PulseForge";
    case ContentLayout::psych:
        return "Psych";
    case ContentLayout::denpa:
        return "DenpaEx";
    case ContentLayout::vslice:
        return "V-Slice";
    case ContentLayout::midi:
        return "MIDI";
    case ContentLayout::pfm:
        return "PFM";
    case ContentLayout::unknown:
        return "Unknown";
    }
    return "Unknown";
}

std::string_view to_string(const ContentProfile profile) noexcept {
    switch (profile) {
    case ContentProfile::pulseforge:
        return "PulseForge";
    case ContentProfile::psych:
        return "Psych";
    case ContentProfile::pslice:
        return "P-Slice";
    case ContentProfile::hslice:
        return "H-Slice";
    case ContentProfile::js_engine:
        return "JS Engine";
    case ContentProfile::denpa:
        return "DenpaEx";
    case ContentProfile::vslice:
        return "V-Slice";
    case ContentProfile::unknown:
        return "Unknown";
    }
    return "Unknown";
}

std::string_view to_string(const ContentProvenance provenance) noexcept {
    switch (provenance) {
    case ContentProvenance::manifest:
        return "Manifest";
    case ContentProvenance::convention:
        return "Convention";
    case ContentProvenance::recursive_scan:
        return "Recursive scan";
    }
    return "Recursive scan";
}

}  // namespace pulseforge
