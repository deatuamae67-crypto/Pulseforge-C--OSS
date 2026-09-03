#include "psych_character_semantics.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void test_all_flip_combinations() {
    using pulseforge::detail::psych_character_effective_flip_x;

    require(!psych_character_effective_flip_x(false, false),
            "non-player false flip_x must remain false");
    require(psych_character_effective_flip_x(true, false),
            "non-player true flip_x must remain true");
    require(psych_character_effective_flip_x(false, true),
            "player false flip_x must invert to true");
    require(!psych_character_effective_flip_x(true, true),
            "player true flip_x must invert to false");
}

}  // namespace

int main() {
    try {
        test_all_flip_combinations();
        std::cout << "[PASS] Psych player-facing character semantics\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
