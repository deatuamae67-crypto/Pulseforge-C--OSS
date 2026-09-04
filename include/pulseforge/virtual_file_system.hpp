#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge {

enum class VfsErrorCode : std::uint8_t {
    invalid_virtual_path,
    invalid_mount,
    duplicate_mount_id,
    mount_limit_exceeded,
    symlink_escape,
    not_found,
    read_limit_exceeded,
    io_failure,
};

class VirtualFileSystemError final : public std::runtime_error {
public:
    VirtualFileSystemError(
        VfsErrorCode code,
        std::string message,
        std::string virtual_path = {},
        std::filesystem::path physical_path = {}
    );

    [[nodiscard]] VfsErrorCode code() const noexcept;
    [[nodiscard]] const std::string& virtual_path() const noexcept;
    [[nodiscard]] const std::filesystem::path& physical_path() const noexcept;

private:
    VfsErrorCode code_;
    std::string virtual_path_;
    std::filesystem::path physical_path_;
};

struct DirectoryMount {
    std::string id;
    std::filesystem::path root;
    std::int32_t priority{};
};

// This is the canonical, immutable form of a DirectoryMount. The collection
// returned by VirtualFileSystem::mounts() is already in lookup order.
struct MountedDirectory {
    std::string id;
    std::filesystem::path canonical_root;
    std::int32_t priority{};
    std::size_t declaration_order{};
};

struct VfsProvenance {
    std::string mount_id;
    std::filesystem::path mount_root;
    std::filesystem::path physical_path;
    std::int32_t mount_priority{};
    std::size_t mount_declaration_order{};
};

struct ResolvedVirtualFile {
    // UTF-8, slash-separated, relative and lexically normalized.
    std::string virtual_path;
    VfsProvenance provenance;
    std::uintmax_t size_bytes{};
};

struct BinaryVirtualFile {
    ResolvedVirtualFile source;
    std::vector<std::byte> bytes;
};

struct TextVirtualFile {
    ResolvedVirtualFile source;
    std::string text;
};

struct VirtualFileSystemOptions {
    std::vector<DirectoryMount> mounts;
    std::uintmax_t max_read_bytes{64U * 1024U * 1024U};
    std::size_t max_mounts{1'024U};
};

// An immutable directory-overlay snapshot. Higher priorities win. When two
// mounts have the same priority, the one declared later wins. Copies share the
// frozen mount table and are safe to query concurrently.
class VirtualFileSystem final {
public:
    explicit VirtualFileSystem(VirtualFileSystemOptions options);

    [[nodiscard]] static std::string normalize_virtual_path(
        std::string_view virtual_path
    );

    [[nodiscard]] std::span<const MountedDirectory> mounts() const noexcept;
    [[nodiscard]] std::uintmax_t max_read_bytes() const noexcept;

    // Missing files return nullopt. Invalid paths, I/O failures and detected
    // symlink escapes throw VirtualFileSystemError.
    [[nodiscard]] std::optional<ResolvedVirtualFile> resolve(
        std::string_view virtual_path
    ) const;
    [[nodiscard]] bool exists(std::string_view virtual_path) const;

    [[nodiscard]] BinaryVirtualFile read_binary(
        std::string_view virtual_path
    ) const;
    [[nodiscard]] BinaryVirtualFile read_binary(
        std::string_view virtual_path,
        std::uintmax_t max_bytes
    ) const;
    [[nodiscard]] TextVirtualFile read_text(
        std::string_view virtual_path
    ) const;
    [[nodiscard]] TextVirtualFile read_text(
        std::string_view virtual_path,
        std::uintmax_t max_bytes
    ) const;

private:
    struct Snapshot;
    std::shared_ptr<const Snapshot> snapshot_;
};

}  // namespace pulseforge
