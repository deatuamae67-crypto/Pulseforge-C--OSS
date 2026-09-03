#include "editor_ui.hpp"
#include "mobile_touch_controls.hpp"
#include "pulseforge/note_types.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulseforge {

using detail::MobileTouchContext;
using detail::poll_mobile_event;
using detail::present_with_mobile_touch;
using detail::ScopedMobileTouchContext;

namespace {

constexpr float canvas_width = 1'280.0F;
constexpr float canvas_height = 720.0F;
constexpr SDL_FRect chart_grid{28.0F, 112.0F, 900.0F, 520.0F};
constexpr SDL_FRect inspector{948.0F, 94.0F, 304.0F, 548.0F};

constexpr SDL_Color background{10, 10, 28, 255};
constexpr SDL_Color panel_color{24, 22, 50, 244};
constexpr SDL_Color panel_border{88, 76, 138, 255};
constexpr SDL_Color cyan{94, 236, 255, 255};
constexpr SDL_Color purple{154, 103, 255, 255};
constexpr SDL_Color yellow{255, 224, 105, 255};
constexpr SDL_Color white{236, 232, 248, 255};
constexpr SDL_Color muted{174, 164, 202, 255};
constexpr SDL_Color danger{255, 116, 139, 255};
constexpr SDL_Color success{112, 234, 164, 255};

void erase_last_utf8(std::string& value) {
    if (value.empty()) {
        return;
    }
    std::size_t index = value.size() - 1U;
    while (index > 0U
        && (static_cast<unsigned char>(value[index]) & 0xC0U) == 0x80U) {
        --index;
    }
    value.erase(index);
}

void fill(
    SDL_Renderer* const renderer,
    const SDL_FRect& rectangle,
    const SDL_Color color
) {
    static_cast<void>(SDL_SetRenderDrawColor(
        renderer,
        color.r,
        color.g,
        color.b,
        color.a
    ));
    static_cast<void>(SDL_RenderFillRect(renderer, &rectangle));
}

void outline(
    SDL_Renderer* const renderer,
    const SDL_FRect& rectangle,
    const SDL_Color color
) {
    static_cast<void>(SDL_SetRenderDrawColor(
        renderer,
        color.r,
        color.g,
        color.b,
        color.a
    ));
    static_cast<void>(SDL_RenderRect(renderer, &rectangle));
}

void line(
    SDL_Renderer* const renderer,
    const float x1,
    const float y1,
    const float x2,
    const float y2,
    const SDL_Color color
) {
    static_cast<void>(SDL_SetRenderDrawColor(
        renderer,
        color.r,
        color.g,
        color.b,
        color.a
    ));
    static_cast<void>(SDL_RenderLine(renderer, x1, y1, x2, y2));
}

[[nodiscard]] std::string printable(
    const std::string_view value,
    const std::size_t maximum
) {
    std::string result;
    result.reserve(std::min(value.size(), maximum));
    for (const unsigned char character : value) {
        if (result.size() == maximum) {
            break;
        }
        result.push_back(
            character >= 32U && character <= 126U
                ? static_cast<char>(character)
                : '?'
        );
    }
    if (value.size() > maximum && maximum >= 3U) {
        result.resize(maximum - 3U);
        result += "...";
    }
    return result;
}

void text(
    SDL_Renderer* const renderer,
    const float x,
    const float y,
    const std::string_view value,
    const SDL_Color color = white,
    const float scale = 1.15F,
    const std::size_t maximum = 120U
) {
    float old_x = 1.0F;
    float old_y = 1.0F;
    static_cast<void>(SDL_GetRenderScale(renderer, &old_x, &old_y));
    static_cast<void>(SDL_SetRenderDrawColor(
        renderer,
        color.r,
        color.g,
        color.b,
        color.a
    ));
    static_cast<void>(SDL_SetRenderScale(renderer, scale, scale));
    const auto safe = printable(value, maximum);
    static_cast<void>(SDL_RenderDebugText(
        renderer,
        x / scale,
        y / scale,
        safe.c_str()
    ));
    static_cast<void>(SDL_SetRenderScale(renderer, old_x, old_y));
}

[[nodiscard]] bool contains(
    const SDL_FRect& rectangle,
    const float x,
    const float y
) noexcept {
    return x >= rectangle.x && x <= rectangle.x + rectangle.w
        && y >= rectangle.y && y <= rectangle.y + rectangle.h;
}

[[nodiscard]] std::int64_t milliseconds_to_us(const double milliseconds) {
    const long double value = static_cast<long double>(milliseconds) * 1'000.0L;
    if (value <= static_cast<long double>(
            std::numeric_limits<std::int64_t>::min()
        )) {
        return std::numeric_limits<std::int64_t>::min();
    }
    if (value >= static_cast<long double>(
            std::numeric_limits<std::int64_t>::max()
        )) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(std::llround(value));
}

[[nodiscard]] double initial_bpm(const StreamingChartEditor& editor) {
    if (editor.metadata().tempos.empty()) {
        return 120.0;
    }
    const double bpm = editor.metadata().tempos.front().bpm;
    return std::isfinite(bpm) && bpm > 0.0 ? bpm : 120.0;
}

[[nodiscard]] ChartEditorChoiceCatalog streaming_editor_choices(
    const StreamingChartEditor& editor,
    const ChartEditorUiOptions& options
) {
    ChartEditorChoiceCatalog choices = options.choices;
    for (const auto id : builtin_note_type_ids()) {
        choices.note_types.emplace_back(id);
    }
    for (const auto& value : editor.note_kinds()) {
        choices.note_types.push_back(value);
    }
    choices.note_styles.insert(
        choices.note_styles.end(),
        {"funkin", "normal", "pixel", "NOTE_assets", "NOTE_assets-classic"}
    );
    choices.note_styles.push_back(editor.metadata().note_style);

    auto discovery_roots = options.choice_discovery_roots;
    if (options.storage != nullptr) {
        discovery_roots.push_back(options.storage->root());
    }
    auto discovered = discovery_roots.empty()
        ? ChartEditorChoiceCatalog{}
        : discover_chart_editor_choices(
              discovery_roots,
              options.choice_discovery_limits
          );
    merge_chart_editor_choices(choices, discovered);
    normalize_chart_editor_choices(choices);
    return choices;
}

constexpr SDL_FRect streaming_picker_list{320.0F, 232.0F, 640.0F, 288.0F};
constexpr SDL_FRect streaming_picker_custom{320.0F, 532.0F, 640.0F, 40.0F};
constexpr float streaming_picker_row_height = 32.0F;

class StreamingChoicePicker final {
public:
    StreamingChoicePicker(
        std::string title,
        const std::size_t maximum_bytes,
        std::vector<std::string> choices,
        const bool show_note_skin_preview = false
    ) : model_(9U),
        title_(std::move(title)),
        maximum_bytes_(std::max<std::size_t>(maximum_bytes, 1U)),
        base_choices_(std::move(choices)),
        show_note_skin_preview_(show_note_skin_preview) {}

    void open(SDL_Window* const window, const std::string_view current) {
        active_ = true;
        accept_custom_ = false;
        current_ = std::string(current);
        query_.clear();
        error_.clear();
        auto choices = base_choices_;
        if (!current.empty()) {
            choices.emplace_back(current);
        }
        model_.set_options(std::move(choices));
        model_.set_query(query_);
        static_cast<void>(model_.select_value(current));
        static_cast<void>(SDL_StartTextInput(window));
    }

    void close(SDL_Window* const window) {
        if (active_) {
            static_cast<void>(SDL_StopTextInput(window));
        }
        active_ = false;
    }

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] const std::string& query() const noexcept { return query_; }
    [[nodiscard]] const SearchableOptionModel& model() const noexcept {
        return model_;
    }
    [[nodiscard]] std::string_view title() const noexcept { return title_; }
    [[nodiscard]] std::string_view current() const noexcept { return current_; }
    [[nodiscard]] std::string_view error() const noexcept { return error_; }
    [[nodiscard]] bool show_note_skin_preview() const noexcept {
        return show_note_skin_preview_;
    }
    void remember(std::string value) {
        if (!value.empty()) {
            base_choices_.push_back(std::move(value));
        }
    }
    [[nodiscard]] std::string_view preview_value() const noexcept {
        const auto selected = model_.selected_value();
        return selected.has_value() ? *selected : std::string_view(current_);
    }

    enum class Result { none, accepted, cancelled };

    [[nodiscard]] Result handle(SDL_Window* const window, const SDL_Event& event) {
        if (!active_) {
            return Result::none;
        }
        if (event.type == SDL_EVENT_TEXT_INPUT) {
            const std::string_view incoming{event.text.text};
            const auto remaining = maximum_bytes_ - std::min(
                maximum_bytes_,
                query_.size()
            );
            const auto accepted = bounded_chart_text_prefix_bytes(incoming, remaining);
            if (accepted != 0U) {
                query_.append(incoming.substr(0U, accepted));
                model_.set_query(query_);
            }
            error_ = accepted == incoming.size()
                ? std::string{}
                : "Text is invalid UTF-8, contains a control, or is too long";
            return Result::none;
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            model_.scroll_view(event.wheel.y > 0.0F ? -1 : 1);
            return Result::none;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
            && event.button.button == SDL_BUTTON_LEFT) {
            if (contains(streaming_picker_custom, event.button.x, event.button.y)) {
                if (!query_.empty()) {
                    accept_custom_ = true;
                    close(window);
                    return Result::accepted;
                }
                error_ = "Type a free-form ID first";
                return Result::none;
            }
            if (contains(streaming_picker_list, event.button.x, event.button.y)) {
                const auto row = static_cast<std::size_t>(std::clamp(
                    static_cast<int>(
                        (event.button.y - streaming_picker_list.y)
                        / streaming_picker_row_height
                    ),
                    0,
                    8
                ));
                static_cast<void>(model_.select_visible_row(row));
            }
            return Result::none;
        }
        if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
            return Result::none;
        }
        const bool control = (event.key.mod & SDL_KMOD_CTRL) != 0;
        if (control && event.key.scancode == SDL_SCANCODE_A) {
            query_.clear();
            model_.set_query(query_);
            error_.clear();
            return Result::none;
        }
        if (control && event.key.scancode == SDL_SCANCODE_V) {
            char* const clipboard = SDL_GetClipboardText();
            if (clipboard != nullptr) {
                const std::string_view incoming{clipboard};
                const auto remaining = maximum_bytes_ - std::min(
                    maximum_bytes_,
                    query_.size()
                );
                const auto accepted = bounded_chart_text_prefix_bytes(
                    incoming,
                    remaining
                );
                query_.append(incoming.substr(0U, accepted));
                model_.set_query(query_);
                error_ = accepted == incoming.size()
                    ? std::string{}
                    : "Clipboard text was truncated before invalid/oversized data";
                SDL_free(clipboard);
            }
            return Result::none;
        }
        switch (event.key.scancode) {
        case SDL_SCANCODE_ESCAPE:
            close(window);
            return Result::cancelled;
        case SDL_SCANCODE_BACKSPACE:
            if (!query_.empty()) {
                erase_last_utf8(query_);
                model_.set_query(query_);
                error_.clear();
            }
            return Result::none;
        case SDL_SCANCODE_UP: model_.move_selection(-1); return Result::none;
        case SDL_SCANCODE_DOWN: model_.move_selection(1); return Result::none;
        case SDL_SCANCODE_PAGEUP: model_.page_selection(-1); return Result::none;
        case SDL_SCANCODE_PAGEDOWN: model_.page_selection(1); return Result::none;
        case SDL_SCANCODE_HOME: model_.select_home(); return Result::none;
        case SDL_SCANCODE_END: model_.select_end(); return Result::none;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            accept_custom_ = control && !query_.empty();
            if (!accept_custom_ && !model_.selected_value().has_value()) {
                error_ = query_.empty()
                    ? "Select one of the rows"
                    : "No selected match: use CTRL+ENTER or click FREE-FORM";
                return Result::none;
            }
            close(window);
            return Result::accepted;
        default:
            return Result::none;
        }
    }

    [[nodiscard]] std::string accepted_value() const {
        if (accept_custom_ && !query_.empty()) {
            return query_;
        }
        const auto selected = model_.selected_value();
        if (selected.has_value()) {
            return std::string(*selected);
        }
        return accept_custom_ ? query_ : std::string{};
    }

private:
    SearchableOptionModel model_;
    std::string title_;
    std::string current_;
    std::string query_;
    std::string error_;
    std::size_t maximum_bytes_{};
    std::vector<std::string> base_choices_;
    bool active_{};
    bool accept_custom_{};
    bool show_note_skin_preview_{};
};

void draw_streaming_choice_picker(
    SDL_Renderer* const renderer,
    const StreamingChoicePicker& picker
) {
    fill(renderer, {250.0F, 90.0F, 780.0F, 566.0F}, {7, 8, 22, 248});
    outline(renderer, {250.0F, 90.0F, 780.0F, 566.0F}, cyan);
    text(renderer, 286.0F, 122.0F, picker.title(), cyan, 1.45F, 62U);
    if (picker.show_note_skin_preview()) {
        constexpr std::array<std::string_view, 4U> arrows{"<", "v", "^", ">"};
        constexpr std::array<SDL_Color, 4U> colors{
            SDL_Color{194, 87, 255, 255},
            SDL_Color{54, 224, 255, 255},
            SDL_Color{73, 245, 142, 255},
            SDL_Color{255, 77, 145, 255},
        };
        for (std::size_t lane = 0U; lane < arrows.size(); ++lane) {
            const float x = 830.0F + static_cast<float>(lane) * 42.0F;
            fill(renderer, {x, 108.0F, 34.0F, 34.0F}, {15, 14, 34, 255});
            outline(renderer, {x, 108.0F, 34.0F, 34.0F}, colors[lane]);
            text(renderer, x + 11.0F, 119.0F, arrows[lane], colors[lane], 1.0F);
        }
        text(
            renderer,
            830.0F,
            146.0F,
            picker.preview_value(),
            white,
            0.72F,
            28U
        );
    }
    fill(renderer, {286.0F, 164.0F, 708.0F, 46.0F}, {30, 27, 62, 255});
    outline(renderer, {286.0F, 164.0F, 708.0F, 46.0F}, purple);
    text(
        renderer,
        302.0F,
        180.0F,
        picker.query().empty() ? "type to filter..." : picker.query(),
        picker.query().empty() ? muted : white,
        1.15F,
        88U
    );
    const auto& model = picker.model();
    const auto first = model.first_visible_row();
    const auto selected = model.selected_row();
    float y = streaming_picker_list.y + 8.0F;
    for (std::size_t visible = 0U;
         visible < model.visible_rows() && first + visible < model.result_count();
         ++visible) {
        const auto row = first + visible;
        const bool active = selected.has_value() && *selected == row;
        if (active) {
            fill(
                renderer,
                {streaming_picker_list.x, y - 8.0F, streaming_picker_list.w, 28.0F},
                {74, 51, 137, 230}
            );
            fill(renderer, {streaming_picker_list.x, y - 8.0F, 5.0F, 28.0F}, cyan);
        }
        text(
            renderer,
            338.0F,
            y,
            model.result_at(row),
            active ? white : muted,
            1.05F,
            72U
        );
        y += streaming_picker_row_height;
    }
    if (model.result_count() == 0U) {
        text(
            renderer,
            338.0F,
            258.0F,
            "No existing match. Use the explicit FREE-FORM action below.",
            yellow,
            1.05F
        );
    }
    const bool custom_available = !picker.query().empty();
    fill(
        renderer,
        streaming_picker_custom,
        custom_available ? SDL_Color{43, 34, 82, 255}
                         : SDL_Color{22, 20, 43, 255}
    );
    outline(
        renderer,
        streaming_picker_custom,
        custom_available ? purple : panel_border
    );
    text(
        renderer,
        streaming_picker_custom.x + 14.0F,
        streaming_picker_custom.y + 13.0F,
        custom_available
            ? "FREE-FORM // USE EXACT TEXT: " + picker.query()
            : "FREE-FORM // type any ID above",
        custom_available ? yellow : muted,
        0.98F,
        86U
    );
    const auto first_display = model.result_count() == 0U
        ? 0U
        : model.first_visible_row() + 1U;
    const auto last_display = std::min(
        model.result_count(),
        model.first_visible_row() + model.visible_rows()
    );
    text(
        renderer,
        286.0F,
        582.0F,
        std::to_string(first_display) + "-" + std::to_string(last_display)
            + " / " + std::to_string(model.result_count())
            + "   Current: " + std::string(picker.current()),
        muted,
        0.88F,
        104U
    );
    text(
        renderer,
        286.0F,
        605.0F,
        picker.error().empty()
            ? "TYPE filter   ENTER selected row   CTRL+ENTER exact custom   ESC cancel"
            : picker.error(),
        picker.error().empty() ? muted : danger,
        1.0F
    );
}

[[nodiscard]] double quantize(
    const double time_ms,
    const double bpm,
    const int subdivisions
) {
    const double step = 60'000.0 / bpm
        / static_cast<double>(std::max(subdivisions, 1));
    return std::round(time_ms / step) * step;
}

class WindowTitleGuard final {
public:
    WindowTitleGuard(SDL_Window* const window, SDL_Renderer* const renderer)
        : window_(window), renderer_(renderer) {
        const char* const current = SDL_GetWindowTitle(window_);
        old_title_ = current != nullptr ? current : "PulseForge";
        static_cast<void>(SDL_GetRenderScale(renderer_, &old_scale_x_, &old_scale_y_));
        static_cast<void>(SDL_SetRenderScale(renderer_, 1.0F, 1.0F));
        SDL_SetWindowTitle(window_, "PulseForge // Streaming Chart Editor");
    }

    ~WindowTitleGuard() {
        static_cast<void>(SDL_SetRenderClipRect(renderer_, nullptr));
        static_cast<void>(SDL_SetRenderScale(renderer_, old_scale_x_, old_scale_y_));
        SDL_SetWindowTitle(window_, old_title_.c_str());
    }

private:
    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    std::string old_title_;
    float old_scale_x_{1.0F};
    float old_scale_y_{1.0F};
};

}  // namespace

EditorUiOutcome run_streaming_chart_editor_ui(
    SDL_Window* const window,
    SDL_Renderer* const renderer,
    StreamingChartEditor& editor,
    const ChartEditorUiOptions& options
) {
    EditorUiOutcome outcome;
    if (window == nullptr || renderer == nullptr) {
        outcome.exit = EditorUiExit::invalid_context;
        outcome.message = "Streaming Chart Editor requires an SDL context";
        return outcome;
    }

    ScopedMobileTouchContext touch_context{MobileTouchContext::editor};

    WindowTitleGuard guard(window, renderer);
    static_cast<void>(SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND));

    double playhead_ms = 0.0;
    double milliseconds_per_pixel = 4.0;
    int subdivisions = 4;
    std::uint64_t sustain_steps = 0U;
    float mouse_x = 0.0F;
    float mouse_y = 0.0F;
    bool running = true;
    std::optional<StreamingEditorNoteId> selected_note;
    const auto available_choices = streaming_editor_choices(editor, options);
    StreamingChoicePicker note_type_picker(
        "NOTE TYPE // SEARCH OR WRITE ANY NAME",
        maximum_chart_note_kind_bytes,
        available_choices.note_types
    );
    StreamingChoicePicker note_style_picker(
        "NOTE SKIN / STYLE // SEARCH OR WRITE ANY ID",
        1'024U,
        available_choices.note_styles,
        true
    );
    std::string active_note_style = editor.metadata().note_style;
    if (active_note_style.empty()) {
        active_note_style = "funkin";
    }
    std::uint32_t active_note_kind_id{};
    if (const auto normal = std::find(
            editor.note_kinds().begin(),
            editor.note_kinds().end(),
            std::string{"normal"}
        ); normal != editor.note_kinds().end()) {
        active_note_kind_id = static_cast<std::uint32_t>(
            std::distance(editor.note_kinds().begin(), normal)
        );
    }
    StreamingEditorViewport viewport;
    double cached_start = std::numeric_limits<double>::quiet_NaN();
    double cached_end = std::numeric_limits<double>::quiet_NaN();
    std::uint64_t cached_revision = std::numeric_limits<std::uint64_t>::max();

    const double bpm = initial_bpm(editor);
    const auto structural_duration_ms = std::max(
        1'000.0,
        static_cast<double>(editor.content_end_us()) / 1'000.0 + 2'000.0
    );
    detail::ChartEditorAudioSession audio_session(
        options,
        editor.metadata().audio,
        structural_duration_ms
    );
    auto* const editor_audio = audio_session.transport();
    if (editor_audio != nullptr) {
        playhead_ms = editor_audio->position_ms();
    }
    const auto duration_ms = editor_audio == nullptr
        ? structural_duration_ms
        : std::max(structural_duration_ms, editor_audio->duration_ms());
    std::string status = audio_session.error().empty()
        ? "PFC1 bounded editor: LMB add/select, RMB delete, Ctrl+S export, Esc back"
        : "Audio unavailable: " + audio_session.error();
    bool status_error = !audio_session.error().empty();

    const auto seek_to = [&](const double requested_ms) {
        playhead_ms = std::clamp(requested_ms, 0.0, duration_ms);
        if (editor_audio != nullptr) {
            editor_audio->seek_ms(playhead_ms);
        }
    };

    const auto refresh_viewport = [&]() {
        const double half = static_cast<double>(chart_grid.h * 0.5F)
            * milliseconds_per_pixel;
        const double first = playhead_ms - half;
        const double last = playhead_ms + half;
        if (first == cached_start && last == cached_end
            && cached_revision == editor.revision()) {
            return;
        }
        viewport = editor.query(
            milliseconds_to_us(first),
            milliseconds_to_us(last),
            250'000U
        );
        cached_start = first;
        cached_end = last;
        cached_revision = editor.revision();
        if (!viewport) {
            status = "Viewport read failed: " + viewport.error;
            status_error = true;
        }
    };

    const auto save = [&]() {
        if (options.storage == nullptr) {
            status = "No editor storage was supplied";
            status_error = true;
            return;
        }
        if (!options.project_path.empty()) {
            const auto patch = editor.save_patch(
                *options.storage,
                options.project_path
            );
            if (!patch) {
                status = "Patch save failed: " + patch.message;
                status_error = true;
                return;
            }
            outcome.project_saved = true;
        }
        if (!options.psych_chart_path.empty()) {
            status = "Streaming compatible Psych JSON to disk...";
            const auto exported = editor.export_psych_json(
                *options.storage,
                options.psych_chart_path
            );
            if (!exported) {
                status = "Psych export failed: " + exported.message;
                status_error = true;
                return;
            }
            outcome.compatible_json_saved = true;
        }
        status = "Large chart patch + compatible Psych JSON saved atomically";
        status_error = false;
    };

    while (running) {
        if (editor_audio != nullptr
            && editor_audio->state() == AudioTransportState::playing) {
            playhead_ms = editor_audio->position_ms();
        }
        SDL_Event event;
        while (poll_mobile_event(&event)) {
            static_cast<void>(SDL_ConvertEventToRenderCoordinates(renderer, &event));
            if (editor_audio != nullptr
                && event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                editor_audio->set_focused(true);
            } else if (editor_audio != nullptr
                       && event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                editor_audio->set_focused(false);
            }
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                outcome.exit = EditorUiExit::quit_requested;
                running = false;
                break;
            }
            if (note_type_picker.active()) {
                const auto picker_result = note_type_picker.handle(window, event);
                if (picker_result == StreamingChoicePicker::Result::accepted) {
                    auto kind = note_type_picker.accepted_value();
                    std::string error;
                    std::optional<PackedNote> selected_snapshot;
                    if (selected_note.has_value()) {
                        selected_snapshot = editor.note_by_id(*selected_note, &error);
                        if (!selected_snapshot.has_value()) {
                            status = error.empty()
                                ? "Selected note no longer exists; type was not applied"
                                : error + "; note type was not applied";
                            status_error = true;
                            if (error.find("removed") != std::string::npos
                                || error.find("no longer exists")
                                    != std::string::npos
                                || error.find("does not exist")
                                    != std::string::npos) {
                                selected_note.reset();
                            }
                            continue;
                        }
                    }
                    const auto kind_id = editor.ensure_note_kind(kind, &error);
                    if (!kind_id.has_value()) {
                        status = error.empty() ? "Note type was rejected" : error;
                        status_error = true;
                        continue;
                    }
                    if (selected_note.has_value()) {
                        auto changed = *selected_snapshot;
                        changed.kind_id = *kind_id;
                        if (!editor.update_note(*selected_note, changed, &error)) {
                            status = error.empty()
                                ? "Selected note type could not be changed"
                                : error;
                            status_error = true;
                            continue;
                        }
                        status = "Selected note type: " + kind;
                    } else {
                        status = "New-note type: " + kind;
                    }
                    active_note_kind_id = *kind_id;
                    note_type_picker.remember(kind);
                    status_error = false;
                } else if (picker_result
                           == StreamingChoicePicker::Result::cancelled) {
                    status = "Note type selection cancelled";
                    status_error = false;
                }
                continue;
            }
            if (note_style_picker.active()) {
                const auto picker_result = note_style_picker.handle(window, event);
                if (picker_result == StreamingChoicePicker::Result::accepted) {
                    auto selected_style = note_style_picker.accepted_value();
                    std::string error;
                    if (!editor.set_note_style(selected_style, &error)) {
                        status = error.empty()
                            ? "Note skin/style was rejected"
                            : error;
                        status_error = true;
                    } else {
                        active_note_style = std::move(selected_style);
                        note_style_picker.remember(active_note_style);
                        status = "Chart note skin: " + active_note_style;
                        status_error = false;
                    }
                } else if (picker_result == StreamingChoicePicker::Result::cancelled) {
                    status = "Note skin selection cancelled";
                    status_error = false;
                }
                continue;
            }
            if (event.type == SDL_EVENT_MOUSE_MOTION) {
                mouse_x = event.motion.x;
                mouse_y = event.motion.y;
                continue;
            }
            if (event.type == SDL_EVENT_KEY_DOWN) {
                const bool control = (event.key.mod & SDL_KMOD_CTRL) != 0;
                if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    running = false;
                } else if (control && event.key.scancode == SDL_SCANCODE_S) {
                    save();
                } else if (event.key.scancode == SDL_SCANCODE_SPACE) {
                    if (editor_audio != nullptr) {
                        if (editor_audio->state() == AudioTransportState::playing) {
                            editor_audio->pause();
                            status = "Audio paused";
                        } else if (editor_audio->state()
                                   == AudioTransportState::paused) {
                            editor_audio->resume();
                            status = "Audio resumed";
                        } else {
                            if (editor_audio->state() == AudioTransportState::ended) {
                                playhead_ms = 0.0;
                            }
                            editor_audio->seek_ms(playhead_ms);
                            editor_audio->play();
                            status = "Audio playing";
                        }
                        status_error = false;
                    } else {
                        status = audio_session.audio_requested()
                            ? "Audio unavailable: " + audio_session.error()
                            : "No song audio was supplied to the Chart Editor";
                        status_error = true;
                    }
                } else if (event.key.scancode == SDL_SCANCODE_UP
                           || event.key.scancode == SDL_SCANCODE_W) {
                    seek_to(playhead_ms - 60'000.0 / bpm);
                } else if (event.key.scancode == SDL_SCANCODE_DOWN
                           || event.key.scancode == SDL_SCANCODE_S) {
                    seek_to(playhead_ms + 60'000.0 / bpm);
                } else if (event.key.scancode == SDL_SCANCODE_LEFT) {
                    seek_to(
                        playhead_ms - 60'000.0 / bpm / subdivisions
                    );
                } else if (event.key.scancode == SDL_SCANCODE_RIGHT) {
                    seek_to(
                        playhead_ms + 60'000.0 / bpm / subdivisions
                    );
                } else if (event.key.scancode == SDL_SCANCODE_EQUALS
                           || event.key.scancode == SDL_SCANCODE_KP_PLUS) {
                    milliseconds_per_pixel = std::max(
                        0.005,
                        milliseconds_per_pixel * 0.8
                    );
                } else if (event.key.scancode == SDL_SCANCODE_MINUS
                           || event.key.scancode == SDL_SCANCODE_KP_MINUS) {
                    milliseconds_per_pixel = std::min(
                        50'000.0,
                        milliseconds_per_pixel * 1.25
                    );
                } else if (event.key.scancode == SDL_SCANCODE_LEFTBRACKET) {
                    subdivisions = std::max(1, subdivisions / 2);
                } else if (event.key.scancode == SDL_SCANCODE_RIGHTBRACKET) {
                    subdivisions = std::min(64, subdivisions * 2);
                } else if (event.key.scancode == SDL_SCANCODE_Q) {
                    if (sustain_steps != 0U) {
                        --sustain_steps;
                    }
                } else if (event.key.scancode == SDL_SCANCODE_E) {
                    if (sustain_steps != std::numeric_limits<std::uint64_t>::max()) {
                        ++sustain_steps;
                    }
                } else if (event.key.scancode == SDL_SCANCODE_N) {
                    const auto kinds = editor.note_kinds();
                    const auto current = active_note_kind_id < kinds.size()
                        ? std::string_view(kinds[active_note_kind_id])
                        : std::string_view{"normal"};
                    note_type_picker.open(window, current);
                } else if (event.key.scancode == SDL_SCANCODE_K) {
                    note_style_picker.open(window, active_note_style);
                } else if ((event.key.scancode == SDL_SCANCODE_DELETE
                            || event.key.scancode == SDL_SCANCODE_BACKSPACE)
                           && selected_note.has_value()) {
                    std::string error;
                    if (editor.remove_note(*selected_note, &error)) {
                        selected_note.reset();
                        status = "Note removed from large-chart overlay";
                        status_error = false;
                    } else {
                        status = error;
                        status_error = true;
                    }
                }
                continue;
            }
            if (event.type == SDL_EVENT_MOUSE_WHEEL
                && contains(chart_grid, mouse_x, mouse_y)) {
                const bool control = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
                if (control) {
                    milliseconds_per_pixel = std::clamp(
                        milliseconds_per_pixel
                            * (event.wheel.y > 0.0F ? 0.8 : 1.25),
                        0.005,
                        50'000.0
                    );
                } else {
                    seek_to(
                        playhead_ms
                            - static_cast<double>(event.wheel.y)
                                * 60'000.0 / bpm
                    );
                }
                continue;
            }
            const SDL_FRect timeline{28.0F, 648.0F, 900.0F, 24.0F};
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                && event.button.button == SDL_BUTTON_LEFT
                && contains(timeline, event.button.x, event.button.y)) {
                const auto ratio = std::clamp(
                    static_cast<double>(
                        (event.button.x - timeline.x) / timeline.w
                    ),
                    0.0,
                    1.0
                );
                seek_to(duration_ms * ratio);
                continue;
            }
            if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN
                || !contains(chart_grid, event.button.x, event.button.y)) {
                continue;
            }

            refresh_viewport();
            const auto columns = static_cast<std::uint32_t>(editor.key_count()) * 2U;
            const float column_width = chart_grid.w
                / static_cast<float>(columns);
            const auto requested_column = static_cast<std::uint32_t>(std::clamp(
                static_cast<int>((event.button.x - chart_grid.x) / column_width),
                0,
                static_cast<int>(columns - 1U)
            ));
            const double requested_time = playhead_ms
                + static_cast<double>(
                    event.button.y - (chart_grid.y + chart_grid.h * 0.5F)
                ) * milliseconds_per_pixel;

            std::optional<StreamingEditorNoteId> nearest;
            double nearest_distance = milliseconds_per_pixel * 12.0;
            for (const auto& candidate : viewport.notes) {
                const auto column = static_cast<std::uint32_t>(candidate.note.lane)
                    + (candidate.note.owner == PackedNoteOwner::player
                        ? editor.key_count()
                        : 0U);
                if (column != requested_column) {
                    continue;
                }
                const double time = static_cast<double>(candidate.note.time_us)
                    / 1'000.0;
                const double distance = std::abs(time - requested_time);
                if (distance <= nearest_distance) {
                    nearest = candidate.id;
                    nearest_distance = distance;
                }
            }

            if (event.button.button == SDL_BUTTON_RIGHT) {
                if (nearest.has_value()) {
                    std::string error;
                    if (editor.remove_note(*nearest, &error)) {
                        selected_note.reset();
                        status = "Note removed from large-chart overlay";
                        status_error = false;
                    } else {
                        status = error;
                        status_error = true;
                    }
                }
                continue;
            }
            if (event.button.button != SDL_BUTTON_LEFT) {
                continue;
            }
            if (nearest.has_value()) {
                selected_note = nearest;
                const auto selected = std::find_if(
                    viewport.notes.begin(),
                    viewport.notes.end(),
                    [&](const StreamingEditorViewportNote& note) {
                        return note.id == *nearest;
                    }
                );
                if (selected != viewport.notes.end()
                    && selected->note.kind_id < editor.note_kinds().size()) {
                    active_note_kind_id = selected->note.kind_id;
                }
                status = "Note selected; N changes type, Delete or RMB removes it";
                status_error = false;
                continue;
            }

            PackedNote note;
            note.time_us = milliseconds_to_us(quantize(
                requested_time,
                bpm,
                subdivisions
            ));
            const long double sustain_us = static_cast<long double>(
                60'000'000.0 / bpm / static_cast<double>(subdivisions)
            ) * static_cast<long double>(sustain_steps);
            note.duration_us = sustain_us
                    >= static_cast<long double>(
                        std::numeric_limits<std::uint64_t>::max()
                    )
                ? std::numeric_limits<std::uint64_t>::max()
                : static_cast<std::uint64_t>(sustain_us + 0.5L);
            note.owner = requested_column < editor.key_count()
                ? PackedNoteOwner::opponent
                : PackedNoteOwner::player;
            note.lane = static_cast<std::uint16_t>(
                requested_column % editor.key_count()
            );
            note.kind_id = active_note_kind_id;
            std::string error;
            const auto id = editor.add_note(note, &error);
            if (id.has_value()) {
                selected_note = *id;
                status = "Note added without materializing the source chart";
                status_error = false;
            } else {
                status = error;
                status_error = true;
            }
        }

        if (!running) {
            break;
        }
        refresh_viewport();

        static_cast<void>(SDL_SetRenderClipRect(renderer, nullptr));
        fill(renderer, {0.0F, 0.0F, canvas_width, canvas_height}, background);
        fill(renderer, {0.0F, 0.0F, canvas_width, 82.0F}, {22, 15, 48, 255});
        fill(renderer, {0.0F, 80.0F, canvas_width, 2.0F}, cyan);
        text(renderer, 28.0F, 22.0F, "PULSEFORGE // STREAMING CHART EDITOR", cyan, 1.7F);
        text(
            renderer,
            28.0F,
            55.0F,
            "SPACE audio  Arrows/W/S seek  +/- zoom  [ ] quantize  Q/E sustain  N type  K note skin  Ctrl+S save",
            muted,
            1.0F
        );

        fill(renderer, chart_grid, panel_color);
        outline(renderer, chart_grid, panel_border);
        fill(renderer, inspector, panel_color);
        outline(renderer, inspector, panel_border);
        const auto columns = static_cast<std::uint32_t>(editor.key_count()) * 2U;
        const float column_width = chart_grid.w / static_cast<float>(columns);
        for (std::uint32_t column = 1U; column < columns; ++column) {
            const float x = chart_grid.x + column_width * column;
            line(
                renderer,
                x,
                chart_grid.y,
                x,
                chart_grid.y + chart_grid.h,
                column == editor.key_count()
                    ? white
                    : SDL_Color{64, 57, 94, 180}
            );
        }
        const float receptor_y = chart_grid.y + chart_grid.h * 0.5F;
        fill(
            renderer,
            {chart_grid.x, receptor_y - 2.0F, chart_grid.w, 4.0F},
            yellow
        );

        const SDL_Rect clip{
            static_cast<int>(chart_grid.x),
            static_cast<int>(chart_grid.y),
            static_cast<int>(chart_grid.w),
            static_cast<int>(chart_grid.h),
        };
        static_cast<void>(SDL_SetRenderClipRect(renderer, &clip));
        if (viewport.dense_lod) {
            for (const auto& span : viewport.density_spans) {
                const double first_ms = static_cast<double>(span.first_time_us)
                    / 1'000.0;
                const double last_ms = static_cast<double>(span.last_time_us)
                    / 1'000.0;
                const float y1 = receptor_y + static_cast<float>(
                    (first_ms - playhead_ms) / milliseconds_per_pixel
                );
                const float y2 = receptor_y + static_cast<float>(
                    (last_ms - playhead_ms) / milliseconds_per_pixel
                );
                const auto alpha = static_cast<std::uint8_t>(std::clamp(
                    35.0 + std::log10(
                        static_cast<double>(std::max<std::uint64_t>(
                            1U,
                            span.note_count
                        ))
                    ) * 42.0,
                    35.0,
                    220.0
                ));
                fill(
                    renderer,
                    {chart_grid.x,
                     std::min(y1, y2),
                     chart_grid.w,
                     std::max(1.0F, std::abs(y2 - y1))},
                    {purple.r, purple.g, purple.b, alpha}
                );
            }
        }
        for (const auto& visible : viewport.notes) {
            const auto column = static_cast<std::uint32_t>(visible.note.lane)
                + (visible.note.owner == PackedNoteOwner::player
                    ? editor.key_count()
                    : 0U);
            const float x = chart_grid.x + column_width * column + 2.0F;
            const double time_ms = static_cast<double>(visible.note.time_us)
                / 1'000.0;
            const float y = receptor_y + static_cast<float>(
                (time_ms - playhead_ms) / milliseconds_per_pixel
            );
            const SDL_Color color = visible.note.owner == PackedNoteOwner::player
                ? cyan
                : danger;
            const float height = viewport.dense_lod ? 1.0F : 8.0F;
            const SDL_FRect rectangle{
                x,
                y - height * 0.5F,
                std::max(1.0F, column_width - 4.0F),
                height,
            };
            fill(renderer, rectangle, color);
            if (selected_note == visible.id) {
                outline(renderer, rectangle, yellow);
            }
        }
        static_cast<void>(SDL_SetRenderClipRect(renderer, nullptr));

        text(renderer, 966.0F, 116.0F, "LARGE CHART", cyan, 1.4F);
        text(renderer, 966.0F, 150.0F, editor.metadata().title, white, 1.05F, 35U);
        text(
            renderer,
            966.0F,
            184.0F,
            "Notes: " + std::to_string(editor.note_count()),
            yellow
        );
        text(
            renderer,
            966.0F,
            210.0F,
            "PFC chunks: " + std::to_string(editor.reader().chunk_count()),
            muted
        );
        text(
            renderer,
            966.0F,
            236.0F,
            "Time: " + std::to_string(
                static_cast<std::uint64_t>(std::max(0.0, playhead_ms))
            ) + " ms",
            muted
        );
        text(
            renderer,
            966.0F,
            262.0F,
            "BPM: " + std::to_string(bpm),
            muted
        );
        text(
            renderer,
            966.0F,
            288.0F,
            "Quant: 1/" + std::to_string(subdivisions),
            muted
        );
        text(
            renderer,
            966.0F,
            314.0F,
            "Sustain: " + std::to_string(sustain_steps),
            muted
        );
        const auto kinds = editor.note_kinds();
        text(
            renderer,
            966.0F,
            338.0F,
            "Type: " + std::string(
                active_note_kind_id < kinds.size()
                    ? std::string_view(kinds[active_note_kind_id])
                    : std::string_view{"normal"}
            ),
            yellow,
            0.9F,
            38U
        );
        text(
            renderer,
            966.0F,
            364.0F,
            "Skin: " + active_note_style,
            yellow,
            0.9F,
            38U
        );
        text(
            renderer,
            966.0F,
            394.0F,
            viewport.dense_lod
                ? "DENSITY LOD: zoom in to edit every note"
                : "Exact indexed notes",
            viewport.dense_lod ? yellow : success,
            0.9F,
            38U
        );
        text(
            renderer,
            966.0F,
            434.0F,
            "Source stays on disk; edits use an overlay",
            muted,
            0.9F,
            40U
        );

        const float progress = static_cast<float>(std::clamp(
            playhead_ms / duration_ms,
            0.0,
            1.0
        ));
        const SDL_FRect timeline{28.0F, 648.0F, 900.0F, 24.0F};
        fill(renderer, timeline, panel_color);
        fill(
            renderer,
            {timeline.x, timeline.y, timeline.w * progress, timeline.h},
            {cyan.r, cyan.g, cyan.b, 90}
        );
        outline(renderer, timeline, panel_border);
        text(
            renderer,
            38.0F,
            654.0F,
            std::to_string(static_cast<std::uint64_t>(playhead_ms)) + " / "
                + std::to_string(static_cast<std::uint64_t>(duration_ms))
                + " ms",
            white,
            0.9F
        );
        if (!status.empty()) {
            text(
                renderer,
                28.0F,
                690.0F,
                status,
                status_error ? danger : success,
                0.95F,
                150U
            );
        }
        if (note_type_picker.active()) {
            draw_streaming_choice_picker(renderer, note_type_picker);
        } else if (note_style_picker.active()) {
            draw_streaming_choice_picker(renderer, note_style_picker);
        }
        static_cast<void>(present_with_mobile_touch(renderer));
        SDL_Delay(1U);
    }

    outcome.message = status;
    return outcome;
}

}  // namespace pulseforge
