#include "offline_encoder.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace pulseforge::detail {
namespace {

constexpr std::uintmax_t maximum_diagnostic_bytes = 64U * 1024U;
// Below one MiB a complete frame fits in the tuned Windows pipe buffer. The
// worker cannot overlap meaningful pipe stalls there and its synchronization
// can cost more than it saves (notably at 640x360). Keep that path synchronous.
constexpr std::size_t asynchronous_frame_threshold = 1U * 1024U * 1024U;
#if defined(_WIN32)
// CreatePipe's zero-size default is only a small system-dependent buffer. A
// render frame is commonly several MiB, so that default forces many producer /
// consumer wake-ups while FFmpeg is draining stdin. This is only a hint to the
// kernel (not a per-frame allocation) and remains bounded for every render.
constexpr DWORD ffmpeg_pipe_buffer_bytes = 1U * 1024U * 1024U;
#endif

void assign_error(std::string* output, std::string message) {
    if (output != nullptr) {
        *output = std::move(message);
    }
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {
        reinterpret_cast<const char*>(value.data()),
        value.size(),
    };
}

void remove_if_present(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove(path, ignored));
}

#if defined(_WIN32)

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : value_(other.release()) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }

    [[nodiscard]] HANDLE release() noexcept {
        const auto result = value_;
        value_ = nullptr;
        return result;
    }

    void reset(HANDLE value = nullptr) noexcept {
        if (valid()) {
            static_cast<void>(CloseHandle(value_));
        }
        value_ = value;
    }

private:
    HANDLE value_{};
};

[[nodiscard]] std::string windows_error_message(const DWORD code) {
    return std::system_category().message(static_cast<int>(code));
}

[[nodiscard]] bool utf8_to_wide(
    const std::string_view input,
    std::wstring& output
) {
    output.clear();
    if (input.empty()) {
        return true;
    }
    if (input.size() > static_cast<std::size_t>(
            std::numeric_limits<int>::max()
        )) {
        return false;
    }
    const auto input_size = static_cast<int>(input.size());
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        input.data(),
        input_size,
        nullptr,
        0
    );
    if (required <= 0) {
        return false;
    }
    output.resize(static_cast<std::size_t>(required));
    return MultiByteToWideChar(
               CP_UTF8,
               MB_ERR_INVALID_CHARS,
               input.data(),
               input_size,
               output.data(),
               required
           ) == required;
}

// Quote one argv element according to the CommandLineToArgvW/MSVC parsing
// rules. Every argument is quoted, which makes whitespace and shell-looking
// characters inert while preserving trailing backslashes and embedded quotes.
[[nodiscard]] std::wstring quote_windows_argument(
    const std::wstring_view argument
) {
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

[[nodiscard]] bool build_windows_command_line(
    const std::vector<std::string>& arguments,
    std::wstring& output,
    std::string* error
) {
    // CreateProcessW includes the terminating null in its 32,767 UTF-16 code
    // unit limit. Keep one unit free while composing the mutable buffer.
    constexpr std::size_t maximum_units = 32'766U;
    output.clear();
    for (const auto& argument : arguments) {
        std::wstring wide;
        if (!utf8_to_wide(argument, wide)) {
            assign_error(error, "FFmpeg argument is not valid UTF-8");
            return false;
        }
        auto quoted = quote_windows_argument(wide);
        const std::size_t separator = output.empty() ? 0U : 1U;
        if (quoted.size() > maximum_units
            || output.size() > maximum_units - quoted.size()
            || separator > maximum_units - output.size() - quoted.size()) {
            assign_error(error, "FFmpeg command line exceeds the Windows limit");
            return false;
        }
        if (separator != 0U) {
            output.push_back(L' ');
        }
        output += quoted;
    }
    return !output.empty();
}

#endif

}  // namespace

OfflineEncoder::~OfflineEncoder() {
    if (!finished_) {
        cancel();
    }
}

bool OfflineEncoder::start(OfflineRenderPlan plan, std::string* error) {
    if (started_) {
        assign_error(error, "offline encoder was already started");
        return false;
    }
    plan_ = std::move(plan);
    if (plan_.arguments.empty() || plan_.arguments.front().empty()) {
        assign_error(error, "FFmpeg command plan is empty");
        return false;
    }
    if (plan_.width == 0U || plan_.height == 0U
        || plan_.width > static_cast<std::uint32_t>(
            std::numeric_limits<int>::max()
        )
        || plan_.height > static_cast<std::uint32_t>(
            std::numeric_limits<int>::max()
        )
        || static_cast<std::size_t>(plan_.width)
            > std::numeric_limits<std::size_t>::max() / 4U) {
        assign_error(error, "offline render dimensions cannot form an RGBA frame");
        return false;
    }
    row_bytes_ = static_cast<std::size_t>(plan_.width) * 4U;
    if (row_bytes_ > static_cast<std::size_t>(
            std::numeric_limits<int>::max()
        )
        || static_cast<std::size_t>(plan_.height)
            > std::numeric_limits<std::size_t>::max() / row_bytes_) {
        assign_error(error, "offline RGBA frame size exceeds addressable memory");
        return false;
    }
    frame_bytes_ = row_bytes_ * static_cast<std::size_t>(plan_.height);
    asynchronous_writer_ = frame_bytes_ > asynchronous_frame_threshold;

    // PULSEFORGE_P1_5_0E_BOUNDED_MAX_PERFORMANCE_ENCODER_QUEUE_V1
    // Normal rendering keeps the historical ~64 MiB target. Maximum-
    // performance mode may pipeline more capture/pipe work, but remains hard
    // bounded at ~256 MiB and at most 12 full RGBA surfaces.
    const std::size_t queue_memory_budget = plan_.maximum_performance
        ? 256U * 1024U * 1024U
        : 64U * 1024U * 1024U;
    const auto memory_limited_slots = frame_bytes_ == 0U
        ? std::size_t{2U}
        : std::max<std::size_t>(
            2U,
            queue_memory_budget / frame_bytes_
        );
    maximum_frames_in_flight_ = std::clamp<std::size_t>(
        memory_limited_slots,
        2U,
        plan_.maximum_performance ? 12U : 6U
    );

    std::error_code filesystem_error;
    std::filesystem::create_directories(
        plan_.final_output_path.parent_path(),
        filesystem_error
    );
    if (filesystem_error) {
        assign_error(
            error,
            "cannot create renders directory: " + filesystem_error.message()
        );
        return false;
    }
    if (std::filesystem::exists(plan_.final_output_path, filesystem_error)
        && !filesystem_error && !plan_.overwrite) {
        assign_error(
            error,
            "render already exists (pass --render-overwrite to replace it): "
                + path_utf8(plan_.final_output_path)
        );
        return false;
    }
    remove_if_present(plan_.temporary_output_path);
    remove_if_present(plan_.diagnostic_log_path);

#if defined(_WIN32)
    std::wstring command_line;
    if (!build_windows_command_line(plan_.arguments, command_line, error)) {
        remove_private_files();
        return false;
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = static_cast<DWORD>(sizeof(security));
    security.bInheritHandle = TRUE;

    HANDLE raw_input_read{};
    HANDLE raw_input_write{};
    if (!CreatePipe(
            &raw_input_read,
            &raw_input_write,
            &security,
            ffmpeg_pipe_buffer_bytes
        )) {
        assign_error(
            error,
            "cannot create FFmpeg stdin pipe: "
                + windows_error_message(GetLastError())
        );
        return false;
    }
    UniqueHandle input_read(raw_input_read);
    UniqueHandle input_write(raw_input_write);
    if (!SetHandleInformation(
            input_write.get(),
            HANDLE_FLAG_INHERIT,
            0U
        )) {
        assign_error(
            error,
            "cannot isolate FFmpeg stdin pipe: "
                + windows_error_message(GetLastError())
        );
        return false;
    }

    UniqueHandle log(CreateFileW(
        plan_.diagnostic_log_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        &security,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    ));
    if (!log.valid()) {
        assign_error(
            error,
            "cannot create FFmpeg diagnostic log: "
                + windows_error_message(GetLastError())
        );
        return false;
    }
    UniqueHandle null_output(CreateFileW(
        L"NUL",
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    ));
    if (!null_output.valid()) {
        log.reset();
        remove_private_files();
        assign_error(
            error,
            "cannot open the null output for FFmpeg: "
                + windows_error_message(GetLastError())
        );
        return false;
    }

    SIZE_T attribute_bytes = 0U;
    static_cast<void>(InitializeProcThreadAttributeList(
        nullptr,
        1U,
        0U,
        &attribute_bytes
    ));
    if (attribute_bytes == 0U) {
        log.reset();
        remove_private_files();
        assign_error(
            error,
            "cannot size FFmpeg process attributes: "
                + windows_error_message(GetLastError())
        );
        return false;
    }
    std::vector<std::byte> attribute_storage(attribute_bytes);
    auto* const attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        attribute_storage.data()
    );
    if (!InitializeProcThreadAttributeList(
            attributes,
            1U,
            0U,
            &attribute_bytes
        )) {
        log.reset();
        remove_private_files();
        assign_error(
            error,
            "cannot initialize FFmpeg process attributes: "
                + windows_error_message(GetLastError())
        );
        return false;
    }

    HANDLE inherited_handles[]{
        input_read.get(),
        null_output.get(),
        log.get(),
    };
    const bool handle_list_ready = UpdateProcThreadAttribute(
        attributes,
        0U,
        PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited_handles,
        sizeof(inherited_handles),
        nullptr,
        nullptr
    ) != FALSE;
    if (!handle_list_ready) {
        const auto attribute_error = GetLastError();
        DeleteProcThreadAttributeList(attributes);
        log.reset();
        remove_private_files();
        assign_error(
            error,
            "cannot restrict FFmpeg inherited handles: "
                + windows_error_message(attribute_error)
        );
        return false;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = static_cast<DWORD>(sizeof(startup));
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input_read.get();
    startup.StartupInfo.hStdOutput = null_output.get();
    startup.StartupInfo.hStdError = log.get();
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process_info{};
    const BOOL created = CreateProcessW(
        plan_.ffmpeg_executable.c_str(),
        command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
        nullptr,
        nullptr,
        &startup.StartupInfo,
        &process_info
    );
    const auto process_error = created ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!created) {
        log.reset();
        remove_private_files();
        assign_error(
            error,
            "cannot start the validated FFmpeg executable: "
                + windows_error_message(process_error)
        );
        return false;
    }
    UniqueHandle thread(process_info.hThread);
    process_handle_ = process_info.hProcess;
    input_handle_ = input_write.release();
#else
    SDL_IOStream* log = SDL_IOFromFile(
        path_utf8(plan_.diagnostic_log_path).c_str(),
        "wb"
    );
    if (log == nullptr) {
        assign_error(
            error,
            "cannot create FFmpeg diagnostic log: " + std::string(SDL_GetError())
        );
        return false;
    }

    std::vector<const char*> argv;
    argv.reserve(plan_.arguments.size() + 1U);
    for (const auto& argument : plan_.arguments) {
        argv.push_back(argument.c_str());
    }
    argv.push_back(nullptr);

    const SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0U) {
        SDL_CloseIO(log);
        remove_private_files();
        assign_error(
            error,
            "cannot allocate FFmpeg process properties: "
                + std::string(SDL_GetError())
        );
        return false;
    }
    SDL_SetPointerProperty(
        properties,
        SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
        argv.data()
    );
    SDL_SetNumberProperty(
        properties,
        SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
        SDL_PROCESS_STDIO_APP
    );
    SDL_SetNumberProperty(
        properties,
        SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
        SDL_PROCESS_STDIO_NULL
    );
    SDL_SetNumberProperty(
        properties,
        SDL_PROP_PROCESS_CREATE_STDERR_NUMBER,
        SDL_PROCESS_STDIO_REDIRECT
    );
    SDL_SetPointerProperty(
        properties,
        SDL_PROP_PROCESS_CREATE_STDERR_POINTER,
        log
    );
    process_ = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    SDL_CloseIO(log);
    if (process_ == nullptr) {
        const std::string process_error = SDL_GetError();
        remove_private_files();
        assign_error(
            error,
            "cannot start the validated FFmpeg executable: " + process_error
        );
        return false;
    }
    input_ = SDL_GetProcessInput(process_);
    if (input_ == nullptr) {
        const std::string input_error = SDL_GetError();
        stop_process();
        remove_private_files();
        assign_error(
            error,
            "FFmpeg stdin pipe is unavailable: " + input_error
        );
        return false;
    }
#endif
    if (asynchronous_writer_) {
        {
            std::lock_guard lock(writer_mutex_);
            accepting_frames_ = true;
            discard_pending_ = false;
            writer_failed_ = false;
            writer_error_.clear();
        }
        try {
            writer_thread_ = std::thread(&OfflineEncoder::writer_loop, this);
        } catch (const std::exception& exception) {
            {
                std::lock_guard lock(writer_mutex_);
                accepting_frames_ = false;
            }
            stop_process();
            remove_private_files();
            assign_error(
                error,
                "cannot start the bounded FFmpeg writer: "
                    + std::string(exception.what())
            );
            return false;
        }
    }
    started_ = true;
    return true;
}

bool OfflineEncoder::write_frame(SDL_Renderer* renderer, std::string* error) {
#if defined(_WIN32)
    const bool process_ready = process_handle_ != nullptr
        && input_handle_ != nullptr;
#else
    const bool process_ready = process_ != nullptr && input_ != nullptr;
#endif
    if (!started_ || finished_ || !process_ready) {
        assign_error(error, "offline encoder is not accepting frames");
        return false;
    }
    if (frames_submitted_ >= plan_.frame_count) {
        assign_error(error, "offline renderer produced more frames than planned");
        return false;
    }
    const auto slot_started = std::chrono::steady_clock::now();
    if (asynchronous_writer_ && !reserve_frame_slot(error)) {
        return false;
    }
    const auto slot_wait_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - slot_started
        ).count()
    );
    last_slot_wait_ns_.store(slot_wait_ns, std::memory_order_relaxed);
    total_slot_wait_ns_.fetch_add(slot_wait_ns, std::memory_order_relaxed);

    const auto readback_started = std::chrono::steady_clock::now();
    SDL_Surface* captured = SDL_RenderReadPixels(renderer, nullptr);
    const auto readback_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - readback_started
        ).count()
    );
    last_readback_ns_.store(readback_ns, std::memory_order_relaxed);
    total_readback_ns_.fetch_add(readback_ns, std::memory_order_relaxed);
    if (captured == nullptr) {
        if (asynchronous_writer_) {
            release_frame_slot();
        }
        assign_error(
            error,
            "SDL framebuffer readback failed: " + std::string(SDL_GetError())
        );
        return false;
    }
    const bool valid_dimensions = captured->w == static_cast<int>(plan_.width)
        && captured->h == static_cast<int>(plan_.height);
    if (!valid_dimensions) {
        const auto actual = std::to_string(captured->w) + 'x'
            + std::to_string(captured->h);
        SDL_DestroySurface(captured);
        if (asynchronous_writer_) {
            release_frame_slot();
        }
        assign_error(
            error,
            "renderer readback size changed unexpectedly (got " + actual
                + ", expected " + std::to_string(plan_.width) + 'x'
                + std::to_string(plan_.height) + ')'
        );
        return false;
    }
    if (!asynchronous_writer_) {
        std::string write_failure;
        const bool wrote = write_surface(captured, write_failure);
        SDL_DestroySurface(captured);
        if (!wrote) {
            auto message = "FFmpeg stopped accepting raw video at frame "
                + std::to_string(frames_committed_) + ": " + write_failure;
            const auto diagnostics = diagnostic_excerpt();
            if (!diagnostics.empty()) {
                message += '\n';
                message += diagnostics;
            }
            assign_error(error, std::move(message));
            return false;
        }
        ++frames_submitted_;
        ++frames_committed_;
        return true;
    }
    {
        std::unique_lock lock(writer_mutex_);
        if (!accepting_frames_ || discard_pending_ || writer_failed_) {
            const auto failure = writer_error_;
            --frames_in_flight_;
            lock.unlock();
            writer_capacity_.notify_all();
            SDL_DestroySurface(captured);
            assign_error(
                error,
                failure.empty()
                    ? "offline encoder stopped while capturing a frame"
                    : failure
            );
            return false;
        }
        try {
            pending_frames_.push_back(captured);
        } catch (const std::exception& exception) {
            --frames_in_flight_;
            lock.unlock();
            writer_capacity_.notify_all();
            SDL_DestroySurface(captured);
            assign_error(
                error,
                "cannot queue captured render frame: "
                    + std::string(exception.what())
            );
            return false;
        }
        ++frames_submitted_;
    }
    writer_ready_.notify_one();
    return true;
}

bool OfflineEncoder::reserve_frame_slot(std::string* error) {
    std::unique_lock lock(writer_mutex_);
    writer_capacity_.wait(lock, [this]() {
        return frames_in_flight_ < maximum_frames_in_flight_
            || !accepting_frames_ || discard_pending_ || writer_failed_;
    });
    if (!accepting_frames_ || discard_pending_ || writer_failed_) {
        const auto failure = writer_error_;
        lock.unlock();
        assign_error(
            error,
            failure.empty()
                ? "offline encoder is no longer accepting frames"
                : writer_failure_message()
        );
        return false;
    }
    ++frames_in_flight_;
    return true;
}

void OfflineEncoder::release_frame_slot() noexcept {
    {
        std::lock_guard lock(writer_mutex_);
        if (frames_in_flight_ != 0U) {
            --frames_in_flight_;
        }
    }
    writer_capacity_.notify_all();
}

void OfflineEncoder::writer_loop() noexcept {
#if defined(_WIN32)
    if (plan_.maximum_performance) {
        // Encoder worker only; never elevate the process to realtime priority.
        static_cast<void>(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST));
    }
#endif
    for (;;) {
        SDL_Surface* surface{};
        std::deque<SDL_Surface*> discarded;
        {
            std::unique_lock lock(writer_mutex_);
            writer_ready_.wait(lock, [this]() {
                return !pending_frames_.empty() || !accepting_frames_
                    || discard_pending_;
            });
            if (discard_pending_) {
                discarded.swap(pending_frames_);
                frames_in_flight_ -= std::min(
                    frames_in_flight_,
                    discarded.size()
                );
            } else if (!pending_frames_.empty()) {
                surface = pending_frames_.front();
                pending_frames_.pop_front();
            } else {
                break;
            }
        }
        for (auto* const pending : discarded) {
            SDL_DestroySurface(pending);
        }
        if (!discarded.empty()) {
            writer_capacity_.notify_all();
        }
        if (surface == nullptr) {
            break;
        }

        std::string failure;
        bool wrote = false;
        const auto write_started = std::chrono::steady_clock::now();
        try {
            wrote = write_surface(surface, failure);
        } catch (const std::exception& exception) {
            failure = "writer allocation failed: " + std::string(exception.what());
        } catch (...) {
            failure = "writer failed with an unknown exception";
        }
        const auto worker_write_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - write_started
            ).count()
        );
        last_worker_write_ns_.store(
            worker_write_ns,
            std::memory_order_relaxed
        );
        total_worker_write_ns_.fetch_add(
            worker_write_ns,
            std::memory_order_relaxed
        );
        SDL_DestroySurface(surface);

        discarded.clear();
        {
            std::lock_guard lock(writer_mutex_);
            if (frames_in_flight_ != 0U) {
                --frames_in_flight_;
            }
            if (wrote) {
                ++frames_committed_;
            } else {
                writer_failed_ = true;
                accepting_frames_ = false;
                discard_pending_ = true;
                writer_error_ = "FFmpeg stopped accepting raw video at frame "
                    + std::to_string(frames_committed_) + ": " + failure;
                discarded.swap(pending_frames_);
                frames_in_flight_ -= std::min(
                    frames_in_flight_,
                    discarded.size()
                );
            }
        }
        for (auto* const pending : discarded) {
            SDL_DestroySurface(pending);
        }
        writer_capacity_.notify_all();
        writer_ready_.notify_all();
        if (!wrote) {
            break;
        }
    }
}

bool OfflineEncoder::write_surface(
    SDL_Surface* const surface,
    std::string& error
) {
    const std::byte* pixels{};
    if (surface->format == SDL_PIXELFORMAT_RGBA32
        && surface->pitch > 0
        && static_cast<std::size_t>(surface->pitch) == row_bytes_) {
        pixels = static_cast<const std::byte*>(surface->pixels);
    } else {
        frame_buffer_.resize(frame_bytes_);
        if (!SDL_ConvertPixels(
                surface->w,
                surface->h,
                surface->format,
                surface->pixels,
                surface->pitch,
                SDL_PIXELFORMAT_RGBA32,
                frame_buffer_.data(),
                static_cast<int>(row_bytes_)
            )) {
            error = "SDL RGBA frame conversion failed: "
                + std::string(SDL_GetError());
            return false;
        }
        pixels = frame_buffer_.data();
    }
    return write_bytes(pixels, frame_bytes_, error);
}

bool OfflineEncoder::write_bytes(
    const std::byte* bytes,
    const std::size_t byte_count,
    std::string& error
) {
    auto* cursor = bytes;
    std::size_t remaining = byte_count;
#if defined(_WIN32)
    while (remaining != 0U) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            remaining,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())
        ));
        DWORD written = 0U;
        if (!WriteFile(
                static_cast<HANDLE>(input_handle_),
                cursor,
                chunk,
                &written,
                nullptr
            ) || written == 0U) {
            error = windows_error_message(GetLastError());
            return false;
        }
        cursor += written;
        remaining -= written;
    }
#else
    std::uint32_t not_ready_retries = 0U;
    while (remaining != 0U) {
        const auto written = SDL_WriteIO(input_, cursor, remaining);
        if (written == 0U) {
            const auto status = SDL_GetIOStatus(input_);
            int process_exit_code = 0;
            const bool process_exited = SDL_WaitProcess(
                process_,
                false,
                &process_exit_code
            );
            if (!process_exited
                && (status == SDL_IO_STATUS_READY
                    || status == SDL_IO_STATUS_NOT_READY)
                && ++not_ready_retries <= 5'000U) {
                SDL_Delay(1U);
                continue;
            }
            error = SDL_GetError();
            if (error.empty()) {
                error = "FFmpeg stdin closed";
            }
            return false;
        }
        not_ready_retries = 0U;
        cursor += written;
        remaining -= written;
    }
#endif
    return true;
}

std::string OfflineEncoder::writer_failure_message() const {
    std::string result;
    {
        std::lock_guard lock(writer_mutex_);
        result = writer_error_;
    }
    if (result.empty()) {
        result = "FFmpeg writer stopped unexpectedly";
    }
    const auto diagnostics = diagnostic_excerpt();
    if (!diagnostics.empty()) {
        result += '\n';
        result += diagnostics;
    }
    return result;
}

void OfflineEncoder::stop_writer(const bool discard_pending) noexcept {
    {
        std::lock_guard lock(writer_mutex_);
        accepting_frames_ = false;
        discard_pending_ = discard_pending_ || discard_pending;
    }
    writer_ready_.notify_all();
    writer_capacity_.notify_all();
    if (discard_pending) {
        interrupt_process_write();
    }
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }

    std::deque<SDL_Surface*> abandoned;
    {
        std::lock_guard lock(writer_mutex_);
        abandoned.swap(pending_frames_);
        frames_in_flight_ = 0U;
    }
    for (auto* const surface : abandoned) {
        SDL_DestroySurface(surface);
    }
    writer_capacity_.notify_all();
}

void OfflineEncoder::interrupt_process_write() noexcept {
#if defined(_WIN32)
    if (process_handle_ != nullptr) {
        const auto process = static_cast<HANDLE>(process_handle_);
        if (WaitForSingleObject(process, 0U) == WAIT_TIMEOUT) {
            static_cast<void>(TerminateProcess(
                process,
                static_cast<UINT>(EXIT_FAILURE)
            ));
        }
    }
    if (writer_thread_.joinable()) {
        static_cast<void>(CancelSynchronousIo(
            static_cast<HANDLE>(writer_thread_.native_handle())
        ));
    }
#else
    if (process_ != nullptr) {
        static_cast<void>(SDL_KillProcess(process_, true));
    }
#endif
}

void OfflineEncoder::close_stdin() noexcept {
#if defined(_WIN32)
    if (input_handle_ != nullptr) {
        static_cast<void>(CloseHandle(static_cast<HANDLE>(input_handle_)));
        input_handle_ = nullptr;
    }
#else
    if (input_ == nullptr) {
        return;
    }
    if (process_ != nullptr) {
        const auto properties = SDL_GetProcessProperties(process_);
        if (properties != 0U) {
            SDL_SetPointerProperty(
                properties,
                SDL_PROP_PROCESS_STDIN_POINTER,
                nullptr
            );
        }
    }
    SDL_CloseIO(input_);
    input_ = nullptr;
#endif
}

void OfflineEncoder::stop_process() noexcept {
    close_stdin();
#if defined(_WIN32)
    if (process_handle_ == nullptr) {
        return;
    }
    const auto process = static_cast<HANDLE>(process_handle_);
    const DWORD wait_result = WaitForSingleObject(process, 0U);
    if (wait_result == WAIT_TIMEOUT || wait_result == WAIT_FAILED) {
        if (TerminateProcess(process, static_cast<UINT>(EXIT_FAILURE))) {
            static_cast<void>(WaitForSingleObject(process, INFINITE));
        }
    }
    static_cast<void>(CloseHandle(process));
    process_handle_ = nullptr;
#else
    if (process_ == nullptr) {
        return;
    }
    int exit_code = 0;
    if (!SDL_WaitProcess(process_, false, &exit_code)) {
        static_cast<void>(SDL_KillProcess(process_, false));
        if (!SDL_WaitProcess(process_, true, &exit_code)) {
            static_cast<void>(SDL_KillProcess(process_, true));
            static_cast<void>(SDL_WaitProcess(process_, true, &exit_code));
        }
    }
    SDL_DestroyProcess(process_);
    process_ = nullptr;
#endif
}

std::string OfflineEncoder::diagnostic_excerpt() const {
    std::ifstream input(plan_.diagnostic_log_path, std::ios::binary | std::ios::ate);
    if (!input) {
        return {};
    }
    const auto end = input.tellg();
    if (end <= std::streampos{0}) {
        return {};
    }
    const auto bytes = std::min<std::uintmax_t>(
        static_cast<std::uintmax_t>(end),
        maximum_diagnostic_bytes
    );
    std::string result(static_cast<std::size_t>(bytes), '\0');
    input.seekg(end - static_cast<std::streamoff>(bytes), std::ios::beg);
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    if (!input) {
        return {};
    }
    while (!result.empty()
        && (result.back() == '\0' || result.back() == '\r'
            || result.back() == '\n')) {
        result.pop_back();
    }
    return result;
}

void OfflineEncoder::remove_private_files() noexcept {
    remove_if_present(plan_.temporary_output_path);
    remove_if_present(plan_.diagnostic_log_path);
}

bool OfflineEncoder::commit_output(std::string* error) {
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(
            plan_.temporary_output_path,
            filesystem_error
        ) || filesystem_error
        || std::filesystem::file_size(
                plan_.temporary_output_path,
                filesystem_error
            ) == 0U
        || filesystem_error) {
        assign_error(error, "FFmpeg exited successfully but produced no MP4");
        return false;
    }

    const auto backup = plan_.temporary_output_path.parent_path()
        / (plan_.temporary_output_path.stem().string() + ".previous.mp4");
    remove_if_present(backup);
    const bool had_existing = std::filesystem::exists(
        plan_.final_output_path,
        filesystem_error
    ) && !filesystem_error;
    if (had_existing) {
        if (!plan_.overwrite) {
            assign_error(error, "render destination appeared during encoding");
            return false;
        }
        std::filesystem::rename(
            plan_.final_output_path,
            backup,
            filesystem_error
        );
        if (filesystem_error) {
            assign_error(
                error,
                "cannot preserve the previous render: "
                    + filesystem_error.message()
            );
            return false;
        }
    }
    std::filesystem::rename(
        plan_.temporary_output_path,
        plan_.final_output_path,
        filesystem_error
    );
    if (filesystem_error) {
        if (had_existing) {
            std::error_code restore_error;
            std::filesystem::rename(
                backup,
                plan_.final_output_path,
                restore_error
            );
        }
        assign_error(
            error,
            "cannot publish the completed render: " + filesystem_error.message()
        );
        return false;
    }
    remove_if_present(backup);
    return true;
}

bool OfflineEncoder::finish(std::string* error) {
#if defined(_WIN32)
    const bool process_ready = process_handle_ != nullptr;
#else
    const bool process_ready = process_ != nullptr;
#endif
    if (!started_ || finished_ || !process_ready) {
        assign_error(error, "offline encoder cannot be finalized in this state");
        return false;
    }
    std::uint64_t submitted = 0U;
    {
        std::lock_guard lock(writer_mutex_);
        submitted = frames_submitted_;
    }
    if (submitted != plan_.frame_count) {
        const auto message = "offline renderer wrote "
            + std::to_string(submitted) + " of "
            + std::to_string(plan_.frame_count) + " frames";
        cancel();
        assign_error(
            error,
            message
        );
        return false;
    }

    if (asynchronous_writer_) {
        stop_writer(false);
    }
    bool writer_failed = false;
    std::uint64_t committed = 0U;
    {
        std::lock_guard lock(writer_mutex_);
        writer_failed = writer_failed_;
        committed = frames_committed_;
    }
    if (writer_failed || committed != plan_.frame_count) {
        auto message = writer_failed
            ? writer_failure_message()
            : "bounded FFmpeg writer committed " + std::to_string(committed)
                + " of " + std::to_string(plan_.frame_count) + " frames";
        stop_process();
        remove_private_files();
        finished_ = true;
        assign_error(error, std::move(message));
        return false;
    }

    close_stdin();
    int exit_code = -1;
#if defined(_WIN32)
    const auto process = static_cast<HANDLE>(process_handle_);
    const DWORD wait_result = WaitForSingleObject(process, INFINITE);
    DWORD native_exit_code = 0U;
    bool exited = false;
    std::string wait_failure;
    if (wait_result == WAIT_OBJECT_0) {
        exited = GetExitCodeProcess(process, &native_exit_code) != FALSE;
        if (!exited) {
            wait_failure = windows_error_message(GetLastError());
        }
    } else {
        wait_failure = wait_result == WAIT_FAILED
            ? windows_error_message(GetLastError())
            : "unexpected process wait result";
    }
    if (exited) {
        exit_code = static_cast<int>(native_exit_code);
    }
    static_cast<void>(CloseHandle(process));
    process_handle_ = nullptr;
#else
    const bool exited = SDL_WaitProcess(process_, true, &exit_code);
    SDL_DestroyProcess(process_);
    process_ = nullptr;
#endif
    if (!exited || exit_code != 0) {
        const auto diagnostics = diagnostic_excerpt();
        remove_private_files();
        finished_ = true;
        assign_error(
            error,
            "FFmpeg failed"
                + std::string(exited ? " with exit code " : ": ")
                + (exited
                    ? std::to_string(exit_code)
#if defined(_WIN32)
                    : wait_failure
#else
                    : std::string(SDL_GetError())
#endif
                )
                + (diagnostics.empty() ? std::string{} : "\n" + diagnostics)
        );
        return false;
    }
    if (!commit_output(error)) {
        remove_private_files();
        finished_ = true;
        return false;
    }
    remove_if_present(plan_.diagnostic_log_path);
    finished_ = true;
    return true;
}

void OfflineEncoder::cancel() noexcept {
    if (!started_ || finished_) {
        return;
    }
    if (asynchronous_writer_) {
        stop_writer(true);
    }
    stop_process();
    remove_private_files();
    finished_ = true;
}

const OfflineRenderPlan* OfflineEncoder::plan() const noexcept {
    return started_ ? &plan_ : nullptr;
}

std::uint64_t OfflineEncoder::frames_written() const noexcept {
    std::lock_guard lock(writer_mutex_);
    return frames_submitted_;
}

std::uint64_t OfflineEncoder::total_frames() const noexcept {
    return started_ ? plan_.frame_count : 0U;
}

double OfflineEncoder::progress_fraction() const noexcept {
    if (!started_ || plan_.frame_count == 0U) {
        return 0.0;
    }
    return std::clamp(
        static_cast<double>(frames_written())
            / static_cast<double>(plan_.frame_count),
        0.0,
        1.0
    );
}

bool OfflineEncoder::active() const noexcept {
#if defined(_WIN32)
    const bool process_ready = process_handle_ != nullptr;
#else
    const bool process_ready = process_ != nullptr;
#endif
    std::lock_guard lock(writer_mutex_);
    return started_ && !finished_ && process_ready && !writer_failed_;
}

OfflineEncoderTelemetry OfflineEncoder::telemetry() const noexcept {
    OfflineEncoderTelemetry result;
    result.last_slot_wait_ns =
        last_slot_wait_ns_.load(std::memory_order_relaxed);
    result.last_readback_ns =
        last_readback_ns_.load(std::memory_order_relaxed);
    result.last_worker_write_ns =
        last_worker_write_ns_.load(std::memory_order_relaxed);
    result.total_slot_wait_ns =
        total_slot_wait_ns_.load(std::memory_order_relaxed);
    result.total_readback_ns =
        total_readback_ns_.load(std::memory_order_relaxed);
    result.total_worker_write_ns =
        total_worker_write_ns_.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(writer_mutex_);
        result.frames_in_flight = frames_in_flight_;
        result.queue_depth = pending_frames_.size();
        result.maximum_frames_in_flight = maximum_frames_in_flight_;
    }
    return result;
}

}  // namespace pulseforge::detail
