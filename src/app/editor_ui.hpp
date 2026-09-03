#pragma once

#include "pulseforge/autochart_review.hpp"
#include "pulseforge/editor_choices.hpp"
#include "pulseforge/editor_models.hpp"
#include "pulseforge/audio_transport.hpp"
#include "pulseforge/streaming_chart_editor.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct SDL_Renderer;
struct SDL_Window;

namespace pulseforge {

enum class EditorUiExit {
    closed,
    quit_requested,
    invalid_context,
};

struct EditorUiOutcome {
    EditorUiExit exit{EditorUiExit::closed};
    bool project_saved{};
    bool compatible_json_saved{};
    std::string message;
};

struct ChartEditorUiOptions {
    const EditorStorage* storage{};
    std::filesystem::path project_path;
    std::filesystem::path psych_chart_path;
    std::filesystem::path autosave_path;
    std::chrono::milliseconds autosave_interval{std::chrono::seconds(30)};
    // A preloaded transport can be injected by an embedding host. The editor
    // controls play/pause/seek but never unloads or destroys it.
    AudioTransport* audio{};
    // When no preloaded transport is supplied, the editor creates a private
    // transport for this manifest. Relative paths are first checked as-is,
    // then below audio_search_roots (including nested song directories).
    // An empty manifest falls back to the audio stored in the editor model.
    AudioManifest audio_manifest;
    std::vector<std::filesystem::path> audio_search_roots;
    AudioSettings audio_settings;
    AudioTransportBackend audio_backend{AudioTransportBackend::system_default};
    // Optional AutoChart provenance overlay. The reader streams only the bounded
    // review queue plus on-demand JSONL records; it never materializes the full
    // review stream. F6/Shift+F6 navigate uncertain notes and F7 toggles the
    // overlay while ordinary editor operations remain unchanged.
    std::filesystem::path autochart_review_index_path;
    std::string autochart_review_difficulty;
    bool autochart_review_overlay{true};
    std::size_t autochart_review_queue_limit{5'000U};
    // Callers may provide a pre-indexed catalog, discovery roots, or both.
    // Discovery is bounded and runs once before the modal loop begins.
    ChartEditorChoiceCatalog choices;
    std::vector<std::filesystem::path> choice_discovery_roots;
    ChartEditorChoiceDiscoveryLimits choice_discovery_limits;
};

namespace detail {

// Shared by the materialized and PFC1 editors. Kept in the app-internal
// header so both modal implementations have identical audio semantics.
class ChartEditorAudioSession final {
public:
    ChartEditorAudioSession(
        const ChartEditorUiOptions& options,
        const AudioManifest& model_audio,
        double fallback_duration_ms
    );
    ~ChartEditorAudioSession();

    ChartEditorAudioSession(const ChartEditorAudioSession&) = delete;
    ChartEditorAudioSession& operator=(const ChartEditorAudioSession&) = delete;

    [[nodiscard]] AudioTransport* transport() const noexcept;
    [[nodiscard]] const std::string& error() const noexcept;
    [[nodiscard]] bool audio_requested() const noexcept;

private:
    std::unique_ptr<AudioTransport> owned_;
    AudioTransport* transport_{};
    std::string error_;
    bool audio_requested_{};
    double external_playback_rate_{1.0};
    bool external_looping_{};
};

}  // namespace detail

struct DescriptorEditorUiOptions {
    const EditorStorage* storage{};
    std::filesystem::path psych_json_path;
};

// Modal in the game-loop sense only: these functions reuse the supplied SDL
// window/renderer and return to their caller. They never create or destroy an
// SDL window, renderer, or audio transport.
[[nodiscard]] EditorUiOutcome run_chart_editor_ui(
    SDL_Window* window,
    SDL_Renderer* renderer,
    ChartEditor& editor,
    const ChartEditorUiOptions& options = {}
);

// Bounded-memory editor used when a JSON chart cannot be materialized. The
// source remains in PFC1, edits are an overlay, and Ctrl+S streams both a
// compact patch and a compatible Psych JSON without a total-note limit.
[[nodiscard]] EditorUiOutcome run_streaming_chart_editor_ui(
    SDL_Window* window,
    SDL_Renderer* renderer,
    StreamingChartEditor& editor,
    const ChartEditorUiOptions& options = {}
);

[[nodiscard]] EditorUiOutcome run_character_editor_ui(
    SDL_Window* window,
    SDL_Renderer* renderer,
    CharacterEditor& editor,
    const DescriptorEditorUiOptions& options = {}
);

[[nodiscard]] EditorUiOutcome run_week_editor_ui(
    SDL_Window* window,
    SDL_Renderer* renderer,
    WeekEditor& editor,
    const DescriptorEditorUiOptions& options = {}
);

}  // namespace pulseforge
