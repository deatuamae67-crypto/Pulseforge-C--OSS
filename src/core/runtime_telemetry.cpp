#include "pulseforge/runtime_telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace pulseforge {
namespace {

[[nodiscard]] std::string truncate_utf8_bytes(std::string value, const std::size_t limit) {
    if (value.size() <= limit) return value;
    if (limit == 0U) return {};

    // Reserve the ellipsis first, then cut only at a complete UTF-8 code-point
    // boundary. Mod/chart names are user content and can contain multibyte text.
    const std::size_t body_limit = limit >= 3U ? limit - 3U : limit;
    std::size_t cut = std::min(body_limit, value.size());
    if (cut != 0U && cut < value.size()) {
        std::size_t leader = cut - 1U;
        while (leader > 0U
               && (static_cast<unsigned char>(value[leader]) & 0xC0U) == 0x80U) {
            --leader;
        }
        const auto byte = static_cast<unsigned char>(value[leader]);
        const std::size_t expected = (byte & 0x80U) == 0U ? 1U
            : (byte & 0xE0U) == 0xC0U ? 2U
            : (byte & 0xF0U) == 0xE0U ? 3U
            : (byte & 0xF8U) == 0xF0U ? 4U
            : 1U;
        if (leader + expected > cut) cut = leader;
    }
    value.resize(cut);
    if (limit >= 3U) value += "...";
    return value;
}

void append_piece(std::string& target, std::string_view piece) {
    if (piece.empty()) return;
    if (!target.empty()) target += " • ";
    target.append(piece);
}

[[nodiscard]] std::string percent_text(const double ratio) {
    const auto bounded = std::clamp(
        std::isfinite(ratio) ? ratio : 0.0,
        0.0,
        1.0
    );
    char buffer[24]{};
    std::snprintf(buffer, sizeof(buffer), "%.0f%%", bounded * 100.0);
    return buffer;
}

[[nodiscard]] double gameplay_progress(const RuntimeTelemetrySnapshot& snapshot) noexcept {
    if (snapshot.duration_ms > 0.0 && std::isfinite(snapshot.duration_ms)
        && std::isfinite(snapshot.song_position_ms)) {
        return std::clamp(snapshot.song_position_ms / snapshot.duration_ms, 0.0, 1.0);
    }
    if (snapshot.logical_note_total != 0U) {
        const long double resolved = static_cast<long double>(snapshot.chart_total);
        const long double total = static_cast<long double>(snapshot.logical_note_total);
        return static_cast<double>(std::clamp(resolved / total, 0.0L, 1.0L));
    }
    return 0.0;
}

[[nodiscard]] std::string mania_text(const std::uint16_t key_count) {
    return key_count == 0U ? std::string{} : std::to_string(key_count) + "K";
}

[[nodiscard]] std::string note_counter_text(const RuntimeTelemetrySnapshot& snapshot) {
    if (snapshot.logical_note_total == 0U) {
        return std::to_string(snapshot.chart_total) + " notes";
    }
    return std::to_string(snapshot.chart_total) + " / "
        + std::to_string(snapshot.logical_note_total) + " notes";
}

[[nodiscard]] std::string accuracy_text(const double accuracy) {
    char buffer[32]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%.2f%%",
        std::clamp(std::isfinite(accuracy) ? accuracy : 0.0, 0.0, 100.0)
    );
    return buffer;
}

[[nodiscard]] std::string render_resolution_text(const RuntimeTelemetrySnapshot& snapshot) {
    if (snapshot.render_width == 0U || snapshot.render_height == 0U) return {};
    return std::to_string(snapshot.render_width) + "x"
        + std::to_string(snapshot.render_height);
}

[[nodiscard]] std::string trim_ascii_spaces(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

void replace_all(
    std::string& target,
    const std::string_view needle,
    const std::string_view replacement
) {
    if (needle.empty()) return;
    std::size_t position = 0U;
    while ((position = target.find(needle, position)) != std::string::npos) {
        target.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
}

[[nodiscard]] bool http_url(const std::string_view value) noexcept {
    return value.starts_with("https://") || value.starts_with("http://");
}

[[nodiscard]] std::string bounded_http_url(
    std::string value,
    const std::size_t maximum_bytes = 256U
) {
    value = trim_ascii_spaces(std::move(value));
    return value.size() <= maximum_bytes && http_url(value) ? value : std::string{};
}

[[nodiscard]] std::string bounded_asset(std::string value) {
    value = trim_ascii_spaces(std::move(value));
    return !value.empty() && value.size() <= 300U ? value : std::string{};
}

[[nodiscard]] std::string template_progress_text(
    const RuntimeTelemetrySnapshot& snapshot
) {
    const auto progress = snapshot.activity == RuntimeActivityKind::autochart
            || snapshot.activity == RuntimeActivityKind::rendering
        ? std::clamp(
            std::isfinite(snapshot.task_progress) ? snapshot.task_progress : 0.0,
            0.0,
            1.0
        )
        : gameplay_progress(snapshot);
    return percent_text(progress);
}

[[nodiscard]] std::string activity_default_state(const RuntimeActivityKind kind) {
    switch (kind) {
    case RuntimeActivityKind::launcher: return "Exploring PulseForge";
    case RuntimeActivityKind::loading: return "Preparing the chart";
    case RuntimeActivityKind::story: return "Choosing a week";
    case RuntimeActivityKind::freeplay: return "Choosing a chart";
    case RuntimeActivityKind::editor: return "Editing content";
    case RuntimeActivityKind::mods: return "Managing installed mods";
    case RuntimeActivityKind::rendering: return "Exporting a video";
    case RuntimeActivityKind::options: return "Configuring the engine";
    case RuntimeActivityKind::gameplay: return "In Gameplay";
    case RuntimeActivityKind::paused: return "Gameplay paused";
    case RuntimeActivityKind::results: return "Viewing results";
    case RuntimeActivityKind::autochart: return "Analyzing audio";
    }
    return "In PulseForge";
}

[[nodiscard]] std::string activity_details(const RuntimeActivityKind kind) {
    switch (kind) {
    case RuntimeActivityKind::launcher: return "Main Menu";
    case RuntimeActivityKind::loading: return "Loading a chart";
    case RuntimeActivityKind::story: return "Story Mode";
    case RuntimeActivityKind::freeplay: return "Freeplay";
    case RuntimeActivityKind::editor: return "Chart Editor";
    case RuntimeActivityKind::mods: return "Mods";
    case RuntimeActivityKind::rendering: return "Render Mode";
    case RuntimeActivityKind::options: return "Options";
    case RuntimeActivityKind::gameplay: return "Playing a chart";
    case RuntimeActivityKind::paused: return "Paused";
    case RuntimeActivityKind::results: return "Results";
    case RuntimeActivityKind::autochart: return "Generating a chart";
    }
    return "PulseForge";
}

[[nodiscard]] double percentile(
    const std::array<double, RuntimePerformanceAccumulator::capacity>& values,
    const std::size_t size,
    const double p
) noexcept {
    if (size == 0U) return 0.0;
    auto sorted = values;
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(size));
    const auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(size)));
    const auto index = std::min(size - 1U, rank == 0U ? 0U : rank - 1U);
    return sorted[index];
}

void record_ring(
    std::array<double, RuntimePerformanceAccumulator::capacity>& values,
    std::size_t& cursor,
    std::size_t& size,
    long double& sum,
    std::uint64_t& total_samples,
    const double value
) noexcept {
    if (!std::isfinite(value) || value < 0.0) return;
    if (size == values.size()) {
        sum -= values[cursor];
    } else {
        ++size;
    }
    values[cursor] = value;
    sum += value;
    cursor = (cursor + 1U) % values.size();
    if (total_samples != std::numeric_limits<std::uint64_t>::max()) {
        ++total_samples;
    }
}

}  // namespace

void RuntimePerformanceAccumulator::record_frame_ms(const double value) noexcept {
    record_ring(
        frame_ms_,
        frame_cursor_,
        frame_size_,
        frame_sum_,
        frame_samples_total_,
        value
    );
}

void RuntimePerformanceAccumulator::record_input_age_ms(const double value) noexcept {
    record_ring(
        input_age_ms_,
        input_cursor_,
        input_size_,
        input_sum_,
        input_samples_total_,
        value
    );
}

void RuntimePerformanceAccumulator::reset() noexcept {
    frame_ms_.fill(0.0);
    input_age_ms_.fill(0.0);
    frame_cursor_ = 0U;
    frame_size_ = 0U;
    input_cursor_ = 0U;
    input_size_ = 0U;
    frame_sum_ = 0.0L;
    input_sum_ = 0.0L;
    frame_samples_total_ = 0U;
    input_samples_total_ = 0U;
}

RuntimePerformanceReport RuntimePerformanceAccumulator::report() const noexcept {
    RuntimePerformanceReport output;
    output.frame_samples = frame_samples_total_;
    output.input_samples = input_samples_total_;
    if (frame_size_ != 0U) {
        output.average_frame_ms = static_cast<double>(
            frame_sum_ / static_cast<long double>(frame_size_)
        );
        output.frame_p50_ms = percentile(frame_ms_, frame_size_, 0.50);
        output.frame_p95_ms = percentile(frame_ms_, frame_size_, 0.95);
        output.frame_p99_ms = percentile(frame_ms_, frame_size_, 0.99);
        output.max_frame_ms = *std::max_element(
            frame_ms_.begin(),
            frame_ms_.begin() + static_cast<std::ptrdiff_t>(frame_size_)
        );
    }
    if (input_size_ != 0U) {
        output.average_input_age_ms = static_cast<double>(
            input_sum_ / static_cast<long double>(input_size_)
        );
        output.input_age_p95_ms = percentile(input_age_ms_, input_size_, 0.95);
    }
    return output;
}

std::string_view runtime_activity_name(const RuntimeActivityKind kind) noexcept {
    switch (kind) {
    case RuntimeActivityKind::launcher: return "launcher";
    case RuntimeActivityKind::loading: return "loading";
    case RuntimeActivityKind::story: return "story";
    case RuntimeActivityKind::freeplay: return "freeplay";
    case RuntimeActivityKind::editor: return "editor";
    case RuntimeActivityKind::mods: return "mods";
    case RuntimeActivityKind::rendering: return "rendering";
    case RuntimeActivityKind::options: return "options";
    case RuntimeActivityKind::gameplay: return "gameplay";
    case RuntimeActivityKind::paused: return "paused";
    case RuntimeActivityKind::results: return "results";
    case RuntimeActivityKind::autochart: return "autochart";
    }
    return "launcher";
}

std::string_view discord_privacy_name(const DiscordPresencePrivacy privacy) noexcept {
    switch (privacy) {
    case DiscordPresencePrivacy::full: return "full";
    case DiscordPresencePrivacy::reduced: return "reduced";
    case DiscordPresencePrivacy::minimal: return "minimal";
    }
    return "full";
}

bool discord_oauth_redirect_uri_valid(const std::string_view uri) noexcept {
    if (uri.empty() || uri == "http://127.0.0.1/callback") return true;
    constexpr std::string_view callback_suffix = ":/authorize/callback";
    if (!uri.ends_with(callback_suffix)) return false;
    const auto scheme = uri.substr(0U, uri.size() - callback_suffix.size());
    if (scheme.empty() || scheme.size() > 64U) return false;
    const auto ascii_alpha = [](const char value) {
        return (value >= 'a' && value <= 'z')
            || (value >= 'A' && value <= 'Z');
    };
    const auto ascii_scheme = [&](const char value) {
        return ascii_alpha(value) || (value >= '0' && value <= '9')
            || value == '+' || value == '-' || value == '.';
    };
    if (!ascii_alpha(scheme.front())) return false;
    if (!std::all_of(scheme.begin() + 1, scheme.end(), ascii_scheme)) return false;
    const auto ascii_iequals = [](const std::string_view left, const std::string_view right) {
        if (left.size() != right.size()) return false;
        for (std::size_t index = 0U; index < left.size(); ++index) {
            const auto lower = [](const char value) {
                return value >= 'A' && value <= 'Z'
                    ? static_cast<char>(value - 'A' + 'a')
                    : value;
            };
            if (lower(left[index]) != lower(right[index])) return false;
        }
        return true;
    };
    return !ascii_iequals(scheme, "http")
        && !ascii_iequals(scheme, "https")
        && !ascii_iequals(scheme, "javascript")
        && !ascii_iequals(scheme, "data")
        && !ascii_iequals(scheme, "file");
}

bool discord_oauth_redirect_uri_compatible(
    const std::string_view uri,
    const bool mobile_platform
) noexcept {
    if (!discord_oauth_redirect_uri_valid(uri) || uri.empty()) {
        return discord_oauth_redirect_uri_valid(uri);
    }
    return mobile_platform
        ? uri != "http://127.0.0.1/callback"
        : uri == "http://127.0.0.1/callback";
}

bool discord_presence_structural_change(
    const RuntimeTelemetrySnapshot& previous,
    const RuntimeTelemetrySnapshot& current
) noexcept {
    return previous.activity != current.activity
        || previous.chart_title != current.chart_title
        || previous.difficulty != current.difficulty
        || previous.mod_name != current.mod_name
        || previous.media_title != current.media_title
        || previous.media_artist != current.media_artist
        || previous.media_album != current.media_album
        || previous.media_url != current.media_url
        || previous.media_art_url != current.media_art_url
        || previous.key_count != current.key_count
        || previous.render_width != current.render_width
        || previous.render_height != current.render_height
        || previous.render_fps != current.render_fps
        || previous.botplay != current.botplay
        || previous.practice != current.practice
        || previous.paused != current.paused
        || previous.failed != current.failed
        || previous.completed != current.completed
        || previous.streaming != current.streaming
        || previous.third_strum != current.third_strum;
}

bool discord_presence_configuration_change(
    const DiscordPresenceSettings& previous,
    const DiscordPresenceSettings& current
) noexcept {
    // enabled is handled before payload construction/SDK access. The interval
    // changes scheduling only; it does not change backend identity or visible
    // Rich Presence, so it does not force an otherwise redundant SDK update.
    return previous.application_id != current.application_id
        || previous.privacy != current.privacy
        || previous.show_chart_name != current.show_chart_name
        || previous.show_difficulty_mania != current.show_difficulty_mania
        || previous.show_progress != current.show_progress
        || previous.show_note_counter != current.show_note_counter
        || previous.show_gameplay_stats != current.show_gameplay_stats
        || previous.show_botplay != current.show_botplay
        || previous.show_remaining_time != current.show_remaining_time
        || previous.show_mod_name != current.show_mod_name
        || previous.advanced_customization != current.advanced_customization
        || previous.activity_name_template != current.activity_name_template
        || previous.details_template != current.details_template
        || previous.state_template != current.state_template
        || previous.details_url_template != current.details_url_template
        || previous.state_url_template != current.state_url_template
        || previous.large_image_template != current.large_image_template
        || previous.large_text_template != current.large_text_template
        || previous.large_url_template != current.large_url_template
        || previous.small_image_template != current.small_image_template
        || previous.small_text_template != current.small_text_template
        || previous.small_url_template != current.small_url_template
        || previous.button1 != current.button1
        || previous.button2 != current.button2
        || previous.retry_failed_updates != current.retry_failed_updates;
}

bool discord_presence_should_publish(
    const bool currently_published,
    const bool payload_equal,
    const bool structural_change,
    const bool configuration_change,
    const bool publish_interval_elapsed
) noexcept {
    if (!currently_published) return true;
    if (payload_equal && !configuration_change) return false;
    return structural_change || configuration_change || publish_interval_elapsed;
}

std::string render_discord_presence_template(
    const std::string_view text,
    const RuntimeTelemetrySnapshot& snapshot,
    const DiscordPresencePrivacy privacy
) {
    std::string result(text);
    const bool full = privacy == DiscordPresencePrivacy::full;
    const bool minimal = privacy == DiscordPresencePrivacy::minimal;

    const auto sensitive = [full](std::string value) {
        return full ? value : std::string{};
    };
    const auto sensitive_view = [full](const std::string& value) {
        return full ? value : std::string{};
    };

    replace_all(result, "{engine}", "PulseForge");
    replace_all(result, "{activity}", runtime_activity_name(snapshot.activity));
    replace_all(result, "{status}", minimal ? std::string_view{} : std::string_view(snapshot.status));
    replace_all(result, "{chart.name}", sensitive_view(snapshot.chart_title));
    replace_all(result, "{difficulty}", minimal ? std::string{} : snapshot.difficulty);
    replace_all(result, "{mania}", minimal ? std::string{} : mania_text(snapshot.key_count));
    replace_all(result, "{mod.name}", sensitive_view(snapshot.mod_name));
    replace_all(result, "{progress}", minimal ? std::string{} : template_progress_text(snapshot));
    replace_all(result, "{notes.resolved}", sensitive(std::to_string(snapshot.chart_total)));
    replace_all(
        result,
        "{notes.total}",
        full && snapshot.logical_note_total != 0U
            ? std::to_string(snapshot.logical_note_total)
            : std::string{}
    );
    replace_all(result, "{score}", sensitive(std::to_string(snapshot.score)));
    replace_all(result, "{accuracy}", sensitive(accuracy_text(snapshot.accuracy_percent)));
    replace_all(result, "{misses}", sensitive(std::to_string(snapshot.misses)));
    replace_all(result, "{combo}", sensitive(std::to_string(snapshot.combo)));
    replace_all(result, "{max_combo}", sensitive(std::to_string(snapshot.max_combo)));
    replace_all(result, "{botplay}", full && snapshot.botplay ? "BOTPLAY" : "");
    replace_all(result, "{practice}", full && snapshot.practice ? "Practice" : "");
    replace_all(
        result,
        "{render.resolution}",
        minimal ? std::string{} : render_resolution_text(snapshot)
    );
    replace_all(
        result,
        "{render.fps}",
        minimal || snapshot.render_fps == 0U
            ? std::string{}
            : std::to_string(snapshot.render_fps)
    );
    replace_all(
        result,
        "{render.frame}",
        minimal ? std::string{} : std::to_string(snapshot.render_frame)
    );
    replace_all(
        result,
        "{render.frames}",
        minimal || snapshot.render_frame_count == 0U
            ? std::string{}
            : std::to_string(snapshot.render_frame_count)
    );
    replace_all(result, "{media.title}", sensitive_view(snapshot.media_title));
    replace_all(result, "{media.artist}", sensitive_view(snapshot.media_artist));
    replace_all(result, "{media.album}", sensitive_view(snapshot.media_album));
    replace_all(result, "{media.url}", sensitive_view(snapshot.media_url));
    replace_all(result, "{media.art}", sensitive_view(snapshot.media_art_url));
    return trim_ascii_spaces(std::move(result));
}

std::uint32_t discord_presence_retry_delay_ms(
    const std::uint32_t consecutive_failures,
    const std::uint32_t server_retry_after_ms
) noexcept {
    if (consecutive_failures == 0U && server_retry_after_ms == 0U) return 0U;
    constexpr std::uint32_t maximum_backoff_ms = 30'000U;
    constexpr std::uint32_t maximum_retry_after_ms = 60'000U;
    const auto exponent = std::min(consecutive_failures > 0U
        ? consecutive_failures - 1U
        : 0U, 5U);
    const auto backoff = consecutive_failures == 0U
        ? 0U
        : std::min<std::uint32_t>(1'000U << exponent, maximum_backoff_ms);
    return std::min(
        std::max(backoff, server_retry_after_ms),
        maximum_retry_after_ms
    );
}

DiscordPresencePayload build_discord_presence_payload(
    const RuntimeTelemetrySnapshot& snapshot,
    const DiscordPresenceSettings& settings,
    const std::uint64_t now_unix_ms
) {
    DiscordPresencePayload payload;
    const auto progress = snapshot.activity == RuntimeActivityKind::autochart
            || snapshot.activity == RuntimeActivityKind::rendering
        ? std::clamp(
            std::isfinite(snapshot.task_progress) ? snapshot.task_progress : 0.0,
            0.0,
            1.0
        )
        : gameplay_progress(snapshot);

    if (settings.privacy == DiscordPresencePrivacy::minimal) {
        payload.details = "Playing PulseForge";
        payload.state = activity_default_state(snapshot.activity);
    } else if (snapshot.activity == RuntimeActivityKind::gameplay
               || snapshot.activity == RuntimeActivityKind::paused
               || snapshot.activity == RuntimeActivityKind::results) {
        if (settings.privacy == DiscordPresencePrivacy::full
            && settings.show_chart_name && !snapshot.chart_title.empty()) {
            // Keep enough room on Discord's 128-byte details line for the
            // difficulty, mania, progress and mode flags. A pathological chart
            // title must not consume the entire Rich Presence payload.
            payload.details = truncate_utf8_bytes(snapshot.chart_title, 72U);
        } else {
            payload.details = activity_details(snapshot.activity);
        }
        if (settings.show_difficulty_mania) {
            append_piece(payload.details, snapshot.difficulty);
            append_piece(payload.details, mania_text(snapshot.key_count));
        }
        if (settings.show_progress && !snapshot.completed) {
            append_piece(payload.details, percent_text(progress));
        }
        if (settings.privacy == DiscordPresencePrivacy::full) {
            // Mode flags live in details as well as the expanded card so they
            // remain visible when Discord uses Details for the compact status.
            if (settings.show_botplay && snapshot.botplay) {
                append_piece(payload.details, "BOTPLAY");
            }
            if (snapshot.practice) {
                append_piece(payload.details, "PRACTICE");
            }
            if (snapshot.third_strum) {
                append_piece(payload.details, "THIRD STRUM");
            }
            if (std::isfinite(snapshot.playback_rate)
                && std::abs(snapshot.playback_rate - 1.0) >= 0.005) {
                char rate_buffer[24]{};
                std::snprintf(
                    rate_buffer,
                    sizeof(rate_buffer),
                    "%.2gx speed",
                    snapshot.playback_rate
                );
                append_piece(payload.details, rate_buffer);
            }

            if (settings.show_note_counter) {
                append_piece(payload.state, note_counter_text(snapshot));
            }
            if (settings.show_gameplay_stats) {
                if (!snapshot.failed) {
                    append_piece(payload.state, accuracy_text(snapshot.accuracy_percent));
                }
                append_piece(payload.state, std::to_string(snapshot.misses) + " misses");
                if (snapshot.combo != 0U) {
                    append_piece(
                        payload.state,
                        std::to_string(snapshot.combo) + "x combo"
                    );
                }
                if (snapshot.score != 0) {
                    append_piece(
                        payload.state,
                        std::to_string(snapshot.score) + " pts"
                    );
                }
                if ((snapshot.completed || snapshot.failed)
                    && snapshot.max_combo != 0U) {
                    append_piece(
                        payload.state,
                        "max " + std::to_string(snapshot.max_combo) + "x"
                    );
                }
            }
            if (settings.show_mod_name && !snapshot.mod_name.empty()) {
                append_piece(payload.state, snapshot.mod_name);
            }
        } else {
            // Reduced privacy intentionally suppresses chart/mod names, note
            // counter and gameplay statistics while retaining coarse context.
            if (snapshot.paused) append_piece(payload.state, "Paused");
            if (snapshot.failed) append_piece(payload.state, "Failed");
            if (snapshot.completed) append_piece(payload.state, "Completed");
        }
    } else if (snapshot.activity == RuntimeActivityKind::rendering) {
        payload.details = "Rendering";
        if (settings.privacy == DiscordPresencePrivacy::full
            && settings.show_chart_name && !snapshot.chart_title.empty()) {
            payload.details += " " + snapshot.chart_title;
        }
        append_piece(payload.state, render_resolution_text(snapshot));
        if (snapshot.render_fps != 0U) {
            append_piece(payload.state, std::to_string(snapshot.render_fps) + " FPS");
        }
        if (snapshot.render_frame_count != 0U) {
            append_piece(
                payload.state,
                "Frame " + std::to_string(snapshot.render_frame) + " / "
                    + std::to_string(snapshot.render_frame_count)
            );
        } else if (settings.show_progress) {
            append_piece(payload.state, percent_text(progress));
        }
    } else if (snapshot.activity == RuntimeActivityKind::autochart) {
        payload.details = "Generating a chart";
        payload.state = snapshot.status.empty() ? "Analyzing audio" : snapshot.status;
        if (settings.show_progress) append_piece(payload.state, percent_text(progress));
    } else if (snapshot.activity == RuntimeActivityKind::editor) {
        payload.details = "Chart Editor";
        if (settings.privacy == DiscordPresencePrivacy::full
            && settings.show_chart_name && !snapshot.chart_title.empty()) {
            payload.state = snapshot.chart_title;
            if (settings.show_difficulty_mania) {
                append_piece(payload.state, snapshot.difficulty);
                append_piece(payload.state, mania_text(snapshot.key_count));
            }
            if (snapshot.logical_note_total != 0U && settings.show_note_counter) {
                append_piece(payload.state, std::to_string(snapshot.logical_note_total) + " notes");
            }
        } else {
            payload.state = "Editing a chart";
        }
    } else {
        payload.details = activity_details(snapshot.activity);
        payload.state = snapshot.status.empty()
            ? activity_default_state(snapshot.activity)
            : snapshot.status;
    }

    // Do not send built-in Rich Presence asset keys unless the distributor
    // explicitly configures them through advanced customization. Discord asset
    // keys must exist in the application's Rich Presence assets; sending names
    // such as "launcher", "paused" or "completed" without matching Developer
    // Portal assets can make an otherwise valid activity fail validation. With
    // no explicit ActivityAssets the Social SDK/Discord client falls back to
    // the application's icon, which is the safest default for fresh installs.

    if (settings.show_remaining_time
        && settings.privacy != DiscordPresencePrivacy::minimal
        && snapshot.activity == RuntimeActivityKind::gameplay
        && !snapshot.paused && !snapshot.failed && !snapshot.completed
        && snapshot.duration_ms > snapshot.song_position_ms
        && snapshot.playback_rate > 0.0
        && std::isfinite(snapshot.duration_ms)
        && std::isfinite(snapshot.song_position_ms)
        && std::isfinite(snapshot.playback_rate)) {
        const long double remaining_ms =
            (static_cast<long double>(snapshot.duration_ms)
             - static_cast<long double>(snapshot.song_position_ms))
            / static_cast<long double>(snapshot.playback_rate);
        const long double maximum = static_cast<long double>(
            std::numeric_limits<std::uint64_t>::max() - now_unix_ms
        );
        payload.end_unix_ms = now_unix_ms + static_cast<std::uint64_t>(
            std::clamp(remaining_ms, 0.0L, maximum)
        );
    }

    if (settings.advanced_customization
        && settings.privacy != DiscordPresencePrivacy::minimal) {
        const auto render = [&](const std::string& text) {
            return render_discord_presence_template(text, snapshot, settings.privacy);
        };

        if (!settings.activity_name_template.empty()) {
            const auto custom = render(settings.activity_name_template);
            if (!custom.empty()) payload.activity_name = custom;
        }
        if (!settings.details_template.empty()) payload.details = render(settings.details_template);
        if (!settings.state_template.empty()) payload.state = render(settings.state_template);
        if (!settings.details_url_template.empty()) {
            payload.details_url = bounded_http_url(render(settings.details_url_template));
        }
        if (!settings.state_url_template.empty()) {
            payload.state_url = bounded_http_url(render(settings.state_url_template));
        }
        if (!settings.large_image_template.empty()) {
            const auto custom = bounded_asset(render(settings.large_image_template));
            if (!custom.empty()) payload.large_image_key = custom;
        }
        if (!settings.large_text_template.empty()) {
            payload.large_image_text = render(settings.large_text_template);
        }
        if (!settings.large_url_template.empty()) {
            payload.large_image_url = bounded_http_url(render(settings.large_url_template));
        }
        if (!settings.small_image_template.empty()) {
            payload.small_image_key = bounded_asset(render(settings.small_image_template));
        }
        if (!settings.small_text_template.empty()) {
            payload.small_image_text = render(settings.small_text_template);
        }
        if (!settings.small_url_template.empty()) {
            payload.small_image_url = bounded_http_url(render(settings.small_url_template));
        }

        const auto append_button = [&](const DiscordPresenceButtonSettings& source) {
            if (!source.enabled || payload.button_count >= payload.buttons.size()) return;
            auto label = truncate_utf8_bytes(render(source.label), 32U);
            auto url = bounded_http_url(render(source.url));
            if (label.empty() || url.empty()) return;
            payload.buttons[payload.button_count++] = {
                std::move(label),
                std::move(url),
            };
        };
        append_button(settings.button1);
        append_button(settings.button2);
    }

    payload.activity_name = truncate_utf8_bytes(std::move(payload.activity_name), 128U);
    payload.details = truncate_utf8_bytes(std::move(payload.details), 128U);
    payload.state = truncate_utf8_bytes(std::move(payload.state), 128U);
    payload.large_image_text = truncate_utf8_bytes(
        std::move(payload.large_image_text), 128U
    );
    payload.small_image_text = truncate_utf8_bytes(
        std::move(payload.small_image_text), 128U
    );
    return payload;
}

}  // namespace pulseforge
