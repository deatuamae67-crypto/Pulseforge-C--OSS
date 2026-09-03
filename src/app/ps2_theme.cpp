#include "ps2_theme.hpp"

#include "intro_player.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge::detail {
namespace {

void fill_rect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    static_cast<void>(SDL_RenderFillRect(renderer, &rect));
}

void outline_rect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    static_cast<void>(SDL_RenderRect(renderer, &rect));
}

void text(SDL_Renderer* renderer, float x, float y, std::string_view value,
          SDL_Color color, float scale = 1.0F) {
    std::string printable;
    printable.reserve(value.size());
    for (const unsigned char c : value) {
        printable.push_back(c >= 32U && c <= 126U ? static_cast<char>(c) : '?');
    }
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    float old_x = 1.0F;
    float old_y = 1.0F;
    static_cast<void>(SDL_GetRenderScale(renderer, &old_x, &old_y));
    static_cast<void>(SDL_SetRenderScale(renderer, scale, scale));
    static_cast<void>(SDL_RenderDebugText(renderer, x / scale, y / scale, printable.c_str()));
    static_cast<void>(SDL_SetRenderScale(renderer, old_x, old_y));
}

void centered_text(SDL_Renderer* renderer, float center_x, float y,
                   std::string_view value, SDL_Color color, float scale = 1.0F) {
    const float width = static_cast<float>(value.size()) * 8.0F * scale;
    text(renderer, center_x - width * 0.5F, y, value, color, scale);
}

std::vector<std::string> wrap_ascii(std::string_view value, std::size_t width) {
    std::vector<std::string> lines;
    std::string current;
    std::string word;
    auto flush_word = [&]() {
        if (word.empty()) return;
        if (!current.empty() && current.size() + 1U + word.size() > width) {
            lines.push_back(current);
            current.clear();
        }
        if (!current.empty()) current.push_back(' ');
        if (word.size() <= width) {
            current += word;
        } else {
            std::size_t offset = 0U;
            while (offset < word.size()) {
                const auto part = word.substr(offset, width);
                if (!current.empty()) { lines.push_back(current); current.clear(); }
                lines.push_back(part);
                offset += part.size();
            }
        }
        word.clear();
    };
    for (const unsigned char raw : value) {
        const char c = raw >= 32U && raw <= 126U ? static_cast<char>(raw) : ' ';
        if (c == '\n') {
            flush_word();
            if (!current.empty()) lines.push_back(current);
            current.clear();
        } else if (c == ' ' || c == '\t') {
            flush_word();
        } else {
            word.push_back(c);
        }
    }
    flush_word();
    if (!current.empty()) lines.push_back(current);
    if (lines.empty()) lines.emplace_back("Unknown chart/runtime error");
    if (lines.size() > 7U) {
        lines.resize(7U);
        if (lines.back().size() > width - 3U) lines.back().resize(width - 3U);
        lines.back() += "...";
    }
    return lines;
}

struct ErrorOverlayData {
    std::string title;
    std::string detail;
    std::string hint;
};

void draw_ps2_error_overlay(SDL_Renderer* renderer, std::uint64_t elapsed_ms,
                            void* userdata) {
    if (renderer == nullptr || userdata == nullptr) return;
    const auto& data = *static_cast<ErrorOverlayData*>(userdata);

    fill_rect(renderer, {245.0F, 372.0F, 790.0F, 284.0F}, {5, 0, 2, 158});
    outline_rect(renderer, {245.0F, 372.0F, 790.0F, 284.0F}, {196, 64, 64, 150});
    centered_text(renderer, 640.0F, 404.0F, "PULSEFORGE", {218,218,218,255}, 1.72F);
    centered_text(renderer, 640.0F, 444.0F, data.title, {242,232,232,255}, 1.10F);

    const auto lines = wrap_ascii(data.detail, 74U);
    float y = 488.0F;
    for (const auto& line : lines) {
        centered_text(renderer, 640.0F, y, line, {231,219,219,255}, 0.88F);
        y += 22.0F;
    }
    if (((elapsed_ms / 650U) & 1U) == 0U) {
        centered_text(renderer, 640.0F, 622.0F, data.hint, {204,177,177,235}, 0.72F);
    }
}

bool acknowledge_event(const SDL_Event& event) noexcept {
    return event.type == SDL_EVENT_QUIT
        || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
        || event.type == SDL_EVENT_FINGER_UP
        || (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat
            && (event.key.scancode == SDL_SCANCODE_RETURN
                || event.key.scancode == SDL_SCANCODE_KP_ENTER
                || event.key.scancode == SDL_SCANCODE_SPACE
                || event.key.scancode == SDL_SCANCODE_ESCAPE))
        || (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN
            && event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH);
}

void draw_procedural_fallback(SDL_Renderer* renderer, std::string_view title,
                              std::string_view detail, std::string_view hint,
                              std::uint64_t elapsed_ms) {
    SDL_SetRenderDrawColor(renderer, 18, 0, 3, 255);
    SDL_RenderClear(renderer);
    const float cx = 640.0F;
    const float cy = 316.0F;
    for (int layer = 0; layer < 18; ++layer) {
        const float phase = std::fmod(
            static_cast<float>(elapsed_ms) * 0.00024F
                + static_cast<float>(layer) / 18.0F,
            1.0F
        );
        const float size = 42.0F + phase * 1'020.0F;
        const SDL_FRect rect{cx-size*0.5F, cy-size*0.28F, size, size*0.56F};
        outline_rect(renderer, rect, {226,18,24,static_cast<std::uint8_t>(
            std::clamp(190.0F*(1.0F-phase),14.0F,190.0F))});
    }
    ErrorOverlayData overlay{std::string(title),std::string(detail),std::string(hint)};
    draw_ps2_error_overlay(renderer,elapsed_ms,&overlay);
    SDL_RenderPresent(renderer);
}

}  // namespace

void draw_ps2_startup_overlay(SDL_Renderer* renderer, std::uint64_t elapsed_ms, void*) {
    if (renderer == nullptr) return;
    if (elapsed_ms >= 450U && elapsed_ms <= 3'450U) {
        fill_rect(renderer,{330.0F,252.0F,620.0F,142.0F},{0,0,7,255});
        centered_text(renderer,640.0F,286.0F,"PULSEFORGE",{232,236,255,255},1.82F);
        centered_text(renderer,640.0F,333.0F,"RHYTHM ENGINE",{128,145,205,230},0.73F);
    }
    if (elapsed_ms >= 23'000U && elapsed_ms <= 27'900U) {
        const float fade_in=std::clamp(static_cast<float>(elapsed_ms-23'000U)/1'100.0F,0.0F,1.0F);
        const float fade_out=std::clamp(static_cast<float>(27'900U-elapsed_ms)/900.0F,0.0F,1.0F);
        const float alpha=std::min(fade_in,fade_out);
        centered_text(renderer,640.0F,452.0F,"PULSEFORGE",
            {232,236,255,static_cast<std::uint8_t>(255.0F*alpha)},2.15F);
        centered_text(renderer,640.0F,493.0F,"RHYTHM ENGINE",
            {118,137,199,static_cast<std::uint8_t>(230.0F*alpha)},0.72F);
    }
    if (((elapsed_ms/900U)&1U)==0U) {
        centered_text(renderer,640.0F,675.0F,"PRESS ENTER OR SPACE TO SKIP",
            {98,118,180,165},0.68F);
    }
}

void show_ps2_error_screen(SDL_Window* window, SDL_Renderer* renderer,
                           std::string_view title, std::string_view detail,
                           std::string_view hint,
                           const std::filesystem::path& background_movie,
                           const AudioSettings& audio_settings) {
    if (window == nullptr || renderer == nullptr) return;
    if (!background_movie.empty()) {
        ErrorOverlayData overlay{std::string(title),std::string(detail),std::string(hint)};
        StartupIntroPlaybackOptions playback;
        playback.allow_decoded_derivative=true;
        playback.allow_native_movie=false;
        playback.allow_procedural_fallback=false;
        playback.loop_until_skip=true;
        playback.show_skip_hint=false;
        playback.overlay=&draw_ps2_error_overlay;
        playback.overlay_userdata=&overlay;
        const auto result=play_startup_intro_ex(window,renderer,background_movie,audio_settings,playback);
        if (result.status != StartupIntroStatus::fallback_played) return;
    }
    const auto started=SDL_GetTicks();
    bool waiting=true;
    while (waiting) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (acknowledge_event(event)) { waiting=false; break; }
        }
        draw_procedural_fallback(renderer,title,detail,hint,SDL_GetTicks()-started);
        if (waiting) SDL_Delay(8U);
    }
}

void show_ps2_error_screen_standalone(std::string_view title, std::string_view detail,
                                      const std::filesystem::path& background_movie,
                                      const AudioSettings& audio_settings) {
    const bool already_video=(SDL_WasInit(SDL_INIT_VIDEO)&SDL_INIT_VIDEO)!=0U;
    if (!already_video && !SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS|SDL_INIT_GAMEPAD)) return;
    SDL_Window* window=SDL_CreateWindow("PulseForge - System Data Error",1280,720,
        SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) { if (!already_video) SDL_Quit(); return; }
    SDL_Renderer* renderer=SDL_CreateRenderer(window,nullptr);
    if (renderer != nullptr) {
        show_ps2_error_screen(window,renderer,title,detail,
            "PRESS ENTER / SPACE / ESC TO CLOSE THIS ERROR",background_movie,audio_settings);
        SDL_DestroyRenderer(renderer);
    }
    SDL_DestroyWindow(window);
    if (!already_video) SDL_Quit();
}

}  // namespace pulseforge::detail
