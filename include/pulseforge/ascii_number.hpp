#pragma once

#include <charconv>
#include <cmath>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace pulseforge {

// Parse a dot-decimal floating-point value without consulting the device
// locale. Android's supported libc++ releases omit floating-point from_chars,
// while desktop standard libraries provide the allocation-free fast path.
template <typename Floating>
    requires std::is_floating_point_v<Floating>
[[nodiscard]] inline bool parse_ascii_floating(
    const std::string_view source,
    Floating& destination
) noexcept {
    if (source.empty()) {
        return false;
    }
    Floating candidate{};
#if defined(__ANDROID__)
    try {
        std::istringstream input{std::string(source)};
        input.imbue(std::locale::classic());
        input >> std::noskipws >> candidate;
        if (!input || !input.eof() || !std::isfinite(candidate)) {
            return false;
        }
    } catch (...) {
        return false;
    }
#else
    const auto* first = source.data();
    const auto* last = first + source.size();
    const auto parsed = std::from_chars(
        first,
        last,
        candidate,
        std::chars_format::general
    );
    if (parsed.ec != std::errc{} || parsed.ptr != last
        || !std::isfinite(candidate)) {
        return false;
    }
#endif
    destination = candidate;
    return true;
}

}  // namespace pulseforge
