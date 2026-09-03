#include "pulseforge/script_manager.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace pulseforge {
namespace {

constexpr std::size_t maximum_managed_scripts = 256;
constexpr std::size_t maximum_manager_diagnostics = 256;

void append_manager_diagnostic(
    std::vector<LuaManagerDiagnostic>& diagnostics,
    LuaManagerDiagnostic diagnostic
) {
    if (diagnostics.size() >= maximum_manager_diagnostics) {
        diagnostics.erase(diagnostics.begin());
    }
    diagnostics.push_back(std::move(diagnostic));
}

[[nodiscard]] std::string normalized_source_key(const std::string_view name) {
    std::string result;
    result.reserve(name.size());
    for (const unsigned char value : name) {
        const char character = static_cast<char>(value);
        result.push_back(character == '\\'
            ? '/'
            : static_cast<char>(std::tolower(value)));
    }
    return result;
}

[[nodiscard]] bool script_definition_less(
    const LuaScriptDefinition& left,
    const LuaScriptDefinition& right
) {
    if (left.origin != right.origin) {
        return left.origin < right.origin;
    }
    if (left.priority != right.priority) {
        return left.priority < right.priority;
    }
    return normalized_source_key(left.source_name)
        < normalized_source_key(right.source_name);
}

void accumulate(
    LuaDispatchReport& aggregate,
    const LuaCallResult result
) noexcept {
    switch (result.status) {
    case LuaCallStatus::completed:
        ++aggregate.completed;
        break;
    case LuaCallStatus::stop_requested:
        ++aggregate.stop_requests;
        break;
    case LuaCallStatus::failed:
        ++aggregate.failed;
        break;
    case LuaCallStatus::not_loaded:
    case LuaCallStatus::missing:
    case LuaCallStatus::disabled:
        ++aggregate.skipped;
        break;
    }
}

void accumulate(
    LuaDispatchReport& aggregate,
    const LuaDispatchReport& report
) noexcept {
    aggregate.completed += report.completed;
    aggregate.stop_requests += report.stop_requests;
    aggregate.failed += report.failed;
    aggregate.skipped += report.skipped;
}

}  // namespace

struct LuaScriptManager::Impl {
    struct Slot {
        LuaScriptDefinition definition;
        std::unique_ptr<LuaRuntime> runtime;
    };

    LuaHostInterface& host;
    LuaRuntimeConfig config;
    std::vector<Slot> slots;
    std::vector<LuaManagerDiagnostic> persistent_diagnostics;

    explicit Impl(
        LuaHostInterface& host_value,
        LuaRuntimeConfig config_value
    )
        : host(host_value), config(std::move(config_value)) {}

    template <typename Callback>
    [[nodiscard]] LuaManagerCallReport call_each(Callback&& callback) {
        LuaManagerCallReport report;
        for (auto& slot : slots) {
            const auto result = callback(*slot.runtime);
            accumulate(report.callbacks, result);
            if (result.status == LuaCallStatus::stop_requested) {
                report.stop_requested = true;
                break;
            }
        }
        return report;
    }
};

LuaScriptManager::LuaScriptManager(
    LuaHostInterface& host,
    LuaRuntimeConfig per_script_config
)
    : impl_(std::make_unique<Impl>(
          host,
          std::move(per_script_config)
      )) {}

LuaScriptManager::~LuaScriptManager() = default;

LuaScriptManager::LuaScriptManager(LuaScriptManager&&) noexcept = default;

LuaScriptManager& LuaScriptManager::operator=(LuaScriptManager&&) noexcept =
    default;

LuaScriptLoadReport LuaScriptManager::load_scripts(
    const std::span<const LuaScriptDefinition> definitions
) {
    LuaScriptLoadReport report;
    report.requested = definitions.size();
    if (impl_ == nullptr) {
        report.failed = definitions.size();
        return report;
    }

    std::vector<Impl::Slot> candidate_slots;
    std::vector<LuaManagerDiagnostic> candidate_diagnostics;
    candidate_slots.reserve(definitions.size());

    std::vector<LuaScriptDefinition> ordered(
        definitions.begin(),
        definitions.end()
    );
    std::stable_sort(
        ordered.begin(),
        ordered.end(),
        script_definition_less
    );
    if (ordered.size() > maximum_managed_scripts) {
        report.failed += ordered.size() - maximum_managed_scripts;
        ordered.resize(maximum_managed_scripts);
        candidate_diagnostics.push_back({
            "<manager>",
            {
                LuaDiagnosticKind::load_error,
                "<scripts>",
                "script count exceeds the 256-state safety limit",
            },
        });
    }

    std::unordered_set<std::string> seen;
    seen.reserve(ordered.size());
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        auto& definition = ordered[index];
        if (definition.source_name.empty()) {
            definition.source_name = "@script-" + std::to_string(index) + ".lua";
        }
        if (!seen.insert(normalized_source_key(definition.source_name)).second) {
            ++report.duplicates;
            continue;
        }

        auto runtime = std::make_unique<LuaRuntime>(
            impl_->host,
            impl_->config
        );
        if (!runtime->load_script(
                definition.source,
                definition.source_name
            )) {
            ++report.failed;
            for (const auto& diagnostic : runtime->diagnostics()) {
                candidate_diagnostics.push_back({
                    definition.source_name,
                    diagnostic,
                });
            }
            continue;
        }

        candidate_slots.push_back({
            std::move(definition),
            std::move(runtime),
        });
        ++report.loaded;
    }
    // Hot reload is transactional when every candidate fails: the last good
    // script snapshot stays live while the new diagnostics are published.
    if (definitions.empty() || report.loaded != 0) {
        impl_->slots = std::move(candidate_slots);
    }
    impl_->persistent_diagnostics = std::move(candidate_diagnostics);
    return report;
}

// PULSEFORGE_P1_1_18_DYNAMIC_SCRIPT_MANAGER_IMPL_V1
LuaScriptLoadReport LuaScriptManager::add_script(
    LuaScriptDefinition definition
) {
    LuaScriptLoadReport report;
    report.requested = 1U;
    if (impl_ == nullptr) {
        report.failed = 1U;
        return report;
    }
    if (impl_->slots.size() >= maximum_managed_scripts) {
        report.failed = 1U;
        append_manager_diagnostic(
            impl_->persistent_diagnostics,
            {
                "<manager>",
                {
                    LuaDiagnosticKind::load_error,
                    "<scripts>",
                    "script count exceeds the 256-state safety limit",
                },
            }
        );
        return report;
    }
    if (definition.source_name.empty()) {
        definition.source_name = "@dynamic-script.lua";
    }

    const auto key = normalized_source_key(definition.source_name);
    const auto duplicate = std::find_if(
        impl_->slots.begin(),
        impl_->slots.end(),
        [&](const Impl::Slot& slot) {
            return normalized_source_key(slot.definition.source_name) == key;
        }
    );
    if (duplicate != impl_->slots.end()) {
        report.duplicates = 1U;
        return report;
    }

    auto runtime = std::make_unique<LuaRuntime>(impl_->host, impl_->config);
    if (!runtime->load_script(definition.source, definition.source_name)) {
        report.failed = 1U;
        for (const auto& diagnostic : runtime->diagnostics()) {
            append_manager_diagnostic(
                impl_->persistent_diagnostics,
                {
                    definition.source_name,
                    diagnostic,
                }
            );
        }
        return report;
    }

    Impl::Slot slot{
        std::move(definition),
        std::move(runtime),
    };
    const auto insert_at = std::upper_bound(
        impl_->slots.begin(),
        impl_->slots.end(),
        slot,
        [](const Impl::Slot& left, const Impl::Slot& right) {
            return script_definition_less(
                left.definition,
                right.definition
            );
        }
    );
    impl_->slots.insert(insert_at, std::move(slot));
    report.loaded = 1U;
    return report;
}

bool LuaScriptManager::remove_script(
    const std::string_view source_name
) noexcept {
    if (impl_ == nullptr || source_name.empty()) {
        return false;
    }
    try {
        const auto key = normalized_source_key(source_name);
        const auto found = std::find_if(
            impl_->slots.begin(),
            impl_->slots.end(),
            [&](const Impl::Slot& slot) {
                return normalized_source_key(slot.definition.source_name)
                    == key;
            }
        );
        if (found == impl_->slots.end()) {
            return false;
        }
        try {
            found->runtime->unload();
        } catch (...) {
            // Removal is a containment boundary. LuaRuntime normally converts
            // callback errors to diagnostics, but teardown must never unwind.
        }
        impl_->slots.erase(found);
        return true;
    } catch (...) {
        return false;
    }
}

bool LuaScriptManager::contains(
    const std::string_view source_name
) const noexcept {
    if (impl_ == nullptr || source_name.empty()) {
        return false;
    }
    try {
        const auto key = normalized_source_key(source_name);
        return std::any_of(
            impl_->slots.begin(),
            impl_->slots.end(),
            [&](const Impl::Slot& slot) {
                return normalized_source_key(slot.definition.source_name)
                    == key;
            }
        );
    } catch (...) {
        return false;
    }
}

LuaManagerCallReport LuaScriptManager::initialize_script(
    const std::string_view source_name,
    const bool song_started
) {
    LuaManagerCallReport report;
    if (impl_ == nullptr || source_name.empty()) {
        return report;
    }
    const auto key = normalized_source_key(source_name);
    const auto found = std::find_if(
        impl_->slots.begin(),
        impl_->slots.end(),
        [&](const Impl::Slot& slot) {
            return normalized_source_key(slot.definition.source_name) == key;
        }
    );
    if (found == impl_->slots.end()) {
        return report;
    }

    const auto call = [&](const LuaCallResult current) {
        accumulate(report.callbacks, current);
        if (current.status == LuaCallStatus::stop_requested) {
            report.stop_requested = true;
            return false;
        }
        return true;
    };

    if (!call(found->runtime->on_create())) {
        return report;
    }
    if (!call(found->runtime->on_create_post())) {
        return report;
    }
    if (song_started) {
        static_cast<void>(call(found->runtime->on_song_start()));
    }
    return report;
}

void LuaScriptManager::unload() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    for (auto& slot : impl_->slots) {
        try {
            slot.runtime->unload();
        } catch (...) {
            // LuaRuntime contains callback failures; this is an additional
            // guard for destruction paths where exceptions cannot propagate.
        }
    }
    impl_->slots.clear();
}

std::size_t LuaScriptManager::loaded_count() const noexcept {
    return impl_ == nullptr ? 0 : impl_->slots.size();
}

LuaManagerCallReport LuaScriptManager::on_create() {
    return impl_ == nullptr
        ? LuaManagerCallReport{}
        : impl_->call_each([](LuaRuntime& runtime) {
            return runtime.on_create();
        });
}

LuaManagerCallReport LuaScriptManager::on_create_post() {
    return impl_ == nullptr
        ? LuaManagerCallReport{}
        : impl_->call_each([](LuaRuntime& runtime) {
            return runtime.on_create_post();
        });
}

LuaManagerCallReport LuaScriptManager::on_start_countdown() {
    // PULSEFORGE_P1_1_4_MANAGER_COUNTDOWN_V1
    return impl_ == nullptr
        ? LuaManagerCallReport{}
        : impl_->call_each([](LuaRuntime& runtime) {
            return runtime.on_start_countdown();
        });
}

LuaManagerCallReport LuaScriptManager::on_song_start() {
    return impl_ == nullptr
        ? LuaManagerCallReport{}
        : impl_->call_each([](LuaRuntime& runtime) {
            return runtime.on_song_start();
        });
}

LuaManagerCallReport LuaScriptManager::on_end_song() {
    return impl_ == nullptr
        ? LuaManagerCallReport{}
        : impl_->call_each([](LuaRuntime& runtime) {
            return runtime.on_end_song();
        });
}

LuaManagerCallReport LuaScriptManager::on_game_over() {
    return impl_ == nullptr
        ? LuaManagerCallReport{}
        : impl_->call_each([](LuaRuntime& runtime) {
            return runtime.on_game_over();
        });
}

LuaManagerCallReport LuaScriptManager::on_pause() {
    return impl_ == nullptr
        ? LuaManagerCallReport{}
        : impl_->call_each([](LuaRuntime& runtime) {
            return runtime.on_pause();
        });
}

LuaManagerCallReport LuaScriptManager::on_resume() {
    return impl_ == nullptr
        ? LuaManagerCallReport{}
        : impl_->call_each([](LuaRuntime& runtime) {
            return runtime.on_resume();
        });
}

LuaManagerCallReport LuaScriptManager::on_tween_completed(
    const std::string_view tag,
    const std::string_view target
) {
    return impl_ == nullptr
        ? LuaManagerCallReport{}
        : impl_->call_each([&](LuaRuntime& runtime) {
            return runtime.on_tween_completed(tag, target);
        });
}

LuaManagerCallReport LuaScriptManager::on_timer_completed(
    const std::string_view tag,
    const std::int64_t loops,
    const std::int64_t loops_left
) {
    return impl_ == nullptr
        ? LuaManagerCallReport{}
        : impl_->call_each([&](LuaRuntime& runtime) {
            return runtime.on_timer_completed(tag, loops, loops_left);
        });
}

// PULSEFORGE_P1_1_18_RECURSIVE_EVENT_BUS_MANAGER_IMPL_V1
LuaManagerCallReport LuaScriptManager::on_event(
    const std::string_view name,
    const std::string_view value1,
    const std::string_view value2
) {
    return impl_ == nullptr
        ? LuaManagerCallReport{}
        : impl_->call_each([&](LuaRuntime& runtime) {
            return runtime.on_event(name, value1, value2);
        });
}

LuaManagerCallReport LuaScriptManager::on_sound_finished(
    const std::string_view tag
) {
    return impl_ == nullptr
        ? LuaManagerCallReport{}
        : impl_->call_each([&](LuaRuntime& runtime) {
            return runtime.on_sound_finished(tag);
        });
}

LuaManagerCallReport LuaScriptManager::on_destroy() {
    return impl_ == nullptr
        ? LuaManagerCallReport{}
        : impl_->call_each([](LuaRuntime& runtime) {
            return runtime.on_destroy();
        });
}

LuaManagerCallReport LuaScriptManager::dispatch_frame(
    const GameplaySession& session,
    const double elapsed_seconds
) {
    LuaManagerCallReport report;
    if (impl_ == nullptr) {
        return report;
    }
    for (auto& slot : impl_->slots) {
        const auto current = slot.runtime->dispatch_frame(
            session,
            elapsed_seconds
        );
        accumulate(report.callbacks, current);
        if (current.stop_requests != 0) {
            report.stop_requested = true;
            break;
        }
    }
    return report;
}


LuaManagerCallReport LuaScriptManager::dispatch_frame(
    const StreamingGameplaySession& session,
    const double elapsed_seconds
) {
    LuaManagerCallReport report;
    if (impl_ == nullptr) {
        return report;
    }
    for (auto& slot : impl_->slots) {
        const auto current = slot.runtime->dispatch_frame(
            session,
            elapsed_seconds
        );
        accumulate(report.callbacks, current);
        if (current.stop_requests != 0U) {
            report.stop_requested = true;
            break;
        }
    }
    return report;
}

std::vector<LuaManagerDiagnostic> LuaScriptManager::diagnostics() const {
    if (impl_ == nullptr) {
        return {};
    }
    auto result = impl_->persistent_diagnostics;
    for (const auto& slot : impl_->slots) {
        for (const auto& diagnostic : slot.runtime->diagnostics()) {
            result.push_back({slot.definition.source_name, diagnostic});
        }
    }
    return result;
}

}  // namespace pulseforge
