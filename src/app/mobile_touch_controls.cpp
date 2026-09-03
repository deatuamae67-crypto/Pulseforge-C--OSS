#include "mobile_touch_controls.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulseforge::detail {
namespace {

enum class TouchAction : std::uint8_t {
    lane_0,
    lane_1,
    lane_2,
    lane_3,
    lane_4,
    lane_5,
    lane_6,
    lane_7,
    lane_8,
    lane_9,
    lane_10,
    lane_11,
    lane_12,
    lane_13,
    lane_14,
    lane_15,
    lane_16,
    lane_17,
    ui_left,
    ui_down,
    ui_up,
    ui_right,
    accept,
    back,
    pause,
    volume_down,
    volume_mute,
    volume_up,
    editor_play,
    editor_save,
    editor_undo,
    editor_redo,
    editor_delete,
    count,
};

constexpr std::size_t action_count = static_cast<std::size_t>(TouchAction::count);
constexpr std::size_t maximum_touch_lanes = 18U;
constexpr SDL_KeyboardID synthetic_touch_keyboard_id = 0x50544654U;

struct KeySpec final {
    SDL_Scancode scancode{SDL_SCANCODE_UNKNOWN};
    SDL_Keymod modifiers{SDL_KMOD_NONE};
};

struct Button final {
    TouchAction action{};
    SDL_FRect rectangle{};
    SDL_Color color{};
    std::string_view label;
};

struct FingerState final {
    std::optional<TouchAction> action;
    bool pointer{};
    float window_x{};
    float window_y{};
};

[[nodiscard]] constexpr std::size_t action_index(const TouchAction action) noexcept {
    return static_cast<std::size_t>(action);
}

[[nodiscard]] constexpr TouchAction lane_action(const std::size_t lane) noexcept {
    return static_cast<TouchAction>(action_index(TouchAction::lane_0) + lane);
}

[[nodiscard]] bool environment_truthy(const char* name) noexcept {
    const char* const raw = SDL_GetEnvironmentVariable(
        SDL_GetEnvironment(),
        name
    );
    if (raw == nullptr) {
        return false;
    }
    const std::string_view value{raw};
    return value == "1" || value == "true" || value == "TRUE"
        || value == "yes" || value == "YES" || value == "on"
        || value == "ON";
}

[[nodiscard]] bool native_android_build() noexcept {
#if defined(__ANDROID__)
    return true;
#else
    return false;
#endif
}

[[nodiscard]] KeySpec parse_key_spec(
    std::string name,
    const SDL_Scancode fallback,
    const SDL_Keymod fallback_modifiers = SDL_KMOD_NONE
) {
    name = canonicalize_input_name(name);
    SDL_Keymod modifiers = SDL_KMOD_NONE;
    bool consumed = true;
    while (consumed) {
        consumed = false;
        const auto consume = [&](const std::string_view prefix, const SDL_Keymod flag) {
            if (name.starts_with(prefix)) {
                modifiers = static_cast<SDL_Keymod>(modifiers | flag);
                name.erase(0U, prefix.size());
                consumed = true;
            }
        };
        consume("ctrl+", SDL_KMOD_CTRL);
        consume("alt+", SDL_KMOD_ALT);
        consume("shift+", SDL_KMOD_SHIFT);
        consume("gui+", SDL_KMOD_GUI);
    }

    SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
    if (name == "equals" || name == "=") {
        scancode = SDL_SCANCODE_EQUALS;
    } else if (name == "minus" || name == "-") {
        scancode = SDL_SCANCODE_MINUS;
    } else {
        scancode = SDL_GetScancodeFromName(name.c_str());
    }
    if (scancode == SDL_SCANCODE_UNKNOWN) {
        return {fallback, fallback_modifiers};
    }
    return {scancode, modifiers};
}

[[nodiscard]] KeySpec action_key(
    const InputBindings& bindings,
    const std::string_view action,
    const SDL_Scancode fallback,
    const SDL_Keymod fallback_modifiers = SDL_KMOD_NONE
) {
    const auto* const configured = find_action_binding(bindings, action);
    if (configured != nullptr) {
        for (const auto& input : configured->inputs) {
            if (input.device == InputDevice::keyboard) {
                return parse_key_spec(input.name, fallback, fallback_modifiers);
            }
        }
    }
    return {fallback, fallback_modifiers};
}

[[nodiscard]] SDL_FRect inset_rect(const SDL_FRect rectangle, const float amount) {
    const float inset = std::min(
        std::max(amount, 0.0F),
        std::max(0.0F, std::min(rectangle.w, rectangle.h) * 0.45F)
    );
    return {
        rectangle.x + inset,
        rectangle.y + inset,
        std::max(0.0F, rectangle.w - inset * 2.0F),
        std::max(0.0F, rectangle.h - inset * 2.0F),
    };
}

[[nodiscard]] bool contains(const SDL_FRect& rectangle, const float x, const float y) {
    return x >= rectangle.x && y >= rectangle.y
        && x <= rectangle.x + rectangle.w
        && y <= rectangle.y + rectangle.h;
}

[[nodiscard]] float squared_distance_to_center(
    const SDL_FRect& rectangle,
    const float x,
    const float y
) {
    const float dx = x - (rectangle.x + rectangle.w * 0.5F);
    const float dy = y - (rectangle.y + rectangle.h * 0.5F);
    return dx * dx + dy * dy;
}

}  // namespace

class MobileTouchControls::Impl final {
public:
    Impl() = default;

    ~Impl() {
        detach_watch();
    }

    void configure(
        SDL_Window* const window,
        SDL_Renderer* const renderer,
        const TouchSettings& settings,
        const InputBindings& bindings
    ) {
        window_ = window;
        renderer_ = renderer;
        settings_ = settings;
        bindings_ = bindings;
        platform_active_ = native_android_build()
            || environment_truthy("PULSEFORGE_FORCE_TOUCH_CONTROLS");
        build_key_map();
        if (!platform_active_ || window_ == nullptr || renderer_ == nullptr) {
            set_context(MobileTouchContext::disabled, 4U);
            return;
        }
        // The router creates exactly one mouse stream for unclaimed editor/menu
        // touches. Disabling SDL's parallel emulation prevents duplicate clicks.
        static_cast<void>(SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0"));
        attach_watch();
    }

    void set_context(
        const MobileTouchContext context,
        const std::uint16_t gameplay_key_count
    ) {
        if (context_ == context && gameplay_key_count_ == gameplay_key_count) {
            return;
        }
        cancel_all(SDL_GetTicksNS());
        context_ = platform_active_ ? context : MobileTouchContext::disabled;
        gameplay_key_count_ = std::clamp<std::uint16_t>(gameplay_key_count, 1U, 18U);
    }

    [[nodiscard]] bool poll_event(SDL_Event* const output) {
        if (output == nullptr) {
            return false;
        }
        if (!platform_active_) {
            return SDL_PollEvent(output);
        }

        if (lifecycle_cancel_requested_.exchange(false, std::memory_order_acq_rel)) {
            cancel_all(SDL_GetTicksNS());
        }
        if (pop_pending(output)) {
            return true;
        }

        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST
                || event.type == SDL_EVENT_WILL_ENTER_BACKGROUND
                || event.type == SDL_EVENT_DID_ENTER_BACKGROUND
                || event.type == SDL_EVENT_TERMINATING) {
                cancel_all(event.common.timestamp);
                pending_.push_back(event);
                return pop_pending(output);
            }

            if (is_touch_event(event.type)
                && context_ != MobileTouchContext::disabled) {
                process_touch(event.tfinger);
                if (pop_pending(output)) {
                    return true;
                }
                continue;
            }

            *output = event;
            return true;
        }
        return pop_pending(output);
    }

    void render(SDL_Renderer* const renderer) {
        if (!platform_active_ || renderer == nullptr || renderer != renderer_
            || context_ == MobileTouchContext::disabled) {
            return;
        }
        const auto buttons = build_buttons();
        if (buttons.empty()) {
            return;
        }

        Uint8 old_r = 0U;
        Uint8 old_g = 0U;
        Uint8 old_b = 0U;
        Uint8 old_a = 0U;
        SDL_BlendMode old_blend = SDL_BLENDMODE_NONE;
        float old_scale_x = 1.0F;
        float old_scale_y = 1.0F;
        static_cast<void>(SDL_GetRenderDrawColor(
            renderer,
            &old_r,
            &old_g,
            &old_b,
            &old_a
        ));
        static_cast<void>(SDL_GetRenderDrawBlendMode(renderer, &old_blend));
        static_cast<void>(SDL_GetRenderScale(renderer, &old_scale_x, &old_scale_y));
        static_cast<void>(SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND));
        static_cast<void>(SDL_SetRenderScale(renderer, 1.0F, 1.0F));

        const float configured_opacity = std::clamp(settings_.opacity, 0.05F, 1.0F);
        for (const auto& button : buttons) {
            const bool pressed = action_refs_[action_index(button.action)] != 0U;
            const float alpha_scale = context_ == MobileTouchContext::gameplay
                ? (pressed ? std::min(1.0F, configured_opacity + 0.42F)
                           : configured_opacity * 0.38F)
                : (pressed ? std::min(1.0F, configured_opacity + 0.30F)
                           : configured_opacity);
            const auto alpha = static_cast<Uint8>(std::lround(
                std::clamp(alpha_scale, 0.0F, 1.0F) * 255.0F
            ));
            const SDL_Color fill{
                button.color.r,
                button.color.g,
                button.color.b,
                static_cast<Uint8>(std::max<int>(pressed ? 80 : 24, alpha / 2))
            };
            static_cast<void>(SDL_SetRenderDrawColor(
                renderer,
                fill.r,
                fill.g,
                fill.b,
                fill.a
            ));
            static_cast<void>(SDL_RenderFillRect(renderer, &button.rectangle));

            const SDL_Color border{
                static_cast<Uint8>(std::min<int>(255, button.color.r + (pressed ? 80 : 30))),
                static_cast<Uint8>(std::min<int>(255, button.color.g + (pressed ? 80 : 30))),
                static_cast<Uint8>(std::min<int>(255, button.color.b + (pressed ? 80 : 30))),
                alpha,
            };
            static_cast<void>(SDL_SetRenderDrawColor(
                renderer,
                border.r,
                border.g,
                border.b,
                border.a
            ));
            static_cast<void>(SDL_RenderRect(renderer, &button.rectangle));

            if (settings_.show_labels && !button.label.empty()) {
                const float label_width = static_cast<float>(button.label.size()) * 8.0F;
                const float text_x = button.rectangle.x
                    + std::max(4.0F, (button.rectangle.w - label_width) * 0.5F);
                const float text_y = button.rectangle.y
                    + std::max(4.0F, (button.rectangle.h - 8.0F) * 0.5F);
                static_cast<void>(SDL_SetRenderDrawColor(
                    renderer,
                    245U,
                    250U,
                    255U,
                    static_cast<Uint8>(std::max<int>(90, alpha))
                ));
                static_cast<void>(SDL_RenderDebugText(
                    renderer,
                    text_x,
                    text_y,
                    std::string(button.label).c_str()
                ));
            }
        }

        static_cast<void>(SDL_SetRenderScale(renderer, old_scale_x, old_scale_y));
        static_cast<void>(SDL_SetRenderDrawBlendMode(renderer, old_blend));
        static_cast<void>(SDL_SetRenderDrawColor(
            renderer,
            old_r,
            old_g,
            old_b,
            old_a
        ));
    }

    void shutdown() noexcept {
        cancel_all(SDL_GetTicksNS());
        pending_.clear();
        context_ = MobileTouchContext::disabled;
        window_ = nullptr;
        renderer_ = nullptr;
        primary_pointer_.reset();
        detach_watch();
        if (platform_active_) {
            static_cast<void>(SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1"));
        }
        platform_active_ = false;
    }

    [[nodiscard]] MobileTouchContext context() const noexcept {
        return context_;
    }

    [[nodiscard]] std::uint16_t gameplay_key_count() const noexcept {
        return gameplay_key_count_;
    }

    [[nodiscard]] bool platform_active() const noexcept {
        return platform_active_;
    }

private:
    static bool SDLCALL lifecycle_watch(void* const userdata, SDL_Event* const event) {
        auto* const self = static_cast<Impl*>(userdata);
        if (self == nullptr || event == nullptr) {
            return true;
        }
        if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND
            || event->type == SDL_EVENT_DID_ENTER_BACKGROUND
            || event->type == SDL_EVENT_TERMINATING) {
            // Event watches may execute on a platform thread. Only publish an
            // atomic cancellation request here; actual maps/queues stay on the
            // SDL main thread in poll_event().
            self->lifecycle_cancel_requested_.store(true, std::memory_order_release);
        }
        return true;
    }

    void attach_watch() {
        if (!watch_registered_) {
            watch_registered_ = SDL_AddEventWatch(&Impl::lifecycle_watch, this);
        }
    }

    void detach_watch() noexcept {
        if (watch_registered_) {
            SDL_RemoveEventWatch(&Impl::lifecycle_watch, this);
            watch_registered_ = false;
        }
    }

    [[nodiscard]] static bool is_touch_event(const Uint32 type) noexcept {
        return type == SDL_EVENT_FINGER_DOWN
            || type == SDL_EVENT_FINGER_MOTION
            || type == SDL_EVENT_FINGER_UP
            || type == SDL_EVENT_FINGER_CANCELED;
    }

    [[nodiscard]] bool pop_pending(SDL_Event* const output) {
        if (pending_.empty()) {
            return false;
        }
        *output = pending_.front();
        pending_.pop_front();
        return true;
    }

    void build_key_map() {
        key_map_.fill({SDL_SCANCODE_UNKNOWN, SDL_KMOD_NONE});
        for (std::size_t lane = 0U; lane < maximum_touch_lanes; ++lane) {
            key_map_[action_index(lane_action(lane))] = {
                mobile_touch_lane_scancode(static_cast<std::uint16_t>(lane)),
                SDL_KMOD_NONE,
            };
        }
        key_map_[action_index(TouchAction::ui_left)] = action_key(
            bindings_, "ui_left", SDL_SCANCODE_LEFT
        );
        key_map_[action_index(TouchAction::ui_down)] = action_key(
            bindings_, "ui_down", SDL_SCANCODE_DOWN
        );
        key_map_[action_index(TouchAction::ui_up)] = action_key(
            bindings_, "ui_up", SDL_SCANCODE_UP
        );
        key_map_[action_index(TouchAction::ui_right)] = action_key(
            bindings_, "ui_right", SDL_SCANCODE_RIGHT
        );
        key_map_[action_index(TouchAction::accept)] = action_key(
            bindings_, "ui_accept", SDL_SCANCODE_RETURN
        );
        key_map_[action_index(TouchAction::back)] = action_key(
            bindings_, "ui_back", SDL_SCANCODE_ESCAPE
        );
        key_map_[action_index(TouchAction::pause)] = action_key(
            bindings_, "pause", SDL_SCANCODE_ESCAPE
        );
        key_map_[action_index(TouchAction::volume_down)] = action_key(
            bindings_, "volume_down", SDL_SCANCODE_MINUS
        );
        key_map_[action_index(TouchAction::volume_mute)] = action_key(
            bindings_, "volume_mute", SDL_SCANCODE_M
        );
        key_map_[action_index(TouchAction::volume_up)] = action_key(
            bindings_, "volume_up", SDL_SCANCODE_EQUALS, SDL_KMOD_SHIFT
        );
        key_map_[action_index(TouchAction::editor_play)] = action_key(
            bindings_, "editor_play_pause", SDL_SCANCODE_SPACE
        );
        key_map_[action_index(TouchAction::editor_save)] = action_key(
            bindings_, "editor_save", SDL_SCANCODE_S, SDL_KMOD_CTRL
        );
        key_map_[action_index(TouchAction::editor_undo)] = action_key(
            bindings_, "editor_undo", SDL_SCANCODE_Z, SDL_KMOD_CTRL
        );
        key_map_[action_index(TouchAction::editor_redo)] = action_key(
            bindings_, "editor_redo", SDL_SCANCODE_Y, SDL_KMOD_CTRL
        );
        key_map_[action_index(TouchAction::editor_delete)] = action_key(
            bindings_, "editor_delete", SDL_SCANCODE_DELETE
        );
    }

    [[nodiscard]] SDL_FRect safe_area() const {
        SDL_Rect safe{};
        if (renderer_ != nullptr && SDL_GetRenderSafeArea(renderer_, &safe)
            && safe.w > 1 && safe.h > 1) {
            return {
                static_cast<float>(safe.x),
                static_cast<float>(safe.y),
                static_cast<float>(safe.w),
                static_cast<float>(safe.h),
            };
        }

        int width = 0;
        int height = 0;
        SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
        if (renderer_ != nullptr) {
            static_cast<void>(SDL_GetRenderLogicalPresentation(
                renderer_,
                &width,
                &height,
                &mode
            ));
            if (width <= 0 || height <= 0) {
                static_cast<void>(SDL_GetCurrentRenderOutputSize(
                    renderer_,
                    &width,
                    &height
                ));
            }
        }
        return {
            0.0F,
            0.0F,
            static_cast<float>(std::max(width, 1)),
            static_cast<float>(std::max(height, 1)),
        };
    }

    void append_audio_buttons(std::vector<Button>& buttons, const SDL_FRect safe) const {
        const float margin = std::clamp(12.0F * settings_.scale, 8.0F, 24.0F);
        const float width = std::clamp(62.0F * settings_.scale, 48.0F, 90.0F);
        const float height = std::clamp(42.0F * settings_.scale, 36.0F, 64.0F);
        const float gap = std::max(6.0F, 8.0F * settings_.scale);
        const float y = safe.y + margin;
        float x = safe.x + margin;
        buttons.push_back({
            TouchAction::volume_down, {x, y, width, height}, {45, 156, 235, 255}, "VOL-"
        });
        x += width + gap;
        buttons.push_back({
            TouchAction::volume_mute, {x, y, width, height}, {220, 173, 44, 255}, "MUTE"
        });
        x += width + gap;
        buttons.push_back({
            TouchAction::volume_up, {x, y, width, height}, {66, 201, 135, 255}, "VOL+"
        });
    }

    [[nodiscard]] std::vector<Button> build_buttons() const {
        std::vector<Button> result;
        if (!platform_active_ || context_ == MobileTouchContext::disabled
            || renderer_ == nullptr) {
            return result;
        }
        const SDL_FRect safe = safe_area();
        if (safe.w <= 1.0F || safe.h <= 1.0F) {
            return result;
        }

        append_audio_buttons(result, safe);
        if (context_ == MobileTouchContext::gameplay) {
            const float margin = std::clamp(12.0F * settings_.scale, 8.0F, 24.0F);
            const float pause_width = std::clamp(78.0F * settings_.scale, 58.0F, 110.0F);
            const float pause_height = std::clamp(42.0F * settings_.scale, 36.0F, 64.0F);
            result.push_back({
                TouchAction::pause,
                {
                    safe.x + safe.w - pause_width - margin,
                    safe.y + margin,
                    pause_width,
                    pause_height,
                },
                {238, 188, 61, 255},
                "PAUSE",
            });
            if (!settings_.gameplay_enabled) {
                return result;
            }
            const float coverage = std::clamp(
                settings_.gameplay_coverage,
                0.35F,
                1.0F
            );
            const float height = safe.h * coverage;
            const float desired_y = safe.y + safe.h - height
                + settings_.vertical_offset * safe.h;
            const float y = std::clamp(
                desired_y,
                safe.y,
                safe.y + safe.h - height
            );
            const auto lane_count = static_cast<std::size_t>(gameplay_key_count_);
            const float lane_width = safe.w / static_cast<float>(lane_count);
            constexpr std::array<SDL_Color, 4> colors{
                SDL_Color{178, 76, 235, 255},
                SDL_Color{52, 196, 235, 255},
                SDL_Color{75, 220, 127, 255},
                SDL_Color{237, 72, 107, 255},
            };
            constexpr std::array<std::string_view, 18> labels{
                "LEFT", "DOWN", "UP", "RIGHT", "5", "6", "7", "8", "9",
                "10", "11", "12", "13", "14", "15", "16", "17", "18",
            };
            for (std::size_t lane = 0U; lane < lane_count; ++lane) {
                result.push_back({
                    lane_action(lane),
                    {
                        safe.x + lane_width * static_cast<float>(lane),
                        y,
                        lane_width,
                        height,
                    },
                    colors[lane % colors.size()],
                    labels[lane],
                });
            }
            return result;
        }

        const float scale = std::clamp(settings_.scale, 0.65F, 1.6F);
        const float button_size = std::clamp(76.0F * scale, 52.0F, 122.0F);
        const float gap = std::clamp(7.0F * scale, 5.0F, 14.0F);
        const float cluster = button_size * 3.0F + gap * 2.0F;
        const float margin = std::clamp(14.0F * scale, 8.0F, 28.0F);
        const float shifted_x = safe.x + margin + settings_.horizontal_offset * safe.w;
        const float shifted_y = safe.y + safe.h - margin - cluster
            + settings_.vertical_offset * safe.h;
        const float dpad_x = std::clamp(
            shifted_x,
            safe.x + margin,
            std::max(safe.x + margin, safe.x + safe.w - cluster - margin)
        );
        const float dpad_y = std::clamp(
            shifted_y,
            safe.y + margin + 54.0F * scale,
            std::max(
                safe.y + margin + 54.0F * scale,
                safe.y + safe.h - cluster - margin
            )
        );
        const auto cell = [&](const int column, const int row) {
            return SDL_FRect{
                dpad_x + static_cast<float>(column) * (button_size + gap),
                dpad_y + static_cast<float>(row) * (button_size + gap),
                button_size,
                button_size,
            };
        };
        result.push_back({TouchAction::ui_up, cell(1, 0), {75, 220, 127, 255}, "UP"});
        result.push_back({TouchAction::ui_left, cell(0, 1), {178, 76, 235, 255}, "LEFT"});
        result.push_back({TouchAction::ui_right, cell(2, 1), {237, 72, 107, 255}, "RIGHT"});
        result.push_back({TouchAction::ui_down, cell(1, 2), {52, 196, 235, 255}, "DOWN"});

        const float action_x = safe.x + safe.w - margin - button_size;
        const float action_y = safe.y + safe.h - margin - button_size;
        result.push_back({
            TouchAction::accept,
            {action_x, action_y, button_size, button_size},
            {75, 220, 127, 255},
            "OK",
        });
        result.push_back({
            TouchAction::back,
            {
                action_x - button_size - gap,
                action_y - button_size * 0.72F,
                button_size,
                button_size,
            },
            {237, 72, 107, 255},
            "BACK",
        });

        if (context_ == MobileTouchContext::editor) {
            const float utility_height = std::clamp(44.0F * scale, 36.0F, 62.0F);
            const float utility_width = std::clamp(74.0F * scale, 56.0F, 102.0F);
            const float utility_y = safe.y + margin + std::clamp(54.0F * scale, 48.0F, 78.0F);
            const float total_width = utility_width * 5.0F + gap * 4.0F;
            float utility_x = safe.x + safe.w - margin - total_width;
            const std::array utility{
                std::pair{TouchAction::editor_play, std::string_view{"PLAY"}},
                std::pair{TouchAction::editor_save, std::string_view{"SAVE"}},
                std::pair{TouchAction::editor_undo, std::string_view{"UNDO"}},
                std::pair{TouchAction::editor_redo, std::string_view{"REDO"}},
                std::pair{TouchAction::editor_delete, std::string_view{"DEL"}},
            };
            for (const auto& [action, label] : utility) {
                result.push_back({
                    action,
                    {utility_x, utility_y, utility_width, utility_height},
                    {94, 117, 235, 255},
                    label,
                });
                utility_x += utility_width + gap;
            }
        }
        return result;
    }

    [[nodiscard]] std::optional<TouchAction> hit_test(
        const float render_x,
        const float render_y,
        const std::optional<TouchAction> previous
    ) const {
        const auto buttons = build_buttons();
        if (buttons.empty()) {
            return std::nullopt;
        }
        const float sensitivity = std::clamp(settings_.sensitivity, 0.5F, 2.0F);

        // Gameplay lanes use boundary hysteresis instead of overlapping
        // expanded rectangles, which makes slides predictable and suppresses
        // lane chatter around a separator.
        if (context_ == MobileTouchContext::gameplay
            && settings_.gameplay_enabled) {
            for (const auto& button : buttons) {
                if (button.action == TouchAction::pause
                    || button.action == TouchAction::volume_down
                    || button.action == TouchAction::volume_mute
                    || button.action == TouchAction::volume_up) {
                    const float expansion = std::min(button.rectangle.w, button.rectangle.h)
                        * 0.08F * (sensitivity - 1.0F);
                    const SDL_FRect expanded{
                        button.rectangle.x - expansion,
                        button.rectangle.y - expansion,
                        button.rectangle.w + expansion * 2.0F,
                        button.rectangle.h + expansion * 2.0F,
                    };
                    if (contains(expanded, render_x, render_y)) {
                        return button.action;
                    }
                }
            }

            const auto first_lane = std::find_if(
                buttons.begin(),
                buttons.end(),
                [](const Button& button) {
                    return button.action == TouchAction::lane_0;
                }
            );
            if (first_lane == buttons.end()) {
                return std::nullopt;
            }
            const float extension = first_lane->rectangle.h
                * 0.08F * std::max(0.0F, sensitivity - 1.0F);
            if (render_y < first_lane->rectangle.y - extension
                || render_y > first_lane->rectangle.y + first_lane->rectangle.h + extension) {
                return std::nullopt;
            }
            const float left = first_lane->rectangle.x;
            const float width = first_lane->rectangle.w;
            const int lane = static_cast<int>(std::floor((render_x - left) / width));
            const int lane_count = static_cast<int>(gameplay_key_count_);
            if (lane < 0 || lane >= lane_count) {
                return std::nullopt;
            }
            const float local = (render_x - left) - static_cast<float>(lane) * width;
            const float deadzone = std::clamp(settings_.deadzone, 0.0F, 0.20F) * width;
            const bool near_left = lane > 0 && local < deadzone;
            const bool near_right = lane + 1 < lane_count && local > width - deadzone;
            if (near_left || near_right) {
                if (previous.has_value()) {
                    const auto previous_index = action_index(*previous);
                    if (previous_index < maximum_touch_lanes) {
                        return previous;
                    }
                }
                return std::nullopt;
            }
            return lane_action(static_cast<std::size_t>(lane));
        }

        std::optional<TouchAction> best;
        float best_distance = std::numeric_limits<float>::max();
        for (const auto& button : buttons) {
            const float expansion = std::min(button.rectangle.w, button.rectangle.h)
                * 0.10F * (sensitivity - 1.0F);
            SDL_FRect hitbox{
                button.rectangle.x - expansion,
                button.rectangle.y - expansion,
                button.rectangle.w + expansion * 2.0F,
                button.rectangle.h + expansion * 2.0F,
            };
            const float deadzone = std::min(hitbox.w, hitbox.h)
                * std::clamp(settings_.deadzone, 0.0F, 0.20F) * 0.5F;
            hitbox = inset_rect(hitbox, deadzone);
            if (!contains(hitbox, render_x, render_y)) {
                continue;
            }
            const float distance = squared_distance_to_center(hitbox, render_x, render_y);
            if (distance < best_distance) {
                best_distance = distance;
                best = button.action;
            }
        }
        return best;
    }

    void process_touch(const SDL_TouchFingerEvent& touch) {
        if (renderer_ == nullptr || window_ == nullptr) {
            return;
        }
        SDL_Event converted{};
        converted.type = touch.type;
        converted.tfinger = touch;
        if (!SDL_ConvertEventToRenderCoordinates(renderer_, &converted)) {
            return;
        }
        const float render_x = converted.tfinger.x;
        const float render_y = converted.tfinger.y;

        int window_width = 0;
        int window_height = 0;
        static_cast<void>(SDL_GetWindowSize(window_, &window_width, &window_height));
        const float window_x = touch.x * static_cast<float>(std::max(window_width, 1));
        const float window_y = touch.y * static_cast<float>(std::max(window_height, 1));

        const bool released = touch.type == SDL_EVENT_FINGER_UP
            || touch.type == SDL_EVENT_FINGER_CANCELED;
        auto iterator = fingers_.find(touch.fingerID);
        if (released) {
            if (iterator != fingers_.end()) {
                if (iterator->second.pointer) {
                    emit_mouse_button(false, touch.timestamp, touch.windowID,
                                      iterator->second.window_x,
                                      iterator->second.window_y);
                    primary_pointer_.reset();
                } else {
                    update_finger_action(iterator->second, std::nullopt,
                                         touch.timestamp, touch.windowID);
                }
                fingers_.erase(iterator);
            }
            return;
        }

        if (iterator == fingers_.end()) {
            if (touch.type != SDL_EVENT_FINGER_DOWN) {
                return;
            }
            FingerState state;
            state.window_x = window_x;
            state.window_y = window_y;
            const auto action = hit_test(render_x, render_y, std::nullopt);
            if (action.has_value()) {
                update_finger_action(state, action, touch.timestamp, touch.windowID);
            } else if ((context_ == MobileTouchContext::menu
                        || (context_ == MobileTouchContext::editor
                            && settings_.editor_direct_touch))
                       && !primary_pointer_.has_value()) {
                state.pointer = true;
                primary_pointer_ = touch.fingerID;
                emit_mouse_button(true, touch.timestamp, touch.windowID,
                                  window_x, window_y);
            }
            fingers_.emplace(touch.fingerID, state);
            return;
        }

        FingerState& state = iterator->second;
        if (state.pointer) {
            emit_mouse_motion(
                touch.timestamp,
                touch.windowID,
                window_x,
                window_y,
                window_x - state.window_x,
                window_y - state.window_y
            );
            state.window_x = window_x;
            state.window_y = window_y;
            return;
        }
        const auto action = hit_test(render_x, render_y, state.action);
        update_finger_action(state, action, touch.timestamp, touch.windowID);
        state.window_x = window_x;
        state.window_y = window_y;
    }

    void update_finger_action(
        FingerState& finger,
        const std::optional<TouchAction> next,
        const Uint64 timestamp,
        const SDL_WindowID window_id
    ) {
        if (finger.action == next) {
            return;
        }
        if (finger.action.has_value()) {
            release_action(*finger.action, timestamp, window_id);
        }
        finger.action = next;
        if (finger.action.has_value()) {
            press_action(*finger.action, timestamp, window_id);
        }
    }

    void press_action(
        const TouchAction action,
        const Uint64 timestamp,
        const SDL_WindowID window_id
    ) {
        auto& refs = action_refs_[action_index(action)];
        if (refs == 0U) {
            emit_key(action, true, timestamp, window_id);
        }
        if (refs < std::numeric_limits<std::uint16_t>::max()) {
            ++refs;
        }
    }

    void release_action(
        const TouchAction action,
        const Uint64 timestamp,
        const SDL_WindowID window_id
    ) {
        auto& refs = action_refs_[action_index(action)];
        if (refs == 0U) {
            return;
        }
        --refs;
        if (refs == 0U) {
            emit_key(action, false, timestamp, window_id);
        }
    }

    void emit_key(
        const TouchAction action,
        const bool down,
        const Uint64 timestamp,
        const SDL_WindowID window_id
    ) {
        const auto spec = key_map_[action_index(action)];
        if (spec.scancode == SDL_SCANCODE_UNKNOWN) {
            return;
        }
        SDL_Event event{};
        const auto type = static_cast<SDL_EventType>(
            down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP
        );
        event.type = type;
        event.key.type = type;
        event.key.timestamp = timestamp == 0U ? SDL_GetTicksNS() : timestamp;
        event.key.windowID = window_id != 0U
            ? window_id
            : (window_ == nullptr ? 0U : SDL_GetWindowID(window_));
        event.key.which = synthetic_touch_keyboard_id;
        event.key.scancode = spec.scancode;
        event.key.key = SDL_GetKeyFromScancode(spec.scancode, spec.modifiers, false);
        event.key.mod = spec.modifiers;
        event.key.down = down;
        event.key.repeat = false;
        pending_.push_back(event);
    }

    void emit_mouse_button(
        const bool down,
        const Uint64 timestamp,
        const SDL_WindowID window_id,
        const float x,
        const float y
    ) {
        SDL_Event event{};
        const auto type = static_cast<SDL_EventType>(
            down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP
        );
        event.type = type;
        event.button.type = type;
        event.button.timestamp = timestamp == 0U ? SDL_GetTicksNS() : timestamp;
        event.button.windowID = window_id;
        event.button.which = SDL_TOUCH_MOUSEID;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = down;
        event.button.clicks = 1U;
        event.button.x = x;
        event.button.y = y;
        pending_.push_back(event);
    }

    void emit_mouse_motion(
        const Uint64 timestamp,
        const SDL_WindowID window_id,
        const float x,
        const float y,
        const float relative_x,
        const float relative_y
    ) {
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.timestamp = timestamp == 0U ? SDL_GetTicksNS() : timestamp;
        event.motion.windowID = window_id;
        event.motion.which = SDL_TOUCH_MOUSEID;
        event.motion.state = SDL_BUTTON_LMASK;
        event.motion.x = x;
        event.motion.y = y;
        event.motion.xrel = relative_x;
        event.motion.yrel = relative_y;
        pending_.push_back(event);
    }

    void cancel_all(const Uint64 timestamp) noexcept {
        const SDL_WindowID window_id = window_ == nullptr ? 0U : SDL_GetWindowID(window_);
        for (std::size_t index = 0U; index < action_refs_.size(); ++index) {
            if (action_refs_[index] == 0U) {
                continue;
            }
            action_refs_[index] = 0U;
            emit_key(static_cast<TouchAction>(index), false, timestamp, window_id);
        }
        if (primary_pointer_.has_value()) {
            const auto pointer = fingers_.find(*primary_pointer_);
            if (pointer != fingers_.end()) {
                emit_mouse_button(false, timestamp, window_id,
                                  pointer->second.window_x,
                                  pointer->second.window_y);
            }
        }
        primary_pointer_.reset();
        fingers_.clear();
    }

    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    TouchSettings settings_{};
    InputBindings bindings_{};
    MobileTouchContext context_{MobileTouchContext::disabled};
    std::uint16_t gameplay_key_count_{4U};
    std::array<KeySpec, action_count> key_map_{};
    std::array<std::uint16_t, action_count> action_refs_{};
    std::unordered_map<SDL_FingerID, FingerState> fingers_;
    std::optional<SDL_FingerID> primary_pointer_;
    std::deque<SDL_Event> pending_;
    std::atomic<bool> lifecycle_cancel_requested_{false};
    bool platform_active_{};
    bool watch_registered_{};

public:
    void inject_finger_for_testing(const SDL_TouchFingerEvent& event) {
        if (platform_active_ && context_ != MobileTouchContext::disabled) {
            process_touch(event);
        }
    }
};

MobileTouchControls::Impl& MobileTouchControls::impl() {
    if (impl_ == nullptr) {
        // Process lifetime is intentional. The SDL watch is explicitly removed
        // by shutdown(), so static destruction never calls into a torn-down SDL.
        impl_ = new Impl();
    }
    return *impl_;
}

void MobileTouchControls::configure(
    SDL_Window* const window,
    SDL_Renderer* const renderer,
    const TouchSettings& settings,
    const InputBindings& bindings
) {
    impl().configure(window, renderer, settings, bindings);
}

void MobileTouchControls::set_context(
    const MobileTouchContext context,
    const std::uint16_t gameplay_key_count
) {
    impl().set_context(context, gameplay_key_count);
}

MobileTouchContext MobileTouchControls::context() const noexcept {
    return impl_ == nullptr ? MobileTouchContext::disabled : impl_->context();
}

std::uint16_t MobileTouchControls::gameplay_key_count() const noexcept {
    return impl_ == nullptr ? 4U : impl_->gameplay_key_count();
}

bool MobileTouchControls::platform_active() const noexcept {
    return impl_ != nullptr && impl_->platform_active();
}

bool MobileTouchControls::poll_event(SDL_Event* const event) {
    return impl().poll_event(event);
}

void MobileTouchControls::inject_finger_for_testing(
    const SDL_TouchFingerEvent& event
) {
    impl().inject_finger_for_testing(event);
}

void MobileTouchControls::render(SDL_Renderer* const renderer) {
    impl().render(renderer);
}

void MobileTouchControls::shutdown() noexcept {
    if (impl_ != nullptr) {
        // The event watch has been removed by Impl::shutdown before deletion,
        // so there is neither a dangling SDL callback nor a process-lifetime
        // allocation reported as a leak.
        Impl* const dying = impl_;
        impl_ = nullptr;
        dying->shutdown();
        delete dying;
    }
}

MobileTouchControls& mobile_touch_controls() {
    static MobileTouchControls controls;
    return controls;
}

bool poll_mobile_event(SDL_Event* const event) {
    return mobile_touch_controls().poll_event(event);
}

bool present_with_mobile_touch(SDL_Renderer* const renderer) {
    mobile_touch_controls().render(renderer);
    return SDL_RenderPresent(renderer);
}

SDL_Scancode mobile_touch_lane_scancode(const std::uint16_t lane) noexcept {
    constexpr std::array<SDL_Scancode, maximum_touch_lanes> reserved{
        SDL_SCANCODE_F13, SDL_SCANCODE_F14, SDL_SCANCODE_F15,
        SDL_SCANCODE_F16, SDL_SCANCODE_F17, SDL_SCANCODE_F18,
        SDL_SCANCODE_F19, SDL_SCANCODE_F20, SDL_SCANCODE_F21,
        SDL_SCANCODE_F22, SDL_SCANCODE_F23, SDL_SCANCODE_F24,
        SDL_SCANCODE_INTERNATIONAL1, SDL_SCANCODE_INTERNATIONAL2,
        SDL_SCANCODE_INTERNATIONAL3, SDL_SCANCODE_INTERNATIONAL4,
        SDL_SCANCODE_INTERNATIONAL5, SDL_SCANCODE_INTERNATIONAL6,
    };
    return lane < reserved.size() ? reserved[lane] : SDL_SCANCODE_UNKNOWN;
}

std::optional<std::uint16_t> mobile_touch_lane_from_event(
    const SDL_KeyboardEvent& event
) noexcept {
    if (event.which != synthetic_touch_keyboard_id) {
        return std::nullopt;
    }
    for (std::uint16_t lane = 0U; lane < maximum_touch_lanes; ++lane) {
        if (event.scancode == mobile_touch_lane_scancode(lane)) {
            return lane;
        }
    }
    return std::nullopt;
}

ScopedMobileTouchContext::ScopedMobileTouchContext(
    const MobileTouchContext context,
    const std::uint16_t gameplay_key_count
) : previous_(mobile_touch_controls().context()),
    previous_key_count_(mobile_touch_controls().gameplay_key_count()) {
    mobile_touch_controls().set_context(context, gameplay_key_count);
}

ScopedMobileTouchContext::~ScopedMobileTouchContext() {
    mobile_touch_controls().set_context(previous_, previous_key_count_);
}

}  // namespace pulseforge::detail
