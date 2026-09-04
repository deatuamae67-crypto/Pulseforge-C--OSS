#include "pulseforge/audio_controls.hpp"

#include <algorithm>
#include <cmath>

namespace pulseforge {

int master_volume_percent(const AudioSettings& settings) noexcept {
    const auto level = std::clamp(
        static_cast<int>(std::lround(settings.master_volume * 10.0F)),
        0,
        10
    );
    return level * 10;
}

bool adjust_master_volume(
    AudioSettings& settings,
    const int direction
) noexcept {
    if (direction == 0) {
        return false;
    }
    const auto old_level = std::clamp(
        static_cast<int>(std::lround(settings.master_volume * 10.0F)),
        0,
        10
    );
    const auto new_level = std::clamp(
        old_level + (direction > 0 ? 1 : -1),
        0,
        10
    );
    settings.master_volume = static_cast<float>(new_level) / 10.0F;
    return new_level != old_level;
}

void toggle_master_mute(AudioSettings& settings) noexcept {
    settings.muted = !settings.muted;
}

}  // namespace pulseforge
