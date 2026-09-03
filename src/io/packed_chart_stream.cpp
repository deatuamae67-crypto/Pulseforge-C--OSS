#include "pulseforge/packed_chart_stream.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pulseforge {
namespace {

constexpr std::array<std::byte, 8U> pfc_magic{
    static_cast<std::byte>(0x50U), static_cast<std::byte>(0x46U),
    static_cast<std::byte>(0x43U), static_cast<std::byte>(0x31U),
    static_cast<std::byte>(0x0DU), static_cast<std::byte>(0x0AU),
    static_cast<std::byte>(0x1AU), static_cast<std::byte>(0x0AU),
};
constexpr std::uint16_t pfc_version = 1U;
constexpr std::uint16_t pfc_header_size = 192U;
constexpr std::uint32_t pfc_endian_tag = 0x01020304U;
constexpr std::uint32_t note_record_size = 25U;
constexpr std::uint32_t directory_record_size = 64U;
constexpr std::uint32_t pattern_record_size = 56U;
constexpr std::size_t header_crc_offset = 188U;

[[nodiscard]] constexpr std::array<std::uint32_t, 256U> make_crc_table() {
    std::array<std::uint32_t, 256U> table{};
    for (std::uint32_t index = 0U; index < 256U; ++index) {
        auto value = index;
        for (std::uint32_t bit = 0U; bit < 8U; ++bit) {
            value = (value & 1U) != 0U
                ? (value >> 1U) ^ 0xEDB88320U
                : value >> 1U;
        }
        table[static_cast<std::size_t>(index)] = value;
    }
    return table;
}

inline constexpr auto crc_table = make_crc_table();

class Crc32 final {
public:
    void update(const std::span<const std::byte> bytes) noexcept {
        for (const auto byte : bytes) {
            const auto index = static_cast<std::uint8_t>(
                (state_ ^ std::to_integer<std::uint8_t>(byte)) & 0xFFU
            );
            state_ = (state_ >> 8U)
                ^ crc_table[static_cast<std::size_t>(index)];
        }
    }

    [[nodiscard]] std::uint32_t finish() const noexcept {
        return state_ ^ 0xFFFFFFFFU;
    }

private:
    std::uint32_t state_{0xFFFFFFFFU};
};

[[nodiscard]] std::uint32_t crc32(
    const std::span<const std::byte> bytes
) noexcept {
    Crc32 crc;
    crc.update(bytes);
    return crc.finish();
}

[[nodiscard]] bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result
) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] bool checked_multiply(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result
) noexcept {
    if (left != 0U
        && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

void set_error(std::string* const error, const std::string_view message) {
    if (error != nullptr) {
        error->assign(message);
    }
}

[[nodiscard]] bool fail(
    std::string* const error,
    const std::string_view message
) {
    set_error(error, message);
    return false;
}

template <typename Unsigned>
void append_le(std::vector<std::byte>& output, const Unsigned value) {
    static_assert(std::is_unsigned_v<Unsigned>);
    for (std::size_t index = 0U; index < sizeof(Unsigned); ++index) {
        output.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(
            (value >> static_cast<unsigned int>(index * 8U))
                & static_cast<Unsigned>(0xFFU)
        )));
    }
}

void append_i64(std::vector<std::byte>& output, const std::int64_t value) {
    append_le(output, std::bit_cast<std::uint64_t>(value));
}

void encode_note(std::vector<std::byte>& output, const PackedNote& note) {
    append_i64(output, note.time_us);
    append_le(output, note.duration_us);
    append_le(output, note.lane);
    append_le(output, static_cast<std::uint8_t>(note.owner));
    append_le(output, note.flags);
    append_le(output, note.kind_id);
}

[[nodiscard]] std::vector<std::byte> encode_directory_record(
    const PackedChartChunkInfo& info
) {
    std::vector<std::byte> output;
    output.reserve(directory_record_size);
    append_i64(output, info.first_time_us);
    append_i64(output, info.last_time_us);
    append_le(output, info.first_note_index);
    append_le(output, info.note_count);
    append_le(output, 0U);
    append_le(output, info.file_offset);
    append_le(output, info.byte_size);
    append_le(output, info.crc32);
    append_le(output, 0U);
    append_le(output, std::uint64_t{0U});
    return output;
}

struct Header final {
    std::uint16_t key_count{};
    std::uint64_t note_count{};
    std::uint64_t logical_note_count{};
    std::uint64_t chunk_count{};
    std::uint64_t kind_count{};
    std::uint64_t pattern_count{};
    std::uint64_t dictionary_offset{};
    std::uint64_t dictionary_size{};
    std::uint64_t directory_offset{};
    std::uint64_t directory_size{};
    std::uint64_t pattern_offset{};
    std::uint64_t pattern_size{};
    std::uint64_t note_data_offset{};
    std::uint64_t note_data_size{};
    std::uint64_t file_size{};
    std::uint32_t max_notes_per_chunk{};
    std::uint32_t dictionary_crc{};
    std::uint32_t directory_crc{};
    std::uint32_t pattern_crc{};
};

[[nodiscard]] std::vector<std::byte> encode_header(const Header& header) {
    std::vector<std::byte> output;
    output.reserve(pfc_header_size);
    output.insert(output.end(), pfc_magic.begin(), pfc_magic.end());
    append_le(output, pfc_version);
    append_le(output, pfc_header_size);
    append_le(output, pfc_endian_tag);
    append_le(output, 0U); // flags
    append_le(output, header.key_count);
    append_le(output, std::uint16_t{0U});
    append_le(output, header.note_count);
    append_le(output, header.logical_note_count);
    append_le(output, header.chunk_count);
    append_le(output, header.kind_count);
    append_le(output, header.pattern_count);
    append_le(output, header.dictionary_offset);
    append_le(output, header.dictionary_size);
    append_le(output, header.directory_offset);
    append_le(output, header.directory_size);
    append_le(output, header.pattern_offset);
    append_le(output, header.pattern_size);
    append_le(output, header.note_data_offset);
    append_le(output, header.note_data_size);
    append_le(output, header.file_size);
    append_le(output, header.max_notes_per_chunk);
    append_le(output, note_record_size);
    append_le(output, directory_record_size);
    append_le(output, pattern_record_size);
    append_le(output, header.dictionary_crc);
    append_le(output, header.directory_crc);
    append_le(output, header.pattern_crc);
    output.resize(header_crc_offset, std::byte{});
    append_le(output, crc32(output));
    return output;
}

[[nodiscard]] bool stream_owner_is_valid(
    const PackedNoteOwner owner
) noexcept {
    return static_cast<std::uint8_t>(owner)
        <= static_cast<std::uint8_t>(PackedNoteOwner::player);
}

[[nodiscard]] bool stream_to_size(
    const std::uint64_t value,
    std::size_t& result
) noexcept {
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        if (value > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            return false;
        }
    }
    result = static_cast<std::size_t>(value);
    return true;
}

[[nodiscard]] bool build_stream_patterns(
    const PackedChartStreamSpec& spec,
    const PackedChartLimits& limits,
    std::uint64_t& logical_note_count,
    std::vector<std::byte>& output,
    std::string* const error
) {
    const auto pattern_count = static_cast<std::uint64_t>(
        spec.patterns.size()
    );
    if (pattern_count > limits.max_patterns) {
        return fail(error, "streamed PFC1 has too many pattern runs");
    }

    std::uint64_t fixed_size{};
    if (!checked_multiply(pattern_count, pattern_record_size, fixed_size)) {
        return fail(error, "streamed PFC1 pattern directory size overflows");
    }

    logical_note_count = spec.explicit_note_count;
    if (logical_note_count > limits.max_logical_notes) {
        return fail(error, "streamed PFC1 logical note count exceeds its limit");
    }

    std::uint64_t total_lanes{};
    for (const auto& pattern : spec.patterns) {
        if (pattern.count == 0U) {
            return fail(error, "streamed PFC1 pattern cannot be empty");
        }
        if (pattern.lane_pattern.empty()) {
            return fail(error, "streamed PFC1 pattern requires lanes");
        }
        if (pattern.lane_pattern.size()
            > std::numeric_limits<std::uint32_t>::max()) {
            return fail(error, "streamed PFC1 lane pattern is too large");
        }
        if (!stream_owner_is_valid(pattern.owner)) {
            return fail(error, "streamed PFC1 pattern owner is invalid");
        }
        if (pattern.interval_denominator == 0U) {
            return fail(error, "streamed PFC1 pattern interval denominator is zero");
        }
        if (pattern.kind_id >= spec.kinds.size()) {
            return fail(error, "streamed PFC1 pattern kind is invalid");
        }
        for (const auto lane : pattern.lane_pattern) {
            if (lane >= spec.key_count) {
                return fail(error, "streamed PFC1 pattern lane is invalid");
            }
        }
        if (!pattern.note_at(pattern.count - 1U).has_value()) {
            return fail(error, "streamed PFC1 pattern time arithmetic overflows");
        }
        if (!checked_add(logical_note_count, pattern.count, logical_note_count)
            || logical_note_count > limits.max_logical_notes) {
            return fail(error, "streamed PFC1 logical note count exceeds its limit");
        }
        if (!checked_add(
                total_lanes,
                static_cast<std::uint64_t>(pattern.lane_pattern.size()),
                total_lanes
            )
            || total_lanes > limits.max_pattern_lanes) {
            return fail(error, "streamed PFC1 pattern lanes exceed their limit");
        }
    }

    std::uint64_t lane_bytes{};
    std::uint64_t total_size{};
    if (!checked_multiply(total_lanes, 2U, lane_bytes)
        || !checked_add(fixed_size, lane_bytes, total_size)
        || total_size > limits.max_pattern_bytes) {
        return fail(error, "streamed PFC1 pattern section exceeds its limit");
    }

    std::size_t total_size_native{};
    std::size_t fixed_size_native{};
    if (!stream_to_size(total_size, total_size_native)
        || !stream_to_size(fixed_size, fixed_size_native)) {
        return fail(error, "streamed PFC1 pattern section cannot fit in memory");
    }

    std::vector<std::byte> records;
    std::vector<std::byte> lanes;
    records.reserve(fixed_size_native);
    lanes.reserve(total_size_native - fixed_size_native);

    std::uint64_t lane_offset = fixed_size;
    for (const auto& pattern : spec.patterns) {
        append_i64(records, pattern.start_us);
        append_le(records, pattern.interval_us);
        append_le(records, pattern.count);
        append_le(records, pattern.duration_us);
        append_le(records, pattern.kind_id);
        append_le(records, pattern.flags);
        append_le(records, static_cast<std::uint8_t>(pattern.owner));
        const bool rational_interval = pattern.interval_denominator != 1U;
        append_le(
            records,
            static_cast<std::uint8_t>(rational_interval ? 1U : 0U)
        );
        append_le(
            records,
            static_cast<std::uint32_t>(pattern.lane_pattern.size())
        );
        append_le(
            records,
            rational_interval ? pattern.interval_denominator : 0U
        );
        append_le(records, lane_offset);

        for (const auto lane : pattern.lane_pattern) {
            append_le(lanes, lane);
        }
        lane_offset += static_cast<std::uint64_t>(
            pattern.lane_pattern.size()
        ) * 2U;
    }

    output = std::move(records);
    output.insert(output.end(), lanes.begin(), lanes.end());
    return true;
}

[[nodiscard]] bool write_bytes(
    std::ofstream& output,
    const std::span<const std::byte> bytes,
    std::string* const error
) {
    if (bytes.size() > static_cast<std::size_t>(
            std::numeric_limits<std::streamsize>::max())) {
        return fail(error, "streamed packed-chart write is too large");
    }
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    return output
        ? true
        : fail(error, "failed to write streamed packed chart");
}

[[nodiscard]] bool write_zero_bytes(
    std::ofstream& output,
    std::uint64_t remaining,
    std::string* const error
) {
    constexpr std::size_t block_size = 64U * 1024U;
    const std::array<std::byte, block_size> zeroes{};
    while (remaining != 0U) {
        const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
            remaining,
            zeroes.size()
        ));
        if (!write_bytes(output, std::span(zeroes).first(amount), error)) {
            return false;
        }
        remaining -= static_cast<std::uint64_t>(amount);
    }
    return true;
}

[[nodiscard]] bool seek_output(
    std::ofstream& output,
    const std::uint64_t offset,
    std::string* const error
) {
    if (offset > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max())) {
        return fail(error, "streamed packed-chart offset is not seekable");
    }
    output.clear();
    output.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    return output
        ? true
        : fail(error, "failed to seek streamed packed chart");
}

[[nodiscard]] bool destination_is_absent(
    const std::filesystem::path& destination,
    std::string* const error
) {
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(
        destination,
        status_error
    );
    if (status_error == std::errc::no_such_file_or_directory) {
        return true;
    }
    if (status_error) {
        return fail(error, "failed to inspect streamed PFC1 destination");
    }
    return status.type() == std::filesystem::file_type::not_found
        ? true
        : fail(error, "streamed PFC1 destination already exists");
}

}  // namespace

class PackedChartStreamWriter::Impl final {
public:
    ~Impl() {
        if (output_.is_open()) {
            output_.close();
        }
        if (!owns_temporary_) {
            return;
        }
        std::error_code ignored;
        std::filesystem::remove(temporary_file_, ignored);
        ignored.clear();
        std::filesystem::remove(temporary_directory_, ignored);
    }

    [[nodiscard]] bool initialize(
        const std::filesystem::path& destination,
        const PackedChartStreamSpec& spec,
        const PackedChartWriteOptions& options,
        std::string* const error
    ) {
        if (destination.empty() || spec.key_count == 0U) {
            return fail(error, "streamed PFC1 destination/key count is invalid");
        }
        if (options.max_notes_per_chunk == 0U) {
            return fail(error, "streamed PFC1 chunk note limit is zero");
        }
        if (spec.explicit_note_count > options.limits.max_explicit_notes) {
            return fail(error, "streamed PFC1 explicit note count exceeds its limit");
        }
        if (spec.kinds.size() > options.limits.max_kinds
            || ((spec.explicit_note_count != 0U || !spec.patterns.empty())
                && spec.kinds.empty())) {
            return fail(error, "streamed PFC1 kind count is invalid");
        }

        std::unordered_set<std::string_view> unique;
        unique.reserve(spec.kinds.size());
        for (const auto& kind : spec.kinds) {
            if (kind.empty()
                || kind.size() > options.limits.max_kind_bytes
                || kind.size() > std::numeric_limits<std::uint32_t>::max()
                || !unique.insert(kind).second) {
                return fail(error, "streamed PFC1 kind dictionary is invalid");
            }
            std::uint64_t next_size{};
            if (!checked_add(
                    static_cast<std::uint64_t>(dictionary_.size()),
                    4U + static_cast<std::uint64_t>(kind.size()),
                    next_size
                )
                || next_size > options.limits.max_dictionary_bytes) {
                return fail(error, "streamed PFC1 dictionary exceeds its limit");
            }
            append_le(dictionary_, static_cast<std::uint32_t>(kind.size()));
            const auto* begin = reinterpret_cast<const std::byte*>(kind.data());
            dictionary_.insert(dictionary_.end(), begin, begin + kind.size());
        }

        std::uint64_t logical_note_count{};
        if (!build_stream_patterns(
                spec,
                options.limits,
                logical_note_count,
                patterns_,
                error
            )) {
            return false;
        }

        std::uint64_t maximum_chunk_bytes{};
        if (!checked_multiply(
                options.max_notes_per_chunk,
                note_record_size,
                maximum_chunk_bytes
            )
            || maximum_chunk_bytes > options.limits.max_chunk_bytes
            || maximum_chunk_bytes > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            return fail(error, "streamed PFC1 chunk exceeds its byte limit");
        }
        header_.key_count = spec.key_count;
        header_.note_count = spec.explicit_note_count;
        header_.logical_note_count = logical_note_count;
        header_.pattern_count = static_cast<std::uint64_t>(spec.patterns.size());
        header_.pattern_size = static_cast<std::uint64_t>(patterns_.size());
        header_.pattern_crc = crc32(patterns_);
        header_.chunk_count = spec.explicit_note_count == 0U
            ? 0U
            : ((spec.explicit_note_count - 1U)
                / options.max_notes_per_chunk) + 1U;
        if (header_.chunk_count > options.limits.max_chunks) {
            return fail(error, "streamed PFC1 chunk count exceeds its limit");
        }
        header_.kind_count = static_cast<std::uint64_t>(spec.kinds.size());
        header_.dictionary_offset = pfc_header_size;
        header_.dictionary_size = static_cast<std::uint64_t>(dictionary_.size());
        header_.directory_offset = header_.dictionary_offset
            + header_.dictionary_size;
        if (!checked_multiply(
                header_.chunk_count,
                directory_record_size,
                header_.directory_size
            )
            || !checked_add(
                header_.directory_offset,
                header_.directory_size,
                header_.pattern_offset
            )) {
            return fail(error, "streamed PFC1 directory geometry overflows");
        }
        if (!checked_add(
                header_.pattern_offset,
                header_.pattern_size,
                header_.note_data_offset
            )) {
            return fail(error, "streamed PFC1 pattern geometry overflows");
        }
        if (!checked_multiply(
                header_.note_count,
                note_record_size,
                header_.note_data_size
            )
            || !checked_add(
                header_.note_data_offset,
                header_.note_data_size,
                header_.file_size
            )) {
            return fail(error, "streamed PFC1 file geometry overflows uint64");
        }
        if (header_.file_size > options.limits.max_file_bytes) {
            return fail(
                error,
                "streamed PFC1 file exceeds the configured file-size policy"
            );
        }
        if (header_.file_size > static_cast<std::uint64_t>(
                std::numeric_limits<std::streamoff>::max())) {
            return fail(
                error,
                "streamed PFC1 file is not seekable on this platform"
            );
        }
        header_.max_notes_per_chunk = options.max_notes_per_chunk;
        header_.dictionary_crc = crc32(dictionary_);
        limits_ = options.limits;
        max_notes_per_chunk_ = options.max_notes_per_chunk;
        chunk_.reserve(static_cast<std::size_t>(maximum_chunk_bytes));

        std::error_code path_error;
        destination_ = std::filesystem::absolute(destination, path_error)
            .lexically_normal();
        if (path_error || destination_.filename().empty()) {
            return fail(error, "failed to resolve streamed PFC1 destination");
        }
        const auto parent = destination_.parent_path();
        const auto parent_status = std::filesystem::status(parent, path_error);
        if (path_error || !std::filesystem::is_directory(parent_status)) {
            return fail(error, "streamed PFC1 destination directory is unavailable");
        }
        if (!destination_is_absent(destination_, error)) {
            return false;
        }

        const auto nonce = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
        for (std::uint32_t attempt = 0U; attempt < 1'024U; ++attempt) {
            const auto candidate = parent / (
                ".pulseforge-pfc1-stream-" + std::to_string(nonce)
                + "-" + std::to_string(attempt)
            );
            path_error.clear();
            if (std::filesystem::create_directory(candidate, path_error)) {
                temporary_directory_ = candidate;
                temporary_file_ = candidate / "payload.pfc1";
                owns_temporary_ = true;
                break;
            }
            if (path_error) {
                return fail(error, "failed to create streamed PFC1 temporary directory");
            }
        }
        if (!owns_temporary_) {
            return fail(error, "could not reserve streamed PFC1 temporary directory");
        }

        output_.open(temporary_file_, std::ios::binary | std::ios::trunc);
        if (!output_
            || !write_zero_bytes(output_, pfc_header_size, error)
            || !write_bytes(output_, dictionary_, error)
            || !write_zero_bytes(output_, header_.directory_size, error)
            || !write_bytes(output_, patterns_, error)) {
            return false;
        }
        initialized_ = true;
        return true;
    }

    [[nodiscard]] bool append(
        const PackedNote& note,
        std::string* const error
    ) {
        if (!initialized_ || finished_ || failed_) {
            return fail(error, "streamed PFC1 writer is not appendable");
        }
        if (received_ >= header_.note_count) {
            failed_ = true;
            return fail(error, "streamed PFC1 received too many notes");
        }
        if (static_cast<std::uint8_t>(note.owner)
                > static_cast<std::uint8_t>(PackedNoteOwner::player)
            || note.lane >= header_.key_count
            || note.kind_id >= header_.kind_count) {
            failed_ = true;
            return fail(error, "streamed PFC1 note fields are invalid");
        }
        if (last_time_.has_value() && *last_time_ > note.time_us) {
            failed_ = true;
            return fail(error, "streamed PFC1 notes are not time-sorted");
        }
        if (chunk_note_count_ == 0U) {
            chunk_first_time_ = note.time_us;
        }
        encode_note(chunk_, note);
        ++chunk_note_count_;
        ++received_;
        last_time_ = note.time_us;
        if (chunk_note_count_ == max_notes_per_chunk_) {
            return flush_chunk(error);
        }
        return true;
    }

    [[nodiscard]] bool finish(std::string* const error) {
        if (!initialized_ || finished_ || failed_) {
            return fail(error, "streamed PFC1 writer cannot be finished");
        }
        if (received_ != header_.note_count) {
            failed_ = true;
            return fail(error, "streamed PFC1 received the wrong note count");
        }
        if (!flush_chunk(error) || chunks_written_ != header_.chunk_count) {
            failed_ = true;
            if (error != nullptr && error->empty()) {
                *error = "streamed PFC1 wrote the wrong chunk count";
            }
            return false;
        }
        header_.directory_crc = directory_crc_.finish();
        const auto encoded_header = encode_header(header_);
        if (!seek_output(output_, 0U, error)
            || !write_bytes(output_, encoded_header, error)) {
            failed_ = true;
            return false;
        }
        output_.flush();
        if (!output_) {
            failed_ = true;
            return fail(error, "failed to flush streamed PFC1");
        }
        output_.close();
        if (!output_) {
            failed_ = true;
            return fail(error, "failed to close streamed PFC1");
        }

        std::error_code filesystem_error;
        const auto size = std::filesystem::file_size(
            temporary_file_,
            filesystem_error
        );
        if (filesystem_error || size != header_.file_size) {
            failed_ = true;
            return fail(error, "streamed PFC1 temporary file has wrong size");
        }
        if (!destination_is_absent(destination_, error)) {
            failed_ = true;
            return false;
        }
        std::filesystem::rename(
            temporary_file_,
            destination_,
            filesystem_error
        );
        if (filesystem_error) {
            failed_ = true;
            return fail(error, "failed to atomically commit streamed PFC1");
        }
        finished_ = true;
        return true;
    }

    [[nodiscard]] std::uint64_t received() const noexcept { return received_; }
    [[nodiscard]] std::uint64_t chunks() const noexcept { return chunks_written_; }

private:
    [[nodiscard]] bool flush_chunk(std::string* const error) {
        if (chunk_note_count_ == 0U) {
            return true;
        }
        const auto first_note_index = received_ - chunk_note_count_;
        std::uint64_t relative_note_offset{};
        std::uint64_t payload_offset{};
        if (!checked_multiply(
                first_note_index,
                note_record_size,
                relative_note_offset
            )
            || !checked_add(
                header_.note_data_offset,
                relative_note_offset,
                payload_offset
            )
            || !seek_output(output_, payload_offset, error)
            || !write_bytes(output_, chunk_, error)) {
            failed_ = true;
            return false;
        }

        const PackedChartChunkInfo info{
            chunk_first_time_,
            *last_time_,
            first_note_index,
            chunk_note_count_,
            payload_offset,
            static_cast<std::uint64_t>(chunk_.size()),
            crc32(chunk_),
        };
        const auto directory = encode_directory_record(info);
        directory_crc_.update(directory);
        std::uint64_t relative_directory_offset{};
        std::uint64_t directory_offset{};
        if (!checked_multiply(
                chunks_written_,
                directory_record_size,
                relative_directory_offset
            )
            || !checked_add(
                header_.directory_offset,
                relative_directory_offset,
                directory_offset
            )
            || !seek_output(output_, directory_offset, error)
            || !write_bytes(output_, directory, error)) {
            failed_ = true;
            return false;
        }
        ++chunks_written_;
        chunk_.clear();
        chunk_note_count_ = 0U;
        return true;
    }

    PackedChartLimits limits_;
    Header header_;
    std::filesystem::path destination_;
    std::filesystem::path temporary_directory_;
    std::filesystem::path temporary_file_;
    std::ofstream output_;
    std::vector<std::byte> dictionary_;
    std::vector<std::byte> patterns_;
    std::vector<std::byte> chunk_;
    Crc32 directory_crc_;
    std::optional<std::int64_t> last_time_;
    std::uint64_t received_{};
    std::uint64_t chunks_written_{};
    std::uint32_t max_notes_per_chunk_{};
    std::uint32_t chunk_note_count_{};
    std::int64_t chunk_first_time_{};
    bool initialized_{};
    bool finished_{};
    bool failed_{};
    bool owns_temporary_{};
};

PackedChartStreamWriter::PackedChartStreamWriter() noexcept = default;
PackedChartStreamWriter::~PackedChartStreamWriter() = default;
PackedChartStreamWriter::PackedChartStreamWriter(
    PackedChartStreamWriter&&
) noexcept = default;
PackedChartStreamWriter& PackedChartStreamWriter::operator=(
    PackedChartStreamWriter&&
) noexcept = default;

PackedChartStreamWriter::PackedChartStreamWriter(
    std::unique_ptr<Impl> implementation
) : implementation_(std::move(implementation)) {}

std::optional<PackedChartStreamWriter> PackedChartStreamWriter::create(
    const std::filesystem::path& destination,
    const PackedChartStreamSpec& spec,
    const PackedChartWriteOptions& options,
    std::string* const error
) {
    if (error != nullptr) {
        error->clear();
    }
    try {
        auto implementation = std::make_unique<Impl>();
        if (!implementation->initialize(destination, spec, options, error)) {
            return std::nullopt;
        }
        return PackedChartStreamWriter(std::move(implementation));
    } catch (const std::bad_alloc&) {
        set_error(
            error,
            "streamed PFC1 create failed: insufficient memory for bounded "
            "dictionary/pattern/chunk state"
        );
        return std::nullopt;
    } catch (const std::length_error&) {
        set_error(
            error,
            "streamed PFC1 create failed: metadata cannot fit in this "
            "process address space"
        );
        return std::nullopt;
    } catch (const std::exception& exception) {
        set_error(error, std::string("streamed PFC1 create failed: ") + exception.what());
        return std::nullopt;
    } catch (...) {
        set_error(error, "streamed PFC1 create failed");
        return std::nullopt;
    }
}

bool PackedChartStreamWriter::append(
    const PackedNote& note,
    std::string* const error
) {
    if (error != nullptr) {
        error->clear();
    }
    try {
        return implementation_ != nullptr
            ? implementation_->append(note, error)
            : fail(error, "streamed PFC1 writer is empty");
    } catch (const std::bad_alloc&) {
        set_error(error, "streamed PFC1 append failed: insufficient memory");
        return false;
    } catch (const std::exception& exception) {
        set_error(error, std::string("streamed PFC1 append failed: ") + exception.what());
        return false;
    } catch (...) {
        return fail(error, "streamed PFC1 append failed");
    }
}

bool PackedChartStreamWriter::append(
    const std::span<const PackedNote> notes,
    std::string* const error
) {
    if (error != nullptr) {
        error->clear();
    }
    for (const auto& note : notes) {
        if (!append(note, error)) {
            return false;
        }
    }
    return true;
}

bool PackedChartStreamWriter::finish(std::string* const error) {
    if (error != nullptr) {
        error->clear();
    }
    try {
        return implementation_ != nullptr
            ? implementation_->finish(error)
            : fail(error, "streamed PFC1 writer is empty");
    } catch (const std::bad_alloc&) {
        set_error(error, "streamed PFC1 finish failed: insufficient memory");
        return false;
    } catch (const std::exception& exception) {
        set_error(error, std::string("streamed PFC1 finish failed: ") + exception.what());
        return false;
    } catch (...) {
        return fail(error, "streamed PFC1 finish failed");
    }
}

std::uint64_t PackedChartStreamWriter::notes_received() const noexcept {
    return implementation_ != nullptr ? implementation_->received() : 0U;
}

std::uint64_t PackedChartStreamWriter::chunks_written() const noexcept {
    return implementation_ != nullptr ? implementation_->chunks() : 0U;
}

}  // namespace pulseforge
