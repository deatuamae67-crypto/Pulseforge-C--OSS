#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(rel: str, old: str, new: str) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one patch anchor, found {count}: {old[:120]!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def insert_before(rel: str, anchor: str, addition: str) -> None:
    replace_once(rel, anchor, addition + anchor)


# ---------------------------------------------------------------------------
# Native NoteTypes: these are immutable core definitions, not mod fallbacks.
# ---------------------------------------------------------------------------
replace_once(
    "src/core/note_types.cpp",
    '''constexpr std::array<std::string_view, 8U> builtin_ids{
    "normal",
    "Alt Animation",
    "GF Sing",
    "Hurt Note",
    "Hey!",
    "No Animation",
    "Cross Fade",
    "GF Cross Fade",
};''',
    '''constexpr std::array<std::string_view, 11U> builtin_ids{
    "normal",
    "Alt Animation",
    "GF Sing",
    "Hurt Note",
    "Hey!",
    "No Animation",
    "Cross Fade",
    "GF Cross Fade",
    "3rd Player",
    "5th Player",
    "the note",
};''',
)
replace_once(
    "src/core/note_types.cpp",
    '''    } else if (id == "GF Cross Fade") {
        result.animation.target = NoteAnimationTarget::girlfriend;
        result.cross_fade = NoteCrossFadeTarget::girlfriend;
    }
    return result;''',
    '''    } else if (id == "GF Cross Fade") {
        result.animation.target = NoteAnimationTarget::girlfriend;
        result.cross_fade = NoteCrossFadeTarget::girlfriend;
    } else if (id == "the note") {
        // Native PulseForge definition recovered from the Complete corpus.
        // The registry marks this built-in, so a mod cannot replace its core
        // semantics. The bundled animation hook may add presentation only.
        result.health.miss = 0.6;
        result.visual.texture_id = "BULLET";
        result.feedback.hitsound_enabled = true;
        result.feedback.hitsound_id = "gunshot";
    }
    // 3rd Player and 5th Player are intentionally native identity types. The
    // source corpus defines no extra semantics for them, so normal gameplay is
    // correct while their exact IDs remain first-class and immutable.
    return result;''',
)
replace_once(
    "include/pulseforge/editor_choices.hpp",
    '''    // These are engine-bundled, replaceable compatibility note-type IDs rather
    // than immutable NoteTypeRegistry built-ins. Keeping them in the default
    // editor catalogue makes them selectable even when a packaged platform
    // cannot synchronously scan the bundled assets (notably Android), while an
    // active mod can still provide the real definition for the same ID.
    std::vector<std::string> note_types{
        "3rd Player",
        "5th Player",
        "the note",
    };''',
    '''    // Native NoteTypes are injected from builtin_note_type_ids() by both
    // editor frontends. Filesystem discovery only contributes additional mod
    // types, so the core list is available even on packaged/mobile builds.
    std::vector<std::string> note_types;''',
)
replace_once(
    "tests/note_types_test.cpp",
    '''    require(ids.size() == 8U, "all editor-compatible built-ins are exposed");
    require(ids.front() == "normal", "normal is the canonical fallback");
    require(ids.back() == "GF Cross Fade", "JS cross-fade built-in is exposed");''',
    '''    require(ids.size() == 11U, "all editor-compatible built-ins are exposed");
    require(ids.front() == "normal", "normal is the canonical fallback");
    require(ids[7U] == "GF Cross Fade", "JS cross-fade built-in is exposed");
    require(ids[8U] == "3rd Player", "third-player native identity is exposed");
    require(ids[9U] == "5th Player", "fifth-player native identity is exposed");
    require(ids.back() == "the note", "Complete bullet note is a native built-in");''',
)
replace_once(
    "tests/note_types_test.cpp",
    '''    const auto gf_cross_fade = registry.resolve("GF Cross Fade");
    require(
        gf_cross_fade.behavior().cross_fade
            == pulseforge::NoteCrossFadeTarget::girlfriend,
        "GF Cross Fade targets the girlfriend"
    );
}''',
    '''    const auto gf_cross_fade = registry.resolve("GF Cross Fade");
    require(
        gf_cross_fade.behavior().cross_fade
            == pulseforge::NoteCrossFadeTarget::girlfriend,
        "GF Cross Fade targets the girlfriend"
    );

    const auto third = registry.resolve("3rd Player");
    require(!third.used_fallback, "3rd Player is a native identity type");
    require(third.behavior().builtin, "3rd Player cannot be replaced by a mod");
    const auto fifth = registry.resolve("5th Player");
    require(!fifth.used_fallback, "5th Player is a native identity type");
    require(fifth.behavior().builtin, "5th Player cannot be replaced by a mod");
    const auto bullet = registry.resolve("the note");
    require(!bullet.used_fallback, "the note is a native built-in");
    require(bullet.behavior().builtin, "the note cannot be replaced by a mod");
    require(bullet.behavior().health.miss == 0.6, "the note keeps its native miss penalty");
    require(bullet.behavior().visual.texture_id == "BULLET", "the note uses native BULLET skin");
    require(bullet.behavior().feedback.hitsound_enabled, "the note enables its native gunshot");
    require(bullet.behavior().feedback.hitsound_id == "gunshot", "the note uses native gunshot id");

    pulseforge::NoteTypeDefinition attempted_override;
    attempted_override.id = "the note";
    std::string override_error;
    require(
        !registry.register_definition(
            std::move(attempted_override),
            pulseforge::NoteTypeReplacePolicy::replace_custom,
            &override_error
        ),
        "mods cannot replace a native NoteType"
    );
}''',
)

# ---------------------------------------------------------------------------
# Native NoteSkins: fixed logical engine entries are present without scanning.
# Installed mod skins remain discoverable as additional choices.
# ---------------------------------------------------------------------------
replace_once(
    "include/pulseforge/note_skin_catalog.hpp",
    '''struct NoteSkinCatalogEntry {
    std::string selection;
    std::string display_name;
    std::string style;
    std::filesystem::path source_root;
    bool pixel{};
};''',
    '''struct NoteSkinCatalogEntry {
    std::string selection;
    std::string display_name;
    std::string style;
    std::filesystem::path source_root;
    bool pixel{};
    bool builtin{};
};''',
)
replace_once(
    "include/pulseforge/note_skin_catalog.hpp",
    '''    if (name.empty()) return "content root";
    if (lower_ascii(name) == "assets") return "built-in assets";''',
    '''    if (name.empty()) return "PulseForge native";
    if (lower_ascii(name) == "assets") return "PulseForge native";''',
)
replace_once(
    "include/pulseforge/note_skin_catalog.hpp",
    '''    entry.source_root = source_root;
    entry.selection = make_selection(pixel, style, source_root);
    entry.display_name = pretty_name(style, pixel)
        + "  //  " + source_label(source_root);

    if (std::none_of(
            entries.begin(),
            entries.end(),
            [&](const NoteSkinCatalogEntry& existing) {
                return existing.selection == entry.selection;
            }
        )) {
        entries.push_back(std::move(entry));
    }''',
    '''    entry.source_root = source_root;
    entry.selection = make_selection(pixel, style, source_root);
    entry.display_name = pretty_name(style, pixel)
        + "  //  " + source_label(source_root);
    entry.builtin = false;

    if (std::none_of(
            entries.begin(),
            entries.end(),
            [&](const NoteSkinCatalogEntry& existing) {
                // A native style is authoritative: do not duplicate it merely
                // because a mod ships an atlas with the same logical name.
                return existing.selection == entry.selection
                    || (existing.builtin && existing.pixel == entry.pixel
                        && equals_ascii_insensitive(existing.style, entry.style));
            }
        )) {
        entries.push_back(std::move(entry));
    }''',
)
replace_once(
    "include/pulseforge/note_skin_catalog.hpp",
    '''    const auto install_roots = expand_install_roots(roots);
    std::vector<NoteSkinCatalogEntry> entries;
    entries.reserve(64U);
    std::size_t examined_files{};''',
    '''    const auto install_roots = expand_install_roots(roots);
    std::vector<NoteSkinCatalogEntry> entries;
    entries.reserve(68U);

    // Native engine-owned logical skins. These exist even when the platform
    // cannot enumerate packaged assets (Android) and are kept ahead of mod
    // discovery. BULLET and HURTNOTE_assets are shipped by the engine itself.
    entries.push_back({
        "atlas:NOTE_assets", "Classic  //  PulseForge native",
        "NOTE_assets", {}, false, true,
    });
    entries.push_back({
        "pixel:arrows-pixels", "Pixel  //  PulseForge native",
        "arrows-pixels", {}, true, true,
    });
    entries.push_back({
        "atlas:HURTNOTE_assets", "HURTNOTE assets  //  PulseForge native",
        "HURTNOTE_assets", {}, false, true,
    });
    entries.push_back({
        "atlas:BULLET", "BULLET  //  PulseForge native",
        "BULLET", {}, false, true,
    });
    std::size_t examined_files{};''',
)

# ---------------------------------------------------------------------------
# Complete installer localisation: system locale, supported-language fallback.
# ---------------------------------------------------------------------------
replace_once(
    "src/app/complete_content_gate.hpp",
    '''#include "complete_content_transport.hpp"

#include "pulseforge/application.hpp"''',
    '''#include "complete_content_transport.hpp"
#include "system_locale.hpp"

#include "pulseforge/application.hpp"''',
)
old_prompt = '''[[nodiscard]] inline bool prompt_install(
    const std::size_t pending_mods,
    const std::uint64_t pending_files
) {
    const std::array<SDL_MessageBoxButtonData, 2> buttons{{
        {
            SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,
            1,
            "Yes",
        },
        {
            SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,
            0,
            "No",
        },
    }};
    const std::string message =
        "PulseForge Complete has " + std::to_string(pending_mods)
        + (pending_mods == 1U ? " mod to install/update (" : " mods to install/update (")
        + std::to_string(pending_files) + " files).\\n\\n"
          "Install the mods now?\\n\\n"
          "Files are transferred directly from Google Drive into the mods folder. "
          "If you choose No, PulseForge will ask again on a later launch.";
    const SDL_MessageBoxData data{
        .flags = SDL_MESSAGEBOX_INFORMATION,
        .window = nullptr,
        .title = "PulseForge Complete",
        .message = message.c_str(),
        .numbuttons = static_cast<int>(buttons.size()),
        .buttons = buttons.data(),
        .colorScheme = nullptr,
    };
    int selected = 0;
    if (!SDL_ShowMessageBox(&data, &selected)) {
        std::cerr << "Complete install prompt failed: " << SDL_GetError() << '\\n';
        return false;
    }
    return selected == 1;
}

inline void show_complete_error(const std::string& error) {
    const std::string message =
        "PulseForge could not complete the Complete mod installation.\\n\\n"
        + error
        + "\\n\\nAlready installed content was preserved. PulseForge can continue and will "
          "try again on a later launch.";
    SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_ERROR,
        "PulseForge Complete",
        message.c_str(),
        nullptr
    );
}'''
new_prompt = '''[[nodiscard]] inline bool prompt_install(
    const std::size_t pending_mods,
    const std::uint64_t pending_files
) {
    const auto strings = complete_ui_strings();
    const std::array<SDL_MessageBoxButtonData, 2> buttons{{
        {
            SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,
            1,
            strings.yes.data(),
        },
        {
            SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,
            0,
            strings.no.data(),
        },
    }};
    const std::string prefix = pending_mods == 1U
        ? std::string(strings.has_one_mod)
        : replace_count_token(strings.has_many_mods, pending_mods);
    const std::string message = prefix
        + std::to_string(pending_files) + std::string(strings.files_suffix)
        + "\\n\\n" + std::string(strings.install_now)
        + "\\n\\n" + std::string(strings.transfer_hint);
    const SDL_MessageBoxData data{
        .flags = SDL_MESSAGEBOX_INFORMATION,
        .window = nullptr,
        .title = "PulseForge Complete",
        .message = message.c_str(),
        .numbuttons = static_cast<int>(buttons.size()),
        .buttons = buttons.data(),
        .colorScheme = nullptr,
    };
    int selected = 0;
    if (!SDL_ShowMessageBox(&data, &selected)) {
        std::cerr << "Complete install prompt failed: " << SDL_GetError() << '\\n';
        return false;
    }
    return selected == 1;
}

inline void show_complete_error(const std::string& error) {
    const auto strings = complete_ui_strings();
    const std::string message = std::string(strings.install_failed)
        + "\\n\\n" + error + "\\n\\n" + std::string(strings.preserved_hint);
    SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_ERROR,
        "PulseForge Complete",
        message.c_str(),
        nullptr
    );
}'''
replace_once("src/app/complete_content_gate.hpp", old_prompt, new_prompt)
replace_once(
    "src/app/complete_content_gate.hpp",
    '''    SDL_Window* window = SDL_CreateWindow(
        "PulseForge Complete — preparing installation",
        960,
        280,
        SDL_WINDOW_HIGH_PIXEL_DENSITY
    );''',
    '''    const auto localized = complete_ui_strings();
    const std::string preparing_title = "PulseForge Complete — "
        + std::string(localized.preparing);
    SDL_Window* window = SDL_CreateWindow(
        preparing_title.c_str(),
        960,
        280,
        SDL_WINDOW_HIGH_PIXEL_DENSITY
    );''',
)
replace_once(
    "src/app/complete_content_gate.hpp",
    '''            const std::string title = cancel.load(std::memory_order_relaxed)
                ? "PulseForge Complete — cancelling..."
                : "PulseForge Complete — " + detail + " — mods "
                    + std::to_string(mods_done) + '/' + std::to_string(mods_total)
                    + " — files " + std::to_string(files_done) + '/'
                    + std::to_string(files_total);''',
    '''            const std::string title = cancel.load(std::memory_order_relaxed)
                ? "PulseForge Complete — " + std::string(localized.cancelling)
                : "PulseForge Complete — " + detail + " — "
                    + std::string(localized.mods_label) + " "
                    + std::to_string(mods_done) + '/' + std::to_string(mods_total)
                    + " — " + std::string(localized.files_label) + " "
                    + std::to_string(files_done) + '/' + std::to_string(files_total);''',
)

# The source invariant now tests locale detection rather than one hard-coded UI.
replace_once(
    "scripts/complete_runtime_transport_selftest.py",
    '''    for needle in (
        "Install the mods now?",
        '"Yes"',
        '"No"',
        "PULSEFORGE_COMPLETE_MANIFEST",
        "PULSEFORGE_MOD_ROOT",
        "run_progress_window",
    ):
        require(gate, needle)
    for needle in (
        "Pretende instalar os mods agora?",
        '"Sim"',
        '"Não"',
        "ficheiros",
        "a preparar instalação",
    ):
        forbid(gate, needle)''',
    '''    locale = (ROOT / "src/app/system_locale.hpp").read_text(encoding="utf-8")
    for needle in (
        "complete_ui_strings()",
        "PULSEFORGE_COMPLETE_MANIFEST",
        "PULSEFORGE_MOD_ROOT",
        "run_progress_window",
    ):
        require(gate, needle)
    for needle in (
        "SDL_GetPreferredLocales",
        "SystemUiLanguage::portuguese",
        "SystemUiLanguage::english",
        "SystemUiLanguage::spanish",
        "SystemUiLanguage::french",
        "SystemUiLanguage::german",
        "SystemUiLanguage::japanese",
        "SystemUiLanguage::chinese",
        "Pretende instalar os mods agora?",
        "Install the mods now?",
    ):
        require(locale, needle)''',
)

# system_locale.hpp uses std::string/std::size_t directly.
replace_once(
    "src/app/system_locale.hpp",
    '''#include <SDL3/SDL.h>

#include <string_view>''',
    '''#include <SDL3/SDL.h>

#include <cstddef>
#include <string>
#include <string_view>''',
)

# ---------------------------------------------------------------------------
# Visible in-editor audio import, including persistence into chart metadata.
# ---------------------------------------------------------------------------
replace_once(
    "src/app/editor_ui.cpp",
    '''#include <algorithm>
#include <array>
#include <charconv>''',
    '''#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>''',
)
replace_once(
    "src/app/editor_ui.cpp",
    '''#include <limits>
#include <optional>
#include <sstream>''',
    '''#include <limits>
#include <mutex>
#include <optional>
#include <sstream>''',
)
insert_before(
    "src/app/editor_ui.cpp",
    '''[[nodiscard]] std::filesystem::path direct_audio_candidate(''',
    '''struct EditorAudioImportDialogState final {
    std::mutex mutex;
    std::filesystem::path selected;
    std::string error;
    std::atomic<bool> complete{false};
    std::atomic<bool> active{false};
};

void SDLCALL editor_audio_import_dialog_callback(
    void* const userdata,
    const char* const* file_list,
    const int /*filter*/
) {
    auto* const state = static_cast<EditorAudioImportDialogState*>(userdata);
    if (state == nullptr) return;
    {
        std::scoped_lock lock(state->mutex);
        state->selected.clear();
        state->error.clear();
        if (file_list == nullptr) {
            const char* const message = SDL_GetError();
            state->error = message != nullptr && message[0] != '\\0'
                ? message
                : "Audio file dialog failed";
        } else if (file_list[0] != nullptr && file_list[0][0] != '\\0') {
            state->selected = std::filesystem::path{file_list[0]};
        }
    }
    state->complete.store(true, std::memory_order_release);
}

''',
)
replace_once(
    "src/app/editor_ui.cpp",
    '''    auto* const editor_audio = audio_session.transport();
    ChartViewState view;''',
    '''    AudioTransport* editor_audio = audio_session.transport();
    std::unique_ptr<AudioTransport> imported_audio;
    EditorAudioImportDialogState audio_import_dialog;
    ChartViewState view;''',
)
# Add visible audio button and compact save button.
replace_once(
    "src/app/editor_ui.cpp",
    '''    static_cast<void>(button(
        renderer,
        {854.0F, 610.0F, 382.0F, 36.0F},
        "CTRL+S  SAVE PROJECT + PSYCH",
        contains({854.0F, 610.0F, 382.0F, 36.0F}, mouse_x, mouse_y),
        can_save,
        success
    ));''',
    '''    static_cast<void>(button(
        renderer,
        {854.0F, 610.0F, 184.0F, 36.0F},
        "A  IMPORT AUDIO",
        contains({854.0F, 610.0F, 184.0F, 36.0F}, mouse_x, mouse_y),
        true,
        cyan
    ));
    static_cast<void>(button(
        renderer,
        {1'052.0F, 610.0F, 184.0F, 36.0F},
        "CTRL+S  SAVE",
        contains({1'052.0F, 610.0F, 184.0F, 36.0F}, mouse_x, mouse_y),
        can_save,
        success
    ));''',
)
# Audio import lambdas immediately before scroll edit.
insert_before(
    "src/app/editor_ui.cpp",
    '''    const auto begin_scroll_edit = [&]() {''',
    '''    const auto begin_audio_import = [&]() {
        if (audio_import_dialog.active.exchange(true, std::memory_order_acq_rel)) {
            status = "Audio file picker is already open";
            status_error = false;
            return;
        }
        {
            std::scoped_lock lock(audio_import_dialog.mutex);
            audio_import_dialog.selected.clear();
            audio_import_dialog.error.clear();
        }
        audio_import_dialog.complete.store(false, std::memory_order_release);
        static constexpr std::array<SDL_DialogFileFilter, 2U> filters{{
            {"Audio", "mp3;ogg;wav;flac;opus;m4a;aac"},
            {"All files", "*"},
        }};
        SDL_ShowOpenFileDialog(
            editor_audio_import_dialog_callback,
            &audio_import_dialog,
            window,
            filters.data(),
            static_cast<int>(filters.size()),
            nullptr,
            false
        );
        status = "Choose an audio file to chart";
        status_error = false;
    };

    const auto process_audio_import = [&]() {
        if (!audio_import_dialog.complete.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        audio_import_dialog.active.store(false, std::memory_order_release);
        std::filesystem::path selected;
        std::string dialog_error;
        {
            std::scoped_lock lock(audio_import_dialog.mutex);
            selected = audio_import_dialog.selected;
            dialog_error = audio_import_dialog.error;
        }
        if (!dialog_error.empty()) {
            status = "Audio import failed: " + dialog_error;
            status_error = true;
            return;
        }
        if (selected.empty()) {
            status = "Audio import cancelled";
            status_error = false;
            return;
        }

        auto candidate = std::make_unique<AudioTransport>();
        std::string audio_error;
        if (!candidate->initialize(options.audio_settings, options.audio_backend, &audio_error)) {
            status = "Audio import failed: " + audio_error;
            status_error = true;
            return;
        }
        AudioManifest manifest;
        manifest.instrumental = selected;
        if (!candidate->load(manifest, structural_duration_ms, 120.0, &audio_error)) {
            status = "Audio import failed: " + audio_error;
            status_error = true;
            return;
        }
        candidate->set_looping(false);
        candidate->set_playback_rate(1.0);

        auto metadata = editor.metadata();
        metadata.audio = manifest;
        std::string metadata_error;
        if (!editor.set_metadata(metadata, &metadata_error)) {
            status = "Audio loaded but chart metadata rejected it: " + metadata_error;
            status_error = true;
            return;
        }
        if (editor_audio != nullptr
            && editor_audio->state() == AudioTransportState::playing) {
            editor_audio->pause();
        }
        imported_audio = std::move(candidate);
        editor_audio = imported_audio.get();
        view.playhead_ms = 0.0;
        editor_audio->seek_ms(0.0);
        status = "Audio imported: " + selected.filename().string();
        status_error = false;
    };

''',
)
replace_once(
    "src/app/editor_ui.cpp",
    '''    while (running) {
        if (editor_audio != nullptr''',
    '''    while (running) {
        process_audio_import();
        if (editor_audio != nullptr''',
)
replace_once(
    "src/app/editor_ui.cpp",
    '''                case SDL_SCANCODE_N:
                    begin_note_kind_edit();
                    break;''',
    '''                case SDL_SCANCODE_A:
                    begin_audio_import();
                    break;
                case SDL_SCANCODE_N:
                    begin_note_kind_edit();
                    break;''',
)
# Mouse: replace the old full-width save target with audio + save targets.
replace_once(
    "src/app/editor_ui.cpp",
    '''                } else if (event.button.button == SDL_BUTTON_LEFT
                           && contains(
                               {854.0F, 610.0F, 382.0F, 36.0F},
                               event.button.x,
                               event.button.y
                           )) {
                    save_documents();
                }''',
    '''                } else if (event.button.button == SDL_BUTTON_LEFT
                           && contains(
                               {854.0F, 610.0F, 184.0F, 36.0F},
                               event.button.x,
                               event.button.y
                           )) {
                    begin_audio_import();
                } else if (event.button.button == SDL_BUTTON_LEFT
                           && contains(
                               {1'052.0F, 610.0F, 184.0F, 36.0F},
                               event.button.x,
                               event.button.y
                           )) {
                    save_documents();
                }''',
)

# ---------------------------------------------------------------------------
# Complete desktop package: remove development payload but retain runtime libs
# and legal/licence material. Shared libraries under lib are preserved.
# ---------------------------------------------------------------------------
replace_once(
    "scripts/complete_release_package.sh",
    '''  cp -a "$RUNNER_TEMP/compiled/complete-engine-stage-windows-x86_64/." "$root/$package/"
  rm -rf "$root/$package/bin/assets" "$root/$package/bin/mods"''',
    '''  cp -a "$RUNNER_TEMP/compiled/complete-engine-stage-windows-x86_64/." "$root/$package/"
  rm -rf "$root/$package/include" "$root/$package/docs"
  if [[ -d "$root/$package/lib" ]]; then
    find "$root/$package/lib" -type f \
      \( -name '*.a' -o -name '*.lib' -o -name '*.cmake' -o -name '*.pc' \) -delete
    find "$root/$package/lib" -type d -empty -delete
  fi
  rm -rf "$root/$package/bin/assets" "$root/$package/bin/mods"''',
)
replace_once(
    "scripts/complete_release_package.sh",
    '''  cp -a "$RUNNER_TEMP/compiled/complete-engine-stage-${stage_key}/." "$root/$package/"
  rm -rf "$root/$package/bin/assets" "$root/$package/bin/mods"''',
    '''  cp -a "$RUNNER_TEMP/compiled/complete-engine-stage-${stage_key}/." "$root/$package/"
  rm -rf "$root/$package/include" "$root/$package/docs"
  if [[ -d "$root/$package/lib" ]]; then
    find "$root/$package/lib" -type f \
      \( -name '*.a' -o -name '*.lib' -o -name '*.cmake' -o -name '*.pc' \) -delete
    find "$root/$package/lib" -type d -empty -delete
  fi
  rm -rf "$root/$package/bin/assets" "$root/$package/bin/mods"''',
)

print("Native tester follow-up patches applied successfully")
