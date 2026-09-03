#include "pulseforge/virtual_file_system.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Function>
void require_error(
    const pulseforge::VfsErrorCode expected,
    Function&& function,
    const std::string_view message
) {
    try {
        std::forward<Function>(function)();
    } catch (const pulseforge::VirtualFileSystemError& error) {
        require(error.code() == expected, message);
        return;
    }
    throw std::runtime_error(std::string(message));
}

void write_text(
    const std::filesystem::path& path,
    const std::string_view text
) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to create VFS fixture");
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) {
        throw std::runtime_error("failed to write VFS fixture");
    }
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto nonce = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("pulseforge-vfs-test-" + std::to_string(nonce));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void test_overlay_and_provenance(const std::filesystem::path& root) {
    const auto base = root / "base";
    const auto high = root / "high";
    const auto first = root / "first";
    const auto second = root / "second";
    std::filesystem::create_directories(base);
    std::filesystem::create_directories(high);
    std::filesystem::create_directories(first);
    std::filesystem::create_directories(second);

    write_text(base / "shared.txt", "base");
    write_text(high / "shared.txt", "high");
    write_text(base / "fallback.txt", "fallback");
    write_text(first / "tie.txt", "first");
    write_text(second / "tie.txt", "second");
    write_text(second / "data" / "songs" / "entry.json", "normalized");

    pulseforge::VirtualFileSystemOptions options;
    options.mounts = {
        {"base", base, 0},
        {"high", high, 20},
        {"first", first, 10},
        {"second", second, 10},
    };
    options.max_read_bytes = 1'024U;
    const pulseforge::VirtualFileSystem vfs(std::move(options));

    require(vfs.mounts().size() == 4U, "all mounts are retained");
    require(vfs.mounts()[0].id == "high", "higher priority resolves first");
    require(
        vfs.mounts()[1].id == "second",
        "later equal-priority mount resolves first"
    );
    require(vfs.mounts()[2].id == "first", "equal priorities are stable");

    const auto shared = vfs.read_text("shared.txt");
    require(shared.text == "high", "highest-priority content wins");
    require(
        shared.source.provenance.mount_id == "high",
        "read result exposes its mount id"
    );
    require(
        shared.source.provenance.mount_priority == 20,
        "read result exposes its mount priority"
    );

    const auto tie = vfs.read_text("tie.txt");
    require(tie.text == "second", "declaration order breaks priority ties");
    require(
        tie.source.provenance.mount_declaration_order == 3U,
        "provenance retains original declaration order"
    );

    const auto fallback = vfs.resolve("fallback.txt");
    require(fallback.has_value(), "lower-priority fallback resolves");
    require(
        fallback->provenance.mount_id == "base",
        "fallback provenance identifies the base mount"
    );
    require(
        fallback->provenance.physical_path.is_absolute(),
        "resolved physical paths are canonical and absolute"
    );
    require(vfs.exists("fallback.txt"), "exists reports a regular file");
    require(!vfs.exists("missing.txt"), "exists reports a missing file");

    const auto normalized = vfs.read_text("data//./songs\\entry.json");
    require(normalized.text == "normalized", "mixed separators normalize");
    require(
        normalized.source.virtual_path == "data/songs/entry.json",
        "normalized virtual path is reported"
    );
}

void test_invalid_paths(const std::filesystem::path& root) {
    const auto mount = root / "invalid-path-mount";
    std::filesystem::create_directories(mount);
    pulseforge::VirtualFileSystemOptions options;
    options.mounts = {{"base", mount, 0}};
    const pulseforge::VirtualFileSystem vfs(std::move(options));

    std::vector<std::string> invalid_paths{
        "",
        ".",
        "..",
        "../secret.txt",
        "safe/../../secret.txt",
        "/absolute.txt",
        "\\absolute.txt",
        "C:/absolute.txt",
        "C:drive-relative.txt",
        "//server/share/file.txt",
        "\\\\server\\share\\file.txt",
    };
    auto null_path = std::string("safe/file.txt");
    null_path.insert(null_path.begin() + 4, '\0');
    invalid_paths.push_back(std::move(null_path));
    auto invalid_utf8 = std::string("invalid-");
    invalid_utf8.push_back(static_cast<char>(0xFFU));
    invalid_paths.push_back(std::move(invalid_utf8));
    for (const auto& path : invalid_paths) {
        require_error(
            pulseforge::VfsErrorCode::invalid_virtual_path,
            [&vfs, &path] { static_cast<void>(vfs.resolve(path)); },
            "unsafe virtual path must be rejected"
        );
    }
}

void test_bounded_reads(const std::filesystem::path& root) {
    const auto mount = root / "bounded-mount";
    std::filesystem::create_directories(mount);
    write_text(mount / "small.bin", "0123456789");
    write_text(mount / "empty.bin", "");

    pulseforge::VirtualFileSystemOptions options;
    options.mounts = {{"base", mount, 0}};
    options.max_read_bytes = 8U;
    const pulseforge::VirtualFileSystem vfs(std::move(options));

    require_error(
        pulseforge::VfsErrorCode::read_limit_exceeded,
        [&vfs] { static_cast<void>(vfs.read_binary("small.bin")); },
        "snapshot read cap must be enforced"
    );
    require_error(
        pulseforge::VfsErrorCode::read_limit_exceeded,
        [&vfs] { static_cast<void>(vfs.read_binary("small.bin", 4U)); },
        "per-call read cap must narrow the snapshot cap"
    );
    require(
        vfs.read_binary("empty.bin", 0U).bytes.empty(),
        "a zero-byte cap can read an empty file"
    );
    require_error(
        pulseforge::VfsErrorCode::not_found,
        [&vfs] { static_cast<void>(vfs.read_text("missing.txt")); },
        "read distinguishes missing files"
    );
}

void test_mount_validation(const std::filesystem::path& root) {
    const auto mount = root / "valid-mount";
    std::filesystem::create_directories(mount);

    require_error(
        pulseforge::VfsErrorCode::duplicate_mount_id,
        [&mount] {
            pulseforge::VirtualFileSystemOptions options;
            options.mounts = {{"same", mount, 0}, {"same", mount, 1}};
            const pulseforge::VirtualFileSystem vfs(std::move(options));
            static_cast<void>(vfs);
        },
        "duplicate mount ids must be rejected"
    );
    require_error(
        pulseforge::VfsErrorCode::invalid_mount,
        [&root] {
            pulseforge::VirtualFileSystemOptions options;
            options.mounts = {{"missing", root / "does-not-exist", 0}};
            const pulseforge::VirtualFileSystem vfs(std::move(options));
            static_cast<void>(vfs);
        },
        "missing mount roots must be rejected"
    );
    require_error(
        pulseforge::VfsErrorCode::mount_limit_exceeded,
        [&mount] {
            pulseforge::VirtualFileSystemOptions options;
            options.mounts = {{"one", mount, 0}};
            options.max_mounts = 0U;
            const pulseforge::VirtualFileSystem vfs(std::move(options));
            static_cast<void>(vfs);
        },
        "mount count limit must be enforced"
    );
}

void test_symlink_confinement(const std::filesystem::path& root) {
    const auto mount = root / "symlink-mount";
    const auto outside = root / "outside";
    std::filesystem::create_directories(mount);
    std::filesystem::create_directories(outside);
    write_text(outside / "secret.txt", "secret");
    write_text(mount / "inside.txt", "inside");

    pulseforge::VirtualFileSystemOptions options;
    options.mounts = {{"base", mount, 0}};
    const pulseforge::VirtualFileSystem vfs(std::move(options));

    std::error_code outside_link_error;
    std::filesystem::create_directory_symlink(
        outside,
        mount / "escape",
        outside_link_error
    );
    if (!outside_link_error) {
        require_error(
            pulseforge::VfsErrorCode::symlink_escape,
            [&vfs] {
                static_cast<void>(vfs.resolve("escape/secret.txt"));
            },
            "a directory symlink must not escape its mount"
        );
    } else {
        std::cout << "Symlink escape fixture skipped: "
                  << outside_link_error.message() << '\n';
    }

    std::error_code inside_link_error;
    std::filesystem::create_symlink(
        mount / "inside.txt",
        mount / "inside-link.txt",
        inside_link_error
    );
    if (!inside_link_error) {
        require(
            vfs.read_text("inside-link.txt").text == "inside",
            "symlinks that remain inside a mount are allowed"
        );
    } else {
        std::cout << "Internal symlink fixture skipped: "
                  << inside_link_error.message() << '\n';
    }
}

}  // namespace

int main() {
    try {
        TemporaryDirectory temporary;
        test_overlay_and_provenance(temporary.path());
        test_invalid_paths(temporary.path());
        test_bounded_reads(temporary.path());
        test_mount_validation(temporary.path());
        test_symlink_confinement(temporary.path());
        std::cout << "Virtual file system tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "Virtual file system tests failed: "
                  << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
