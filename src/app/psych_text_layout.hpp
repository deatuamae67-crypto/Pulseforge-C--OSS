#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge::detail {

struct PsychDebugTextMetrics {
    double scale_x{1.0};
    double scale_y{1.0};
    double glyph_advance_px{8.0};
    double line_height_px{10.0};
};

[[nodiscard]] inline PsychDebugTextMetrics psych_debug_text_metrics(
    const double requested_size,
    const double object_scale_x,
    const double object_scale_y,
    const double camera_zoom
) noexcept {
    const double size = std::clamp(
        std::isfinite(requested_size) ? requested_size : 16.0,
        1.0,
        256.0
    );
    const double sx = std::clamp(
        std::isfinite(object_scale_x) ? object_scale_x : 1.0,
        0.01,
        100.0
    );
    const double sy = std::clamp(
        std::isfinite(object_scale_y) ? object_scale_y : 1.0,
        0.01,
        100.0
    );
    const double zoom = std::clamp(
        std::isfinite(camera_zoom) ? camera_zoom : 1.0,
        0.05,
        8.0
    );

    const double visual_advance = size * 0.5 * sx * zoom;
    const double visual_height = size * sy * zoom;

    return {
        std::clamp(visual_advance / 8.0, 0.125, 32.0),
        std::clamp(visual_height / 8.0, 0.125, 32.0),
        visual_advance,
        std::max(1.0, visual_height * 1.15),
    };
}

struct PsychTextLayoutLine {
    std::string text;
    double width_px{};
    double x_offset_px{};
};

struct PsychTextLayout {
    std::vector<PsychTextLayoutLine> lines;
};

namespace psych_text_layout_detail {

[[nodiscard]] inline std::size_t utf8_sequence_length(
    const unsigned char lead
) noexcept {
    if ((lead & 0x80U) == 0U) return 1U;
    if ((lead & 0xE0U) == 0xC0U) return 2U;
    if ((lead & 0xF0U) == 0xE0U) return 3U;
    if ((lead & 0xF8U) == 0xF0U) return 4U;
    return 1U;
}

[[nodiscard]] inline std::size_t utf8_codepoint_count(
    const std::string_view text
) noexcept {
    std::size_t count = 0U;
    for (std::size_t index = 0U; index < text.size();) {
        const auto bytes = std::min(
            utf8_sequence_length(static_cast<unsigned char>(text[index])),
            text.size() - index
        );
        index += bytes;
        ++count;
    }
    return count;
}

[[nodiscard]] inline std::size_t byte_offset_for_codepoints(
    const std::string_view text,
    const std::size_t codepoints
) noexcept {
    std::size_t index = 0U;
    std::size_t count = 0U;
    while (index < text.size() && count < codepoints) {
        const auto bytes = std::min(
            utf8_sequence_length(static_cast<unsigned char>(text[index])),
            text.size() - index
        );
        index += bytes;
        ++count;
    }
    return index;
}

[[nodiscard]] inline bool ascii_space(const char value) noexcept {
    return value == ' ' || value == '\t';
}

inline void trim_right_ascii_space(std::string& text) {
    while (!text.empty() && ascii_space(text.back())) {
        text.pop_back();
    }
}

inline void trim_left_ascii_space(std::string_view& text) noexcept {
    while (!text.empty() && ascii_space(text.front())) {
        text.remove_prefix(1U);
    }
}

}  // namespace psych_text_layout_detail

[[nodiscard]] inline PsychTextLayout layout_psych_debug_text(
    const std::string_view input,
    const double field_width_px,
    const double glyph_advance_px,
    const std::string_view alignment,
    const std::size_t maximum_lines = 256U
) {
    using namespace psych_text_layout_detail;

    PsychTextLayout result;
    if (input.empty() || maximum_lines == 0U) return result;

    const double advance = std::max(0.125, glyph_advance_px);
    const double field_width = std::max(0.0, field_width_px);
    const std::size_t max_columns = field_width > 0.0
        ? std::max<std::size_t>(
            1U,
            static_cast<std::size_t>(std::floor(field_width / advance))
        )
        : static_cast<std::size_t>(-1);

    auto append_line = [&](std::string line) {
        if (result.lines.size() >= maximum_lines) return;

        const auto codepoints = utf8_codepoint_count(line);
        const double line_width = static_cast<double>(codepoints) * advance;

        double offset = 0.0;
        if (field_width > line_width) {
            if (alignment == "center" || alignment == "Center") {
                offset = (field_width - line_width) * 0.5;
            } else if (alignment == "right" || alignment == "Right") {
                offset = field_width - line_width;
            }
        }

        result.lines.push_back({
            std::move(line),
            line_width,
            offset,
        });
    };

    std::size_t paragraph_begin = 0U;
    while (paragraph_begin <= input.size()
           && result.lines.size() < maximum_lines) {
        const auto newline = input.find('\n', paragraph_begin);
        const auto paragraph_end = newline == std::string_view::npos
            ? input.size()
            : newline;
        std::string_view remaining =
            input.substr(paragraph_begin, paragraph_end - paragraph_begin);

        if (remaining.empty()) {
            append_line(std::string{});
        } else if (field_width <= 0.0) {
            append_line(std::string(remaining));
        } else {
            while (!remaining.empty()
                   && result.lines.size() < maximum_lines) {
                trim_left_ascii_space(remaining);
                if (remaining.empty()) break;

                const auto codepoints = utf8_codepoint_count(remaining);
                if (codepoints <= max_columns) {
                    append_line(std::string(remaining));
                    remaining = {};
                    break;
                }

                const auto hard_byte =
                    byte_offset_for_codepoints(remaining, max_columns);

                std::size_t break_byte = hard_byte;
                for (std::size_t cursor = hard_byte; cursor > 0U; --cursor) {
                    if (ascii_space(remaining[cursor - 1U])) {
                        break_byte = cursor - 1U;
                        break;
                    }
                }
                if (break_byte == 0U) break_byte = hard_byte;

                std::string line(remaining.substr(0U, break_byte));
                trim_right_ascii_space(line);
                append_line(std::move(line));

                remaining.remove_prefix(break_byte);
                trim_left_ascii_space(remaining);
            }
        }

        if (newline == std::string_view::npos) break;
        paragraph_begin = newline + 1U;
    }

    return result;
}

}  // namespace pulseforge::detail
