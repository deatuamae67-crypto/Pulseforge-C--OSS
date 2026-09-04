#pragma once

namespace pulseforge::detail {

[[nodiscard]] constexpr bool psych_character_effective_flip_x(
    const bool descriptor_flip_x,
    const bool is_player
) noexcept {
    return descriptor_flip_x != is_player;
}

}  // namespace pulseforge::detail
