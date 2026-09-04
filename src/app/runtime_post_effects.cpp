#include "runtime_post_effects.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace pulseforge::detail {
namespace {

[[nodiscard]] int checked_texture_extent(const float extent) noexcept {
    if (!std::isfinite(extent) || extent < 1.0F
        || extent > static_cast<float>(std::numeric_limits<int>::max())) {
        return 0;
    }
    return static_cast<int>(std::lround(extent));
}

}  // namespace

RuntimePostEffects::~RuntimePostEffects() {
    reset();
}

void RuntimePostEffects::assign_warning(
    std::string* const warning,
    const std::string& message
) const {
    if (warning != nullptr) {
        *warning = message;
    }
}

bool RuntimePostEffects::begin_frame(
    SDL_Renderer* const renderer,
    const PostEffect effect,
    const bool maximum_performance_bypass,
    const float logical_width,
    const float logical_height,
    std::string* const warning
) {
    if (warning != nullptr) {
        warning->clear();
    }
    // A previous incomplete frame is restored defensively. In normal use
    // finish_frame() has already cleared this state.
    if (frame_active_ && !finish_frame(warning)) {
        // Do not overwrite destination_/rgb_target_ while the previous staging
        // texture may still be selected.  The application will tear down the
        // renderer instead of attempting another frame on indeterminate state.
        return false;
    }

    renderer_ = renderer;
    destination_ = renderer != nullptr ? SDL_GetRenderTarget(renderer) : nullptr;
    logical_width_ = logical_width;
    logical_height_ = logical_height;
    effect_ = maximum_performance_bypass ? PostEffect::off : effect;
    frame_active_ = renderer != nullptr && effect_ != PostEffect::off;
    rgb_staged_ = false;
    rgb_fallback_ = false;

    if (!frame_active_ || effect_ != PostEffect::rgb_split) {
        return true;
    }
    if (!prepare_rgb_target(warning)) {
        if (SDL_GetRenderTarget(renderer_) != destination_) {
            assign_warning(
                warning,
                "Post-effect staging could not restore the frame destination: "
                    + std::string(SDL_GetError())
            );
            frame_active_ = false;
            return false;
        }
        // Draw the frame directly to its original destination and apply a
        // deterministic lightweight overlay in finish_frame().
        rgb_fallback_ = true;
    }
    return true;
}

bool RuntimePostEffects::prepare_rgb_target(std::string* const warning) {
    int width{};
    int height{};
    if (destination_ != nullptr) {
        float texture_width{};
        float texture_height{};
        if (!SDL_GetTextureSize(
                destination_,
                &texture_width,
                &texture_height
            )) {
            assign_warning(
                warning,
                "RGB Split fell back to an overlay because the destination size is unavailable: "
                    + std::string(SDL_GetError())
            );
            return false;
        }
        width = checked_texture_extent(texture_width);
        height = checked_texture_extent(texture_height);
    } else if (!SDL_GetRenderOutputSize(renderer_, &width, &height)) {
        assign_warning(
            warning,
            "RGB Split fell back to an overlay because the render output size is unavailable: "
                + std::string(SDL_GetError())
        );
        return false;
    }
    if (width <= 0 || height <= 0) {
        assign_warning(
            warning,
            "RGB Split fell back to an overlay because the render output has invalid dimensions"
        );
        return false;
    }

    if (rgb_target_ == nullptr || width != rgb_target_width_
        || height != rgb_target_height_) {
        if (rgb_target_ != nullptr) {
            SDL_DestroyTexture(rgb_target_);
            rgb_target_ = nullptr;
        }
        rgb_target_ = SDL_CreateTexture(
            renderer_,
            SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_TARGET,
            width,
            height
        );
        if (rgb_target_ == nullptr) {
            assign_warning(
                warning,
                "RGB Split fell back to an overlay because this renderer cannot create a target texture: "
                    + std::string(SDL_GetError())
            );
            rgb_target_width_ = 0;
            rgb_target_height_ = 0;
            return false;
        }
        rgb_target_width_ = width;
        rgb_target_height_ = height;
        static_cast<void>(SDL_SetTextureScaleMode(
            rgb_target_,
            SDL_SCALEMODE_LINEAR
        ));
    }

    if (!SDL_SetRenderTarget(renderer_, rgb_target_)) {
        assign_warning(
            warning,
            "RGB Split fell back to an overlay because this renderer rejected the staging target: "
                + std::string(SDL_GetError())
        );
        static_cast<void>(SDL_SetRenderTarget(renderer_, destination_));
        return false;
    }
    // From this point onwards reset()/begin_frame() must regard the staging
    // texture as selected even if logical-presentation setup fails below.
    rgb_staged_ = true;
    if (!SDL_SetRenderLogicalPresentation(
            renderer_,
            checked_texture_extent(logical_width_),
            checked_texture_extent(logical_height_),
            SDL_LOGICAL_PRESENTATION_STRETCH
        )) {
        const std::string logical_error = SDL_GetError();
        if (SDL_SetRenderTarget(renderer_, destination_)) {
            rgb_staged_ = false;
        }
        assign_warning(
            warning,
            "RGB Split fell back to an overlay because its logical target could not be configured: "
                + logical_error
        );
        return false;
    }

    SDL_BlendMode previous_blend{SDL_BLENDMODE_BLEND};
    static_cast<void>(SDL_GetRenderDrawBlendMode(renderer_, &previous_blend));
    static_cast<void>(SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE));
    static_cast<void>(SDL_SetRenderDrawColor(renderer_, 0U, 0U, 0U, 255U));
    static_cast<void>(SDL_RenderClear(renderer_));
    static_cast<void>(SDL_SetRenderDrawBlendMode(renderer_, previous_blend));
    rgb_staged_ = true;
    return true;
}

bool RuntimePostEffects::finish_frame(std::string* const error) {
    if (error != nullptr) {
        error->clear();
    }
    if (!frame_active_) {
        return true;
    }

    bool success = true;
    if (effect_ == PostEffect::rgb_split && rgb_staged_) {
        success = composite_rgb_split(error);
    } else if (effect_ == PostEffect::watch_dogs_scanlines) {
        draw_scanlines(false);
    } else if (effect_ == PostEffect::rgb_split && rgb_fallback_) {
        draw_scanlines(true);
    }

    // If target restoration failed, keep ownership/state intact. The caller
    // must treat this as a renderer lifecycle failure; destroying rgb_target_
    // while it may still be selected would leave SDL with a dangling target.
    if (!success && rgb_staged_
        && SDL_GetRenderTarget(renderer_) == rgb_target_) {
        return false;
    }
    frame_active_ = false;
    rgb_staged_ = false;
    rgb_fallback_ = false;
    destination_ = nullptr;
    return success;
}

bool RuntimePostEffects::composite_rgb_split(std::string* const error) {
    if (!SDL_SetRenderTarget(renderer_, destination_)) {
        if (error != nullptr) {
            *error = "RGB Split could not restore the frame destination: "
                + std::string(SDL_GetError());
        }
        return false;
    }

    SDL_BlendMode previous_draw_blend{SDL_BLENDMODE_BLEND};
    static_cast<void>(SDL_GetRenderDrawBlendMode(
        renderer_,
        &previous_draw_blend
    ));
    static_cast<void>(SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE));
    static_cast<void>(SDL_SetRenderDrawColor(renderer_, 0U, 0U, 0U, 255U));
    const bool cleared = SDL_RenderClear(renderer_);
    static_cast<void>(SDL_SetRenderDrawBlendMode(
        renderer_,
        previous_draw_blend
    ));

    static_cast<void>(SDL_SetTextureBlendMode(rgb_target_, SDL_BLENDMODE_ADD));
    static_cast<void>(SDL_SetTextureAlphaMod(rgb_target_, 255U));
    const float offset = std::clamp(
        std::min(logical_width_, logical_height_) * 0.003F,
        1.0F,
        3.0F
    );
    const SDL_FRect red_destination{
        -offset,
        0.0F,
        logical_width_,
        logical_height_,
    };
    const SDL_FRect green_destination{
        0.0F,
        0.0F,
        logical_width_,
        logical_height_,
    };
    const SDL_FRect blue_destination{
        offset,
        0.0F,
        logical_width_,
        logical_height_,
    };

    static_cast<void>(SDL_SetTextureColorMod(rgb_target_, 255U, 0U, 0U));
    const bool red = SDL_RenderTexture(
        renderer_,
        rgb_target_,
        nullptr,
        &red_destination
    );
    static_cast<void>(SDL_SetTextureColorMod(rgb_target_, 0U, 255U, 0U));
    const bool green = SDL_RenderTexture(
        renderer_,
        rgb_target_,
        nullptr,
        &green_destination
    );
    static_cast<void>(SDL_SetTextureColorMod(rgb_target_, 0U, 0U, 255U));
    const bool blue = SDL_RenderTexture(
        renderer_,
        rgb_target_,
        nullptr,
        &blue_destination
    );
    static_cast<void>(SDL_SetTextureColorMod(
        rgb_target_,
        255U,
        255U,
        255U
    ));
    static_cast<void>(SDL_SetTextureBlendMode(rgb_target_, SDL_BLENDMODE_BLEND));

    if (!cleared || !red || !green || !blue) {
        if (error != nullptr) {
            *error = "RGB Split composition failed: " + std::string(SDL_GetError());
        }
        return false;
    }
    return true;
}

void RuntimePostEffects::build_scanline_geometry(
    const float logical_width,
    const float logical_height
) {
    if (logical_width == scanline_geometry_width_
        && logical_height == scanline_geometry_height_
        && !scanline_vertices_.empty()) {
        return;
    }
    scanline_geometry_width_ = logical_width;
    scanline_geometry_height_ = logical_height;
    scanline_vertices_.clear();
    scanline_indices_.clear();

    constexpr float pitch = 4.0F;
    constexpr float thickness = 1.0F;
    const auto stripe_count = static_cast<std::size_t>(
        std::max(0.0F, std::ceil(logical_height / pitch))
    );
    scanline_vertices_.reserve(stripe_count * 4U);
    scanline_indices_.reserve(stripe_count * 6U);
    for (std::size_t stripe = 0U; stripe < stripe_count; ++stripe) {
        const float y = static_cast<float>(stripe) * pitch;
        const SDL_FColor color = stripe % 24U == 0U
            ? SDL_FColor{0.0F, 0.72F, 0.78F, 0.085F}
            : SDL_FColor{0.0F, 0.0F, 0.0F, 0.16F};
        const int base = static_cast<int>(scanline_vertices_.size());
        scanline_vertices_.push_back({{0.0F, y}, color, {0.0F, 0.0F}});
        scanline_vertices_.push_back(
            {{logical_width, y}, color, {0.0F, 0.0F}}
        );
        scanline_vertices_.push_back(
            {{logical_width, y + thickness}, color, {0.0F, 0.0F}}
        );
        scanline_vertices_.push_back(
            {{0.0F, y + thickness}, color, {0.0F, 0.0F}}
        );
        scanline_indices_.insert(
            scanline_indices_.end(),
            {base, base + 1, base + 2, base, base + 2, base + 3}
        );
    }
}

void RuntimePostEffects::draw_scanlines(const bool rgb_fallback) noexcept {
    build_scanline_geometry(logical_width_, logical_height_);
    SDL_BlendMode previous_blend{SDL_BLENDMODE_BLEND};
    static_cast<void>(SDL_GetRenderDrawBlendMode(renderer_, &previous_blend));
    static_cast<void>(SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND));
    static_cast<void>(SDL_RenderGeometry(
        renderer_,
        nullptr,
        scanline_vertices_.data(),
        static_cast<int>(scanline_vertices_.size()),
        scanline_indices_.data(),
        static_cast<int>(scanline_indices_.size())
    ));
    static_cast<void>(SDL_SetRenderDrawBlendMode(renderer_, previous_blend));
    if (!rgb_fallback) {
        return;
    }

    // Render-target-less drivers still get a deterministic, low-cost visual
    // indication instead of undefined output or an effect-related crash.
    previous_blend = SDL_BLENDMODE_BLEND;
    static_cast<void>(SDL_GetRenderDrawBlendMode(renderer_, &previous_blend));
    static_cast<void>(SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND));
    static_cast<void>(SDL_SetRenderDrawColor(renderer_, 255U, 0U, 110U, 24U));
    const SDL_FRect left{0.0F, 0.0F, 2.0F, logical_height_};
    static_cast<void>(SDL_RenderFillRect(renderer_, &left));
    static_cast<void>(SDL_SetRenderDrawColor(renderer_, 0U, 220U, 255U, 24U));
    const SDL_FRect right{
        std::max(0.0F, logical_width_ - 2.0F),
        0.0F,
        2.0F,
        logical_height_,
    };
    static_cast<void>(SDL_RenderFillRect(renderer_, &right));
    static_cast<void>(SDL_SetRenderDrawBlendMode(renderer_, previous_blend));
}

void RuntimePostEffects::reset() noexcept {
    bool may_destroy_rgb_target = true;
    if (renderer_ != nullptr && rgb_target_ != nullptr
        && SDL_GetRenderTarget(renderer_) == rgb_target_) {
        may_destroy_rgb_target = SDL_SetRenderTarget(renderer_, destination_);
    }
    frame_active_ = false;
    rgb_staged_ = false;
    rgb_fallback_ = false;
    destination_ = nullptr;
    if (rgb_target_ != nullptr && may_destroy_rgb_target) {
        SDL_DestroyTexture(rgb_target_);
    }
    // If restoration failed, deliberately relinquish the texture.  The caller
    // marks the platform unsafe and SDL_DestroyRenderer owns the final cleanup;
    // explicitly destroying a selected target here would leave a dangling
    // render target inside SDL.
    rgb_target_ = nullptr;
    rgb_target_width_ = 0;
    rgb_target_height_ = 0;
    renderer_ = nullptr;
    scanline_vertices_.clear();
    scanline_indices_.clear();
    scanline_geometry_width_ = 0.0F;
    scanline_geometry_height_ = 0.0F;
}

}  // namespace pulseforge::detail
