from pathlib import Path


def replace_once(path, old, new, label):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly 1 match, got {count}")
    p.write_text(text.replace(old, new, 1))


# Shared deterministic presentation LOD for genuinely gigantic charts.
p = Path("include/pulseforge/dense_chart_routing.hpp")
text = p.read_text()
anchor = "\n}  // namespace pulseforge\n"
if text.count(anchor) != 1:
    raise SystemExit("dense routing namespace anchor mismatch")
addition = r'''

// Charts in the multi-million-note class need a second, independent visual
// quality governor. Note/PFC streaming remains exact; only presentation-only
// stage effects are reduced. This is deliberately keyed to logical chart size
// so a 33M/50M chart starts in a sane GPU/draw-call state before the first FPS
// sample exists, then tightens further only if frame pacing still collapses.
inline constexpr std::uint64_t extreme_chart_visual_lod_notes = 5'000'000ULL;
inline constexpr std::uint64_t massive_chart_visual_lod_notes = 25'000'000ULL;

struct DenseChartVisualLod final {
    std::size_t wavy_segment_cap{64U};
    std::size_t wiggle_segment_cap{24U};
    bool allow_cpu_heavy_texture_effects{true};
};

[[nodiscard]] inline DenseChartVisualLod dense_chart_visual_lod(
    const std::uint64_t logical_notes,
    const double smoothed_fps,
    const double smoothed_frame_ms
) noexcept {
    DenseChartVisualLod result;
    if (logical_notes < extreme_chart_visual_lod_notes) {
        return result;
    }

    if (logical_notes >= massive_chart_visual_lod_notes) {
        result.wavy_segment_cap = 24U;
        result.wiggle_segment_cap = 12U;
        result.allow_cpu_heavy_texture_effects = false;
    } else {
        result.wavy_segment_cap = 40U;
        result.wiggle_segment_cap = 18U;
    }

    const bool has_fps = std::isfinite(smoothed_fps) && smoothed_fps > 0.0;
    const bool has_frame = std::isfinite(smoothed_frame_ms)
        && smoothed_frame_ms > 0.0;
    if (!has_fps && !has_frame) {
        return result;
    }

    const auto slower_than = [&](const double fps, const double frame_ms) {
        return (has_fps && smoothed_fps < fps)
            || (has_frame && smoothed_frame_ms > frame_ms);
    };
    if (slower_than(30.0, 33.34)) {
        result.wavy_segment_cap = 8U;
        result.wiggle_segment_cap = 8U;
        result.allow_cpu_heavy_texture_effects = false;
    } else if (slower_than(45.0, 22.23)) {
        result.wavy_segment_cap = std::min<std::size_t>(result.wavy_segment_cap, 12U);
        result.wiggle_segment_cap = 8U;
        result.allow_cpu_heavy_texture_effects = false;
    } else if (slower_than(55.0, 18.19)) {
        result.wavy_segment_cap = std::min<std::size_t>(result.wavy_segment_cap, 16U);
        result.wiggle_segment_cap = std::min<std::size_t>(result.wiggle_segment_cap, 10U);
        result.allow_cpu_heavy_texture_effects = false;
    } else if (slower_than(59.5, 16.81)) {
        result.wavy_segment_cap = std::min<std::size_t>(result.wavy_segment_cap, 24U);
        result.wiggle_segment_cap = std::min<std::size_t>(result.wiggle_segment_cap, 12U);
        result.allow_cpu_heavy_texture_effects = false;
    }
    return result;
}
'''
p.write_text(text.replace(anchor, addition + anchor, 1))

# Extreme interactive cache hits repair PVD1 instead of dropping to exact PFC visits.
replace_once(
    "include/pulseforge/streaming_chart_importer.hpp",
    '''    // Offline rendering benefits from the multi-resolution visual index on
    // every frame. When true, a missing/corrupt PVD1 is rebuilt during render
    // preparation and its size is committed to the manifest. Interactive play
    // leaves this false and falls back immediately to exact PFC1 visits.
    bool require_visual_density{false};
''',
    '''    // Offline rendering benefits from the multi-resolution visual index on
    // every frame. When true, a missing/corrupt PVD1 is rebuilt during render
    // preparation and its size is committed to the manifest.
    bool require_visual_density{false};

    // Interactive charts at or above this logical-note count also repair PVD1
    // automatically. Zero disables the automatic policy. This is a performance
    // routing threshold, never a chart-validity or note-count limit.
    std::uint64_t auto_require_visual_density_logical_notes{5'000'000ULL};
''',
    "streaming cache options PVD policy",
)
replace_once(
    "src/io/streaming_chart_importer.cpp",
    "        if (!visual_valid && options.require_visual_density) {\n",
    '''        const bool visual_density_required =
            options.require_visual_density
            || (options.auto_require_visual_density_logical_notes != 0U
                && reader->logical_note_count()
                    >= options.auto_require_visual_density_logical_notes);
        if (!visual_valid && visual_density_required) {
''',
    "cache-hit automatic PVD rebuild",
)

# RuntimeScene effect LOD surface/state.
replace_once(
    "src/app/runtime_scene.hpp",
    '''    void begin_note_skin_profile_frame(bool enabled) noexcept;
    [[nodiscard]] RuntimeNoteSkinProfileStats note_skin_profile_stats()
        const noexcept;
''',
    '''    void begin_note_skin_profile_frame(bool enabled) noexcept;
    [[nodiscard]] RuntimeNoteSkinProfileStats note_skin_profile_stats()
        const noexcept;

    // Presentation-only LOD for extreme charts. It never changes chart time,
    // note positions, input, scoring, replay state or Lua-visible properties.
    void set_effect_lod(
        std::size_t wavy_segment_cap,
        std::size_t wiggle_segment_cap,
        bool allow_cpu_heavy_texture_effects
    ) noexcept;
''',
    "RuntimeScene public effect LOD API",
)
replace_once(
    "src/app/runtime_scene.cpp",
    '''    std::uint64_t cpu_color_variant_bytes{};
    std::uint64_t cpu_color_pixels_this_frame{};
    std::vector<RuntimeSceneDiagnostic> diagnostics;
''',
    '''    std::uint64_t cpu_color_variant_bytes{};
    std::uint64_t cpu_color_pixels_this_frame{};
    std::size_t wavy_segment_cap{psych_wavy_max_segments};
    std::size_t wiggle_segment_cap{psych_wiggle_max_segments};
    bool allow_cpu_heavy_texture_effects{true};
    std::vector<RuntimeSceneDiagnostic> diagnostics;
''',
    "RuntimeScene effect LOD state",
)
replace_once(
    "src/app/runtime_scene.cpp",
    '''        const auto segment_count = psych_wavy_segment_count(
            sprite.wavy_effect,
            destination.h
        );
        if (segment_count == 0U
            || segment_count > psych_wavy_max_segments) {
''',
    '''        const auto requested_segment_count = psych_wavy_segment_count(
            sprite.wavy_effect,
            destination.h
        );
        const auto segment_count = std::min(requested_segment_count, wavy_segment_cap);
        if (segment_count == 0U
            || segment_count > psych_wavy_max_segments) {
''',
    "wavy effect segment LOD",
)
replace_once(
    "src/app/runtime_scene.cpp",
    '''        const auto segment_count = psych_wiggle_segment_count(
            shader_state,
            destination.w,
            destination.h
        );
        if (segment_count == 0U
''',
    '''        const auto requested_segment_count = psych_wiggle_segment_count(
            shader_state,
            destination.w,
            destination.h
        );
        const auto segment_count = std::min(requested_segment_count, wiggle_segment_cap);
        if (segment_count == 0U
''',
    "wiggle effect segment LOD",
)
replace_once(
    "src/app/runtime_scene.cpp",
    '''            const std::uint64_t texture_pixel_limit =
                shader_kind == PsychShaderCompatKind::gaussian_blur
''',
    '''            if (!allow_cpu_heavy_texture_effects
                && (shader_kind == PsychShaderCompatKind::mosaic
                    || shader_kind == PsychShaderCompatKind::gaussian_blur)) {
                return nullptr;
            }
            const std::uint64_t texture_pixel_limit =
                shader_kind == PsychShaderCompatKind::gaussian_blur
''',
    "CPU-heavy texture effect LOD",
)
replace_once(
    "src/app/runtime_scene.cpp",
    "void RuntimeScene::begin_note_skin_profile_frame(const bool enabled) noexcept {\n",
    '''void RuntimeScene::set_effect_lod(
    const std::size_t wavy_cap,
    const std::size_t wiggle_cap,
    const bool allow_cpu_heavy_texture_effects
) noexcept {
    if (implementation_ == nullptr) {
        return;
    }
    implementation_->wavy_segment_cap = std::clamp<std::size_t>(
        wavy_cap, psych_wavy_min_segments, psych_wavy_max_segments
    );
    implementation_->wiggle_segment_cap = std::clamp<std::size_t>(
        wiggle_cap, psych_wiggle_min_segments, psych_wiggle_max_segments
    );
    implementation_->allow_cpu_heavy_texture_effects = allow_cpu_heavy_texture_effects;
}

void RuntimeScene::begin_note_skin_profile_frame(const bool enabled) noexcept {
''',
    "RuntimeScene effect LOD implementation",
)

# Baseline effect LOD exists before frame one, then responds to measured pacing.
replace_once(
    "src/app/application.cpp",
    '''        scene_ = std::make_unique<detail::RuntimeScene>(
            renderer_,
            *chart_,
            scene_roots,
            scene_limits
        );
''',
    '''        scene_ = std::make_unique<detail::RuntimeScene>(
            renderer_,
            *chart_,
            scene_roots,
            scene_limits
        );
        if (scene_ != nullptr) {
            const auto logical_notes = streaming_reader_.has_value()
                ? streaming_reader_->logical_note_count()
                : static_cast<std::uint64_t>(chart_->notes.size());
            const auto effect_lod = dense_chart_visual_lod(logical_notes, 0.0, 0.0);
            scene_->set_effect_lod(
                effect_lod.wavy_segment_cap,
                effect_lod.wiggle_segment_cap,
                effect_lod.allow_cpu_heavy_texture_effects
            );
        }
''',
    "initial extreme-chart effect LOD",
)
replace_once(
    "src/app/application.cpp",
    '''        adaptive_scroll_.update(
            static_cast<double>(elapsed),
            smoothed_fps_,
            smoothed_frame_ms_,
            rendered_notes_,
            options_.settings.performance.max_visible_notes,
            !options_.offline_render.enabled && !options_.smoke_test
        );
''',
    '''        adaptive_scroll_.update(
            static_cast<double>(elapsed),
            smoothed_fps_,
            smoothed_frame_ms_,
            rendered_notes_,
            options_.settings.performance.max_visible_notes,
            !options_.offline_render.enabled && !options_.smoke_test
        );
        if (scene_ != nullptr) {
            const auto logical_notes = streaming_reader_.has_value()
                ? streaming_reader_->logical_note_count()
                : (chart_.has_value()
                    ? static_cast<std::uint64_t>(chart_->notes.size())
                    : 0ULL);
            const auto effect_lod = dense_chart_visual_lod(
                logical_notes, smoothed_fps_, smoothed_frame_ms_
            );
            scene_->set_effect_lod(
                effect_lod.wavy_segment_cap,
                effect_lod.wiggle_segment_cap,
                effect_lod.allow_cpu_heavy_texture_effects
            );
        }
''',
    "adaptive extreme-chart effect LOD",
)

# Known-absent Lua note callbacks must not reserve thousands of callback slots.
replace_once(
    "src/script/lua_runtime.cpp",
    '''    [[nodiscard]] static Impl* from_state(lua_State* lua) noexcept {
        void* userdata = nullptr;
        (void)lua_getallocf(lua, &userdata);
        return static_cast<Impl*>(userdata);
    }
''',
    '''    [[nodiscard]] static Impl* from_state(lua_State* lua) noexcept {
        void* userdata = nullptr;
        (void)lua_getallocf(lua, &userdata);
        return static_cast<Impl*>(userdata);
    }

    [[nodiscard]] bool callback_available(const Callback callback) const noexcept {
        if (!script_loaded || state == nullptr) {
            return false;
        }
        const auto index = static_cast<std::size_t>(callback);
        return !disabled[index] && !absent[index];
    }
''',
    "Lua callback availability helper",
)

p = Path("src/script/lua_runtime.cpp")
text = p.read_text()
old_group = '''        case GameplayEventType::note_hit:
        case GameplayEventType::note_miss:
        case GameplayEventType::hold_tick:
        case GameplayEventType::hold_drop:
        case GameplayEventType::opponent_hit:
            plan(event.logical_occurrence_count);
            break;
'''
new_group = '''        case GameplayEventType::note_hit:
        case GameplayEventType::hold_tick:
            if (impl_->callback_available(Impl::Callback::good_note_hit)) {
                plan(event.logical_occurrence_count);
            }
            break;
        case GameplayEventType::note_miss:
        case GameplayEventType::hold_drop:
            if (impl_->callback_available(Impl::Callback::note_miss)) {
                plan(event.logical_occurrence_count);
            }
            break;
        case GameplayEventType::opponent_hit:
            if (impl_->callback_available(Impl::Callback::opponent_note_hit)) {
                plan(event.logical_occurrence_count);
            }
            break;
'''
count = text.count(old_group)
if count != 2:
    raise SystemExit(f"Lua note planning groups: expected 2, got {count}")
text = text.replace(old_group, new_group)
old_repeat = '''        const auto runs = static_cast<std::size_t>(
            std::min<std::uint64_t>(count, available)
        );
        for (std::size_t index = 0U; index < runs; ++index) {
            accumulate(report, callback(index));
        }
        event_callbacks_used += runs;
        if (count > runs) {
            add_skipped(count - static_cast<std::uint64_t>(runs));
            fanout_truncated = true;
        }
'''
new_repeat = '''        const auto runs = static_cast<std::size_t>(
            std::min<std::uint64_t>(count, available)
        );
        std::size_t executed = 0U;
        bool callback_unavailable = false;
        for (std::size_t index = 0U; index < runs; ++index) {
            const auto current = callback(index);
            accumulate(report, current);
            ++executed;
            if (current.status == LuaCallStatus::missing
                || current.status == LuaCallStatus::disabled) {
                callback_unavailable = true;
                break;
            }
        }
        event_callbacks_used += executed;
        if (count > executed) {
            add_skipped(count - static_cast<std::uint64_t>(executed));
            if (!callback_unavailable && count > runs) {
                fanout_truncated = true;
            }
        }
'''
count = text.count(old_repeat)
if count != 2:
    raise SystemExit(f"Lua repeat loops: expected 2, got {count}")
p.write_text(text.replace(old_repeat, new_repeat))

# Deterministic LOD regression coverage for Easy / 5M / 33M / 50M classes.
p = Path("tests/streaming_gameplay_test.cpp")
text = p.read_text()
anchor = "\n}  // namespace\n\nint main() {"
if text.count(anchor) != 1:
    raise SystemExit("streaming gameplay test namespace anchor mismatch")
test = r'''

void test_extreme_chart_visual_lod() {
    const auto easy = pulseforge::dense_chart_visual_lod(1'059'988ULL, 70.0, 14.2);
    require(easy.wavy_segment_cap == 64U && easy.wiggle_segment_cap == 24U
            && easy.allow_cpu_heavy_texture_effects,
        "one-million-note Easy gameplay keeps full stage quality");

    const auto end_of_world = pulseforge::dense_chart_visual_lod(5'000'000ULL, 70.0, 14.2);
    require(end_of_world.wavy_segment_cap == 40U
            && end_of_world.wiggle_segment_cap == 18U
            && end_of_world.allow_cpu_heavy_texture_effects,
        "five-million-note charts start with a mild presentation LOD");

    const auto overkill = pulseforge::dense_chart_visual_lod(33'000'000ULL, 70.0, 14.2);
    require(overkill.wavy_segment_cap == 24U && overkill.wiggle_segment_cap == 12U
            && !overkill.allow_cpu_heavy_texture_effects,
        "33-million-note Overkill starts with bounded expensive effects");

    const auto nullifier_collapse = pulseforge::dense_chart_visual_lod(50'000'000ULL, 7.0, 142.0);
    require(nullifier_collapse.wavy_segment_cap == 8U
            && nullifier_collapse.wiggle_segment_cap == 8U
            && !nullifier_collapse.allow_cpu_heavy_texture_effects,
        "50-million-note collapse drops presentation work to minimum LOD");

    const auto nullifier_recovered = pulseforge::dense_chart_visual_lod(50'000'000ULL, 60.0, 16.6);
    require(nullifier_recovered.wavy_segment_cap == 24U
            && nullifier_recovered.wiggle_segment_cap == 12U,
        "extreme chart effect quality recovers after stable 60 Hz pacing");
}
'''
text = text.replace(anchor, test + anchor, 1)
text = text.replace(
    "        test_dense_chart_streaming_routing();\n",
    "        test_dense_chart_streaming_routing();\n        test_extreme_chart_visual_lod();\n",
    1,
)
p.write_text(text)

# Existing missing-PVD test becomes an automatic-extreme-policy test at threshold 1.
replace_once(
    "tests/streaming_chart_importer_test.cpp",
    "    options.require_visual_density = true;\n    const auto lazy_visual = pulseforge::prepare_streaming_chart_cache(\n",
    '''    options.auto_require_visual_density_logical_notes = 1U;
    const auto lazy_visual = pulseforge::prepare_streaming_chart_cache(
''',
    "automatic PVD repair regression",
)

p = Path("CHANGELOG.md")
text = p.read_text()
marker = "### Fixed\n\n"
if marker not in text:
    raise SystemExit("CHANGELOG Fixed marker missing")
bullet = (
    "- Extreme-chart runtime now repairs missing PVD1 indexes automatically at "
    "5M+ logical notes, removes no-op Lua logical-note callback fan-out after "
    "callbacks are known absent, and dynamically reduces presentation-only "
    "wavy/wiggle/CPU-heavy stage effects when 5M-50M+ charts threaten the 60 FPS "
    "frame budget; note timing, judgment, scoring and chart validity are unchanged.\n"
)
p.write_text(text.replace(marker, marker + bullet, 1))
