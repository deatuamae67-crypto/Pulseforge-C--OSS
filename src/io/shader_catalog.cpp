#include "pulseforge/shader_catalog.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace pulseforge {
namespace {

constexpr std::uint64_t fnv_offset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t fnv_prime = 1'099'511'628'211ULL;

[[nodiscard]] char ascii_lower(const char value) noexcept {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value - 'A' + 'a')
        : value;
}

[[nodiscard]] std::string lower_ascii(const std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), ascii_lower);
    return result;
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {
        reinterpret_cast<const char*>(value.data()),
        value.size(),
    };
}

[[nodiscard]] std::filesystem::path normalized_absolute(
    const std::filesystem::path& path,
    std::error_code& error
) {
    error.clear();
    auto result = std::filesystem::weakly_canonical(path, error);
    if (error) {
        error.clear();
        result = std::filesystem::absolute(path, error);
    }
    return error ? std::filesystem::path{} : result.lexically_normal();
}

[[nodiscard]] bool equal_path_component(
    const std::filesystem::path& left,
    const std::filesystem::path& right
) {
#if defined(_WIN32)
    return lower_ascii(path_utf8(left)) == lower_ascii(path_utf8(right));
#else
    return left == right;
#endif
}

[[nodiscard]] bool path_is_within(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate
) {
    auto root_part = root.begin();
    auto candidate_part = candidate.begin();
    for (; root_part != root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end()
            || !equal_path_component(*root_part, *candidate_part)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool safe_relative_directory(
    const std::filesystem::path& path
) {
    if (path.empty() || path.is_absolute() || path.has_root_name()
        || path.has_root_directory()) {
        return false;
    }
    for (const auto& component : path) {
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::string> canonical_shader_id(
    const std::string_view raw,
    const std::size_t maximum_bytes
) {
    if (raw.empty() || raw.size() > maximum_bytes) {
        return std::nullopt;
    }
    std::string normalized;
    normalized.reserve(raw.size());
    for (const char value : raw) {
        const auto byte = static_cast<unsigned char>(value);
        if (byte == 0U || byte < 0x20U || value == ':' || value == '*'
            || value == '?' || value == '"' || value == '<' || value == '>'
            || value == '|') {
            return std::nullopt;
        }
        normalized.push_back(value == '\\' ? '/' : ascii_lower(value));
    }
    if (normalized.front() == '/' || normalized.back() == '/') {
        return std::nullopt;
    }
    std::size_t segment_begin{};
    while (segment_begin < normalized.size()) {
        const auto separator = normalized.find('/', segment_begin);
        const auto segment_end = separator == std::string::npos
            ? normalized.size()
            : separator;
        const auto segment = std::string_view(normalized).substr(
            segment_begin,
            segment_end - segment_begin
        );
        if (segment.empty() || segment == "." || segment == "..") {
            return std::nullopt;
        }
        if (separator == std::string::npos) {
            break;
        }
        segment_begin = separator + 1U;
    }
    return normalized;
}

class DiagnosticSink final {
public:
    DiagnosticSink(
        std::vector<ShaderDiagnostic>& diagnostics,
        const std::size_t maximum
    ) noexcept
        : diagnostics_(diagnostics), maximum_(maximum) {}

    void add(
        const ShaderDiagnosticSeverity severity,
        const ShaderDiagnosticCode code,
        std::string shader_id,
        std::filesystem::path path,
        const std::size_t line,
        std::string message
    ) {
        if (diagnostics_.size() >= maximum_) {
            return;
        }
        diagnostics_.push_back({
            severity,
            code,
            std::move(shader_id),
            std::move(path),
            line,
            std::move(message),
        });
    }

private:
    std::vector<ShaderDiagnostic>& diagnostics_;
    std::size_t maximum_{};
};

struct Candidate {
    ShaderSourceLocation location;
    std::size_t search_directory_order{};
};

[[nodiscard]] bool candidate_wins(
    const Candidate& incoming,
    const Candidate& current
) {
    if (incoming.location.root_priority
        != current.location.root_priority) {
        return incoming.location.root_priority
            > current.location.root_priority;
    }
    if (incoming.location.root_declaration_order
        != current.location.root_declaration_order) {
        return incoming.location.root_declaration_order
            > current.location.root_declaration_order;
    }
    if (incoming.search_directory_order
        != current.search_directory_order) {
        return incoming.search_directory_order
            < current.search_directory_order;
    }
    return path_utf8(incoming.location.physical_path)
        < path_utf8(current.location.physical_path);
}

void consider_candidate(
    std::map<std::string, Candidate>& candidates,
    std::string shader_id,
    Candidate candidate
) {
    const auto found = candidates.find(shader_id);
    if (found == candidates.end()) {
        candidates.emplace(std::move(shader_id), std::move(candidate));
        return;
    }
    if (candidate_wins(candidate, found->second)) {
        found->second = std::move(candidate);
    }
}

[[nodiscard]] bool is_shader_extension(
    const std::filesystem::path& path,
    ShaderStage& stage
) {
    const auto extension = lower_ascii(path_utf8(path.extension()));
    if (extension == ".frag") {
        stage = ShaderStage::fragment;
        return true;
    }
    if (extension == ".vert") {
        stage = ShaderStage::vertex;
        return true;
    }
    return false;
}

enum class TokenKind : std::uint8_t {
    identifier,
    number,
    symbol,
};

struct Token {
    std::string_view text;
    std::size_t line{1U};
    TokenKind kind{TokenKind::symbol};
};

[[nodiscard]] bool identifier_start(const char value) noexcept {
    return (value >= 'A' && value <= 'Z')
        || (value >= 'a' && value <= 'z') || value == '_';
}

[[nodiscard]] bool identifier_continue(const char value) noexcept {
    return identifier_start(value) || (value >= '0' && value <= '9');
}

[[nodiscard]] std::vector<Token> tokenize_glsl(
    const std::string_view source
) {
    std::vector<Token> tokens;
    tokens.reserve(source.size() / 8U);
    std::size_t offset{};
    std::size_t line{1U};
    while (offset < source.size()) {
        const char value = source[offset];
        if (value == '\n') {
            ++line;
            ++offset;
            continue;
        }
        if (value == ' ' || value == '\t' || value == '\r'
            || value == '\f' || value == '\v') {
            ++offset;
            continue;
        }
        if (value == '/' && offset + 1U < source.size()) {
            if (source[offset + 1U] == '/') {
                offset += 2U;
                while (offset < source.size() && source[offset] != '\n') {
                    ++offset;
                }
                continue;
            }
            if (source[offset + 1U] == '*') {
                offset += 2U;
                while (offset + 1U < source.size()
                    && !(source[offset] == '*'
                        && source[offset + 1U] == '/')) {
                    if (source[offset] == '\n') {
                        ++line;
                    }
                    ++offset;
                }
                offset = std::min(source.size(), offset + 2U);
                continue;
            }
        }
        if (value == '"' || value == '\'') {
            const char delimiter = value;
            ++offset;
            bool escaped{};
            while (offset < source.size()) {
                const char current = source[offset++];
                if (current == '\n') {
                    ++line;
                }
                if (!escaped && current == delimiter) {
                    break;
                }
                if (!escaped && current == '\\') {
                    escaped = true;
                } else {
                    escaped = false;
                }
            }
            continue;
        }
        if (identifier_start(value)) {
            const auto begin = offset++;
            while (offset < source.size()
                && identifier_continue(source[offset])) {
                ++offset;
            }
            tokens.push_back({
                source.substr(begin, offset - begin),
                line,
                TokenKind::identifier,
            });
            continue;
        }
        if (value >= '0' && value <= '9') {
            const auto begin = offset++;
            while (offset < source.size()
                && source[offset] >= '0' && source[offset] <= '9') {
                ++offset;
            }
            tokens.push_back({
                source.substr(begin, offset - begin),
                line,
                TokenKind::number,
            });
            continue;
        }
        tokens.push_back({
            source.substr(offset, 1U),
            line,
            TokenKind::symbol,
        });
        ++offset;
    }
    return tokens;
}

[[nodiscard]] ShaderUniformType uniform_type(
    const std::string_view name
) noexcept {
    if (name == "bool") {
        return ShaderUniformType::boolean;
    }
    if (name == "int") {
        return ShaderUniformType::signed_integer;
    }
    if (name == "uint") {
        return ShaderUniformType::unsigned_integer;
    }
    if (name == "float" || name == "double") {
        return ShaderUniformType::floating_point;
    }
    if (name == "vec2" || name == "dvec2") {
        return ShaderUniformType::vec2;
    }
    if (name == "vec3" || name == "dvec3") {
        return ShaderUniformType::vec3;
    }
    if (name == "vec4" || name == "dvec4") {
        return ShaderUniformType::vec4;
    }
    if (name == "ivec2") {
        return ShaderUniformType::ivec2;
    }
    if (name == "ivec3") {
        return ShaderUniformType::ivec3;
    }
    if (name == "ivec4") {
        return ShaderUniformType::ivec4;
    }
    if (name == "uvec2") {
        return ShaderUniformType::uvec2;
    }
    if (name == "uvec3") {
        return ShaderUniformType::uvec3;
    }
    if (name == "uvec4") {
        return ShaderUniformType::uvec4;
    }
    if (name == "mat2" || name == "dmat2") {
        return ShaderUniformType::mat2;
    }
    if (name == "mat3" || name == "dmat3") {
        return ShaderUniformType::mat3;
    }
    if (name == "mat4" || name == "dmat4") {
        return ShaderUniformType::mat4;
    }
    if (name == "sampler2D") {
        return ShaderUniformType::sampler2d;
    }
    if (name.starts_with("sampler") || name.starts_with("isampler")
        || name.starts_with("usampler")) {
        return ShaderUniformType::sampler_other;
    }
    return ShaderUniformType::unknown;
}

[[nodiscard]] bool uniform_qualifier(const std::string_view value) noexcept {
    return value == "lowp" || value == "mediump" || value == "highp"
        || value == "readonly" || value == "writeonly"
        || value == "coherent" || value == "volatile"
        || value == "restrict" || value == "const";
}

struct StageInspection {
    std::vector<ShaderUniformDeclaration> uniforms;
    bool has_error{};
};

void add_inspection_error(
    StageInspection& inspection,
    DiagnosticSink& diagnostics,
    const ShaderDiagnosticCode code,
    const std::string& shader_id,
    const std::filesystem::path& path,
    const std::size_t line,
    std::string message
) {
    inspection.has_error = true;
    diagnostics.add(
        ShaderDiagnosticSeverity::error,
        code,
        shader_id,
        path,
        line,
        std::move(message)
    );
}

[[nodiscard]] StageInspection inspect_stage(
    const std::string_view source,
    const ShaderStage stage,
    const std::string& shader_id,
    const std::filesystem::path& path,
    const ShaderLimits& limits,
    DiagnosticSink& diagnostics
) {
    StageInspection result;
    if (source.find('\0') != std::string_view::npos) {
        add_inspection_error(
            result,
            diagnostics,
            ShaderDiagnosticCode::embedded_nul,
            shader_id,
            path,
            0U,
            "shader source contains an embedded NUL byte"
        );
        return result;
    }

    const auto tokens = tokenize_glsl(source);
    bool has_main{};
    for (std::size_t index = 0; index + 2U < tokens.size(); ++index) {
        if (tokens[index].text == "#"
            && tokens[index + 1U].text == "include") {
            add_inspection_error(
                result,
                diagnostics,
                ShaderDiagnosticCode::include_not_supported,
                shader_id,
                path,
                tokens[index].line,
                "#include is disabled until a confined, depth-bounded preprocessor exists"
            );
        }
        if (tokens[index].text == "void"
            && tokens[index + 1U].text == "main"
            && tokens[index + 2U].text == "(") {
            has_main = true;
        }
    }
    if (!has_main) {
        add_inspection_error(
            result,
            diagnostics,
            ShaderDiagnosticCode::missing_entry_point,
            shader_id,
            path,
            0U,
            "shader stage does not declare void main(...)"
        );
    }

    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (tokens[index].text != "uniform") {
            continue;
        }
        std::size_t cursor = index + 1U;
        while (cursor < tokens.size()
            && uniform_qualifier(tokens[cursor].text)) {
            ++cursor;
        }
        if (cursor >= tokens.size()
            || tokens[cursor].kind != TokenKind::identifier) {
            diagnostics.add(
                ShaderDiagnosticSeverity::warning,
                ShaderDiagnosticCode::malformed_uniform,
                shader_id,
                path,
                tokens[index].line,
                "uniform declaration has no readable type"
            );
            continue;
        }
        const auto type_name = tokens[cursor].text;
        const auto type = uniform_type(type_name);
        ++cursor;
        if (cursor < tokens.size() && tokens[cursor].text == "{") {
            // Uniform blocks require real backend reflection. They are not
            // exposed through the scalar Psych-compatible setter facade.
            continue;
        }
        bool parsed_name{};
        while (cursor < tokens.size() && tokens[cursor].text != ";") {
            if (tokens[cursor].text == ",") {
                ++cursor;
                continue;
            }
            if (tokens[cursor].kind != TokenKind::identifier) {
                ++cursor;
                continue;
            }
            ShaderUniformDeclaration declaration;
            declaration.name = std::string(tokens[cursor].text);
            declaration.type = type;
            declaration.used_by_vertex = stage == ShaderStage::vertex;
            declaration.used_by_fragment = stage == ShaderStage::fragment;
            const auto declaration_line = tokens[cursor].line;
            ++cursor;
            if (cursor < tokens.size() && tokens[cursor].text == "[") {
                ++cursor;
                declaration.array_elements = 0U;
                if (cursor < tokens.size()
                    && tokens[cursor].kind == TokenKind::number) {
                    std::size_t elements{};
                    const auto* begin = tokens[cursor].text.data();
                    const auto* end = begin + tokens[cursor].text.size();
                    const auto conversion = std::from_chars(
                        begin,
                        end,
                        elements
                    );
                    if (conversion.ec == std::errc{} && conversion.ptr == end) {
                        declaration.array_elements = elements;
                    }
                    ++cursor;
                }
                while (cursor < tokens.size() && tokens[cursor].text != "]"
                    && tokens[cursor].text != ";") {
                    ++cursor;
                }
                if (cursor < tokens.size() && tokens[cursor].text == "]") {
                    ++cursor;
                }
            }
            result.uniforms.push_back(std::move(declaration));
            parsed_name = true;
            if (type == ShaderUniformType::unknown) {
                diagnostics.add(
                    ShaderDiagnosticSeverity::warning,
                    ShaderDiagnosticCode::malformed_uniform,
                    shader_id,
                    path,
                    declaration_line,
                    "uniform type '" + std::string(type_name)
                        + "' requires backend reflection before it can be edited"
                );
            }
            while (cursor < tokens.size() && tokens[cursor].text != ","
                && tokens[cursor].text != ";") {
                ++cursor;
            }
        }
        if (!parsed_name) {
            diagnostics.add(
                ShaderDiagnosticSeverity::warning,
                ShaderDiagnosticCode::malformed_uniform,
                shader_id,
                path,
                tokens[index].line,
                "uniform declaration has no editable name"
            );
        }
        index = cursor < tokens.size() ? cursor : tokens.size() - 1U;
    }

    if (result.uniforms.size() > limits.maximum_uniforms) {
        add_inspection_error(
            result,
            diagnostics,
            ShaderDiagnosticCode::uniform_limit_exceeded,
            shader_id,
            path,
            0U,
            "shader stage exceeds the configured uniform declaration limit"
        );
    }
    const auto samplers = static_cast<std::size_t>(std::count_if(
        result.uniforms.begin(),
        result.uniforms.end(),
        [](const ShaderUniformDeclaration& uniform) {
            return uniform.type == ShaderUniformType::sampler2d
                || uniform.type == ShaderUniformType::sampler_other;
        }
    ));
    if (samplers > limits.maximum_samplers) {
        add_inspection_error(
            result,
            diagnostics,
            ShaderDiagnosticCode::sampler_limit_exceeded,
            shader_id,
            path,
            0U,
            "shader stage exceeds the configured sampler limit"
        );
    }
    return result;
}

[[nodiscard]] std::optional<std::string> read_source(
    const ShaderSourceLocation& location,
    const ShaderLimits& limits,
    const std::string& shader_id,
    DiagnosticSink& diagnostics
) {
    std::error_code error;
    const auto canonical_root = normalized_absolute(location.root, error);
    if (error || canonical_root.empty()
        || !std::filesystem::is_directory(canonical_root, error)) {
        diagnostics.add(
            ShaderDiagnosticSeverity::error,
            ShaderDiagnosticCode::invalid_root,
            shader_id,
            location.root,
            0U,
            "shader root is no longer an accessible directory"
        );
        return std::nullopt;
    }
    const auto canonical_file = normalized_absolute(
        location.physical_path,
        error
    );
    if (error || canonical_file.empty() || !path_is_within(
        canonical_root,
        canonical_file
    )) {
        diagnostics.add(
            ShaderDiagnosticSeverity::error,
            ShaderDiagnosticCode::path_escape,
            shader_id,
            location.physical_path,
            0U,
            "shader file resolves outside its declared root"
        );
        return std::nullopt;
    }
    const auto size = std::filesystem::file_size(canonical_file, error);
    if (error) {
        diagnostics.add(
            ShaderDiagnosticSeverity::error,
            ShaderDiagnosticCode::io_failure,
            shader_id,
            canonical_file,
            0U,
            "could not inspect shader source size"
        );
        return std::nullopt;
    }
    if (size > limits.maximum_source_bytes
        || size > static_cast<std::uintmax_t>(
            (std::numeric_limits<std::size_t>::max)()
        )
        || size > static_cast<std::uintmax_t>(
            (std::numeric_limits<std::streamsize>::max)()
        )) {
        diagnostics.add(
            ShaderDiagnosticSeverity::error,
            ShaderDiagnosticCode::source_too_large,
            shader_id,
            canonical_file,
            0U,
            "shader source exceeds the configured byte limit"
        );
        return std::nullopt;
    }
    if (size != location.size_bytes) {
        diagnostics.add(
            ShaderDiagnosticSeverity::warning,
            ShaderDiagnosticCode::source_changed,
            shader_id,
            canonical_file,
            0U,
            "shader source changed after catalog discovery; it was revalidated"
        );
    }
    std::ifstream input(canonical_file, std::ios::binary);
    if (!input) {
        diagnostics.add(
            ShaderDiagnosticSeverity::error,
            ShaderDiagnosticCode::io_failure,
            shader_id,
            canonical_file,
            0U,
            "could not open shader source"
        );
        return std::nullopt;
    }
    std::string source(static_cast<std::size_t>(size), '\0');
    if (!source.empty()) {
        input.read(source.data(), static_cast<std::streamsize>(source.size()));
    }
    if (!input || input.peek() != std::char_traits<char>::eof()) {
        diagnostics.add(
            ShaderDiagnosticSeverity::error,
            ShaderDiagnosticCode::io_failure,
            shader_id,
            canonical_file,
            0U,
            "shader source changed or failed while it was being read"
        );
        return std::nullopt;
    }
    return source;
}

void fingerprint_append(
    std::uint64_t& fingerprint,
    const std::string_view value
) noexcept {
    for (const char byte : value) {
        fingerprint ^= static_cast<unsigned char>(byte);
        fingerprint *= fnv_prime;
    }
    fingerprint ^= 0xFFU;
    fingerprint *= fnv_prime;
}

struct ProgramInspection {
    ShaderProgramSource program;
    std::vector<ShaderDiagnostic> diagnostics;
    bool valid{};
};

[[nodiscard]] ProgramInspection inspect_program(
    const std::string& shader_id,
    const ShaderSourceLocation& fragment,
    const std::optional<ShaderSourceLocation>& vertex,
    const ShaderLimits& limits
) {
    ProgramInspection result;
    result.program.id = shader_id;
    DiagnosticSink diagnostics(result.diagnostics, limits.maximum_diagnostics);
    auto fragment_source = read_source(
        fragment,
        limits,
        shader_id,
        diagnostics
    );
    if (!fragment_source.has_value()) {
        return result;
    }
    std::optional<std::string> vertex_source;
    if (vertex.has_value()) {
        vertex_source = read_source(
            *vertex,
            limits,
            shader_id,
            diagnostics
        );
        if (!vertex_source.has_value()) {
            return result;
        }
    }

    auto fragment_inspection = inspect_stage(
        *fragment_source,
        ShaderStage::fragment,
        shader_id,
        fragment.physical_path,
        limits,
        diagnostics
    );
    StageInspection vertex_inspection;
    if (vertex_source.has_value()) {
        vertex_inspection = inspect_stage(
            *vertex_source,
            ShaderStage::vertex,
            shader_id,
            vertex->physical_path,
            limits,
            diagnostics
        );
    }

    std::map<std::string, ShaderUniformDeclaration> merged;
    // Semantic validity must not depend on whether the caller elected to
    // retain diagnostic text. In particular, a zero/full diagnostic budget
    // must never turn a conflicting program into a valid one.
    bool program_has_error{};
    const auto merge_uniforms = [&](const auto& source) {
        for (const auto& uniform : source) {
            const auto [position, inserted] = merged.emplace(
                uniform.name,
                uniform
            );
            if (inserted) {
                continue;
            }
            auto& existing = position->second;
            if (existing.type != uniform.type
                || existing.array_elements != uniform.array_elements) {
                program_has_error = true;
                diagnostics.add(
                    ShaderDiagnosticSeverity::error,
                    ShaderDiagnosticCode::uniform_type_conflict,
                    shader_id,
                    fragment.physical_path,
                    0U,
                    "uniform '" + uniform.name
                        + "' has conflicting declarations between stages"
                );
                continue;
            }
            existing.used_by_vertex = existing.used_by_vertex
                || uniform.used_by_vertex;
            existing.used_by_fragment = existing.used_by_fragment
                || uniform.used_by_fragment;
        }
    };
    merge_uniforms(fragment_inspection.uniforms);
    merge_uniforms(vertex_inspection.uniforms);
    if (merged.size() > limits.maximum_uniforms) {
        program_has_error = true;
        diagnostics.add(
            ShaderDiagnosticSeverity::error,
            ShaderDiagnosticCode::uniform_limit_exceeded,
            shader_id,
            fragment.physical_path,
            0U,
            "combined shader program exceeds the configured uniform limit"
        );
    }
    const auto combined_samplers = static_cast<std::size_t>(std::count_if(
        merged.begin(),
        merged.end(),
        [](const auto& item) {
            return item.second.type == ShaderUniformType::sampler2d
                || item.second.type == ShaderUniformType::sampler_other;
        }
    ));
    if (combined_samplers > limits.maximum_samplers) {
        program_has_error = true;
        diagnostics.add(
            ShaderDiagnosticSeverity::error,
            ShaderDiagnosticCode::sampler_limit_exceeded,
            shader_id,
            fragment.physical_path,
            0U,
            "combined shader program exceeds the configured sampler limit"
        );
    }

    result.program.uniforms.reserve(merged.size());
    for (auto& [name, declaration] : merged) {
        static_cast<void>(name);
        result.program.uniforms.push_back(std::move(declaration));
    }
    result.program.fragment = std::move(*fragment_source);
    result.program.vertex = std::move(vertex_source);
    result.program.source_fingerprint = fnv_offset;
    fingerprint_append(result.program.source_fingerprint, shader_id);
    fingerprint_append(
        result.program.source_fingerprint,
        result.program.fragment
    );
    if (result.program.vertex.has_value()) {
        fingerprint_append(
            result.program.source_fingerprint,
            *result.program.vertex
        );
    }
    result.valid = !fragment_inspection.has_error
        && !vertex_inspection.has_error
        && !program_has_error
        && std::none_of(
            result.diagnostics.begin(),
            result.diagnostics.end(),
            [](const ShaderDiagnostic& diagnostic) {
                return diagnostic.severity == ShaderDiagnosticSeverity::error;
            }
        );
    return result;
}

}  // namespace

ShaderCatalog ShaderCatalog::scan(const ShaderCatalogOptions& options) {
    ShaderCatalog catalog;
    catalog.limits_ = options.limits;
    DiagnosticSink diagnostics(
        catalog.diagnostics_,
        options.limits.maximum_diagnostics
    );
    std::map<std::string, Candidate> fragments;
    std::map<std::string, Candidate> vertices;
    std::size_t files_seen{};
    bool stop_scanning{};

    if (options.roots.empty()) {
        diagnostics.add(
            ShaderDiagnosticSeverity::warning,
            ShaderDiagnosticCode::invalid_configuration,
            {},
            {},
            0U,
            "shader catalog has no search roots"
        );
    }

    for (std::size_t root_order = 0U;
        root_order < options.roots.size() && !stop_scanning;
        ++root_order) {
        const auto& root = options.roots[root_order];
        if (root.id.empty() || root.id.find('\0') != std::string::npos) {
            diagnostics.add(
                ShaderDiagnosticSeverity::error,
                ShaderDiagnosticCode::invalid_root,
                {},
                root.root,
                0U,
                "shader root id is empty or contains an embedded NUL"
            );
            continue;
        }
        std::error_code error;
        const auto canonical_root = normalized_absolute(root.root, error);
        if (error || canonical_root.empty()
            || !std::filesystem::is_directory(canonical_root, error)) {
            diagnostics.add(
                ShaderDiagnosticSeverity::error,
                ShaderDiagnosticCode::invalid_root,
                {},
                root.root,
                0U,
                "shader root is not an accessible directory"
            );
            continue;
        }

        for (std::size_t directory_order = 0U;
            directory_order < options.search_directories.size()
                && !stop_scanning;
            ++directory_order) {
            const auto& search_directory =
                options.search_directories[directory_order];
            if (!safe_relative_directory(search_directory)) {
                diagnostics.add(
                    ShaderDiagnosticSeverity::error,
                    ShaderDiagnosticCode::invalid_search_directory,
                    {},
                    search_directory,
                    0U,
                    "shader search directory must be relative and cannot contain '.' or '..'"
                );
                continue;
            }
            const auto physical_directory = canonical_root / search_directory;
            error.clear();
            if (!std::filesystem::is_directory(physical_directory, error)) {
                continue;
            }
            std::filesystem::recursive_directory_iterator iterator(
                physical_directory,
                std::filesystem::directory_options::skip_permission_denied,
                error
            );
            const std::filesystem::recursive_directory_iterator end;
            if (error) {
                diagnostics.add(
                    ShaderDiagnosticSeverity::warning,
                    ShaderDiagnosticCode::io_failure,
                    {},
                    physical_directory,
                    0U,
                    "could not enumerate shader directory"
                );
                continue;
            }
            while (iterator != end && !stop_scanning) {
                const auto entry_path = iterator->path();
                error.clear();
                const bool directory = iterator->is_directory(error);
                if (directory) {
                    error.clear();
                    const bool symlink = iterator->is_symlink(error);
                    if (symlink
                        || static_cast<std::size_t>(iterator.depth())
                            >= options.limits.maximum_directory_depth) {
                        iterator.disable_recursion_pending();
                    }
                    iterator.increment(error);
                    if (error) {
                        error.clear();
                    }
                    continue;
                }
                error.clear();
                if (!iterator->is_regular_file(error)) {
                    iterator.increment(error);
                    if (error) {
                        error.clear();
                    }
                    continue;
                }
                ShaderStage stage{};
                if (!is_shader_extension(entry_path, stage)) {
                    iterator.increment(error);
                    if (error) {
                        error.clear();
                    }
                    continue;
                }
                if (files_seen >= options.limits.maximum_files) {
                    catalog.truncated_ = true;
                    stop_scanning = true;
                    diagnostics.add(
                        ShaderDiagnosticSeverity::error,
                        ShaderDiagnosticCode::file_limit_exceeded,
                        {},
                        physical_directory,
                        0U,
                        "shader scan stopped at the configured file limit"
                    );
                    break;
                }
                ++files_seen;

                auto relative = entry_path.lexically_relative(
                    physical_directory
                );
                relative.replace_extension();
                const auto shader_id = canonical_shader_id(
                    path_utf8(relative),
                    options.limits.maximum_identifier_bytes
                );
                if (!shader_id.has_value()) {
                    diagnostics.add(
                        ShaderDiagnosticSeverity::error,
                        ShaderDiagnosticCode::invalid_identifier,
                        {},
                        entry_path,
                        0U,
                        "shader filename cannot be represented as a safe identifier"
                    );
                    iterator.increment(error);
                    if (error) {
                        error.clear();
                    }
                    continue;
                }
                const auto canonical_file = normalized_absolute(
                    entry_path,
                    error
                );
                if (error || canonical_file.empty()
                    || !path_is_within(canonical_root, canonical_file)) {
                    diagnostics.add(
                        ShaderDiagnosticSeverity::error,
                        ShaderDiagnosticCode::path_escape,
                        *shader_id,
                        entry_path,
                        0U,
                        "shader symlink resolves outside its declared root"
                    );
                    iterator.increment(error);
                    if (error) {
                        error.clear();
                    }
                    continue;
                }
                const auto size = std::filesystem::file_size(
                    canonical_file,
                    error
                );
                if (error) {
                    diagnostics.add(
                        ShaderDiagnosticSeverity::warning,
                        ShaderDiagnosticCode::io_failure,
                        *shader_id,
                        canonical_file,
                        0U,
                        "could not inspect shader file"
                    );
                    iterator.increment(error);
                    if (error) {
                        error.clear();
                    }
                    continue;
                }
                auto virtual_path = search_directory / relative;
                virtual_path.replace_extension(
                    stage == ShaderStage::fragment ? ".frag" : ".vert"
                );
                Candidate candidate{
                    ShaderSourceLocation{
                        root.id,
                        canonical_root,
                        canonical_file,
                        path_utf8(virtual_path),
                        root.priority,
                        root_order,
                        size,
                    },
                    directory_order,
                };
                consider_candidate(
                    stage == ShaderStage::fragment ? fragments : vertices,
                    *shader_id,
                    std::move(candidate)
                );
                iterator.increment(error);
                if (error) {
                    error.clear();
                }
            }
        }
    }

    catalog.entries_.reserve((std::min)(
        fragments.size(),
        options.limits.maximum_entries
    ));
    for (const auto& [shader_id, fragment] : fragments) {
        if (catalog.entries_.size() >= options.limits.maximum_entries) {
            catalog.truncated_ = true;
            diagnostics.add(
                ShaderDiagnosticSeverity::error,
                ShaderDiagnosticCode::entry_limit_exceeded,
                {},
                {},
                0U,
                "shader catalog stopped at the configured entry limit"
            );
            break;
        }
        std::optional<ShaderSourceLocation> vertex;
        if (const auto found = vertices.find(shader_id);
            found != vertices.end()) {
            vertex = found->second.location;
        }
        const auto inspection = inspect_program(
            shader_id,
            fragment.location,
            vertex,
            options.limits
        );
        for (const auto& diagnostic : inspection.diagnostics) {
            diagnostics.add(
                diagnostic.severity,
                diagnostic.code,
                diagnostic.shader_id,
                diagnostic.path,
                diagnostic.line,
                diagnostic.message
            );
        }
        ShaderCatalogEntry entry;
        entry.id = shader_id;
        entry.fragment = fragment.location;
        entry.vertex = std::move(vertex);
        entry.valid = inspection.valid;
        if (inspection.valid) {
            entry.uniforms = inspection.program.uniforms;
            entry.source_fingerprint = inspection.program.source_fingerprint;
        }
        catalog.entries_.push_back(std::move(entry));
    }
    return catalog;
}

std::span<const ShaderCatalogEntry> ShaderCatalog::entries() const noexcept {
    return entries_;
}

std::span<const ShaderDiagnostic> ShaderCatalog::diagnostics() const noexcept {
    return diagnostics_;
}

bool ShaderCatalog::truncated() const noexcept {
    return truncated_;
}

const ShaderCatalogEntry* ShaderCatalog::find(
    const std::string_view shader_id
) const noexcept {
    const auto canonical = canonical_shader_id(
        shader_id,
        limits_.maximum_identifier_bytes
    );
    if (!canonical.has_value()) {
        return nullptr;
    }
    const auto found = std::lower_bound(
        entries_.begin(),
        entries_.end(),
        *canonical,
        [](const ShaderCatalogEntry& entry, const std::string_view id) {
            return entry.id < id;
        }
    );
    return found != entries_.end() && found->id == *canonical
        ? std::addressof(*found)
        : nullptr;
}

ShaderLoadResult ShaderCatalog::load(const std::string_view shader_id) const {
    ShaderLoadResult result;
    const auto* entry = find(shader_id);
    if (entry == nullptr) {
        DiagnosticSink diagnostics(result.diagnostics, limits_.maximum_diagnostics);
        diagnostics.add(
            ShaderDiagnosticSeverity::error,
            ShaderDiagnosticCode::shader_not_found,
            std::string(shader_id),
            {},
            0U,
            "shader identifier was not found in the catalog"
        );
        return result;
    }
    auto inspection = inspect_program(
        entry->id,
        entry->fragment,
        entry->vertex,
        limits_
    );
    result.diagnostics = std::move(inspection.diagnostics);
    if (!inspection.valid) {
        return result;
    }
    if (entry->source_fingerprint != 0U
        && entry->source_fingerprint
            != inspection.program.source_fingerprint) {
        DiagnosticSink diagnostics(result.diagnostics, limits_.maximum_diagnostics);
        diagnostics.add(
            ShaderDiagnosticSeverity::warning,
            ShaderDiagnosticCode::source_changed,
            entry->id,
            entry->fragment.physical_path,
            0U,
            "shader content changed after discovery; the new source was revalidated"
        );
    }
    result.program = std::move(inspection.program);
    return result;
}

ShaderRuntimeCapability current_shader_runtime_capability() noexcept {
    return {
        "SDL_Renderer compatibility shaders",
        true,
        false,
        false,
        true,
        "Validated shaders can be bound from Lua and common scalar color/alpha uniforms are emulated; arbitrary GLSL execution still requires a programmable GPU backend.",
    };
}

std::string_view to_string(const ShaderStage stage) noexcept {
    switch (stage) {
    case ShaderStage::vertex:
        return "vertex";
    case ShaderStage::fragment:
        return "fragment";
    }
    return "unknown";
}

std::string_view to_string(const ShaderUniformType type) noexcept {
    switch (type) {
    case ShaderUniformType::unknown:
        return "unknown";
    case ShaderUniformType::boolean:
        return "bool";
    case ShaderUniformType::signed_integer:
        return "int";
    case ShaderUniformType::unsigned_integer:
        return "uint";
    case ShaderUniformType::floating_point:
        return "float";
    case ShaderUniformType::vec2:
        return "vec2";
    case ShaderUniformType::vec3:
        return "vec3";
    case ShaderUniformType::vec4:
        return "vec4";
    case ShaderUniformType::ivec2:
        return "ivec2";
    case ShaderUniformType::ivec3:
        return "ivec3";
    case ShaderUniformType::ivec4:
        return "ivec4";
    case ShaderUniformType::uvec2:
        return "uvec2";
    case ShaderUniformType::uvec3:
        return "uvec3";
    case ShaderUniformType::uvec4:
        return "uvec4";
    case ShaderUniformType::mat2:
        return "mat2";
    case ShaderUniformType::mat3:
        return "mat3";
    case ShaderUniformType::mat4:
        return "mat4";
    case ShaderUniformType::sampler2d:
        return "sampler2D";
    case ShaderUniformType::sampler_other:
        return "sampler";
    }
    return "unknown";
}

std::string_view to_string(
    const ShaderDiagnosticSeverity severity
) noexcept {
    switch (severity) {
    case ShaderDiagnosticSeverity::information:
        return "information";
    case ShaderDiagnosticSeverity::warning:
        return "warning";
    case ShaderDiagnosticSeverity::error:
        return "error";
    }
    return "unknown";
}

std::string_view to_string(const ShaderDiagnosticCode code) noexcept {
    switch (code) {
    case ShaderDiagnosticCode::invalid_configuration:
        return "invalid_configuration";
    case ShaderDiagnosticCode::invalid_identifier:
        return "invalid_identifier";
    case ShaderDiagnosticCode::invalid_root:
        return "invalid_root";
    case ShaderDiagnosticCode::invalid_search_directory:
        return "invalid_search_directory";
    case ShaderDiagnosticCode::io_failure:
        return "io_failure";
    case ShaderDiagnosticCode::path_escape:
        return "path_escape";
    case ShaderDiagnosticCode::source_too_large:
        return "source_too_large";
    case ShaderDiagnosticCode::embedded_nul:
        return "embedded_nul";
    case ShaderDiagnosticCode::include_not_supported:
        return "include_not_supported";
    case ShaderDiagnosticCode::missing_entry_point:
        return "missing_entry_point";
    case ShaderDiagnosticCode::malformed_uniform:
        return "malformed_uniform";
    case ShaderDiagnosticCode::uniform_type_conflict:
        return "uniform_type_conflict";
    case ShaderDiagnosticCode::uniform_limit_exceeded:
        return "uniform_limit_exceeded";
    case ShaderDiagnosticCode::sampler_limit_exceeded:
        return "sampler_limit_exceeded";
    case ShaderDiagnosticCode::entry_limit_exceeded:
        return "entry_limit_exceeded";
    case ShaderDiagnosticCode::file_limit_exceeded:
        return "file_limit_exceeded";
    case ShaderDiagnosticCode::source_changed:
        return "source_changed";
    case ShaderDiagnosticCode::shader_not_found:
        return "shader_not_found";
    }
    return "unknown";
}

}  // namespace pulseforge
