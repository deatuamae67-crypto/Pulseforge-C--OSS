#pragma once

#include "pulseforge/settings.hpp"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace pulseforge::detail {

// Executes the small set of post effects that SDL_Renderer can implement
// portably. It does not parse or execute arbitrary GLSL; ShaderCatalog remains
// discovery/validation metadata until the planned SDL_GPU backend lands.
class RuntimePostEffects final {
public:
    RuntimePostEffects() = default;
    ~RuntimePostEffects();

    RuntimePostEffects(const RuntimePostEffects&) = delete;
    RuntimePostEffects& operator=(const RuntimePostEffects&) = delete;
    RuntimePostEffects(RuntimePostEffects&&) = delete;
    RuntimePostEffects& operator=(RuntimePostEffects&&) = delete;

    // Must be called after the frame destination (window backbuffer or exact
    // offline target) is selected and before any scene draw calls.
    [[nodiscard]] bool begin_frame(
        SDL_Renderer* renderer,
        PostEffect effect,
        bool maximum_performance_bypass,
        float logical_width,
        float logical_height,
        std::string* warning = nullptr
    );

    // Composites the staged RGB frame or draws the scanline overlay. On
    // success the original destination is selected again, ready for readback
    // or SDL_RenderPresent(). A false result means target restoration failed.
    [[nodiscard]] bool finish_frame(std::string* error = nullptr);

    // Releases every target texture while its owning SDL_Renderer is alive.
    void reset() noexcept;

private:
    [[nodiscard]] bool prepare_rgb_target(std::string* warning);
    [[nodiscard]] bool composite_rgb_split(std::string* error);
    void draw_scanlines(bool rgb_fallback) noexcept;
    void build_scanline_geometry(float logical_width, float logical_height);
    void assign_warning(std::string* warning, const std::string& message) const;

    SDL_Renderer* renderer_{};
    SDL_Texture* destination_{};
    SDL_Texture* rgb_target_{};
    int rgb_target_width_{};
    int rgb_target_height_{};
    float logical_width_{};
    float logical_height_{};
    PostEffect effect_{PostEffect::off};
    bool frame_active_{};
    bool rgb_staged_{};
    bool rgb_fallback_{};
    std::vector<SDL_Vertex> scanline_vertices_;
    std::vector<int> scanline_indices_;
    float scanline_geometry_width_{};
    float scanline_geometry_height_{};
};

}  // namespace pulseforge::detail
