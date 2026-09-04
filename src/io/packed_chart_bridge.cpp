#include "pulseforge/packed_chart_bridge.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulseforge {

namespace {

constexpr double signed_microseconds_minimum = -9'223'372'036'854'775'808.0;
constexpr double signed_microseconds_limit = 9'223'372'036'854'775'808.0;
constexpr double unsigned_microseconds_limit = 18'446'744'073'709'551'616.0;

[[nodiscard]] bool checked_add_size(
    const std::size_t left,
    const std::size_t right,
    std::size_t& result
) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] bool milliseconds_to_signed_microseconds(
    const double milliseconds,
    std::int64_t& microseconds
) noexcept {
    if (!std::isfinite(milliseconds)) {
        return false;
    }
    const auto scaled = milliseconds * 1'000.0;
    if (!std::isfinite(scaled)) {
        return false;
    }
    const auto rounded = std::round(scaled);
    if (rounded < signed_microseconds_minimum
        || rounded >= signed_microseconds_limit) {
        return false;
    }
    const auto candidate = static_cast<std::int64_t>(rounded);
    if (static_cast<double>(candidate) / 1'000.0 != milliseconds) {
        return false;
    }
    microseconds = candidate;
    return true;
}

[[nodiscard]] bool milliseconds_to_unsigned_microseconds(
    const double milliseconds,
    std::uint64_t& microseconds
) noexcept {
    if (!std::isfinite(milliseconds) || milliseconds < 0.0) {
        return false;
    }
    const auto scaled = milliseconds * 1'000.0;
    if (!std::isfinite(scaled)) {
        return false;
    }
    const auto rounded = std::round(scaled);
    if (rounded < 0.0 || rounded >= unsigned_microseconds_limit) {
        return false;
    }
    const auto candidate = static_cast<std::uint64_t>(rounded);
    if (static_cast<double>(candidate) / 1'000.0 != milliseconds) {
        return false;
    }
    microseconds = candidate;
    return true;
}

[[nodiscard]] bool signed_microseconds_to_milliseconds(
    const std::int64_t microseconds,
    double& milliseconds
) noexcept {
    const auto candidate = static_cast<double>(microseconds) / 1'000.0;
    std::int64_t restored{};
    if (!milliseconds_to_signed_microseconds(candidate, restored)
        || restored != microseconds) {
        return false;
    }
    milliseconds = candidate;
    return true;
}

[[nodiscard]] bool unsigned_microseconds_to_milliseconds(
    const std::uint64_t microseconds,
    double& milliseconds
) noexcept {
    const auto candidate = static_cast<double>(microseconds) / 1'000.0;
    std::uint64_t restored{};
    if (!milliseconds_to_unsigned_microseconds(candidate, restored)
        || restored != microseconds) {
        return false;
    }
    milliseconds = candidate;
    return true;
}

[[nodiscard]] std::string note_error(
    const std::size_t note_index,
    const std::string_view message
) {
    return std::string("note ") + std::to_string(note_index)
        + ": " + std::string(message);
}

[[nodiscard]] std::string packed_note_error(
    const std::uint64_t note_index,
    const std::string_view message
) {
    return std::string("packed note ") + std::to_string(note_index)
        + ": " + std::string(message);
}

[[nodiscard]] PackedChartStatistics reader_metadata(
    const PackedChartReader& reader
) noexcept {
    PackedChartStatistics statistics;
    statistics.key_count = reader.key_count();
    statistics.kind_count = static_cast<std::uint64_t>(reader.kinds().size());
    statistics.explicit_note_count = reader.explicit_note_count();
    statistics.logical_note_count = reader.logical_note_count();
    statistics.pattern_run_count = static_cast<std::uint64_t>(
        reader.patterns().size()
    );
    statistics.pattern_note_count = statistics.logical_note_count
        - statistics.explicit_note_count;
    statistics.chunk_count = reader.chunk_count();
    return statistics;
}

void observe_explicit_note(
    PackedChartStatistics& statistics,
    const PackedNote& note
) noexcept {
    if (!statistics.first_explicit_time_us.has_value()
        || note.time_us < *statistics.first_explicit_time_us) {
        statistics.first_explicit_time_us = note.time_us;
    }
    if (!statistics.last_explicit_time_us.has_value()
        || note.time_us > *statistics.last_explicit_time_us) {
        statistics.last_explicit_time_us = note.time_us;
    }
    if (note.owner == PackedNoteOwner::player) {
        ++statistics.player_note_count;
    } else {
        ++statistics.opponent_note_count;
    }
    if (note.duration_us != 0U) {
        ++statistics.hold_note_count;
    }
    if (note.flags != 0U) {
        ++statistics.nonzero_flag_note_count;
    }
}

[[nodiscard]] bool validate_kind_budget(
    const std::span<const std::string> kinds,
    const std::size_t max_kinds,
    const std::size_t max_kind_bytes,
    const std::size_t max_total_kind_bytes,
    std::string& error
) {
    if (kinds.size() > max_kinds) {
        error = "packed chart kind count exceeds the bridge budget";
        return false;
    }
    std::size_t total_kind_bytes{};
    for (const auto& kind : kinds) {
        if (kind.empty()) {
            error = "packed chart contains an empty note kind";
            return false;
        }
        if (kind.size() > max_kind_bytes) {
            error = "packed chart note kind exceeds the bridge byte budget";
            return false;
        }
        if (!checked_add_size(total_kind_bytes, kind.size(), total_kind_bytes)
            || total_kind_bytes > max_total_kind_bytes) {
            error = "packed chart kind dictionary exceeds the bridge byte budget";
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool metadata_time_extent(
    const PackedChartReader& reader,
    PackedChartStatistics& statistics,
    std::string& error
) {
    if (statistics.explicit_note_count == 0U) {
        if (statistics.chunk_count != 0U) {
            error = "packed chart has chunks but no explicit notes";
            return false;
        }
        return true;
    }
    if (statistics.chunk_count == 0U) {
        error = "packed chart has explicit notes but no chunks";
        return false;
    }

    const auto first = reader.chunk_info(0U, &error);
    if (!first.has_value()) {
        return false;
    }
    const auto last = reader.chunk_info(statistics.chunk_count - 1U, &error);
    if (!last.has_value()) {
        return false;
    }
    statistics.first_explicit_time_us = first->first_time_us;
    statistics.last_explicit_time_us = last->last_time_us;
    return true;
}

}  // namespace

ChartToPackedResult convert_chart_to_packed(
    const Chart& chart,
    const ChartToPackedOptions& options
) {
    ChartToPackedResult result;
    try {
        if (chart.key_count == 0U) {
            result.error = "chart key count cannot be zero";
            return result;
        }
        if (chart.notes.size() > options.max_notes) {
            result.error = "chart note count exceeds the bridge budget";
            return result;
        }
        if (!options.note_flags.empty()
            && options.note_flags.size() != chart.notes.size()) {
            result.error = "note flag sidecar size does not match chart notes";
            return result;
        }
        switch (options.payload_policy) {
        case ChartPayloadPolicy::reject_non_empty:
        case ChartPayloadPolicy::discard:
            break;
        default:
            result.error = "chart payload policy is invalid";
            return result;
        }

        PackedChartData packed;
        packed.key_count = chart.key_count;
        packed.notes.reserve(chart.notes.size());
        std::unordered_map<std::string_view, std::uint32_t> kind_ids;
        kind_ids.reserve(chart.notes.size());

        PackedChartStatistics statistics;
        statistics.key_count = chart.key_count;
        statistics.explicit_note_count = static_cast<std::uint64_t>(
            chart.notes.size()
        );
        statistics.logical_note_count = statistics.explicit_note_count;
        statistics.explicit_notes_scanned = true;
        std::size_t total_kind_bytes{};

        for (std::size_t index = 0U; index < chart.notes.size(); ++index) {
            const auto& source = chart.notes[index];
            if (source.lane >= chart.key_count) {
                result.error = note_error(index, "lane is outside key count");
                return result;
            }

            PackedNoteOwner owner{};
            switch (source.owner) {
            case NoteOwner::opponent:
                owner = PackedNoteOwner::opponent;
                break;
            case NoteOwner::player:
                owner = PackedNoteOwner::player;
                break;
            case NoteOwner::secondary_opponent:
                // PULSEFORGE_P1_4_0_PACKED_BRIDGE_THIRD_STRUM_V1
                // PFC1 v1 has two physical owners. Preserve the third-strum
                // identity in the kind dictionary while packing it as AI.
                owner = PackedNoteOwner::opponent;
                break;
            default:
                result.error = note_error(index, "owner is invalid");
                return result;
            }

            std::int64_t time_us{};
            std::uint64_t duration_us{};
            if (!milliseconds_to_signed_microseconds(source.time_ms, time_us)) {
                result.error = note_error(
                    index,
                    "time is not exactly representable in microseconds"
                );
                return result;
            }
            if (!milliseconds_to_unsigned_microseconds(
                    source.duration_ms,
                    duration_us
                )) {
                result.error = note_error(
                    index,
                    "duration is not exactly representable in microseconds"
                );
                return result;
            }
            if (!packed.notes.empty()
                && packed.notes.back().time_us > time_us) {
                result.error = note_error(index, "notes are not time-sorted");
                return result;
            }

            if (source.kind.empty()) {
                result.error = note_error(index, "kind cannot be empty");
                return result;
            }
            if (source.kind.size() > options.max_kind_bytes) {
                result.error = note_error(index, "kind exceeds the bridge byte budget");
                return result;
            }
            auto kind = kind_ids.find(source.kind);
            std::uint32_t kind_id{};
            if (kind == kind_ids.end()) {
                if (packed.kinds.size() >= options.max_kinds
                    || packed.kinds.size()
                        >= std::numeric_limits<std::uint32_t>::max()) {
                    result.error = "chart kind count exceeds the bridge budget";
                    return result;
                }
                if (!checked_add_size(
                        total_kind_bytes,
                        source.kind.size(),
                        total_kind_bytes
                    )
                    || total_kind_bytes > options.max_total_kind_bytes) {
                    result.error = "chart kind dictionary exceeds the bridge byte budget";
                    return result;
                }
                kind_id = static_cast<std::uint32_t>(packed.kinds.size());
                packed.kinds.push_back(source.kind);
                kind_ids.emplace(source.kind, kind_id);
            } else {
                kind_id = kind->second;
            }

            if (source.payload_id >= chart.note_payloads.size()) {
                result.error = note_error(index, "payload id is invalid");
                return result;
            }
            if (!chart.note_payloads[source.payload_id].empty()) {
                if (options.payload_policy
                    == ChartPayloadPolicy::reject_non_empty) {
                    result.error = note_error(
                        index,
                        "non-empty payload is not representable in PFC1"
                    );
                    return result;
                }
                ++statistics.discarded_payload_note_count;
            }

            const auto flags = options.note_flags.empty()
                ? std::uint16_t{}
                : options.note_flags[index];
            const PackedNote converted{
                time_us,
                duration_us,
                source.lane,
                owner,
                flags,
                kind_id,
            };
            packed.notes.push_back(converted);
            observe_explicit_note(statistics, converted);
        }

        statistics.kind_count = static_cast<std::uint64_t>(packed.kinds.size());
        result.chart = std::move(packed);
        result.statistics = statistics;
        return result;
    } catch (const std::exception& exception) {
        result.chart = {};
        result.statistics = {};
        result.error = std::string("chart to PFC1 conversion failed: ")
            + exception.what();
        return result;
    } catch (...) {
        result.chart = {};
        result.statistics = {};
        result.error = "chart to PFC1 conversion failed";
        return result;
    }
}

PackedToChartResult convert_packed_to_chart(
    const PackedChartReader& reader,
    const PackedToChartOptions& options
) {
    PackedToChartResult result;
    try {
        switch (options.pattern_policy) {
        case PackedPatternPolicy::reject:
        case PackedPatternPolicy::ignore:
            break;
        default:
            result.error = "packed pattern policy is invalid";
            return result;
        }
        if (!reader.patterns().empty()
            && options.pattern_policy == PackedPatternPolicy::reject) {
            result.error = "packed chart contains pattern runs; implicit expansion is forbidden";
            return result;
        }
        if (reader.explicit_note_count()
            > static_cast<std::uint64_t>(options.max_notes)) {
            result.error = "packed chart explicit note count exceeds the bridge budget";
            return result;
        }
        std::size_t note_count{};
        if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
            if (reader.explicit_note_count()
                > static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max()
                )) {
                result.error = "packed chart note count cannot fit in memory";
                return result;
            }
        }
        note_count = static_cast<std::size_t>(reader.explicit_note_count());
        if (!validate_kind_budget(
                reader.kinds(),
                options.max_kinds,
                options.max_kind_bytes,
                options.max_total_kind_bytes,
                result.error
            )) {
            return result;
        }

        Chart chart;
        chart.key_count = reader.key_count();
        chart.tempos.push_back(TempoChange{});
        chart.notes.reserve(note_count);
        std::vector<std::uint16_t> flags;
        flags.reserve(note_count);
        auto statistics = reader_metadata(reader);
        statistics.first_explicit_time_us.reset();
        statistics.last_explicit_time_us.reset();
        statistics.explicit_notes_scanned = true;

        std::uint64_t global_note_index{};
        for (std::uint64_t chunk_index = 0U;
             chunk_index < reader.chunk_count();
             ++chunk_index) {
            const auto chunk = reader.read_chunk(chunk_index);
            if (!chunk) {
                result.error = std::string("failed to read packed chart chunk: ")
                    + chunk.error;
                return result;
            }
            for (const auto& source : chunk.notes) {
                if (source.kind_id >= reader.kinds().size()) {
                    result.error = packed_note_error(
                        global_note_index,
                        "kind id is invalid"
                    );
                    return result;
                }
                double time_ms{};
                double duration_ms{};
                if (!signed_microseconds_to_milliseconds(
                        source.time_us,
                        time_ms
                    )) {
                    result.error = packed_note_error(
                        global_note_index,
                        "time is not exactly representable in milliseconds"
                    );
                    return result;
                }
                if (!unsigned_microseconds_to_milliseconds(
                        source.duration_us,
                        duration_ms
                    )) {
                    result.error = packed_note_error(
                        global_note_index,
                        "duration is not exactly representable in milliseconds"
                    );
                    return result;
                }

                NoteOwner owner{};
                switch (source.owner) {
                case PackedNoteOwner::opponent:
                    owner = NoteOwner::opponent;
                    break;
                case PackedNoteOwner::player:
                    owner = NoteOwner::player;
                    break;
                default:
                    result.error = packed_note_error(
                        global_note_index,
                        "owner is invalid"
                    );
                    return result;
                }
                const auto& kind = reader.kinds()[source.kind_id];
                if (owner == NoteOwner::opponent && kind == "Third Strum") {
                    // PULSEFORGE_P1_4_0_PACKED_BRIDGE_THIRD_STRUM_V1
                    owner = NoteOwner::secondary_opponent;
                    chart.secondary_opponent_enabled = true;
                }
                chart.notes.push_back(Note{
                    time_ms,
                    duration_ms,
                    source.lane,
                    owner,
                    kind,
                    0U,
                });
                flags.push_back(source.flags);
                observe_explicit_note(statistics, source);
                ++global_note_index;
            }
        }
        if (global_note_index != reader.explicit_note_count()) {
            result.error = "packed chart chunks do not match explicit note count";
            return result;
        }

        result.chart = std::move(chart);
        result.note_flags = std::move(flags);
        result.statistics = statistics;
        return result;
    } catch (const std::exception& exception) {
        result.chart = {};
        result.note_flags.clear();
        result.statistics = {};
        result.error = std::string("PFC1 to chart conversion failed: ")
            + exception.what();
        return result;
    } catch (...) {
        result.chart = {};
        result.note_flags.clear();
        result.statistics = {};
        result.error = "PFC1 to chart conversion failed";
        return result;
    }
}

PackedChartInspectionResult inspect_packed_chart(
    const PackedChartReader& reader,
    const PackedChartInspectionOptions& options
) {
    PackedChartInspectionResult result;
    try {
        auto statistics = reader_metadata(reader);
        if (!metadata_time_extent(reader, statistics, result.error)) {
            return result;
        }
        if (!options.scan_explicit_notes) {
            result.statistics = statistics;
            return result;
        }
        if (statistics.explicit_note_count
            > options.max_explicit_notes_to_scan) {
            result.error = "packed chart inspection exceeds its scan budget";
            return result;
        }

        statistics.first_explicit_time_us.reset();
        statistics.last_explicit_time_us.reset();
        std::uint64_t scanned{};
        for (std::uint64_t chunk_index = 0U;
             chunk_index < statistics.chunk_count;
             ++chunk_index) {
            const auto chunk = reader.read_chunk(chunk_index);
            if (!chunk) {
                result.error = std::string("failed to inspect packed chart chunk: ")
                    + chunk.error;
                return result;
            }
            for (const auto& note : chunk.notes) {
                observe_explicit_note(statistics, note);
                ++scanned;
            }
        }
        if (scanned != statistics.explicit_note_count) {
            result.error = "packed chart inspection note count is inconsistent";
            return result;
        }
        statistics.explicit_notes_scanned = true;
        result.statistics = statistics;
        return result;
    } catch (const std::exception& exception) {
        result.statistics = {};
        result.error = std::string("packed chart inspection failed: ")
            + exception.what();
        return result;
    } catch (...) {
        result.statistics = {};
        result.error = "packed chart inspection failed";
        return result;
    }
}

}  // namespace pulseforge
