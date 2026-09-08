#pragma once

#include "pulseforge/mod_installer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace pulseforge::detail {

struct CompleteRuntimeFile {
    std::filesystem::path relative;
    std::string url;
};

struct CompleteRuntimeMod {
    std::string slug;
    std::string name;
    std::string drive_id;
    std::string revision;
    bool enabled_by_default{};
    std::uint64_t expected_min_files{};
    std::uint64_t expected_min_bytes{};
    std::vector<CompleteRuntimeFile> files;
};

struct CompleteRuntimeManifest {
    std::string content_revision;
    std::string source_commit;
    std::vector<CompleteRuntimeMod> mods;
};

struct CompleteRuntimePlanEntry {
    const CompleteRuntimeMod* mod{};
    bool installed{};
    bool revision_current{};

    [[nodiscard]] bool requires_install() const noexcept {
        return !installed || !revision_current;
    }
};

struct CompleteRuntimePlan {
    std::string content_revision;
    std::vector<CompleteRuntimePlanEntry> entries;

    [[nodiscard]] std::size_t pending_count() const noexcept {
        return static_cast<std::size_t>(std::count_if(
            entries.begin(),
            entries.end(),
            [](const CompleteRuntimePlanEntry& entry) {
                return entry.requires_install();
            }
        ));
    }

    [[nodiscard]] bool complete() const noexcept {
        return pending_count() == 0U;
    }
};

struct CompleteRuntimeInstallResult {
    bool success{};
    std::filesystem::path installed_path;
    std::uint64_t installed_files{};
    std::uint64_t installed_bytes{};
    std::string error;
};

namespace complete_runtime_detail {

[[nodiscard]] inline std::string path_utf8(
    const std::filesystem::path& path
) {
    const auto encoded = path.generic_u8string();
    return {encoded.begin(), encoded.end()};
}

[[nodiscard]] inline bool is_hex_revision(const std::string_view value) noexcept {
    return value.size() == 64U && std::all_of(
        value.begin(),
        value.end(),
        [](const unsigned char character) {
            return std::isxdigit(character) != 0;
        }
    );
}

[[nodiscard]] inline bool valid_slug(const std::string_view value) noexcept {
    if (value.empty() || value.size() > 96U || value.front() == '-'
        || value.back() == '-') {
        return false;
    }
    bool separator = false;
    for (const unsigned char character : value) {
        if (character >= 'a' && character <= 'z') {
            separator = false;
            continue;
        }
        if (character >= '0' && character <= '9') {
            separator = false;
            continue;
        }
        if (character == '-' && !separator) {
            separator = true;
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] inline bool safe_relative_path(
    const std::filesystem::path& path
) noexcept {
    if (path.empty() || path.is_absolute() || path.has_root_path()) {
        return false;
    }
    for (const auto& component : path) {
        const auto text = path_utf8(component);
        if (text.empty() || text == "." || text == "..") {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool allowed_drive_url(const std::string_view url) noexcept {
    constexpr std::string_view drive_prefix = "https://drive.google.com/";
    constexpr std::string_view content_prefix = "https://drive.usercontent.google.com/";
    return url.starts_with(drive_prefix) || url.starts_with(content_prefix);
}

[[nodiscard]] inline nlohmann::json read_json(
    const std::filesystem::path& path
) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error(
            "cannot open Complete runtime JSON: " + path_utf8(path)
        );
    }
    try {
        return nlohmann::json::parse(input);
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error(
            "invalid Complete runtime JSON " + path_utf8(path) + ": "
            + error.what()
        );
    }
}

[[nodiscard]] inline std::map<std::string, std::string> load_state_revisions(
    const std::filesystem::path& state_path
) {
    std::map<std::string, std::string> revisions;
    std::error_code error;
    if (!std::filesystem::is_regular_file(state_path, error) || error) {
        return revisions;
    }
    try {
        const auto state = read_json(state_path);
        if (!state.is_object() || state.value("schema_version", 0) != 1
            || state.value("edition", std::string{}) != "1.0.0-complete"
            || !state.contains("mods") || !state["mods"].is_object()) {
            return {};
        }
        for (auto iterator = state["mods"].begin();
             iterator != state["mods"].end(); ++iterator) {
            if (!iterator.value().is_object()) {
                continue;
            }
            const auto revision = iterator.value().value(
                "revision",
                std::string{}
            );
            if (valid_slug(iterator.key()) && is_hex_revision(revision)) {
                revisions.emplace(iterator.key(), revision);
            }
        }
    } catch (const std::exception&) {
        return {};
    }
    return revisions;
}

inline void atomic_write_text(
    const std::filesystem::path& destination,
    const std::string_view text
) {
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        throw std::runtime_error(
            "cannot create Complete runtime state directory: " + error.message()
        );
    }
    const auto temporary = destination.parent_path()
        / (destination.filename().string() + ".tmp");
    {
        std::ofstream output(
            temporary,
            std::ios::binary | std::ios::trunc
        );
        if (!output.is_open()) {
            throw std::runtime_error("cannot create Complete runtime temporary state");
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            throw std::runtime_error("cannot write Complete runtime temporary state");
        }
    }
    std::filesystem::rename(temporary, destination, error);
    if (!error) {
        return;
    }
    // Windows cannot atomically rename over an existing destination. Keep a
    // rollback copy so a failed replacement never destroys the prior state.
    const auto rollback = destination.parent_path()
        / (destination.filename().string() + ".rollback");
    std::filesystem::remove(rollback, error);
    error.clear();
    const bool had_destination = std::filesystem::exists(destination, error)
        && !error;
    if (had_destination) {
        std::filesystem::rename(destination, rollback, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            throw std::runtime_error("cannot stage Complete runtime state replacement");
        }
    }
    error.clear();
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        if (had_destination) {
            std::error_code restore_error;
            std::filesystem::rename(rollback, destination, restore_error);
        }
        throw std::runtime_error("cannot commit Complete runtime state replacement");
    }
    if (had_destination) {
        std::filesystem::remove(rollback, error);
    }
}

inline void ensure_mods_list_entry(
    const std::filesystem::path& mods_root,
    const CompleteRuntimeMod& mod
) {
    const auto list_path = mods_root / "modsList.txt";
    std::vector<std::string> lines;
    std::error_code error;
    if (std::filesystem::is_regular_file(list_path, error) && !error) {
        std::ifstream input(list_path, std::ios::binary);
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                lines.push_back(line);
            }
        }
    }
    const auto prefix = mod.slug + "|";
    const bool already_present = std::any_of(
        lines.begin(),
        lines.end(),
        [&](const std::string& line) {
            return line.starts_with(prefix);
        }
    );
    if (!already_present) {
        lines.push_back(mod.slug + (mod.enabled_by_default ? "|1" : "|0"));
    }
    std::ostringstream output;
    for (const auto& line : lines) {
        output << line << '\n';
    }
    atomic_write_text(list_path, output.str());
}

inline void persist_state_revision(
    const CompleteRuntimeManifest& manifest,
    const std::filesystem::path& mods_root,
    const CompleteRuntimeMod& installed_mod
) {
    const auto state_path = mods_root / ".pulseforge-complete-state.json";
    nlohmann::json state = {
        {"schema_version", 1},
        {"edition", "1.0.0-complete"},
        {"content_revision", ""},
        {"mods", nlohmann::json::object()},
    };
    std::error_code error;
    if (std::filesystem::is_regular_file(state_path, error) && !error) {
        try {
            const auto existing = read_json(state_path);
            if (existing.is_object()
                && existing.value("schema_version", 0) == 1
                && existing.value("edition", std::string{}) == "1.0.0-complete"
                && existing.contains("mods") && existing["mods"].is_object()) {
                state["mods"] = existing["mods"];
            }
        } catch (const std::exception&) {
            // A corrupt optional cache is rebuilt from successfully installed
            // entries rather than preventing the engine from starting.
        }
    }
    state["mods"][installed_mod.slug] = {
        {"revision", installed_mod.revision},
        {"name", installed_mod.name},
    };

    bool all_current = true;
    for (const auto& mod : manifest.mods) {
        const auto destination = mods_root / mod.slug;
        if (!std::filesystem::is_directory(destination, error) || error
            || !state["mods"].contains(mod.slug)
            || !state["mods"][mod.slug].is_object()
            || state["mods"][mod.slug].value("revision", std::string{})
                != mod.revision) {
            all_current = false;
            error.clear();
            break;
        }
    }
    if (all_current) {
        state["content_revision"] = manifest.content_revision;
    }
    atomic_write_text(state_path, state.dump(2) + "\n");
}

}  // namespace complete_runtime_detail

[[nodiscard]] inline CompleteRuntimeManifest load_complete_runtime_manifest(
    const std::filesystem::path& path
) {
    using namespace complete_runtime_detail;
    const auto document = read_json(path);
    if (!document.is_object() || document.value("schema_version", 0) != 1
        || document.value("edition", std::string{}) != "1.0.0-complete"
        || document.value("transport", std::string{})
            != "google-drive-gdown-resolved-https") {
        throw std::runtime_error("unsupported Complete runtime manifest schema");
    }

    CompleteRuntimeManifest manifest;
    manifest.content_revision = document.value("content_revision", std::string{});
    manifest.source_commit = document.value("source_commit", std::string{});
    if (!is_hex_revision(manifest.content_revision)) {
        throw std::runtime_error("Complete runtime manifest has invalid content revision");
    }
    if (!document.contains("mods") || !document["mods"].is_array()) {
        throw std::runtime_error("Complete runtime manifest has no mod array");
    }
    const auto declared_count = document.value("mod_count", 0U);
    if (declared_count != document["mods"].size()) {
        throw std::runtime_error("Complete runtime manifest mod count mismatch");
    }

    std::set<std::string> slugs;
    std::set<std::string> folded_paths;
    for (const auto& value : document["mods"]) {
        if (!value.is_object()) {
            throw std::runtime_error("Complete runtime manifest mod is not an object");
        }
        CompleteRuntimeMod mod;
        mod.slug = value.value("slug", std::string{});
        mod.name = value.value("name", std::string{});
        mod.drive_id = value.value("drive_id", std::string{});
        mod.revision = value.value("revision", std::string{});
        mod.enabled_by_default = value.value("enabled_by_default", false);
        mod.expected_min_files = value.value("expected_min_files", 0ULL);
        mod.expected_min_bytes = value.value("expected_min_bytes", 0ULL);
        if (!valid_slug(mod.slug) || mod.name.empty() || mod.drive_id.empty()
            || !is_hex_revision(mod.revision) || !slugs.insert(mod.slug).second) {
            throw std::runtime_error("Complete runtime manifest has invalid mod identity");
        }
        if (!value.contains("files") || !value["files"].is_array()
            || value["files"].empty()) {
            throw std::runtime_error("Complete runtime manifest mod has no files");
        }
        std::set<std::string> exact;
        std::set<std::string> folded;
        for (const auto& file_value : value["files"]) {
            if (!file_value.is_object()) {
                throw std::runtime_error("Complete runtime file is not an object");
            }
            const auto relative_text = file_value.value("path", std::string{});
            const auto url = file_value.value("url", std::string{});
            const std::filesystem::path relative(relative_text);
            if (!safe_relative_path(relative) || !allowed_drive_url(url)
                || !exact.insert(relative_text).second) {
                throw std::runtime_error("Complete runtime manifest has unsafe file metadata");
            }
            std::string folded_text = relative_text;
            std::transform(
                folded_text.begin(),
                folded_text.end(),
                folded_text.begin(),
                [](const unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                }
            );
            if (!folded.insert(folded_text).second) {
                throw std::runtime_error("Complete runtime manifest has case-colliding paths");
            }
            mod.files.push_back({relative, url});
        }
        if (mod.files.size() < mod.expected_min_files) {
            throw std::runtime_error("Complete runtime manifest misses descriptor files");
        }
        manifest.mods.push_back(std::move(mod));
    }
    return manifest;
}

[[nodiscard]] inline CompleteRuntimePlan plan_complete_runtime_content(
    const CompleteRuntimeManifest& manifest,
    const std::filesystem::path& mods_root
) {
    using namespace complete_runtime_detail;
    const auto revisions = load_state_revisions(
        mods_root / ".pulseforge-complete-state.json"
    );
    CompleteRuntimePlan plan;
    plan.content_revision = manifest.content_revision;
    std::error_code error;
    for (const auto& mod : manifest.mods) {
        const bool installed = std::filesystem::is_directory(
            mods_root / mod.slug,
            error
        ) && !error;
        error.clear();
        const auto iterator = revisions.find(mod.slug);
        plan.entries.push_back({
            .mod = &mod,
            .installed = installed,
            .revision_current = installed && iterator != revisions.end()
                && iterator->second == mod.revision,
        });
    }
    return plan;
}

[[nodiscard]] inline bool validate_complete_runtime_staging(
    const CompleteRuntimeMod& mod,
    const std::filesystem::path& staging,
    std::uint64_t& file_count,
    std::uint64_t& total_bytes,
    std::string& error
) {
    using namespace complete_runtime_detail;
    file_count = 0U;
    total_bytes = 0U;
    std::set<std::string> expected;
    for (const auto& file : mod.files) {
        expected.insert(path_utf8(file.relative));
    }
    std::set<std::string> actual;
    std::error_code filesystem_error;
    if (!std::filesystem::is_directory(staging, filesystem_error)
        || filesystem_error || std::filesystem::is_symlink(staging, filesystem_error)
        || filesystem_error) {
        error = "Complete staging root is not a safe directory";
        return false;
    }
    std::filesystem::recursive_directory_iterator iterator(
        staging,
        std::filesystem::directory_options::skip_permission_denied,
        filesystem_error
    );
    const std::filesystem::recursive_directory_iterator end;
    for (; !filesystem_error && iterator != end;
         iterator.increment(filesystem_error)) {
        std::error_code status_error;
        if (iterator->is_symlink(status_error) && !status_error) {
            error = "Complete staging contains a symbolic link";
            return false;
        }
        if (status_error || iterator->is_directory(status_error)) {
            if (status_error) {
                error = "Complete staging entry cannot be inspected";
                return false;
            }
            continue;
        }
        if (!iterator->is_regular_file(status_error) || status_error) {
            error = "Complete staging contains an unsupported file type";
            return false;
        }
        const auto relative = iterator->path().lexically_relative(staging);
        const auto relative_text = path_utf8(relative);
        if (!safe_relative_path(relative) || !actual.insert(relative_text).second) {
            error = "Complete staging contains an unsafe or duplicate path";
            return false;
        }
        const auto size = iterator->file_size(status_error);
        if (status_error || total_bytes > UINT64_MAX - size) {
            error = "Complete staging size cannot be measured safely";
            return false;
        }
        total_bytes += size;
        ++file_count;
    }
    if (filesystem_error) {
        error = "Complete staging enumeration failed";
        return false;
    }
    if (actual != expected || file_count < mod.expected_min_files
        || total_bytes < mod.expected_min_bytes) {
        error = "Complete staging does not match the Drive manifest";
        return false;
    }
    return true;
}

[[nodiscard]] inline CompleteRuntimeInstallResult install_complete_runtime_staging(
    const CompleteRuntimeManifest& manifest,
    const CompleteRuntimeMod& mod,
    const std::filesystem::path& staging,
    const std::filesystem::path& mods_root
) {
    using namespace complete_runtime_detail;
    CompleteRuntimeInstallResult result;
    if (!valid_slug(mod.slug)) {
        result.error = "invalid Complete destination slug";
        return result;
    }
    if (!validate_complete_runtime_staging(
            mod,
            staging,
            result.installed_files,
            result.installed_bytes,
            result.error
        )) {
        return result;
    }

    std::error_code error;
    std::filesystem::create_directories(mods_root, error);
    if (error) {
        result.error = "cannot create Complete mods root";
        return result;
    }
    const auto destination = mods_root / mod.slug;
    const auto rollback = mods_root / ("." + mod.slug + ".complete-rollback");
    std::filesystem::remove_all(rollback, error);
    error.clear();
    const bool replacing = std::filesystem::exists(destination, error) && !error;
    if (replacing) {
        std::filesystem::rename(destination, rollback, error);
        if (error) {
            result.error = "cannot stage existing Complete mod for replacement";
            return result;
        }
    }

    ModInstallOptions options;
    options.destination_name = mod.slug;
    const auto installed = install_mod(staging, mods_root, options);
    if (!installed) {
        std::filesystem::remove_all(destination, error);
        if (replacing) {
            error.clear();
            std::filesystem::rename(rollback, destination, error);
        }
        result.error = "Complete mod installation failed: " + installed.error;
        return result;
    }

    try {
        ensure_mods_list_entry(mods_root, mod);
        persist_state_revision(manifest, mods_root, mod);
    } catch (const std::exception& state_error) {
        std::filesystem::remove_all(destination, error);
        if (replacing) {
            error.clear();
            std::filesystem::rename(rollback, destination, error);
        }
        result.error = std::string("Complete state commit failed: ")
            + state_error.what();
        return result;
    }

    if (replacing) {
        std::filesystem::remove_all(rollback, error);
    }
    result.success = true;
    result.installed_path = installed.installed_path;
    return result;
}

}  // namespace pulseforge::detail
