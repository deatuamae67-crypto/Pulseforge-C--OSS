#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace pulseforge {

struct NoteSkinCatalogEntry {
    std::string selection;
    std::string display_name;
    std::string style;
    std::filesystem::path source_root;
    bool pixel{};
};

struct ParsedNoteSkinSelection {
    bool chart_default{true};
    bool pixel{};
    std::string style;
    std::filesystem::path source_hint;
};

namespace note_skin_catalog_detail {

[[nodiscard]] inline std::string path_utf8(
    const std::filesystem::path& path
) {
    const auto value = path.generic_u8string();
    return {
        reinterpret_cast<const char*>(value.data()),
        value.size(),
    };
}

[[nodiscard]] inline std::string lower_ascii(std::string value) {
    for (auto& character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= static_cast<unsigned char>('A')
            && byte <= static_cast<unsigned char>('Z')) {
            character = static_cast<char>(
                byte - static_cast<unsigned char>('A')
                + static_cast<unsigned char>('a')
            );
        }
    }
    return value;
}

[[nodiscard]] inline bool equals_ascii_insensitive(
    const std::string_view left,
    const std::string_view right
) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        auto a = static_cast<unsigned char>(left[index]);
        auto b = static_cast<unsigned char>(right[index]);
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

[[nodiscard]] inline bool valid_style_name(
    const std::string_view style
) noexcept {
    if (style.empty() || style.size() > 255U) return false;
    for (const unsigned char value : style) {
        if (value < 0x20U || value == '/' || value == '\\'
            || value == ':' || value == '*' || value == '?'
            || value == '"' || value == '<' || value == '>'
            || value == '|') {
            return false;
        }
    }
    return style != "." && style != "..";
}

[[nodiscard]] inline bool regular_file(
    const std::filesystem::path& path
) noexcept {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error
        && !std::filesystem::is_symlink(path, error) && !error;
}

[[nodiscard]] inline bool directory(
    const std::filesystem::path& path
) noexcept {
    std::error_code error;
    return std::filesystem::is_directory(path, error) && !error
        && !std::filesystem::is_symlink(path, error) && !error;
}

inline void append_unique_root(
    std::vector<std::filesystem::path>& roots,
    const std::filesystem::path& root
) {
    if (root.empty() || !directory(root)) return;
    std::error_code error;
    auto normalized = std::filesystem::absolute(root, error);
    if (error) {
        error.clear();
        normalized = root;
    }
    normalized = normalized.lexically_normal();
    if (std::find(roots.begin(), roots.end(), normalized) == roots.end()) {
        roots.push_back(std::move(normalized));
    }
}

inline void append_mod_children(
    std::vector<std::filesystem::path>& roots,
    const std::filesystem::path& mods_root
) {
    if (!directory(mods_root)) return;
    std::error_code error;
    std::filesystem::directory_iterator iterator(
        mods_root,
        std::filesystem::directory_options::skip_permission_denied,
        error
    );
    std::filesystem::directory_iterator end;
    for (; !error && iterator != end; iterator.increment(error)) {
        const auto& entry = *iterator;
        std::error_code type_error;
        if (entry.is_directory(type_error) && !type_error
            && !entry.is_symlink(type_error) && !type_error) {
            append_unique_root(roots, entry.path());
        }
    }
}

[[nodiscard]] inline std::vector<std::filesystem::path> expand_install_roots(
    const std::span<const std::filesystem::path> input_roots
) {
    std::vector<std::filesystem::path> roots;
    roots.reserve(input_roots.size() * 3U + 16U);

    const auto add_root_family = [&](const std::filesystem::path& root) {
        if (root.empty()) return;
        append_unique_root(roots, root);

        const auto filename = lower_ascii(path_utf8(root.filename()));
        if (filename == "mods") {
            append_mod_children(roots, root);
        } else {
            append_unique_root(roots, root / "assets");
            append_mod_children(roots, root / "mods");
        }
    };

    for (const auto& root : input_roots) {
        add_root_family(root);
    }

    std::error_code error;
    const auto cwd = std::filesystem::current_path(error);
    if (!error) {
        append_unique_root(roots, cwd / "assets");
        append_mod_children(roots, cwd / "mods");
    }
    return roots;
}

[[nodiscard]] inline std::optional<std::filesystem::path> atlas_xml(
    const std::filesystem::path& png
) {
    auto xml = png;
    xml.replace_extension(".xml");
    if (regular_file(xml)) return xml;
    xml = png;
    xml.replace_extension(".XML");
    if (regular_file(xml)) return xml;
    return std::nullopt;
}

[[nodiscard]] inline bool likely_note_atlas(
    const std::filesystem::path& xml,
    const bool explicit_note_skin_directory,
    const std::string_view stem
) {
    if (explicit_note_skin_directory
        || equals_ascii_insensitive(stem, "NOTE_assets")) {
        return true;
    }

    std::ifstream input(xml, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const auto end = input.tellg();
    constexpr std::streamoff maximum_probe_bytes = 2 * 1024 * 1024;
    if (end <= std::streampos{0}
        || end > std::streampos{maximum_probe_bytes}) {
        return false;
    }

    std::string text(static_cast<std::size_t>(end), '\0');
    input.seekg(0, std::ios::beg);
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!input) return false;
    text = lower_ascii(std::move(text));

    constexpr std::string_view markers[]{
        "arrowleft", "arrowdown", "arrowup", "arrowright",
        "purple0", "blue0", "green0", "red0",
        "purple hold", "blue hold", "green hold", "red hold",
    };
    std::size_t matches{};
    for (const auto marker : markers) {
        if (text.find(marker) != std::string::npos && ++matches >= 2U) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline std::string pretty_name(
    std::string_view style,
    const bool pixel
) {
    if (equals_ascii_insensitive(style, "NOTE_assets")) {
        return "Classic";
    }
    if (equals_ascii_insensitive(style, "arrows-pixels")) {
        return "Pixel";
    }

    std::string result(style);
    for (auto& character : result) {
        if (character == '_' || character == '-') character = ' ';
    }
    if (pixel) result += " [Pixel]";
    return result;
}

[[nodiscard]] inline std::string source_label(
    const std::filesystem::path& source_root
) {
    const auto name = path_utf8(source_root.filename());
    if (name.empty()) return "content root";
    if (lower_ascii(name) == "assets") return "built-in assets";
    return name;
}

[[nodiscard]] inline std::string make_selection(
    const bool pixel,
    const std::string_view style,
    const std::filesystem::path& source_root
) {
    std::string result = pixel ? "pixel:" : "atlas:";
    result.append(style);
    if (!source_root.empty()) {
        result.push_back('|');
        result.append(path_utf8(source_root));
    }
    return result;
}

inline void add_entry(
    std::vector<NoteSkinCatalogEntry>& entries,
    const std::filesystem::path& source_root,
    const std::filesystem::path& png,
    const bool pixel
) {
    const auto style = path_utf8(png.stem());
    if (!valid_style_name(style)) return;

    // Sustain-end sheets are companions, not independent selectable skins.
    const auto lower_style = lower_ascii(style);
    if (pixel
        && (lower_style.find("ends") != std::string::npos
            || lower_style == "arrowends")) {
        return;
    }

    NoteSkinCatalogEntry entry;
    entry.style = style;
    entry.pixel = pixel;
    entry.source_root = source_root;
    entry.selection = make_selection(pixel, style, source_root);
    entry.display_name = pretty_name(style, pixel)
        + "  //  " + source_label(source_root);

    if (std::none_of(
            entries.begin(),
            entries.end(),
            [&](const NoteSkinCatalogEntry& existing) {
                return existing.selection == entry.selection;
            }
        )) {
        entries.push_back(std::move(entry));
    }
}

inline void scan_atlas_directory(
    std::vector<NoteSkinCatalogEntry>& entries,
    const std::filesystem::path& source_root,
    const std::filesystem::path& directory_path,
    const bool explicit_note_skin_directory,
    std::size_t& examined_files,
    const std::size_t maximum_files
) {
    if (!directory(directory_path) || examined_files >= maximum_files) return;

    std::error_code error;
    std::filesystem::directory_iterator iterator(
        directory_path,
        std::filesystem::directory_options::skip_permission_denied,
        error
    );
    std::filesystem::directory_iterator end;
    for (; !error && iterator != end && examined_files < maximum_files;
         iterator.increment(error)) {
        ++examined_files;
        const auto& entry = *iterator;
        std::error_code type_error;
        if (!entry.is_regular_file(type_error) || type_error
            || entry.is_symlink(type_error) || type_error) {
            continue;
        }
        auto extension = lower_ascii(path_utf8(entry.path().extension()));
        if (extension != ".png") continue;
        const auto xml = atlas_xml(entry.path());
        if (!xml.has_value()) continue;
        const auto stem = path_utf8(entry.path().stem());
        if (!likely_note_atlas(*xml, explicit_note_skin_directory, stem)) {
            continue;
        }
        add_entry(entries, source_root, entry.path(), false);
    }
}

inline void scan_pixel_directory(
    std::vector<NoteSkinCatalogEntry>& entries,
    const std::filesystem::path& source_root,
    const std::filesystem::path& directory_path,
    const bool explicit_note_skin_directory,
    std::size_t& examined_files,
    const std::size_t maximum_files
) {
    if (!directory(directory_path) || examined_files >= maximum_files) return;

    std::error_code error;
    std::filesystem::directory_iterator iterator(
        directory_path,
        std::filesystem::directory_options::skip_permission_denied,
        error
    );
    std::filesystem::directory_iterator end;
    for (; !error && iterator != end && examined_files < maximum_files;
         iterator.increment(error)) {
        ++examined_files;
        const auto& entry = *iterator;
        std::error_code type_error;
        if (!entry.is_regular_file(type_error) || type_error
            || entry.is_symlink(type_error) || type_error) {
            continue;
        }
        auto extension = lower_ascii(path_utf8(entry.path().extension()));
        if (extension != ".png") continue;

        const auto stem = path_utf8(entry.path().stem());
        const auto lower_stem = lower_ascii(stem);
        if (!explicit_note_skin_directory
            && lower_stem != "arrows-pixels"
            && lower_stem.rfind("note_assets", 0U) != 0U) {
            continue;
        }
        add_entry(entries, source_root, entry.path(), true);
    }
}

}  // namespace note_skin_catalog_detail

[[nodiscard]] inline ParsedNoteSkinSelection parse_note_skin_selection(
    const std::string_view raw
) {
    ParsedNoteSkinSelection result;
    if (raw.empty() || raw == "chart-default" || raw == "chart_default"
        || raw == "auto") {
        return result;
    }

    std::string_view normalized = raw;
    if (raw == "classic" || raw == "NOTE_assets") {
        normalized = "atlas:NOTE_assets";
    } else if (raw == "pixel" || raw == "pixelUI") {
        normalized = "pixel:arrows-pixels";
    }

    constexpr std::string_view atlas_prefix{"atlas:"};
    constexpr std::string_view pixel_prefix{"pixel:"};
    if (normalized.starts_with(atlas_prefix)) {
        result.pixel = false;
        normalized.remove_prefix(atlas_prefix.size());
    } else if (normalized.starts_with(pixel_prefix)) {
        result.pixel = true;
        normalized.remove_prefix(pixel_prefix.size());
    } else {
        return result;
    }

    const auto separator = normalized.find('|');
    const auto style = separator == std::string_view::npos
        ? normalized
        : normalized.substr(0U, separator);
    if (!note_skin_catalog_detail::valid_style_name(style)) {
        return result;
    }

    result.chart_default = false;
    result.style.assign(style);
    if (separator != std::string_view::npos
        && separator + 1U < normalized.size()) {
        const auto source = normalized.substr(separator + 1U);
        if (source.size() <= 1'024U
            && source.find('\0') == std::string_view::npos) {
            result.source_hint = std::filesystem::path{std::string(source)};
        }
    }
    return result;
}

[[nodiscard]] inline bool valid_note_skin_selection(
    const std::string_view value
) {
    if (value.size() > 1'536U) return false;
    if (value.empty() || value == "chart-default" || value == "chart_default"
        || value == "auto" || value == "classic" || value == "NOTE_assets"
        || value == "pixel" || value == "pixelUI") {
        return true;
    }
    return !parse_note_skin_selection(value).chart_default;
}

[[nodiscard]] inline std::string normalize_note_skin_selection(
    const std::string_view value
) {
    if (value.empty() || value == "chart_default" || value == "auto") {
        return "chart-default";
    }
    if (value == "classic" || value == "NOTE_assets") {
        return "atlas:NOTE_assets";
    }
    if (value == "pixel" || value == "pixelUI") {
        return "pixel:arrows-pixels";
    }
    return std::string(value);
}

[[nodiscard]] inline std::string note_skin_selection_display_name(
    const std::string_view value
) {
    const auto parsed = parse_note_skin_selection(value);
    if (parsed.chart_default) return "Chart default";

    auto label = note_skin_catalog_detail::pretty_name(
        parsed.style,
        parsed.pixel
    );
    if (!parsed.source_hint.empty()) {
        label += "  //  ";
        label += note_skin_catalog_detail::source_label(parsed.source_hint);
    }
    return label;
}

[[nodiscard]] inline std::vector<NoteSkinCatalogEntry> discover_note_skins(
    const std::span<const std::filesystem::path> roots,
    const std::size_t maximum_files = 20'000U
) {
    using namespace note_skin_catalog_detail;

    const auto install_roots = expand_install_roots(roots);
    std::vector<NoteSkinCatalogEntry> entries;
    entries.reserve(64U);
    std::size_t examined_files{};

    constexpr std::string_view atlas_directories[]{
        "images",
        "shared/images",
        "assets/shared/images",
    };
    constexpr std::string_view atlas_skin_directories[]{
        "images/noteSkins",
        "images/noteskins",
        "shared/images/noteSkins",
        "shared/images/noteskins",
        "assets/shared/images/noteSkins",
        "assets/shared/images/noteskins",
    };
    constexpr std::string_view pixel_directories[]{
        "pixelUI",
        "shared/images/pixelUI",
        "assets/shared/images/pixelUI",
    };
    constexpr std::string_view pixel_skin_directories[]{
        "pixelUI/noteSkins",
        "pixelUI/noteskins",
        "shared/images/pixelUI/noteSkins",
        "shared/images/pixelUI/noteskins",
        "assets/shared/images/pixelUI/noteSkins",
        "assets/shared/images/pixelUI/noteskins",
    };

    for (const auto& source_root : install_roots) {
        for (const auto directory_name : atlas_directories) {
            scan_atlas_directory(
                entries,
                source_root,
                source_root / std::filesystem::path{directory_name},
                false,
                examined_files,
                maximum_files
            );
        }
        for (const auto directory_name : atlas_skin_directories) {
            scan_atlas_directory(
                entries,
                source_root,
                source_root / std::filesystem::path{directory_name},
                true,
                examined_files,
                maximum_files
            );
        }
        for (const auto directory_name : pixel_directories) {
            scan_pixel_directory(
                entries,
                source_root,
                source_root / std::filesystem::path{directory_name},
                false,
                examined_files,
                maximum_files
            );
        }
        for (const auto directory_name : pixel_skin_directories) {
            scan_pixel_directory(
                entries,
                source_root,
                source_root / std::filesystem::path{directory_name},
                true,
                examined_files,
                maximum_files
            );
        }
        if (examined_files >= maximum_files) break;
    }

    std::sort(
        entries.begin(),
        entries.end(),
        [](const NoteSkinCatalogEntry& left, const NoteSkinCatalogEntry& right) {
            const auto left_name =
                note_skin_catalog_detail::lower_ascii(left.display_name);
            const auto right_name =
                note_skin_catalog_detail::lower_ascii(right.display_name);
            if (left_name != right_name) return left_name < right_name;
            return left.selection < right.selection;
        }
    );
    return entries;
}

[[nodiscard]] inline std::optional<NoteSkinCatalogEntry>
resolve_note_skin_selection(
    const std::span<const std::filesystem::path> roots,
    const std::string_view selection
) {
    const auto parsed = parse_note_skin_selection(selection);
    if (parsed.chart_default) return std::nullopt;

    const auto entries = discover_note_skins(roots);
    const auto normalized = normalize_note_skin_selection(selection);

    const auto exact = std::find_if(
        entries.begin(),
        entries.end(),
        [&](const NoteSkinCatalogEntry& entry) {
            return entry.selection == normalized;
        }
    );
    if (exact != entries.end()) return *exact;

    // If a project/mod was moved, retain the chosen style and bind it to the
    // first currently installed matching skin rather than silently resetting.
    const auto relocated = std::find_if(
        entries.begin(),
        entries.end(),
        [&](const NoteSkinCatalogEntry& entry) {
            return entry.pixel == parsed.pixel
                && note_skin_catalog_detail::equals_ascii_insensitive(
                    entry.style,
                    parsed.style
                );
        }
    );
    return relocated == entries.end()
        ? std::nullopt
        : std::optional<NoteSkinCatalogEntry>{*relocated};
}

}  // namespace pulseforge
