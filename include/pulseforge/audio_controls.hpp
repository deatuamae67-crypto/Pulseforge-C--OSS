#pragma once

#include "pulseforge/settings.hpp"

namespace pulseforge {

// Master volume is deliberately quantized at the settings boundary. This
// keeps keyboard adjustment, menus, gameplay and persisted JSON in agreement.
[[nodiscard]] int master_volume_percent(const AudioSettings& settings) noexcept;
[[nodiscard]] bool adjust_master_volume(
    AudioSettings& settings,
    int direction
) noexcept;
void toggle_master_mute(AudioSettings& settings) noexcept;

}  // namespace pulseforge
