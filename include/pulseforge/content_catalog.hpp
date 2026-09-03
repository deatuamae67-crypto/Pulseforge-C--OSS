#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge {

enum class ContentLayout : std::uint8_t {
    native,
    psych,
    denpa,
    vslice,
    midi,
    pfm,
    unknown,
};

// The layout describes the chart schema. The profile describes the content
// conventions used around it (mod manifests, weeks and script discovery).
enum class ContentProfile : std::uint8_t {
    pulseforge,
    psych,
    pslice,
    hslice,
    js_engine,
    denpa,
    vslice,
    unknown,
};

enum class ContentProvenance : std::uint8_t {
    manifest,
    convention,
    recursive_scan,
};

struct ModCatalogEntry {
    std::string id;
    std::string name;
    std::filesystem::path root;
    std::optional<std::filesystem::path> manifest_path;
    ContentProfile profile{ContentProfile::unknown};
    std::size_t order{};
    bool enabled{true};
};

struct SongCatalogEntry {
    std::string id;
    std::string song_id;
    std::string title;
    std::string difficulty{"normal"};
    std::string mod_id{"base"};
    std::string mod_name{"Base Game"};
    std::string week;
    std::size_t week_song_order{};
    std::filesystem::path content_root;
    std::filesystem::path mod_root;
    std::optional<std::filesystem::path> manifest_path;
    std::filesystem::path chart_path;
    std::optional<std::filesystem::path> metadata_path;
    // script_path is retained for source compatibility with the first launcher
    // implementation. New code should consume script_paths.
    std::optional<std::filesystem::path> script_path;
    std::vector<std::filesystem::path> script_paths;
    std::uintmax_t chart_size_bytes{};
    ContentLayout layout{ContentLayout::unknown};
    ContentProfile profile{ContentProfile::unknown};
    ContentProvenance provenance{ContentProvenance::recursive_scan};
    std::size_t mod_order{};
};

struct ContentCatalogDiagnostic {
    std::filesystem::path path;
    std::string message;
};

struct ContentCatalogOptions {
    std::vector<std::filesystem::path> roots;
    std::size_t max_files{100'000};
    std::size_t max_entries{25'000};
    std::size_t max_diagnostics{256};
    std::size_t max_depth{16};
    std::size_t probe_bytes{256U * 1024U};
    // An empty path disables the persistent layout index. The default keeps
    // generated state outside assets/mods and can be overridden by embedders.
    std::filesystem::path cache_path{
        "out/cache/content-catalog-v1.json"
    };
    bool use_persistent_cache{true};
};

class ContentCatalog {
public:
    [[nodiscard]] static ContentCatalog scan(
        const ContentCatalogOptions& options
    );

    [[nodiscard]] std::span<const SongCatalogEntry> entries() const noexcept;
    [[nodiscard]] std::span<const ModCatalogEntry> mods() const noexcept;
    [[nodiscard]] std::span<const ContentCatalogDiagnostic> diagnostics()
        const noexcept;
    [[nodiscard]] bool truncated() const noexcept;
    [[nodiscard]] std::size_t cache_hits() const noexcept;
    [[nodiscard]] std::size_t cache_misses() const noexcept;

    // Selector matching is ASCII case-insensitive. It accepts the stable entry
    // id, song id or display title. Difficulty is optional.
    [[nodiscard]] const SongCatalogEntry* find(
        std::string_view selector,
        std::string_view difficulty = {}
    ) const noexcept;

private:
    std::vector<SongCatalogEntry> entries_;
    std::vector<ModCatalogEntry> mods_;
    std::vector<ContentCatalogDiagnostic> diagnostics_;
    bool truncated_{};
    std::size_t cache_hits_{};
    std::size_t cache_misses_{};
};

[[nodiscard]] std::string_view to_string(ContentLayout layout) noexcept;
[[nodiscard]] std::string_view to_string(ContentProfile profile) noexcept;
[[nodiscard]] std::string_view to_string(ContentProvenance provenance) noexcept;

}  // namespace pulseforge
