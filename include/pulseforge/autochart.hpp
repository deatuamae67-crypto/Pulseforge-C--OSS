#pragma once

#include "pulseforge/chart.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge {

enum class AutoChartMode : std::uint8_t {
    fast,
    accurate,
    maximum,
};

enum class AutoChartVideoMode : std::uint8_t {
    off,
    automatic,
    on,
};

enum class AutoChartMlMode : std::uint8_t {
    off,
    automatic,
    on,
};

enum class AutoChartProgressStage : std::uint8_t {
    validating,
    decoding,
    analyzing,
    ml_analysis,
    tempo,
    candidates,
    structure,
    charting,
    writing,
    installing,
    completed,
    cancelled,

    // Source-compatibility aliases used by launcher/UI revisions.
    preparing = validating,
    audio_decode = decoding,
    feature_analysis = analyzing,
    analysis = analyzing,
    neural_analysis = ml_analysis,
    ml = ml_analysis,
    beat_tracking = tempo,
    candidate_fusion = candidates,
    structural_analysis = structure,
    generating = charting,
    generation = charting,
    reviewing = writing,
    saving = writing,
    finalizing = writing,
    done = completed,
};

struct AutoChartProgress final {
    // PULSEFORGE_AUTOCHART_PROGRESS_C2679_CTOR_V2
    AutoChartProgress() = default;

    AutoChartProgress(
        const AutoChartProgressStage stage_value,
        const double overall_fraction_value,
        const double stage_fraction_value,
        const std::string_view message_value,
        const std::string_view difficulty_value = {},
        const bool can_cancel_value = true
    )
        : stage(stage_value),
          fraction(overall_fraction_value),
          progress(overall_fraction_value),
          overall_fraction(overall_fraction_value),
          stage_fraction(stage_fraction_value),
          percentage(overall_fraction_value * 100.0),
          can_cancel(can_cancel_value),
          message(message_value),
          detail(message_value),
          status(message_value),
          label(message_value),
          difficulty(difficulty_value) {}

    AutoChartProgressStage stage{AutoChartProgressStage::validating};
    // Canonical 0..1 overall progress value used by launcher/GUI frontends.
    double fraction{};
    // Compatibility aliases retained for the in-engine launcher integration.
    double progress{};
    double overall_fraction{};
    double stage_fraction{};
    double percentage{};
    std::uint64_t current{};
    std::uint64_t total{};
    bool indeterminate{};
    bool can_cancel{true};
    std::string message;
    std::string detail;
    std::string status;
    std::string label;
    std::string difficulty;
};

struct AutoChartOptions final {
    AutoChartMode mode{AutoChartMode::accurate};
    AutoChartVideoMode video_mode{AutoChartVideoMode::automatic};
    AutoChartMlMode ml_mode{AutoChartMlMode::automatic};
    std::uint16_t key_count{4U};
    std::vector<std::string> difficulties{"expert"};
    std::filesystem::path output_root;
    std::filesystem::path mods_root{"mods"};
    std::filesystem::path ffmpeg_path;
    std::filesystem::path ml_python_path;
    std::filesystem::path ml_backend_script;
    std::filesystem::path ml_cache_root;
    std::filesystem::path analysis_cache_root;
    std::string ml_device{"auto"};
    std::string mod_id;
    std::string title;
    std::string artist{"Unknown"};
    std::string charter{"PulseForge AutoChart"};
    double scroll_speed{1.0};
    bool add_to_mods{true};
    bool variable_tempo{false};
    bool overwrite{false};
    bool ml_cache{true};
    bool analysis_cache{true};
    // PULSEFORGE_P1_5_0E_BOUNDED_PRECISION_CACHE_OPTIONS_V1
    // Completed per-song ML evidence and native DSP features are aggressively
    // bounded; shared model weights live outside these budgets.
    std::uint64_t ml_run_cache_max_bytes{1ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint64_t analysis_cache_max_bytes{512ULL * 1024ULL * 1024ULL};
    // Avoids downloading a second multi-model Demucs family merely for the
    // maximum preset. Existing neural + higher-resolution DSP evidence is fused.
    bool compact_ml_models{true};
    bool ml_source_separation{true};
    bool ml_beat_tracking{true};
    bool ml_drum_transcription{true};
    bool ml_pitch_transcription{true};
    // Optional phoneme/syllable refinement is deliberately separate from
    // Basic Pitch so singing-oriented charts can be A/B tested or disabled
    // when the speech model is a poor fit for a particular song.
    bool ml_vocal_refinement{true};
    // Phase 3 quality layer. Both switches default on, but explicit fallbacks
    // are kept so regressions can be isolated without disabling AutoChart.
    bool structural_charting{true};
    bool beam_lane_optimizer{true};
    // Phase 5 reliability/review layer. Review artifacts are bounded and do
    // not scale memory linearly with huge charts.
    bool write_review_artifacts{true};
    bool write_html_review{true};
    std::uint32_t maximum_review_notes{20'000U};

    // Launcher-facing names from the integrated AutoChart workflow. Keep both
    // spellings so the standalone CLI/Studio and the in-engine launcher share
    // one ABI/source contract. Generation normalizes these aliases.
    bool review_artifacts{true};
    std::uint32_t review_queue_limit{20'000U};
    std::function<void(const AutoChartProgress&)> progress_callback;
    std::function<bool()> cancel_requested;
};

struct AutoChartMlHealthStage final {
    std::string name;
    bool available{};
    bool tested{};
    double latency_ms{};
    std::string detail;
};

struct AutoChartMlHealthReport final {
    bool ok{};
    bool deep{};
    std::string error;
    std::string python_version;
    std::string device;
    std::vector<AutoChartMlHealthStage> stages;
};

struct AutoChartDifficultyResult final {
    std::string difficulty;
    std::filesystem::path chart_path;
    std::uint64_t note_count{};
    double average_nps{};
    double peak_nps{};
    double mean_confidence{};
    double quality_score{};
    std::uint64_t review_note_count{};
    std::uint64_t low_confidence_note_count{};
    std::uint64_t high_priority_review_count{};
};

struct AutoChartResult final {
    bool ok{};
    std::string error;
    std::filesystem::path mod_root;
    std::filesystem::path audio_path;
    std::filesystem::path report_path;
    std::filesystem::path review_path;
    std::filesystem::path review_html_path;
    double duration_seconds{};
    double detected_bpm{};
    double beat_confidence{};
    bool video_assist_used{};
    bool ml_used{};
    bool ml_cache_hit{};
    bool analysis_cache_hit{};
    bool source_separation_used{};
    bool neural_beat_used{};
    bool drum_transcription_used{};
    bool pitch_transcription_used{};
    bool vocal_refinement_used{};
    bool structural_analysis_used{};
    bool beam_lane_optimizer_used{};
    std::string ml_device;
    double structure_confidence{};
    std::uint32_t structural_section_count{};
    std::uint32_t phrase_count{};
    std::uint32_t lane_beam_width{};
    std::uint64_t candidate_count{};
    std::uint64_t dsp_candidate_count{};
    std::uint64_t stem_candidate_count{};
    std::uint64_t drum_event_count{};
    std::uint64_t pitch_event_count{};
    std::uint64_t phoneme_event_count{};
    std::uint64_t syllable_event_count{};
    double overall_quality_score{};
    std::uint64_t review_note_count{};
    std::uint64_t low_confidence_note_count{};
    std::vector<AutoChartDifficultyResult> difficulties;

    // Preview/install workflow metadata used by the Windows launcher.
    bool cancelled{};
    std::filesystem::path review_index_path;
    std::uint64_t high_priority_review_count{};
};

[[nodiscard]] AutoChartMlHealthReport inspect_autochart_ml_backend(
    const AutoChartOptions& options = {},
    bool deep = false
);

[[nodiscard]] AutoChartResult generate_autochart_mod(
    const std::filesystem::path& media_path,
    const AutoChartOptions& options = {}
);

struct AutoChartFileOperationResult final {
    bool ok{};
    bool cancelled{};
    std::string error;
    std::string message;
    std::filesystem::path mod_root;
    std::filesystem::path installed_path;
    std::filesystem::path destination_path;
    std::filesystem::path report_path;
    std::filesystem::path review_index_path;

    [[nodiscard]] explicit operator bool() const noexcept { return ok; }
};

// Install a preview/staging result into the real mods directory. The non-const
// overload rewrites the result paths to the installed location.
[[nodiscard]] AutoChartFileOperationResult install_autochart_mod(
    AutoChartResult& staged,
    const std::filesystem::path& mods_root,
    bool overwrite = false
);
[[nodiscard]] AutoChartFileOperationResult install_autochart_mod(
    const AutoChartResult& staged,
    const std::filesystem::path& mods_root,
    bool overwrite = false
);
[[nodiscard]] AutoChartFileOperationResult install_autochart_mod(
    const std::filesystem::path& staging_root,
    const std::filesystem::path& mods_root,
    bool overwrite = false
);

[[nodiscard]] AutoChartFileOperationResult discard_autochart_staging(
    AutoChartResult& staged
);
[[nodiscard]] AutoChartFileOperationResult discard_autochart_staging(
    const AutoChartResult& staged
);
[[nodiscard]] AutoChartFileOperationResult discard_autochart_staging(
    const std::filesystem::path& staging_root
);

// Convenience overloads for launcher revisions that pass the complete
// AutoChartOptions object instead of spelling out mods_root/overwrite.
[[nodiscard]] inline AutoChartFileOperationResult install_autochart_mod(
    AutoChartResult& staged,
    const AutoChartOptions& options
) {
    return install_autochart_mod(staged, options.mods_root, options.overwrite);
}

[[nodiscard]] inline AutoChartFileOperationResult install_autochart_mod(
    const AutoChartResult& staged,
    const AutoChartOptions& options
) {
    return install_autochart_mod(staged, options.mods_root, options.overwrite);
}

[[nodiscard]] inline AutoChartFileOperationResult install_autochart_mod(
    const std::filesystem::path& staging_root,
    const AutoChartOptions& options
) {
    return install_autochart_mod(staging_root, options.mods_root, options.overwrite);
}

// Tolerate launcher revisions that carry an extra UI-only flag/context after
// the stable install/discard arguments. These wrappers deliberately ignore the
// extra frontend metadata; filesystem behavior stays in the non-template API.
template <typename... Extra>
[[nodiscard]] AutoChartFileOperationResult install_autochart_mod(
    AutoChartResult& staged,
    const std::filesystem::path& mods_root,
    const bool overwrite,
    Extra&&...
) {
    return install_autochart_mod(staged, mods_root, overwrite);
}

template <typename... Extra>
[[nodiscard]] AutoChartFileOperationResult install_autochart_mod(
    const AutoChartResult& staged,
    const std::filesystem::path& mods_root,
    const bool overwrite,
    Extra&&...
) {
    return install_autochart_mod(staged, mods_root, overwrite);
}

template <typename FirstExtra, typename... Extra>
[[nodiscard]] AutoChartFileOperationResult install_autochart_mod(
    AutoChartResult& staged,
    const std::filesystem::path& mods_root,
    FirstExtra&&,
    Extra&&...
) {
    return install_autochart_mod(staged, mods_root, false);
}

template <typename FirstExtra, typename... Extra>
[[nodiscard]] AutoChartFileOperationResult install_autochart_mod(
    const AutoChartResult& staged,
    const std::filesystem::path& mods_root,
    FirstExtra&&,
    Extra&&...
) {
    return install_autochart_mod(staged, mods_root, false);
}

template <typename FirstExtra, typename... Extra>
[[nodiscard]] AutoChartFileOperationResult install_autochart_mod(
    const std::filesystem::path& staging_root,
    const std::filesystem::path& mods_root,
    FirstExtra&&,
    Extra&&...
) {
    return install_autochart_mod(staging_root, mods_root, false);
}

template <typename... Extra>
[[nodiscard]] AutoChartFileOperationResult discard_autochart_staging(
    AutoChartResult& staged,
    Extra&&...
) {
    return discard_autochart_staging(staged);
}

template <typename... Extra>
[[nodiscard]] AutoChartFileOperationResult discard_autochart_staging(
    const std::filesystem::path& staging_root,
    Extra&&...
) {
    return discard_autochart_staging(staging_root);
}

[[nodiscard]] std::string_view to_string(AutoChartMode mode) noexcept;
[[nodiscard]] std::string_view to_string(AutoChartVideoMode mode) noexcept;
[[nodiscard]] std::string_view to_string(AutoChartMlMode mode) noexcept;
[[nodiscard]] std::string_view to_string(AutoChartProgressStage stage) noexcept;

}  // namespace pulseforge
