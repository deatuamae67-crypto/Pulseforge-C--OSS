#pragma once

#include "pulseforge/content_descriptors.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge {

struct StaticStageLuaLimits {
    std::size_t maximum_source_bytes{2U * 1024U * 1024U};
    std::size_t maximum_tokens{250'000U};
    std::size_t maximum_calls{32'768U};
    std::size_t maximum_sprites{4'096U};
    std::size_t maximum_animations{16'384U};
    std::size_t maximum_string_bytes{1'024U};
    std::size_t maximum_diagnostics{128U};
};

struct StaticStageLuaDiagnostic {
    std::size_t line{};
    std::string message;
};

struct StaticStageLuaResult {
    std::optional<StageDescriptor> stage;
    std::vector<StaticStageLuaDiagnostic> diagnostics;
    bool truncated{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return stage.has_value();
    }
};

// Parses the static, data-like subset used by common Psych/H-Slice stage Lua
// files. It never executes Lua and never performs filesystem I/O. Unsupported
// expressions and callbacks remain scripts for ScriptManager; supported
// sprite declarations are converted to the same bounded StageDescriptor used
// by native JSON stages.
[[nodiscard]] StaticStageLuaResult parse_static_psych_stage_lua(
    std::string_view source,
    std::string_view stage_id,
    const StaticStageLuaLimits& limits = {}
);

}  // namespace pulseforge
