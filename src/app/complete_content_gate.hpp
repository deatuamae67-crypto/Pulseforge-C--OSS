#pragma once

#include "complete_content_transport.hpp"

#include "pulseforge/application.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace pulseforge::detail {
namespace complete_gate_detail {

[[nodiscard]] inline std::optional<std::filesystem::path> environment_path(
    const char* name
) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::filesystem::path(value);
}

[[nodiscard]] inline bool regular_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

[[nodiscard]] inline bool directory(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_directory(path, error) && !error;
}

[[nodiscard]] inline std::string filename_lower(
    const std::filesystem::path& path
) {
    auto value = path.filename().string();
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

[[nodiscard]] inline std::optional<std::filesystem::path> find_manifest(
    const AppLaunchOptions& options
) {
    if (const auto configured = environment_path("PULSEFORGE_COMPLETE_MANIFEST")) {
        if (regular_file(*configured)) {
            return *configured;
        }
    }

    std::vector<std::filesystem::path> candidates;
    const auto append = [&](const std::filesystem::path& path) {
        if (path.empty()) return;
        if (std::find(candidates.begin(), candidates.end(), path) == candidates.end()) {
            candidates.push_back(path);
        }
    };
    if (!options.settings_path.empty()) {
        append(options.settings_path.parent_path() / "complete-runtime-manifest.json");
    }
    for (const auto& root : options.content_roots) {
        append(root / "complete-runtime-manifest.json");
        append(root / "assets" / "complete-runtime-manifest.json");
        if (filename_lower(root) == "assets") {
            append(root / "complete-runtime-manifest.json");
        }
    }
    append(std::filesystem::current_path() / "assets" / "complete-runtime-manifest.json");
    append(std::filesystem::current_path() / "complete-runtime-manifest.json");
    for (const auto& candidate : candidates) {
        if (regular_file(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline std::filesystem::path find_mods_root(
    const AppLaunchOptions& options
) {
    if (const auto configured = environment_path("PULSEFORGE_MOD_ROOT")) {
        std::error_code error;
        std::filesystem::create_directories(*configured, error);
        if (!error && directory(*configured)) {
            return *configured;
        }
    }
    if (!options.selected_mod_root.empty()) {
        std::error_code error;
        std::filesystem::create_directories(options.selected_mod_root, error);
        if (!error && directory(options.selected_mod_root)) {
            return options.selected_mod_root;
        }
    }
    for (auto iterator = options.content_roots.rbegin();
         iterator != options.content_roots.rend(); ++iterator) {
        if (filename_lower(*iterator) == "mods" && directory(*iterator)) {
            return *iterator;
        }
    }
    const auto fallback = std::filesystem::current_path() / "mods";
    std::error_code error;
    std::filesystem::create_directories(fallback, error);
    return fallback;
}

[[nodiscard]] inline bool prompt_install(
    const std::size_t pending_mods,
    const std::uint64_t pending_files
) {
    const std::array<SDL_MessageBoxButtonData, 2> buttons{{
        {
            SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,
            1,
            "Sim",
        },
        {
            SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,
            0,
            "Não",
        },
    }};
    const std::string message =
        "O PulseForge Complete tem " + std::to_string(pending_mods)
        + (pending_mods == 1U ? " mod por instalar/atualizar (" : " mods por instalar/atualizar (")
        + std::to_string(pending_files) + " ficheiros).\n\n"
          "Pretende instalar os mods agora?\n\n"
          "Os ficheiros são transferidos diretamente do Google Drive para a pasta de mods. "
          "Se escolher Não, o PulseForge volta a perguntar num próximo arranque.";
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
        std::cerr << "Complete install prompt failed: " << SDL_GetError() << '\n';
        return false;
    }
    return selected == 1;
}

inline void show_complete_error(const std::string& error) {
    const std::string message =
        "Não foi possível concluir a instalação dos mods Complete.\n\n"
        + error
        + "\n\nO conteúdo já instalado foi preservado. O PulseForge pode continuar e tentará "
          "novamente num próximo arranque.";
    SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_ERROR,
        "PulseForge Complete",
        message.c_str(),
        nullptr
    );
}

inline void render_progress(
    SDL_Renderer* renderer,
    const std::uint64_t done,
    const std::uint64_t total
) {
    if (renderer == nullptr) return;
    SDL_SetRenderDrawColor(renderer, 3, 7, 10, 255);
    SDL_RenderClear(renderer);
    constexpr SDL_FRect frame{70.0F, 116.0F, 820.0F, 44.0F};
    SDL_SetRenderDrawColor(renderer, 30, 42, 48, 255);
    SDL_RenderFillRect(renderer, &frame);
    const double ratio = total == 0U
        ? 0.0
        : std::clamp(
            static_cast<double>(done) / static_cast<double>(total),
            0.0,
            1.0
        );
    SDL_FRect fill{
        frame.x + 4.0F,
        frame.y + 4.0F,
        static_cast<float>((frame.w - 8.0F) * ratio),
        frame.h - 8.0F,
    };
    SDL_SetRenderDrawColor(renderer, 73, 245, 199, 255);
    SDL_RenderFillRect(renderer, &fill);
    SDL_RenderPresent(renderer);
}

inline CompleteTransferResult run_progress_window(
    const CompleteRuntimeManifest& manifest,
    const CompleteRuntimePlan& plan,
    const std::filesystem::path& mods_root
) {
    CompleteTransferProgress progress;
    std::atomic<bool> cancel{false};
    CompleteTransferResult transfer;

    std::uint64_t total_files = 0U;
    std::uint64_t total_mods = 0U;
    for (const auto& entry : plan.entries) {
        if (entry.requires_install() && entry.mod != nullptr) {
            ++total_mods;
            total_files += static_cast<std::uint64_t>(entry.mod->files.size());
        }
    }
    progress.total_files.store(total_files, std::memory_order_relaxed);
    progress.total_mods.store(total_mods, std::memory_order_relaxed);

    SDL_Window* window = SDL_CreateWindow(
        "PulseForge Complete — a preparar instalação",
        960,
        280,
        SDL_WINDOW_HIGH_PIXEL_DENSITY
    );
    SDL_Renderer* renderer = window != nullptr ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (renderer != nullptr) {
        SDL_SetRenderLogicalPresentation(
            renderer,
            960,
            280,
            SDL_LOGICAL_PRESENTATION_LETTERBOX
        );
    }

    std::thread worker([&] {
        transfer = install_pending_complete_runtime(
            manifest,
            plan,
            mods_root,
            &progress,
            &cancel
        );
        progress.success.store(transfer.success, std::memory_order_release);
        progress.cancelled.store(transfer.cancelled, std::memory_order_release);
        {
            std::scoped_lock lock(progress.detail_mutex);
            progress.error = transfer.error;
        }
        progress.finished.store(true, std::memory_order_release);
    });

    while (!progress.finished.load(std::memory_order_acquire)) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
                || (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                cancel.store(true, std::memory_order_relaxed);
            }
        }
        const auto files_done = progress.files_done.load(std::memory_order_relaxed);
        const auto files_total = progress.total_files.load(std::memory_order_relaxed);
        const auto mods_done = progress.mods_done.load(std::memory_order_relaxed);
        const auto mods_total = progress.total_mods.load(std::memory_order_relaxed);
        std::string detail;
        {
            std::scoped_lock lock(progress.detail_mutex);
            detail = progress.detail;
        }
        if (window != nullptr) {
            const std::string title = cancel.load(std::memory_order_relaxed)
                ? "PulseForge Complete — a cancelar..."
                : "PulseForge Complete — " + detail + " — mods "
                    + std::to_string(mods_done) + '/' + std::to_string(mods_total)
                    + " — ficheiros " + std::to_string(files_done) + '/'
                    + std::to_string(files_total);
            SDL_SetWindowTitle(window, title.c_str());
        }
        render_progress(renderer, files_done, files_total);
        SDL_Delay(33U);
    }

    if (worker.joinable()) worker.join();
    if (renderer != nullptr) SDL_DestroyRenderer(renderer);
    if (window != nullptr) SDL_DestroyWindow(window);
    return transfer;
}

}  // namespace complete_gate_detail

// Optional startup gate. Normal/open-source builds that do not package a
// Complete runtime manifest are unchanged. Complete builds prompt only while at
// least one manifest revision is missing, and declining is intentionally not
// persisted so the question appears on a later launch as requested.
inline void run_complete_content_gate(const AppLaunchOptions& options) noexcept {
    using namespace complete_gate_detail;
    try {
        if (options.smoke_test || options.safe_mode) {
            return;
        }
        const auto manifest_path = find_manifest(options);
        if (!manifest_path.has_value()) {
            return;
        }
        const auto mods_root = find_mods_root(options);
        const auto manifest = load_complete_runtime_manifest(*manifest_path);
        const auto plan = plan_complete_runtime_content(manifest, mods_root);
        if (plan.complete()) {
            return;
        }

        std::uint64_t pending_files = 0U;
        for (const auto& entry : plan.entries) {
            if (entry.requires_install() && entry.mod != nullptr) {
                pending_files += static_cast<std::uint64_t>(entry.mod->files.size());
            }
        }

        const bool owned_video = SDL_WasInit(SDL_INIT_VIDEO) == 0U;
        if (owned_video && !SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
            std::cerr << "Complete gate skipped: SDL initialization failed: "
                      << SDL_GetError() << '\n';
            return;
        }
        const bool accepted = prompt_install(plan.pending_count(), pending_files);
        if (!accepted) {
            if (owned_video) SDL_Quit();
            return;
        }

        const auto transfer = run_progress_window(manifest, plan, mods_root);
        if (!transfer.success && !transfer.cancelled) {
            show_complete_error(transfer.error);
        }
        if (owned_video) SDL_Quit();
    } catch (const std::exception& error) {
        std::cerr << "Complete content gate warning: " << error.what() << '\n';
        // The Complete downloader is fail-open: a malformed/offline optional
        // manifest never prevents local/offline gameplay from launching.
    } catch (...) {
        std::cerr << "Complete content gate warning: unknown error\n";
    }
}

}  // namespace pulseforge::detail
