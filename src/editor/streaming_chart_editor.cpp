#include "pulseforge/streaming_chart_editor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace pulseforge {
namespace {

using Json = nlohmann::json;
constexpr std::size_t maximum_editor_note_kinds = 65'536U;
constexpr std::size_t maximum_editor_note_style_bytes = 1'024U;

void assign_error(std::string* const destination, std::string message) {
    if (destination != nullptr) {
        *destination = std::move(message);
    }
}

[[nodiscard]] std::string quoted(const std::string_view value) {
    return Json(std::string(value)).dump();
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(encoded.data()),
        encoded.size()
    );
}

[[nodiscard]] std::uint64_t saturated_add(
    const std::uint64_t left,
    const std::uint64_t right
) noexcept {
    return right > std::numeric_limits<std::uint64_t>::max() - left
        ? std::numeric_limits<std::uint64_t>::max()
        : left + right;
}

[[nodiscard]] std::string_view patch_format_name(
    const ChartFormat format
) noexcept {
    switch (format) {
    case ChartFormat::native:
        return "native";
    case ChartFormat::psych:
        return "psych";
    case ChartFormat::denpa:
        return "denpa";
    case ChartFormat::vslice:
        return "vslice";
    }
    return "native";
}

[[nodiscard]] std::filesystem::path path_from_utf8(
    const std::string_view value
) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const unsigned char character : value) {
        encoded.push_back(static_cast<char8_t>(character));
    }
    return std::filesystem::path(encoded);
}

void write_string_array(
    std::ostream& output,
    const std::vector<std::string>& values
) {
    output << '[';
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        output << quoted(values[index]);
    }
    output << ']';
}

void write_packed_note(std::ostream& output, const PackedNote& note) {
    output << "{\"timeUs\":" << note.time_us
           << ",\"durationUs\":" << note.duration_us
           << ",\"lane\":" << note.lane
           << ",\"owner\":"
           << quoted(
                  note.owner == PackedNoteOwner::player
                      ? "player"
                      : "opponent"
              )
           << ",\"flags\":" << note.flags
           << ",\"kindId\":" << note.kind_id << '}';
}

[[nodiscard]] bool json_unsigned(
    const Json& object,
    const std::string_view field,
    std::uint64_t& destination,
    std::string& error
) {
    const auto found = object.find(field);
    if (found == object.end() || !found->is_number_unsigned()) {
        error = "streaming patch field '" + std::string(field)
            + "' must be an unsigned integer";
        return false;
    }
    destination = found->get<std::uint64_t>();
    return true;
}

[[nodiscard]] bool parse_packed_note(
    const Json& value,
    PackedNote& note,
    std::string& error
) {
    if (!value.is_object()) {
        error = "streaming patch note must be an object";
        return false;
    }
    const auto time = value.find("timeUs");
    const auto owner = value.find("owner");
    std::uint64_t duration{};
    std::uint64_t lane{};
    std::uint64_t flags{};
    std::uint64_t kind{};
    if (time == value.end() || !time->is_number_integer()
        || owner == value.end() || !owner->is_string()
        || !json_unsigned(value, "durationUs", duration, error)
        || !json_unsigned(value, "lane", lane, error)
        || !json_unsigned(value, "flags", flags, error)
        || !json_unsigned(value, "kindId", kind, error)) {
        if (error.empty()) {
            error = "streaming patch note has invalid required fields";
        }
        return false;
    }
    if (time->is_number_unsigned()
        && time->get<std::uint64_t>()
            > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()
            )) {
        error = "streaming patch note timestamp exceeds signed 64-bit range";
        return false;
    }
    if (lane > std::numeric_limits<std::uint16_t>::max()
        || flags > std::numeric_limits<std::uint16_t>::max()
        || kind > std::numeric_limits<std::uint32_t>::max()) {
        error = "streaming patch note integer exceeds its field width";
        return false;
    }
    const auto& owner_name = owner->get_ref<const std::string&>();
    if (owner_name == "player") {
        note.owner = PackedNoteOwner::player;
    } else if (owner_name == "opponent") {
        note.owner = PackedNoteOwner::opponent;
    } else {
        error = "streaming patch note owner is invalid";
        return false;
    }
    note.time_us = time->get<std::int64_t>();
    note.duration_us = duration;
    note.lane = static_cast<std::uint16_t>(lane);
    note.flags = static_cast<std::uint16_t>(flags);
    note.kind_id = static_cast<std::uint32_t>(kind);
    return true;
}

[[nodiscard]] std::uint64_t pattern_lower_bound(
    const PatternRun& pattern,
    const std::int64_t time_us
) noexcept {
    std::uint64_t lower = 0U;
    std::uint64_t upper = pattern.count;
    while (lower < upper) {
        const auto middle = lower + ((upper - lower) / 2U);
        const auto note = pattern.note_at(middle);
        if (!note.has_value() || note->time_us >= time_us) {
            upper = middle;
        } else {
            lower = middle + 1U;
        }
    }
    return lower;
}

[[nodiscard]] std::uint64_t pattern_upper_bound(
    const PatternRun& pattern,
    const std::int64_t time_us
) noexcept {
    std::uint64_t lower = 0U;
    std::uint64_t upper = pattern.count;
    while (lower < upper) {
        const auto middle = lower + ((upper - lower) / 2U);
        const auto note = pattern.note_at(middle);
        if (!note.has_value() || note->time_us > time_us) {
            upper = middle;
        } else {
            lower = middle + 1U;
        }
    }
    return lower;
}

void append_density_span(
    std::vector<StreamingEditorDensitySpan>& spans,
    const StreamingEditorDensitySpan span
) {
    if (span.note_count == 0U) {
        return;
    }
    constexpr std::size_t maximum_density_spans = 2'048U;
    if (spans.size() < maximum_density_spans) {
        spans.push_back(span);
        return;
    }
    auto& aggregate = spans.back();
    aggregate.first_time_us = std::min(
        aggregate.first_time_us,
        span.first_time_us
    );
    aggregate.last_time_us = std::max(
        aggregate.last_time_us,
        span.last_time_us
    );
    aggregate.note_count = saturated_add(
        aggregate.note_count,
        span.note_count
    );
}

}  // namespace

StreamingChartEditor::StreamingChartEditor(
    PackedChartReader reader,
    Chart metadata,
    std::filesystem::path source_path,
    const std::uint64_t content_end_us,
    std::vector<std::string> scripts,
    const std::uint64_t source_fingerprint
) : reader_(std::move(reader)),
    metadata_(std::move(metadata)),
    source_path_(std::move(source_path)),
    note_kinds_(reader_.kinds().begin(), reader_.kinds().end()),
    scripts_(std::move(scripts)),
    source_fingerprint_(source_fingerprint),
    content_end_us_(content_end_us) {
    metadata_.notes.clear();
    if (metadata_.tempos.empty()) {
        metadata_.tempos.push_back({0.0, 120.0, 4U, 4U});
    }
    if (metadata_.note_payloads.empty()) {
        metadata_.note_payloads.push_back("");
    }
    metadata_.key_count = reader_.key_count();
}

const Chart& StreamingChartEditor::metadata() const noexcept {
    return metadata_;
}

const std::filesystem::path& StreamingChartEditor::source_path() const noexcept {
    return source_path_;
}

const PackedChartReader& StreamingChartEditor::reader() const noexcept {
    return reader_;
}

std::span<const std::string> StreamingChartEditor::note_kinds() const noexcept {
    return note_kinds_;
}

std::uint16_t StreamingChartEditor::key_count() const noexcept {
    return reader_.key_count();
}

std::uint64_t StreamingChartEditor::explicit_source_note_count() const noexcept {
    return reader_.explicit_note_count();
}

std::uint64_t StreamingChartEditor::source_note_count() const noexcept {
    return reader_.logical_note_count();
}

std::uint64_t StreamingChartEditor::note_count() const noexcept {
    const auto retained = reader_.logical_note_count()
        - static_cast<std::uint64_t>(deleted_source_notes_.size());
    return saturated_add(
        retained,
        static_cast<std::uint64_t>(added_notes_.size())
    );
}

std::uint64_t StreamingChartEditor::content_end_us() const noexcept {
    auto result = content_end_us_;
    const auto include_note = [&result](const PackedNote& note) {
        std::uint64_t end{};
        if (note.time_us >= 0) {
            end = static_cast<std::uint64_t>(note.time_us) + note.duration_us;
        } else {
            const auto magnitude = static_cast<std::uint64_t>(
                -(note.time_us + 1)
            ) + 1U;
            end = note.duration_us > magnitude
                ? note.duration_us - magnitude
                : 0U;
        }
        result = std::max(result, end);
    };
    for (const auto& [index, note] : updated_source_notes_) {
        static_cast<void>(index);
        include_note(note);
    }
    for (const auto& [id, note] : added_notes_) {
        static_cast<void>(id);
        include_note(note);
    }
    return result;
}

bool StreamingChartEditor::dirty() const noexcept {
    return revision_ != saved_revision_;
}

std::uint64_t StreamingChartEditor::revision() const noexcept {
    return revision_;
}

const std::vector<std::string>& StreamingChartEditor::scripts() const noexcept {
    return scripts_;
}

bool StreamingChartEditor::set_note_style(
    std::string style,
    std::string* const error
) {
    if (style.empty()) {
        assign_error(error, "note skin/style cannot be empty");
        return false;
    }
    if (style.size() > maximum_editor_note_style_bytes
        || bounded_chart_text_prefix_bytes(style, style.size()) != style.size()) {
        assign_error(
            error,
            "note skin/style must be valid UTF-8 without control characters and fit 1024 bytes"
        );
        return false;
    }
    if (metadata_.note_style == style) {
        return true;
    }
    metadata_.note_style = std::move(style);
    ++revision_;
    return true;
}

std::optional<std::uint32_t> StreamingChartEditor::ensure_note_kind(
    std::string kind,
    std::string* const error
) {
    if (kind.empty()) {
        assign_error(error, "note type cannot be empty");
        return std::nullopt;
    }
    if (kind == "0") {
        kind = "normal";
    }
    if (!valid_chart_note_kind_text(kind)) {
        assign_error(
            error,
            "note type must be valid UTF-8 without control characters and fit 128 bytes"
        );
        return std::nullopt;
    }
    const auto found = std::find(note_kinds_.begin(), note_kinds_.end(), kind);
    if (found != note_kinds_.end()) {
        return static_cast<std::uint32_t>(
            std::distance(note_kinds_.begin(), found)
        );
    }
    if (note_kinds_.size() >= maximum_editor_note_kinds) {
        assign_error(error, "streaming editor note type limit has been reached");
        return std::nullopt;
    }
    note_kinds_.push_back(std::move(kind));
    ++revision_;
    return static_cast<std::uint32_t>(note_kinds_.size() - 1U);
}

StreamingEditorViewport StreamingChartEditor::query(
    const std::int64_t first_time_us,
    const std::int64_t last_time_us,
    const std::size_t maximum_individual_notes
) const {
    StreamingEditorViewport result;
    if (first_time_us > last_time_us || maximum_individual_notes == 0U) {
        result.error = "streaming editor viewport range is invalid";
        return result;
    }

    const auto source = reader_.read_indexed_explicit_notes_in_range(
        first_time_us,
        last_time_us,
        maximum_individual_notes
    );
    if (!source) {
        result.error = source.error;
        return result;
    }
    std::vector<std::uint64_t> deleted_indices(
        deleted_source_notes_.begin(),
        deleted_source_notes_.end()
    );
    std::sort(deleted_indices.begin(), deleted_indices.end());
    std::vector<std::uint64_t> updated_indices;
    updated_indices.reserve(updated_source_notes_.size());
    for (const auto& [index, note] : updated_source_notes_) {
        static_cast<void>(note);
        updated_indices.push_back(index);
    }
    std::sort(updated_indices.begin(), updated_indices.end());
    const auto count_between = [](
        const std::vector<std::uint64_t>& indices,
        const std::uint64_t first,
        const std::uint64_t one_past_last
    ) {
        return static_cast<std::uint64_t>(
            std::lower_bound(indices.begin(), indices.end(), one_past_last)
            - std::lower_bound(indices.begin(), indices.end(), first)
        );
    };
    const auto extra_count = saturated_add(
        static_cast<std::uint64_t>(updated_source_notes_.size()),
        static_cast<std::uint64_t>(added_notes_.size())
    );
    const auto reserve_count = saturated_add(
        static_cast<std::uint64_t>(maximum_individual_notes),
        extra_count
    );
    if (reserve_count <= static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max()
        )) {
        result.notes.reserve(static_cast<std::size_t>(reserve_count));
    }
    for (const auto& indexed : source.notes) {
        if (deleted_source_notes_.contains(indexed.index)
            || updated_source_notes_.contains(indexed.index)) {
            continue;
        }
        result.notes.push_back({indexed.index + 1U, indexed.note, true});
    }

    std::size_t remaining_individual_notes = maximum_individual_notes
        - std::min(maximum_individual_notes, source.notes.size());
    std::uint64_t pattern_base = reader_.explicit_note_count();
    for (const auto& pattern : reader_.patterns()) {
        const auto first = pattern_lower_bound(pattern, first_time_us);
        const auto one_past_last = pattern_upper_bound(pattern, last_time_us);
        auto occurrence = first;
        while (occurrence < one_past_last
               && remaining_individual_notes != 0U) {
            const auto global_index = pattern_base + occurrence;
            if (!deleted_source_notes_.contains(global_index)
                && !updated_source_notes_.contains(global_index)) {
                const auto note = pattern.note_at(occurrence);
                if (!note.has_value()) {
                    result.error = "PFC1 PatternRun arithmetic became invalid";
                    result.notes.clear();
                    result.density_spans.clear();
                    return result;
                }
                result.notes.push_back({global_index + 1U, *note, true});
                --remaining_individual_notes;
            }
            ++occurrence;
        }
        if (occurrence < one_past_last) {
            auto omitted = one_past_last - occurrence;
            const auto omitted_begin = pattern_base + occurrence;
            const auto omitted_end = pattern_base + one_past_last;
            omitted -= count_between(
                deleted_indices,
                omitted_begin,
                omitted_end
            );
            omitted -= count_between(
                updated_indices,
                omitted_begin,
                omitted_end
            );
            const auto first_note = pattern.note_at(occurrence);
            const auto last_note = pattern.note_at(one_past_last - 1U);
            if (!first_note.has_value() || !last_note.has_value()) {
                result.error = "PFC1 PatternRun arithmetic became invalid";
                result.notes.clear();
                result.density_spans.clear();
                return result;
            }
            append_density_span(result.density_spans, {
                first_note->time_us,
                last_note->time_us,
                omitted,
            });
        }
        pattern_base += pattern.count;
    }

    for (const auto& [index, note] : updated_source_notes_) {
        if (!deleted_source_notes_.contains(index)
            && note.time_us >= first_time_us
            && note.time_us <= last_time_us) {
            result.notes.push_back({index + 1U, note, true});
        }
    }
    for (const auto& [id, note] : added_notes_) {
        if (note.time_us >= first_time_us && note.time_us <= last_time_us) {
            result.notes.push_back({id, note, false});
        }
    }
    std::sort(
        result.notes.begin(),
        result.notes.end(),
        [](const StreamingEditorViewportNote& left,
           const StreamingEditorViewportNote& right) {
            return left.note.time_us != right.note.time_us
                ? left.note.time_us < right.note.time_us
                : left.id < right.id;
        }
    );

    if (source.truncated && reader_.chunk_count() != 0U) {
        const auto first_omitted_index = source.notes.empty()
            ? 0U
            : source.notes.back().index + 1U;

        std::uint64_t lower = 0U;
        std::uint64_t upper = reader_.chunk_count();
        while (lower < upper) {
            const auto middle = lower + ((upper - lower) / 2U);
            std::string lookup_error;
            const auto info = reader_.chunk_info(middle, &lookup_error);
            if (!info.has_value()) {
                result.error = std::move(lookup_error);
                result.notes.clear();
                return result;
            }
            if (info->last_time_us < first_time_us) {
                lower = middle + 1U;
            } else {
                upper = middle;
            }
        }
        const auto first_chunk = lower;
        upper = reader_.chunk_count();
        while (lower < upper) {
            const auto middle = lower + ((upper - lower) / 2U);
            std::string lookup_error;
            const auto info = reader_.chunk_info(middle, &lookup_error);
            if (!info.has_value()) {
                result.error = std::move(lookup_error);
                result.notes.clear();
                return result;
            }
            if (info->first_time_us <= last_time_us) {
                lower = middle + 1U;
            } else {
                upper = middle;
            }
        }
        const auto one_past_last = lower;

        for (auto chunk_index = first_chunk;
             chunk_index < one_past_last;
             ++chunk_index) {
            std::string lookup_error;
            const auto info = reader_.chunk_info(chunk_index, &lookup_error);
            if (!info.has_value()) {
                result.error = std::move(lookup_error);
                result.notes.clear();
                result.density_spans.clear();
                return result;
            }
            const auto chunk_end = info->first_note_index + info->note_count;
            const auto omitted_begin = std::max(
                info->first_note_index,
                first_omitted_index
            );
            if (omitted_begin >= chunk_end) {
                continue;
            }
            std::uint64_t omitted{};
            auto span_first = std::max(first_time_us, info->first_time_us);
            auto span_last = std::min(last_time_us, info->last_time_us);
            if (info->first_time_us < first_time_us
                || info->last_time_us > last_time_us) {
                const auto chunk = reader_.read_chunk(chunk_index);
                if (!chunk) {
                    result.error = chunk.error;
                    result.notes.clear();
                    result.density_spans.clear();
                    return result;
                }
                span_first = std::numeric_limits<std::int64_t>::max();
                span_last = std::numeric_limits<std::int64_t>::min();
                for (std::size_t offset = 0U;
                     offset < chunk.notes.size();
                     ++offset) {
                    const auto index = info->first_note_index
                        + static_cast<std::uint64_t>(offset);
                    const auto& note = chunk.notes[offset];
                    if (index < omitted_begin || note.time_us < first_time_us
                        || note.time_us > last_time_us
                        || deleted_source_notes_.contains(index)
                        || updated_source_notes_.contains(index)) {
                        continue;
                    }
                    ++omitted;
                    span_first = std::min(span_first, note.time_us);
                    span_last = std::max(span_last, note.time_us);
                }
            } else {
                omitted = chunk_end - omitted_begin;
                omitted -= count_between(
                    deleted_indices,
                    omitted_begin,
                    chunk_end
                );
                omitted -= count_between(
                    updated_indices,
                    omitted_begin,
                    chunk_end
                );
            }
            append_density_span(result.density_spans, {
                span_first,
                span_last,
                omitted,
            });
        }
    }

    result.dense_lod = source.truncated || !result.density_spans.empty();
    return result;
}

bool StreamingChartEditor::validate_note(
    const PackedNote& note,
    std::string& error
) const {
    if (note.lane >= key_count()) {
        error = "note lane is outside the chart key count";
        return false;
    }
    if (note.kind_id >= note_kinds_.size()) {
        error = "note kind is outside the editor dictionary";
        return false;
    }
    const auto maximum_duration = note.time_us >= 0
        ? static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max() - note.time_us
          )
        : static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max()
          ) + static_cast<std::uint64_t>(-(note.time_us + 1)) + 1U;
    if (note.duration_us > maximum_duration) {
        error = "note duration overflows the editor timeline";
        return false;
    }
    return true;
}

bool StreamingChartEditor::is_added_id(
    const StreamingEditorNoteId id
) const noexcept {
    return (id & added_id_mask) != 0U;
}

std::optional<std::uint64_t> StreamingChartEditor::source_index(
    const StreamingEditorNoteId id
) const noexcept {
    if (id == 0U || is_added_id(id)) {
        return std::nullopt;
    }
    const auto index = id - 1U;
    return index < reader_.logical_note_count()
        ? std::optional<std::uint64_t>(index)
        : std::nullopt;
}

std::optional<PackedNote> StreamingChartEditor::note_by_id(
    const StreamingEditorNoteId id,
    std::string* const error
) const {
    if (error != nullptr) {
        error->clear();
    }
    if (is_added_id(id)) {
        const auto found = added_notes_.find(id);
        if (found == added_notes_.end()) {
            assign_error(error, "selected added note no longer exists");
            return std::nullopt;
        }
        return found->second;
    }

    const auto index = source_index(id);
    if (!index.has_value()) {
        assign_error(error, "selected source note does not exist");
        return std::nullopt;
    }
    if (deleted_source_notes_.contains(*index)) {
        assign_error(error, "selected source note was removed");
        return std::nullopt;
    }
    if (const auto changed = updated_source_notes_.find(*index);
        changed != updated_source_notes_.end()) {
        return changed->second;
    }

    if (*index < reader_.explicit_note_count()) {
        std::uint64_t lower = 0U;
        std::uint64_t upper = reader_.chunk_count();
        while (lower < upper) {
            const auto middle = lower + ((upper - lower) / 2U);
            std::string lookup_error;
            const auto info = reader_.chunk_info(middle, &lookup_error);
            if (!info.has_value()) {
                assign_error(error, std::move(lookup_error));
                return std::nullopt;
            }
            const auto one_past_last = info->first_note_index
                + static_cast<std::uint64_t>(info->note_count);
            if (*index < info->first_note_index) {
                upper = middle;
            } else if (*index >= one_past_last) {
                lower = middle + 1U;
            } else {
                const auto chunk = reader_.read_chunk(middle);
                if (!chunk) {
                    assign_error(error, chunk.error);
                    return std::nullopt;
                }
                const auto offset = *index - info->first_note_index;
                if (offset >= chunk.notes.size()) {
                    assign_error(error, "selected source note chunk is inconsistent");
                    return std::nullopt;
                }
                return chunk.notes[static_cast<std::size_t>(offset)];
            }
        }
        assign_error(error, "selected source note chunk was not found");
        return std::nullopt;
    }

    auto pattern_index = *index - reader_.explicit_note_count();
    for (const auto& pattern : reader_.patterns()) {
        if (pattern_index < pattern.count) {
            const auto note = pattern.note_at(pattern_index);
            if (!note.has_value()) {
                assign_error(error, "selected PatternRun note is invalid");
            }
            return note;
        }
        pattern_index -= pattern.count;
    }
    assign_error(error, "selected PatternRun note was not found");
    return std::nullopt;
}

std::optional<StreamingEditorNoteId> StreamingChartEditor::add_note(
    PackedNote note,
    std::string* const error
) {
    std::string validation_error;
    if (!validate_note(note, validation_error)) {
        assign_error(error, std::move(validation_error));
        return std::nullopt;
    }
    if (next_added_id_ == std::numeric_limits<StreamingEditorNoteId>::max()) {
        assign_error(error, "streaming editor added-note id space is exhausted");
        return std::nullopt;
    }
    const auto id = next_added_id_++;
    added_notes_.emplace(id, note);
    ++revision_;
    return id;
}

bool StreamingChartEditor::update_note(
    const StreamingEditorNoteId id,
    PackedNote note,
    std::string* const error
) {
    std::string validation_error;
    if (!validate_note(note, validation_error)) {
        assign_error(error, std::move(validation_error));
        return false;
    }
    if (is_added_id(id)) {
        const auto found = added_notes_.find(id);
        if (found == added_notes_.end()) {
            assign_error(error, "added note id does not exist");
            return false;
        }
        found->second = note;
    } else {
        const auto index = source_index(id);
        if (!index.has_value()) {
            assign_error(error, "source note id does not exist");
            return false;
        }
        deleted_source_notes_.erase(*index);
        updated_source_notes_[*index] = note;
    }
    ++revision_;
    return true;
}

bool StreamingChartEditor::remove_note(
    const StreamingEditorNoteId id,
    std::string* const error
) {
    if (is_added_id(id)) {
        if (added_notes_.erase(id) == 0U) {
            assign_error(error, "added note id does not exist");
            return false;
        }
    } else {
        const auto index = source_index(id);
        if (!index.has_value()) {
            assign_error(error, "source note id does not exist");
            return false;
        }
        if (!deleted_source_notes_.insert(*index).second) {
            assign_error(error, "source note is already deleted");
            return false;
        }
        updated_source_notes_.erase(*index);
    }
    ++revision_;
    return true;
}

EditorIoResult StreamingChartEditor::load_patch(
    const EditorStorage& storage,
    const std::filesystem::path& relative_path
) {
    std::string text;
    auto read = storage.read_text(relative_path, text);
    if (!read) {
        return read;
    }

    Json root;
    try {
        root = Json::parse(text);
    } catch (const std::exception& exception) {
        return {
            EditorIoStatus::serialization_error,
            read.path,
            std::string{"invalid streaming chart patch JSON: "}
                + exception.what(),
        };
    }
    const auto format = root.is_object() ? root.find("format") : root.end();
    if (!root.is_object() || format == root.end() || !format->is_string()
        || format->get_ref<const std::string&>()
            != "pulseforge-streaming-chart-patch") {
        return {
            EditorIoStatus::validation_error,
            read.path,
            "file is not a PulseForge streaming chart patch",
        };
    }

    const auto version = root.find("version");
    if (version == root.end() || !version->is_number_unsigned()) {
        return {
            EditorIoStatus::validation_error,
            read.path,
            "streaming chart patch version is missing or invalid",
        };
    }
    const auto patch_version = version->get<std::uint64_t>();
    if (patch_version != 1U && patch_version != 2U && patch_version != 3U) {
        return {
            EditorIoStatus::validation_error,
            read.path,
            "streaming chart patch version is unsupported",
        };
    }

    std::uint64_t stored_count{};
    std::string parse_error;
    if (!json_unsigned(root, "sourceNoteCount", stored_count, parse_error)) {
        return {EditorIoStatus::validation_error, read.path, parse_error};
    }
    const auto expected_stored_count = patch_version == 1U
        ? reader_.explicit_note_count()
        : reader_.logical_note_count();
    if (stored_count != expected_stored_count) {
        return {
            EditorIoStatus::validation_error,
            read.path,
            "patch belongs to a different source chart note count",
        };
    }
    if (patch_version >= 2U) {
        std::uint64_t explicit_count{};
        std::uint64_t key_count_value{};
        if (!json_unsigned(
                root,
                "sourceExplicitNoteCount",
                explicit_count,
                parse_error
            )
            || !json_unsigned(root, "keyCount", key_count_value, parse_error)) {
            return {EditorIoStatus::validation_error, read.path, parse_error};
        }
        if (explicit_count != reader_.explicit_note_count()
            || key_count_value != reader_.key_count()) {
            return {
                EditorIoStatus::validation_error,
                read.path,
                "patch source PFC1 shape does not match this chart",
            };
        }
        const auto fingerprint = root.find("sourceFingerprint");
        if (source_fingerprint_ != 0U
            && (fingerprint == root.end()
                || !fingerprint->is_number_unsigned()
                || fingerprint->get<std::uint64_t>()
                    != source_fingerprint_)) {
            return {
                EditorIoStatus::validation_error,
                read.path,
                "patch source fingerprint does not match this chart",
            };
        }
        std::uint64_t pattern_count{};
        const auto kinds = root.find("sourceKinds");
        if (!json_unsigned(
                root,
                "sourcePatternCount",
                pattern_count,
                parse_error
            )
            || kinds == root.end() || !kinds->is_array()
            || pattern_count != reader_.patterns().size()
            || kinds->size() != reader_.kinds().size()) {
            return {
                EditorIoStatus::validation_error,
                read.path,
                parse_error.empty()
                    ? "patch source PFC1 dictionary does not match this chart"
                    : parse_error,
            };
        }
        for (std::size_t index = 0U; index < kinds->size(); ++index) {
            if (!(*kinds)[index].is_string()
                || (*kinds)[index].get_ref<const std::string&>()
                    != reader_.kinds()[index]) {
                return {
                    EditorIoStatus::validation_error,
                    read.path,
                    "patch source PFC1 kinds do not match this chart",
                };
            }
        }
    }

    std::vector<std::string> restored_note_kinds(
        reader_.kinds().begin(),
        reader_.kinds().end()
    );
    if (patch_version >= 3U) {
        const auto kinds = root.find("noteKinds");
        if (kinds == root.end() || !kinds->is_array()
            || kinds->size() < reader_.kinds().size()
            || kinds->size() > maximum_editor_note_kinds) {
            return {
                EditorIoStatus::validation_error,
                read.path,
                "streaming patch editable note-type dictionary is invalid",
            };
        }
        restored_note_kinds.clear();
        restored_note_kinds.reserve(kinds->size());
        std::unordered_set<std::string> unique_kinds;
        unique_kinds.reserve(kinds->size());
        for (std::size_t index = 0U; index < kinds->size(); ++index) {
            if (!(*kinds)[index].is_string()) {
                parse_error = "streaming patch note type must be a string";
                break;
            }
            auto kind = (*kinds)[index].get<std::string>();
            if (kind.empty() || kind.size() > maximum_chart_note_kind_bytes
                || !unique_kinds.insert(kind).second
                || (index < reader_.kinds().size()
                    && kind != reader_.kinds()[index])) {
                parse_error = "streaming patch note-type dictionary is invalid";
                break;
            }
            restored_note_kinds.push_back(std::move(kind));
        }
        if (!parse_error.empty()) {
            return {EditorIoStatus::validation_error, read.path, parse_error};
        }
    }

    const auto validate_restored_note = [this, &restored_note_kinds](
        const PackedNote& note,
        std::string& error
    ) {
        if (note.lane >= key_count()) {
            error = "note lane is outside the chart key count";
            return false;
        }
        if (note.kind_id >= restored_note_kinds.size()) {
            error = "note kind is outside the patch editor dictionary";
            return false;
        }
        const auto maximum_duration = note.time_us >= 0
            ? static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max() - note.time_us
              )
            : static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max()
              ) + static_cast<std::uint64_t>(-(note.time_us + 1)) + 1U;
        if (note.duration_us > maximum_duration) {
            error = "note duration overflows the editor timeline";
            return false;
        }
        return true;
    };

    const auto deleted_json = root.find("deleted");
    const auto updated_json = root.find("updated");
    const auto added_json = root.find("added");
    if (deleted_json == root.end() || !deleted_json->is_array()
        || updated_json == root.end() || !updated_json->is_array()
        || added_json == root.end() || !added_json->is_array()) {
        return {
            EditorIoStatus::validation_error,
            read.path,
            "streaming chart patch overlay arrays are missing or invalid",
        };
    }

    std::unordered_set<std::uint64_t> deleted;
    std::unordered_map<std::uint64_t, PackedNote> updated;
    std::unordered_map<StreamingEditorNoteId, PackedNote> added;
    deleted.reserve(deleted_json->size());
    updated.reserve(updated_json->size());
    added.reserve(added_json->size());

    for (const auto& value : *deleted_json) {
        if (!value.is_number_unsigned()) {
            parse_error = "deleted source note index must be unsigned";
            break;
        }
        const auto index = value.get<std::uint64_t>();
        const auto patch_limit = patch_version == 1U
            ? reader_.explicit_note_count()
            : reader_.logical_note_count();
        if (index >= patch_limit || !deleted.insert(index).second) {
            parse_error = "deleted source note index is invalid or duplicated";
            break;
        }
    }
    if (parse_error.empty()) {
        for (const auto& value : *updated_json) {
            if (!value.is_object()) {
                parse_error = "updated source note entry must be an object";
                break;
            }
            std::uint64_t index{};
            if (!json_unsigned(value, "index", index, parse_error)) {
                break;
            }
            const auto note_json = value.find("note");
            PackedNote note;
            if (index >= (patch_version == 1U
                    ? reader_.explicit_note_count()
                    : reader_.logical_note_count())
                || note_json == value.end()
                || !parse_packed_note(*note_json, note, parse_error)
                || !validate_restored_note(note, parse_error)
                || deleted.contains(index)
                || !updated.emplace(index, note).second) {
                if (parse_error.empty()) {
                    parse_error = "updated source note is invalid or duplicated";
                }
                break;
            }
        }
    }

    StreamingEditorNoteId next_added = added_id_mask | 1ULL;
    if (parse_error.empty()) {
        for (const auto& value : *added_json) {
            if (!value.is_object()) {
                parse_error = "added note entry must be an object";
                break;
            }
            std::uint64_t id{};
            if (!json_unsigned(value, "id", id, parse_error)) {
                break;
            }
            const auto note_json = value.find("note");
            PackedNote note;
            if ((id & added_id_mask) == 0U
                || id == std::numeric_limits<StreamingEditorNoteId>::max()
                || note_json == value.end()
                || !parse_packed_note(*note_json, note, parse_error)
                || !validate_restored_note(note, parse_error)
                || !added.emplace(id, note).second) {
                if (parse_error.empty()) {
                    parse_error = "added note id is invalid or duplicated";
                }
                break;
            }
            next_added = std::max(next_added, id + 1U);
        }
    }
    if (!parse_error.empty()) {
        return {EditorIoStatus::validation_error, read.path, parse_error};
    }

    Chart restored_metadata = metadata_;
    std::vector<std::string> restored_scripts = scripts_;
    if (patch_version >= 2U) {
        const auto metadata = root.find("metadata");
        if (metadata != root.end()) {
            if (!metadata->is_object()) {
                return {
                    EditorIoStatus::validation_error,
                    read.path,
                    "streaming patch metadata must be an object",
                };
            }
            const auto string_field = [&metadata](
                const char* const name,
                std::string& destination
            ) {
                const auto found = metadata->find(name);
                if (found != metadata->end() && found->is_string()) {
                    destination = found->get<std::string>();
                }
            };
            string_field("title", restored_metadata.title);
            string_field("artist", restored_metadata.artist);
            string_field("charter", restored_metadata.charter);
            string_field("difficulty", restored_metadata.difficulty);
            string_field("stage", restored_metadata.stage_id);
            string_field("player1", restored_metadata.player_character);
            string_field("player2", restored_metadata.opponent_character);
            string_field("gfVersion", restored_metadata.girlfriend_character);
            string_field("noteStyle", restored_metadata.note_style);
            if (!restored_metadata.note_style.empty()
                && (restored_metadata.note_style.size()
                        > maximum_editor_note_style_bytes
                    || bounded_chart_text_prefix_bytes(
                           restored_metadata.note_style,
                           restored_metadata.note_style.size()
                       ) != restored_metadata.note_style.size())) {
                return {
                    EditorIoStatus::validation_error,
                    read.path,
                    "streaming patch note skin/style is invalid",
                };
            }
            const auto source_format = metadata->find("sourceFormat");
            if (source_format != metadata->end()) {
                if (!source_format->is_string()) {
                    return {
                        EditorIoStatus::validation_error,
                        read.path,
                        "streaming patch source format is invalid",
                    };
                }
                const auto& value = source_format->get_ref<const std::string&>();
                if (value == "native") {
                    restored_metadata.source_format = ChartFormat::native;
                } else if (value == "psych") {
                    restored_metadata.source_format = ChartFormat::psych;
                } else if (value == "denpa") {
                    restored_metadata.source_format = ChartFormat::denpa;
                } else if (value == "vslice") {
                    restored_metadata.source_format = ChartFormat::vslice;
                } else {
                    return {
                        EditorIoStatus::validation_error,
                        read.path,
                        "streaming patch source format is unknown",
                    };
                }
            }
            const auto scroll = metadata->find("scrollSpeed");
            if (scroll != metadata->end() && scroll->is_number()) {
                const auto value = scroll->get<double>();
                if (!std::isfinite(value) || value <= 0.0) {
                    return {
                        EditorIoStatus::validation_error,
                        read.path,
                        "streaming patch scroll speed is invalid",
                    };
                }
                restored_metadata.chart_scroll_speed = value;
            }

            const auto tempos = metadata->find("tempos");
            if (tempos != metadata->end()) {
                if (!tempos->is_array()
                    || tempos->size() > maximum_chart_tempo_changes) {
                    return {
                        EditorIoStatus::validation_error,
                        read.path,
                        "streaming patch tempo list is invalid or too large",
                    };
                }
                std::vector<TempoChange> parsed_tempos;
                parsed_tempos.reserve(tempos->size());
                for (const auto& tempo : *tempos) {
                    if (!tempo.is_object()) {
                        parse_error = "streaming patch tempo must be an object";
                        break;
                    }
                    const auto time = tempo.find("timeMs");
                    const auto bpm = tempo.find("bpm");
                    const auto numerator = tempo.find("numerator");
                    const auto denominator = tempo.find("denominator");
                    if (time == tempo.end() || !time->is_number()
                        || bpm == tempo.end() || !bpm->is_number()
                        || numerator == tempo.end()
                        || !numerator->is_number_unsigned()
                        || denominator == tempo.end()
                        || !denominator->is_number_unsigned()) {
                        parse_error = "streaming patch tempo fields are invalid";
                        break;
                    }
                    const auto time_value = time->get<double>();
                    const auto bpm_value = bpm->get<double>();
                    const auto numerator_value = numerator->get<std::uint64_t>();
                    const auto denominator_value = denominator->get<std::uint64_t>();
                    if (!std::isfinite(time_value) || !std::isfinite(bpm_value)
                        || bpm_value <= 0.0 || numerator_value == 0U
                        || numerator_value > std::numeric_limits<std::uint16_t>::max()
                        || denominator_value == 0U
                        || denominator_value > std::numeric_limits<std::uint16_t>::max()) {
                        parse_error = "streaming patch tempo value is invalid";
                        break;
                    }
                    parsed_tempos.push_back({
                        time_value,
                        bpm_value,
                        static_cast<std::uint16_t>(numerator_value),
                        static_cast<std::uint16_t>(denominator_value),
                    });
                }
                if (!parse_error.empty() || parsed_tempos.empty()) {
                    return {
                        EditorIoStatus::validation_error,
                        read.path,
                        parse_error.empty()
                            ? "streaming patch needs at least one tempo"
                            : parse_error,
                    };
                }
                restored_metadata.tempos = std::move(parsed_tempos);
            }

            const auto events = metadata->find("events");
            if (events != metadata->end()) {
                if (!events->is_array()
                    || events->size() > maximum_chart_events) {
                    return {
                        EditorIoStatus::validation_error,
                        read.path,
                        "streaming patch event list is invalid or too large",
                    };
                }
                std::vector<ChartEvent> parsed_events;
                parsed_events.reserve(events->size());
                for (const auto& event : *events) {
                    if (!event.is_object()) {
                        parse_error = "streaming patch event must be an object";
                        break;
                    }
                    const auto time = event.find("timeMs");
                    const auto name = event.find("name");
                    if (time == event.end() || !time->is_number()
                        || name == event.end() || !name->is_string()) {
                        parse_error = "streaming patch event fields are invalid";
                        break;
                    }
                    ChartEvent parsed;
                    parsed.time_ms = time->get<double>();
                    parsed.name = name->get<std::string>();
                    if (!std::isfinite(parsed.time_ms)
                        || parsed.name.size() > maximum_chart_event_name_bytes) {
                        parse_error = "streaming patch event value is invalid";
                        break;
                    }
                    const auto optional_string = [&event](
                        const char* const name,
                        std::string& destination
                    ) {
                        const auto found = event.find(name);
                        if (found != event.end() && found->is_string()) {
                            destination = found->get<std::string>();
                        }
                    };
                    optional_string("value1", parsed.value1);
                    optional_string("value2", parsed.value2);
                    optional_string("payloadJson", parsed.payload_json);
                    if (parsed.value1.size() > maximum_chart_event_value_bytes
                        || parsed.value2.size() > maximum_chart_event_value_bytes
                        || parsed.payload_json.size()
                            > maximum_chart_note_payload_bytes) {
                        parse_error = "streaming patch event payload is too large";
                        break;
                    }
                    parsed_events.push_back(std::move(parsed));
                }
                if (!parse_error.empty()) {
                    return {
                        EditorIoStatus::validation_error,
                        read.path,
                        parse_error,
                    };
                }
                restored_metadata.events = std::move(parsed_events);
            }

            const auto scripts = metadata->find("scripts");
            if (scripts != metadata->end()) {
                if (!scripts->is_array() || scripts->size() > 4'096U) {
                    return {
                        EditorIoStatus::validation_error,
                        read.path,
                        "streaming patch script list is invalid or too large",
                    };
                }
                restored_scripts.clear();
                restored_scripts.reserve(scripts->size());
                for (const auto& script : *scripts) {
                    if (!script.is_string()
                        || script.get_ref<const std::string&>().size() > 4'096U) {
                        parse_error = "streaming patch script path is invalid";
                        break;
                    }
                    restored_scripts.push_back(script.get<std::string>());
                }
                if (!parse_error.empty()) {
                    return {
                        EditorIoStatus::validation_error,
                        read.path,
                        parse_error,
                    };
                }
            }

            const auto note_payloads = metadata->find("notePayloads");
            if (note_payloads != metadata->end()) {
                if (!note_payloads->is_array()
                    || note_payloads->size()
                        > static_cast<std::size_t>(
                            std::numeric_limits<std::uint32_t>::max()
                        )) {
                    return {
                        EditorIoStatus::validation_error,
                        read.path,
                        "streaming patch note-payload list is invalid",
                    };
                }
                restored_metadata.note_payloads.clear();
                restored_metadata.note_payloads.reserve(note_payloads->size());
                for (const auto& payload : *note_payloads) {
                    if (!payload.is_string()
                        || payload.get_ref<const std::string&>().size()
                            > maximum_chart_note_payload_bytes) {
                        parse_error = "streaming patch note payload is invalid";
                        break;
                    }
                    restored_metadata.note_payloads.push_back(
                        payload.get<std::string>()
                    );
                }
                if (!parse_error.empty()
                    || restored_metadata.note_payloads.empty()) {
                    return {
                        EditorIoStatus::validation_error,
                        read.path,
                        parse_error.empty()
                            ? "streaming patch note payload list cannot be empty"
                            : parse_error,
                    };
                }
            }

            const auto audio = metadata->find("audio");
            if (audio != metadata->end()) {
                if (!audio->is_object()) {
                    return {
                        EditorIoStatus::validation_error,
                        read.path,
                        "streaming patch audio manifest is invalid",
                    };
                }
                restored_metadata.audio = {};
                const auto instrumental = audio->find("instrumental");
                if (instrumental != audio->end() && instrumental->is_string()) {
                    restored_metadata.audio.instrumental = path_from_utf8(
                        instrumental->get_ref<const std::string&>()
                    );
                }
                const auto vocals = audio->find("vocals");
                if (vocals != audio->end()) {
                    if (!vocals->is_array()) {
                        return {
                            EditorIoStatus::validation_error,
                            read.path,
                            "streaming patch vocals list is invalid",
                        };
                    }
                    for (const auto& vocal : *vocals) {
                        if (!vocal.is_string()) {
                            parse_error = "streaming patch vocal path is invalid";
                            break;
                        }
                        restored_metadata.audio.vocals.push_back(
                            path_from_utf8(vocal.get_ref<const std::string&>())
                        );
                    }
                    if (!parse_error.empty()) {
                        return {
                            EditorIoStatus::validation_error,
                            read.path,
                            parse_error,
                        };
                    }
                }
            }
        }
    }

    restored_metadata.notes.clear();
    restored_metadata.key_count = reader_.key_count();
    deleted_source_notes_ = std::move(deleted);
    updated_source_notes_ = std::move(updated);
    added_notes_ = std::move(added);
    metadata_ = std::move(restored_metadata);
    note_kinds_ = std::move(restored_note_kinds);
    scripts_ = std::move(restored_scripts);
    next_added_id_ = next_added;
    ++revision_;
    saved_revision_ = revision_;
    return {EditorIoStatus::ok, read.path, "streaming chart patch loaded"};
}

EditorIoResult StreamingChartEditor::save_patch(
    const EditorStorage& storage,
    const std::filesystem::path& relative_path
) {
    auto result = storage.write_atomic(
        relative_path,
        [this](std::ostream& output, std::string& error) {
            std::vector<std::uint64_t> deleted(
                deleted_source_notes_.begin(),
                deleted_source_notes_.end()
            );
            std::sort(deleted.begin(), deleted.end());
            std::vector<std::pair<std::uint64_t, PackedNote>> updated(
                updated_source_notes_.begin(),
                updated_source_notes_.end()
            );
            std::sort(
                updated.begin(),
                updated.end(),
                [](const auto& left, const auto& right) {
                    return left.first < right.first;
                }
            );
            std::vector<std::pair<StreamingEditorNoteId, PackedNote>> added(
                added_notes_.begin(),
                added_notes_.end()
            );
            std::sort(
                added.begin(),
                added.end(),
                [](const auto& left, const auto& right) {
                    return left.first < right.first;
                }
            );

            output << std::setprecision(17);
            output << "{\"format\":\"pulseforge-streaming-chart-patch\""
                   << ",\"version\":3,\"source\":"
                   << quoted(path_utf8(source_path_))
                   << ",\"sourceNoteCount\":" << source_note_count()
                   << ",\"sourceFingerprint\":" << source_fingerprint_
                   << ",\"sourceExplicitNoteCount\":"
                   << explicit_source_note_count()
                   << ",\"keyCount\":" << key_count()
                   << ",\"sourcePatternCount\":"
                   << reader_.patterns().size()
                   << ",\"sourceKinds\":[";
            for (std::size_t index = 0U; index < reader_.kinds().size(); ++index) {
                if (index != 0U) {
                    output << ',';
                }
                output << quoted(reader_.kinds()[index]);
            }
            output << "],\"noteKinds\":[";
            for (std::size_t index = 0U; index < note_kinds_.size(); ++index) {
                if (index != 0U) {
                    output << ',';
                }
                output << quoted(note_kinds_[index]);
            }
            output << ']'
                   << ",\"metadata\":{\"title\":"
                   << quoted(metadata_.title)
                   << ",\"artist\":" << quoted(metadata_.artist)
                   << ",\"charter\":" << quoted(metadata_.charter)
                   << ",\"difficulty\":" << quoted(metadata_.difficulty)
                   << ",\"sourceFormat\":"
                   << quoted(patch_format_name(metadata_.source_format))
                   << ",\"stage\":" << quoted(metadata_.stage_id)
                   << ",\"player1\":"
                   << quoted(metadata_.player_character)
                   << ",\"player2\":"
                   << quoted(metadata_.opponent_character)
                   << ",\"gfVersion\":"
                   << quoted(metadata_.girlfriend_character)
                   << ",\"noteStyle\":" << quoted(metadata_.note_style)
                   << ",\"scrollSpeed\":"
                   << metadata_.chart_scroll_speed
                   << ",\"audio\":{\"instrumental\":"
                   << quoted(path_utf8(metadata_.audio.instrumental))
                   << ",\"vocals\":[";
            for (std::size_t index = 0U;
                 index < metadata_.audio.vocals.size();
                 ++index) {
                if (index != 0U) {
                    output << ',';
                }
                output << quoted(path_utf8(metadata_.audio.vocals[index]));
            }
            output << "]},\"tempos\":[";
            for (std::size_t index = 0U;
                 index < metadata_.tempos.size();
                 ++index) {
                if (index != 0U) {
                    output << ',';
                }
                const auto& tempo = metadata_.tempos[index];
                output << "{\"timeMs\":" << tempo.time_ms
                       << ",\"bpm\":" << tempo.bpm
                       << ",\"numerator\":" << tempo.numerator
                       << ",\"denominator\":" << tempo.denominator << '}';
            }
            output << "],\"events\":[";
            for (std::size_t index = 0U;
                 index < metadata_.events.size();
                 ++index) {
                if (index != 0U) {
                    output << ',';
                }
                const auto& event = metadata_.events[index];
                output << "{\"timeMs\":" << event.time_ms
                       << ",\"name\":" << quoted(event.name)
                       << ",\"value1\":" << quoted(event.value1)
                       << ",\"value2\":" << quoted(event.value2)
                       << ",\"payloadJson\":"
                       << quoted(event.payload_json) << '}';
            }
            output << "],\"scripts\":";
            write_string_array(output, scripts_);
            output << ",\"notePayloads\":";
            write_string_array(output, metadata_.note_payloads);
            output << "}"
                   << ",\"deleted\":[";
            for (std::size_t index = 0U; index < deleted.size(); ++index) {
                if (index != 0U) {
                    output << ',';
                }
                output << deleted[index];
            }
            output << "],\"updated\":[";
            for (std::size_t index = 0U; index < updated.size(); ++index) {
                if (index != 0U) {
                    output << ',';
                }
                output << "{\"index\":" << updated[index].first
                       << ",\"note\":";
                write_packed_note(output, updated[index].second);
                output << '}';
            }
            output << "],\"added\":[";
            for (std::size_t index = 0U; index < added.size(); ++index) {
                if (index != 0U) {
                    output << ',';
                }
                output << "{\"id\":" << added[index].first
                       << ",\"note\":";
                write_packed_note(output, added[index].second);
                output << '}';
            }
            output << "]}";
            if (!output) {
                error = "failed while streaming the large-chart patch";
                return false;
            }
            return true;
        }
    );
    if (result) {
        saved_revision_ = revision_;
    }
    return result;
}

EditorIoResult StreamingChartEditor::export_psych_json(
    const EditorStorage& storage,
    const std::filesystem::path& relative_path
) const {
    return storage.write_atomic(
        relative_path,
        [this](std::ostream& output, std::string& error) {
            std::vector<TempoChange> tempos = metadata_.tempos;
            if (tempos.empty()) {
                tempos.push_back({0.0, 120.0, 4U, 4U});
            }
            for (const auto& tempo : tempos) {
                if (!std::isfinite(tempo.time_ms)
                    || !std::isfinite(tempo.bpm) || tempo.bpm <= 0.0) {
                    error = "streaming Psych export has an invalid BPM change";
                    return false;
                }
            }
            std::stable_sort(
                tempos.begin(),
                tempos.end(),
                [](const TempoChange& left, const TempoChange& right) {
                    return left.time_ms < right.time_ms;
                }
            );
            double initial_bpm = tempos.front().bpm;
            for (const auto& tempo : tempos) {
                if (tempo.time_ms > 0.0) {
                    break;
                }
                initial_bpm = tempo.bpm;
            }
            double content_end_ms = static_cast<double>(content_end_us_)
                / 1'000.0;
            for (const auto& tempo : tempos) {
                content_end_ms = std::max(content_end_ms, tempo.time_ms);
            }
            for (const auto& event : metadata_.events) {
                content_end_ms = std::max(content_end_ms, event.time_ms);
            }
            for (const auto& [index, note] : updated_source_notes_) {
                static_cast<void>(index);
                content_end_ms = std::max(
                    content_end_ms,
                    static_cast<double>(note.time_us) / 1'000.0
                        + static_cast<double>(note.duration_us) / 1'000.0
                );
            }
            for (const auto& [id, note] : added_notes_) {
                static_cast<void>(id);
                content_end_ms = std::max(
                    content_end_ms,
                    static_cast<double>(note.time_us) / 1'000.0
                        + static_cast<double>(note.duration_us) / 1'000.0
                );
            }
            output << std::setprecision(17);
            output << "{\"song\":{\"song\":" << quoted(metadata_.title)
                   << ",\"artist\":" << quoted(metadata_.artist)
                   << ",\"charter\":" << quoted(metadata_.charter)
                   << ",\"bpm\":" << initial_bpm
                   << ",\"speed\":" << metadata_.chart_scroll_speed
                   << ",\"needsVoices\":"
                   << (metadata_.audio.vocals.empty() ? "false" : "true")
                   << ",\"player1\":"
                   << quoted(metadata_.player_character)
                   << ",\"player2\":"
                   << quoted(metadata_.opponent_character)
                   << ",\"gfVersion\":"
                   << quoted(metadata_.girlfriend_character)
                   << ",\"stage\":" << quoted(metadata_.stage_id)
                   << ",\"arrowSkin\":" << quoted(metadata_.note_style)
                   << ",\"keyCount\":" << key_count()
                   << ",\"validScore\":true"
                   << ",\"pulseforgeSourceLogicalNotes\":"
                   << source_note_count()
                   << ",\"pulseforgeDifficulty\":"
                   << quoted(metadata_.difficulty)
                   << ",\"pulseforgeSourceFormat\":"
                   << quoted(patch_format_name(metadata_.source_format))
                   << ",\"pulseforgeScripts\":";
            write_string_array(output, scripts_);
            output << ",\"pulseforgeNotePayloads\":";
            write_string_array(output, metadata_.note_payloads);
            output << ",\"pulseforgeTempos\":[";
            for (std::size_t index = 0U; index < tempos.size(); ++index) {
                if (index != 0U) {
                    output << ',';
                }
                const auto& tempo = tempos[index];
                output << "{\"timeMs\":" << tempo.time_ms
                       << ",\"bpm\":" << tempo.bpm
                       << ",\"numerator\":" << tempo.numerator
                       << ",\"denominator\":" << tempo.denominator << '}';
            }
            output << "],\"pulseforgeEvents\":[";
            for (std::size_t index = 0U;
                 index < metadata_.events.size();
                 ++index) {
                if (index != 0U) {
                    output << ',';
                }
                const auto& event = metadata_.events[index];
                output << "{\"timeMs\":" << event.time_ms
                       << ",\"name\":" << quoted(event.name)
                       << ",\"value1\":" << quoted(event.value1)
                       << ",\"value2\":" << quoted(event.value2)
                       << ",\"payloadJson\":"
                       << quoted(event.payload_json) << '}';
            }
            output << "],\"pulseforgeAudio\":{\"instrumental\":"
                   << quoted(path_utf8(metadata_.audio.instrumental))
                   << ",\"vocals\":[";
            for (std::size_t index = 0U;
                 index < metadata_.audio.vocals.size();
                 ++index) {
                if (index != 0U) {
                    output << ',';
                }
                output << quoted(path_utf8(metadata_.audio.vocals[index]));
            }
            output << "]},\"notes\":[{\"sectionNotes\":[";

            bool first_note = true;
            const auto emit_note = [&](const PackedNote& note) {
                if (!first_note) {
                    output << ',';
                }
                first_note = false;
                const auto raw_lane = static_cast<std::uint32_t>(note.lane)
                    + (note.owner == PackedNoteOwner::player
                        ? static_cast<std::uint32_t>(key_count())
                        : 0U);
                const auto kind = note.kind_id < note_kinds_.size()
                    ? std::string_view(note_kinds_[note.kind_id])
                    : std::string_view{"normal"};
                output << '['
                       << static_cast<double>(note.time_us) / 1'000.0 << ','
                       << raw_lane << ','
                       << static_cast<double>(note.duration_us) / 1'000.0 << ','
                       << quoted(kind) << ']';
            };

            for (std::uint64_t chunk_index = 0U;
                 chunk_index < reader_.chunk_count();
                 ++chunk_index) {
                const auto chunk = reader_.read_chunk(chunk_index);
                if (!chunk) {
                    error = chunk.error;
                    return false;
                }
                std::string lookup_error;
                const auto info = reader_.chunk_info(chunk_index, &lookup_error);
                if (!info.has_value()) {
                    error = std::move(lookup_error);
                    return false;
                }
                for (std::size_t offset = 0U;
                     offset < chunk.notes.size();
                     ++offset) {
                    const auto index = info->first_note_index
                        + static_cast<std::uint64_t>(offset);
                    if (deleted_source_notes_.contains(index)) {
                        continue;
                    }
                    const auto updated = updated_source_notes_.find(index);
                    emit_note(
                        updated == updated_source_notes_.end()
                            ? chunk.notes[offset]
                            : updated->second
                    );
                }
            }
            std::uint64_t pattern_base = reader_.explicit_note_count();
            for (const auto& pattern : reader_.patterns()) {
                for (std::uint64_t occurrence = 0U;
                     occurrence < pattern.count;
                     ++occurrence) {
                    const auto index = pattern_base + occurrence;
                    if (deleted_source_notes_.contains(index)) {
                        continue;
                    }
                    const auto updated = updated_source_notes_.find(index);
                    if (updated != updated_source_notes_.end()) {
                        emit_note(updated->second);
                        continue;
                    }
                    const auto note = pattern.note_at(occurrence);
                    if (!note.has_value()) {
                        error = "PFC1 PatternRun arithmetic became invalid";
                        return false;
                    }
                    emit_note(*note);
                }
                pattern_base += pattern.count;
            }
            std::vector<std::pair<StreamingEditorNoteId, PackedNote>> added(
                added_notes_.begin(),
                added_notes_.end()
            );
            std::sort(
                added.begin(),
                added.end(),
                [](const auto& left, const auto& right) {
                    return left.second.time_us != right.second.time_us
                        ? left.second.time_us < right.second.time_us
                        : left.first < right.first;
                }
            );
            for (const auto& [id, note] : added) {
                static_cast<void>(id);
                emit_note(note);
            }

            std::size_t next_tempo = 0U;
            while (next_tempo < tempos.size()
                   && tempos[next_tempo].time_ms <= 0.0) {
                ++next_tempo;
            }
            double section_start = 0.0;
            double current_bpm = initial_bpm;
            const auto next_section_end = [&]() {
                if (next_tempo < tempos.size()
                    && tempos[next_tempo].time_ms > section_start) {
                    return tempos[next_tempo].time_ms;
                }
                if (content_end_ms > section_start) {
                    return content_end_ms;
                }
                return section_start + 240'000.0 / current_bpm;
            };
            auto section_end = next_section_end();
            if (!std::isfinite(section_end) || section_end <= section_start) {
                error = "streaming Psych section generation made no progress";
                return false;
            }
            auto length_steps = (section_end - section_start)
                * current_bpm / 15'000.0;
            output << "],\"lengthInSteps\":" << length_steps
                   << ",\"mustHitSection\":false,\"gfSection\":false"
                   << ",\"altAnim\":false,\"changeBPM\":false,\"bpm\":"
                   << current_bpm << '}';
            section_start = section_end;

            while (section_start < content_end_ms
                   || next_tempo < tempos.size()) {
                bool change_bpm = false;
                while (next_tempo < tempos.size()
                       && tempos[next_tempo].time_ms <= section_start
                            + 1.0e-7) {
                    current_bpm = tempos[next_tempo].bpm;
                    change_bpm = true;
                    ++next_tempo;
                }
                section_end = next_section_end();
                if (!std::isfinite(section_end)
                    || section_end <= section_start) {
                    error = "streaming Psych section generation made no progress";
                    return false;
                }
                length_steps = (section_end - section_start)
                    * current_bpm / 15'000.0;
                output << ",{\"sectionNotes\":[],\"lengthInSteps\":"
                       << length_steps
                       << ",\"mustHitSection\":false,\"gfSection\":false"
                       << ",\"altAnim\":false,\"changeBPM\":"
                       << (change_bpm ? "true" : "false")
                       << ",\"bpm\":" << current_bpm << '}';
                section_start = section_end;
            }
            output << "],\"events\":[";
            std::vector<const ChartEvent*> events;
            events.reserve(metadata_.events.size());
            for (const auto& event : metadata_.events) {
                events.push_back(&event);
            }
            std::stable_sort(
                events.begin(),
                events.end(),
                [](const ChartEvent* const left,
                   const ChartEvent* const right) {
                    return left->time_ms < right->time_ms;
                }
            );
            std::size_t event_index = 0U;
            bool first_group = true;
            while (event_index < events.size()) {
                const auto group_time = events[event_index]->time_ms;
                if (!first_group) {
                    output << ',';
                }
                first_group = false;
                output << '[' << group_time << ",[";
                bool first_event = true;
                while (event_index < events.size()
                       && std::abs(events[event_index]->time_ms - group_time)
                            < 1.0e-7) {
                    if (!first_event) {
                        output << ',';
                    }
                    first_event = false;
                    const auto& event = *events[event_index];
                    output << '[' << quoted(event.name) << ','
                           << quoted(event.value1) << ','
                           << quoted(event.value2) << ']';
                    ++event_index;
                }
                output << "]]";
            }
            output << "]}}";
            if (!output) {
                error = "failed while streaming compatible Psych JSON";
                return false;
            }
            return true;
        }
    );
}

}  // namespace pulseforge
