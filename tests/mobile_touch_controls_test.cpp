#include "mobile_touch_controls.hpp"

#include "pulseforge/input_bindings.hpp"
#include "pulseforge/settings.hpp"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::string_view current_phase{"initialization"};
std::size_t routed_ordinal{};

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void force_touch(const bool enabled) {
    require(
        SDL_SetEnvironmentVariable(
            SDL_GetEnvironment(),
            "PULSEFORGE_FORCE_TOUCH_CONTROLS",
            enabled ? "1" : "0",
            true
        ),
        "set touch test hook"
    );
}

void drain_raw_events() {
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
    }
}

SDL_Event finger_event(
    const SDL_EventType type,
    const SDL_WindowID window_id,
    const SDL_FingerID finger_id,
    const float normalized_x,
    const float normalized_y = 0.5F
) {
    SDL_Event event{};
    event.type = type;
    event.tfinger.type = type;
    event.tfinger.timestamp = SDL_GetTicksNS();
    event.tfinger.windowID = window_id;
    event.tfinger.touchID = 1U;
    event.tfinger.fingerID = finger_id;
    event.tfinger.x = normalized_x;
    event.tfinger.y = normalized_y;
    event.tfinger.pressure = type == SDL_EVENT_FINGER_UP
            || type == SDL_EVENT_FINGER_CANCELED
        ? 0.0F
        : 1.0F;
    return event;
}

SDL_Event routed_event() {
    ++routed_ordinal;
    SDL_Event event{};
    if (!pulseforge::detail::poll_mobile_event(&event)) {
        throw std::runtime_error(
            "router produced no event #" + std::to_string(routed_ordinal)
                + " during " + std::string(current_phase)
                + ": " + SDL_GetError()
        );
    }
    return event;
}

void require_no_routed_event(const std::string_view message) {
    SDL_Event event{};
    require(!pulseforge::detail::poll_mobile_event(&event), message);
}

void push(SDL_Event event) {
    require(SDL_PushEvent(&event), "push SDL test event");
}

void inject_touch(const SDL_Event& event) {
    pulseforge::detail::mobile_touch_controls().inject_finger_for_testing(
        event.tfinger
    );
}

void require_lane_key(
    const SDL_Event& event,
    const bool down,
    const std::uint16_t lane
) {
    require(
        event.type == static_cast<Uint32>(
            down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP
        ),
        down ? "lane produces key down" : "lane produces key up"
    );
    const auto routed_lane =
        pulseforge::detail::mobile_touch_lane_from_event(event.key);
    require(routed_lane.has_value() && *routed_lane == lane, "touch lane identity");
}

}  // namespace

int main() {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    try {
        static_cast<void>(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
        require(
            SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS),
            std::string{"SDL init: "} + SDL_GetError()
        );
        require(
            SDL_CreateWindowAndRenderer(
                "PulseForge touch router test",
                1'280,
                720,
                0U,
                &window,
                &renderer
            ),
            std::string{"dummy renderer: "} + SDL_GetError()
        );
        require(
            SDL_SetRenderLogicalPresentation(
                renderer,
                1'280,
                720,
                SDL_LOGICAL_PRESENTATION_LETTERBOX
            ),
            "logical presentation"
        );
        drain_raw_events();

        force_touch(true);
        pulseforge::TouchSettings settings;
        settings.gameplay_coverage = 0.80F;
        auto& router = pulseforge::detail::mobile_touch_controls();
        router.configure(
            window,
            renderer,
            settings,
            pulseforge::default_input_bindings()
        );
        router.set_context(pulseforge::detail::MobileTouchContext::gameplay, 18U);
        require(router.platform_active(), "forced touch router active");
        require(router.gameplay_key_count() == 18U, "18K context retained");
        require(
            router.context() == pulseforge::detail::MobileTouchContext::gameplay,
            "gameplay context retained"
        );

        const auto window_id = SDL_GetWindowID(window);
        const auto lane_x = [](const std::uint16_t lane) {
            return (static_cast<float>(lane) + 0.5F) / 18.0F;
        };
        SDL_Event coordinate_probe = finger_event(
            SDL_EVENT_FINGER_DOWN,
            window_id,
            99U,
            lane_x(0U)
        );
        require(
            SDL_ConvertEventToRenderCoordinates(renderer, &coordinate_probe),
            std::string{"touch coordinate conversion: "} + SDL_GetError()
        );
        require(
            coordinate_probe.tfinger.x > 1.0F
                && coordinate_probe.tfinger.y > 100.0F,
            "touch coordinates become logical render coordinates"
        );
        // Two independent fingers form a chord across the entire 18K range.
        current_phase = "18K chord";
        inject_touch(finger_event(SDL_EVENT_FINGER_DOWN, window_id, 10U, lane_x(0U)));
        require_lane_key(routed_event(), true, 0U);
        inject_touch(finger_event(SDL_EVENT_FINGER_DOWN, window_id, 11U, lane_x(17U)));
        require_lane_key(routed_event(), true, 17U);
        inject_touch(finger_event(SDL_EVENT_FINGER_UP, window_id, 10U, lane_x(0U)));
        require_lane_key(routed_event(), false, 0U);
        inject_touch(finger_event(SDL_EVENT_FINGER_UP, window_id, 11U, lane_x(17U)));
        require_lane_key(routed_event(), false, 17U);

        // Two fingers sharing one lane must not release it until the final up.
        current_phase = "same-lane refcount";
        inject_touch(finger_event(SDL_EVENT_FINGER_DOWN, window_id, 20U, lane_x(4U)));
        require_lane_key(routed_event(), true, 4U);
        inject_touch(finger_event(SDL_EVENT_FINGER_DOWN, window_id, 21U, lane_x(4U)));
        require_no_routed_event("second finger does not duplicate key down");
        inject_touch(finger_event(SDL_EVENT_FINGER_UP, window_id, 20U, lane_x(4U)));
        require_no_routed_event("first finger does not release shared lane");
        inject_touch(finger_event(SDL_EVENT_FINGER_UP, window_id, 21U, lane_x(4U)));
        require_lane_key(routed_event(), false, 4U);

        // Sliding is ordered: release old lane, then press new lane.
        current_phase = "lane slide";
        inject_touch(finger_event(SDL_EVENT_FINGER_DOWN, window_id, 30U, lane_x(6U)));
        require_lane_key(routed_event(), true, 6U);
        inject_touch(finger_event(SDL_EVENT_FINGER_MOTION, window_id, 30U, lane_x(7U)));
        require_lane_key(routed_event(), false, 6U);
        require_lane_key(routed_event(), true, 7U);
        inject_touch(finger_event(SDL_EVENT_FINGER_CANCELED, window_id, 30U, lane_x(7U)));
        require_lane_key(routed_event(), false, 7U);

        // Focus loss releases held state before forwarding the lifecycle event.
        current_phase = "focus loss";
        inject_touch(finger_event(SDL_EVENT_FINGER_DOWN, window_id, 40U, lane_x(2U)));
        require_lane_key(routed_event(), true, 2U);
        SDL_Event focus_lost{};
        focus_lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        focus_lost.window.windowID = window_id;
        push(focus_lost);
        require_lane_key(routed_event(), false, 2U);
        require(
            routed_event().type == SDL_EVENT_WINDOW_FOCUS_LOST,
            "focus event forwarded after releases"
        );
        require_no_routed_event("focus cancellation leaves no stuck event");

        // Live settings/binding reconfiguration releases held virtual input
        // using the old map before installing the new one. This prevents a
        // settings edit or launcher/gameplay handoff from leaving a stuck lane.
        current_phase = "live reconfiguration";
        inject_touch(finger_event(SDL_EVENT_FINGER_DOWN, window_id, 45U, lane_x(5U)));
        require_lane_key(routed_event(), true, 5U);
        auto reconfigured = settings;
        reconfigured.opacity = 0.70F;
        router.configure(
            window,
            renderer,
            reconfigured,
            pulseforge::default_input_bindings()
        );
        require_lane_key(routed_event(), false, 5U);
        require_no_routed_event("reconfiguration leaves no stuck lane");
        router.set_context(pulseforge::detail::MobileTouchContext::gameplay, 18U);

        current_phase = "desktop pass-through";
        router.shutdown();
        force_touch(false);
        router.configure(
            window,
            renderer,
            settings,
            pulseforge::default_input_bindings()
        );
        router.set_context(pulseforge::detail::MobileTouchContext::gameplay, 18U);
        require(!router.platform_active(), "desktop path remains inactive");
        push(finger_event(SDL_EVENT_FINGER_DOWN, window_id, 50U, lane_x(0U)));
        require(
            routed_event().type == SDL_EVENT_FINGER_DOWN,
            "desktop path passes touch through without synthesis"
        );

        router.shutdown();
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
        SDL_DestroyWindow(window);
        window = nullptr;
        SDL_Quit();
        std::cout << "PulseForge mobile touch control tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        pulseforge::detail::mobile_touch_controls().shutdown();
        if (renderer != nullptr) SDL_DestroyRenderer(renderer);
        if (window != nullptr) SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }
}
