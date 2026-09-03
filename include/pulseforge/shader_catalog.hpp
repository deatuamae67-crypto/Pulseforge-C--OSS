#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge {

enum class ShaderStage : std::uint8_t {
    vertex,
    fragment,
};

enum class ShaderUniformType : std::uint8_t {
    unknown,
    boolean,
    signed_integer,
    unsigned_integer,
    floating_point,
    vec2,
    vec3,
    vec4,
    ivec2,
    ivec3,
    ivec4,
    uvec2,
    uvec3,
    uvec4,
    mat2,
    mat3,
    mat4,
    sampler2d,
    sampler_other,
};

enum class ShaderDiagnosticSeverity : std::uint8_t {
    information,
    warning,
    error,
};

enum class ShaderDiagnosticCode : std::uint8_t {
    invalid_configuration,
    invalid_identifier,
    invalid_root,
    invalid_search_directory,
    io_failure,
    path_escape,
    source_too_large,
    embedded_nul,
    include_not_supported,
    missing_entry_point,
    malformed_uniform,
    uniform_type_conflict,
    uniform_limit_exceeded,
    sampler_limit_exceeded,
    entry_limit_exceeded,
    file_limit_exceeded,
    source_changed,
    shader_not_found,
};

struct ShaderLimits {
    std::uintmax_t maximum_source_bytes{512U * 1024U};
    std::size_t maximum_files{8'192U};
    std::size_t maximum_entries{2'048U};
    std::size_t maximum_diagnostics{256U};
    std::size_t maximum_directory_depth{8U};
    std::size_t maximum_uniforms{256U};
    std::size_t maximum_samplers{16U};
    std::size_t maximum_identifier_bytes{192U};
};

struct ShaderSearchRoot {
    std::string id;
    std::filesystem::path root;
    std::int32_t priority{};
};

struct ShaderCatalogOptions {
    // Higher priorities win. For equal priorities, a later declaration wins,
    // matching the overlay semantics of VirtualFileSystem.
    std::vector<ShaderSearchRoot> roots;
    // Every entry must be a relative, confined directory. Earlier directories
    // win inside the same root (Psych-style `shaders` before built-in assets).
    std::vector<std::filesystem::path> search_directories{
        "shaders",
        "assets/shaders",
    };
    ShaderLimits limits;
};

struct ShaderSourceLocation {
    std::string root_id;
    std::filesystem::path root;
    std::filesystem::path physical_path;
    std::string virtual_path;
    std::int32_t root_priority{};
    std::size_t root_declaration_order{};
    std::uintmax_t size_bytes{};
};

struct ShaderUniformDeclaration {
    std::string name;
    ShaderUniformType type{ShaderUniformType::unknown};
    // One means scalar/non-array. Zero means an unsized array.
    std::size_t array_elements{1U};
    bool used_by_vertex{};
    bool used_by_fragment{};
};

struct ShaderDiagnostic {
    ShaderDiagnosticSeverity severity{ShaderDiagnosticSeverity::information};
    ShaderDiagnosticCode code{ShaderDiagnosticCode::io_failure};
    std::string shader_id;
    std::filesystem::path path;
    std::size_t line{};
    std::string message;
};

struct ShaderCatalogEntry {
    std::string id;
    ShaderSourceLocation fragment;
    std::optional<ShaderSourceLocation> vertex;
    std::vector<ShaderUniformDeclaration> uniforms;
    std::uint64_t source_fingerprint{};
    bool valid{};
};

struct ShaderProgramSource {
    std::string id;
    std::string fragment;
    std::optional<std::string> vertex;
    std::vector<ShaderUniformDeclaration> uniforms;
    std::uint64_t source_fingerprint{};
};

struct ShaderLoadResult {
    std::optional<ShaderProgramSource> program;
    std::vector<ShaderDiagnostic> diagnostics;

    [[nodiscard]] explicit operator bool() const noexcept {
        return program.has_value();
    }
};

// This describes the renderer used by the current PulseForge runtime. It is a
// deliberately explicit null execution backend: shader sources can be safely
// discovered and edited, but SDL_Renderer draws are never advertised as GLSL.
struct ShaderRuntimeCapability {
    std::string_view backend_name;
    bool catalog_available{};
    bool custom_program_execution{};
    bool accepts_raw_glsl{};
    bool maximum_performance_bypass{};
    std::string_view status;
};

class ShaderCatalog final {
public:
    [[nodiscard]] static ShaderCatalog scan(
        const ShaderCatalogOptions& options
    );

    [[nodiscard]] std::span<const ShaderCatalogEntry> entries() const noexcept;
    [[nodiscard]] std::span<const ShaderDiagnostic> diagnostics() const noexcept;
    [[nodiscard]] bool truncated() const noexcept;

    // Matching is ASCII case-insensitive and accepts slash-separated nested
    // identifiers such as `post/watch-dogs-scanlines`.
    [[nodiscard]] const ShaderCatalogEntry* find(
        std::string_view shader_id
    ) const noexcept;

    // Re-opens and revalidates the selected source at use time. A mod cannot
    // replace a file after scanning and thereby bypass size/path checks.
    [[nodiscard]] ShaderLoadResult load(std::string_view shader_id) const;

private:
    ShaderLimits limits_;
    std::vector<ShaderCatalogEntry> entries_;
    std::vector<ShaderDiagnostic> diagnostics_;
    bool truncated_{};
};

[[nodiscard]] ShaderRuntimeCapability current_shader_runtime_capability()
    noexcept;
[[nodiscard]] std::string_view to_string(ShaderStage stage) noexcept;
[[nodiscard]] std::string_view to_string(ShaderUniformType type) noexcept;
[[nodiscard]] std::string_view to_string(
    ShaderDiagnosticSeverity severity
) noexcept;
[[nodiscard]] std::string_view to_string(ShaderDiagnosticCode code) noexcept;

}  // namespace pulseforge
