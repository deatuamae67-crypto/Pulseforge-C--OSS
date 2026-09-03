#include "pulseforge/mod_installer.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace pulseforge {
namespace {

constexpr std::size_t extraction_buffer_bytes = 256U * 1024U;

[[nodiscard]] char lower_ascii(const char value) noexcept {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value - 'A' + 'a')
        : value;
}

[[nodiscard]] std::string lower_ascii(std::string value) {
    std::ranges::transform(value, value.begin(), [](const char character) {
        return lower_ascii(character);
    });
    return value;
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return {encoded.begin(), encoded.end()};
}

[[nodiscard]] bool valid_utf8(const std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        std::size_t continuation = 0;
        std::uint32_t codepoint = 0;
        if (first <= 0x7FU) {
            ++index;
            continue;
        }
        if ((first & 0xE0U) == 0xC0U) {
            continuation = 1;
            codepoint = first & 0x1FU;
        } else if ((first & 0xF0U) == 0xE0U) {
            continuation = 2;
            codepoint = first & 0x0FU;
        } else if ((first & 0xF8U) == 0xF0U) {
            continuation = 3;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (continuation > text.size() - index - 1U) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation; ++offset) {
            const auto next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (next & 0x3FU);
        }
        const bool overlong = (continuation == 1U && codepoint < 0x80U)
            || (continuation == 2U && codepoint < 0x800U)
            || (continuation == 3U && codepoint < 0x10000U);
        if (overlong || codepoint > 0x10FFFFU
            || (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
            return false;
        }
        index += continuation + 1U;
    }
    return true;
}

[[nodiscard]] bool reserved_windows_component(
    const std::string_view component
) noexcept {
    const auto dot = component.find('.');
    auto basename = component.substr(0, dot);
    while (!basename.empty()
        && (basename.back() == ' ' || basename.back() == '.')) {
        basename.remove_suffix(1U);
    }
    if (basename.empty()) {
        return false;
    }

    std::string folded(basename);
    std::ranges::transform(folded, folded.begin(), [](const char character) {
        return lower_ascii(character);
    });
    constexpr std::array<std::string_view, 7> fixed_devices{
        "con", "prn", "aux", "nul", "clock$", "conin$", "conout$",
    };
    if (std::ranges::find(fixed_devices, folded) != fixed_devices.end()) {
        return true;
    }
    if (folded.size() == 4U
        && (folded.starts_with("com") || folded.starts_with("lpt"))
        && folded.back() >= '1' && folded.back() <= '9') {
        return true;
    }

    // Win32 also reserves COM/LPT followed by superscript 1, 2 or 3,
    // including when the device token appears before an extension.
    constexpr std::array<std::string_view, 6> superscript_devices{
        "com\xC2\xB9", "com\xC2\xB2", "com\xC2\xB3",
        "lpt\xC2\xB9", "lpt\xC2\xB2", "lpt\xC2\xB3",
    };
    return std::ranges::find(
        superscript_devices,
        folded
    ) != superscript_devices.end();
}

[[nodiscard]] std::optional<std::filesystem::path> safe_relative_path(
    std::string raw,
    const std::size_t maximum_bytes
) {
    if (raw.empty() || raw.size() > maximum_bytes || !valid_utf8(raw)) {
        return std::nullopt;
    }
    std::ranges::replace(raw, '\\', '/');
    if (raw.starts_with('/') || raw.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    while (raw.ends_with('/')) {
        raw.pop_back();
    }
    if (raw.empty()) {
        return std::nullopt;
    }

    std::filesystem::path result;
    std::size_t begin = 0;
    while (begin <= raw.size()) {
        const auto end = raw.find('/', begin);
        const auto component = std::string_view(raw).substr(
            begin,
            end == std::string::npos ? raw.size() - begin : end - begin
        );
        if (component.empty() || component == "." || component == ".."
            || component.back() == ' ' || component.back() == '.'
            || reserved_windows_component(component)) {
            return std::nullopt;
        }
        for (const unsigned char character : component) {
            if (character < 32U || character == 127U
                || character == '<' || character == '>'
                || character == ':' || character == '"'
                || character == '|' || character == '?'
                || character == '*') {
                return std::nullopt;
            }
        }
        std::u8string encoded_component;
        encoded_component.reserve(component.size());
        for (const unsigned char character : component) {
            encoded_component.push_back(static_cast<char8_t>(character));
        }
        result /= std::filesystem::path(encoded_component);
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1U;
    }
    return result.empty() || result.is_absolute() || result.has_root_path()
        ? std::nullopt
        : std::optional<std::filesystem::path>(result.lexically_normal());
}

#if defined(_WIN32)
using DestinationAliasKey = std::wstring;
#else
using DestinationAliasKey = std::string;
#endif

[[nodiscard]] std::optional<DestinationAliasKey> destination_alias_key(
    const std::filesystem::path& path
) {
#if defined(_WIN32)
    const auto native = path.generic_wstring();
    if (native.empty()
        || native.size() > static_cast<std::size_t>(
            std::numeric_limits<int>::max()
        )) {
        return std::nullopt;
    }
    const auto source_size = static_cast<int>(native.size());
    const int required = LCMapStringEx(
        LOCALE_NAME_INVARIANT,
        LCMAP_UPPERCASE,
        native.data(),
        source_size,
        nullptr,
        0,
        nullptr,
        nullptr,
        0
    );
    if (required <= 0) {
        return std::nullopt;
    }
    DestinationAliasKey result(static_cast<std::size_t>(required), L'\0');
    const int written = LCMapStringEx(
        LOCALE_NAME_INVARIANT,
        LCMAP_UPPERCASE,
        native.data(),
        source_size,
        result.data(),
        required,
        nullptr,
        nullptr,
        0
    );
    if (written != required) {
        return std::nullopt;
    }
    return result;
#else
    const auto encoded = path.generic_u8string();
    DestinationAliasKey result(encoded.begin(), encoded.end());
    std::ranges::transform(result, result.begin(), [](const char character) {
        return lower_ascii(character);
    });
    return result.empty()
        ? std::nullopt
        : std::optional<DestinationAliasKey>(std::move(result));
#endif
}

template <typename Member>
[[nodiscard]] bool validate_destination_aliases(
    const std::vector<Member>& members,
    const std::string_view subject,
    std::string& error
) {
    std::vector<DestinationAliasKey> keys;
    keys.reserve(members.size());
    std::unordered_set<DestinationAliasKey> files;
    files.reserve(members.size());
    for (const auto& member : members) {
        auto key = destination_alias_key(member.relative);
        if (!key.has_value()) {
            error = std::string(subject)
                + " contains a destination path that cannot be normalized";
            return false;
        }
        if (!files.insert(*key).second) {
            error = std::string(subject)
                + " contains colliding Windows destination paths";
            return false;
        }
        keys.push_back(std::move(*key));
    }

    const auto separator = static_cast<DestinationAliasKey::value_type>('/');
    for (const auto& key : keys) {
        auto position = key.find(separator);
        while (position != DestinationAliasKey::npos) {
            if (files.contains(key.substr(0U, position))) {
                error = std::string(subject)
                    + " contains a file/directory destination collision";
                return false;
            }
            position = key.find(separator, position + 1U);
        }
    }
    return true;
}

[[nodiscard]] bool blocked_extension(const std::filesystem::path& path) {
    const auto extension = lower_ascii(path_utf8(path.extension()));
    constexpr std::array<std::string_view, 18> blocked{
        ".bat", ".cmd", ".com", ".cpl", ".dll", ".dylib",
        ".exe", ".hta", ".jar", ".js", ".lnk", ".msi",
        ".pif", ".ps1", ".scr", ".so", ".vbs", ".wsf",
    };
    return std::ranges::find(blocked, extension) != blocked.end();
}

[[nodiscard]] std::string safe_mod_name(std::string value) {
    if (value.size() > 96U) {
        value.resize(96U);
    }
    std::string result;
    result.reserve(value.size());
    bool separator = false;
    for (const unsigned char character : value) {
        if ((character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9')) {
            result.push_back(static_cast<char>(character));
            separator = false;
        } else if (character >= 'A' && character <= 'Z') {
            result.push_back(static_cast<char>(character - 'A' + 'a'));
            separator = false;
        } else if (!separator && !result.empty()) {
            result.push_back('-');
            separator = true;
        }
    }
    while (!result.empty() && result.back() == '-') {
        result.pop_back();
    }
    if (result.empty() || reserved_windows_component(result)) {
        return "imported-mod";
    }
    return result;
}

[[nodiscard]] bool add_bounded(
    std::uint64_t& total,
    const std::uint64_t value,
    const std::uint64_t limit
) noexcept {
    if (value > limit || total > limit - value) {
        return false;
    }
    total += value;
    return true;
}

[[nodiscard]] bool is_zip_symlink(
    const mz_zip_archive_file_stat& stat
) noexcept {
    const auto unix_mode = static_cast<std::uint32_t>(stat.m_external_attr >> 16U);
    return (unix_mode & 0170000U) == 0120000U;
}

class StagingGuard final {
public:
    explicit StagingGuard(std::filesystem::path path)
        : path_(std::move(path)) {}

    ~StagingGuard() {
        if (armed_) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    StagingGuard(const StagingGuard&) = delete;
    StagingGuard& operator=(const StagingGuard&) = delete;

    void release() noexcept { armed_ = false; }

private:
    std::filesystem::path path_;
    bool armed_{true};
};

[[nodiscard]] std::filesystem::path unique_staging_path(
    const std::filesystem::path& root,
    const std::string_view name
) {
    static std::atomic<std::uint64_t> sequence{};
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    const auto id = sequence.fetch_add(1U, std::memory_order_relaxed);
    return root / ("." + std::string(name) + ".install-"
        + std::to_string(ticks) + "-" + std::to_string(id));
}

struct ZipMember {
    mz_uint index{};
    std::filesystem::path relative;
    std::uint64_t size{};
};

struct ZipArchiveGuard {
    mz_zip_archive archive{};
    std::FILE* file{};
    bool open{};

    ~ZipArchiveGuard() {
        if (open) {
            mz_zip_reader_end(&archive);
        }
        if (file != nullptr) {
            std::fclose(file);
        }
    }
};

struct ArchiveMember {
    std::uint64_t ordinal{};
    std::filesystem::path source_relative;
    std::filesystem::path relative;
    std::uint64_t size{};
};

struct DirectoryMember {
    std::filesystem::path source;
    std::filesystem::path relative;
    std::uint64_t size{};
};

class ArchiveReadGuard final {
public:
    ArchiveReadGuard() : value_(archive_read_new()) {}
    ~ArchiveReadGuard() {
        if (value_ != nullptr) {
            archive_read_free(value_);
        }
    }

    ArchiveReadGuard(const ArchiveReadGuard&) = delete;
    ArchiveReadGuard& operator=(const ArchiveReadGuard&) = delete;

    [[nodiscard]] archive* get() const noexcept { return value_; }

private:
    archive* value_{};
};

[[nodiscard]] std::string archive_diagnostic(archive* const reader) {
    const char* const message = archive_error_string(reader);
    return message == nullptr ? "unknown archive error" : std::string(message);
}

[[nodiscard]] bool open_multi_archive(
    ArchiveReadGuard& reader,
    const std::filesystem::path& source,
    std::string& error
) {
    if (reader.get() == nullptr) {
        error = "cannot allocate the archive reader";
        return false;
    }
    auto* const handle = reader.get();
    const auto extension = lower_ascii(path_utf8(source.extension()));
    int support = ARCHIVE_FATAL;
    if (extension == ".7z") {
        support = archive_read_support_format_7zip(handle);
    } else if (extension == ".rar") {
        support = archive_read_support_format_rar(handle);
        if (support == ARCHIVE_OK) {
            support = archive_read_support_format_rar5(handle);
        }
    } else if (extension == ".tar") {
        support = archive_read_support_format_tar(handle);
    }
    if (support != ARCHIVE_OK
        || archive_read_support_filter_none(handle) != ARCHIVE_OK) {
        error = "this archive format is unavailable in the current build";
        return false;
    }
#if defined(_WIN32)
    const int opened = archive_read_open_filename_w(
        handle,
        source.c_str(),
        extraction_buffer_bytes
    );
#else
    const int opened = archive_read_open_filename(
        handle,
        source.c_str(),
        extraction_buffer_bytes
    );
#endif
    if (opened != ARCHIVE_OK) {
        error = "cannot open mod archive: " + archive_diagnostic(handle);
        return false;
    }
    return true;
}

[[nodiscard]] bool inspect_multi_archive(
    const std::filesystem::path& source,
    const ModInstallOptions& options,
    std::vector<ArchiveMember>& members,
    std::uint64_t& total_bytes,
    std::string& error
) {
    ArchiveReadGuard reader;
    if (!open_multi_archive(reader, source, error)) {
        return false;
    }
    std::optional<std::filesystem::path> common_root;
    bool strip_common_root = true;
    std::uint64_t ordinal = 0U;
    archive_entry* entry = nullptr;
    while (true) {
        const int status = archive_read_next_header(reader.get(), &entry);
        if (status == ARCHIVE_EOF) {
            break;
        }
        if (status != ARCHIVE_OK || entry == nullptr) {
            error = "cannot inspect mod archive: "
                + archive_diagnostic(reader.get());
            return false;
        }
        ++ordinal;
        if (ordinal > options.limits.max_files) {
            error = "mod archive contains too many entries";
            return false;
        }
        if (archive_entry_hardlink(entry) != nullptr
            || archive_entry_symlink(entry) != nullptr) {
            error = "mod archive contains a link";
            return false;
        }
        const auto file_type = archive_entry_filetype(entry);
        const bool directory = file_type == AE_IFDIR;
        if (!directory && file_type != AE_IFREG) {
            error = "mod archive contains an unsupported file type";
            return false;
        }
        const char* raw_name = archive_entry_pathname_utf8(entry);
        if (raw_name == nullptr) {
            raw_name = archive_entry_pathname(entry);
        }
        if (raw_name == nullptr) {
            error = "mod archive contains an entry without a path";
            return false;
        }
        auto relative = safe_relative_path(
            raw_name,
            options.limits.max_relative_path_bytes
        );
        if (!relative.has_value()) {
            error = "mod archive contains an unsafe path";
            return false;
        }
        auto iterator = relative->begin();
        const auto first = iterator == relative->end()
            ? std::filesystem::path{}
            : *iterator;
        ++iterator;
        if (!common_root.has_value()) {
            common_root = first;
        } else if (*common_root != first) {
            strip_common_root = false;
        }
        if (iterator == relative->end() && !directory) {
            strip_common_root = false;
        }
        if (directory) {
            if (archive_read_data_skip(reader.get()) != ARCHIVE_OK) {
                error = "cannot skip a mod archive directory";
                return false;
            }
            continue;
        }
        if (archive_entry_is_encrypted(entry) == 1) {
            error = "encrypted mod archives are not supported";
            return false;
        }
        if (!archive_entry_size_is_set(entry)
            || archive_entry_size(entry) < 0) {
            error = "mod archive contains a file without a bounded size";
            return false;
        }
        if (options.reject_executable_files && blocked_extension(*relative)) {
            error = "mod archive contains a blocked executable file";
            return false;
        }
        const auto size = static_cast<std::uint64_t>(archive_entry_size(entry));
        if (size > options.limits.max_single_file_bytes
            || !add_bounded(
                total_bytes,
                size,
                options.limits.max_total_uncompressed_bytes
            )) {
            error = "mod archive exceeds its uncompressed size budget";
            return false;
        }
        members.push_back({ordinal, *relative, std::move(*relative), size});
        if (archive_read_data_skip(reader.get()) != ARCHIVE_OK) {
            error = "cannot inspect the complete mod archive";
            return false;
        }
    }
    if (members.empty()) {
        error = "mod archive contains no files";
        return false;
    }
    std::error_code filesystem_error;
    const auto archive_bytes = std::filesystem::file_size(
        source,
        filesystem_error
    );
    if (filesystem_error || archive_bytes == 0U) {
        error = "cannot determine the compressed archive size";
        return false;
    }
    if (total_bytes / archive_bytes > options.limits.max_compression_ratio) {
        error = "mod archive exceeds its aggregate compression-ratio budget";
        return false;
    }
    if (strip_common_root && common_root.has_value()) {
        for (auto& member : members) {
            std::filesystem::path stripped;
            auto iterator = member.relative.begin();
            if (iterator != member.relative.end()) {
                ++iterator;
            }
            for (; iterator != member.relative.end(); ++iterator) {
                stripped /= *iterator;
            }
            if (stripped.empty()) {
                error = "mod archive has an invalid common root";
                return false;
            }
            member.relative = std::move(stripped);
        }
    }
    return validate_destination_aliases(members, "mod archive", error);
}

[[nodiscard]] bool extract_multi_archive_member(
    archive* const reader,
    const ArchiveMember& member,
    const std::filesystem::path& destination,
    std::string& error
) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(
        destination.parent_path(),
        filesystem_error
    );
    if (filesystem_error) {
        error = "cannot create a mod directory";
        return false;
    }
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        error = "cannot create an extracted mod file";
        return false;
    }
    std::uint64_t written = 0U;
    while (true) {
        const void* block = nullptr;
        std::size_t block_size = 0U;
        la_int64_t block_offset = 0;
        const int status = archive_read_data_block(
            reader,
            &block,
            &block_size,
            &block_offset
        );
        if (status == ARCHIVE_EOF) {
            break;
        }
        if (status != ARCHIVE_OK || block == nullptr || block_offset < 0
            || written > member.size
            || static_cast<std::uint64_t>(block_offset) != written
            || block_size > member.size - written) {
            error = "cannot safely extract a complete archive member";
            return false;
        }
        output.write(
            static_cast<const char*>(block),
            static_cast<std::streamsize>(block_size)
        );
        if (!output) {
            error = "cannot write an extracted mod file";
            return false;
        }
        written += static_cast<std::uint64_t>(block_size);
    }
    output.flush();
    if (!output || written != member.size) {
        error = "archive member size changed during extraction";
        return false;
    }
    return true;
}

[[nodiscard]] bool extract_multi_archive(
    const std::filesystem::path& source,
    const std::vector<ArchiveMember>& members,
    const std::filesystem::path& staging,
    const ModInstallOptions& options,
    std::string& error
) {
    ArchiveReadGuard reader;
    if (!open_multi_archive(reader, source, error)) {
        return false;
    }
    std::size_t member_index = 0U;
    std::uint64_t ordinal = 0U;
    archive_entry* entry = nullptr;
    while (member_index < members.size()) {
        const int status = archive_read_next_header(reader.get(), &entry);
        if (status != ARCHIVE_OK || entry == nullptr) {
            error = "mod archive changed while it was being installed";
            return false;
        }
        ++ordinal;
        if (ordinal != members[member_index].ordinal) {
            if (archive_read_data_skip(reader.get()) != ARCHIVE_OK) {
                error = "cannot skip an archive entry during extraction";
                return false;
            }
            continue;
        }
        const char* raw_name = archive_entry_pathname_utf8(entry);
        if (raw_name == nullptr) {
            raw_name = archive_entry_pathname(entry);
        }
        const auto relative = raw_name == nullptr
            ? std::optional<std::filesystem::path>{}
            : safe_relative_path(
                raw_name,
                options.limits.max_relative_path_bytes
            );
        if (!relative.has_value()
            || *relative != members[member_index].source_relative
            || archive_entry_filetype(entry) != AE_IFREG
            || archive_entry_hardlink(entry) != nullptr
            || archive_entry_symlink(entry) != nullptr
            || !archive_entry_size_is_set(entry)
            || archive_entry_size(entry) < 0
            || static_cast<std::uint64_t>(archive_entry_size(entry))
                != members[member_index].size
            || archive_entry_is_encrypted(entry) == 1) {
            error = "mod archive metadata changed during extraction";
            return false;
        }
        if (!extract_multi_archive_member(
                reader.get(),
                members[member_index],
                staging / members[member_index].relative,
                error
            )) {
            return false;
        }
        ++member_index;
    }
    return true;
}

[[nodiscard]] bool inspect_zip(
    const std::filesystem::path& source,
    const ModInstallOptions& options,
    std::vector<ZipMember>& members,
    std::uint64_t& total_bytes,
    std::string& error,
    ZipArchiveGuard& zip
) {
#if defined(_WIN32)
    if (_wfopen_s(&zip.file, source.c_str(), L"rb") != 0
        || zip.file == nullptr) {
        error = "cannot open ZIP archive";
        return false;
    }
#else
    zip.file = std::fopen(source.c_str(), "rb");
    if (zip.file == nullptr) {
        error = "cannot open ZIP archive";
        return false;
    }
#endif
    // cfile avoids narrow Win32 paths; the guard closes the stream after
    // mz_zip_reader_end because miniz deliberately does not own it.
    if (!mz_zip_reader_init_cfile(&zip.archive, zip.file, 0U, 0U)) {
        error = "cannot open ZIP archive";
        return false;
    }
    zip.open = true;
    const auto count = mz_zip_reader_get_num_files(&zip.archive);
    if (static_cast<std::uint64_t>(count) > options.limits.max_files) {
        error = "mod archive contains too many files";
        return false;
    }

    members.reserve(count);
    std::optional<std::filesystem::path> common_root;
    bool strip_common_root = true;
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip.archive, index, &stat)) {
            error = "cannot inspect ZIP member";
            return false;
        }
        if (is_zip_symlink(stat)) {
            error = "mod archive contains a symbolic link";
            return false;
        }
        const auto filename_bytes = mz_zip_reader_get_filename(
            &zip.archive,
            index,
            nullptr,
            0U
        );
        if (filename_bytes <= 1U
            || static_cast<std::uint64_t>(filename_bytes - 1U)
                > options.limits.max_relative_path_bytes) {
            error = "mod archive contains an unsafe path";
            return false;
        }
        std::vector<char> filename(static_cast<std::size_t>(filename_bytes));
        if (mz_zip_reader_get_filename(
                &zip.archive,
                index,
                filename.data(),
                filename_bytes
            ) != filename_bytes) {
            error = "cannot inspect the complete ZIP member path";
            return false;
        }
        const std::string raw_name(
            filename.data(),
            static_cast<std::size_t>(filename_bytes - 1U)
        );
        const bool directory = mz_zip_reader_is_file_a_directory(
            &zip.archive,
            index
        ) != 0;
        auto relative = safe_relative_path(
            raw_name,
            options.limits.max_relative_path_bytes
        );
        if (!relative.has_value()) {
            if (directory && (raw_name == "/" || raw_name.empty())) {
                continue;
            }
            error = "mod archive contains an unsafe path";
            return false;
        }
        auto iterator = relative->begin();
        const auto first = iterator == relative->end()
            ? std::filesystem::path{}
            : *iterator;
        ++iterator;
        if (!common_root.has_value()) {
            common_root = first;
        } else if (*common_root != first) {
            strip_common_root = false;
        }
        if (iterator == relative->end() && !directory) {
            strip_common_root = false;
        }
        if (directory) {
            continue;
        }
        if (options.reject_executable_files && blocked_extension(*relative)) {
            error = "mod archive contains a blocked executable file";
            return false;
        }
        const auto size = static_cast<std::uint64_t>(stat.m_uncomp_size);
        if (size > options.limits.max_single_file_bytes
            || !add_bounded(
                total_bytes,
                size,
                options.limits.max_total_uncompressed_bytes
            )) {
            error = "mod archive exceeds its uncompressed size budget";
            return false;
        }
        const auto compressed = static_cast<std::uint64_t>(stat.m_comp_size);
        if (compressed == 0U) {
            if (size > 0U) {
                error = "mod archive contains an impossible compression ratio";
                return false;
            }
        } else if (size / compressed > options.limits.max_compression_ratio) {
            error = "mod archive exceeds its compression-ratio budget";
            return false;
        }
        members.push_back({index, std::move(*relative), size});
    }

    if (members.empty()) {
        error = "mod archive contains no files";
        return false;
    }
    if (strip_common_root && common_root.has_value()) {
        for (auto& member : members) {
            std::filesystem::path stripped;
            auto iterator = member.relative.begin();
            if (iterator != member.relative.end()) {
                ++iterator;
            }
            for (; iterator != member.relative.end(); ++iterator) {
                stripped /= *iterator;
            }
            if (stripped.empty()) {
                error = "mod archive has an invalid common root";
                return false;
            }
            member.relative = std::move(stripped);
        }
    }
    return validate_destination_aliases(members, "mod archive", error);
}

[[nodiscard]] bool extract_zip_member(
    ZipArchiveGuard& zip,
    const ZipMember& member,
    const std::filesystem::path& destination,
    std::string& error
) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(
        destination.parent_path(),
        filesystem_error
    );
    if (filesystem_error) {
        error = "cannot create a mod directory";
        return false;
    }
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        error = "cannot create an extracted mod file";
        return false;
    }
    auto* iterator = mz_zip_reader_extract_iter_new(
        &zip.archive,
        member.index,
        0
    );
    if (iterator == nullptr) {
        error = "cannot initialize ZIP member extraction";
        return false;
    }
    std::array<unsigned char, extraction_buffer_bytes> buffer{};
    std::uint64_t written = 0;
    bool success = true;
    while (written < member.size) {
        const auto maximum = static_cast<std::size_t>(std::min<std::uint64_t>(
            buffer.size(),
            member.size - written
        ));
        const auto count = mz_zip_reader_extract_iter_read(
            iterator,
            buffer.data(),
            maximum
        );
        if (count == 0U || count > maximum) {
            success = false;
            break;
        }
        output.write(
            reinterpret_cast<const char*>(buffer.data()),
            static_cast<std::streamsize>(count)
        );
        if (!output) {
            success = false;
            break;
        }
        written += static_cast<std::uint64_t>(count);
    }
    if (!mz_zip_reader_extract_iter_free(iterator)) {
        success = false;
    }
    output.flush();
    if (!output || written != member.size) {
        success = false;
    }
    if (!success) {
        error = "cannot extract a complete ZIP member";
    }
    return success;
}

[[nodiscard]] bool copy_directory_mod(
    const std::filesystem::path& source,
    const std::filesystem::path& staging,
    const ModInstallOptions& options,
    std::uint64_t& file_count,
    std::uint64_t& total_bytes,
    std::string& error
) {
    std::vector<DirectoryMember> members;
    std::error_code iterator_error;
    std::filesystem::recursive_directory_iterator iterator(
        source,
        std::filesystem::directory_options::skip_permission_denied,
        iterator_error
    );
    const std::filesystem::recursive_directory_iterator end;
    for (; !iterator_error && iterator != end; iterator.increment(iterator_error)) {
        std::error_code status_error;
        if (iterator->is_symlink(status_error) && !status_error) {
            error = "mod directory contains a symbolic link";
            return false;
        }
        if (status_error) {
            error = "cannot inspect a mod directory entry";
            return false;
        }
        const auto source_relative = iterator->path().lexically_relative(source);
        const auto generic = source_relative.generic_u8string();
        const std::string generic_text(generic.begin(), generic.end());
        auto relative = safe_relative_path(
            generic_text,
            options.limits.max_relative_path_bytes
        );
        if (!relative.has_value()) {
            error = "mod directory contains an unsafe path";
            return false;
        }
        if (iterator->is_directory(status_error) && !status_error) {
            continue;
        }
        if (!iterator->is_regular_file(status_error) || status_error) {
            error = "mod directory contains an unsupported file type";
            return false;
        }
        if (options.reject_executable_files && blocked_extension(*relative)) {
            error = "mod directory contains a blocked executable file";
            return false;
        }
        const auto size = iterator->file_size(status_error);
        if (status_error || size > options.limits.max_single_file_bytes
            || file_count >= options.limits.max_files
            || !add_bounded(
                total_bytes,
                size,
                options.limits.max_total_uncompressed_bytes
            )) {
            error = "mod directory exceeds an installation budget";
            return false;
        }
        ++file_count;
        members.push_back({iterator->path(), std::move(*relative), size});
    }
    if (iterator_error) {
        error = "cannot enumerate the complete mod directory";
        return false;
    }
    if (file_count == 0U) {
        error = "mod directory contains no files";
        return false;
    }
    if (!validate_destination_aliases(members, "mod directory", error)) {
        return false;
    }

    for (const auto& member : members) {
        std::error_code status_error;
        const auto destination = staging / member.relative;
        std::filesystem::create_directories(
            destination.parent_path(),
            status_error
        );
        if (status_error || !std::filesystem::copy_file(
                member.source,
                destination,
                std::filesystem::copy_options::none,
                status_error
            ) || status_error) {
            error = "cannot copy a mod file";
            return false;
        }
        const auto copied_size = std::filesystem::file_size(
            destination,
            status_error
        );
        if (status_error || copied_size != member.size) {
            error = "mod source changed while it was being installed";
            return false;
        }
    }
    return true;
}

}  // namespace

bool is_supported_mod_archive(const std::filesystem::path& source) noexcept {
    const auto extension = lower_ascii(path_utf8(source.extension()));
    return extension == ".zip" || extension == ".7z"
        || extension == ".rar" || extension == ".tar";
}

ModInstallResult install_mod(
    const std::filesystem::path& source,
    const std::filesystem::path& mods_root,
    const ModInstallOptions& options
) {
    ModInstallResult result;
    std::error_code error;
    const auto absolute_source = std::filesystem::absolute(source, error);
    if (error || std::filesystem::is_symlink(absolute_source, error) || error
        || (!std::filesystem::is_regular_file(absolute_source, error)
        && !std::filesystem::is_directory(absolute_source, error)) || error) {
        result.error = "mod source is not an accessible file or directory";
        return result;
    }

    const bool archive = std::filesystem::is_regular_file(absolute_source, error);
    if (error || (archive && !is_supported_mod_archive(absolute_source))) {
        result.error =
            "only unpacked directories and ZIP, 7z, RAR or TAR archives are supported";
        return result;
    }
    const auto requested_name = options.destination_name.value_or(
        archive
            ? path_utf8(absolute_source.stem())
            : path_utf8(absolute_source.filename())
    );
    result.mod_id = safe_mod_name(requested_name);

    const auto absolute_root = std::filesystem::absolute(mods_root, error);
    if (error) {
        result.error = "cannot resolve the mods root";
        return result;
    }
    std::filesystem::create_directories(absolute_root, error);
    if (error || !std::filesystem::is_directory(absolute_root, error) || error
        || std::filesystem::is_symlink(absolute_root, error) || error) {
        result.error = "mods root is not a safe directory";
        return result;
    }
    const auto destination = absolute_root / result.mod_id;
    if (std::filesystem::exists(destination, error) || error) {
        result.error = "a mod with this destination name already exists";
        return result;
    }
    const auto staging = unique_staging_path(absolute_root, result.mod_id);
    if (std::filesystem::exists(staging, error) || error) {
        result.error = "cannot allocate a unique installation staging path";
        return result;
    }
    const bool staging_created = std::filesystem::create_directory(staging, error);
    if (error || !staging_created) {
        result.error = "cannot create the installation staging directory";
        return result;
    }
    if (!std::filesystem::is_directory(staging, error) || error
        || std::filesystem::is_symlink(staging, error) || error) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(staging, cleanup_error);
        result.error = "installation staging is not a safe owned directory";
        return result;
    }
    StagingGuard staging_guard(staging);

    if (archive && lower_ascii(path_utf8(absolute_source.extension())) == ".zip") {
        ZipArchiveGuard zip;
        std::vector<ZipMember> members;
        if (!inspect_zip(
                absolute_source,
                options,
                members,
                result.installed_bytes,
                result.error,
                zip
            )) {
            return result;
        }
        for (const auto& member : members) {
            if (!extract_zip_member(
                    zip,
                    member,
                    staging / member.relative,
                    result.error
                )) {
                return result;
            }
            ++result.installed_files;
        }
    } else if (archive) {
        std::vector<ArchiveMember> members;
        if (!inspect_multi_archive(
                absolute_source,
                options,
                members,
                result.installed_bytes,
                result.error
            ) || !extract_multi_archive(
                absolute_source,
                members,
                staging,
                options,
                result.error
            )) {
            return result;
        }
        result.installed_files = static_cast<std::uint64_t>(members.size());
    } else if (!copy_directory_mod(
            absolute_source,
            staging,
            options,
            result.installed_files,
            result.installed_bytes,
            result.error
        )) {
        return result;
    }

    std::filesystem::rename(staging, destination, error);
    if (error) {
        result.error = "cannot atomically commit the installed mod";
        return result;
    }
    staging_guard.release();
    result.installed_path = destination;
    return result;
}

}  // namespace pulseforge
