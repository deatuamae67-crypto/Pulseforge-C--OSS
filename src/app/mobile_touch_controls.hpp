#pragma once

#include "pulseforge/settings.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>

namespace pulseforge::detail {

// The touch router is deliberately app-internal: charts, gameplay sessions and
// mods continue to receive the same keyboard actions as desktop builds. This
// keeps the timing/judgement path identical while SDL touch events provide the
// physical source on Android.
enum class MobileTouchContext : std::uint8_t {
    disabled,
    menu,
    gameplay,
    editor,
};

class MobileTouchControls final {
public:
    void configure(
        SDL_Window* window,
        SDL_Renderer* renderer,
        const TouchSettings& settings,
        const InputBindings& bindings
    );
    void set_context(
        MobileTouchContext context,
        std::uint16_t gameplay_key_count = 4U
    );
    [[nodiscard]] MobileTouchContext context() const noexcept;
    [[nodiscard]] std::uint16_t gameplay_key_count() const noexcept;
    [[nodiscard]] bool platform_active() const noexcept;

    // Drop-in replacement for SDL_PollEvent in interactive loops. Raw touch
    // events are consumed and emitted as ordinary keyboard/mouse events.
    [[nodiscard]] bool poll_event(SDL_Event* event);

    // Deterministic app-internal test seam. It is inert unless the Android
    // router (or its explicit force hook) is active and never enters a
    // production call path.
    void inject_finger_for_testing(const SDL_TouchFingerEvent& event);

    // Draws the active virtual controls after the scene/UI and immediately
    // before SDL_RenderPresent. It is a no-op outside Android (unless the
    // explicit PULSEFORGE_FORCE_TOUCH_CONTROLS test hook is enabled).
    void render(SDL_Renderer* renderer);

    // Releases every synthetic action, removes lifecycle hooks and restores
    // SDL's normal touch-to-mouse policy before SDL_Quit.
    void shutdown() noexcept;

private:
    class Impl;
    Impl* impl_{};

    [[nodiscard]] Impl& impl();
};

[[nodiscard]] MobileTouchControls& mobile_touch_controls();
[[nodiscard]] bool poll_mobile_event(SDL_Event* event);
bool present_with_mobile_touch(SDL_Renderer* renderer);
// Reserved virtual scancodes used only as the internal bridge between touch
// zones and the existing zero-allocation gameplay input path.
[[nodiscard]] SDL_Scancode mobile_touch_lane_scancode(
    std::uint16_t lane
) noexcept;
// Identifies the private synthetic lane event without reserving or shadowing a
// physical keyboard binding that happens to use F13-F24/International keys.
[[nodiscard]] std::optional<std::uint16_t> mobile_touch_lane_from_event(
    const SDL_KeyboardEvent& event
) noexcept;

class ScopedMobileTouchContext final {
public:
    ScopedMobileTouchContext(
        MobileTouchContext context,
        std::uint16_t gameplay_key_count = 4U
    );
    ~ScopedMobileTouchContext();

    ScopedMobileTouchContext(const ScopedMobileTouchContext&) = delete;
    ScopedMobileTouchContext& operator=(const ScopedMobileTouchContext&) = delete;

private:
    MobileTouchContext previous_{MobileTouchContext::disabled};
    std::uint16_t previous_key_count_{4U};
};

}  // namespace pulseforge::detail
