#include "pulseforge/editor_models.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>
#include <ostream>
#include <streambuf>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pulseforge {
namespace {

[[nodiscard]] EditorIoResult failure(
    const EditorIoStatus status,
    std::filesystem::path path,
    std::string message
) {
    return EditorIoResult{status, std::move(path), std::move(message)};
}

[[nodiscard]] bool is_dot_component(const std::filesystem::path& component) {
    return component == std::filesystem::path{"."}
        || component == std::filesystem::path{".."};
}

[[nodiscard]] bool is_valid_relative_path(
    const std::filesystem::path& path,
    const EditorStorageLimits& limits,
    std::string& error
) {
    if (path.empty() || path.is_absolute() || path.has_root_name()
        || path.has_root_directory()) {
        error = "editor path must be a non-empty relative path";
        return false;
    }
    if (path.native().size() > limits.maximum_relative_path_characters) {
        error = "editor path exceeds the configured character limit";
        return false;
    }
    for (const auto& component : path) {
        if (component.empty() || is_dot_component(component)) {
            error = "editor path contains an empty, dot, or parent component";
            return false;
        }
        const auto& native = component.native();
        if (std::find(native.begin(), native.end(), 0) != native.end()) {
            error = "editor path contains a NUL character";
            return false;
        }
    }
    if (path.filename().empty()) {
        error = "editor path must name a file";
        return false;
    }
    return true;
}

class LimitedStreamBuffer final : public std::streambuf {
public:
    LimitedStreamBuffer(std::streambuf* destination, const std::uint64_t limit)
        : destination_(destination), limit_(limit) {}

    [[nodiscard]] bool exceeded() const noexcept {
        return exceeded_;
    }

private:
    int_type overflow(const int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) {
            return traits_type::not_eof(value);
        }
        if (written_ >= limit_) {
            exceeded_ = true;
            return traits_type::eof();
        }
        const auto result = destination_->sputc(traits_type::to_char_type(value));
        if (traits_type::eq_int_type(result, traits_type::eof())) {
            return traits_type::eof();
        }
        ++written_;
        return result;
    }

    std::streamsize xsputn(
        const char_type* data,
        const std::streamsize count
    ) override {
        if (count <= 0) {
            return 0;
        }
        const auto requested = static_cast<std::uint64_t>(count);
        const auto available = written_ < limit_ ? limit_ - written_ : 0U;
        const auto allowed = std::min(requested, available);
        const auto actual = destination_->sputn(
            data,
            static_cast<std::streamsize>(allowed)
        );
        if (actual > 0) {
            written_ += static_cast<std::uint64_t>(actual);
        }
        if (static_cast<std::uint64_t>(actual) != requested) {
            exceeded_ = allowed != requested;
        }
        return actual;
    }

    int sync() override {
        return destination_->pubsync();
    }

    std::streambuf* destination_{};
    std::uint64_t limit_{};
    std::uint64_t written_{};
    bool exceeded_{};
};

class TemporaryFileGuard final {
public:
    explicit TemporaryFileGuard(std::filesystem::path path)
        : path_(std::move(path)) {}

    ~TemporaryFileGuard() {
        if (!committed_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    void commit() noexcept {
        committed_ = true;
    }

private:
    std::filesystem::path path_;
    bool committed_{};
};

[[nodiscard]] bool synchronize_file(
    const std::filesystem::path& path,
    std::string& error
) {
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (handle == INVALID_HANDLE_VALUE) {
        error = "cannot reopen temporary editor file for durable flush (Win32 "
            + std::to_string(GetLastError()) + ")";
        return false;
    }
    const BOOL flushed = FlushFileBuffers(handle);
    const DWORD flush_error = flushed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!flushed) {
        error = "cannot durably flush temporary editor file (Win32 "
            + std::to_string(flush_error) + ")";
        return false;
    }
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        error = "cannot reopen temporary editor file for durable flush";
        return false;
    }
    const int sync_result = ::fsync(descriptor);
    const int close_result = ::close(descriptor);
    if (sync_result != 0 || close_result != 0) {
        error = "cannot durably flush temporary editor file";
        return false;
    }
#endif
    return true;
}

[[nodiscard]] bool atomic_replace(
    const std::filesystem::path& temporary,
    const std::filesystem::path& target,
    std::string& error
) {
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(target.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (!ReplaceFileW(
                target.c_str(),
                temporary.c_str(),
                nullptr,
                REPLACEFILE_WRITE_THROUGH,
                nullptr,
                nullptr
            )) {
            error = "cannot atomically replace editor file (Win32 "
                + std::to_string(GetLastError()) + ")";
            return false;
        }
    } else {
        const DWORD attribute_error = GetLastError();
        if (attribute_error != ERROR_FILE_NOT_FOUND
            && attribute_error != ERROR_PATH_NOT_FOUND) {
            error = "cannot inspect editor destination (Win32 "
                + std::to_string(attribute_error) + ")";
            return false;
        }
        if (!MoveFileExW(
                temporary.c_str(),
                target.c_str(),
                MOVEFILE_WRITE_THROUGH
            )) {
            error = "cannot atomically commit editor file (Win32 "
                + std::to_string(GetLastError()) + ")";
            return false;
        }
    }
#else
    std::error_code rename_error;
    std::filesystem::rename(temporary, target, rename_error);
    if (rename_error) {
        error = "cannot atomically commit editor file: "
            + rename_error.message();
        return false;
    }
    const int directory = ::open(target.parent_path().c_str(), O_RDONLY);
    if (directory >= 0) {
        (void)::fsync(directory);
        (void)::close(directory);
    }
#endif
    return true;
}

[[nodiscard]] std::filesystem::path reserve_temporary_path(
    const std::filesystem::path& target,
    std::ofstream& stream,
    std::string& error
) {
    static std::atomic<std::uint64_t> sequence{};
    const auto clock_value = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    for (std::uint32_t attempt = 0U; attempt < 256U; ++attempt) {
        const auto nonce = sequence.fetch_add(1U, std::memory_order_relaxed)
            ^ clock_value ^ static_cast<std::uint64_t>(attempt);
        auto temporary = target;
        temporary += ".pf-tmp-" + std::to_string(nonce);

        std::error_code exists_error;
        const bool already_exists = std::filesystem::exists(
            temporary,
            exists_error
        );
        if (exists_error || already_exists) {
            continue;
        }
        stream.open(temporary, std::ios::binary | std::ios::out | std::ios::trunc);
        if (stream.is_open()) {
            return temporary;
        }
        stream.clear();
    }
    error = "cannot reserve a sibling temporary editor file";
    return {};
}

}  // namespace

struct EditorStorage::Resolution {
    std::filesystem::path path;
    EditorIoResult error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.status == EditorIoStatus::ok;
    }
};

EditorStorage::EditorStorage(
    std::filesystem::path root,
    const EditorStorageLimits limits
) : limits_(limits) {
    if (limits_.maximum_read_bytes == 0U
        || limits_.maximum_write_bytes == 0U
        || limits_.maximum_relative_path_characters == 0U) {
        initialization_error_ = "editor storage limits must be greater than zero";
        return;
    }
    if (root.empty()) {
        initialization_error_ = "editor storage root is empty";
        return;
    }

    std::error_code filesystem_error;
    auto absolute = std::filesystem::absolute(root, filesystem_error);
    if (filesystem_error) {
        initialization_error_ = "cannot make editor root absolute: "
            + filesystem_error.message();
        return;
    }
    std::filesystem::create_directories(absolute, filesystem_error);
    if (filesystem_error) {
        initialization_error_ = "cannot create editor root: "
            + filesystem_error.message();
        return;
    }
    root_ = std::filesystem::canonical(absolute, filesystem_error);
    if (filesystem_error || root_.empty()) {
        initialization_error_ = "cannot canonicalize editor root";
        root_.clear();
        return;
    }
    if (!std::filesystem::is_directory(root_, filesystem_error)
        || filesystem_error) {
        initialization_error_ = "editor storage root is not a directory";
        root_.clear();
    }
}

bool EditorStorage::ready() const noexcept {
    return initialization_error_.empty() && !root_.empty();
}

const std::filesystem::path& EditorStorage::root() const noexcept {
    return root_;
}

const std::string& EditorStorage::initialization_error() const noexcept {
    return initialization_error_;
}

EditorStorage::Resolution EditorStorage::resolve(
    const std::filesystem::path& relative_path,
    const bool create_parents
) const {
    if (!ready()) {
        return {{}, failure(
            EditorIoStatus::invalid_root,
            root_,
            initialization_error_.empty()
                ? "editor storage is not initialized"
                : initialization_error_
        )};
    }

    std::string path_error;
    if (!is_valid_relative_path(relative_path, limits_, path_error)) {
        return {{}, failure(
            EditorIoStatus::unsafe_path,
            relative_path,
            std::move(path_error)
        )};
    }

    auto current = root_;
    for (const auto& component : relative_path.parent_path()) {
        current /= component;
        std::error_code status_error;
        const auto status = std::filesystem::symlink_status(current, status_error);
        if (status_error
            && status_error != std::errc::no_such_file_or_directory) {
            return {{}, failure(
                EditorIoStatus::io_error,
                current,
                "cannot inspect editor path component: "
                    + status_error.message()
            )};
        }
        if (status.type() == std::filesystem::file_type::symlink) {
            return {{}, failure(
                EditorIoStatus::unsafe_path,
                current,
                "editor path crosses a symbolic link"
            )};
        }
        if (status.type() == std::filesystem::file_type::not_found) {
            if (!create_parents) {
                return {{}, failure(
                    EditorIoStatus::not_found,
                    current,
                    "editor path parent does not exist"
                )};
            }
            std::error_code create_error;
            if (!std::filesystem::create_directory(current, create_error)
                || create_error) {
                return {{}, failure(
                    EditorIoStatus::io_error,
                    current,
                    "cannot create editor path parent: "
                        + create_error.message()
                )};
            }
        } else if (status.type() != std::filesystem::file_type::directory) {
            return {{}, failure(
                EditorIoStatus::wrong_file_type,
                current,
                "editor path parent is not a directory"
            )};
        }
    }

    const auto target = (root_ / relative_path).lexically_normal();
    std::error_code target_error;
    const auto target_status = std::filesystem::symlink_status(
        target,
        target_error
    );
    if (target_error
        && target_error != std::errc::no_such_file_or_directory) {
        return {{}, failure(
            EditorIoStatus::io_error,
            target,
            "cannot inspect editor target: " + target_error.message()
        )};
    }
    if (target_status.type() == std::filesystem::file_type::symlink) {
        return {{}, failure(
            EditorIoStatus::unsafe_path,
            target,
            "editor target cannot be a symbolic link"
        )};
    }
    if (target_status.type() != std::filesystem::file_type::not_found
        && target_status.type() != std::filesystem::file_type::regular) {
        return {{}, failure(
            EditorIoStatus::wrong_file_type,
            target,
            "editor target is not a regular file"
        )};
    }
    return {target, EditorIoResult{EditorIoStatus::ok, target, {}}};
}

EditorIoResult EditorStorage::read_text(
    const std::filesystem::path& relative_path,
    std::string& output
) const {
    output.clear();
    const auto resolved = resolve(relative_path, false);
    if (!resolved) {
        return resolved.error;
    }

    std::error_code size_error;
    const auto size = std::filesystem::file_size(resolved.path, size_error);
    if (size_error) {
        return failure(
            EditorIoStatus::not_found,
            resolved.path,
            "cannot read editor file size: " + size_error.message()
        );
    }
    if (size > limits_.maximum_read_bytes
        || size > static_cast<std::uintmax_t>(
            std::numeric_limits<std::size_t>::max()
        )) {
        return failure(
            EditorIoStatus::too_large,
            resolved.path,
            "editor file exceeds the configured read limit"
        );
    }

    std::ifstream input(resolved.path, std::ios::binary);
    if (!input) {
        return failure(
            EditorIoStatus::io_error,
            resolved.path,
            "cannot open editor file for reading"
        );
    }
    output.resize(static_cast<std::size_t>(size));
    if (!output.empty()) {
        input.read(output.data(), static_cast<std::streamsize>(output.size()));
    }
    if (!input || static_cast<std::size_t>(input.gcount()) != output.size()) {
        output.clear();
        return failure(
            EditorIoStatus::io_error,
            resolved.path,
            "editor file changed or failed while reading"
        );
    }
    return EditorIoResult{EditorIoStatus::ok, resolved.path, {}};
}

EditorIoResult EditorStorage::write_atomic(
    const std::filesystem::path& relative_path,
    const std::string_view content
) const {
    return write_atomic(
        relative_path,
        [content](std::ostream& output, std::string&) {
            output.write(content.data(), static_cast<std::streamsize>(content.size()));
            return static_cast<bool>(output);
        }
    );
}

EditorIoResult EditorStorage::write_atomic(
    const std::filesystem::path& relative_path,
    const EditorStreamWriter& writer
) const {
    if (!writer) {
        return failure(
            EditorIoStatus::serialization_error,
            relative_path,
            "editor writer callback is empty"
        );
    }
    const auto resolved = resolve(relative_path, true);
    if (!resolved) {
        return resolved.error;
    }

    std::ofstream temporary_stream;
    std::string write_error;
    const auto temporary = reserve_temporary_path(
        resolved.path,
        temporary_stream,
        write_error
    );
    if (temporary.empty()) {
        return failure(
            EditorIoStatus::io_error,
            resolved.path,
            std::move(write_error)
        );
    }
    TemporaryFileGuard guard(temporary);

    LimitedStreamBuffer limited_buffer(
        temporary_stream.rdbuf(),
        limits_.maximum_write_bytes
    );
    std::ostream limited_output(&limited_buffer);
    bool serialized = false;
    try {
        serialized = writer(limited_output, write_error);
    } catch (const std::exception& exception) {
        write_error = "editor writer threw an exception: "
            + std::string(exception.what());
    } catch (...) {
        write_error = "editor writer threw an unknown exception";
    }
    limited_output.flush();
    temporary_stream.flush();

    if (limited_buffer.exceeded()) {
        temporary_stream.close();
        return failure(
            EditorIoStatus::too_large,
            resolved.path,
            "serialized editor file exceeds the configured write limit"
        );
    }
    if (!serialized || !limited_output || !temporary_stream) {
        temporary_stream.close();
        return failure(
            EditorIoStatus::serialization_error,
            resolved.path,
            write_error.empty() ? "cannot serialize editor file" : write_error
        );
    }
    temporary_stream.close();
    if (temporary_stream.fail()) {
        return failure(
            EditorIoStatus::io_error,
            resolved.path,
            "cannot close temporary editor file"
        );
    }
    if (!synchronize_file(temporary, write_error)) {
        return failure(
            EditorIoStatus::io_error,
            resolved.path,
            std::move(write_error)
        );
    }
    if (!atomic_replace(temporary, resolved.path, write_error)) {
        return failure(
            EditorIoStatus::io_error,
            resolved.path,
            std::move(write_error)
        );
    }
    guard.commit();
    return EditorIoResult{EditorIoStatus::ok, resolved.path, {}};
}

std::string_view to_string(const EditorIoStatus status) noexcept {
    switch (status) {
    case EditorIoStatus::ok:
        return "ok";
    case EditorIoStatus::invalid_root:
        return "invalid_root";
    case EditorIoStatus::unsafe_path:
        return "unsafe_path";
    case EditorIoStatus::not_found:
        return "not_found";
    case EditorIoStatus::wrong_file_type:
        return "wrong_file_type";
    case EditorIoStatus::too_large:
        return "too_large";
    case EditorIoStatus::io_error:
        return "io_error";
    case EditorIoStatus::serialization_error:
        return "serialization_error";
    case EditorIoStatus::validation_error:
        return "validation_error";
    }
    return "unknown";
}

}  // namespace pulseforge
