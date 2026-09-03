#include "pulseforge/autochart.hpp"

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace pulseforge {
namespace {

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr std::uint32_t analysis_sample_rate = 48'000U;
constexpr std::size_t decoded_read_samples = 128U * 1024U;
constexpr std::size_t video_width = 64U;
constexpr std::size_t video_height = 36U;
constexpr double video_fps = 12.0;
constexpr std::uint32_t feature_cache_schema = 3U;
constexpr std::size_t maximum_cached_feature_frames = 2'000'000U;
constexpr std::uint32_t default_review_queue_limit = 20'000U;
constexpr float high_priority_review_threshold = 0.60F;

class AutoChartCancelled final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "AutoChart generation cancelled";
    }
};

void emit_progress(
    const AutoChartOptions& options,
    const AutoChartProgressStage stage,
    const double fraction,
    std::string message,
    const std::uint64_t current = 0U,
    const std::uint64_t total = 0U
) noexcept {
    if (!options.progress_callback) {
        return;
    }
    AutoChartProgress progress;
    progress.stage = stage;
    progress.fraction = std::clamp(fraction, 0.0, 1.0);
    progress.progress = progress.fraction;
    progress.overall_fraction = progress.fraction;
    progress.stage_fraction = progress.fraction;
    progress.percentage = progress.fraction * 100.0;
    progress.current = current;
    progress.total = total;
    progress.message = std::move(message);
    progress.detail = progress.message;
    progress.status = progress.message;
    progress.label = progress.message;
    try {
        options.progress_callback(progress);
    } catch (...) {
        // UI/reporting callbacks are observers. A frontend bug must never turn
        // an otherwise valid deterministic chart generation into a failure.
    }
}

[[nodiscard]] bool autochart_cancel_requested(
    const AutoChartOptions& options
) noexcept {
    if (!options.cancel_requested) {
        return false;
    }
    try {
        return options.cancel_requested();
    } catch (...) {
        return false;
    }
}

void throw_if_autochart_cancelled(const AutoChartOptions& options) {
    if (autochart_cancel_requested(options)) {
        throw AutoChartCancelled{};
    }
}

struct AnalysisParameters final {
    std::size_t fft_size{};
    std::size_t hop_size{};
    float onset_floor{};
};

struct FeatureFrame final {
    double time_ms{};
    float rms{};
    float flux{};
    float low_flux{};
    float mid_flux{};
    float high_flux{};
    float centroid_hz{};
    float peak_hz{};
    float onset{};
    float video_pulse{};
};

enum class CandidateSource : std::uint8_t {
    low,
    mid,
    high,
    mixed,
    drums,
    bass,
    vocals,
    other,
};

enum CandidateEvidence : std::uint32_t {
    evidence_dsp = 1U << 0U,
    evidence_video = 1U << 1U,
    evidence_stem = 1U << 2U,
    evidence_pitch = 1U << 3U,
    evidence_neural_beat = 1U << 4U,
    evidence_drum_ml = 1U << 5U,
    evidence_vocal_ml = 1U << 6U,
};

struct Candidate final {
    double time_ms{};
    double sustain_ms{};
    float confidence{};
    float onset{};
    float beat_alignment{};
    float video_pulse{};
    float peak_hz{};
    float centroid_hz{};
    CandidateSource source{CandidateSource::mid};
    bool polyphonic{};
    std::uint32_t evidence{evidence_dsp};
};


struct ReviewNote final {
    double time_ms{};
    double duration_ms{};
    double quantization_shift_ms{};
    std::uint16_t lane{};
    float confidence{};
    float priority{};
    float beat_alignment{};
    float review_priority{};
    std::uint32_t evidence{};
    CandidateSource source{CandidateSource::mid};
    bool chord{};
    bool jack_risk{};
};

struct DifficultyReview final {
    std::string difficulty;
    double quality_score{};
    std::uint64_t note_count{};
    std::uint64_t low_confidence_count{};
    std::uint64_t uncertain_count{};
    std::uint64_t high_priority_count{};
    std::vector<ReviewNote> notes;
};

struct ReviewNoteLess final {
    [[nodiscard]] bool operator()(const ReviewNote& left, const ReviewNote& right) const noexcept {
        // priority_queue top() is the least useful retained review entry so it
        // can be replaced in O(log K) while memory stays bounded by K.
        return left.review_priority > right.review_priority;
    }
};

class BoundedReviewCollector final {
public:
    explicit BoundedReviewCollector(const std::size_t limit) : limit_(limit) {}

    void add(ReviewNote note) {
        if (limit_ == 0U) {
            return;
        }
        if (notes_.size() < limit_) {
            notes_.push(std::move(note));
            return;
        }
        if (note.review_priority <= notes_.top().review_priority) {
            return;
        }
        notes_.pop();
        notes_.push(std::move(note));
    }

    [[nodiscard]] std::vector<ReviewNote> take() {
        std::vector<ReviewNote> result;
        result.reserve(notes_.size());
        while (!notes_.empty()) {
            result.push_back(notes_.top());
            notes_.pop();
        }
        std::sort(result.begin(), result.end(), [](const ReviewNote& left, const ReviewNote& right) {
            if (left.review_priority != right.review_priority) {
                return left.review_priority > right.review_priority;
            }
            if (left.time_ms != right.time_ms) {
                return left.time_ms < right.time_ms;
            }
            return left.lane < right.lane;
        });
        return result;
    }

private:
    std::size_t limit_{};
    std::priority_queue<ReviewNote, std::vector<ReviewNote>, ReviewNoteLess> notes_;
};


struct MlPitchEvent final {
    double start_ms{};
    double end_ms{};
    std::uint8_t midi{};
    float confidence{};
    CandidateSource source{CandidateSource::other};
};

enum class MlDrumRole : std::uint8_t {
    kick,
    snare,
    tom,
    hihat,
    cymbal,
    other,
};

enum class MlVocalRole : std::uint8_t {
    phoneme,
    syllable,
};

struct MlVocalEvent final {
    double start_ms{};
    double end_ms{};
    float confidence{};
    MlVocalRole role{MlVocalRole::phoneme};
    std::string token;
};

struct MlDrumEvent final {
    double time_ms{};
    std::uint8_t midi{};
    float confidence{};
    MlDrumRole role{MlDrumRole::other};
};

struct MlAnalysis final {
    bool used{};
    bool cache_hit{};
    bool source_separation_used{};
    bool beat_tracking_used{};
    bool drum_transcription_used{};
    bool pitch_transcription_used{};
    bool vocal_refinement_used{};
    std::string device;
    std::filesystem::path drums;
    std::filesystem::path bass;
    std::filesystem::path vocals;
    std::filesystem::path other;
    std::vector<double> beats_ms;
    std::vector<double> downbeats_ms;
    std::vector<MlDrumEvent> drum_events;
    std::vector<MlPitchEvent> pitch_events;
    std::vector<MlVocalEvent> vocal_events;
    std::vector<std::string> diagnostics;
};

struct BeatGrid final {
    double bpm{120.0};
    double period_ms{500.0};
    double offset_ms{};
    double confidence{};
    std::vector<double> beats;
    std::vector<double> downbeats;
    std::vector<TempoChange> tempos;
};

enum class StructuralSectionKind : std::uint8_t {
    intro,
    verse,
    chorus,
    buildup,
    drop,
    breakdown,
    bridge,
    outro,
};

struct StructuralSection final {
    double start_ms{};
    double end_ms{};
    float intensity{};
    float density{};
    float novelty{};
    float density_scale{1.0F};
    float confidence{};
    StructuralSectionKind kind{StructuralSectionKind::verse};
};

struct SongStructure final {
    bool used{};
    double confidence{};
    std::vector<StructuralSection> sections;
    std::vector<double> phrase_boundaries;
};

struct BarDescriptor final {
    double start_ms{};
    double end_ms{};
    float rms{};
    float onset{};
    float low{};
    float mid{};
    float high{};
    float centroid{};
    float candidate_density{};
    float intensity{};
    float novelty{};
};

struct DifficultyProfile final {
    std::string_view name;
    float minimum_confidence{};
    double minimum_spacing_ms{};
    double target_nps{};
    std::uint32_t peak_nps{};
    std::uint32_t subdivisions_per_beat{};
    float chord_confidence{};
    double sustain_minimum_ms{};
    double maximum_sustain_ms{};
};

struct DifficultyBuild final {
    Chart chart;
    DifficultyReview review;
    double mean_confidence{};
    double average_nps{};
    double peak_nps{};
    std::uint32_t beam_width{};
    bool beam_used{};
};

// PULSEFORGE_AUTOCHART_TEMP_CLEANUP_V1
[[nodiscard]] std::uint64_t autochart_current_process_id() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

[[nodiscard]] bool autochart_process_is_alive(
    const std::uint64_t raw_pid
) noexcept {
    if (raw_pid == 0U) {
        return false;
    }

#if defined(_WIN32)
    if (raw_pid > static_cast<std::uint64_t>((std::numeric_limits<DWORD>::max)())) {
        return false;
    }
    const DWORD pid = static_cast<DWORD>(raw_pid);
    if (pid == GetCurrentProcessId()) {
        return true;
    }

    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) {
        // ERROR_INVALID_PARAMETER means no process with that PID exists.
        // Access-denied and other failures are treated as alive so cleanup is
        // conservative rather than deleting a possibly-active workspace.
        return GetLastError() != ERROR_INVALID_PARAMETER;
    }
    const DWORD wait = WaitForSingleObject(process, 0U);
    CloseHandle(process);
    return wait == WAIT_TIMEOUT;
#else
    if (raw_pid > static_cast<std::uint64_t>((std::numeric_limits<pid_t>::max)())) {
        return false;
    }
    const auto pid = static_cast<pid_t>(raw_pid);
    if (pid == ::getpid()) {
        return true;
    }
    errno = 0;
    if (::kill(pid, 0) == 0) {
        return true;
    }
    return errno == EPERM;
#endif
}

[[nodiscard]] std::optional<std::uint64_t>
autochart_workspace_owner_pid(const std::filesystem::path& path) {
    // PULSEFORGE_AUTOCHART_TEMP_UNICODE_FAILFAST_FIX_V1
#if defined(_WIN32)
    constexpr std::wstring_view prefix{L"pulseforge-autochart-v2-"};
    const auto name = path.filename().wstring();
    if (!std::wstring_view{name}.starts_with(prefix)) {
        return std::nullopt;
    }

    const auto remainder = std::wstring_view{name}.substr(prefix.size());
    const auto separator = remainder.find(L'-');
    if (separator == std::wstring_view::npos || separator == 0U) {
        return std::nullopt;
    }

    std::uint64_t pid = 0U;
    for (std::size_t index = 0U; index < separator; ++index) {
        const wchar_t c = remainder[index];
        if (c < L'0' || c > L'9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(c - L'0');
        if (pid > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10U) {
            return std::nullopt;
        }
        pid = pid * 10U + digit;
    }
    return pid;
#else
    constexpr std::string_view prefix{"pulseforge-autochart-v2-"};
    const auto name = path.filename().string();
    if (!std::string_view{name}.starts_with(prefix)) {
        return std::nullopt;
    }

    const auto remainder = std::string_view{name}.substr(prefix.size());
    const auto separator = remainder.find('-');
    if (separator == std::string_view::npos || separator == 0U) {
        return std::nullopt;
    }

    std::uint64_t pid{};
    const auto parsed = std::from_chars(
        remainder.data(),
        remainder.data() + separator,
        pid
    );
    if (parsed.ec != std::errc{} || parsed.ptr != remainder.data() + separator) {
        return std::nullopt;
    }
    return pid;
#endif
}

void cleanup_abandoned_autochart_workspaces(
    const std::filesystem::path& base
) noexcept {
    // PULSEFORGE_AUTOCHART_TEMP_UNICODE_FAILFAST_FIX_V1
    try {
        std::error_code error;
        std::filesystem::directory_iterator iterator(base, error);
        const std::filesystem::directory_iterator end;
        if (error) {
            return;
        }

        while (iterator != end) {
            try {
                const auto candidate = iterator->path();

#if defined(_WIN32)
                const auto filename = candidate.filename().wstring();
                const std::wstring_view name{filename};
                const bool is_v2 =
                    name.starts_with(L"pulseforge-autochart-v2-");
                const bool is_legacy =
                    !is_v2 && name.starts_with(L"pulseforge-autochart-");
#else
                const auto filename = candidate.filename().string();
                const std::string_view name{filename};
                const bool is_v2 =
                    name.starts_with("pulseforge-autochart-v2-");
                const bool is_legacy =
                    !is_v2 && name.starts_with("pulseforge-autochart-");
#endif

                bool remove = false;
                if (is_v2) {
                    const auto pid = autochart_workspace_owner_pid(candidate);
                    remove = pid.has_value()
                        && *pid != autochart_current_process_id()
                        && !autochart_process_is_alive(*pid);
                } else if (is_legacy) {
                    std::error_code time_error;
                    const auto modified =
                        std::filesystem::last_write_time(candidate, time_error);
                    if (!time_error) {
                        const auto age =
                            std::filesystem::file_time_type::clock::now()
                            - modified;
                        remove = age > std::chrono::hours{24};
                    }
                }

                if (remove) {
                    std::error_code cleanup_error;
                    std::filesystem::remove_all(candidate, cleanup_error);
                }
            } catch (...) {
                // Ignore one malformed/inaccessible TEMP entry.
            }

            iterator.increment(error);
            if (error) {
                return;
            }
        }
    } catch (...) {
        // Cleanup is best-effort and must never terminate AutoChart.
        return;
    }
}

class TemporaryWorkspace final {
public:
    TemporaryWorkspace() {
        std::error_code error;
        const auto base = std::filesystem::temp_directory_path(error);
        if (error) {
            throw std::runtime_error("cannot resolve the temporary directory");
        }

        cleanup_abandoned_autochart_workspaces(base);

        std::random_device random;
        const auto suffix = static_cast<unsigned long long>(random())
            ^ (static_cast<unsigned long long>(random()) << 32U);
        path_ = base / (
            "pulseforge-autochart-v2-"
            + std::to_string(autochart_current_process_id())
            + "-"
            + std::to_string(suffix)
        );
        std::filesystem::create_directories(path_, error);
        if (error) {
            throw std::runtime_error("cannot create the AutoChart workspace");
        }
    }

    ~TemporaryWorkspace() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryWorkspace(const TemporaryWorkspace&) = delete;
    TemporaryWorkspace& operator=(const TemporaryWorkspace&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return value;
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {
        reinterpret_cast<const char*>(value.data()),
        value.size(),
    };
}

[[nodiscard]] std::string sanitize_slug(const std::string_view input) {
    std::string output;
    output.reserve(input.size());
    bool separator = false;
    for (const char raw : input) {
        const unsigned char c = static_cast<unsigned char>(raw);
        const bool ascii_alnum = (c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (ascii_alnum) {
            if (separator && !output.empty()) {
                output.push_back('-');
            }
            output.push_back(
                raw >= 'A' && raw <= 'Z'
                    ? static_cast<char>(raw - 'A' + 'a')
                    : raw
            );
            separator = false;
        } else if (c >= 0x80U) {
            if (separator && !output.empty()) {
                output.push_back('-');
            }
            output.push_back(raw);
            separator = false;
        } else {
            separator = !output.empty();
        }
    }
    while (!output.empty() && output.back() == '-') {
        output.pop_back();
    }
    return output.empty() ? std::string{"autochart"} : output;
}

[[nodiscard]] std::string json_escape(const std::string_view input) {
    std::ostringstream output;
    for (const char raw : input) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (c < 0x20U) {
                output << "\\u" << std::hex << std::setw(4)
                       << std::setfill('0') << static_cast<unsigned int>(c)
                       << std::dec << std::setfill(' ');
            } else {
                output << static_cast<char>(c);
            }
            break;
        }
    }
    return output.str();
}

[[nodiscard]] AnalysisParameters parameters_for(const AutoChartMode mode) {
    switch (mode) {
    case AutoChartMode::fast:
        return {1'024U, 512U, 0.28F};
    case AutoChartMode::maximum:
        // PULSEFORGE_P1_5_0E_SUBFRAME_MAXIMUM_ANALYSIS_V1
        // ~2.7 ms hops at 48 kHz while retaining a high-resolution spectrum.
        // This spends CPU, not disk/model GB, for timing precision.
        return {4'096U, 128U, 0.14F};
    case AutoChartMode::accurate:
    default:
        return {2'048U, 256U, 0.18F};
    }
}

[[nodiscard]] DifficultyProfile difficulty_profile(const std::string_view raw) {
    const auto name = lower_ascii(std::string(raw));
    if (name == "easy") {
        return {"easy", 0.72F, 180.0, 1.8, 4U, 1U, 0.98F, 650.0, 1'500.0};
    }
    if (name == "normal" || name == "medium") {
        return {"normal", 0.60F, 120.0, 3.2, 6U, 2U, 0.95F, 525.0, 1'800.0};
    }
    if (name == "hard") {
        return {"hard", 0.48F, 82.0, 5.0, 9U, 4U, 0.91F, 425.0, 2'000.0};
    }
    if (name == "insane" || name == "mania") {
        return {"insane", 0.25F, 34.0, 10.5, 20U, 8U, 0.80F, 300.0, 2'600.0};
    }
    return {"expert", 0.36F, 52.0, 7.2, 14U, 4U, 0.86F, 340.0, 2'300.0};
}

#if defined(_WIN32)
[[nodiscard]] std::wstring quote_windows_argument(const std::wstring& argument) {
    if (argument.empty()) {
        return L"\"\"";
    }
    const bool needs_quotes = argument.find_first_of(L" \t\n\v\"")
        != std::wstring::npos;
    if (!needs_quotes) {
        return argument;
    }
    std::wstring output;
    output.push_back(L'\"');
    std::size_t backslashes = 0U;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'\"') {
            output.append(backslashes * 2U + 1U, L'\\');
            output.push_back(L'\"');
            backslashes = 0U;
            continue;
        }
        output.append(backslashes, L'\\');
        backslashes = 0U;
        output.push_back(c);
    }
    output.append(backslashes * 2U, L'\\');
    output.push_back(L'\"');
    return output;
}

[[nodiscard]] int run_process(
    const std::filesystem::path& executable,
    const std::vector<std::filesystem::path>& arguments
) {
    std::wstring command = quote_windows_argument(executable.wstring());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command.append(quote_windows_argument(argument.wstring()));
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    // PULSEFORGE_AUTOCHART_CHILD_JOB_V1
    // The ML backend can spawn Demucs/FFmpeg children. Put the whole child tree
    // in a kill-on-close Job Object before it starts. If PulseForge is
    // terminated/cancelled, Windows closes the job handle and cannot leave a
    // multi-GB analysis process running in the background.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    bool job_ready = false;
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        job_ready = SetInformationJobObject(
            job,
            JobObjectExtendedLimitInformation,
            &limits,
            sizeof(limits)
        ) != FALSE;
        if (!job_ready) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const DWORD creation_flags =
        CREATE_NO_WINDOW | (job_ready ? CREATE_SUSPENDED : 0U);
    const BOOL created = CreateProcessW(
        nullptr,
        mutable_command.data(),
        nullptr,
        nullptr,
        FALSE,
        creation_flags,
        nullptr,
        nullptr,
        &startup,
        &process
    );
    if (!created) {
        if (job != nullptr) {
            CloseHandle(job);
        }
        return -1;
    }

    if (job_ready) {
        if (AssignProcessToJobObject(job, process.hProcess) == FALSE) {
            // Some hosts already place PulseForge in a non-breakaway job.
            // Fall back to ordinary child execution rather than failing
            // AutoChart altogether.
            CloseHandle(job);
            job = nullptr;
            job_ready = false;
        }
        static_cast<void>(ResumeThread(process.hThread));
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1U;
    static_cast<void>(GetExitCodeProcess(process.hProcess, &exit_code));
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (job != nullptr) {
        CloseHandle(job);
    }
    return static_cast<int>(exit_code);
}

[[nodiscard]] std::filesystem::path executable_directory() {
    std::array<wchar_t, 32'768U> buffer{};
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size())
    );
    if (length == 0U || length >= buffer.size()) {
        return {};
    }
    return std::filesystem::path(
        std::wstring(buffer.data(), static_cast<std::size_t>(length))
    ).parent_path();
}
#else
[[nodiscard]] std::string quote_shell_argument(const std::string& argument) {
    std::string output{"'"};
    for (const char c : argument) {
        if (c == '\'') {
            output += "'\\''";
        } else {
            output.push_back(c);
        }
    }
    output.push_back('\'');
    return output;
}

[[nodiscard]] int run_process(
    const std::filesystem::path& executable,
    const std::vector<std::filesystem::path>& arguments
) {
    std::string command = quote_shell_argument(path_utf8(executable));
    for (const auto& argument : arguments) {
        command.push_back(' ');
        command += quote_shell_argument(path_utf8(argument));
    }
    return std::system(command.c_str());
}

[[nodiscard]] std::filesystem::path executable_directory() {
    return {};
}
#endif

[[nodiscard]] std::filesystem::path resolve_ffmpeg(
    const AutoChartOptions& options
) {
    if (!options.ffmpeg_path.empty()) {
        return options.ffmpeg_path;
    }
    std::vector<std::filesystem::path> candidates;
    const auto exe_dir = executable_directory();
    if (!exe_dir.empty()) {
#if defined(_WIN32)
        candidates.push_back(exe_dir / "tools/ffmpeg/bin/ffmpeg.exe");
        candidates.push_back(exe_dir / "ffmpeg.exe");
#else
        candidates.push_back(exe_dir / "tools/ffmpeg/bin/ffmpeg");
        candidates.push_back(exe_dir / "ffmpeg");
#endif
    }
#if defined(_WIN32)
    candidates.emplace_back("tools/ffmpeg/bin/ffmpeg.exe");
    candidates.emplace_back("ffmpeg.exe");
#else
    candidates.emplace_back("tools/ffmpeg/bin/ffmpeg");
    candidates.emplace_back("ffmpeg");
#endif
    for (const auto& candidate : candidates) {
        if (candidate.has_parent_path()) {
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error) && !error) {
                return candidate;
            }
            continue;
        }
        // Bare executable names deliberately fall through to PATH lookup in
        // CreateProcess/system.
        return candidate;
    }
    return {};
}

[[nodiscard]] std::filesystem::path resolve_ml_python(
    const AutoChartOptions& options
) {
    if (!options.ml_python_path.empty()) {
        return options.ml_python_path;
    }
    const auto exe_dir = executable_directory();
    std::vector<std::filesystem::path> candidates;
#if defined(_WIN32)
    if (!exe_dir.empty()) {
        candidates.push_back(exe_dir / "tools/autochart/.venv/Scripts/python.exe");
        candidates.push_back(exe_dir / "tools/autochart/python/python.exe");
    }
    candidates.emplace_back("tools/autochart/.venv/Scripts/python.exe");
    candidates.emplace_back("python.exe");
#else
    if (!exe_dir.empty()) {
        candidates.push_back(exe_dir / "tools/autochart/.venv/bin/python");
    }
    candidates.emplace_back("tools/autochart/.venv/bin/python");
    candidates.emplace_back("python3");
#endif
    for (const auto& candidate : candidates) {
        if (!candidate.has_parent_path()) {
            return candidate; // PATH lookup is deferred to CreateProcess/system.
        }
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return candidate;
        }
    }
    return {};
}

[[nodiscard]] std::filesystem::path resolve_ml_backend_script(
    const AutoChartOptions& options
) {
    if (!options.ml_backend_script.empty()) {
        return options.ml_backend_script;
    }
    std::vector<std::filesystem::path> candidates;
    const auto exe_dir = executable_directory();
    if (!exe_dir.empty()) {
        candidates.push_back(exe_dir / "tools/autochart/ml_backend.py");
    }
    candidates.emplace_back("tools/autochart/ml_backend.py");
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return candidate;
        }
    }
    return {};
}

[[nodiscard]] std::filesystem::path resolve_ml_cache_root(
    const AutoChartOptions& options
) {
    if (!options.ml_cache_root.empty()) {
        return options.ml_cache_root;
    }

    // PULSEFORGE_AUTOCHART_PERSISTENT_CACHE_V1
    // ML weights/caches are project data, not disposable build products.
    std::vector<std::filesystem::path> search_roots;
    std::error_code current_error;
    const auto current = std::filesystem::current_path(current_error);
    if (!current_error) search_roots.push_back(current);

    const auto exe_dir = executable_directory();
    if (!exe_dir.empty()) search_roots.push_back(exe_dir);

    for (auto candidate : search_roots) {
        for (std::size_t depth = 0U; depth < 10U && !candidate.empty(); ++depth) {
            std::error_code cmake_error;
            std::error_code tools_error;
            const bool has_cmake = std::filesystem::is_regular_file(
                candidate / "CMakeLists.txt", cmake_error
            );
            const bool has_autochart_tools = std::filesystem::is_directory(
                candidate / "tools/autochart", tools_error
            );
            if (!cmake_error && !tools_error && has_cmake && has_autochart_tools) {
                return candidate / "tools/autochart/cache";
            }
            const auto parent = candidate.parent_path();
            if (parent.empty() || parent == candidate) break;
            candidate = parent;
        }
    }

    if (!exe_dir.empty()) {
        std::error_code bundled_error;
        const auto bundled_tools = exe_dir / "tools/autochart";
        if (std::filesystem::is_directory(bundled_tools, bundled_error)
            && !bundled_error) {
            return bundled_tools / "cache";
        }
    }
    return std::filesystem::path{"tools/autochart/cache"};
}


[[nodiscard]] std::filesystem::path resolve_analysis_cache_root(
    const AutoChartOptions& options
) {
    if (!options.analysis_cache_root.empty()) {
        return options.analysis_cache_root;
    }
    const auto exe_dir = executable_directory();
    if (!exe_dir.empty()) {
        return exe_dir / "cache/autochart-dsp";
    }
    return std::filesystem::path{"cache/autochart-dsp"};
}

[[nodiscard]] std::uint64_t fnv1a64(const std::string_view text) noexcept {
    std::uint64_t value = 1469598103934665603ULL;
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        value ^= static_cast<std::uint64_t>(byte);
        value *= 1099511628211ULL;
    }
    return value;
}

struct FeatureCacheIdentity final {
    std::uint64_t source_size{};
    std::int64_t source_mtime{};
    std::uint64_t path_hash{};
    std::uint64_t content_sample_hash{};
};

[[nodiscard]] std::optional<std::uint64_t> sampled_media_hash(
    const std::filesystem::path& media,
    const std::uint64_t size
) {
    // Cache validation should stay much cheaper than decoding a song, but file
    // size + mtime alone can be spoofed or preserved by copy tools. Hash three
    // bounded 64 KiB samples (head/middle/tail) so a stale feature cache is
    // rejected even when metadata happens to match.
    constexpr std::size_t sample_bytes = 64U * 1024U;
    std::ifstream input(media, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix_bytes = [&](const char* data, const std::size_t count) {
        for (std::size_t index = 0U; index < count; ++index) {
            hash ^= static_cast<std::uint64_t>(
                static_cast<unsigned char>(data[index])
            );
            hash *= 1099511628211ULL;
        }
    };
    const auto mix_position = [&](const std::uint64_t position) {
        for (unsigned shift = 0U; shift < 64U; shift += 8U) {
            hash ^= (position >> shift) & 0xFFULL;
            hash *= 1099511628211ULL;
        }
    };

    std::array<char, sample_bytes> buffer{};
    std::array<std::uint64_t, 3U> positions{
        0U,
        size > sample_bytes
            ? (size - static_cast<std::uint64_t>(sample_bytes)) / 2U
            : 0U,
        size > sample_bytes
            ? size - static_cast<std::uint64_t>(sample_bytes)
            : 0U,
    };
    std::sort(positions.begin(), positions.end());
    const auto unique_end = std::unique(positions.begin(), positions.end());
    for (auto iterator = positions.begin(); iterator != unique_end; ++iterator) {
        input.clear();
        input.seekg(static_cast<std::streamoff>(*iterator), std::ios::beg);
        if (!input) {
            return std::nullopt;
        }
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count < 0) {
            return std::nullopt;
        }
        mix_position(*iterator);
        mix_bytes(buffer.data(), static_cast<std::size_t>(count));
    }
    // Mix the total length as well so identical sampled bytes at different file
    // lengths cannot share the same content-sample identity.
    mix_position(size);
    return hash;
}

[[nodiscard]] std::optional<FeatureCacheIdentity> feature_cache_identity(
    const std::filesystem::path& media
) {
    std::error_code error;
    const auto size = std::filesystem::file_size(media, error);
    if (error || size > static_cast<std::uintmax_t>(
                          std::numeric_limits<std::uint64_t>::max())) {
        return std::nullopt;
    }
    const auto source_size = static_cast<std::uint64_t>(size);
    const auto mtime = std::filesystem::last_write_time(media, error);
    if (error) {
        return std::nullopt;
    }
    const auto sample_hash = sampled_media_hash(media, source_size);
    if (!sample_hash.has_value()) {
        return std::nullopt;
    }
    auto canonical = std::filesystem::weakly_canonical(media, error);
    if (error) {
        canonical = media.lexically_normal();
    }
    return FeatureCacheIdentity{
        source_size,
        static_cast<std::int64_t>(mtime.time_since_epoch().count()),
        fnv1a64(path_utf8(canonical)),
        *sample_hash,
    };
}

[[nodiscard]] std::uint64_t feature_cache_key(
    const FeatureCacheIdentity identity,
    const AnalysisParameters parameters,
    const AutoChartVideoMode video_mode
) noexcept {
    std::uint64_t value = identity.path_hash;
    const auto mix = [&](const std::uint64_t part) {
        value ^= part + 0x9E3779B97F4A7C15ULL + (value << 6U) + (value >> 2U);
    };
    mix(identity.source_size);
    mix(static_cast<std::uint64_t>(identity.source_mtime));
    mix(identity.content_sample_hash);
    mix(parameters.fft_size);
    mix(parameters.hop_size);
    mix(static_cast<std::uint64_t>(video_mode));
    mix(feature_cache_schema);
    return value;
}

[[nodiscard]] std::string hex_u64(const std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

template <typename T>
void write_binary(std::ostream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(T)));
}

template <typename T>
[[nodiscard]] bool read_binary(std::istream& input, T& value) {
    input.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    return static_cast<bool>(input);
}

[[nodiscard]] std::filesystem::path feature_cache_path(
    const std::filesystem::path& media,
    const AutoChartOptions& options,
    const AnalysisParameters parameters
) {
    const auto identity = feature_cache_identity(media);
    if (!identity.has_value()) {
        return {};
    }
    return resolve_analysis_cache_root(options)
        / (hex_u64(feature_cache_key(*identity, parameters, options.video_mode)) + ".pfaf");
}

[[nodiscard]] bool load_feature_cache(
    const std::filesystem::path& media,
    const AutoChartOptions& options,
    const AnalysisParameters parameters,
    std::vector<FeatureFrame>& frames,
    double& duration_seconds,
    bool& video_used
) {
    if (!options.analysis_cache) {
        return false;
    }
    const auto identity = feature_cache_identity(media);
    const auto path = feature_cache_path(media, options, parameters);
    if (!identity.has_value() || path.empty()) {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    std::array<char, 8U> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    const std::array<char, 8U> expected{'P','F','A','F','0','3','\r','\n'};
    std::uint32_t schema = 0U;
    std::uint32_t endian = 0U;
    std::uint64_t size = 0U;
    std::int64_t mtime = 0;
    std::uint64_t path_hash = 0U;
    std::uint64_t content_sample_hash = 0U;
    std::uint64_t frame_count = 0U;
    std::uint64_t fft_size = 0U;
    std::uint64_t hop_size = 0U;
    std::uint8_t cached_video_used = 0U;
    double cached_duration = 0.0;
    if (!input || magic != expected
        || !read_binary(input, schema)
        || !read_binary(input, endian)
        || !read_binary(input, size)
        || !read_binary(input, mtime)
        || !read_binary(input, path_hash)
        || !read_binary(input, content_sample_hash)
        || !read_binary(input, fft_size)
        || !read_binary(input, hop_size)
        || !read_binary(input, frame_count)
        || !read_binary(input, cached_duration)
        || !read_binary(input, cached_video_used)) {
        return false;
    }
    if (schema != feature_cache_schema || endian != 0x01020304U
        || size != identity->source_size || mtime != identity->source_mtime
        || path_hash != identity->path_hash
        || content_sample_hash != identity->content_sample_hash
        || fft_size != parameters.fft_size || hop_size != parameters.hop_size
        || frame_count == 0U || frame_count > maximum_cached_feature_frames
        || !std::isfinite(cached_duration) || cached_duration <= 0.0) {
        return false;
    }
    std::vector<FeatureFrame> loaded;
    loaded.resize(static_cast<std::size_t>(frame_count));
    for (auto& frame : loaded) {
        if (!read_binary(input, frame.time_ms)
            || !read_binary(input, frame.rms)
            || !read_binary(input, frame.flux)
            || !read_binary(input, frame.low_flux)
            || !read_binary(input, frame.mid_flux)
            || !read_binary(input, frame.high_flux)
            || !read_binary(input, frame.centroid_hz)
            || !read_binary(input, frame.peak_hz)
            || !read_binary(input, frame.onset)
            || !read_binary(input, frame.video_pulse)) {
            return false;
        }
    }
    if (input.peek() != std::char_traits<char>::eof()) {
        // Trailing bytes indicate a schema mismatch or interrupted rewrite.
        return false;
    }
    frames = std::move(loaded);
    duration_seconds = cached_duration;
    video_used = cached_video_used != 0U;
    return true;
}

void prune_feature_cache(
    const std::filesystem::path& root,
    const std::filesystem::path& keep,
    const std::uint64_t maximum_bytes
) noexcept {
    if (maximum_bytes == 0U) {
        return;
    }
    struct Entry final {
        std::filesystem::path path;
        std::filesystem::file_time_type modified;
        std::uint64_t bytes{};
    };
    std::vector<Entry> entries;
    std::uint64_t total{};
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(root, error), end;
         iterator != end && !error;
         iterator.increment(error)) {
        if (!iterator->is_regular_file(error) || error
            || iterator->path().extension() != ".pfaf") {
            error.clear();
            continue;
        }
        const auto size = iterator->file_size(error);
        if (error) {
            error.clear();
            continue;
        }
        const auto modified = iterator->last_write_time(error);
        if (error) {
            error.clear();
            continue;
        }
        const auto bounded = static_cast<std::uint64_t>((std::min)(
            size,
            static_cast<std::uintmax_t>(std::numeric_limits<std::uint64_t>::max())
        ));
        entries.push_back({iterator->path(), modified, bounded});
        total = bounded > std::numeric_limits<std::uint64_t>::max() - total
            ? std::numeric_limits<std::uint64_t>::max()
            : total + bounded;
    }
    std::ranges::sort(entries, [](const Entry& left, const Entry& right) {
        return left.modified < right.modified;
    });
    for (const auto& entry : entries) {
        if (total <= maximum_bytes) {
            break;
        }
        if (entry.path == keep) {
            continue;
        }
        std::filesystem::remove(entry.path, error);
        if (!error) {
            total = entry.bytes > total ? 0U : total - entry.bytes;
        }
        error.clear();
    }
}

void save_feature_cache(
    const std::filesystem::path& media,
    const AutoChartOptions& options,
    const AnalysisParameters parameters,
    const std::vector<FeatureFrame>& frames,
    const double duration_seconds,
    const bool video_used
) {
    if (!options.analysis_cache || frames.empty()
        || frames.size() > maximum_cached_feature_frames
        || !std::isfinite(duration_seconds) || duration_seconds <= 0.0) {
        return;
    }
    const auto identity = feature_cache_identity(media);
    const auto path = feature_cache_path(media, options, parameters);
    if (!identity.has_value() || path.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return;
    }
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        return;
    }
    const std::array<char, 8U> magic{'P','F','A','F','0','3','\r','\n'};
    output.write(magic.data(), static_cast<std::streamsize>(magic.size()));
    const std::uint32_t endian = 0x01020304U;
    const auto frame_count = static_cast<std::uint64_t>(frames.size());
    const auto fft_size = static_cast<std::uint64_t>(parameters.fft_size);
    const auto hop_size = static_cast<std::uint64_t>(parameters.hop_size);
    const std::uint8_t cached_video_used = video_used ? 1U : 0U;
    write_binary(output, feature_cache_schema);
    write_binary(output, endian);
    write_binary(output, identity->source_size);
    write_binary(output, identity->source_mtime);
    write_binary(output, identity->path_hash);
    write_binary(output, identity->content_sample_hash);
    write_binary(output, fft_size);
    write_binary(output, hop_size);
    write_binary(output, frame_count);
    write_binary(output, duration_seconds);
    write_binary(output, cached_video_used);
    for (const auto& frame : frames) {
        write_binary(output, frame.time_ms);
        write_binary(output, frame.rms);
        write_binary(output, frame.flux);
        write_binary(output, frame.low_flux);
        write_binary(output, frame.mid_flux);
        write_binary(output, frame.high_flux);
        write_binary(output, frame.centroid_hz);
        write_binary(output, frame.peak_hz);
        write_binary(output, frame.onset);
        write_binary(output, frame.video_pulse);
    }
    output.flush();
    if (!output) {
        output.close();
        std::filesystem::remove(temporary, error);
        return;
    }
    output.close();
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return;
    }
    // PULSEFORGE_P1_5_0E_BOUNDED_DSP_FEATURE_CACHE_V1
    prune_feature_cache(
        path.parent_path(),
        path,
        options.analysis_cache_max_bytes
    );
}

[[nodiscard]] CandidateSource ml_source_from_text(const std::string_view value) {
    if (value == "drums") {
        return CandidateSource::drums;
    }
    if (value == "bass") {
        return CandidateSource::bass;
    }
    if (value == "vocals") {
        return CandidateSource::vocals;
    }
    return CandidateSource::other;
}

[[nodiscard]] MlDrumRole ml_drum_role_from_text(const std::string_view value) {
    if (value == "kick") return MlDrumRole::kick;
    if (value == "snare") return MlDrumRole::snare;
    if (value == "tom") return MlDrumRole::tom;
    if (value == "hihat") return MlDrumRole::hihat;
    if (value == "cymbal") return MlDrumRole::cymbal;
    return MlDrumRole::other;
}

[[nodiscard]] double parse_decimal_or(
    const std::string_view text,
    const double fallback
) {
    std::string buffer(text);
    char* end = nullptr;
    const double value = std::strtod(buffer.c_str(), &end);
    if (end == nullptr || end != buffer.c_str() + buffer.size()
        || !std::isfinite(value)) {
        return fallback;
    }
    return value;
}

void load_ml_drum_events(
    const std::filesystem::path& path,
    MlAnalysis& analysis,
    const std::size_t maximum_events = 1'000'000U
) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return;
    }
    std::string line;
    static_cast<void>(std::getline(input, line));
    analysis.drum_events.reserve((std::min)(maximum_events, std::size_t{65'536U}));
    while (analysis.drum_events.size() < maximum_events && std::getline(input, line)) {
        if (line.size() > 256U) {
            continue;
        }
        std::array<std::string_view, 4U> fields{};
        std::size_t field = 0U;
        std::size_t begin = 0U;
        const std::string_view view(line);
        while (field < fields.size()) {
            const auto tab = view.find('\t', begin);
            const auto end = tab == std::string_view::npos ? view.size() : tab;
            fields[field++] = view.substr(begin, end - begin);
            if (tab == std::string_view::npos) {
                break;
            }
            begin = tab + 1U;
        }
        if (field != fields.size()) {
            continue;
        }
        const double time = parse_decimal_or(fields[0], -1.0);
        const double midi_number = parse_decimal_or(fields[1], -1.0);
        const double confidence = parse_decimal_or(fields[2], -1.0);
        if (time < 0.0 || midi_number < 0.0 || midi_number > 127.0
            || confidence < 0.0) {
            continue;
        }
        analysis.drum_events.push_back({
            time,
            static_cast<std::uint8_t>(std::llround(midi_number)),
            static_cast<float>(std::clamp(confidence, 0.0, 1.0)),
            ml_drum_role_from_text(fields[3]),
        });
    }
}

void load_ml_pitch_events(
    const std::filesystem::path& path,
    MlAnalysis& analysis,
    const std::size_t maximum_events = 1'000'000U
) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return;
    }
    std::string line;
    static_cast<void>(std::getline(input, line)); // header
    analysis.pitch_events.reserve((std::min)(maximum_events, std::size_t{65'536U}));
    while (analysis.pitch_events.size() < maximum_events
           && std::getline(input, line)) {
        if (line.size() > 512U) {
            continue;
        }
        std::array<std::string_view, 5U> fields{};
        std::size_t field = 0U;
        std::size_t begin = 0U;
        const std::string_view view(line);
        while (field < fields.size()) {
            const auto tab = view.find('\t', begin);
            const auto end = tab == std::string_view::npos ? view.size() : tab;
            fields[field++] = view.substr(begin, end - begin);
            if (tab == std::string_view::npos) {
                break;
            }
            begin = tab + 1U;
        }
        if (field != fields.size()) {
            continue;
        }
        const double start = parse_decimal_or(fields[0], -1.0);
        const double end = parse_decimal_or(fields[1], -1.0);
        const double midi_number = parse_decimal_or(fields[2], -1.0);
        const double confidence = parse_decimal_or(fields[3], -1.0);
        if (start < 0.0 || end <= start || midi_number < 0.0
            || midi_number > 127.0 || confidence < 0.0) {
            continue;
        }
        analysis.pitch_events.push_back({
            start,
            end,
            static_cast<std::uint8_t>(std::llround(midi_number)),
            static_cast<float>(std::clamp(confidence, 0.0, 1.0)),
            ml_source_from_text(fields[4]),
        });
    }
}

[[nodiscard]] MlVocalRole ml_vocal_role_from_text(
    const std::string_view value
) noexcept {
    return value == "syllable" ? MlVocalRole::syllable : MlVocalRole::phoneme;
}

void load_ml_vocal_events(
    const std::filesystem::path& path,
    MlAnalysis& analysis,
    const std::size_t maximum_events = 1'000'000U
) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return;
    }
    std::string line;
    static_cast<void>(std::getline(input, line)); // header
    analysis.vocal_events.reserve((std::min)(maximum_events, std::size_t{65'536U}));
    while (analysis.vocal_events.size() < maximum_events
           && std::getline(input, line)) {
        if (line.size() > 1'024U) {
            continue;
        }
        std::array<std::string_view, 5U> fields{};
        std::size_t field = 0U;
        std::size_t begin = 0U;
        const std::string_view view(line);
        while (field < fields.size()) {
            const auto tab = view.find('\t', begin);
            const auto finish = tab == std::string_view::npos ? view.size() : tab;
            fields[field++] = view.substr(begin, finish - begin);
            if (tab == std::string_view::npos) {
                break;
            }
            begin = tab + 1U;
        }
        if (field != fields.size()) {
            continue;
        }
        const double start = parse_decimal_or(fields[0], -1.0);
        const double finish = parse_decimal_or(fields[1], -1.0);
        const double confidence = parse_decimal_or(fields[2], -1.0);
        if (start < 0.0 || finish <= start || confidence < 0.0
            || fields[4].size() > 128U) {
            continue;
        }
        analysis.vocal_events.push_back({
            start,
            finish,
            static_cast<float>(std::clamp(confidence, 0.0, 1.0)),
            ml_vocal_role_from_text(fields[3]),
            std::string(fields[4]),
        });
    }
}

[[nodiscard]] MlAnalysis run_ml_analysis(
    const std::filesystem::path& media_path,
    const std::filesystem::path& ffmpeg,
    const AutoChartOptions& options,
    const TemporaryWorkspace& workspace
) {
    MlAnalysis analysis;
    const bool requested = options.ml_mode == AutoChartMlMode::on
        || (options.ml_mode == AutoChartMlMode::automatic
            && options.mode != AutoChartMode::fast);
    if (!requested) {
        return analysis;
    }

    const auto python = resolve_ml_python(options);
    const auto script = resolve_ml_backend_script(options);
    if (python.empty() || script.empty()) {
        const std::string message =
            "AutoChart ML environment is unavailable; run scripts/setup-autochart-ml.ps1";
        if (options.ml_mode == AutoChartMlMode::on) {
            throw std::runtime_error(message);
        }
        std::cout << "[AutoChart] " << message << "; using deterministic DSP fallback\n";
        return analysis;
    }

    const auto ml_workspace = workspace.path() / "ml";
    const auto result_path = workspace.path() / "ml-result.json";
    std::error_code error;
    std::filesystem::create_directories(ml_workspace, error);
    if (error) {
        if (options.ml_mode == AutoChartMlMode::on) {
            throw std::runtime_error("cannot create AutoChart ML workspace");
        }
        return analysis;
    }
    const auto cache_root = resolve_ml_cache_root(options);
    std::filesystem::create_directories(cache_root, error);
    if (error && options.ml_mode == AutoChartMlMode::on) {
        throw std::runtime_error("cannot create AutoChart ML cache directory");
    }

    std::vector<std::filesystem::path> arguments{
        script,
        "--input", media_path,
        "--output", result_path,
        "--workspace", ml_workspace,
        "--ffmpeg", ffmpeg,
        "--cache-root", cache_root,
        "--mode", std::string(to_string(options.mode)),
        "--device", options.ml_device,
        "--source-separation", options.ml_source_separation ? "1" : "0",
        "--beat-tracking", options.ml_beat_tracking ? "1" : "0",
        "--drum-transcription", options.ml_drum_transcription ? "1" : "0",
        "--pitch-transcription", options.ml_pitch_transcription ? "1" : "0",
        "--vocal-refinement", options.ml_vocal_refinement ? "1" : "0",
        "--cache", options.ml_cache ? "1" : "0",
        "--compact-models", options.compact_ml_models ? "1" : "0",
        "--run-cache-budget-mb", std::to_string(
            options.ml_run_cache_max_bytes / (1024ULL * 1024ULL)
        ),
    };
    std::cout << "[AutoChart] Running optional neural music analysis backend\n";
    const int exit_code = run_process(python, arguments);

    std::error_code size_error;
    const auto result_size = std::filesystem::file_size(result_path, size_error);
    if (size_error || result_size > 4U * 1024U * 1024U) {
        if (options.ml_mode == AutoChartMlMode::on) {
            throw std::runtime_error("AutoChart ML backend did not produce a bounded result file");
        }
        std::cout << "[AutoChart] ML backend unavailable; using DSP fallback\n";
        return analysis;
    }

    nlohmann::json root;
    try {
        std::ifstream input(result_path, std::ios::binary);
        input >> root;
    } catch (const std::exception& exception) {
        if (options.ml_mode == AutoChartMlMode::on) {
            throw std::runtime_error(
                std::string("cannot parse AutoChart ML result: ") + exception.what()
            );
        }
        return analysis;
    }
    if (exit_code != 0 || !root.value("ok", false)) {
        const auto message = root.value("error", std::string{"optional neural backend failed"});
        if (options.ml_mode == AutoChartMlMode::on) {
            throw std::runtime_error(message);
        }
        std::cout << "[AutoChart] " << message << "; using available DSP analysis\n";
        return analysis;
    }

    analysis.used = true;
    analysis.cache_hit = root.value("cacheHit", false);
    analysis.device = root.value("device", std::string{"cpu"});
    if (const auto stages = root.find("stages"); stages != root.end() && stages->is_object()) {
        analysis.source_separation_used = stages->value("sourceSeparation", false);
        analysis.beat_tracking_used = stages->value("beatTracking", false);
        analysis.drum_transcription_used = stages->value("drumTranscription", false);
        analysis.pitch_transcription_used = stages->value("pitchTranscription", false);
        analysis.vocal_refinement_used = stages->value("vocalRefinement", false);
    }
    if (const auto stems = root.find("stems"); stems != root.end() && stems->is_object()) {
        const auto path_or_empty = [&](const char* key) -> std::filesystem::path {
            const auto iterator = stems->find(key);
            return iterator != stems->end() && iterator->is_string()
                ? std::filesystem::path(iterator->get<std::string>())
                : std::filesystem::path{};
        };
        analysis.drums = path_or_empty("drums");
        analysis.bass = path_or_empty("bass");
        analysis.vocals = path_or_empty("vocals");
        analysis.other = path_or_empty("other");
    }
    const auto load_times = [&](const char* key, std::vector<double>& output) {
        const auto iterator = root.find(key);
        if (iterator == root.end() || !iterator->is_array()) {
            return;
        }
        output.reserve((std::min)(iterator->size(), std::size_t{1'000'000U}));
        for (const auto& item : *iterator) {
            if (output.size() >= 1'000'000U || !item.is_number()) {
                break;
            }
            const double seconds = item.get<double>();
            if (std::isfinite(seconds) && seconds >= 0.0) {
                output.push_back(seconds * 1'000.0);
            }
        }
    };
    load_times("beatsSeconds", analysis.beats_ms);
    load_times("downbeatsSeconds", analysis.downbeats_ms);

    if (analysis.drum_transcription_used) {
        const auto iterator = root.find("drumEventsPath");
        if (iterator != root.end() && iterator->is_string()) {
            load_ml_drum_events(iterator->get<std::string>(), analysis);
            analysis.drum_transcription_used = !analysis.drum_events.empty();
        } else {
            analysis.drum_transcription_used = false;
        }
    }

    if (analysis.pitch_transcription_used) {
        const auto iterator = root.find("pitchEventsPath");
        if (iterator != root.end() && iterator->is_string()) {
            load_ml_pitch_events(iterator->get<std::string>(), analysis);
            analysis.pitch_transcription_used = !analysis.pitch_events.empty();
        } else {
            analysis.pitch_transcription_used = false;
        }
    }

    if (analysis.vocal_refinement_used) {
        const auto iterator = root.find("vocalEventsPath");
        if (iterator != root.end() && iterator->is_string()) {
            load_ml_vocal_events(iterator->get<std::string>(), analysis);
            analysis.vocal_refinement_used = std::any_of(
                analysis.vocal_events.begin(),
                analysis.vocal_events.end(),
                [](const MlVocalEvent& event) {
                    return event.role == MlVocalRole::syllable;
                }
            );
        } else {
            analysis.vocal_refinement_used = false;
        }
    }
    if (const auto diagnostics = root.find("diagnostics");
        diagnostics != root.end() && diagnostics->is_array()) {
        for (const auto& item : *diagnostics) {
            if (analysis.diagnostics.size() >= 64U) {
                break;
            }
            if (item.is_string()) {
                analysis.diagnostics.push_back(item.get<std::string>());
            }
        }
    }
    std::cout << "[AutoChart] ML stages: Demucs="
              << (analysis.source_separation_used ? "yes" : "no")
              << ", BeatThis=" << (analysis.beat_tracking_used ? "yes" : "no")
              << ", ADTOF=" << (analysis.drum_transcription_used ? "yes" : "no")
              << ", BasicPitch=" << (analysis.pitch_transcription_used ? "yes" : "no")
              << ", VocalPhonemes=" << (analysis.vocal_refinement_used ? "yes" : "no")
              << ", device=" << analysis.device
              << (analysis.cache_hit ? " (cache hit)" : "") << "\n";
    return analysis;
}

[[nodiscard]] bool run_ffmpeg(
    const std::filesystem::path& ffmpeg,
    const std::vector<std::filesystem::path>& arguments
) {
    return run_process(ffmpeg, arguments) == 0;
}

[[nodiscard]] bool decode_audio_to_f32(
    const std::filesystem::path& ffmpeg,
    const std::filesystem::path& input,
    const std::filesystem::path& output
) {
    return run_ffmpeg(ffmpeg, {
        "-nostdin",
        "-hide_banner",
        "-loglevel", "error",
        "-y",
        "-i", input,
        "-map", "0:a:0",
        "-vn",
        "-ac", "1",
        "-ar", std::to_string(analysis_sample_rate),
        "-f", "f32le",
        "-acodec", "pcm_f32le",
        output,
    });
}

[[nodiscard]] bool extract_video_luma(
    const std::filesystem::path& ffmpeg,
    const std::filesystem::path& input,
    const std::filesystem::path& output
) {
    return run_ffmpeg(ffmpeg, {
        "-nostdin",
        "-hide_banner",
        "-loglevel", "error",
        "-y",
        "-i", input,
        "-map", "0:v:0",
        "-an",
        "-vf", "fps=12,scale=64:36:flags=area,format=gray",
        "-f", "rawvideo",
        "-pix_fmt", "gray",
        output,
    });
}

[[nodiscard]] bool runtime_audio_extension(const std::filesystem::path& input) {
    const auto extension = lower_ascii(path_utf8(input.extension()));
    return extension == ".ogg" || extension == ".wav"
        || extension == ".mp3" || extension == ".flac";
}

[[nodiscard]] std::filesystem::path materialize_mod_audio(
    const std::filesystem::path& ffmpeg,
    const std::filesystem::path& input,
    const std::filesystem::path& mod_root
) {
    std::error_code error;
    if (runtime_audio_extension(input)) {
        const auto destination = mod_root / ("Inst" + path_utf8(input.extension()));
        std::filesystem::copy_file(
            input,
            destination,
            std::filesystem::copy_options::overwrite_existing,
            error
        );
        if (!error) {
            return destination;
        }
    }

    const auto ogg = mod_root / "Inst.ogg";
    if (run_ffmpeg(ffmpeg, {
            "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
            "-i", input, "-map", "0:a:0", "-vn",
            "-c:a", "libvorbis", "-q:a", "5", ogg,
        })) {
        return ogg;
    }

    const auto wav = mod_root / "Inst.wav";
    if (run_ffmpeg(ffmpeg, {
            "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
            "-i", input, "-map", "0:a:0", "-vn",
            "-c:a", "pcm_s16le", "-ar", "48000", "-ac", "2",
            wav,
        })) {
        return wav;
    }
    throw std::runtime_error("FFmpeg could not create the runtime audio track");
}

void fft_in_place(std::vector<std::complex<float>>& values) {
    const std::size_t n = values.size();
    for (std::size_t i = 1U, j = 0U; i < n; ++i) {
        std::size_t bit = n >> 1U;
        for (; (j & bit) != 0U; bit >>= 1U) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(values[i], values[j]);
        }
    }
    for (std::size_t length = 2U; length <= n; length <<= 1U) {
        const float angle = static_cast<float>(-2.0 * pi)
            / static_cast<float>(length);
        const std::complex<float> root{std::cos(angle), std::sin(angle)};
        for (std::size_t start = 0U; start < n; start += length) {
            std::complex<float> weight{1.0F, 0.0F};
            const std::size_t half = length >> 1U;
            for (std::size_t offset = 0U; offset < half; ++offset) {
                const auto even = values[start + offset];
                const auto odd = values[start + offset + half] * weight;
                values[start + offset] = even + odd;
                values[start + offset + half] = even - odd;
                weight *= root;
            }
        }
    }
}

[[nodiscard]] std::vector<FeatureFrame> extract_audio_features(
    const std::filesystem::path& raw_pcm,
    const AnalysisParameters parameters,
    double& duration_seconds
) {
    std::error_code size_error;
    const auto bytes = std::filesystem::file_size(raw_pcm, size_error);
    if (size_error || bytes < sizeof(float) * parameters.fft_size
        || bytes % sizeof(float) != 0U) {
        throw std::runtime_error("decoded audio is empty or malformed");
    }
    const std::uint64_t sample_count = bytes / sizeof(float);
    duration_seconds = static_cast<double>(sample_count)
        / static_cast<double>(analysis_sample_rate);

    std::ifstream input(raw_pcm, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open decoded audio for analysis");
    }

    std::vector<float> hann(parameters.fft_size);
    for (std::size_t index = 0U; index < hann.size(); ++index) {
        hann[index] = static_cast<float>(0.5 - 0.5 * std::cos(
            2.0 * pi * static_cast<double>(index)
            / static_cast<double>(hann.size() - 1U)
        ));
    }
    std::vector<std::complex<float>> spectrum(parameters.fft_size);
    std::vector<float> previous_magnitude(parameters.fft_size / 2U + 1U, 0.0F);
    std::vector<float> buffer;
    buffer.reserve(decoded_read_samples + parameters.fft_size * 2U);
    std::vector<float> read_buffer(decoded_read_samples);
    std::vector<FeatureFrame> frames;
    frames.reserve(static_cast<std::size_t>(
        sample_count / static_cast<std::uint64_t>(parameters.hop_size) + 1U
    ));

    std::size_t offset = 0U;
    std::uint64_t frame_index = 0U;
    bool have_previous = false;
    while (true) {
        input.read(
            reinterpret_cast<char*>(read_buffer.data()),
            static_cast<std::streamsize>(read_buffer.size() * sizeof(float))
        );
        const auto read_bytes = input.gcount();
        if (read_bytes > 0) {
            const auto count = static_cast<std::size_t>(read_bytes)
                / sizeof(float);
            buffer.insert(
                buffer.end(),
                read_buffer.begin(),
                read_buffer.begin() + static_cast<std::ptrdiff_t>(count)
            );
        }
        while (buffer.size() - offset >= parameters.fft_size) {
            double rms_sum = 0.0;
            for (std::size_t index = 0U; index < parameters.fft_size; ++index) {
                const float sample = buffer[offset + index];
                rms_sum += static_cast<double>(sample) * static_cast<double>(sample);
                spectrum[index] = std::complex<float>{sample * hann[index], 0.0F};
            }
            fft_in_place(spectrum);

            float flux = 0.0F;
            float low_flux = 0.0F;
            float mid_flux = 0.0F;
            float high_flux = 0.0F;
            double weighted_frequency = 0.0;
            double magnitude_sum = 0.0;
            float peak_magnitude = 0.0F;
            float peak_hz = 0.0F;
            const auto bins = parameters.fft_size / 2U + 1U;
            for (std::size_t bin = 1U; bin < bins; ++bin) {
                const float magnitude = std::abs(spectrum[bin]);
                const float difference = have_previous
                    ? (std::max)(0.0F, magnitude - previous_magnitude[bin])
                    : 0.0F;
                previous_magnitude[bin] = magnitude;
                const double frequency = static_cast<double>(bin)
                    * static_cast<double>(analysis_sample_rate)
                    / static_cast<double>(parameters.fft_size);
                flux += difference;
                if (frequency >= 35.0 && frequency < 180.0) {
                    low_flux += difference;
                } else if (frequency < 2'500.0) {
                    mid_flux += difference;
                } else if (frequency < 12'000.0) {
                    high_flux += difference;
                }
                magnitude_sum += magnitude;
                weighted_frequency += frequency * static_cast<double>(magnitude);
                if (frequency >= 65.0 && frequency <= 4'000.0
                    && magnitude > peak_magnitude) {
                    peak_magnitude = magnitude;
                    peak_hz = static_cast<float>(frequency);
                }
            }
            have_previous = true;
            const float rms = static_cast<float>(std::sqrt(
                rms_sum / static_cast<double>(parameters.fft_size)
            ));
            const float centroid = magnitude_sum > 1.0e-12
                ? static_cast<float>(weighted_frequency / magnitude_sum)
                : 0.0F;
            frames.push_back({
                static_cast<double>(frame_index * parameters.hop_size)
                    * 1'000.0 / static_cast<double>(analysis_sample_rate),
                rms,
                flux,
                low_flux,
                mid_flux,
                high_flux,
                centroid,
                peak_hz,
                0.0F,
                0.0F,
            });
            ++frame_index;
            offset += parameters.hop_size;
        }
        if (offset > decoded_read_samples) {
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(offset));
            offset = 0U;
        }
        if (input.bad()) {
            throw std::runtime_error("I/O error while reading decoded audio");
        }
        if (input.eof()) {
            break;
        }
        if (input.fail() && read_bytes == 0) {
            break;
        }
    }
    if (frames.size() < 8U) {
        throw std::runtime_error("audio is too short for AutoChart analysis");
    }
    return frames;
}

void calculate_onset_envelope(
    std::vector<FeatureFrame>& frames,
    const AnalysisParameters parameters
) {
    std::vector<double> prefix(frames.size() + 1U, 0.0);
    std::vector<double> prefix_squared(frames.size() + 1U, 0.0);
    for (std::size_t index = 0U; index < frames.size(); ++index) {
        const double value = std::log1p(static_cast<double>(frames[index].flux));
        prefix[index + 1U] = prefix[index] + value;
        prefix_squared[index + 1U] = prefix_squared[index] + value * value;
    }
    const double frame_seconds = static_cast<double>(parameters.hop_size)
        / static_cast<double>(analysis_sample_rate);
    const std::size_t radius = (std::max)(
        std::size_t{4U},
        static_cast<std::size_t>(std::llround(0.45 / frame_seconds))
    );
    std::vector<float> raw(frames.size(), 0.0F);
    for (std::size_t index = 0U; index < frames.size(); ++index) {
        const std::size_t begin = index > radius ? index - radius : 0U;
        const std::size_t end = (std::min)(frames.size(), index + radius + 1U);
        const double count = static_cast<double>(end - begin);
        const double sum = prefix[end] - prefix[begin];
        const double squared = prefix_squared[end] - prefix_squared[begin];
        const double mean = sum / count;
        const double variance = (std::max)(0.0, squared / count - mean * mean);
        const double deviation = std::sqrt(variance) + 1.0e-6;
        const double value = std::log1p(static_cast<double>(frames[index].flux));
        const double z = (value - mean) / deviation;
        raw[index] = static_cast<float>(std::clamp(
            (z - 0.35) / 3.5,
            0.0,
            1.0
        ));
    }
    for (std::size_t index = 2U; index + 2U < frames.size(); ++index) {
        const float value = raw[index];
        const bool local_maximum = value >= raw[index - 1U]
            && value >= raw[index - 2U]
            && value >= raw[index + 1U]
            && value >= raw[index + 2U];
        frames[index].onset = local_maximum && value >= parameters.onset_floor
            ? value
            : value * 0.25F;
        if (local_maximum && value >= parameters.onset_floor) {
            // PULSEFORGE_P1_5_0E_PARABOLIC_ONSET_REFINEMENT_V1
            // Three-point interpolation estimates the peak between FFT hops.
            // It improves timing without a second decoded audio file or model.
            const double left = raw[index - 1U];
            const double center = raw[index];
            const double right = raw[index + 1U];
            const double denominator = left - 2.0 * center + right;
            if (std::abs(denominator) > 1.0e-9) {
                const double delta = std::clamp(
                    0.5 * (left - right) / denominator,
                    -0.5,
                    0.5
                );
                frames[index].time_ms += delta
                    * static_cast<double>(parameters.hop_size)
                    / static_cast<double>(analysis_sample_rate)
                    * 1'000.0;
            }
        }
    }
}

[[nodiscard]] std::vector<float> analyze_video_pulses(
    const std::filesystem::path& raw_video
) {
    std::error_code size_error;
    const auto bytes = std::filesystem::file_size(raw_video, size_error);
    constexpr std::size_t frame_bytes = video_width * video_height;
    if (size_error || bytes < frame_bytes * 2U) {
        return {};
    }
    std::ifstream input(raw_video, std::ios::binary);
    if (!input) {
        return {};
    }
    std::vector<unsigned char> previous(frame_bytes, 0U);
    std::vector<unsigned char> current(frame_bytes, 0U);
    std::vector<float> differences;
    bool first = true;
    while (input.read(
        reinterpret_cast<char*>(current.data()),
        static_cast<std::streamsize>(current.size())
    )) {
        if (first) {
            previous = current;
            first = false;
            differences.push_back(0.0F);
            continue;
        }
        double sum = 0.0;
        for (std::size_t index = 0U; index < frame_bytes; ++index) {
            sum += std::abs(
                static_cast<int>(current[index]) - static_cast<int>(previous[index])
            );
        }
        differences.push_back(static_cast<float>(sum / static_cast<double>(frame_bytes)));
        previous.swap(current);
    }
    if (differences.size() < 3U) {
        return {};
    }
    const double mean = std::accumulate(
        differences.begin(), differences.end(), 0.0
    ) / static_cast<double>(differences.size());
    double variance = 0.0;
    for (const float value : differences) {
        const double difference = static_cast<double>(value) - mean;
        variance += difference * difference;
    }
    variance /= static_cast<double>(differences.size());
    const double deviation = std::sqrt(variance) + 1.0e-6;
    for (auto& value : differences) {
        const double z = (static_cast<double>(value) - mean) / deviation;
        value = static_cast<float>(std::clamp((z - 0.4) / 3.0, 0.0, 1.0));
    }
    return differences;
}

void merge_video_pulses(
    std::vector<FeatureFrame>& frames,
    const std::vector<float>& video
) {
    if (video.empty()) {
        return;
    }
    for (auto& frame : frames) {
        const auto index = static_cast<std::size_t>(std::llround(
            frame.time_ms * video_fps / 1'000.0
        ));
        if (index >= video.size()) {
            continue;
        }
        frame.video_pulse = video[index];
        // Visualizers are corroborating evidence, never the primary source.
        frame.onset = std::clamp(
            frame.onset * 0.88F + video[index] * 0.12F,
            0.0F,
            1.0F
        );
    }
}

[[nodiscard]] double autocorrelation_at(
    const std::vector<float>& envelope,
    const std::size_t lag
) {
    if (lag == 0U || lag >= envelope.size()) {
        return 0.0;
    }
    double numerator = 0.0;
    double left_energy = 0.0;
    double right_energy = 0.0;
    for (std::size_t index = lag; index < envelope.size(); ++index) {
        const double left = envelope[index];
        const double right = envelope[index - lag];
        numerator += left * right;
        left_energy += left * left;
        right_energy += right * right;
    }
    const double denominator = std::sqrt(left_energy * right_energy) + 1.0e-12;
    return numerator / denominator;
}

[[nodiscard]] BeatGrid detect_beat_grid(
    const std::vector<FeatureFrame>& frames,
    const AnalysisParameters parameters,
    const double duration_seconds,
    const bool variable_tempo
) {
    std::vector<float> envelope;
    envelope.reserve(frames.size());
    for (const auto& frame : frames) {
        envelope.push_back(frame.onset);
    }
    const double frame_ms = static_cast<double>(parameters.hop_size)
        * 1'000.0 / static_cast<double>(analysis_sample_rate);
    const auto lag_for_bpm = [&](const double bpm) {
        return static_cast<std::size_t>(std::llround(60'000.0 / (bpm * frame_ms)));
    };
    const std::size_t minimum_lag = (std::max)(std::size_t{2U}, lag_for_bpm(240.0));
    const std::size_t maximum_lag = (std::min)(
        frames.size() / 2U,
        lag_for_bpm(55.0)
    );
    if (minimum_lag >= maximum_lag) {
        return {};
    }

    double best_score = -1.0;
    double second_score = -1.0;
    std::size_t best_lag = minimum_lag;
    for (std::size_t lag = minimum_lag; lag <= maximum_lag; ++lag) {
        const double bpm = 60'000.0 / (frame_ms * static_cast<double>(lag));
        const double base = autocorrelation_at(envelope, lag);
        double harmonic = base;
        if (lag * 2U <= maximum_lag) {
            harmonic += 0.30 * autocorrelation_at(envelope, lag * 2U);
        }
        if (lag / 2U >= minimum_lag) {
            harmonic += 0.12 * autocorrelation_at(envelope, lag / 2U);
        }
        const double octave_distance = std::abs(std::log2(bpm / 120.0));
        const double tempo_prior = std::exp(-0.5 * octave_distance * octave_distance / 1.44);
        const double score = harmonic * (0.82 + 0.18 * tempo_prior);
        if (score > best_score) {
            second_score = best_score;
            best_score = score;
            best_lag = lag;
        } else if (score > second_score) {
            second_score = score;
        }
    }

    BeatGrid grid;
    grid.bpm = 60'000.0 / (frame_ms * static_cast<double>(best_lag));
    grid.period_ms = 60'000.0 / grid.bpm;
    const double contrast = best_score > 1.0e-9
        ? (best_score - (std::max)(0.0, second_score)) / best_score
        : 0.0;
    grid.confidence = std::clamp(0.55 * best_score + 0.45 * contrast, 0.0, 1.0);

    double best_phase_score = -1.0;
    std::size_t best_phase = 0U;
    const std::size_t phase_frames = (std::min)(
        envelope.size(),
        static_cast<std::size_t>(std::llround(75'000.0 / frame_ms))
    );
    for (std::size_t phase = 0U; phase < best_lag; ++phase) {
        double score = 0.0;
        std::size_t count = 0U;
        for (std::size_t index = phase; index < phase_frames; index += best_lag) {
            score += static_cast<double>(envelope[index]);
            ++count;
        }
        if (count != 0U) {
            score /= static_cast<double>(count);
        }
        if (score > best_phase_score) {
            best_phase_score = score;
            best_phase = phase;
        }
    }
    grid.offset_ms = static_cast<double>(best_phase) * frame_ms;

    const double duration_ms = duration_seconds * 1'000.0;
    for (double expected = grid.offset_ms; expected <= duration_ms;
         expected += grid.period_ms) {
        const double search_radius = (std::min)(50.0, grid.period_ms * 0.14);
        const auto center = static_cast<std::int64_t>(std::llround(expected / frame_ms));
        const auto radius = static_cast<std::int64_t>(std::ceil(search_radius / frame_ms));
        float strongest = 0.0F;
        std::int64_t strongest_index = center;
        const auto begin = (std::max)(std::int64_t{0}, center - radius);
        const auto end = (std::min)(
            static_cast<std::int64_t>(envelope.size()) - 1,
            center + radius
        );
        for (auto index = begin; index <= end; ++index) {
            if (envelope[static_cast<std::size_t>(index)] > strongest) {
                strongest = envelope[static_cast<std::size_t>(index)];
                strongest_index = index;
            }
        }
        double beat = expected;
        if (strongest >= 0.30F) {
            beat = static_cast<double>(strongest_index) * frame_ms;
        }
        if (!grid.beats.empty() && beat <= grid.beats.back() + grid.period_ms * 0.55) {
            beat = expected;
        }
        grid.beats.push_back(beat);
    }
    if (grid.beats.empty()) {
        grid.beats.push_back(0.0);
    }

    // The native tracker has no trained downbeat head. Infer the 4/4 phase
    // that carries the strongest low-frequency/onset accents instead of simply
    // assuming that the first detected beat is beat one of the bar.
    std::size_t downbeat_phase = 0U;
    double best_downbeat_score = -1.0;
    for (std::size_t phase = 0U; phase < 4U; ++phase) {
        double phase_score = 0.0;
        std::size_t phase_count = 0U;
        for (std::size_t beat_index = phase;
             beat_index < grid.beats.size();
             beat_index += 4U) {
            const auto frame_index = static_cast<std::size_t>(std::clamp(
                std::llround(grid.beats[beat_index] / frame_ms),
                0LL,
                static_cast<long long>(frames.size() - 1U)
            ));
            const auto& beat_frame = frames[frame_index];
            const float band_total = beat_frame.low_flux + beat_frame.mid_flux
                + beat_frame.high_flux + 1.0e-9F;
            const double low_share = static_cast<double>(
                beat_frame.low_flux / band_total
            );
            phase_score += static_cast<double>(beat_frame.onset) * 0.72
                + low_share * 0.28;
            ++phase_count;
        }
        if (phase_count != 0U) {
            phase_score /= static_cast<double>(phase_count);
        }
        if (phase_score > best_downbeat_score) {
            best_downbeat_score = phase_score;
            downbeat_phase = phase;
        }
    }
    for (std::size_t index = downbeat_phase;
         index < grid.beats.size();
         index += 4U) {
        grid.downbeats.push_back(grid.beats[index]);
    }

    grid.tempos.push_back({0.0, grid.bpm, 4U, 4U});
    if (variable_tempo && grid.beats.size() >= 32U) {
        double current_bpm = grid.bpm;
        constexpr std::size_t window_beats = 8U;
        for (std::size_t start = window_beats;
             start + window_beats * 2U < grid.beats.size();
             start += window_beats) {
            const auto local_bpm = [&](const std::size_t begin) {
                std::vector<double> intervals;
                intervals.reserve(window_beats - 1U);
                for (std::size_t i = begin + 1U; i < begin + window_beats; ++i) {
                    const double interval = grid.beats[i] - grid.beats[i - 1U];
                    if (interval > 1.0) {
                        intervals.push_back(interval);
                    }
                }
                if (intervals.empty()) {
                    return current_bpm;
                }
                const auto middle = intervals.begin()
                    + static_cast<std::ptrdiff_t>(intervals.size() / 2U);
                std::nth_element(intervals.begin(), middle, intervals.end());
                return 60'000.0 / *middle;
            };
            const double first = local_bpm(start);
            const double second = local_bpm(start + window_beats);
            const double relative_change = std::abs(first - current_bpm)
                / (std::max)(1.0, current_bpm);
            const double persistence = std::abs(first - second)
                / (std::max)(1.0, first);
            if (std::isfinite(first) && first > 20.0 && first < 600.0
                && relative_change >= 0.04 && persistence <= 0.025) {
                current_bpm = (first + second) * 0.5;
                grid.tempos.push_back({
                    grid.beats[start], current_bpm, 4U, 4U,
                });
            }
        }
    }
    return grid;
}

[[nodiscard]] double median_value(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    const auto middle = values.begin()
        + static_cast<std::ptrdiff_t>(values.size() / 2U);
    std::nth_element(values.begin(), middle, values.end());
    double result = *middle;
    if (values.size() % 2U == 0U) {
        const auto lower = (std::max_element)(values.begin(), middle);
        result = (*lower + result) * 0.5;
    }
    return result;
}

[[nodiscard]] std::optional<BeatGrid> beat_grid_from_ml(
    const MlAnalysis& analysis,
    const double duration_seconds,
    const bool variable_tempo
) {
    if (!analysis.beat_tracking_used || analysis.beats_ms.size() < 4U) {
        return std::nullopt;
    }
    std::vector<double> intervals;
    intervals.reserve(analysis.beats_ms.size() - 1U);
    for (std::size_t index = 1U; index < analysis.beats_ms.size(); ++index) {
        const double interval = analysis.beats_ms[index] - analysis.beats_ms[index - 1U];
        if (std::isfinite(interval) && interval >= 120.0 && interval <= 2'000.0) {
            intervals.push_back(interval);
        }
    }
    if (intervals.size() < 3U) {
        return std::nullopt;
    }
    const double period = median_value(intervals);
    if (!std::isfinite(period) || period <= 0.0) {
        return std::nullopt;
    }
    std::vector<double> deviations;
    deviations.reserve(intervals.size());
    for (const double interval : intervals) {
        deviations.push_back(std::abs(interval - period));
    }
    const double mad = median_value(std::move(deviations));
    const double bpm = 60'000.0 / period;
    if (!std::isfinite(bpm) || bpm < 30.0 || bpm > 500.0) {
        return std::nullopt;
    }

    BeatGrid grid;
    grid.bpm = bpm;
    grid.period_ms = period;
    grid.beats = analysis.beats_ms;
    const double duration_ms = duration_seconds * 1'000.0;
    grid.beats.erase(
        std::remove_if(
            grid.beats.begin(), grid.beats.end(),
            [duration_ms](const double value) {
                return !std::isfinite(value) || value < 0.0
                    || value > duration_ms + 5'000.0;
            }
        ),
        grid.beats.end()
    );
    if (grid.beats.size() < 4U) {
        return std::nullopt;
    }
    grid.downbeats = analysis.downbeats_ms;
    grid.downbeats.erase(
        std::remove_if(
            grid.downbeats.begin(), grid.downbeats.end(),
            [duration_ms](const double value) {
                return !std::isfinite(value) || value < 0.0
                    || value > duration_ms + 5'000.0;
            }
        ),
        grid.downbeats.end()
    );
    std::sort(grid.downbeats.begin(), grid.downbeats.end());
    grid.downbeats.erase(
        std::unique(
            grid.downbeats.begin(), grid.downbeats.end(),
            [](const double left, const double right) {
                return std::abs(left - right) < 4.0;
            }
        ),
        grid.downbeats.end()
    );
    if (grid.downbeats.empty()) {
        for (std::size_t index = 0U; index < grid.beats.size(); index += 4U) {
            grid.downbeats.push_back(grid.beats[index]);
        }
    }
    grid.offset_ms = !grid.downbeats.empty()
        ? grid.downbeats.front()
        : grid.beats.front();
    const double stability = 1.0 - std::clamp(mad / period * 8.0, 0.0, 1.0);
    const double coverage = std::clamp(
        static_cast<double>(grid.beats.size()) * period
            / (std::max)(1.0, duration_ms),
        0.0,
        1.0
    );
    grid.confidence = std::clamp(0.72 + stability * 0.20 + coverage * 0.08, 0.0, 1.0);
    grid.tempos.push_back({0.0, bpm, 4U, 4U});

    if (variable_tempo && grid.beats.size() >= 24U) {
        constexpr std::size_t window_beats = 8U;
        double current_bpm = bpm;
        for (std::size_t start = window_beats;
             start + window_beats * 2U < grid.beats.size();
             start += window_beats) {
            const auto window_bpm = [&](const std::size_t begin) {
                std::vector<double> local;
                local.reserve(window_beats - 1U);
                for (std::size_t index = begin + 1U;
                     index < begin + window_beats;
                     ++index) {
                    const double interval = grid.beats[index] - grid.beats[index - 1U];
                    if (interval >= 120.0 && interval <= 2'000.0) {
                        local.push_back(interval);
                    }
                }
                const double local_period = median_value(std::move(local));
                return local_period > 0.0 ? 60'000.0 / local_period : current_bpm;
            };
            const double first = window_bpm(start);
            const double second = window_bpm(start + window_beats);
            const double change = std::abs(first - current_bpm)
                / (std::max)(1.0, current_bpm);
            const double persistence = std::abs(first - second)
                / (std::max)(1.0, first);
            if (std::isfinite(first) && first > 20.0 && first < 600.0
                && change >= 0.035 && persistence <= 0.02) {
                current_bpm = (first + second) * 0.5;
                grid.tempos.push_back({grid.beats[start], current_bpm, 4U, 4U});
            }
        }
    }
    return grid;
}


[[nodiscard]] double tempo_octave_agreement(
    const double first_bpm,
    const double second_bpm
) noexcept {
    if (!std::isfinite(first_bpm) || !std::isfinite(second_bpm)
        || first_bpm <= 0.0 || second_bpm <= 0.0) {
        return 0.0;
    }
    const auto relative = [](const double left, const double right) {
        return std::abs(left - right) / (std::max)(1.0, right);
    };
    const double error = (std::min)({
        relative(first_bpm, second_bpm),
        relative(first_bpm * 2.0, second_bpm),
        relative(first_bpm, second_bpm * 2.0),
    });
    return 1.0 - std::clamp(error / 0.08, 0.0, 1.0);
}

[[nodiscard]] double nearest_beat_distance_ms(
    const BeatGrid& grid,
    const double time_ms
) {
    if (grid.beats.empty()) {
        return grid.period_ms;
    }
    const auto iterator = std::lower_bound(grid.beats.begin(), grid.beats.end(), time_ms);
    double distance = std::numeric_limits<double>::infinity();
    if (iterator != grid.beats.end()) {
        distance = std::abs(*iterator - time_ms);
    }
    if (iterator != grid.beats.begin()) {
        distance = (std::min)(distance, std::abs(*std::prev(iterator) - time_ms));
    }
    return distance;
}


[[nodiscard]] std::string_view to_string(
    const StructuralSectionKind kind
) noexcept {
    switch (kind) {
    case StructuralSectionKind::intro: return "intro";
    case StructuralSectionKind::verse: return "verse";
    case StructuralSectionKind::chorus: return "chorus";
    case StructuralSectionKind::buildup: return "buildup";
    case StructuralSectionKind::drop: return "drop";
    case StructuralSectionKind::breakdown: return "breakdown";
    case StructuralSectionKind::bridge: return "bridge";
    case StructuralSectionKind::outro: return "outro";
    }
    return "verse";
}

[[nodiscard]] double nearest_sorted_distance_ms(
    const std::vector<double>& values,
    const double time_ms,
    const double fallback
) {
    if (values.empty()) {
        return fallback;
    }
    const auto iterator = std::lower_bound(values.begin(), values.end(), time_ms);
    double distance = fallback;
    if (iterator != values.end()) {
        distance = (std::min)(distance, std::abs(*iterator - time_ms));
    }
    if (iterator != values.begin()) {
        distance = (std::min)(
            distance,
            std::abs(*std::prev(iterator) - time_ms)
        );
    }
    return distance;
}

[[nodiscard]] float robust_unit_value(
    const double value,
    std::vector<double> distribution
) {
    if (distribution.empty() || !std::isfinite(value)) {
        return 0.0F;
    }
    distribution.erase(
        std::remove_if(
            distribution.begin(), distribution.end(),
            [](const double item) { return !std::isfinite(item); }
        ),
        distribution.end()
    );
    if (distribution.empty()) {
        return 0.0F;
    }
    std::sort(distribution.begin(), distribution.end());
    const auto percentile = [&](const double fraction) {
        const double position = fraction
            * static_cast<double>(distribution.size() - 1U);
        const auto lower = static_cast<std::size_t>(std::floor(position));
        const auto upper = (std::min)(distribution.size() - 1U, lower + 1U);
        const double blend = position - static_cast<double>(lower);
        return distribution[lower] * (1.0 - blend)
            + distribution[upper] * blend;
    };
    const double low = percentile(0.12);
    const double high = percentile(0.88);
    if (high <= low + 1.0e-12) {
        return value > low ? 1.0F : 0.0F;
    }
    return static_cast<float>(std::clamp(
        (value - low) / (high - low),
        0.0,
        1.0
    ));
}

[[nodiscard]] std::vector<double> structure_bar_boundaries(
    const BeatGrid& grid,
    const double duration_ms
) {
    std::vector<double> boundaries;
    boundaries.reserve(grid.downbeats.size() + 4U);
    for (const double value : grid.downbeats) {
        if (std::isfinite(value) && value >= 0.0 && value < duration_ms) {
            boundaries.push_back(value);
        }
    }
    if (boundaries.size() < 2U) {
        boundaries.clear();
        for (std::size_t index = 0U; index < grid.beats.size(); index += 4U) {
            const double value = grid.beats[index];
            if (std::isfinite(value) && value >= 0.0 && value < duration_ms) {
                boundaries.push_back(value);
            }
        }
    }
    if (boundaries.empty() || boundaries.front() > 120.0) {
        boundaries.insert(boundaries.begin(), 0.0);
    } else {
        boundaries.front() = 0.0;
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(
        std::unique(
            boundaries.begin(), boundaries.end(),
            [](const double left, const double right) {
                return std::abs(left - right) < 8.0;
            }
        ),
        boundaries.end()
    );
    if (boundaries.empty() || duration_ms - boundaries.back() > 80.0) {
        boundaries.push_back(duration_ms);
    } else {
        boundaries.back() = duration_ms;
    }
    return boundaries;
}

[[nodiscard]] SongStructure analyze_song_structure(
    const std::vector<FeatureFrame>& frames,
    const BeatGrid& grid,
    const std::vector<Candidate>& candidates,
    const double duration_seconds,
    const bool enabled
) {
    SongStructure structure;
    if (!enabled || frames.empty() || duration_seconds <= 0.0) {
        return structure;
    }
    const double duration_ms = duration_seconds * 1'000.0;
    const auto boundaries = structure_bar_boundaries(grid, duration_ms);
    if (boundaries.size() < 3U) {
        return structure;
    }

    std::vector<BarDescriptor> bars;
    bars.reserve(boundaries.size() - 1U);
    std::size_t frame_begin = 0U;
    std::size_t candidate_begin = 0U;
    for (std::size_t bar_index = 0U;
         bar_index + 1U < boundaries.size();
         ++bar_index) {
        const double start_ms = boundaries[bar_index];
        const double end_ms = boundaries[bar_index + 1U];
        if (end_ms <= start_ms + 1.0) {
            continue;
        }
        while (frame_begin < frames.size()
               && frames[frame_begin].time_ms < start_ms) {
            ++frame_begin;
        }
        std::size_t frame_end = frame_begin;
        double rms = 0.0;
        double onset = 0.0;
        double low = 0.0;
        double mid = 0.0;
        double high = 0.0;
        double centroid = 0.0;
        std::size_t frame_count = 0U;
        while (frame_end < frames.size()
               && frames[frame_end].time_ms < end_ms) {
            const auto& frame = frames[frame_end];
            rms += static_cast<double>(frame.rms);
            onset += static_cast<double>(frame.onset);
            low += static_cast<double>(frame.low_flux);
            mid += static_cast<double>(frame.mid_flux);
            high += static_cast<double>(frame.high_flux);
            centroid += static_cast<double>(frame.centroid_hz);
            ++frame_count;
            ++frame_end;
        }
        while (candidate_begin < candidates.size()
               && candidates[candidate_begin].time_ms < start_ms) {
            ++candidate_begin;
        }
        std::size_t candidate_end = candidate_begin;
        while (candidate_end < candidates.size()
               && candidates[candidate_end].time_ms < end_ms) {
            ++candidate_end;
        }
        const double inverse = frame_count == 0U
            ? 0.0
            : 1.0 / static_cast<double>(frame_count);
        const double seconds = (end_ms - start_ms) / 1'000.0;
        bars.push_back({
            start_ms,
            end_ms,
            static_cast<float>(rms * inverse),
            static_cast<float>(onset * inverse),
            static_cast<float>(low * inverse),
            static_cast<float>(mid * inverse),
            static_cast<float>(high * inverse),
            static_cast<float>(centroid * inverse),
            static_cast<float>(
                static_cast<double>(candidate_end - candidate_begin)
                / (std::max)(0.001, seconds)
            ),
            0.0F,
            0.0F,
        });
        frame_begin = frame_end;
        candidate_begin = candidate_end;
    }
    if (bars.size() < 2U) {
        return structure;
    }

    std::vector<double> rms_values;
    std::vector<double> onset_values;
    std::vector<double> density_values;
    std::vector<double> low_values;
    std::vector<double> high_values;
    std::vector<double> centroid_values;
    rms_values.reserve(bars.size());
    onset_values.reserve(bars.size());
    density_values.reserve(bars.size());
    low_values.reserve(bars.size());
    high_values.reserve(bars.size());
    centroid_values.reserve(bars.size());
    for (const auto& bar : bars) {
        rms_values.push_back(bar.rms);
        onset_values.push_back(bar.onset);
        density_values.push_back(bar.candidate_density);
        low_values.push_back(bar.low);
        high_values.push_back(bar.high);
        centroid_values.push_back(bar.centroid);
    }

    std::vector<float> normalized_rms;
    std::vector<float> normalized_onset;
    std::vector<float> normalized_density;
    std::vector<float> normalized_low;
    std::vector<float> normalized_high;
    std::vector<float> normalized_centroid;
    normalized_rms.reserve(bars.size());
    normalized_onset.reserve(bars.size());
    normalized_density.reserve(bars.size());
    normalized_low.reserve(bars.size());
    normalized_high.reserve(bars.size());
    normalized_centroid.reserve(bars.size());
    for (const auto& bar : bars) {
        normalized_rms.push_back(robust_unit_value(bar.rms, rms_values));
        normalized_onset.push_back(robust_unit_value(bar.onset, onset_values));
        normalized_density.push_back(
            robust_unit_value(bar.candidate_density, density_values)
        );
        normalized_low.push_back(robust_unit_value(bar.low, low_values));
        normalized_high.push_back(robust_unit_value(bar.high, high_values));
        normalized_centroid.push_back(
            robust_unit_value(bar.centroid, centroid_values)
        );
    }

    for (std::size_t index = 0U; index < bars.size(); ++index) {
        bars[index].intensity = std::clamp(
            normalized_rms[index] * 0.34F
                + normalized_onset[index] * 0.28F
                + normalized_density[index] * 0.24F
                + normalized_high[index] * 0.08F
                + normalized_low[index] * 0.06F,
            0.0F,
            1.0F
        );
        if (index == 0U) {
            bars[index].novelty = 0.0F;
            continue;
        }
        bars[index].novelty = std::clamp(
            std::abs(normalized_rms[index] - normalized_rms[index - 1U]) * 0.24F
                + std::abs(
                    normalized_onset[index] - normalized_onset[index - 1U]
                ) * 0.22F
                + std::abs(
                    normalized_density[index] - normalized_density[index - 1U]
                ) * 0.20F
                + std::abs(
                    normalized_low[index] - normalized_low[index - 1U]
                ) * 0.10F
                + std::abs(
                    normalized_high[index] - normalized_high[index - 1U]
                ) * 0.12F
                + std::abs(
                    normalized_centroid[index] - normalized_centroid[index - 1U]
                ) * 0.12F,
            0.0F,
            1.0F
        );
    }

    std::vector<std::size_t> section_starts{0U};
    std::size_t last_boundary = 0U;
    for (std::size_t index = 2U; index < bars.size(); ++index) {
        const std::size_t bars_since = index - last_boundary;
        const bool strong_boundary = bars[index].novelty >= 0.48F
            && bars_since >= 2U;
        const bool phrase_boundary = bars_since >= 4U
            && index % 4U == 0U
            && bars[index].novelty >= 0.22F;
        const bool maximum_phrase = bars_since >= 8U;
        if (strong_boundary || phrase_boundary || maximum_phrase) {
            section_starts.push_back(index);
            structure.phrase_boundaries.push_back(bars[index].start_ms);
            last_boundary = index;
        }
    }
    if (section_starts.back() != bars.size()) {
        section_starts.push_back(bars.size());
    }

    float previous_intensity = 0.0F;
    for (std::size_t section_index = 0U;
         section_index + 1U < section_starts.size();
         ++section_index) {
        const auto begin = section_starts[section_index];
        const auto end = section_starts[section_index + 1U];
        if (end <= begin) {
            continue;
        }
        float intensity_sum = 0.0F;
        float density_sum = 0.0F;
        float novelty_peak = 0.0F;
        for (std::size_t index = begin; index < end; ++index) {
            intensity_sum += bars[index].intensity;
            density_sum += normalized_density[index];
            novelty_peak = (std::max)(novelty_peak, bars[index].novelty);
        }
        const float inverse = 1.0F / static_cast<float>(end - begin);
        const float intensity = intensity_sum * inverse;
        const float density = density_sum * inverse;
        const float trend = bars[end - 1U].intensity - bars[begin].intensity;
        StructuralSectionKind kind = StructuralSectionKind::verse;
        const bool first = structure.sections.empty();
        const bool last = end == bars.size();
        if (first && intensity < 0.48F) {
            kind = StructuralSectionKind::intro;
        } else if (last && intensity < 0.52F) {
            kind = StructuralSectionKind::outro;
        } else if (intensity < 0.26F && density < 0.38F) {
            kind = StructuralSectionKind::breakdown;
        } else if (trend >= 0.20F && intensity >= 0.42F) {
            kind = StructuralSectionKind::buildup;
        } else if (!first && intensity >= 0.68F
                   && intensity - previous_intensity >= 0.18F) {
            kind = StructuralSectionKind::drop;
        } else if (intensity >= 0.64F && density >= 0.56F) {
            kind = StructuralSectionKind::chorus;
        } else if (intensity <= 0.50F && density <= 0.56F) {
            kind = StructuralSectionKind::verse;
        } else {
            kind = StructuralSectionKind::bridge;
        }

        float density_scale = 0.64F + intensity * 0.46F + density * 0.24F;
        switch (kind) {
        case StructuralSectionKind::intro:
        case StructuralSectionKind::outro:
            density_scale *= 0.78F;
            break;
        case StructuralSectionKind::breakdown:
            density_scale *= 0.72F;
            break;
        case StructuralSectionKind::buildup:
            density_scale *= 0.92F;
            break;
        case StructuralSectionKind::chorus:
            density_scale *= 1.08F;
            break;
        case StructuralSectionKind::drop:
            density_scale *= 1.16F;
            break;
        case StructuralSectionKind::bridge:
            density_scale *= 0.96F;
            break;
        case StructuralSectionKind::verse:
            break;
        }
        density_scale = std::clamp(density_scale, 0.52F, 1.38F);
        const float section_confidence = std::clamp(
            0.40F + novelty_peak * 0.30F
                + static_cast<float>(grid.confidence) * 0.20F
                + (end - begin >= 4U ? 0.10F : 0.04F),
            0.0F,
            1.0F
        );
        structure.sections.push_back({
            bars[begin].start_ms,
            bars[end - 1U].end_ms,
            intensity,
            density,
            novelty_peak,
            density_scale,
            section_confidence,
            kind,
        });
        previous_intensity = intensity;
    }

    if (structure.sections.empty()) {
        return {};
    }
    if (structure.phrase_boundaries.empty()) {
        for (std::size_t index = 4U; index < bars.size(); index += 4U) {
            structure.phrase_boundaries.push_back(bars[index].start_ms);
        }
    }
    double confidence_sum = 0.0;
    for (const auto& section : structure.sections) {
        confidence_sum += section.confidence;
    }
    structure.confidence = std::clamp(
        confidence_sum / static_cast<double>(structure.sections.size()),
        0.0,
        1.0
    );
    structure.used = true;
    return structure;
}

[[nodiscard]] const StructuralSection* section_at(
    const SongStructure& structure,
    const double time_ms
) noexcept {
    if (!structure.used || structure.sections.empty()) {
        return nullptr;
    }
    const auto iterator = std::upper_bound(
        structure.sections.begin(), structure.sections.end(), time_ms,
        [](const double time, const StructuralSection& section) {
            return time < section.start_ms;
        }
    );
    if (iterator == structure.sections.begin()) {
        return &structure.sections.front();
    }
    const auto& section = *std::prev(iterator);
    return time_ms <= section.end_ms + 1.0 ? &section : nullptr;
}

[[nodiscard]] float downbeat_alignment(
    const BeatGrid& grid,
    const double time_ms
) {
    const double sigma = (std::max)(18.0, grid.period_ms * 0.085);
    const double distance = nearest_sorted_distance_ms(
        grid.downbeats, time_ms, grid.period_ms * 2.0
    );
    return static_cast<float>(std::exp(
        -0.5 * distance * distance / (sigma * sigma)
    ));
}

[[nodiscard]] float phrase_alignment(
    const SongStructure& structure,
    const double time_ms,
    const double period_ms
) {
    if (!structure.used || structure.phrase_boundaries.empty()) {
        return 0.0F;
    }
    const double sigma = (std::max)(22.0, period_ms * 0.10);
    const double distance = nearest_sorted_distance_ms(
        structure.phrase_boundaries, time_ms, period_ms * 4.0
    );
    return static_cast<float>(std::exp(
        -0.5 * distance * distance / (sigma * sigma)
    ));
}

[[nodiscard]] float structural_candidate_priority(
    const Candidate& candidate,
    const BeatGrid& grid,
    const SongStructure& structure
) {
    const auto* section = section_at(structure, candidate.time_ms);
    const float section_scale = section == nullptr ? 1.0F : section->density_scale;
    const float downbeat = downbeat_alignment(grid, candidate.time_ms);
    const float phrase = phrase_alignment(
        structure, candidate.time_ms, grid.period_ms
    );
    const float evidence_bonus =
        ((candidate.evidence & evidence_drum_ml) != 0U ? 0.05F : 0.0F)
        + ((candidate.evidence & evidence_pitch) != 0U ? 0.04F : 0.0F)
        + ((candidate.evidence & evidence_stem) != 0U ? 0.03F : 0.0F);
    return std::clamp(
        candidate.confidence
            * (0.72F + candidate.beat_alignment * 0.14F
                + downbeat * 0.09F + phrase * 0.05F)
            * std::clamp(section_scale, 0.62F, 1.30F)
            + evidence_bonus,
        0.0F,
        1.35F
    );
}

[[nodiscard]] double estimate_sustain(
    const std::vector<FeatureFrame>& frames,
    const std::size_t onset_index,
    const AnalysisParameters parameters
) {
    const auto& start = frames[onset_index];
    if (start.peak_hz < 80.0F || start.rms <= 1.0e-5F) {
        return 0.0;
    }
    const double frame_ms = static_cast<double>(parameters.hop_size)
        * 1'000.0 / static_cast<double>(analysis_sample_rate);
    const std::size_t maximum_frames = static_cast<std::size_t>(
        std::ceil(2'700.0 / frame_ms)
    );
    std::size_t last_good = onset_index;
    for (std::size_t index = onset_index + 1U;
         index < frames.size() && index <= onset_index + maximum_frames;
         ++index) {
        const auto& frame = frames[index];
        if (frame.onset >= 0.72F && index > onset_index + 2U) {
            break;
        }
        if (frame.rms < start.rms * 0.42F) {
            break;
        }
        if (frame.peak_hz > 0.0F) {
            const double ratio = static_cast<double>(frame.peak_hz)
                / static_cast<double>(start.peak_hz);
            if (ratio < 0.88 || ratio > 1.14) {
                break;
            }
        }
        last_good = index;
    }
    const double duration = static_cast<double>(last_good - onset_index) * frame_ms;
    return duration >= 260.0 ? duration : 0.0;
}

[[nodiscard]] std::vector<Candidate> make_candidates(
    const std::vector<FeatureFrame>& frames,
    const AnalysisParameters parameters,
    const BeatGrid& grid
) {
    std::vector<Candidate> candidates;
    candidates.reserve(frames.size() / 5U);
    for (std::size_t index = 2U; index + 2U < frames.size(); ++index) {
        const auto& frame = frames[index];
        if (frame.onset < parameters.onset_floor) {
            continue;
        }
        if (frame.onset < frames[index - 1U].onset
            || frame.onset < frames[index + 1U].onset) {
            continue;
        }
        const float band_sum = frame.low_flux + frame.mid_flux + frame.high_flux + 1.0e-12F;
        const float low = frame.low_flux / band_sum;
        const float mid = frame.mid_flux / band_sum;
        const float high = frame.high_flux / band_sum;
        CandidateSource source = CandidateSource::mixed;
        const float strongest = (std::max)({low, mid, high});
        if (strongest == low) {
            source = CandidateSource::low;
        } else if (strongest == high) {
            source = CandidateSource::high;
        } else if (strongest == mid) {
            source = CandidateSource::mid;
        }
        const bool polyphonic = (low > 0.22F && high > 0.22F)
            || (low > 0.28F && mid > 0.28F)
            || (mid > 0.30F && high > 0.25F);
        const double beat_distance = nearest_beat_distance_ms(grid, frame.time_ms);
        const double sigma = (std::max)(22.0, grid.period_ms * 0.13);
        const float beat_alignment = static_cast<float>(std::exp(
            -0.5 * beat_distance * beat_distance / (sigma * sigma)
        ));
        const float confidence = std::clamp(
            frame.onset * 0.72F
                + beat_alignment * 0.20F
                + frame.video_pulse * 0.08F,
            0.0F,
            1.0F
        );
        double sustain = 0.0;
        if ((source == CandidateSource::mid || source == CandidateSource::mixed)
            && confidence >= 0.45F) {
            sustain = estimate_sustain(frames, index, parameters);
        }
        candidates.push_back({
            frame.time_ms,
            sustain,
            confidence,
            frame.onset,
            beat_alignment,
            frame.video_pulse,
            frame.peak_hz,
            frame.centroid_hz,
            source,
            polyphonic,
        });
    }
    return candidates;
}

[[nodiscard]] std::vector<Candidate> make_stem_candidates(
    const std::filesystem::path& ffmpeg,
    const std::filesystem::path& stem_path,
    const CandidateSource stem_source,
    AnalysisParameters parameters,
    const BeatGrid& grid,
    const TemporaryWorkspace& workspace,
    const std::string_view label
) {
    if (stem_path.empty()) {
        return {};
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(stem_path, error) || error) {
        return {};
    }
    const auto raw = workspace.path() / ("stem-" + std::string(label) + ".f32");
    if (!decode_audio_to_f32(ffmpeg, stem_path, raw)) {
        return {};
    }
    // Separation removes masking from other instruments, so a slightly lower
    // transient floor recovers musically meaningful attacks without changing
    // the native fallback detector.
    parameters.onset_floor = (std::max)(0.10F, parameters.onset_floor * 0.72F);
    double ignored_duration = 0.0;
    auto frames = extract_audio_features(raw, parameters, ignored_duration);
    calculate_onset_envelope(frames, parameters);
    auto candidates = make_candidates(frames, parameters, grid);
    for (auto& candidate : candidates) {
        candidate.source = stem_source;
        candidate.evidence |= evidence_stem;
        switch (stem_source) {
        case CandidateSource::drums:
            candidate.confidence = std::clamp(candidate.confidence * 0.90F + 0.12F, 0.0F, 1.0F);
            candidate.sustain_ms = 0.0;
            break;
        case CandidateSource::bass:
            candidate.confidence = std::clamp(candidate.confidence * 0.94F + 0.08F, 0.0F, 1.0F);
            break;
        case CandidateSource::vocals:
            candidate.confidence = std::clamp(candidate.confidence * 0.93F + 0.07F, 0.0F, 1.0F);
            break;
        case CandidateSource::other:
            candidate.confidence = std::clamp(candidate.confidence * 0.96F + 0.04F, 0.0F, 1.0F);
            break;
        default:
            break;
        }
    }
    return candidates;
}

[[nodiscard]] float drum_role_frequency(const MlDrumRole role) noexcept {
    switch (role) {
    case MlDrumRole::kick: return 75.0F;
    case MlDrumRole::snare: return 240.0F;
    case MlDrumRole::tom: return 150.0F;
    case MlDrumRole::hihat: return 5'500.0F;
    case MlDrumRole::cymbal: return 8'000.0F;
    case MlDrumRole::other: return 1'000.0F;
    }
    return 1'000.0F;
}

[[nodiscard]] int drum_role_priority(const MlDrumRole role) noexcept {
    switch (role) {
    case MlDrumRole::kick: return 5;
    case MlDrumRole::snare: return 4;
    case MlDrumRole::cymbal: return 3;
    case MlDrumRole::hihat: return 2;
    case MlDrumRole::tom: return 1;
    case MlDrumRole::other: return 0;
    }
    return 0;
}

[[nodiscard]] std::vector<Candidate> make_drum_candidates(
    const MlAnalysis& analysis,
    const BeatGrid& grid
) {
    if (!analysis.drum_transcription_used || analysis.drum_events.empty()) {
        return {};
    }
    std::vector<MlDrumEvent> events = analysis.drum_events;
    std::stable_sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
        if (left.time_ms != right.time_ms) {
            return left.time_ms < right.time_ms;
        }
        return drum_role_priority(left.role) > drum_role_priority(right.role);
    });
    std::vector<Candidate> candidates;
    candidates.reserve(events.size());
    constexpr double cluster_ms = 14.0;
    std::size_t begin = 0U;
    while (begin < events.size()) {
        std::size_t end = begin + 1U;
        while (end < events.size()
               && events[end].time_ms - events[begin].time_ms <= cluster_ms) {
            ++end;
        }
        const MlDrumEvent* strongest = &events[begin];
        double weighted_time = 0.0;
        double weight_sum = 0.0;
        for (std::size_t index = begin; index < end; ++index) {
            const auto& event = events[index];
            const double weight = (std::max)(0.05, static_cast<double>(event.confidence));
            weighted_time += event.time_ms * weight;
            weight_sum += weight;
            if (event.confidence > strongest->confidence + 0.04F
                || (std::abs(event.confidence - strongest->confidence) <= 0.04F
                    && drum_role_priority(event.role) > drum_role_priority(strongest->role))) {
                strongest = &event;
            }
        }
        const double time_ms = weight_sum > 0.0
            ? weighted_time / weight_sum
            : strongest->time_ms;
        const double beat_distance = nearest_beat_distance_ms(grid, time_ms);
        const double sigma = (std::max)(18.0, grid.period_ms * 0.11);
        const float beat_alignment = static_cast<float>(std::exp(
            -0.5 * beat_distance * beat_distance / (sigma * sigma)
        ));
        const bool polyphonic = end - begin >= 2U;
        const float confidence = std::clamp(
            0.48F + strongest->confidence * 0.42F
                + beat_alignment * 0.08F + (polyphonic ? 0.02F : 0.0F),
            0.0F,
            1.0F
        );
        const float proxy_frequency = drum_role_frequency(strongest->role);
        candidates.push_back({
            time_ms,
            0.0,
            confidence,
            confidence,
            beat_alignment,
            0.0F,
            proxy_frequency,
            proxy_frequency,
            CandidateSource::drums,
            polyphonic,
            evidence_drum_ml
                | (analysis.beat_tracking_used ? evidence_neural_beat : 0U),
        });
        begin = end;
    }
    return candidates;
}

[[nodiscard]] float midi_to_frequency(const std::uint8_t midi) {
    return static_cast<float>(440.0 * std::pow(
        2.0,
        (static_cast<double>(midi) - 69.0) / 12.0
    ));
}

[[nodiscard]] std::vector<Candidate> make_pitch_candidates(
    const MlAnalysis& analysis,
    const BeatGrid& grid
) {
    if (!analysis.pitch_transcription_used || analysis.pitch_events.empty()) {
        return {};
    }
    std::vector<MlPitchEvent> events = analysis.pitch_events;
    std::stable_sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
        if (left.start_ms != right.start_ms) {
            return left.start_ms < right.start_ms;
        }
        return left.midi < right.midi;
    });

    std::vector<Candidate> candidates;
    candidates.reserve(events.size() / 2U + 1U);
    constexpr double cluster_ms = 22.0;
    std::size_t begin = 0U;
    while (begin < events.size()) {
        std::size_t end = begin + 1U;
        while (end < events.size()
               && events[end].start_ms - events[begin].start_ms <= cluster_ms) {
            ++end;
        }
        const MlPitchEvent* strongest = &events[begin];
        double weighted_time = 0.0;
        double weight_sum = 0.0;
        double sustain = 0.0;
        std::uint8_t minimum_midi = events[begin].midi;
        std::uint8_t maximum_midi = events[begin].midi;
        for (std::size_t index = begin; index < end; ++index) {
            const auto& event = events[index];
            const double weight = (std::max)(0.05, static_cast<double>(event.confidence));
            weighted_time += event.start_ms * weight;
            weight_sum += weight;
            sustain = (std::max)(sustain, event.end_ms - event.start_ms);
            minimum_midi = (std::min)(minimum_midi, event.midi);
            maximum_midi = (std::max)(maximum_midi, event.midi);
            if (event.confidence > strongest->confidence) {
                strongest = &event;
            }
        }
        const double time_ms = weight_sum > 0.0
            ? weighted_time / weight_sum
            : strongest->start_ms;
        const double beat_distance = nearest_beat_distance_ms(grid, time_ms);
        const double sigma = (std::max)(20.0, grid.period_ms * 0.12);
        const float beat_alignment = static_cast<float>(std::exp(
            -0.5 * beat_distance * beat_distance / (sigma * sigma)
        ));
        const bool polyphonic = end - begin >= 2U
            && static_cast<unsigned int>(maximum_midi - minimum_midi) >= 3U;
        const float confidence = std::clamp(
            0.38F + strongest->confidence * 0.48F
                + beat_alignment * 0.10F + (polyphonic ? 0.04F : 0.0F),
            0.0F,
            1.0F
        );
        candidates.push_back({
            time_ms,
            sustain >= 145.0 ? sustain : 0.0,
            confidence,
            confidence,
            beat_alignment,
            0.0F,
            midi_to_frequency(strongest->midi),
            midi_to_frequency(strongest->midi),
            strongest->source,
            polyphonic,
            evidence_pitch | (analysis.beat_tracking_used ? evidence_neural_beat : 0U),
        });
        begin = end;
    }
    return candidates;
}

[[nodiscard]] std::vector<Candidate> make_vocal_refinement_candidates(
    const MlAnalysis& analysis,
    const BeatGrid& grid
) {
    if (!analysis.vocal_refinement_used || analysis.vocal_events.empty()) {
        return {};
    }

    std::vector<const MlVocalEvent*> syllables;
    syllables.reserve(analysis.vocal_events.size() / 2U + 1U);
    for (const auto& event : analysis.vocal_events) {
        if (event.role == MlVocalRole::syllable
            && event.confidence >= 0.25F
            && std::isfinite(event.start_ms)
            && std::isfinite(event.end_ms)
            && event.end_ms > event.start_ms) {
            syllables.push_back(&event);
        }
    }
    if (syllables.empty()) {
        return {};
    }
    std::stable_sort(
        syllables.begin(),
        syllables.end(),
        [](const MlVocalEvent* left, const MlVocalEvent* right) {
            return left->start_ms < right->start_ms;
        }
    );

    std::vector<const MlPitchEvent*> vocal_pitches;
    if (analysis.pitch_transcription_used) {
        vocal_pitches.reserve(analysis.pitch_events.size() / 3U + 1U);
        for (const auto& event : analysis.pitch_events) {
            if (event.source == CandidateSource::vocals) {
                vocal_pitches.push_back(&event);
            }
        }
        std::stable_sort(
            vocal_pitches.begin(),
            vocal_pitches.end(),
            [](const MlPitchEvent* left, const MlPitchEvent* right) {
                return left->start_ms < right->start_ms;
            }
        );
    }

    std::vector<Candidate> candidates;
    candidates.reserve(syllables.size());
    std::size_t pitch_cursor = 0U;
    for (std::size_t syllable_index = 0U;
         syllable_index < syllables.size();
         ++syllable_index) {
        const auto& syllable = *syllables[syllable_index];
        while (pitch_cursor < vocal_pitches.size()
               && vocal_pitches[pitch_cursor]->end_ms
                    < syllable.start_ms - 120.0) {
            ++pitch_cursor;
        }

        const MlPitchEvent* best_pitch = nullptr;
        float best_pitch_score = -1.0F;
        for (std::size_t index = pitch_cursor;
             index < vocal_pitches.size();
             ++index) {
            const auto& pitch = *vocal_pitches[index];
            if (pitch.start_ms > syllable.start_ms + 120.0) {
                break;
            }
            if (pitch.end_ms < syllable.start_ms - 120.0) {
                continue;
            }
            const double distance = syllable.start_ms < pitch.start_ms
                ? pitch.start_ms - syllable.start_ms
                : syllable.start_ms > pitch.end_ms
                    ? syllable.start_ms - pitch.end_ms
                    : 0.0;
            const float score = pitch.confidence
                - static_cast<float>((std::min)(distance / 250.0, 0.4));
            if (score > best_pitch_score) {
                best_pitch_score = score;
                best_pitch = &pitch;
            }
        }

        double sustain_ms = 0.0;
        float pitch_hz = 330.0F;
        float pitch_confidence = 0.0F;
        std::uint32_t evidence = evidence_vocal_ml;
        if (best_pitch != nullptr) {
            pitch_hz = midi_to_frequency(best_pitch->midi);
            pitch_confidence = best_pitch->confidence;
            evidence |= evidence_pitch;
            double segment_end = best_pitch->end_ms;
            if (syllable_index + 1U < syllables.size()) {
                const double next_onset = syllables[syllable_index + 1U]->start_ms;
                if (next_onset > syllable.start_ms + 110.0
                    && next_onset < segment_end) {
                    // A new syllable inside one Basic-Pitch note is a real
                    // articulation boundary, not one giant hold. Leave a tiny
                    // release gap so two generated sustains never overlap.
                    segment_end = next_onset - 28.0;
                }
            }
            const double possible_sustain = segment_end - syllable.start_ms;
            if (possible_sustain >= 155.0) {
                sustain_ms = possible_sustain;
            }
        }

        const double beat_distance = nearest_beat_distance_ms(grid, syllable.start_ms);
        const double sigma = (std::max)(22.0, grid.period_ms * 0.13);
        const float beat_alignment = static_cast<float>(std::exp(
            -0.5 * beat_distance * beat_distance / (sigma * sigma)
        ));
        float confidence = 0.40F
            + syllable.confidence * 0.38F
            + pitch_confidence * 0.14F
            + beat_alignment * 0.08F;
        if (best_pitch == nullptr) {
            confidence -= 0.08F;
        }
        confidence = std::clamp(confidence, 0.0F, 1.0F);
        if (confidence < 0.47F) {
            continue;
        }
        candidates.push_back({
            syllable.start_ms,
            sustain_ms,
            confidence,
            confidence,
            beat_alignment,
            0.0F,
            pitch_hz,
            pitch_hz,
            CandidateSource::vocals,
            false,
            evidence | (analysis.beat_tracking_used ? evidence_neural_beat : 0U),
        });
    }
    return candidates;
}

[[nodiscard]] int source_priority(const CandidateSource source) noexcept {
    switch (source) {
    case CandidateSource::vocals: return 7;
    case CandidateSource::drums: return 6;
    case CandidateSource::bass: return 5;
    case CandidateSource::other: return 4;
    case CandidateSource::mixed: return 3;
    case CandidateSource::mid: return 2;
    case CandidateSource::high: return 1;
    case CandidateSource::low: return 0;
    }
    return 0;
}

[[nodiscard]] std::vector<Candidate> fuse_candidates(
    std::vector<Candidate> base,
    std::vector<Candidate> additional
) {
    if (additional.empty()) {
        return base;
    }
    base.reserve(base.size() + additional.size());
    std::move(additional.begin(), additional.end(), std::back_inserter(base));
    std::stable_sort(base.begin(), base.end(), [](const Candidate& left, const Candidate& right) {
        if (left.time_ms != right.time_ms) {
            return left.time_ms < right.time_ms;
        }
        return left.confidence > right.confidence;
    });

    std::vector<Candidate> fused;
    fused.reserve(base.size());
    constexpr double merge_window_ms = 18.0;
    for (const auto& candidate : base) {
        if (fused.empty() || candidate.time_ms - fused.back().time_ms > merge_window_ms) {
            fused.push_back(candidate);
            continue;
        }
        auto& target = fused.back();
        const double left_weight = (std::max)(0.05, static_cast<double>(target.confidence));
        const double right_weight = (std::max)(0.05, static_cast<double>(candidate.confidence));
        target.time_ms = (target.time_ms * left_weight + candidate.time_ms * right_weight)
            / (left_weight + right_weight);
        const bool independent_evidence = (target.evidence & candidate.evidence) == 0U;
        const float contribution = candidate.confidence
            * (independent_evidence ? 0.72F : 0.42F);
        target.confidence = 1.0F
            - (1.0F - target.confidence) * (1.0F - std::clamp(contribution, 0.0F, 0.95F));
        target.onset = (std::max)(target.onset, candidate.onset);
        target.beat_alignment = (std::max)(target.beat_alignment, candidate.beat_alignment);
        target.video_pulse = (std::max)(target.video_pulse, candidate.video_pulse);
        target.sustain_ms = (std::max)(target.sustain_ms, candidate.sustain_ms);
        target.polyphonic = target.polyphonic || candidate.polyphonic
            || ((target.evidence & evidence_pitch) != 0U
                && (candidate.evidence & evidence_pitch) != 0U);
        if ((candidate.evidence & evidence_pitch) != 0U && candidate.peak_hz > 0.0F) {
            target.peak_hz = candidate.peak_hz;
            target.centroid_hz = candidate.centroid_hz;
            target.source = candidate.source;
        } else if (source_priority(candidate.source) > source_priority(target.source)) {
            target.source = candidate.source;
            if (target.peak_hz <= 0.0F) {
                target.peak_hz = candidate.peak_hz;
            }
            if (target.centroid_hz <= 0.0F) {
                target.centroid_hz = candidate.centroid_hz;
            }
        }
        target.evidence |= candidate.evidence;
    }
    return fused;
}

[[nodiscard]] double quantize_time(
    const double time_ms,
    const BeatGrid& grid,
    const std::uint32_t subdivisions
) {
    if (subdivisions == 0U || !std::isfinite(grid.period_ms)
        || grid.period_ms <= 0.0) {
        return time_ms;
    }

    // Prefer the observed beat sequence over an idealized constant-period
    // lattice. This preserves live timing and variable-tempo songs while still
    // snapping small detector jitter to musical subdivisions.
    if (grid.beats.size() >= 2U) {
        const auto upper = std::lower_bound(
            grid.beats.begin(), grid.beats.end(), time_ms
        );
        std::size_t left_index = 0U;
        if (upper == grid.beats.begin()) {
            left_index = 0U;
        } else if (upper == grid.beats.end()) {
            left_index = grid.beats.size() - 2U;
        } else {
            left_index = static_cast<std::size_t>(
                std::distance(grid.beats.begin(), upper) - 1
            );
        }
        const double left = grid.beats[left_index];
        const double right = grid.beats[left_index + 1U];
        const double interval = right - left;
        if (std::isfinite(interval) && interval > 1.0) {
            const double step = interval / static_cast<double>(subdivisions);
            const double local_units = (time_ms - left) / step;
            const double snapped = left + std::round(local_units) * step;
            const double maximum_snap = (std::min)(32.0, step * 0.24);
            if (std::abs(snapped - time_ms) <= maximum_snap) {
                return (std::max)(0.0, snapped);
            }
        }
    }

    const double step = grid.period_ms / static_cast<double>(subdivisions);
    const double units = (time_ms - grid.offset_ms) / step;
    const double snapped = grid.offset_ms + std::round(units) * step;
    const double maximum_snap = (std::min)(32.0, step * 0.24);
    return std::abs(snapped - time_ms) <= maximum_snap
        ? (std::max)(0.0, snapped)
        : time_ms;
}

[[nodiscard]] float adaptive_confidence_threshold(
    const std::vector<Candidate>& candidates,
    const DifficultyProfile& profile,
    const double duration_seconds
) {
    if (candidates.empty() || duration_seconds <= 0.0) {
        return profile.minimum_confidence;
    }
    const auto target_count = static_cast<std::size_t>(std::clamp(
        profile.target_nps * duration_seconds,
        1.0,
        static_cast<double>(candidates.size())
    ));
    if (target_count >= candidates.size()) {
        return profile.minimum_confidence;
    }
    std::vector<float> values;
    values.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        values.push_back(candidate.confidence);
    }
    const auto position = values.begin()
        + static_cast<std::ptrdiff_t>(values.size() - target_count);
    std::nth_element(values.begin(), position, values.end());
    return (std::max)(profile.minimum_confidence, *position * 0.93F);
}


struct SelectionBudget final {
    double start_ms{};
    double end_ms{};
    float priority_threshold{};
    float minimum_confidence{};
    double minimum_spacing_ms{};
    std::size_t peak_limit{};
};

[[nodiscard]] std::vector<SelectionBudget> build_selection_budgets(
    const std::vector<Candidate>& candidates,
    const DifficultyProfile& profile,
    const SongStructure& structure,
    const double duration_seconds,
    const BeatGrid& grid
) {
    std::vector<SelectionBudget> budgets;
    if (!structure.used || structure.sections.empty()) {
        budgets.push_back({
            0.0,
            duration_seconds * 1'000.0 + 1.0,
            adaptive_confidence_threshold(
                candidates, profile, duration_seconds
            ),
            profile.minimum_confidence,
            profile.minimum_spacing_ms,
            profile.peak_nps,
        });
        return budgets;
    }

    budgets.reserve(structure.sections.size());
    std::size_t candidate_begin = 0U;
    for (const auto& section : structure.sections) {
        while (candidate_begin < candidates.size()
               && candidates[candidate_begin].time_ms < section.start_ms) {
            ++candidate_begin;
        }
        std::size_t candidate_end = candidate_begin;
        std::vector<float> priorities;
        while (candidate_end < candidates.size()
               && candidates[candidate_end].time_ms < section.end_ms) {
            priorities.push_back(structural_candidate_priority(
                candidates[candidate_end], grid, structure
            ));
            ++candidate_end;
        }

        const double section_seconds = (std::max)(
            0.001,
            (section.end_ms - section.start_ms) / 1'000.0
        );
        const double density_scale = std::clamp(
            static_cast<double>(section.density_scale),
            0.52,
            1.38
        );
        double base_retention = 0.82;
        if (profile.name == "easy") {
            base_retention = 0.34;
        } else if (profile.name == "normal") {
            base_retention = 0.52;
        } else if (profile.name == "hard") {
            base_retention = 0.68;
        } else if (profile.name == "insane") {
            base_retention = 0.96;
        }
        const double structural_position = std::clamp(
            (density_scale - 0.52) / (1.38 - 0.52),
            0.0,
            1.0
        );
        const double retention = std::clamp(
            base_retention
                + (1.0 - base_retention) * structural_position,
            0.0,
            1.0
        );
        const auto desired_by_nps = priorities.empty()
            ? 0LL
            : std::clamp(
                std::llround(
                    profile.target_nps * density_scale * section_seconds
                ),
                1LL,
                static_cast<long long>(priorities.size())
            );
        const auto desired_by_structure = priorities.empty()
            ? 0LL
            : std::clamp(
                std::llround(
                    static_cast<double>(priorities.size()) * retention
                ),
                1LL,
                static_cast<long long>(priorities.size())
            );
        const std::size_t desired = static_cast<std::size_t>((std::min)(
            desired_by_nps,
            desired_by_structure
        ));

        float threshold = profile.minimum_confidence;
        if (!priorities.empty() && desired < priorities.size()) {
            const auto position = priorities.begin()
                + static_cast<std::ptrdiff_t>(priorities.size() - desired);
            std::nth_element(priorities.begin(), position, priorities.end());
            threshold = *position * 0.94F;
        } else if (!priorities.empty()) {
            threshold = *std::min_element(priorities.begin(), priorities.end())
                * 0.94F;
        }

        // Quiet sections should not be filled with weak detector noise merely
        // to satisfy a numeric NPS target. Drops/choruses are allowed a
        // slightly lower raw-confidence floor because corroborated beat/ML
        // evidence is already represented in the structural priority.
        const float confidence_scale = std::clamp(
            1.10F - section.density_scale * 0.12F,
            0.88F,
            1.06F
        );
        const float minimum_confidence =
            profile.minimum_confidence * confidence_scale;
        const double spacing = profile.minimum_spacing_ms / std::sqrt(
            std::clamp(density_scale, 0.65, 1.35)
        );
        const auto peak_limit = static_cast<std::size_t>((std::max)(
            1.0,
            std::round(
                static_cast<double>(profile.peak_nps)
                * std::clamp(density_scale, 0.72, 1.32)
            )
        ));
        budgets.push_back({
            section.start_ms,
            section.end_ms,
            threshold,
            minimum_confidence,
            spacing,
            peak_limit,
        });
        candidate_begin = candidate_end;
    }
    return budgets;
}

[[nodiscard]] const SelectionBudget& selection_budget_at(
    const std::vector<SelectionBudget>& budgets,
    const double time_ms
) {
    const auto iterator = std::upper_bound(
        budgets.begin(), budgets.end(), time_ms,
        [](const double time, const SelectionBudget& budget) {
            return time < budget.start_ms;
        }
    );
    if (iterator == budgets.begin()) {
        return budgets.front();
    }
    return *std::prev(iterator);
}

[[nodiscard]] std::uint16_t preferred_lane(
    const Candidate& candidate,
    const std::uint16_t key_count
) {
    if (key_count <= 1U) {
        return 0U;
    }
    double normalized = 0.5;
    if (candidate.peak_hz >= 65.0F) {
        const double low = std::log2(65.0);
        const double high = std::log2(2'000.0);
        normalized = (std::log2(static_cast<double>(candidate.peak_hz)) - low)
            / (high - low);
    } else if (candidate.centroid_hz > 0.0F) {
        normalized = static_cast<double>(candidate.centroid_hz) / 8'000.0;
    }
    if (candidate.source == CandidateSource::low
        || candidate.source == CandidateSource::bass) {
        normalized *= 0.55;
    } else if (candidate.source == CandidateSource::high) {
        normalized = 0.45 + normalized * 0.55;
    } else if (candidate.source == CandidateSource::drums) {
        // Preserve spectral movement inside the separated drum stem: kicks lean
        // low/left, hats lean high/right, while snares remain central.
        if (candidate.centroid_hz < 700.0F) {
            normalized *= 0.70;
        } else if (candidate.centroid_hz > 3'200.0F) {
            normalized = 0.35 + normalized * 0.65;
        }
    }
    normalized = std::clamp(normalized, 0.0, 1.0);
    return static_cast<std::uint16_t>(std::llround(
        normalized * static_cast<double>(key_count - 1U)
    ));
}

[[nodiscard]] std::uint16_t choose_lane(
    const Candidate& candidate,
    const std::uint16_t key_count,
    const std::vector<Note>& notes
) {
    const auto preferred = preferred_lane(candidate, key_count);
    const Note* last = notes.empty() ? nullptr : &notes.back();
    const Note* previous = notes.size() < 2U ? nullptr : &notes[notes.size() - 2U];
    double best_cost = std::numeric_limits<double>::infinity();
    std::uint16_t best_lane = preferred;
    for (std::uint16_t lane = 0U; lane < key_count; ++lane) {
        double cost = std::abs(
            static_cast<double>(lane) - static_cast<double>(preferred)
        ) * 0.52;
        if (last != nullptr) {
            const double delta = candidate.time_ms - last->time_ms;
            if (lane == last->lane) {
                cost += delta < 90.0 ? 5.0 : delta < 160.0 ? 2.0 : 0.35;
            }
            cost += std::abs(
                static_cast<double>(lane) - static_cast<double>(last->lane)
            ) * (delta < 100.0 ? 0.22 : 0.08);
            if (key_count >= 4U && delta < 125.0) {
                const bool same_hand = (lane < key_count / 2U)
                    == (last->lane < key_count / 2U);
                cost += same_hand ? 0.65 : -0.18;
            }
        }
        if (previous != nullptr && last != nullptr
            && lane == previous->lane && last->lane != previous->lane) {
            cost -= 0.16; // controlled alternation feels natural in streams
        }
        const std::uint64_t deterministic =
            static_cast<std::uint64_t>(candidate.time_ms * 1'000.0)
            ^ (static_cast<std::uint64_t>(lane) * 0x9E3779B97F4A7C15ULL);
        cost += static_cast<double>(deterministic & 0xFFU) / 65'536.0;
        if (cost < best_cost) {
            best_cost = cost;
            best_lane = lane;
        }
    }
    return best_lane;
}


enum class HandSide : std::uint8_t {
    left,
    right,
};

struct HandTracker final {
    bool valid{};
    std::uint16_t lane{};
    double time_ms{};
};

struct LaneTraceNode final {
    std::size_t previous{};
    std::uint16_t lane{};
};

struct BeamLaneState final {
    double cost{};
    std::size_t trace_index{(std::numeric_limits<std::size_t>::max)()};
    bool has_last{};
    bool has_previous{};
    std::uint16_t last_lane{};
    std::uint16_t previous_lane{};
    std::uint16_t last_preferred{};
    double last_time_ms{};
    double previous_time_ms{};
    std::uint8_t repeated_lane_count{};
    std::uint8_t last_hand{2U};
    HandTracker left_hand;
    HandTracker right_hand;
};

struct LaneExpansion final {
    BeamLaneState state;
    std::size_t parent_index{};
    std::uint16_t lane{};
};

[[nodiscard]] std::size_t lane_beam_width_for(
    const AutoChartMode mode,
    const std::uint16_t key_count
) noexcept {
    std::size_t width = 48U;
    switch (mode) {
    case AutoChartMode::fast:
        width = 20U;
        break;
    case AutoChartMode::maximum:
        width = 96U;
        break;
    case AutoChartMode::accurate:
    default:
        width = 48U;
        break;
    }
    if (key_count >= 10U) {
        width = (std::max)(std::size_t{20U}, width / 2U);
    } else if (key_count >= 7U) {
        width = (std::max)(std::size_t{24U}, width * 3U / 4U);
    }
    return width;
}

[[nodiscard]] HandSide hand_for_lane(
    const std::uint16_t lane,
    const std::uint16_t key_count,
    const BeamLaneState& state
) noexcept {
    if (key_count <= 1U) {
        return HandSide::left;
    }
    const auto middle = static_cast<std::uint16_t>(key_count / 2U);
    if (key_count % 2U != 0U && lane == middle) {
        // The centre key of odd-key modes is genuinely ambidextrous. Assign it
        // to the opposite hand from the most recent event to avoid inventing
        // artificial one-hand jack strain.
        return state.last_hand == 0U ? HandSide::right : HandSide::left;
    }
    return lane < middle ? HandSide::left : HandSide::right;
}

[[nodiscard]] double lane_transition_cost(
    const Candidate& candidate,
    const std::uint16_t lane,
    const std::uint16_t key_count,
    const BeamLaneState& state,
    const SongStructure& structure,
    const BeatGrid& grid
) {
    const auto preferred = preferred_lane(candidate, key_count);
    const double lane_span = (std::max)(
        1.0,
        static_cast<double>(key_count > 1U ? key_count - 1U : 1U)
    );
    const double preference_distance = std::abs(
        static_cast<double>(lane) - static_cast<double>(preferred)
    ) / lane_span;

    double movement_scale = 1.0;
    if (const auto* section = section_at(structure, candidate.time_ms);
        section != nullptr) {
        switch (section->kind) {
        case StructuralSectionKind::drop:
        case StructuralSectionKind::chorus:
            movement_scale = 0.78;
            break;
        case StructuralSectionKind::buildup:
            movement_scale = 0.88;
            break;
        case StructuralSectionKind::intro:
        case StructuralSectionKind::breakdown:
        case StructuralSectionKind::outro:
            movement_scale = 1.18;
            break;
        case StructuralSectionKind::bridge:
            movement_scale = 0.96;
            break;
        case StructuralSectionKind::verse:
            movement_scale = 1.04;
            break;
        }
    }

    double cost = preference_distance * 0.92;
    if (!state.has_last) {
        // Start close to the inferred pitch/instrument position, but do not
        // force the extreme edge on the first note.
        const double center = static_cast<double>(key_count - 1U) * 0.5;
        cost += std::abs(static_cast<double>(lane) - center)
            / lane_span * 0.08;
        return cost;
    }

    const double delta_ms = candidate.time_ms - state.last_time_ms;
    const double lane_jump = std::abs(
        static_cast<double>(lane) - static_cast<double>(state.last_lane)
    );
    if (lane == state.last_lane) {
        if (delta_ms < 70.0) {
            cost += 7.5;
        } else if (delta_ms < 105.0) {
            cost += 4.2;
        } else if (delta_ms < 165.0) {
            cost += 1.75;
        } else {
            cost += 0.24;
        }
        cost += static_cast<double>(state.repeated_lane_count) * 0.52;
        if (phrase_alignment(structure, candidate.time_ms, grid.period_ms)
            >= 0.72F) {
            cost += 0.45;
        }
    }

    const double jump_weight = delta_ms < 90.0 ? 0.24
        : delta_ms < 150.0 ? 0.13 : 0.055;
    cost += lane_jump * jump_weight * movement_scale;

    const HandSide hand = hand_for_lane(lane, key_count, state);
    const auto& same_tracker = hand == HandSide::left
        ? state.left_hand
        : state.right_hand;
    if (same_tracker.valid) {
        const double hand_delta = candidate.time_ms - same_tracker.time_ms;
        const double hand_travel = std::abs(
            static_cast<double>(lane)
            - static_cast<double>(same_tracker.lane)
        );
        if (hand_delta < 65.0) {
            cost += 3.2 + hand_travel * 0.34;
        } else if (hand_delta < 105.0) {
            cost += 1.15 + hand_travel * 0.18;
        } else if (hand_delta < 160.0) {
            cost += hand_travel * 0.08;
        }
    }

    const std::uint8_t hand_id = hand == HandSide::left ? 0U : 1U;
    if (delta_ms < 150.0 && key_count >= 4U) {
        if (state.last_hand == hand_id) {
            cost += 0.42;
        } else if (state.last_hand <= 1U) {
            cost -= 0.14;
        }
    }

    if (state.has_previous
        && lane == state.previous_lane
        && state.last_lane != state.previous_lane
        && delta_ms < 190.0) {
        cost -= 0.28; // ABAB streams are comfortable and visually coherent.
    }

    const int preferred_direction =
        preferred > state.last_preferred ? 1
        : preferred < state.last_preferred ? -1 : 0;
    const int actual_direction =
        lane > state.last_lane ? 1 : lane < state.last_lane ? -1 : 0;
    if (preferred_direction != 0 && actual_direction != 0) {
        if (preferred_direction == actual_direction) {
            cost -= 0.10;
        } else {
            cost += 0.62;
        }
    }

    // Tiny deterministic tie breaker. Never use RNG here: the same audio and
    // options must generate byte-for-byte stable lane paths.
    const std::uint64_t deterministic =
        static_cast<std::uint64_t>((std::max)(0.0, candidate.time_ms) * 1'000.0)
        ^ (static_cast<std::uint64_t>(lane) * 0x9E3779B97F4A7C15ULL)
        ^ (static_cast<std::uint64_t>(state.last_lane) * 0xBF58476D1CE4E5B9ULL);
    cost += static_cast<double>(deterministic & 0xFFU) / 262'144.0;
    return cost;
}

[[nodiscard]] BeamLaneState advanced_lane_state(
    const BeamLaneState& parent,
    const Candidate& candidate,
    const std::uint16_t lane,
    const std::uint16_t key_count,
    const double cost
) {
    BeamLaneState next = parent;
    next.cost = cost;
    next.has_previous = parent.has_last;
    next.previous_lane = parent.last_lane;
    next.previous_time_ms = parent.last_time_ms;
    next.has_last = true;
    next.last_lane = lane;
    next.last_time_ms = candidate.time_ms;
    next.last_preferred = preferred_lane(candidate, key_count);
    next.repeated_lane_count =
        parent.has_last && lane == parent.last_lane
            ? static_cast<std::uint8_t>((std::min)(
                255U,
                static_cast<unsigned int>(parent.repeated_lane_count) + 1U
            ))
            : 1U;
    const HandSide hand = hand_for_lane(lane, key_count, parent);
    if (hand == HandSide::left) {
        next.left_hand = {true, lane, candidate.time_ms};
        next.last_hand = 0U;
    } else {
        next.right_hand = {true, lane, candidate.time_ms};
        next.last_hand = 1U;
    }
    return next;
}

[[nodiscard]] std::vector<std::uint16_t> assign_lanes_beam(
    const std::vector<Candidate>& candidates,
    const std::uint16_t key_count,
    const AutoChartMode mode,
    const SongStructure& structure,
    const BeatGrid& grid,
    std::uint32_t& used_width
) {
    used_width = 0U;
    if (candidates.empty()) {
        return {};
    }
    if (key_count <= 1U) {
        used_width = 1U;
        return std::vector<std::uint16_t>(candidates.size(), 0U);
    }

    const std::size_t requested_width = lane_beam_width_for(mode, key_count);
    used_width = static_cast<std::uint32_t>(requested_width);
    constexpr std::size_t no_trace =
        (std::numeric_limits<std::size_t>::max)();

    std::vector<LaneTraceNode> trace;
    constexpr std::size_t maximum_trace_reserve = 4'000'000U;
    const std::size_t reserve_per_candidate =
        (std::min)(requested_width, std::size_t{24U});
    const std::size_t trace_reserve =
        reserve_per_candidate != 0U
            && candidates.size()
                > maximum_trace_reserve / reserve_per_candidate
        ? maximum_trace_reserve
        : (std::min)(
            maximum_trace_reserve,
            candidates.size() * reserve_per_candidate
        );
    trace.reserve(trace_reserve);

    std::vector<BeamLaneState> current(1U);
    current.front().trace_index = no_trace;

    for (const auto& candidate : candidates) {
        std::vector<LaneExpansion> expansions;
        expansions.reserve(
            current.size() * static_cast<std::size_t>(key_count)
        );
        for (std::size_t parent_index = 0U;
             parent_index < current.size();
             ++parent_index) {
            const auto& parent = current[parent_index];
            for (std::uint16_t lane = 0U; lane < key_count; ++lane) {
                const double transition = lane_transition_cost(
                    candidate, lane, key_count, parent, structure, grid
                );
                expansions.push_back({
                    advanced_lane_state(
                        parent,
                        candidate,
                        lane,
                        key_count,
                        parent.cost + transition
                    ),
                    parent_index,
                    lane,
                });
            }
        }

        const auto compare = [](const LaneExpansion& left_value,
                                const LaneExpansion& right_value) {
            if (left_value.state.cost != right_value.state.cost) {
                return left_value.state.cost < right_value.state.cost;
            }
            if (left_value.lane != right_value.lane) {
                return left_value.lane < right_value.lane;
            }
            return left_value.parent_index < right_value.parent_index;
        };
        const std::size_t keep = (std::min)(requested_width, expansions.size());
        if (keep < expansions.size()) {
            std::partial_sort(
                expansions.begin(),
                expansions.begin() + static_cast<std::ptrdiff_t>(keep),
                expansions.end(),
                compare
            );
            expansions.resize(keep);
        } else {
            std::sort(expansions.begin(), expansions.end(), compare);
        }

        std::vector<BeamLaneState> next;
        next.reserve(expansions.size());
        for (auto& expansion : expansions) {
            const std::size_t previous_trace =
                current[expansion.parent_index].trace_index;
            trace.push_back({previous_trace, expansion.lane});
            expansion.state.trace_index = trace.size() - 1U;
            next.push_back(expansion.state);
        }
        current = std::move(next);
    }

    if (current.empty()) {
        throw std::runtime_error("AutoChart lane beam search produced no state");
    }
    const auto best = std::min_element(
        current.begin(), current.end(),
        [](const BeamLaneState& left_value, const BeamLaneState& right_value) {
            return left_value.cost < right_value.cost;
        }
    );
    std::vector<std::uint16_t> lanes(candidates.size(), 0U);
    std::size_t trace_index = best->trace_index;
    for (std::size_t reverse_index = candidates.size();
         reverse_index > 0U;
         --reverse_index) {
        if (trace_index == no_trace || trace_index >= trace.size()) {
            throw std::runtime_error("AutoChart lane beam trace is incomplete");
        }
        const auto& node = trace[trace_index];
        lanes[reverse_index - 1U] = node.lane;
        trace_index = node.previous;
    }
    return lanes;
}

[[nodiscard]] std::uint16_t choose_chord_lane(
    const std::uint16_t primary_lane,
    const Candidate& candidate,
    const std::uint16_t key_count
) noexcept {
    if (key_count <= 1U) {
        return primary_lane;
    }
    const auto preferred = preferred_lane(candidate, key_count);
    std::uint16_t best_lane = primary_lane;
    double best_cost = std::numeric_limits<double>::infinity();
    const bool primary_left = primary_lane < key_count / 2U;
    for (std::uint16_t lane = 0U; lane < key_count; ++lane) {
        if (lane == primary_lane) {
            continue;
        }
        const bool lane_left = lane < key_count / 2U;
        double cost = std::abs(
            static_cast<double>(lane) - static_cast<double>(preferred)
        ) * 0.10;
        if (key_count >= 4U && lane_left == primary_left) {
            cost += 1.2;
        }
        const double spread = std::abs(
            static_cast<double>(lane) - static_cast<double>(primary_lane)
        );
        cost -= (std::min)(spread, 4.0) * 0.18;
        if (cost < best_cost) {
            best_cost = cost;
            best_lane = lane;
        }
    }
    return best_lane == primary_lane
        ? static_cast<std::uint16_t>((primary_lane + 1U) % key_count)
        : best_lane;
}

[[nodiscard]] double calculate_peak_nps(const std::vector<Note>& notes) {
    std::deque<double> active;
    std::size_t peak = 0U;
    for (const auto& note : notes) {
        while (!active.empty() && active.front() < note.time_ms - 1'000.0) {
            active.pop_front();
        }
        active.push_back(note.time_ms);
        peak = (std::max)(peak, active.size());
    }
    return static_cast<double>(peak);
}

void trim_overlapping_sustains(std::vector<Note>& notes, const std::uint16_t key_count) {
    std::vector<std::optional<std::size_t>> last(key_count);
    for (std::size_t index = 0U; index < notes.size(); ++index) {
        auto& note = notes[index];
        if (note.lane >= key_count) {
            continue;
        }
        auto& previous_index = last[note.lane];
        if (previous_index.has_value()) {
            auto& previous = notes[*previous_index];
            if (previous.duration_ms > 0.0
                && previous.end_time_ms() > note.time_ms - 35.0) {
                previous.duration_ms = (std::max)(
                    0.0,
                    note.time_ms - 35.0 - previous.time_ms
                );
                if (previous.duration_ms < 160.0) {
                    previous.duration_ms = 0.0;
                }
            }
        }
        previous_index = index;
    }
}


[[nodiscard]] std::uint32_t evidence_count(std::uint32_t evidence) noexcept {
    std::uint32_t count = 0U;
    while (evidence != 0U) {
        count += evidence & 1U;
        evidence >>= 1U;
    }
    return count;
}

[[nodiscard]] std::string_view candidate_source_name(const CandidateSource source) noexcept {
    switch (source) {
    case CandidateSource::low: return "low";
    case CandidateSource::mid: return "mid";
    case CandidateSource::high: return "high";
    case CandidateSource::mixed: return "mixed";
    case CandidateSource::drums: return "drums";
    case CandidateSource::bass: return "bass";
    case CandidateSource::vocals: return "vocals";
    case CandidateSource::other: return "other";
    }
    return "unknown";
}

[[nodiscard]] std::string evidence_text(const std::uint32_t evidence) {
    std::string value;
    const auto append = [&](const std::string_view text) {
        if (!value.empty()) {
            value.push_back('+');
        }
        value.append(text);
    };
    if ((evidence & evidence_dsp) != 0U) append("DSP");
    if ((evidence & evidence_video) != 0U) append("video");
    if ((evidence & evidence_stem) != 0U) append("stem");
    if ((evidence & evidence_pitch) != 0U) append("pitch");
    if ((evidence & evidence_neural_beat) != 0U) append("beat-ML");
    if ((evidence & evidence_drum_ml) != 0U) append("drum-ML");
    if ((evidence & evidence_vocal_ml) != 0U) append("vocal-ML");
    return value.empty() ? std::string{"none"} : value;
}

[[nodiscard]] float review_priority_for(
    const Candidate& candidate,
    const float structural_priority,
    const double quantization_shift_ms,
    const bool jack_risk,
    const bool chord
) noexcept {
    const float confidence_uncertainty = 1.0F - std::clamp(candidate.confidence, 0.0F, 1.0F);
    const float timing_uncertainty = 1.0F - std::clamp(candidate.beat_alignment, 0.0F, 1.0F);
    const float source_uncertainty = evidence_count(candidate.evidence) <= 1U ? 1.0F : 0.0F;
    const float quantization_uncertainty = static_cast<float>(std::clamp(
        std::abs(quantization_shift_ms) / 36.0,
        0.0,
        1.0
    ));
    const float priority_uncertainty = 1.0F - std::clamp(structural_priority, 0.0F, 1.0F);
    return std::clamp(
        confidence_uncertainty * 0.42F
            + timing_uncertainty * 0.15F
            + source_uncertainty * 0.14F
            + quantization_uncertainty * 0.13F
            + priority_uncertainty * 0.08F
            + (jack_risk ? 0.06F : 0.0F)
            + (chord ? 0.02F : 0.0F),
        0.0F,
        1.0F
    );
}

[[nodiscard]] double difficulty_quality_score(
    const double mean_confidence,
    const double beat_confidence,
    const SongStructure& structure,
    const double evidence_score,
    const double low_confidence_ratio,
    const double risk_ratio,
    const double mean_quantization_shift_ms
) noexcept {
    const double structural = structure.used
        ? std::clamp(structure.confidence, 0.0, 1.0)
        : 0.72;
    const double quantization_quality = 1.0 - std::clamp(
        mean_quantization_shift_ms / 30.0,
        0.0,
        1.0
    );
    const double score =
        std::clamp(mean_confidence, 0.0, 1.0) * 0.34
        + std::clamp(beat_confidence, 0.0, 1.0) * 0.20
        + structural * 0.10
        + std::clamp(evidence_score, 0.0, 1.0) * 0.12
        + (1.0 - std::clamp(low_confidence_ratio, 0.0, 1.0)) * 0.10
        + (1.0 - std::clamp(risk_ratio, 0.0, 1.0)) * 0.08
        + quantization_quality * 0.06;
    return std::clamp(score * 100.0, 0.0, 100.0);
}

[[nodiscard]] DifficultyBuild build_difficulty(
    const std::vector<Candidate>& candidates,
    const BeatGrid& grid,
    const SongStructure& structure,
    const double duration_seconds,
    const DifficultyProfile& profile,
    const AutoChartOptions& options,
    const std::string& audio_filename
) {
    DifficultyBuild result;
    auto& chart = result.chart;
    chart.title = options.title;
    chart.artist = options.artist;
    chart.charter = options.charter;
    chart.difficulty = std::string(profile.name);
    chart.key_count = options.key_count;
    chart.chart_scroll_speed = options.scroll_speed;
    chart.audio.instrumental = audio_filename;
    chart.tempos = grid.tempos;
    if (chart.tempos.empty()) {
        chart.tempos.push_back({0.0, grid.bpm, 4U, 4U});
    }

    const auto budgets = build_selection_budgets(
        candidates, profile, structure, duration_seconds, grid
    );
    std::vector<Candidate> selected;
    std::vector<float> selected_priority;
    std::vector<double> selected_quantization_shift;
    selected.reserve(candidates.size());
    selected_priority.reserve(candidates.size());
    selected_quantization_shift.reserve(candidates.size());

    double last_time = -std::numeric_limits<double>::infinity();
    std::deque<double> recent;
    for (const auto& candidate : candidates) {
        const auto& budget = selection_budget_at(budgets, candidate.time_ms);
        const float priority = structure.used
            ? structural_candidate_priority(candidate, grid, structure)
            : candidate.confidence;
        const float downbeat = downbeat_alignment(grid, candidate.time_ms);
        const bool protected_accent = downbeat >= 0.88F
            || phrase_alignment(structure, candidate.time_ms, grid.period_ms)
                >= 0.88F;
        if (candidate.confidence < budget.minimum_confidence
            && !(protected_accent
                 && candidate.confidence
                     >= budget.minimum_confidence * 0.86F)) {
            continue;
        }
        if (priority < budget.priority_threshold
            && !(protected_accent
                 && priority >= budget.priority_threshold * 0.90F)) {
            continue;
        }

        Candidate adjusted = candidate;
        const double original_time_ms = adjusted.time_ms;
        adjusted.time_ms = quantize_time(
            adjusted.time_ms,
            grid,
            profile.subdivisions_per_beat
        );
        const double quantization_shift_ms = adjusted.time_ms - original_time_ms;

        if (adjusted.time_ms - last_time < budget.minimum_spacing_ms) {
            if (!selected.empty()
                && priority > selected_priority.back() + 0.10F) {
                const double old_time = selected.back().time_ms;
                selected.back() = adjusted;
                selected_priority.back() = priority;
                selected_quantization_shift.back() = quantization_shift_ms;
                last_time = adjusted.time_ms;
                if (!recent.empty()
                    && std::abs(recent.back() - old_time) < 0.001) {
                    recent.back() = adjusted.time_ms;
                }
            }
            continue;
        }

        while (!recent.empty()
               && recent.front() < adjusted.time_ms - 1'000.0) {
            recent.pop_front();
        }
        if (recent.size() >= budget.peak_limit) {
            continue;
        }

        selected.push_back(adjusted);
        selected_priority.push_back(priority);
        selected_quantization_shift.push_back(quantization_shift_ms);
        recent.push_back(adjusted.time_ms);
        last_time = adjusted.time_ms;
    }

    std::vector<std::uint16_t> beam_lanes;
    if (options.beam_lane_optimizer && !selected.empty()) {
        beam_lanes = assign_lanes_beam(
            selected,
            options.key_count,
            options.mode,
            structure,
            grid,
            result.beam_width
        );
        result.beam_used = true;
    }

    double confidence_sum = 0.0;
    double evidence_sum = 0.0;
    double quantization_shift_sum = 0.0;
    std::uint64_t low_confidence_count = 0U;
    std::uint64_t uncertain_count = 0U;
    std::uint64_t high_priority_count = 0U;
    std::uint64_t risk_count = 0U;
    std::vector<double> previous_lane_time(options.key_count, -10'000.0);
    const std::size_t review_limit = options.write_review_artifacts
        ? static_cast<std::size_t>(options.maximum_review_notes)
        : 0U;
    BoundedReviewCollector review_collector(review_limit);
    double last_chord_time = -10'000.0;
    for (std::size_t index = 0U; index < selected.size(); ++index) {
        const auto& candidate = selected[index];
        const std::uint16_t lane = result.beam_used
            ? beam_lanes[index]
            : choose_lane(candidate, options.key_count, chart.notes);
        const double sustain = candidate.sustain_ms >= profile.sustain_minimum_ms
            ? (std::min)(candidate.sustain_ms, profile.maximum_sustain_ms)
            : 0.0;
        const bool jack_risk = lane < previous_lane_time.size()
            && candidate.time_ms - previous_lane_time[lane] < 105.0;
        if (lane < previous_lane_time.size()) {
            previous_lane_time[lane] = candidate.time_ms;
        }
        chart.notes.push_back({
            candidate.time_ms,
            sustain,
            lane,
            NoteOwner::player,
            "normal",
            0U,
        });
        confidence_sum += candidate.confidence;
        evidence_sum += std::clamp(
            static_cast<double>(evidence_count(candidate.evidence)) / 3.0,
            0.0,
            1.0
        );
        quantization_shift_sum += std::abs(selected_quantization_shift[index]);
        low_confidence_count += candidate.confidence < 0.70F ? 1U : 0U;
        risk_count += jack_risk ? 1U : 0U;
        const float note_review_priority = review_priority_for(
            candidate,
            selected_priority[index],
            selected_quantization_shift[index],
            jack_risk,
            false
        );
        uncertain_count += note_review_priority >= 0.40F ? 1U : 0U;
        high_priority_count += note_review_priority >= high_priority_review_threshold
            ? 1U : 0U;
        review_collector.add({
            candidate.time_ms,
            sustain,
            selected_quantization_shift[index],
            lane,
            candidate.confidence,
            selected_priority[index],
            candidate.beat_alignment,
            note_review_priority,
            candidate.evidence,
            candidate.source,
            false,
            jack_risk,
        });

        float chord_threshold = profile.chord_confidence;
        const float accent = downbeat_alignment(grid, candidate.time_ms);
        if (const auto* section = section_at(structure, candidate.time_ms);
            section != nullptr
            && (section->kind == StructuralSectionKind::drop
                || section->kind == StructuralSectionKind::chorus)) {
            chord_threshold -= 0.035F;
        }
        chord_threshold -= accent * 0.025F;
        chord_threshold = (std::max)(0.68F, chord_threshold);

        if (options.key_count >= 2U && candidate.polyphonic
            && candidate.confidence >= chord_threshold
            && candidate.time_ms - last_chord_time >= 120.0) {
            const std::uint16_t second_lane = choose_chord_lane(
                lane, candidate, options.key_count
            );
            const double chord_sustain = sustain > 0.0 ? sustain * 0.65 : 0.0;
            const bool chord_jack_risk = second_lane < previous_lane_time.size()
                && candidate.time_ms - previous_lane_time[second_lane] < 105.0;
            if (second_lane < previous_lane_time.size()) {
                previous_lane_time[second_lane] = candidate.time_ms;
            }
            chart.notes.push_back({
                candidate.time_ms,
                chord_sustain,
                second_lane,
                NoteOwner::player,
                "normal",
                0U,
            });
            risk_count += chord_jack_risk ? 1U : 0U;
            const float chord_review_priority = review_priority_for(
                candidate,
                selected_priority[index],
                selected_quantization_shift[index],
                chord_jack_risk,
                true
            );
            uncertain_count += chord_review_priority >= 0.40F ? 1U : 0U;
            high_priority_count += chord_review_priority >= high_priority_review_threshold
                ? 1U : 0U;
            low_confidence_count += candidate.confidence < 0.70F ? 1U : 0U;
            review_collector.add({
                candidate.time_ms,
                chord_sustain,
                selected_quantization_shift[index],
                second_lane,
                candidate.confidence,
                selected_priority[index],
                candidate.beat_alignment,
                chord_review_priority,
                candidate.evidence,
                candidate.source,
                true,
                chord_jack_risk,
            });
            last_chord_time = candidate.time_ms;
        }
    }

    std::stable_sort(chart.notes.begin(), chart.notes.end(), [](const Note& a, const Note& b) {
        if (a.time_ms != b.time_ms) {
            return a.time_ms < b.time_ms;
        }
        return a.lane < b.lane;
    });
    trim_overlapping_sustains(chart.notes, options.key_count);
    chart.normalize();

    for (const auto& issue : validate_chart(chart)) {
        if (issue.severity == ValidationSeverity::error) {
            throw std::runtime_error(
                "generated chart validation failed: " + issue.message
            );
        }
    }
    result.mean_confidence = selected.empty()
        ? 0.0
        : confidence_sum / static_cast<double>(selected.size());
    result.review.difficulty = std::string(profile.name);
    result.review.note_count = static_cast<std::uint64_t>(chart.notes.size());
    result.review.low_confidence_count = low_confidence_count;
    result.review.uncertain_count = uncertain_count;
    result.review.high_priority_count = high_priority_count;
    result.review.notes = review_collector.take();
    const double selected_count = static_cast<double>((std::max)(selected.size(), std::size_t{1U}));
    const double chart_note_count = static_cast<double>((std::max)(chart.notes.size(), std::size_t{1U}));
    result.review.quality_score = difficulty_quality_score(
        result.mean_confidence,
        grid.confidence,
        structure,
        evidence_sum / selected_count,
        static_cast<double>(low_confidence_count) / chart_note_count,
        static_cast<double>(risk_count) / chart_note_count,
        quantization_shift_sum / selected_count
    );
    result.average_nps = duration_seconds > 0.0
        ? static_cast<double>(chart.notes.size()) / duration_seconds
        : 0.0;
    result.peak_nps = calculate_peak_nps(chart.notes);
    return result;
}

void write_native_chart(const Chart& chart, const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create generated chart: " + path_utf8(path));
    }
    output << std::setprecision(12);
    output << "{\n"
           << "  \"format\": \"pulseforge-chart\",\n"
           << "  \"version\": 1,\n"
           << "  \"song\": {\n"
           << "    \"title\": \"" << json_escape(chart.title) << "\",\n"
           << "    \"artist\": \"" << json_escape(chart.artist) << "\",\n"
           << "    \"charter\": \"" << json_escape(chart.charter) << "\",\n"
           << "    \"difficulty\": \"" << json_escape(chart.difficulty) << "\",\n"
           << "    \"keyCount\": " << chart.key_count << ",\n"
           << "    \"scrollSpeed\": " << chart.chart_scroll_speed << "\n"
           << "  },\n"
           << "  \"audio\": {\n"
           << "    \"instrumental\": \""
           << json_escape(path_utf8(chart.audio.instrumental)) << "\",\n"
           << "    \"vocals\": []\n"
           << "  },\n"
           << "  \"tempos\": [\n";
    for (std::size_t index = 0U; index < chart.tempos.size(); ++index) {
        const auto& tempo = chart.tempos[index];
        output << "    {\"time\": " << tempo.time_ms
               << ", \"bpm\": " << tempo.bpm
               << ", \"numerator\": " << tempo.numerator
               << ", \"denominator\": " << tempo.denominator << "}";
        output << (index + 1U == chart.tempos.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"notes\": [\n";
    for (std::size_t index = 0U; index < chart.notes.size(); ++index) {
        const auto& note = chart.notes[index];
        output << "    {\"time\": " << note.time_ms
               << ", \"duration\": " << note.duration_ms
               << ", \"lane\": " << note.lane
               << ", \"owner\": \""
               << (note.owner == NoteOwner::player ? "player" : "opponent")
               << "\", \"kind\": \"" << json_escape(note.kind) << "\"}";
        output << (index + 1U == chart.notes.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"events\": []\n}\n";
    if (!output) {
        throw std::runtime_error("I/O error while writing generated chart");
    }
}

[[nodiscard]] std::string default_title(const std::filesystem::path& media) {
    auto value = path_utf8(media.stem());
    std::replace(value.begin(), value.end(), '_', ' ');
    return value.empty() ? std::string{"AutoChart Song"} : value;
}

[[nodiscard]] std::filesystem::path choose_mod_root(
    const std::filesystem::path& media,
    const AutoChartOptions& options,
    const std::string& title
) {
    if (!options.output_root.empty()) {
        return options.output_root;
    }
    const auto slug = sanitize_slug(options.mod_id.empty() ? title : options.mod_id);
    if (options.add_to_mods) {
        auto root = options.mods_root;
        if (root.empty()) {
            root = "mods";
        }
        return root / slug;
    }
    return media.parent_path() / (slug + "-autochart");
}

void ensure_output_directory(
    const std::filesystem::path& root,
    const bool overwrite
) {
    std::error_code error;
    if (std::filesystem::exists(root, error) && !error && !overwrite) {
        const auto begin = std::filesystem::directory_iterator(root, error);
        if (!error && begin != std::filesystem::directory_iterator{}) {
            throw std::runtime_error(
                "AutoChart output already exists; pass --overwrite: " + path_utf8(root)
            );
        }
    }
    std::filesystem::create_directories(root, error);
    if (error) {
        throw std::runtime_error("cannot create AutoChart output directory");
    }
}

void enable_mod_in_list(
    const std::filesystem::path& mods_root,
    const std::string& folder_name
) {
    if (mods_root.empty() || folder_name.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(mods_root, error);
    if (error) {
        throw std::runtime_error("cannot create mods root for modsList.txt");
    }
    const auto list_path = mods_root / "modsList.txt";
    std::vector<std::string> lines;
    {
        std::ifstream input(list_path, std::ios::binary);
        std::string line;
        while (input && std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                lines.push_back(std::move(line));
            }
        }
    }
    bool found = false;
    for (auto& line : lines) {
        const auto separator = line.find('|');
        const std::string_view name = separator == std::string::npos
            ? std::string_view(line)
            : std::string_view(line).substr(0U, separator);
        if (name == folder_name) {
            line = folder_name + "|1";
            found = true;
            break;
        }
    }
    if (!found) {
        lines.push_back(folder_name + "|1");
    }
    std::ofstream output(list_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot update modsList.txt");
    }
    for (const auto& line : lines) {
        output << line << '\n';
    }
}

void write_mod_manifest(
    const std::filesystem::path& root,
    const std::string& id,
    const std::string& title,
    const std::string& entry_chart
) {
    std::ofstream output(root / "mod.json", std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create mod.json");
    }
    output << "{\n"
           << "  \"id\": \"" << json_escape(id) << "\",\n"
           << "  \"name\": \"" << json_escape(title) << " (AutoChart)\",\n"
           << "  \"version\": \"1.0.0\",\n"
           << "  \"engineApi\": \"1\",\n"
           << "  \"entryChart\": \"" << json_escape(entry_chart) << "\",\n"
           << "  \"scripts\": []\n"
           << "}\n";
}


[[nodiscard]] std::string html_escape(const std::string_view input) {
    std::string output;
    output.reserve(input.size() + input.size() / 8U);
    for (const char value : input) {
        switch (value) {
        case '&': output += "&amp;"; break;
        case '<': output += "&lt;"; break;
        case '>': output += "&gt;"; break;
        case '\"': output += "&quot;"; break;
        case '\'': output += "&#39;"; break;
        default: output.push_back(value); break;
        }
    }
    return output;
}

[[nodiscard]] std::string url_path_component(const std::string_view input) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(input.size() + input.size() / 4U);
    for (const char raw : input) {
        const auto value = static_cast<unsigned char>(raw);
        const bool unreserved =
            (value >= static_cast<unsigned char>('a')
                && value <= static_cast<unsigned char>('z'))
            || (value >= static_cast<unsigned char>('A')
                && value <= static_cast<unsigned char>('Z'))
            || (value >= static_cast<unsigned char>('0')
                && value <= static_cast<unsigned char>('9'))
            || value == static_cast<unsigned char>('-')
            || value == static_cast<unsigned char>('_')
            || value == static_cast<unsigned char>('.')
            || value == static_cast<unsigned char>('~');
        if (unreserved) {
            output.push_back(raw);
            continue;
        }
        output.push_back('%');
        output.push_back(hex[(value >> 4U) & 0x0FU]);
        output.push_back(hex[value & 0x0FU]);
    }
    return output;
}

[[nodiscard]] std::string review_reason(const ReviewNote& note) {
    std::vector<std::string_view> reasons;
    if (note.confidence < 0.70F) reasons.push_back("low confidence");
    if (note.beat_alignment < 0.55F) reasons.push_back("weak beat alignment");
    if (evidence_count(note.evidence) <= 1U) reasons.push_back("single-source evidence");
    if (std::abs(note.quantization_shift_ms) > 20.0) reasons.push_back("large timing snap");
    if (note.jack_risk) reasons.push_back("possible jack strain");
    if (note.chord) reasons.push_back("generated chord");
    if (reasons.empty()) reasons.push_back("borderline selection");
    std::string result;
    for (std::size_t index = 0U; index < reasons.size(); ++index) {
        if (index != 0U) result += ", ";
        result += reasons[index];
    }
    return result;
}

void write_review_json(
    const std::filesystem::path& path,
    const AutoChartResult& result,
    const std::vector<DifficultyReview>& reviews,
    const SongStructure& structure
) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create AutoChart review JSON");
    }
    output << std::setprecision(12)
           << "{\n  \"schema\": \"pulseforge-autochart-review-v1\",\n"
           << "  \"overallQualityScore\": " << result.overall_quality_score << ",\n"
           << "  \"beatConfidence\": " << result.beat_confidence << ",\n"
           << "  \"structureConfidence\": " << result.structure_confidence << ",\n"
           << "  \"reviewNoteCount\": " << result.review_note_count << ",\n"
           << "  \"lowConfidenceNoteCount\": " << result.low_confidence_note_count << ",\n"
           << "  \"sections\": [\n";
    for (std::size_t index = 0U; index < structure.sections.size(); ++index) {
        const auto& section = structure.sections[index];
        output << "    {\"kind\":\"" << to_string(section.kind)
               << "\",\"startMs\":" << section.start_ms
               << ",\"endMs\":" << section.end_ms
               << ",\"confidence\":" << section.confidence << "}";
        output << (index + 1U == structure.sections.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"difficulties\": [\n";
    for (std::size_t difficulty_index = 0U; difficulty_index < reviews.size(); ++difficulty_index) {
        const auto& review = reviews[difficulty_index];
        output << "    {\n      \"name\": \"" << json_escape(review.difficulty)
               << "\",\n      \"qualityScore\": " << review.quality_score
               << ",\n      \"noteCount\": " << review.note_count
               << ",\n      \"lowConfidenceCount\": " << review.low_confidence_count
               << ",\n      \"uncertainCount\": " << review.uncertain_count
               << ",\n      \"reviewNotes\": [\n";
        for (std::size_t note_index = 0U; note_index < review.notes.size(); ++note_index) {
            const auto& note = review.notes[note_index];
            output << "        {\"timeMs\":" << note.time_ms
                   << ",\"durationMs\":" << note.duration_ms
                   << ",\"lane\":" << note.lane
                   << ",\"confidence\":" << note.confidence
                   << ",\"reviewPriority\":" << note.review_priority
                   << ",\"beatAlignment\":" << note.beat_alignment
                   << ",\"quantizationShiftMs\":" << note.quantization_shift_ms
                   << ",\"source\":\"" << candidate_source_name(note.source)
                   << "\",\"evidence\":\"" << json_escape(evidence_text(note.evidence))
                   << "\",\"reason\":\"" << json_escape(review_reason(note))
                   << "\",\"chord\":" << (note.chord ? "true" : "false") << "}";
            output << (note_index + 1U == review.notes.size() ? "\n" : ",\n");
        }
        output << "      ]\n    }";
        output << (difficulty_index + 1U == reviews.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
}

void write_review_tsv(
    const std::filesystem::path& path,
    const std::vector<DifficultyReview>& reviews
) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create AutoChart review TSV");
    }
    output << "difficulty\ttime_ms\tduration_ms\tlane\tconfidence\treview_priority\tbeat_alignment\tquantization_shift_ms\tsource\tevidence\treason\n";
    for (const auto& review : reviews) {
        for (const auto& note : review.notes) {
            auto reason = review_reason(note);
            std::replace(reason.begin(), reason.end(), '\t', ' ');
            output << review.difficulty << '\t' << note.time_ms << '\t'
                   << note.duration_ms << '\t' << note.lane << '\t'
                   << note.confidence << '\t' << note.review_priority << '\t'
                   << note.beat_alignment << '\t' << note.quantization_shift_ms << '\t'
                   << candidate_source_name(note.source) << '\t'
                   << evidence_text(note.evidence) << '\t' << reason << '\n';
        }
    }
}

void write_review_html(
    const std::filesystem::path& path,
    const std::string& title,
    const std::filesystem::path& audio_path,
    const AutoChartResult& result,
    const std::vector<DifficultyReview>& reviews,
    const SongStructure& structure
) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create AutoChart HTML review");
    }
    const auto quality_label = result.overall_quality_score >= 88.0 ? "Excellent"
        : result.overall_quality_score >= 76.0 ? "Good"
        : result.overall_quality_score >= 64.0 ? "Review recommended"
        : "Manual review required";
    output << "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           << "<title>PulseForge AutoChart Review</title><style>"
           << ":root{color-scheme:dark;--bg:#0b0d12;--panel:#141823;--line:#252b3a;--text:#edf2ff;--muted:#99a4bb;--accent:#8ab4ff;--good:#6ee7a8;--warn:#ffd166;--bad:#ff7b89}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 20% 0,#172033 0,#0b0d12 42%);font:14px/1.45 'Segoe UI',system-ui,sans-serif;color:var(--text)}main{max-width:1200px;margin:auto;padding:28px}.hero{display:flex;justify-content:space-between;gap:24px;align-items:end;margin-bottom:22px}h1{font-size:32px;margin:0}.muted{color:var(--muted)}.score{font-size:48px;font-weight:750}.badge{display:inline-block;padding:5px 10px;border:1px solid var(--line);border-radius:999px;background:#101521}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:12px}.card{background:linear-gradient(180deg,#171c28,#121620);border:1px solid var(--line);border-radius:14px;padding:16px;box-shadow:0 12px 28px #0005}.metric{font-size:24px;font-weight:700}.timeline{position:relative;height:68px;background:#0d1119;border:1px solid var(--line);border-radius:12px;overflow:hidden;margin:12px 0 20px}.section{position:absolute;top:0;bottom:0;border-right:1px solid #ffffff20;padding:7px 8px;font-size:12px;overflow:hidden}.controls{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin:16px 0}button,select{background:#1b2230;color:var(--text);border:1px solid #343d51;border-radius:9px;padding:8px 11px}button{cursor:pointer}button:hover{border-color:var(--accent)}audio{width:min(650px,100%)}table{width:100%;border-collapse:collapse;background:#10141d;border-radius:12px;overflow:hidden}th,td{padding:9px 10px;border-bottom:1px solid #202637;text-align:left}th{position:sticky;top:0;background:#171c28}.p-high{color:var(--bad);font-weight:700}.p-mid{color:var(--warn)}.p-low{color:var(--good)}#notesWrap{max-height:560px;overflow:auto;border:1px solid var(--line);border-radius:12px}@media(max-width:700px){main{padding:16px}.hero{display:block}.score{font-size:38px}}"
           << "</style></head><body><main><div class=\"hero\"><div><span class=\"badge\">PulseForge AutoChart Review</span><h1>"
           << html_escape(title) << "</h1><div class=\"muted\">Generated evidence map and uncertainty queue</div></div><div><div class=\"score\">"
           << std::fixed << std::setprecision(1) << result.overall_quality_score << "</div><div>" << quality_label << "</div></div></div>";
    output << "<div class=\"grid\"><div class=\"card\"><div class=\"muted\">Detected BPM</div><div class=\"metric\">" << result.detected_bpm
           << "</div></div><div class=\"card\"><div class=\"muted\">Beat confidence</div><div class=\"metric\">" << result.beat_confidence * 100.0
           << "%</div></div><div class=\"card\"><div class=\"muted\">Review queue</div><div class=\"metric\">" << result.review_note_count
           << "</div></div><div class=\"card\"><div class=\"muted\">Low confidence</div><div class=\"metric\">" << result.low_confidence_note_count << "</div></div></div>";
    output << "<h2>Structure</h2><div class=\"timeline\">";
    const double duration_ms = (std::max)(1.0, result.duration_seconds * 1'000.0);
    for (const auto& section : structure.sections) {
        const double left = std::clamp(section.start_ms / duration_ms * 100.0, 0.0, 100.0);
        const double width = std::clamp((section.end_ms - section.start_ms) / duration_ms * 100.0, 0.4, 100.0 - left);
        output << "<div class=\"section\" style=\"left:" << left << "%;width:" << width
               << "%\" title=\"confidence " << section.confidence << "\">" << to_string(section.kind) << "</div>";
    }
    output << "</div><h2>Listen and review</h2><audio id=\"audio\" controls preload=\"metadata\" src=\""
           << html_escape(url_path_component(path_utf8(audio_path.filename()))) << "\"></audio>";
    output << "<div class=\"controls\"><label>Difficulty <select id=\"difficulty\">";
    for (const auto& review : reviews) {
        output << "<option value=\"" << html_escape(review.difficulty) << "\">" << html_escape(review.difficulty)
               << " — quality " << std::setprecision(1) << review.quality_score << "</option>";
    }
    output << "</select></label><label>Minimum review priority <select id=\"threshold\"><option value=\"0\">all retained</option><option value=\"0.25\">0.25+</option><option value=\"0.4\" selected>0.40+</option><option value=\"0.6\">0.60+</option></select></label><button id=\"next\">Next uncertain note</button></div>";
    output << "<div id=\"notesWrap\"><table><thead><tr><th>Time</th><th>Lane</th><th>Confidence</th><th>Priority</th><th>Evidence</th><th>Why review?</th><th></th></tr></thead><tbody id=\"rows\"></tbody></table></div>";
    output << "<script>const reviews=";
    output << "{";
    for (std::size_t d = 0U; d < reviews.size(); ++d) {
        if (d != 0U) output << ',';
        const auto& review = reviews[d];
        output << '"' << json_escape(review.difficulty) << "\":[";
        for (std::size_t n = 0U; n < review.notes.size(); ++n) {
            if (n != 0U) output << ',';
            const auto& note = review.notes[n];
            output << "{t:" << note.time_ms << ",l:" << note.lane << ",c:" << note.confidence
                   << ",p:" << note.review_priority << ",e:\"" << json_escape(evidence_text(note.evidence))
                   << "\",r:\"" << json_escape(review_reason(note)) << "\"}";
        }
        output << ']';
    }
    output << "};const audio=document.getElementById('audio'),diff=document.getElementById('difficulty'),thr=document.getElementById('threshold'),rows=document.getElementById('rows');let visible=[];function fmt(ms){let s=ms/1000,m=Math.floor(s/60);s=(s-m*60).toFixed(3).padStart(6,'0');return m+':'+s}function render(){const min=+thr.value;visible=(reviews[diff.value]||[]).filter(n=>n.p>=min);rows.innerHTML=visible.slice(0,5000).map((n,i)=>`<tr><td>${fmt(n.t)}</td><td>${n.l+1}</td><td>${(n.c*100).toFixed(1)}%</td><td class=${n.p>=.6?'p-high':n.p>=.4?'p-mid':'p-low'}>${n.p.toFixed(3)}</td><td>${n.e}</td><td>${n.r}</td><td><button onclick=seek(${i})>Play</button></td></tr>`).join('')}function seek(i){audio.currentTime=Math.max(0,visible[i].t/1000-.45);audio.play()}window.seek=seek;diff.onchange=render;thr.onchange=render;document.getElementById('next').onclick=()=>{if(visible.length)seek(0)};render();</script></main></body></html>";
}

void write_report(
    const std::filesystem::path& path,
    const std::filesystem::path& media,
    const AutoChartOptions& options,
    const AutoChartResult& result,
    const std::vector<AutoChartDifficultyResult>& difficulties,
    const SongStructure& structure
) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create autochart-report.json");
    }
    output << std::fixed << std::setprecision(6);
    output << "{\n"
           << "  \"schema\": \"pulseforge-autochart-report-v5\",\n"
           << "  \"source\": \"" << json_escape(path_utf8(media)) << "\",\n"
           << "  \"mode\": \"" << to_string(options.mode) << "\",\n"
           << "  \"videoAnalysis\": \"" << to_string(options.video_mode) << "\",\n"
           << "  \"videoAssistUsed\": " << (result.video_assist_used ? "true" : "false") << ",\n"
           << "  \"mlMode\": \"" << to_string(options.ml_mode) << "\",\n"
           << "  \"mlUsed\": " << (result.ml_used ? "true" : "false") << ",\n"
           << "  \"mlCacheHit\": " << (result.ml_cache_hit ? "true" : "false") << ",\n"
           << "  \"analysisCacheHit\": " << (result.analysis_cache_hit ? "true" : "false") << ",\n"
           << "  \"mlDevice\": \"" << json_escape(result.ml_device) << "\",\n"
           << "  \"sourceSeparationUsed\": " << (result.source_separation_used ? "true" : "false") << ",\n"
           << "  \"neuralBeatUsed\": " << (result.neural_beat_used ? "true" : "false") << ",\n"
           << "  \"drumTranscriptionUsed\": " << (result.drum_transcription_used ? "true" : "false") << ",\n"
           << "  \"pitchTranscriptionUsed\": " << (result.pitch_transcription_used ? "true" : "false") << ",\n"
           << "  \"vocalRefinementUsed\": " << (result.vocal_refinement_used ? "true" : "false") << ",\n"
           << "  \"structuralAnalysisUsed\": " << (result.structural_analysis_used ? "true" : "false") << ",\n"
           << "  \"structureConfidence\": " << result.structure_confidence << ",\n"
           << "  \"structuralSectionCount\": " << result.structural_section_count << ",\n"
           << "  \"phraseCount\": " << result.phrase_count << ",\n"
           << "  \"beamLaneOptimizerUsed\": " << (result.beam_lane_optimizer_used ? "true" : "false") << ",\n"
           << "  \"laneBeamWidth\": " << result.lane_beam_width << ",\n"
           << "  \"overallQualityScore\": " << result.overall_quality_score << ",\n"
           << "  \"reviewNoteCount\": " << result.review_note_count << ",\n"
           << "  \"lowConfidenceNoteCount\": " << result.low_confidence_note_count << ",\n"
           << "  \"durationSeconds\": " << result.duration_seconds << ",\n"
           << "  \"detectedBpm\": " << result.detected_bpm << ",\n"
           << "  \"beatConfidence\": " << result.beat_confidence << ",\n"
           << "  \"candidateCount\": " << result.candidate_count << ",\n"
           << "  \"dspCandidateCount\": " << result.dsp_candidate_count << ",\n"
           << "  \"stemCandidateCount\": " << result.stem_candidate_count << ",\n"
           << "  \"drumEventCount\": " << result.drum_event_count << ",\n"
           << "  \"pitchEventCount\": " << result.pitch_event_count << ",\n"
           << "  \"phonemeEventCount\": " << result.phoneme_event_count << ",\n"
           << "  \"syllableEventCount\": " << result.syllable_event_count << ",\n"
           << "  \"keyCount\": " << options.key_count << ",\n"
           << "  \"sections\": [\n";
    for (std::size_t index = 0U; index < structure.sections.size(); ++index) {
        const auto& section = structure.sections[index];
        output << "    {\"kind\": \"" << to_string(section.kind)
               << "\", \"startMs\": " << section.start_ms
               << ", \"endMs\": " << section.end_ms
               << ", \"intensity\": " << section.intensity
               << ", \"candidateDensity\": " << section.density
               << ", \"novelty\": " << section.novelty
               << ", \"densityScale\": " << section.density_scale
               << ", \"confidence\": " << section.confidence << "}";
        output << (index + 1U == structure.sections.size() ? "\n" : ",\n");
    }
    output << "  ],\n"
           << "  \"phraseBoundariesMs\": [";
    for (std::size_t index = 0U; index < structure.phrase_boundaries.size(); ++index) {
        if (index != 0U) {
            output << ", ";
        }
        output << structure.phrase_boundaries[index];
    }
    output << "],\n"
           << "  \"difficulties\": [\n";
    for (std::size_t index = 0U; index < difficulties.size(); ++index) {
        const auto& difficulty = difficulties[index];
        output << "    {\"name\": \"" << json_escape(difficulty.difficulty)
               << "\", \"notes\": " << difficulty.note_count
               << ", \"averageNps\": " << difficulty.average_nps
               << ", \"peakNps\": " << difficulty.peak_nps
               << ", \"meanConfidence\": " << difficulty.mean_confidence
               << ", \"qualityScore\": " << difficulty.quality_score
               << ", \"reviewNotes\": " << difficulty.review_note_count
               << ", \"lowConfidenceNotes\": " << difficulty.low_confidence_note_count
               << ", \"chart\": \""
               << json_escape(path_utf8(difficulty.chart_path.filename())) << "\"}";
        output << (index + 1U == difficulties.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
}

[[nodiscard]] std::vector<std::string> normalized_difficulties(
    const std::vector<std::string>& raw
) {
    std::vector<std::string> result;
    for (const auto& value : raw) {
        const auto profile = difficulty_profile(value);
        const std::string canonical(profile.name);
        if (std::find(result.begin(), result.end(), canonical) == result.end()) {
            result.push_back(canonical);
        }
    }
    if (result.empty()) {
        result.push_back("expert");
    }
    return result;
}

}  // namespace

std::string_view to_string(const AutoChartMode mode) noexcept {
    switch (mode) {
    case AutoChartMode::fast: return "fast";
    case AutoChartMode::maximum: return "maximum";
    case AutoChartMode::accurate:
    default: return "accurate";
    }
}

std::string_view to_string(const AutoChartVideoMode mode) noexcept {
    switch (mode) {
    case AutoChartVideoMode::off: return "off";
    case AutoChartVideoMode::on: return "on";
    case AutoChartVideoMode::automatic:
    default: return "auto";
    }
}

std::string_view to_string(const AutoChartMlMode mode) noexcept {
    switch (mode) {
    case AutoChartMlMode::off: return "off";
    case AutoChartMlMode::on: return "on";
    case AutoChartMlMode::automatic:
    default: return "auto";
    }
}

std::string_view to_string(const AutoChartProgressStage stage) noexcept {
    switch (stage) {
    case AutoChartProgressStage::validating: return "validating";
    case AutoChartProgressStage::decoding: return "decoding";
    case AutoChartProgressStage::analyzing: return "analyzing";
    case AutoChartProgressStage::ml_analysis: return "ml-analysis";
    case AutoChartProgressStage::tempo: return "tempo";
    case AutoChartProgressStage::candidates: return "candidates";
    case AutoChartProgressStage::structure: return "structure";
    case AutoChartProgressStage::charting: return "charting";
    case AutoChartProgressStage::writing: return "writing";
    case AutoChartProgressStage::installing: return "installing";
    case AutoChartProgressStage::completed: return "completed";
    case AutoChartProgressStage::cancelled: return "cancelled";
    }
    return "unknown";
}

AutoChartMlHealthReport inspect_autochart_ml_backend(
    const AutoChartOptions& options,
    const bool deep
) {
    AutoChartMlHealthReport report;
    report.deep = deep;
    try {
        if (options.ml_device != "auto" && options.ml_device != "cpu"
            && options.ml_device != "cuda") {
            throw std::runtime_error("AutoChart ML device must be auto, cpu or cuda");
        }
        const auto python = resolve_ml_python(options);
        const auto script = resolve_ml_backend_script(options);
        const auto ffmpeg = resolve_ffmpeg(options);
        if (python.empty()) {
            throw std::runtime_error(
                "AutoChart ML Python environment was not found; run scripts/setup-autochart-ml.ps1"
            );
        }
        if (script.empty()) {
            throw std::runtime_error("AutoChart ML backend script was not found");
        }
        if (ffmpeg.empty()) {
            throw std::runtime_error("FFmpeg was not found for the ML health check");
        }

        TemporaryWorkspace workspace;
        const auto output_path = workspace.path() / "ml-health.json";
        const auto cache_root = resolve_ml_cache_root(options);
        std::error_code error;
        std::filesystem::create_directories(cache_root, error);
        if (error) {
            throw std::runtime_error("cannot create AutoChart ML cache directory");
        }
        std::vector<std::filesystem::path> arguments{
            script,
            deep ? "--deep-health" : "--health",
            "--output", output_path,
            "--workspace", workspace.path() / "health",
            "--ffmpeg", ffmpeg,
            "--cache-root", cache_root,
            "--device", options.ml_device,
        };
        const int exit_code = run_process(python, arguments);

        std::error_code size_error;
        const auto result_size = std::filesystem::file_size(output_path, size_error);
        if (size_error || result_size > 2U * 1024U * 1024U) {
            throw std::runtime_error("ML health backend did not produce a bounded report");
        }
        nlohmann::json root;
        {
            std::ifstream input(output_path, std::ios::binary);
            input >> root;
        }
        report.ok = exit_code == 0 && root.value("ok", false);
        report.python_version = root.value("pythonVersion", std::string{});
        report.device = root.value("device", std::string{});
        report.error = root.value("error", std::string{});
        if (const auto stages = root.find("stages");
            stages != root.end() && stages->is_array()) {
            report.stages.reserve((std::min)(stages->size(), std::size_t{64U}));
            for (const auto& item : *stages) {
                if (report.stages.size() >= 64U || !item.is_object()) {
                    break;
                }
                report.stages.push_back({
                    item.value("name", std::string{}),
                    item.value("available", false),
                    item.value("tested", false),
                    item.value("latencyMs", 0.0),
                    item.value("detail", std::string{}),
                });
            }
        }
        if (!report.ok && report.error.empty()) {
            report.error = "AutoChart ML health check failed";
        }
    } catch (const std::exception& exception) {
        report.ok = false;
        report.error = exception.what();
    }
    return report;
}

AutoChartResult generate_autochart_mod(
    const std::filesystem::path& media_path,
    const AutoChartOptions& input_options
) {
    AutoChartResult result;
    try {
        std::error_code error;
        if (!std::filesystem::is_regular_file(media_path, error) || error) {
            throw std::runtime_error("input media file does not exist");
        }
        AutoChartOptions options = input_options;
        // Normalize the two public naming generations used by the standalone
        // Studio and the integrated launcher. Either frontend may disable
        // review output, and an explicitly changed launcher queue limit wins.
        options.write_review_artifacts = options.write_review_artifacts
            && options.review_artifacts;
        if (options.review_queue_limit != default_review_queue_limit) {
            options.maximum_review_notes = options.review_queue_limit;
        } else {
            options.review_queue_limit = options.maximum_review_notes;
        }
        options.maximum_review_notes = (std::min)(
            options.maximum_review_notes,
            std::uint32_t{200'000U}
        );
        options.review_queue_limit = options.maximum_review_notes;
        emit_progress(
            options,
            AutoChartProgressStage::validating,
            0.01,
            "Validating AutoChart input"
        );
        throw_if_autochart_cancelled(options);
        if (options.key_count == 0U || options.key_count > maximum_supported_key_count) {
            throw std::runtime_error("AutoChart key count must be between 1 and 18");
        }
        if (!std::isfinite(options.scroll_speed) || options.scroll_speed <= 0.0) {
            throw std::runtime_error("AutoChart scroll speed must be finite and positive");
        }
        if (options.ml_device != "auto" && options.ml_device != "cpu"
            && options.ml_device != "cuda") {
            throw std::runtime_error("AutoChart ML device must be auto, cpu or cuda");
        }
        if (options.title.empty()) {
            options.title = default_title(media_path);
        }
        options.difficulties = normalized_difficulties(options.difficulties);

        const auto ffmpeg = resolve_ffmpeg(options);
        if (ffmpeg.empty()) {
            throw std::runtime_error(
                "FFmpeg was not found; pass --ffmpeg or bundle tools/ffmpeg/bin/ffmpeg.exe"
            );
        }

        TemporaryWorkspace workspace;
        emit_progress(
            options,
            AutoChartProgressStage::decoding,
            0.06,
            "Preparing media decode and feature cache"
        );
        throw_if_autochart_cancelled(options);
        const auto parameters = parameters_for(options.mode);
        double duration_seconds = 0.0;
        bool video_used = false;
        std::vector<FeatureFrame> frames;
        const bool analysis_cache_hit = load_feature_cache(
            media_path,
            options,
            parameters,
            frames,
            duration_seconds,
            video_used
        );
        if (analysis_cache_hit) {
            std::cout << "[AutoChart] Native feature cache hit (skipping decode/FFT"
                      << (video_used ? "/video" : "") << ")\n";
        } else {
            const auto decoded = workspace.path() / "audio.f32";
            std::cout << "[AutoChart] Decoding audio from " << path_utf8(media_path) << "\n";
            if (!decode_audio_to_f32(ffmpeg, media_path, decoded)) {
                throw std::runtime_error(
                    "FFmpeg could not decode the first audio stream from the media file"
                );
            }
            std::cout << "[AutoChart] Extracting spectral/onset features ("
                      << parameters.fft_size << " FFT, hop " << parameters.hop_size << ")\n";
            frames = extract_audio_features(decoded, parameters, duration_seconds);
            calculate_onset_envelope(frames, parameters);

            if (options.video_mode != AutoChartVideoMode::off) {
                const auto video_raw = workspace.path() / "video.gray";
                std::cout << "[AutoChart] Probing optional visualizer/video pulse track\n";
                const bool video_ok = extract_video_luma(ffmpeg, media_path, video_raw);
                if (video_ok) {
                    const auto pulses = analyze_video_pulses(video_raw);
                    if (!pulses.empty()) {
                        merge_video_pulses(frames, pulses);
                        video_used = true;
                    }
                } else if (options.video_mode == AutoChartVideoMode::on) {
                    throw std::runtime_error(
                        "video analysis was forced but the input has no decodable video stream"
                    );
                }
            }
            save_feature_cache(
                media_path,
                options,
                parameters,
                frames,
                duration_seconds,
                video_used
            );
        }
        emit_progress(
            options,
            AutoChartProgressStage::analyzing,
            0.30,
            analysis_cache_hit
                ? "Loaded native audio features from cache"
                : "Completed native spectral/onset analysis"
        );
        throw_if_autochart_cancelled(options);
        if (!std::isfinite(duration_seconds) || duration_seconds <= 0.0
            || duration_seconds > 12.0 * 60.0 * 60.0) {
            throw std::runtime_error(
                "AutoChart media duration must be finite, non-zero and at most 12 hours"
            );
        }

        emit_progress(
            options,
            AutoChartProgressStage::ml_analysis,
            0.34,
            "Running optional neural analysis"
        );
        throw_if_autochart_cancelled(options);
        const auto ml = run_ml_analysis(media_path, ffmpeg, options, workspace);
        throw_if_autochart_cancelled(options);

        emit_progress(
            options,
            AutoChartProgressStage::tempo,
            0.48,
            "Detecting tempo and beat grid"
        );
        std::cout << "[AutoChart] Detecting tempo and beat grid\n";
        const auto dsp_beat_grid = detect_beat_grid(
            frames,
            parameters,
            duration_seconds,
            options.variable_tempo
        );
        BeatGrid beat_grid = dsp_beat_grid;
        bool neural_beat_selected = false;
        if (const auto neural_grid = beat_grid_from_ml(
                ml, duration_seconds, options.variable_tempo
            ); neural_grid.has_value()) {
            const double agreement = tempo_octave_agreement(
                dsp_beat_grid.bpm,
                neural_grid->bpm
            );
            const double neural_score = neural_grid->confidence + agreement * 0.08;
            const double dsp_score = dsp_beat_grid.confidence + agreement * 0.04;
            // Prefer Beat This when it is at least competitive, but retain the
            // native tracker when the neural clock is a clear outlier and the
            // DSP evidence is materially stronger. Half/double-tempo matches
            // count as agreement rather than false disagreement.
            if (neural_score + 0.03 >= dsp_score) {
                beat_grid = *neural_grid;
                beat_grid.confidence = std::clamp(
                    beat_grid.confidence + agreement * 0.04,
                    0.0,
                    1.0
                );
                neural_beat_selected = true;
                std::cout << "[AutoChart] Neural beat grid selected"
                          << " (DSP " << dsp_beat_grid.bpm << " BPM, agreement "
                          << agreement << ")\n";
            } else {
                beat_grid.confidence = std::clamp(
                    beat_grid.confidence + agreement * 0.025,
                    0.0,
                    1.0
                );
                std::cout << "[AutoChart] DSP beat grid retained after neural cross-check"
                          << " (neural " << neural_grid->bpm << " BPM, agreement "
                          << agreement << ")\n";
            }
        }
        if (!std::isfinite(beat_grid.bpm) || beat_grid.bpm <= 0.0) {
            throw std::runtime_error("AutoChart could not derive a valid tempo");
        }
        std::cout << "[AutoChart] BPM " << std::fixed << std::setprecision(3)
                  << beat_grid.bpm << " (confidence " << beat_grid.confidence << ")\n";

        throw_if_autochart_cancelled(options);
        emit_progress(
            options,
            AutoChartProgressStage::candidates,
            0.57,
            "Fusing musical onset evidence"
        );
        std::cout << "[AutoChart] Generating musical onset candidates\n";
        auto candidates = make_candidates(frames, parameters, beat_grid);
        const std::uint64_t dsp_candidate_count =
            static_cast<std::uint64_t>(candidates.size());
        std::uint64_t stem_candidate_count = 0U;

        if (ml.source_separation_used) {
            const auto add_stem = [&](
                const std::filesystem::path& path,
                const CandidateSource source,
                const std::string_view label
            ) {
                auto stem_candidates = make_stem_candidates(
                    ffmpeg, path, source, parameters, beat_grid, workspace, label
                );
                stem_candidate_count += static_cast<std::uint64_t>(stem_candidates.size());
                candidates = fuse_candidates(std::move(candidates), std::move(stem_candidates));
            };
            add_stem(ml.drums, CandidateSource::drums, "drums");
            add_stem(ml.bass, CandidateSource::bass, "bass");
            add_stem(ml.vocals, CandidateSource::vocals, "vocals");
            if (options.mode == AutoChartMode::maximum) {
                add_stem(ml.other, CandidateSource::other, "other");
            }
        }

        auto drum_candidates = make_drum_candidates(ml, beat_grid);
        if (!drum_candidates.empty()) {
            candidates = fuse_candidates(std::move(candidates), std::move(drum_candidates));
        }
        auto pitch_candidates = make_pitch_candidates(ml, beat_grid);
        if (!pitch_candidates.empty()) {
            candidates = fuse_candidates(std::move(candidates), std::move(pitch_candidates));
        }
        auto vocal_candidates = make_vocal_refinement_candidates(ml, beat_grid);
        if (!vocal_candidates.empty()) {
            candidates = fuse_candidates(std::move(candidates), std::move(vocal_candidates));
        }
        if (candidates.empty()) {
            throw std::runtime_error("AutoChart found no reliable musical onsets");
        }
        std::cout << "[AutoChart] Candidate fusion: DSP=" << dsp_candidate_count
                  << ", stems=" << stem_candidate_count
                  << ", drum events=" << ml.drum_events.size()
                  << ", pitch events=" << ml.pitch_events.size()
                  << ", vocal events=" << ml.vocal_events.size()
                  << ", fused=" << candidates.size() << "\n";

        throw_if_autochart_cancelled(options);
        emit_progress(
            options,
            AutoChartProgressStage::structure,
            0.66,
            "Analyzing bars, phrases and musical sections"
        );
        std::cout << "[AutoChart] Analyzing bars, phrases and musical sections\n";
        const auto structure = analyze_song_structure(
            frames,
            beat_grid,
            candidates,
            duration_seconds,
            options.structural_charting
        );
        if (structure.used) {
            std::cout << "[AutoChart] Structure: " << structure.sections.size()
                      << " sections, " << structure.phrase_boundaries.size()
                      << " phrase boundaries (confidence "
                      << structure.confidence << ")\n";
            for (const auto& section : structure.sections) {
                std::cout << "  [Structure] " << to_string(section.kind)
                          << " " << std::fixed << std::setprecision(2)
                          << section.start_ms / 1'000.0 << "s-"
                          << section.end_ms / 1'000.0 << "s"
                          << " intensity=" << section.intensity
                          << " densityScale=" << section.density_scale
                          << "\n";
            }
        } else {
            std::cout << "[AutoChart] Structural analysis unavailable/disabled;"
                      << " using global density calibration\n";
        }

        throw_if_autochart_cancelled(options);
        const auto mod_root = choose_mod_root(media_path, options, options.title);
        ensure_output_directory(mod_root, options.overwrite);
        // Set immediately so a cancelled preview can be discarded by the
        // launcher even when cancellation arrives during chart construction.
        result.mod_root = mod_root;
        emit_progress(
            options,
            AutoChartProgressStage::writing,
            0.70,
            "Materializing mod audio"
        );
        const auto audio_path = materialize_mod_audio(ffmpeg, media_path, mod_root);
        const auto audio_filename = path_utf8(audio_path.filename());

        std::vector<AutoChartDifficultyResult> difficulty_results;
        difficulty_results.reserve(options.difficulties.size());
        std::vector<DifficultyReview> difficulty_reviews;
        difficulty_reviews.reserve(options.difficulties.size());
        std::string entry_chart;
        std::uint32_t maximum_beam_width = 0U;
        bool beam_optimizer_used = false;
        for (std::size_t index = 0U; index < options.difficulties.size(); ++index) {
            throw_if_autochart_cancelled(options);
            const double local_fraction = options.difficulties.empty()
                ? 0.82
                : 0.74 + 0.16 * static_cast<double>(index)
                    / static_cast<double>(options.difficulties.size());
            emit_progress(
                options,
                AutoChartProgressStage::charting,
                local_fraction,
                "Building " + options.difficulties[index] + " difficulty",
                static_cast<std::uint64_t>(index),
                static_cast<std::uint64_t>(options.difficulties.size())
            );
            const auto profile = difficulty_profile(options.difficulties[index]);
            std::cout << "[AutoChart] Building " << profile.name << " difficulty\n";
            auto built = build_difficulty(
                candidates,
                beat_grid,
                structure,
                duration_seconds,
                profile,
                options,
                audio_filename
            );
            maximum_beam_width = (std::max)(
                maximum_beam_width,
                built.beam_width
            );
            beam_optimizer_used = beam_optimizer_used || built.beam_used;
            std::filesystem::path chart_path;
            if (options.difficulties.size() == 1U) {
                chart_path = mod_root / "chart.json";
            } else {
                std::filesystem::create_directories(mod_root / "charts");
                chart_path = mod_root / "charts" / (std::string(profile.name) + ".json");
            }
            write_native_chart(built.chart, chart_path);
            if (entry_chart.empty()
                || profile.name == "expert"
                || (entry_chart.find("expert") == std::string::npos
                    && profile.name == "hard")) {
                entry_chart = path_utf8(std::filesystem::relative(chart_path, mod_root));
            }
            difficulty_results.push_back({
                std::string(profile.name),
                chart_path,
                static_cast<std::uint64_t>(built.chart.notes.size()),
                built.average_nps,
                built.peak_nps,
                built.mean_confidence,
                built.review.quality_score,
                built.review.uncertain_count,
                built.review.low_confidence_count,
                built.review.high_priority_count,
            });
            difficulty_reviews.push_back(std::move(built.review));
        }
        if (entry_chart.empty()) {
            throw std::runtime_error("AutoChart did not generate an entry chart");
        }

        throw_if_autochart_cancelled(options);
        emit_progress(
            options,
            AutoChartProgressStage::writing,
            0.92,
            "Writing AutoChart manifest and review artifacts"
        );
        const auto slug = sanitize_slug(options.title);
        const auto mod_id = options.mod_id.empty()
            ? std::string{"org.pulseforge.autochart."} + slug
            : options.mod_id;
        write_mod_manifest(mod_root, mod_id, options.title, entry_chart);
        if (options.add_to_mods && options.output_root.empty()) {
            enable_mod_in_list(
                options.mods_root.empty() ? std::filesystem::path{"mods"} : options.mods_root,
                path_utf8(mod_root.filename())
            );
        }

        result.ok = true;
        result.mod_root = mod_root;
        result.audio_path = audio_path;
        result.report_path = mod_root / "autochart-report.json";
        result.duration_seconds = duration_seconds;
        result.detected_bpm = beat_grid.bpm;
        result.beat_confidence = beat_grid.confidence;
        result.video_assist_used = video_used;
        result.ml_used = ml.used;
        result.ml_cache_hit = ml.cache_hit;
        result.analysis_cache_hit = analysis_cache_hit;
        result.source_separation_used = ml.source_separation_used;
        result.neural_beat_used = neural_beat_selected;
        result.drum_transcription_used = ml.drum_transcription_used;
        result.pitch_transcription_used = ml.pitch_transcription_used;
        result.vocal_refinement_used = ml.vocal_refinement_used;
        result.structural_analysis_used = structure.used;
        result.beam_lane_optimizer_used = beam_optimizer_used;
        result.ml_device = ml.device;
        result.structure_confidence = structure.confidence;
        result.structural_section_count = static_cast<std::uint32_t>(
            (std::min)(
                structure.sections.size(),
                static_cast<std::size_t>(
                    (std::numeric_limits<std::uint32_t>::max)()
                )
            )
        );
        result.phrase_count = static_cast<std::uint32_t>(
            (std::min)(
                structure.phrase_boundaries.size() + (structure.used ? 1U : 0U),
                static_cast<std::size_t>(
                    (std::numeric_limits<std::uint32_t>::max)()
                )
            )
        );
        result.lane_beam_width = maximum_beam_width;
        result.candidate_count = static_cast<std::uint64_t>(candidates.size());
        result.dsp_candidate_count = dsp_candidate_count;
        result.stem_candidate_count = stem_candidate_count;
        result.drum_event_count = static_cast<std::uint64_t>(ml.drum_events.size());
        result.pitch_event_count = static_cast<std::uint64_t>(ml.pitch_events.size());
        result.phoneme_event_count = static_cast<std::uint64_t>(std::count_if(
            ml.vocal_events.begin(),
            ml.vocal_events.end(),
            [](const MlVocalEvent& event) {
                return event.role == MlVocalRole::phoneme;
            }
        ));
        result.syllable_event_count = static_cast<std::uint64_t>(std::count_if(
            ml.vocal_events.begin(),
            ml.vocal_events.end(),
            [](const MlVocalEvent& event) {
                return event.role == MlVocalRole::syllable;
            }
        ));
        result.difficulties = difficulty_results;
        double quality_sum = 0.0;
        for (const auto& review : difficulty_reviews) {
            quality_sum += review.quality_score;
            result.review_note_count += review.uncertain_count;
            result.low_confidence_note_count += review.low_confidence_count;
            result.high_priority_review_count += review.high_priority_count;
        }
        result.overall_quality_score = difficulty_reviews.empty()
            ? 0.0
            : quality_sum / static_cast<double>(difficulty_reviews.size());
        if (options.write_review_artifacts) {
            result.review_path = mod_root / "autochart-review.json";
            write_review_json(result.review_path, result, difficulty_reviews, structure);
            write_review_tsv(mod_root / "autochart-review.tsv", difficulty_reviews);
            if (options.write_html_review) {
                result.review_html_path = mod_root / "autochart-review.html";
                write_review_html(
                    result.review_html_path,
                    options.title,
                    audio_path,
                    result,
                    difficulty_reviews,
                    structure
                );
            }
            result.review_index_path = !result.review_html_path.empty()
                ? result.review_html_path
                : result.review_path;
        }
        write_report(
            result.report_path,
            media_path,
            options,
            result,
            difficulty_results,
            structure
        );
        emit_progress(
            options,
            AutoChartProgressStage::completed,
            1.0,
            "AutoChart generation complete",
            static_cast<std::uint64_t>(options.difficulties.size()),
            static_cast<std::uint64_t>(options.difficulties.size())
        );
        std::cout << "[AutoChart] Mod created at " << path_utf8(mod_root) << "\n";
    } catch (const AutoChartCancelled& exception) {
        result.ok = false;
        result.cancelled = true;
        result.error = exception.what();
        emit_progress(
            input_options,
            AutoChartProgressStage::cancelled,
            1.0,
            result.error
        );
    } catch (const std::exception& exception) {
        result.ok = false;
        result.error = exception.what();
    }
    return result;
}

namespace {

[[nodiscard]] std::filesystem::path rebase_generated_path(
    const std::filesystem::path& value,
    const std::filesystem::path& old_root,
    const std::filesystem::path& new_root
) {
    if (value.empty()) {
        return {};
    }
    const auto relative = value.lexically_relative(old_root);
    if (relative.empty() || relative.native().starts_with(std::filesystem::path{".."}.native())) {
        return value;
    }
    return new_root / relative;
}

void rebase_autochart_result_paths(
    AutoChartResult& result,
    const std::filesystem::path& old_root,
    const std::filesystem::path& new_root
) {
    result.mod_root = new_root;
    result.audio_path = rebase_generated_path(result.audio_path, old_root, new_root);
    result.report_path = rebase_generated_path(result.report_path, old_root, new_root);
    result.review_path = rebase_generated_path(result.review_path, old_root, new_root);
    result.review_html_path = rebase_generated_path(result.review_html_path, old_root, new_root);
    result.review_index_path = rebase_generated_path(result.review_index_path, old_root, new_root);
    for (auto& difficulty : result.difficulties) {
        difficulty.chart_path = rebase_generated_path(
            difficulty.chart_path, old_root, new_root
        );
    }
}

[[nodiscard]] AutoChartFileOperationResult install_staging_root(
    const std::filesystem::path& staging_root,
    const std::filesystem::path& requested_mods_root,
    const bool overwrite
) {
    AutoChartFileOperationResult operation;
    try {
        std::error_code error;
        if (!std::filesystem::is_directory(staging_root, error) || error) {
            throw std::runtime_error("AutoChart staging directory does not exist");
        }
        auto mods_root = requested_mods_root;
        if (mods_root.empty()) {
            mods_root = "mods";
        }
        std::filesystem::create_directories(mods_root, error);
        if (error) {
            throw std::runtime_error("cannot create AutoChart mods directory");
        }
        auto folder = path_utf8(staging_root.filename());
        folder = sanitize_slug(folder);
        if (folder.empty()) {
            throw std::runtime_error("AutoChart staging folder has no valid mod id");
        }
        const auto destination = mods_root / folder;
        std::error_code equivalent_error;
        const bool same_path = std::filesystem::equivalent(
            staging_root, destination, equivalent_error
        );
        if (!equivalent_error && same_path) {
            enable_mod_in_list(mods_root, folder);
            operation.ok = true;
            operation.mod_root = destination;
            operation.installed_path = destination;
            operation.destination_path = destination;
            operation.message = "AutoChart mod is already installed";
            return operation;
        }

        if (std::filesystem::exists(destination, error) && !error) {
            if (!overwrite) {
                throw std::runtime_error(
                    "AutoChart destination mod already exists: "
                    + path_utf8(destination)
                );
            }
            std::filesystem::remove_all(destination, error);
            if (error) {
                throw std::runtime_error("cannot replace existing AutoChart mod");
            }
        }

        // Prefer a same-volume rename. If preview/staging lives on another
        // volume, fall back to a bounded filesystem copy through a temporary
        // sibling so the catalog never observes a half-installed directory.
        std::filesystem::rename(staging_root, destination, error);
        if (error) {
            error.clear();
            const auto temporary = mods_root / (".pulseforge-installing-" + folder);
            std::filesystem::remove_all(temporary, error);
            error.clear();
            std::filesystem::copy(
                staging_root,
                temporary,
                std::filesystem::copy_options::recursive
                    | std::filesystem::copy_options::overwrite_existing,
                error
            );
            if (error) {
                std::filesystem::remove_all(temporary, error);
                throw std::runtime_error("cannot copy AutoChart staging mod into mods");
            }
            std::filesystem::rename(temporary, destination, error);
            if (error) {
                std::filesystem::remove_all(temporary, error);
                throw std::runtime_error("cannot finalize AutoChart mod installation");
            }
            std::filesystem::remove_all(staging_root, error);
            if (error) {
                // Installation is complete; a stale staging folder is a cleanup
                // issue, not a reason to hide the successfully installed mod.
                error.clear();
            }
        }
        enable_mod_in_list(mods_root, folder);
        operation.ok = true;
        operation.mod_root = destination;
        operation.installed_path = destination;
        operation.destination_path = destination;
        operation.message = "AutoChart mod installed";
    } catch (const std::exception& exception) {
        operation.ok = false;
        operation.error = exception.what();
        operation.message = operation.error;
    }
    return operation;
}

}  // namespace

AutoChartFileOperationResult install_autochart_mod(
    AutoChartResult& staged,
    const std::filesystem::path& mods_root,
    const bool overwrite
) {
    const auto old_root = staged.mod_root;
    auto operation = install_staging_root(old_root, mods_root, overwrite);
    if (operation.ok) {
        rebase_autochart_result_paths(staged, old_root, operation.mod_root);
        operation.report_path = staged.report_path;
        operation.review_index_path = staged.review_index_path;
    }
    return operation;
}

AutoChartFileOperationResult install_autochart_mod(
    const AutoChartResult& staged,
    const std::filesystem::path& mods_root,
    const bool overwrite
) {
    return install_staging_root(staged.mod_root, mods_root, overwrite);
}

AutoChartFileOperationResult install_autochart_mod(
    const std::filesystem::path& staging_root,
    const std::filesystem::path& mods_root,
    const bool overwrite
) {
    return install_staging_root(staging_root, mods_root, overwrite);
}

AutoChartFileOperationResult discard_autochart_staging(
    const std::filesystem::path& staging_root
) {
    AutoChartFileOperationResult operation;
    operation.mod_root = staging_root;
    try {
        if (staging_root.empty()) {
            operation.ok = true;
            return operation;
        }
        std::error_code error;
        std::filesystem::remove_all(staging_root, error);
        if (error) {
            throw std::runtime_error("cannot remove AutoChart staging directory");
        }
        operation.ok = true;
        operation.message = "AutoChart staging discarded";
    } catch (const std::exception& exception) {
        operation.ok = false;
        operation.error = exception.what();
        operation.message = operation.error;
    }
    return operation;
}

AutoChartFileOperationResult discard_autochart_staging(
    AutoChartResult& staged
) {
    auto operation = discard_autochart_staging(staged.mod_root);
    if (operation.ok) {
        staged.mod_root.clear();
        staged.audio_path.clear();
        staged.report_path.clear();
        staged.review_path.clear();
        staged.review_html_path.clear();
        staged.review_index_path.clear();
        for (auto& difficulty : staged.difficulties) {
            difficulty.chart_path.clear();
        }
    }
    return operation;
}

AutoChartFileOperationResult discard_autochart_staging(
    const AutoChartResult& staged
) {
    return discard_autochart_staging(staged.mod_root);
}

}  // namespace pulseforge
