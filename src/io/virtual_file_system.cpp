#include "pulseforge/virtual_file_system.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace pulseforge {
namespace {

constexpr std::size_t kReadChunkBytes = 64U * 1024U;

[[noreturn]] void throw_vfs_error(
    const VfsErrorCode code,
    std::string message,
    std::string virtual_path = {},
    std::filesystem::path physical_path = {}
) {
    throw VirtualFileSystemError(
        code,
        std::move(message),
        std::move(virtual_path),
        std::move(physical_path)
    );
}

[[nodiscard]] bool is_ascii_alpha(const char value) noexcept {
    const auto byte = static_cast<unsigned char>(value);
    return std::isalpha(byte) != 0;
}

[[nodiscard]] bool is_missing_error(const std::error_code& error) noexcept {
    return error == std::errc::no_such_file_or_directory
        || error == std::errc::not_a_directory;
}

[[nodiscard]] bool is_valid_utf8(const std::string_view value) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
    std::size_t index = 0U;
    while (index < value.size()) {
        const auto first = bytes[index];
        if (first <= 0x7FU) {
            ++index;
            continue;
        }

        if (first >= 0xC2U && first <= 0xDFU) {
            if (index + 1U >= value.size()
                || bytes[index + 1U] < 0x80U
                || bytes[index + 1U] > 0xBFU) {
                return false;
            }
            index += 2U;
            continue;
        }

        if (first >= 0xE0U && first <= 0xEFU) {
            if (index + 2U >= value.size()) {
                return false;
            }
            const auto second = bytes[index + 1U];
            const auto third = bytes[index + 2U];
            const bool second_is_valid = first == 0xE0U
                ? second >= 0xA0U && second <= 0xBFU
                : first == 0xEDU
                    ? second >= 0x80U && second <= 0x9FU
                    : second >= 0x80U && second <= 0xBFU;
            if (!second_is_valid || third < 0x80U || third > 0xBFU) {
                return false;
            }
            index += 3U;
            continue;
        }

        if (first >= 0xF0U && first <= 0xF4U) {
            if (index + 3U >= value.size()) {
                return false;
            }
            const auto second = bytes[index + 1U];
            const auto third = bytes[index + 2U];
            const auto fourth = bytes[index + 3U];
            const bool second_is_valid = first == 0xF0U
                ? second >= 0x90U && second <= 0xBFU
                : first == 0xF4U
                    ? second >= 0x80U && second <= 0x8FU
                    : second >= 0x80U && second <= 0xBFU;
            if (!second_is_valid || third < 0x80U || third > 0xBFU
                || fourth < 0x80U || fourth > 0xBFU) {
                return false;
            }
            index += 4U;
            continue;
        }

        return false;
    }
    return true;
}

[[nodiscard]] bool path_stays_below(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& canonical_candidate
) {
    const auto relative = canonical_candidate.lexically_relative(canonical_root);
    if (relative.empty() || relative.has_root_name()
        || relative.has_root_directory() || relative.is_absolute()) {
        return false;
    }

    for (const auto& component : relative) {
        if (component == "..") {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::filesystem::path path_from_utf8(
    const std::string_view utf8_path
) {
    std::u8string native_utf8;
    native_utf8.reserve(utf8_path.size());
    for (const char character : utf8_path) {
        native_utf8.push_back(
            static_cast<char8_t>(static_cast<unsigned char>(character))
        );
    }
    return std::filesystem::path(native_utf8);
}

[[nodiscard]] std::filesystem::path make_physical_candidate(
    const MountedDirectory& mount,
    const std::string& normalized_virtual_path
) {
    return mount.canonical_root / path_from_utf8(normalized_virtual_path);
}

[[nodiscard]] std::string mount_error_message(
    const std::string_view prefix,
    const std::string& mount_id,
    const std::error_code& error = {}
) {
    std::string message(prefix);
    message.append(" '");
    message.append(mount_id);
    message.push_back('\'');
    if (error) {
        message.append(": ");
        message.append(error.message());
    }
    return message;
}

}  // namespace

struct VirtualFileSystem::Snapshot final {
    std::vector<MountedDirectory> mounts;
    std::uintmax_t max_read_bytes{};
};

VirtualFileSystemError::VirtualFileSystemError(
    const VfsErrorCode code,
    std::string message,
    std::string virtual_path,
    std::filesystem::path physical_path
)
    : std::runtime_error(std::move(message)),
      code_(code),
      virtual_path_(std::move(virtual_path)),
      physical_path_(std::move(physical_path)) {}

VfsErrorCode VirtualFileSystemError::code() const noexcept {
    return code_;
}

const std::string& VirtualFileSystemError::virtual_path() const noexcept {
    return virtual_path_;
}

const std::filesystem::path& VirtualFileSystemError::physical_path()
    const noexcept {
    return physical_path_;
}

VirtualFileSystem::VirtualFileSystem(VirtualFileSystemOptions options) {
    if (options.max_read_bytes == 0U) {
        throw_vfs_error(
            VfsErrorCode::invalid_mount,
            "the VFS read limit must be greater than zero"
        );
    }
    if (options.mounts.size() > options.max_mounts) {
        throw_vfs_error(
            VfsErrorCode::mount_limit_exceeded,
            "the VFS mount count exceeds the configured limit"
        );
    }

    auto snapshot = std::make_shared<Snapshot>();
    snapshot->max_read_bytes = options.max_read_bytes;
    snapshot->mounts.reserve(options.mounts.size());

    std::unordered_set<std::string> mount_ids;
    mount_ids.reserve(options.mounts.size());

    for (std::size_t index = 0U; index < options.mounts.size(); ++index) {
        auto& input = options.mounts[index];
        if (input.id.empty()) {
            throw_vfs_error(
                VfsErrorCode::invalid_mount,
                "a VFS mount has an empty id",
                {},
                input.root
            );
        }
        if (!mount_ids.emplace(input.id).second) {
            throw_vfs_error(
                VfsErrorCode::duplicate_mount_id,
                mount_error_message("duplicate VFS mount id", input.id),
                {},
                input.root
            );
        }
        if (input.root.empty()) {
            throw_vfs_error(
                VfsErrorCode::invalid_mount,
                mount_error_message("VFS mount has an empty root", input.id)
            );
        }

        std::error_code error;
        const auto canonical_root = std::filesystem::canonical(input.root, error);
        if (error) {
            throw_vfs_error(
                VfsErrorCode::invalid_mount,
                mount_error_message(
                    "cannot canonicalize VFS mount",
                    input.id,
                    error
                ),
                {},
                input.root
            );
        }

        const auto status = std::filesystem::status(canonical_root, error);
        if (error || !std::filesystem::is_directory(status)) {
            throw_vfs_error(
                VfsErrorCode::invalid_mount,
                mount_error_message(
                    "VFS mount root is not a readable directory",
                    input.id,
                    error
                ),
                {},
                canonical_root
            );
        }

        snapshot->mounts.push_back(MountedDirectory{
            std::move(input.id),
            canonical_root,
            input.priority,
            index,
        });
    }

    std::sort(
        snapshot->mounts.begin(),
        snapshot->mounts.end(),
        [](const MountedDirectory& left, const MountedDirectory& right) {
            if (left.priority != right.priority) {
                return left.priority > right.priority;
            }
            return left.declaration_order > right.declaration_order;
        }
    );

    snapshot_ = std::move(snapshot);
}

std::string VirtualFileSystem::normalize_virtual_path(
    const std::string_view virtual_path
) {
    if (virtual_path.empty()) {
        throw_vfs_error(
            VfsErrorCode::invalid_virtual_path,
            "a virtual path cannot be empty"
        );
    }

    std::string portable_path;
    portable_path.reserve(virtual_path.size());
    for (const char character : virtual_path) {
        if (character == '\0') {
            throw_vfs_error(
                VfsErrorCode::invalid_virtual_path,
                "a virtual path cannot contain a null byte",
                std::string(virtual_path)
            );
        }
        portable_path.push_back(character == '\\' ? '/' : character);
    }

    if (!is_valid_utf8(portable_path)) {
        throw_vfs_error(
            VfsErrorCode::invalid_virtual_path,
            "a virtual path must be valid UTF-8",
            std::string(virtual_path)
        );
    }

    if (portable_path.front() == '/' || (
            portable_path.size() >= 2U
            && is_ascii_alpha(portable_path.front())
            && portable_path[1] == ':')) {
        throw_vfs_error(
            VfsErrorCode::invalid_virtual_path,
            "a virtual path must not be absolute or have a root name",
            std::string(virtual_path)
        );
    }

    const std::filesystem::path native_check = path_from_utf8(portable_path);
    if (native_check.is_absolute() || native_check.has_root_name()
        || native_check.has_root_directory()) {
        throw_vfs_error(
            VfsErrorCode::invalid_virtual_path,
            "a virtual path must not be absolute or have a root name",
            std::string(virtual_path)
        );
    }

    std::string normalized;
    std::size_t cursor = 0U;
    while (cursor <= portable_path.size()) {
        const auto separator = portable_path.find('/', cursor);
        const auto end = separator == std::string::npos
            ? portable_path.size()
            : separator;
        const auto segment = std::string_view(portable_path).substr(
            cursor,
            end - cursor
        );

        if (segment == "..") {
            throw_vfs_error(
                VfsErrorCode::invalid_virtual_path,
                "a virtual path must not contain '..'",
                std::string(virtual_path)
            );
        }
        if (!segment.empty() && segment != ".") {
            if (!normalized.empty()) {
                normalized.push_back('/');
            }
            normalized.append(segment);
        }

        if (separator == std::string::npos) {
            break;
        }
        cursor = separator + 1U;
    }

    if (normalized.empty()) {
        throw_vfs_error(
            VfsErrorCode::invalid_virtual_path,
            "a virtual path must identify a file",
            std::string(virtual_path)
        );
    }
    return normalized;
}

std::span<const MountedDirectory> VirtualFileSystem::mounts() const noexcept {
    return snapshot_->mounts;
}

std::uintmax_t VirtualFileSystem::max_read_bytes() const noexcept {
    return snapshot_->max_read_bytes;
}

std::optional<ResolvedVirtualFile> VirtualFileSystem::resolve(
    const std::string_view virtual_path
) const {
    const auto normalized = normalize_virtual_path(virtual_path);

    for (const auto& mount : snapshot_->mounts) {
        const auto candidate = make_physical_candidate(mount, normalized);

        std::error_code error;
        const auto canonical_candidate =
            std::filesystem::weakly_canonical(candidate, error);
        if (error) {
            if (is_missing_error(error)) {
                continue;
            }
            throw_vfs_error(
                VfsErrorCode::io_failure,
                "failed to canonicalize a VFS candidate: " + error.message(),
                normalized,
                candidate
            );
        }

        if (!path_stays_below(mount.canonical_root, canonical_candidate)) {
            throw_vfs_error(
                VfsErrorCode::symlink_escape,
                "a VFS candidate escapes its mount through a symlink",
                normalized,
                canonical_candidate
            );
        }

        const auto status = std::filesystem::status(canonical_candidate, error);
        if (error) {
            if (is_missing_error(error)) {
                continue;
            }
            throw_vfs_error(
                VfsErrorCode::io_failure,
                "failed to inspect a VFS candidate: " + error.message(),
                normalized,
                canonical_candidate
            );
        }
        if (!std::filesystem::is_regular_file(status)) {
            continue;
        }

        const auto size = std::filesystem::file_size(canonical_candidate, error);
        if (error) {
            throw_vfs_error(
                VfsErrorCode::io_failure,
                "failed to measure a VFS file: " + error.message(),
                normalized,
                canonical_candidate
            );
        }

        return ResolvedVirtualFile{
            normalized,
            VfsProvenance{
                mount.id,
                mount.canonical_root,
                canonical_candidate,
                mount.priority,
                mount.declaration_order,
            },
            size,
        };
    }
    return std::nullopt;
}

bool VirtualFileSystem::exists(const std::string_view virtual_path) const {
    return resolve(virtual_path).has_value();
}

BinaryVirtualFile VirtualFileSystem::read_binary(
    const std::string_view virtual_path
) const {
    return read_binary(virtual_path, snapshot_->max_read_bytes);
}

BinaryVirtualFile VirtualFileSystem::read_binary(
    const std::string_view virtual_path,
    const std::uintmax_t max_bytes
) const {
    const auto effective_limit = std::min(max_bytes, snapshot_->max_read_bytes);
    auto source = resolve(virtual_path);
    if (!source) {
        throw_vfs_error(
            VfsErrorCode::not_found,
            "virtual file was not found",
            normalize_virtual_path(virtual_path)
        );
    }

    const auto addressable_limit = static_cast<std::uintmax_t>(
        std::numeric_limits<std::size_t>::max()
    );
    if (source->size_bytes > effective_limit
        || source->size_bytes > addressable_limit) {
        throw_vfs_error(
            VfsErrorCode::read_limit_exceeded,
            "virtual file exceeds the bounded read limit",
            source->virtual_path,
            source->provenance.physical_path
        );
    }

    std::ifstream input(source->provenance.physical_path, std::ios::binary);
    if (!input) {
        throw_vfs_error(
            VfsErrorCode::io_failure,
            "failed to open a resolved virtual file",
            source->virtual_path,
            source->provenance.physical_path
        );
    }

    std::vector<std::byte> bytes;
    bytes.reserve(static_cast<std::size_t>(source->size_bytes));
    std::array<char, kReadChunkBytes> buffer{};

    for (;;) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            const auto count_as_size = static_cast<std::size_t>(count);
            const auto size_as_uintmax =
                static_cast<std::uintmax_t>(bytes.size());
            const auto count_as_uintmax =
                static_cast<std::uintmax_t>(count_as_size);
            if (size_as_uintmax > effective_limit
                || count_as_uintmax > effective_limit - size_as_uintmax) {
                throw_vfs_error(
                    VfsErrorCode::read_limit_exceeded,
                    "virtual file grew beyond the bounded read limit",
                    source->virtual_path,
                    source->provenance.physical_path
                );
            }

            const auto old_size = bytes.size();
            bytes.resize(old_size + count_as_size);
            std::memcpy(
                bytes.data() + old_size,
                buffer.data(),
                count_as_size
            );
        }

        if (input.bad()) {
            throw_vfs_error(
                VfsErrorCode::io_failure,
                "failed while reading a virtual file",
                source->virtual_path,
                source->provenance.physical_path
            );
        }
        if (input.eof()) {
            break;
        }
        if (input.fail()) {
            throw_vfs_error(
                VfsErrorCode::io_failure,
                "a virtual-file read terminated unexpectedly",
                source->virtual_path,
                source->provenance.physical_path
            );
        }
    }

    source->size_bytes = static_cast<std::uintmax_t>(bytes.size());
    return BinaryVirtualFile{std::move(*source), std::move(bytes)};
}

TextVirtualFile VirtualFileSystem::read_text(
    const std::string_view virtual_path
) const {
    return read_text(virtual_path, snapshot_->max_read_bytes);
}

TextVirtualFile VirtualFileSystem::read_text(
    const std::string_view virtual_path,
    const std::uintmax_t max_bytes
) const {
    auto binary = read_binary(virtual_path, max_bytes);
    std::string text;
    text.resize(binary.bytes.size());
    if (!binary.bytes.empty()) {
        std::memcpy(text.data(), binary.bytes.data(), binary.bytes.size());
    }
    return TextVirtualFile{std::move(binary.source), std::move(text)};
}

}  // namespace pulseforge
