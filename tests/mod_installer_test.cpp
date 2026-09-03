#include "pulseforge/mod_installer.hpp"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4267)
#endif
#include <miniz.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <archive.h>
#include <archive_entry.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class TempTree final {
public:
    TempTree() {
        static std::atomic<unsigned long long> sequence{};
        const auto stamp = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
        root = std::filesystem::current_path()
            / ("pulseforge-mod-installer-test-" + std::to_string(stamp)
                + "-" + std::to_string(sequence.fetch_add(1U)));
        std::filesystem::create_directories(root);
    }

    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    std::filesystem::path root;
};

void write_text(
    const std::filesystem::path& path,
    const std::string_view text
) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(static_cast<bool>(output), "test fixture write failed");
}

void write_zip(
    const std::filesystem::path& path,
    const std::vector<std::pair<std::string, std::string>>& entries
) {
    mz_zip_archive archive{};
#if defined(_WIN32)
    std::FILE* file{};
    require(
        _wfopen_s(&file, path.c_str(), L"wb") == 0 && file != nullptr,
        "ZIP fixture file creation failed"
    );
    const bool initialized = mz_zip_writer_init_cfile(&archive, file, 0) != 0;
#else
    const auto filename = path.string();
    const bool initialized = mz_zip_writer_init_file(
        &archive,
        filename.c_str(),
        0
    ) != 0;
#endif
    require(initialized, "ZIP fixture initialization failed");
    bool success = true;
    for (const auto& [name, content] : entries) {
        success = success && mz_zip_writer_add_mem(
            &archive,
            name.c_str(),
            content.data(),
            content.size(),
            MZ_BEST_COMPRESSION
        ) != 0;
    }
    success = success && mz_zip_writer_finalize_archive(&archive) != 0;
    success = mz_zip_writer_end(&archive) != 0 && success;
#if defined(_WIN32)
    success = std::fclose(file) == 0 && success;
#endif
    require(success, "ZIP fixture creation failed");
}

void write_tar(
    const std::filesystem::path& path,
    const std::vector<std::pair<std::string, std::string>>& entries
) {
    archive* writer = archive_write_new();
    require(writer != nullptr, "TAR fixture allocation failed");
    bool success = archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK;
#if defined(_WIN32)
    success = success
        && archive_write_open_filename_w(writer, path.c_str()) == ARCHIVE_OK;
#else
    success = success
        && archive_write_open_filename(writer, path.c_str()) == ARCHIVE_OK;
#endif
    for (const auto& [name, content] : entries) {
        archive_entry* entry = archive_entry_new();
        require(entry != nullptr, "TAR entry allocation failed");
        archive_entry_set_pathname(entry, name.c_str());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(entry, static_cast<la_int64_t>(content.size()));
        success = success
            && archive_write_header(writer, entry) == ARCHIVE_OK
            && archive_write_data(writer, content.data(), content.size())
                == static_cast<la_ssize_t>(content.size());
        archive_entry_free(entry);
    }
    success = archive_write_close(writer) == ARCHIVE_OK && success;
    success = archive_write_free(writer) == ARCHIVE_OK && success;
    require(success, "TAR fixture creation failed");
}

void write_7z(
    const std::filesystem::path& path,
    const std::vector<std::pair<std::string, std::string>>& entries
) {
    archive* writer = archive_write_new();
    require(writer != nullptr, "7z fixture allocation failed");
    bool success = archive_write_set_format_7zip(writer) == ARCHIVE_OK;
#if defined(_WIN32)
    success = success
        && archive_write_open_filename_w(writer, path.c_str()) == ARCHIVE_OK;
#else
    success = success
        && archive_write_open_filename(writer, path.c_str()) == ARCHIVE_OK;
#endif
    for (const auto& [name, content] : entries) {
        archive_entry* entry = archive_entry_new();
        require(entry != nullptr, "7z entry allocation failed");
        archive_entry_set_pathname(entry, name.c_str());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(
            entry,
            static_cast<la_int64_t>(content.size())
        );
        success = success
            && archive_write_header(writer, entry) == ARCHIVE_OK
            && archive_write_data(writer, content.data(), content.size())
                == static_cast<la_ssize_t>(content.size());
        archive_entry_free(entry);
    }
    success = archive_write_close(writer) == ARCHIVE_OK && success;
    success = archive_write_free(writer) == ARCHIVE_OK && success;
    require(success, "7z fixture creation failed");
}

void require_rejected_without_staging(
    const std::filesystem::path& package,
    const std::filesystem::path& mods,
    const std::string_view message
) {
    const auto result = pulseforge::install_mod(package, mods);
    require(!result, message);
    require(!result.error.empty(), "rejected install did not explain its error");
    require(
        !std::filesystem::exists(mods) || std::filesystem::is_empty(mods),
        "rejected install left a destination or staging directory behind"
    );
}

void test_directory_install() {
    TempTree tree;
    const auto source = tree.root / "Source Mod";
    const auto mods = tree.root / "mods";
    write_text(source / "pack.json", R"({"name":"Source Mod"})");
    write_text(source / "data" / "song" / "song.json", "{}");

    const auto result = pulseforge::install_mod(source, mods);
    require(static_cast<bool>(result), result.error);
    require(result.mod_id == "source-mod", "directory mod id mismatch");
    require(result.installed_files == 2U, "directory file count mismatch");
    require(
        std::filesystem::is_regular_file(
            result.installed_path / "data" / "song" / "song.json"
        ),
        "directory chart was not installed"
    );
    const auto duplicate = pulseforge::install_mod(source, mods);
    require(!duplicate, "installer overwrote an existing mod");
}

void test_zip_install_and_root_strip() {
    TempTree tree;
    const auto archive = tree.root / "Pretty Mod.zip";
    write_zip(archive, {
        {"Wrapper/pack.json", R"({"name":"Pretty Mod"})"},
        {"Wrapper/data/song/song-hard.json", R"({"song":{"notes":[]}})"},
    });

    const auto result = pulseforge::install_mod(archive, tree.root / "mods");
    require(static_cast<bool>(result), result.error);
    require(result.mod_id == "pretty-mod", "ZIP mod id mismatch");
    require(result.installed_files == 2U, "ZIP file count mismatch");
    require(
        std::filesystem::is_regular_file(result.installed_path / "pack.json"),
        "common ZIP root was not stripped"
    );
    require(
        !std::filesystem::exists(result.installed_path / "Wrapper"),
        "common ZIP root remained in destination"
    );
}

void test_zip_slip_and_executables_rejected() {
    TempTree tree;
    const auto traversal = tree.root / "traversal.zip";
    write_zip(traversal, {{"../escape.txt", "no"}});
    const auto traversal_result = pulseforge::install_mod(
        traversal,
        tree.root / "mods"
    );
    require(!traversal_result, "ZIP traversal path was accepted");
    require(
        !std::filesystem::exists(tree.root / "escape.txt"),
        "ZIP traversal escaped the mods root"
    );

    const auto executable = tree.root / "executable.zip";
    write_zip(executable, {
        {"Mod/pack.json", "{}"},
        {"Mod/payload.exe", "MZ"},
    });
    const auto executable_result = pulseforge::install_mod(
        executable,
        tree.root / "mods"
    );
    require(!executable_result, "executable ZIP member was accepted");
}

void test_zip_windows_path_aliases_rejected() {
    TempTree tree;
    const std::vector<std::pair<std::string, std::vector<
        std::pair<std::string, std::string>>>> cases{
        {
            "trailing-dot.zip",
            {{"Mod/pack.json", "{}"}, {"Mod/payload.exe.", "MZ"}},
        },
        {
            "trailing-space.zip",
            {{"Mod/pack.json", "{}"}, {"Mod/payload.exe ", "MZ"}},
        },
        {
            "device.zip",
            {{"Mod/pack.json", "{}"}, {"Mod/NUL.txt", "blocked"}},
        },
        {
            "device-extension.zip",
            {{"Mod/pack.json", "{}"}, {"Mod/COM9.chart.json", "blocked"}},
        },
        {
            "ads.zip",
            {{"Mod/pack.json", "{}"}, {"Mod/chart.json:payload.exe", "MZ"}},
        },
        {
            "case-collision.zip",
            {
                {"Mod/Data/chart.json", "first"},
                {"Mod/data/CHART.JSON", "second"},
            },
        },
        {
            "file-directory-collision.zip",
            {{"Mod/assets", "file"}, {"Mod/ASSETS/image.png", "image"}},
        },
    };

    for (const auto& [filename, entries] : cases) {
        const auto package = tree.root / filename;
        write_zip(package, entries);
        require_rejected_without_staging(
            package,
            tree.root / (filename + "-mods"),
            "ZIP with a Windows path alias was accepted"
        );
    }
}

void test_7z_install_and_traversal_rejected() {
    TempTree tree;
    const auto package = tree.root / "Seven Zip Mod.7Z";
    write_7z(package, {
        {"Wrapper/pack.json", R"({"name":"Seven Zip Mod"})"},
        {"Wrapper/data/song/song.json", R"({"song":{"notes":[]}})"},
    });
    require(
        pulseforge::is_supported_mod_archive(package),
        "case-insensitive 7z extension was not recognized"
    );
    const auto installed = pulseforge::install_mod(
        package,
        tree.root / "mods"
    );
    require(static_cast<bool>(installed), installed.error);
    require(installed.installed_files == 2U, "7z file count mismatch");
    require(
        std::filesystem::is_regular_file(
            installed.installed_path / "data/song/song.json"
        ),
        "7z package was not installed with its common root stripped"
    );

    const auto traversal = tree.root / "traversal.7z";
    write_7z(traversal, {{"../escape.txt", "no"}});
    const auto rejected = pulseforge::install_mod(
        traversal,
        tree.root / "other-mods"
    );
    require(!rejected, "7z traversal path was accepted");
    require(
        !std::filesystem::exists(tree.root / "escape.txt"),
        "7z traversal escaped the mods root"
    );
}

void test_7z_windows_path_aliases_rejected() {
    TempTree tree;
    const auto collision = tree.root / "case-collision.7z";
    write_7z(collision, {
        {"Mod/Data/chart.json", "first"},
        {"Mod/data/CHART.JSON", "second"},
    });
    require_rejected_without_staging(
        collision,
        tree.root / "collision-mods",
        "7z case-insensitive destination collision was accepted"
    );

    const auto trailing = tree.root / "trailing-dot.7z";
    write_7z(trailing, {
        {"Mod/pack.json", "{}"},
        {"Mod/payload.exe.", "MZ"},
    });
    require_rejected_without_staging(
        trailing,
        tree.root / "trailing-mods",
        "7z trailing-dot executable alias was accepted"
    );
}

void test_supported_archive_extensions() {
    require(
        pulseforge::is_supported_mod_archive("mod.rar"),
        "RAR extension was not recognized"
    );
    require(
        pulseforge::is_supported_mod_archive("mod.TAR"),
        "TAR extension was not recognized"
    );
    require(
        !pulseforge::is_supported_mod_archive("mod.exe"),
        "unsupported archive extension was recognized"
    );
}

void test_tar_and_unicode_source_install() {
    TempTree tree;
    const auto tar = tree.root / L"Mód Unicode.tar";
    write_tar(tar, {
        {"Wrapper/pack.json", R"({"name":"Unicode"})"},
        {"Wrapper/data/song/song.json", "{}"},
    });
    const auto tar_result = pulseforge::install_mod(tar, tree.root / "tar-mods");
    require(static_cast<bool>(tar_result), tar_result.error);
    require(
        std::filesystem::is_regular_file(
            tar_result.installed_path / "data/song/song.json"
        ),
        "real TAR package with Unicode source path was not installed"
    );

    const auto zip = tree.root / L"Zíp Unicode.zip";
    write_zip(zip, {{"Wrapper/dados/ação.json", "{}"}});
    const auto zip_result = pulseforge::install_mod(zip, tree.root / "zip-mods");
    require(static_cast<bool>(zip_result), zip_result.error);
    require(zip_result.installed_files == 1U, "UTF-8 ZIP member was not installed");
}

void test_install_budgets() {
    TempTree tree;
    const auto archive = tree.root / "large.zip";
    write_zip(archive, {{"Mod/chart.json", std::string(128U, 'x')}});
    pulseforge::ModInstallOptions options;
    options.limits.max_single_file_bytes = 32U;
    options.limits.max_total_uncompressed_bytes = 64U;
    const auto result = pulseforge::install_mod(
        archive,
        tree.root / "mods",
        options
    );
    require(!result, "oversized ZIP member was accepted");
}

}  // namespace

int main() {
    try {
        test_directory_install();
        test_zip_install_and_root_strip();
        test_zip_slip_and_executables_rejected();
        test_zip_windows_path_aliases_rejected();
        test_7z_install_and_traversal_rejected();
        test_7z_windows_path_aliases_rejected();
        test_supported_archive_extensions();
        test_tar_and_unicode_source_install();
        test_install_budgets();
        std::cout << "PulseForge mod installer tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "PulseForge mod installer test failed: "
                  << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
