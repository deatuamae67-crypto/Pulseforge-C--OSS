#include "pulseforge/shader_catalog.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void write_text(
    const std::filesystem::path& path,
    const std::string_view text
) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to create shader fixture");
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) {
        throw std::runtime_error("failed to write shader fixture");
    }
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto nonce = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("pulseforge-shader-test-" + std::to_string(nonce));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] bool has_diagnostic(
    const std::span<const pulseforge::ShaderDiagnostic> diagnostics,
    const pulseforge::ShaderDiagnosticCode code
) {
    return std::any_of(
        diagnostics.begin(),
        diagnostics.end(),
        [code](const pulseforge::ShaderDiagnostic& diagnostic) {
            return diagnostic.code == code;
        }
    );
}

[[nodiscard]] const pulseforge::ShaderUniformDeclaration* find_uniform(
    const pulseforge::ShaderCatalogEntry& entry,
    const std::string_view name
) {
    const auto found = std::find_if(
        entry.uniforms.begin(),
        entry.uniforms.end(),
        [name](const pulseforge::ShaderUniformDeclaration& uniform) {
            return uniform.name == name;
        }
    );
    return found == entry.uniforms.end() ? nullptr : std::addressof(*found);
}

void test_overlay_reflection_and_reload(const std::filesystem::path& root) {
    const auto base = root / "base";
    const auto high = root / "high";
    write_text(
        base / "shaders" / "Shared.frag",
        "uniform float baseOnly;\n"
        "void main() { gl_FragColor = vec4(baseOnly); }\n"
    );
    write_text(
        base / "shaders" / "Shared.vert",
        "uniform mat4 transform;\n"
        "void main() { gl_Position = transform * vec4(0.0); }\n"
    );
    write_text(
        high / "shaders" / "shared.frag",
        "#pragma header\n"
        "uniform sampler2D noiseTex;\n"
        "uniform highp float intensity;\n"
        "uniform vec3 tint[2];\n"
        "void main() { gl_FragColor = vec4(tint[0] * intensity, 1.0); }\n"
    );

    pulseforge::ShaderCatalogOptions options;
    options.roots = {
        {"base", base, 0},
        {"active-mod", high, 50},
    };
    const auto catalog = pulseforge::ShaderCatalog::scan(options);
    require(!catalog.truncated(), "small shader catalog must not truncate");
    require(catalog.entries().size() == 1U, "case aliases produce one program");
    const auto* entry = catalog.find("SHARED");
    require(entry != nullptr, "shader lookup is ASCII case-insensitive");
    require(entry->valid, "valid vertex and fragment pair is accepted");
    require(
        entry->fragment.root_id == "active-mod",
        "higher-priority fragment overrides base content"
    );
    require(
        entry->vertex.has_value() && entry->vertex->root_id == "base",
        "optional vertex stage falls back to the lower-priority root"
    );
    require(
        find_uniform(*entry, "baseOnly") == nullptr,
        "shadowed fragment uniforms are not reflected"
    );
    const auto* tint = find_uniform(*entry, "tint");
    require(tint != nullptr, "fragment array uniform is reflected");
    require(tint->array_elements == 2U, "fixed uniform array length is retained");
    require(
        tint->type == pulseforge::ShaderUniformType::vec3
            && tint->used_by_fragment && !tint->used_by_vertex,
        "uniform type and stage visibility are retained"
    );
    const auto* transform = find_uniform(*entry, "transform");
    require(
        transform != nullptr && transform->used_by_vertex,
        "lower-root optional vertex uniforms are merged"
    );

    const auto first_load = catalog.load("shared");
    require(static_cast<bool>(first_load), "valid shader source loads on demand");
    require(
        first_load.program->fragment.find("noiseTex") != std::string::npos,
        "load returns the winning source"
    );
    require(
        first_load.program->source_fingerprint == entry->source_fingerprint,
        "scan and load calculate the same stable fingerprint"
    );

    write_text(
        high / "shaders" / "shared.frag",
        "#pragma header\n"
        "uniform sampler2D noiseTex;\n"
        "uniform highp float intensity;\n"
        "uniform vec3 tint[2];\n"
        "void main() { gl_FragColor = vec4(tint[1] * intensity, 1.0); }\n"
    );
    const auto changed_load = catalog.load("shared");
    require(
        static_cast<bool>(changed_load),
        "changed source is revalidated instead of trusted from the scan"
    );
    require(
        has_diagnostic(
            changed_load.diagnostics,
            pulseforge::ShaderDiagnosticCode::source_changed
        ),
        "changed source produces an explicit diagnostic"
    );

    const auto capability = pulseforge::current_shader_runtime_capability();
    require(capability.catalog_available, "current runtime exposes the catalog");
    require(
        !capability.custom_program_execution && !capability.accepts_raw_glsl,
        "SDL_Renderer is not falsely advertised as a GLSL backend"
    );
}

void test_validation_budgets(const std::filesystem::path& root) {
    const auto shaders = root / "shaders";
    write_text(
        shaders / "include.frag",
        "#include \"../escape.glsl\"\n"
        "void main() { gl_FragColor = vec4(1.0); }\n"
    );
    write_text(
        shaders / "samplers.frag",
        "uniform sampler2D firstTexture;\n"
        "uniform sampler2D secondTexture;\n"
        "void main() { gl_FragColor = vec4(1.0); }\n"
    );
    write_text(
        shaders / "conflict.frag",
        "uniform float sharedValue;\n"
        "void main() { gl_FragColor = vec4(sharedValue); }\n"
    );
    write_text(
        shaders / "conflict.vert",
        "uniform int sharedValue;\n"
        "void main() { gl_Position = vec4(float(sharedValue)); }\n"
    );
    auto nul_source = std::string(
        "void main() { gl_FragColor = vec4(1.0); }\n"
    );
    nul_source.insert(nul_source.begin() + 8, '\0');
    write_text(shaders / "embedded-nul.frag", nul_source);
    write_text(shaders / "oversize.frag", std::string(1'024U, 'x'));

    pulseforge::ShaderCatalogOptions options;
    options.roots = {{"mod", root, 0}};
    options.limits.maximum_samplers = 1U;
    options.limits.maximum_source_bytes = 512U;
    const auto catalog = pulseforge::ShaderCatalog::scan(options);
    require(
        catalog.entries().size() == 5U,
        "invalid sources remain visible to a diagnostics/editor menu"
    );
    require(!catalog.find("include")->valid, "includes are rejected for now");
    require(!catalog.find("samplers")->valid, "sampler budget is enforced");
    require(
        !catalog.find("conflict")->valid,
        "cross-stage uniform type conflicts are rejected"
    );
    require(!catalog.find("oversize")->valid, "source byte cap is enforced");
    require(
        !catalog.find("embedded-nul")->valid,
        "embedded NUL bytes are rejected"
    );
    require(
        has_diagnostic(
            catalog.diagnostics(),
            pulseforge::ShaderDiagnosticCode::include_not_supported
        ),
        "include rejection is diagnosed"
    );
    require(
        has_diagnostic(
            catalog.diagnostics(),
            pulseforge::ShaderDiagnosticCode::sampler_limit_exceeded
        ),
        "sampler rejection is diagnosed"
    );
    require(
        has_diagnostic(
            catalog.diagnostics(),
            pulseforge::ShaderDiagnosticCode::uniform_type_conflict
        ),
        "uniform conflict is diagnosed"
    );

    auto silent_options = options;
    silent_options.limits.maximum_diagnostics = 0U;
    const auto silent_catalog = pulseforge::ShaderCatalog::scan(silent_options);
    require(
        silent_catalog.diagnostics().empty()
            && silent_catalog.find("conflict") != nullptr
            && !silent_catalog.find("conflict")->valid,
        "semantic rejection does not depend on diagnostic retention budget"
    );
    require(
        has_diagnostic(
            catalog.diagnostics(),
            pulseforge::ShaderDiagnosticCode::source_too_large
        ),
        "oversize source is diagnosed"
    );
    require(
        has_diagnostic(
            catalog.diagnostics(),
            pulseforge::ShaderDiagnosticCode::embedded_nul
        ),
        "embedded NUL is diagnosed"
    );
    require(
        !catalog.load("missing"),
        "loading an unknown shader returns a typed failure"
    );
}

void test_path_confinement_and_truncation(const std::filesystem::path& root) {
    const auto mod = root / "mod";
    const auto outside = root / "outside";
    write_text(
        mod / "shaders" / "a.frag",
        "void main() { gl_FragColor = vec4(1.0); }\n"
    );
    write_text(
        mod / "shaders" / "b.frag",
        "void main() { gl_FragColor = vec4(0.0); }\n"
    );
    write_text(
        outside / "escape.frag",
        "void main() { gl_FragColor = vec4(0.5); }\n"
    );

    pulseforge::ShaderCatalogOptions invalid_directory;
    invalid_directory.roots = {{"mod", mod, 0}};
    invalid_directory.search_directories = {"../outside"};
    const auto invalid = pulseforge::ShaderCatalog::scan(invalid_directory);
    require(invalid.entries().empty(), "parent search path is never scanned");
    require(
        has_diagnostic(
            invalid.diagnostics(),
            pulseforge::ShaderDiagnosticCode::invalid_search_directory
        ),
        "unsafe search directory is diagnosed"
    );

    std::error_code symlink_error;
    std::filesystem::create_symlink(
        outside / "escape.frag",
        mod / "shaders" / "linked.frag",
        symlink_error
    );
    if (!symlink_error) {
        pulseforge::ShaderCatalogOptions confined_options;
        confined_options.roots = {{"mod", mod, 0}};
        const auto confined = pulseforge::ShaderCatalog::scan(
            confined_options
        );
        require(
            confined.find("linked") == nullptr,
            "a shader symlink cannot escape its mod root"
        );
        require(
            has_diagnostic(
                confined.diagnostics(),
                pulseforge::ShaderDiagnosticCode::path_escape
            ),
            "symlink escape is diagnosed"
        );
    }

    pulseforge::ShaderCatalogOptions limited;
    limited.roots = {{"mod", mod, 0}};
    limited.limits.maximum_entries = 1U;
    const auto truncated = pulseforge::ShaderCatalog::scan(limited);
    require(truncated.truncated(), "entry cap marks the catalog truncated");
    require(truncated.entries().size() == 1U, "entry cap is enforced exactly");
}

}  // namespace

int main() {
    try {
        TemporaryDirectory temporary;
        test_overlay_reflection_and_reload(temporary.path() / "overlay");
        test_validation_budgets(temporary.path() / "validation");
        test_path_confinement_and_truncation(temporary.path() / "paths");
        std::cout << "Shader catalog tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "Shader catalog tests failed: " << exception.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
