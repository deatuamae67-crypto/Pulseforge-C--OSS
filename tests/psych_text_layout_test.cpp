#include "psych_text_layout.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void require_near(
    const double actual,
    const double expected,
    const double tolerance,
    const std::string& message
) {
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(message);
    }
}

void test_size_30_metrics() {
    const auto metrics =
        pulseforge::detail::psych_debug_text_metrics(30.0, 1.0, 1.0, 1.0);

    require_near(metrics.glyph_advance_px, 15.0, 0.001,
                 "30px text advance should be half-em");
    require_near(metrics.scale_x, 1.875, 0.001,
                 "horizontal debug glyph scale");
    require_near(metrics.scale_y, 3.75, 0.001,
                 "vertical debug glyph scale");
}

void test_width_300_wraps_side_hud() {
    const auto metrics =
        pulseforge::detail::psych_debug_text_metrics(30.0, 1.0, 1.0, 1.0);

    const auto layout = pulseforge::detail::layout_psych_debug_text(
        "Player Notes: 0 | Opponent Notes: 0 | Misseds: 0",
        300.0,
        metrics.glyph_advance_px,
        "left"
    );

    require(layout.lines.size() >= 2U, "Side HUD should wrap");
    for (const auto& line : layout.lines) {
        require(line.width_px <= 300.001, "wrapped line escaped width");
    }
}

void test_zero_width_no_wrap() {
    const auto layout = pulseforge::detail::layout_psych_debug_text(
        "one two three four", 0.0, 8.0, "left"
    );
    require(layout.lines.size() == 1U, "zero width should auto-size");
    require(layout.lines.front().text == "one two three four",
            "auto-width contents changed");
}

void test_alignment() {
    const auto center = pulseforge::detail::layout_psych_debug_text(
        "abc", 100.0, 10.0, "center"
    );
    require_near(center.lines.front().x_offset_px, 35.0, 0.001,
                 "center alignment");

    const auto right = pulseforge::detail::layout_psych_debug_text(
        "abc", 100.0, 10.0, "right"
    );
    require_near(right.lines.front().x_offset_px, 70.0, 0.001,
                 "right alignment");
}

void test_newline_and_utf8() {
    const auto hard = pulseforge::detail::layout_psych_debug_text(
        "first\nsecond", 300.0, 10.0, "left"
    );
    require(hard.lines.size() == 2U, "explicit newline not honoured");

    constexpr std::u8string_view unicode_u8 = u8"áéíóú漢字Δ";
    const std::string unicode(
        reinterpret_cast<const char*>(unicode_u8.data()),
        unicode_u8.size()
    );
    const auto wrapped = pulseforge::detail::layout_psych_debug_text(
        unicode, 20.0, 10.0, "left"
    );
    std::string joined;
    for (const auto& line : wrapped.lines) joined += line.text;
    require(joined == unicode, "UTF-8 corrupted while wrapping");
}

void test_line_bound() {
    const auto layout = pulseforge::detail::layout_psych_debug_text(
        "a a a a a a a a a a a a a a a a",
        8.0,
        8.0,
        "left",
        4U
    );
    require(layout.lines.size() == 4U, "maximum line bound ignored");
}

}  // namespace

int main() {
    try {
        test_size_30_metrics();
        test_width_300_wraps_side_hud();
        test_zero_width_no_wrap();
        test_alignment();
        test_newline_and_utf8();
        test_line_bound();
        std::cout << "[PASS] Psych Lua text fallback layout\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
