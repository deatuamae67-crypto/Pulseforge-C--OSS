#pragma once

#include "pulseforge/offline_render.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulseforge::detail {

// Owns one FFmpeg subprocess and its raw RGBA stdin stream. Destruction is
// fail-safe: an incomplete child is terminated and every private partial file
// is removed. Only finish() can atomically publish the final MP4.

struct OfflineEncoderTelemetry {
    std::uint64_t last_slot_wait_ns{};
    std::uint64_t last_readback_ns{};
    std::uint64_t last_worker_write_ns{};
    std::uint64_t total_slot_wait_ns{};
    std::uint64_t total_readback_ns{};
    std::uint64_t total_worker_write_ns{};
    std::size_t frames_in_flight{};
    std::size_t queue_depth{};
    std::size_t maximum_frames_in_flight{};
};

class OfflineEncoder final {
public:
    OfflineEncoder() = default;
    ~OfflineEncoder();

    OfflineEncoder(const OfflineEncoder&) = delete;
    OfflineEncoder& operator=(const OfflineEncoder&) = delete;
    OfflineEncoder(OfflineEncoder&&) = delete;
    OfflineEncoder& operator=(OfflineEncoder&&) = delete;

    [[nodiscard]] bool start(
        OfflineRenderPlan plan,
        std::string* error = nullptr
    );
    [[nodiscard]] bool write_frame(
        SDL_Renderer* renderer,
        std::string* error = nullptr
    );
    [[nodiscard]] bool finish(std::string* error = nullptr);
    // Cancels an active encode, terminates FFmpeg and removes every private
    // partial/log file. It is safe to call more than once.
    void cancel() noexcept;

    [[nodiscard]] const OfflineRenderPlan* plan() const noexcept;
    [[nodiscard]] std::uint64_t frames_written() const noexcept;
    [[nodiscard]] std::uint64_t total_frames() const noexcept;
    [[nodiscard]] double progress_fraction() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] OfflineEncoderTelemetry telemetry() const noexcept;

private:

    [[nodiscard]] bool reserve_frame_slot(std::string* error);
    void release_frame_slot() noexcept;
    void writer_loop() noexcept;
    [[nodiscard]] bool write_surface(
        SDL_Surface* surface,
        std::string& error
    );
    [[nodiscard]] bool write_bytes(
        const std::byte* bytes,
        std::size_t byte_count,
        std::string& error
    );
    void stop_writer(bool discard_pending) noexcept;
    void interrupt_process_write() noexcept;
    [[nodiscard]] std::string writer_failure_message() const;
    void close_stdin() noexcept;
    void stop_process() noexcept;
    void remove_private_files() noexcept;
    [[nodiscard]] std::string diagnostic_excerpt() const;
    [[nodiscard]] bool commit_output(std::string* error);

    OfflineRenderPlan plan_;
#if defined(_WIN32)
    // SDL's background-process flag correctly suppresses a console window on
    // Windows, but deliberately masks the child's exit code as zero. Keep the
    // native process and pipe handles opaque here so finish() can verify the
    // real FFmpeg result without exposing windows.h to every includer.
    void* process_handle_{};
    void* input_handle_{};
#else
    SDL_Process* process_{};
    SDL_IOStream* input_{};
#endif
    // SDL readback remains on the render thread. Ownership of each captured
    // surface then crosses an adaptive bounded queue. Normal rendering stays at
    // 2-6 frames with a ~64 MiB target; explicit maximum-performance rendering
    // may use up to 12 frames with a ~256 MiB target. Both policies remain
    // bounded, allowing the next frame to render while the joinable worker
    // drains FFmpeg stdin. Keeping
    // the SDL_Surface alive avoids a full-frame producer-side copy on the
    // common tightly packed RGBA path.
    mutable std::mutex writer_mutex_;
    std::condition_variable writer_ready_;
    std::condition_variable writer_capacity_;
    std::deque<SDL_Surface*> pending_frames_;
    std::thread writer_thread_;
    std::size_t frames_in_flight_{};
    std::size_t maximum_frames_in_flight_{2U};
    std::atomic<std::uint64_t> last_slot_wait_ns_{};
    std::atomic<std::uint64_t> last_readback_ns_{};
    std::atomic<std::uint64_t> last_worker_write_ns_{};
    std::atomic<std::uint64_t> total_slot_wait_ns_{};
    std::atomic<std::uint64_t> total_readback_ns_{};
    std::atomic<std::uint64_t> total_worker_write_ns_{};
    bool accepting_frames_{};
    bool discard_pending_{};
    bool writer_failed_{};
    std::string writer_error_;

    // Worker-owned conversion scratch. It is reused only when readback pitch
    // or format differs from FFmpeg's tightly packed RGBA input.
    std::vector<std::byte> frame_buffer_;
    std::size_t row_bytes_{};
    std::size_t frame_bytes_{};
    std::uint64_t frames_submitted_{};
    std::uint64_t frames_committed_{};
    bool asynchronous_writer_{};
    bool started_{};
    bool finished_{};
};

}  // namespace pulseforge::detail
