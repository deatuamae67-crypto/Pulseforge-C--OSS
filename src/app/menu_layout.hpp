#pragma once

#include <algorithm>
#include <cstddef>

namespace pulseforge::detail {

struct MenuVisibleRange {
    std::size_t first{};
    std::size_t last{};
};

[[nodiscard]] constexpr MenuVisibleRange menu_visible_range(
    const std::size_t item_count,
    const std::size_t selected,
    const std::size_t capacity
) noexcept {
    if (item_count == 0U || capacity == 0U) {
        return {};
    }
    const auto visible = std::min(item_count, capacity);
    const auto bounded_selected = std::min(selected, item_count - 1U);
    const auto half = visible / 2U;
    auto first = bounded_selected > half ? bounded_selected - half : 0U;
    if (first + visible > item_count) {
        first = item_count - visible;
    }
    return {first, first + visible};
}

}  // namespace pulseforge::detail
