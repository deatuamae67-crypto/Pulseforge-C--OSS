#include "sniff_bridge.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace pulseforge::detail {
namespace {

[[nodiscard]] std::string lower_extension(
    const std::filesystem::path& path
) {
    auto extension = path.extension().string();
    std::ranges::transform(
        extension,
        extension.begin(),
        [](const unsigned char item) {
            return static_cast<char>(std::tolower(item));
        }
    );
    return extension;
}

[[nodiscard]] bool regular_file(
    const std::filesystem::path& path
) noexcept {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error
        && !std::filesystem::is_symlink(path, error) && !error;
}

#if defined(_WIN32)

class UniqueHandle final {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.value_) {
        other.value_ = nullptr;
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    void reset() noexcept {
        if (valid()) {
            CloseHandle(value_);
        }
        value_ = nullptr;
    }

private:
    HANDLE value_{};
};

// Always drains the child pipe so a verbose or defective converter cannot
// block on stdout/stderr. Only the first 16 KiB are retained for diagnostics;
// all excess bytes are discarded instead of consuming unbounded disk space.
[[nodiscard]] bool drain_diagnostic_pipe(
    const HANDLE pipe,
    std::string& diagnostic
) noexcept {
    constexpr std::size_t maximum_retained_bytes = 16U * 1024U;
    std::array<char, 16U * 1024U> buffer{};
    for (;;) {
        DWORD available{};
        if (!PeekNamedPipe(pipe, nullptr, 0U, nullptr, &available, nullptr)) {
            return GetLastError() == ERROR_BROKEN_PIPE;
        }
        if (available == 0U) {
            return true;
        }
        DWORD bytes{};
        const DWORD requested = std::min<DWORD>(
            available,
            static_cast<DWORD>(buffer.size())
        );
        if (!ReadFile(pipe, buffer.data(), requested, &bytes, nullptr)) {
            return GetLastError() == ERROR_BROKEN_PIPE;
        }
        if (bytes == 0U) {
            return true;
        }
        const std::size_t retained = std::min<std::size_t>(
            bytes,
            maximum_retained_bytes - diagnostic.size()
        );
        for (std::size_t index = 0U; index < retained; ++index) {
            char value = buffer[index];
            const auto byte = static_cast<unsigned char>(value);
            if (byte < 32U && value != '\r' && value != '\n'
                && value != '\t') {
                value = '?';
            }
            diagnostic.push_back(value);
        }
    }
}

[[nodiscard]] std::wstring quote_argument(const std::wstring_view argument) {
    std::wstring result;
    result.reserve(argument.size() + 2U);
    result.push_back(L'"');
    std::size_t backslashes = 0U;
    for (const wchar_t value : argument) {
        if (value == L'\\') {
            ++backslashes;
            continue;
        }
        if (value == L'"') {
            result.append(backslashes * 2U + 1U, L'\\');
            result.push_back(L'"');
            backslashes = 0U;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0U;
        result.push_back(value);
    }
    result.append(backslashes * 2U, L'\\');
    result.push_back(L'"');
    return result;
}

[[nodiscard]] bool build_command_line(
    const std::vector<std::wstring>& arguments,
    std::wstring& output
) {
    constexpr std::size_t maximum_units = 32'766U;
    output.clear();
    for (const auto& argument : arguments) {
        const auto quoted = quote_argument(argument);
        const std::size_t separator = output.empty() ? 0U : 1U;
        if (quoted.size() > maximum_units
            || output.size() > maximum_units - quoted.size()
            || separator > maximum_units - output.size() - quoted.size()) {
            return false;
        }
        if (separator != 0U) {
            output.push_back(L' ');
        }
        output += quoted;
    }
    return !output.empty();
}

[[nodiscard]] std::filesystem::path powershell_path() {
    std::array<wchar_t, MAX_PATH + 1U> windows{};
    const UINT length = GetWindowsDirectoryW(
        windows.data(),
        static_cast<UINT>(windows.size())
    );
    if (length == 0U || length >= windows.size()) {
        return {};
    }
    return std::filesystem::path(windows.data())
        / "System32/WindowsPowerShell/v1.0/powershell.exe";
}

[[nodiscard]] std::string windows_error(const DWORD value) {
    return std::system_category().message(static_cast<int>(value));
}

[[nodiscard]] UniqueHandle open_locked_regular_file(
    const std::filesystem::path& path
) noexcept {
    UniqueHandle handle(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN
            | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr
    ));
    BY_HANDLE_FILE_INFORMATION information{};
    if (!handle.valid() || GetFileType(handle.get()) != FILE_TYPE_DISK
        || !GetFileInformationByHandle(handle.get(), &information)
        || (information.dwFileAttributes
            & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        handle.reset();
    }
    return handle;
}

[[nodiscard]] std::optional<std::string> sha256_handle(HANDLE file) {
    LARGE_INTEGER beginning{};
    if (file == nullptr || file == INVALID_HANDLE_VALUE
        || !SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) {
        return std::nullopt;
    }
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    const auto close = [&]() noexcept {
        if (hash != nullptr) BCryptDestroyHash(hash);
        if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0U);
    };
    if (BCryptOpenAlgorithmProvider(
            &algorithm,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0U
        ) != 0) {
        close();
        return std::nullopt;
    }
    DWORD object_bytes{};
    DWORD digest_bytes{};
    DWORD returned{};
    if (BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_bytes),
            sizeof(object_bytes),
            &returned,
            0U
        ) != 0
        || object_bytes == 0U
        || BCryptGetProperty(
            algorithm,
            BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&digest_bytes),
            sizeof(digest_bytes),
            &returned,
            0U
        ) != 0
        || digest_bytes != 32U) {
        close();
        return std::nullopt;
    }
    std::vector<unsigned char> object(object_bytes);
    std::array<unsigned char, 32U> digest{};
    if (BCryptCreateHash(
            algorithm,
            &hash,
            object.data(),
            object_bytes,
            nullptr,
            0U,
            0U
        ) != 0) {
        close();
        return std::nullopt;
    }
    std::array<unsigned char, 64U * 1024U> buffer{};
    for (;;) {
        DWORD bytes{};
        if (!ReadFile(
                file,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytes,
                nullptr
            )) {
            close();
            return std::nullopt;
        }
        if (bytes == 0U) break;
        if (BCryptHashData(
                hash,
                buffer.data(),
                bytes,
                0U
            ) != 0) {
            close();
            return std::nullopt;
        }
    }
    if (BCryptFinishHash(
            hash,
            digest.data(),
            static_cast<ULONG>(digest.size()),
            0U
        ) != 0) {
        close();
        return std::nullopt;
    }
    close();
    constexpr char hex[] = "0123456789ABCDEF";
    std::string output;
    output.resize(digest.size() * 2U);
    for (std::size_t index = 0U; index < digest.size(); ++index) {
        output[index * 2U] = hex[digest[index] >> 4U];
        output[index * 2U + 1U] = hex[digest[index] & 0x0FU];
    }
    return output;
}

#endif

}  // namespace

bool audited_sniff_wrapper_integrity(
    const std::filesystem::path& wrapper
) noexcept {
    try {
        if (!regular_file(wrapper) || lower_extension(wrapper) != ".ps1") {
            return false;
        }
#if defined(_WIN32)
        auto locked = open_locked_regular_file(wrapper);
        const auto hash = locked.valid()
            ? sha256_handle(locked.get())
            : std::nullopt;
        return hash.has_value() && *hash == audited_sniff_wrapper_sha256;
#else
        return false;
#endif
    } catch (...) {
        return false;
    }
}

SniffBridgeResult run_sniff_bridge(
    const SniffBridgeRequest& request,
    const std::function<bool()>& pump_events
) {
    SniffBridgeResult result;
    if (!audited_sniff_wrapper_integrity(request.wrapper)) {
        result.error = "the trusted PulseForge SNIFF wrapper is unavailable";
        return result;
    }
    if (!regular_file(request.executable)
        || lower_extension(request.executable) != ".exe") {
        result.error = "select a regular sniff-rusted.exe file";
        return result;
    }
    if (!regular_file(request.input_flp)
        || lower_extension(request.input_flp) != ".flp") {
        result.error = "select a regular FL Studio .flp project";
        return result;
    }
    if (lower_extension(request.output_json) != ".json") {
        result.error = "the SNIFF destination must be a JSON chart";
        return result;
    }
    std::error_code filesystem_error;
    if (std::filesystem::exists(request.output_json, filesystem_error)
        || filesystem_error) {
        result.error = "the SNIFF staging output already exists";
        return result;
    }

#if !defined(_WIN32)
    static_cast<void>(pump_events);
    result.error = "the guarded SNIFF bridge currently requires Windows";
    return result;
#else
    // Keep both path identities immutable between verification and use. The
    // wrapper is hashed natively; the converter is hashed by that wrapper.
    // Denying FILE_SHARE_WRITE/DELETE closes both check/use races while still
    // allowing PowerShell and CreateProcess to read the locked files.
    auto wrapper_lock = open_locked_regular_file(request.wrapper);
    auto converter_lock = open_locked_regular_file(request.executable);
    const auto locked_wrapper_hash = wrapper_lock.valid()
        ? sha256_handle(wrapper_lock.get())
        : std::nullopt;
    if (!locked_wrapper_hash.has_value()
        || *locked_wrapper_hash != audited_sniff_wrapper_sha256
        || !converter_lock.valid()) {
        result.error = "the SNIFF wrapper or converter could not be identity-locked";
        return result;
    }
    const auto powershell = powershell_path();
    if (!regular_file(powershell)) {
        result.error = "Windows PowerShell is unavailable";
        return result;
    }
    std::filesystem::create_directories(
        request.output_json.parent_path(),
        filesystem_error
    );
    if (filesystem_error) {
        result.error = "cannot create the guarded SNIFF staging directory";
        return result;
    }
    const std::vector<std::wstring> arguments{
        powershell.wstring(),
        L"-NoLogo",
        L"-NoProfile",
        L"-NonInteractive",
        L"-ExecutionPolicy",
        L"Bypass",
        L"-File",
        request.wrapper.wstring(),
        L"-SniffExecutable",
        request.executable.wstring(),
        L"-InputPath",
        request.input_flp.wstring(),
        L"-OutputPath",
        request.output_json.wstring(),
        L"-ExpectedSha256",
        std::wstring(
            std::begin(audited_sniff_sha256),
            std::end(audited_sniff_sha256) - 1
        ),
        L"-Direction",
        L"flp2json",
        L"-SongName",
        std::wstring(request.song_name.begin(), request.song_name.end()),
    };
    std::wstring command_line;
    if (!build_command_line(arguments, command_line)) {
        result.error = "the guarded SNIFF command exceeds the Windows limit";
        return result;
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = static_cast<DWORD>(sizeof(security));
    security.bInheritHandle = TRUE;
    HANDLE raw_diagnostic_read{};
    HANDLE raw_diagnostic_write{};
    if (!CreatePipe(
            &raw_diagnostic_read,
            &raw_diagnostic_write,
            &security,
            64U * 1024U
        )) {
        result.error = "cannot create the bounded SNIFF diagnostic pipe";
        return result;
    }
    UniqueHandle diagnostic_read(raw_diagnostic_read);
    UniqueHandle diagnostic_write(raw_diagnostic_write);
    if (!SetHandleInformation(
            diagnostic_read.get(),
            HANDLE_FLAG_INHERIT,
            0U
        )) {
        result.error = "cannot isolate the SNIFF diagnostic pipe";
        return result;
    }
    UniqueHandle null_input(CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    ));
    if (!diagnostic_read.valid() || !diagnostic_write.valid()
        || !null_input.valid()) {
        result.error = "cannot create the isolated SNIFF process streams";
        return result;
    }

    SIZE_T attribute_bytes = 0U;
    static_cast<void>(InitializeProcThreadAttributeList(
        nullptr,
        1U,
        0U,
        &attribute_bytes
    ));
    std::vector<unsigned char> attribute_storage(attribute_bytes);
    auto* const attributes = reinterpret_cast<
        LPPROC_THREAD_ATTRIBUTE_LIST
    >(attribute_storage.data());
    if (attribute_bytes == 0U || !InitializeProcThreadAttributeList(
            attributes,
            1U,
            0U,
            &attribute_bytes
        )) {
        result.error = "cannot isolate the SNIFF process handles";
        return result;
    }
    const std::array inherited{null_input.get(), diagnostic_write.get()};
    if (!UpdateProcThreadAttribute(
            attributes,
            0U,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            const_cast<HANDLE*>(inherited.data()),
            sizeof(inherited),
            nullptr,
            nullptr
        )) {
        DeleteProcThreadAttributeList(attributes);
        result.error = "cannot configure the SNIFF process handle list";
        return result;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = static_cast<DWORD>(sizeof(startup));
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null_input.get();
    startup.StartupInfo.hStdOutput = diagnostic_write.get();
    startup.StartupInfo.hStdError = diagnostic_write.get();
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
    job_limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.valid() || !SetInformationJobObject(
            job.get(),
            JobObjectExtendedLimitInformation,
            &job_limits,
            static_cast<DWORD>(sizeof(job_limits))
        )) {
        DeleteProcThreadAttributeList(attributes);
        result.error = "cannot create the isolated SNIFF process group";
        return result;
    }
    const BOOL created = CreateProcessW(
        powershell.c_str(),
        command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
        nullptr,
        request.output_json.parent_path().c_str(),
        &startup.StartupInfo,
        &process
    );
    DeleteProcThreadAttributeList(attributes);
    if (!created) {
        result.error = "cannot start the guarded SNIFF bridge: "
            + windows_error(GetLastError());
        return result;
    }
    UniqueHandle process_handle(process.hProcess);
    UniqueHandle thread_handle(process.hThread);
    diagnostic_write.reset();
    if (!AssignProcessToJobObject(job.get(), process_handle.get())
        || ResumeThread(thread_handle.get()) == static_cast<DWORD>(-1)) {
        static_cast<void>(TerminateProcess(process_handle.get(), 1U));
        static_cast<void>(WaitForSingleObject(process_handle.get(), 5'000U));
        result.error = "cannot start SNIFF inside its isolated process group";
        process_handle.reset();
        thread_handle.reset();
        job.reset();
        null_input.reset();
        return result;
    }
    thread_handle.reset();
    std::string diagnostic;
    DWORD wait_status = WAIT_TIMEOUT;
    while ((wait_status = WaitForSingleObject(process_handle.get(), 50U))
           == WAIT_TIMEOUT) {
        if (!drain_diagnostic_pipe(diagnostic_read.get(), diagnostic)) {
            static_cast<void>(TerminateProcess(process_handle.get(), 1U));
            static_cast<void>(WaitForSingleObject(process_handle.get(), 5'000U));
            result.error = "cannot drain the bounded SNIFF diagnostic pipe";
            break;
        }
        if (pump_events && !pump_events()) {
            static_cast<void>(TerminateProcess(process_handle.get(), 1223U));
            static_cast<void>(WaitForSingleObject(process_handle.get(), 5'000U));
            result.cancelled = true;
            result.error = "SNIFF conversion cancelled";
            break;
        }
    }
    static_cast<void>(drain_diagnostic_pipe(diagnostic_read.get(), diagnostic));
    if (!result.cancelled && wait_status == WAIT_FAILED) {
        result.error = "cannot wait for the guarded SNIFF process: "
            + windows_error(GetLastError());
    }
    DWORD exit_code = 1U;
    if (!GetExitCodeProcess(process_handle.get(), &exit_code)) {
        result.error = "cannot read the SNIFF process result";
    }
    result.exit_code = exit_code;
    process_handle.reset();
    job.reset();
    diagnostic_read.reset();
    null_input.reset();
    if (result.cancelled) {
        return result;
    }
    if (!result.error.empty()) {
        if (!diagnostic.empty()) {
            result.error += ": " + diagnostic;
        }
        return result;
    }
    if (exit_code != 0U) {
        result.error = "SNIFF conversion failed with exit code "
            + std::to_string(exit_code);
        if (!diagnostic.empty()) {
            result.error += ": " + diagnostic;
        }
        return result;
    }
    if (!regular_file(request.output_json)) {
        result.error = "SNIFF reported success without a JSON chart";
        return result;
    }
    result.success = true;
    return result;
#endif
}

}  // namespace pulseforge::detail
