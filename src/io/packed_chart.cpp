#include "pulseforge/packed_chart.hpp"

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

constexpr std::array<std::byte, 8U> packed_chart_magic{
    static_cast<std::byte>(0x50U),
    static_cast<std::byte>(0x46U),
    static_cast<std::byte>(0x43U),
    static_cast<std::byte>(0x31U),
    static_cast<std::byte>(0x0DU),
    static_cast<std::byte>(0x0AU),
    static_cast<std::byte>(0x1AU),
    static_cast<std::byte>(0x0AU),
};

constexpr std::uint16_t packed_chart_version = 1U;
constexpr std::uint16_t packed_chart_header_size = 192U;
constexpr std::uint32_t packed_chart_endian_tag = 0x01020304U;
constexpr std::uint32_t packed_note_record_size = 25U;
constexpr std::uint32_t packed_directory_record_size = 64U;
constexpr std::uint32_t packed_pattern_record_size = 56U;
constexpr std::size_t header_crc_offset = 188U;
constexpr std::size_t directory_records_per_block = 1'024U;

[[nodiscard]] constexpr std::array<std::uint32_t, 256U> make_crc32_table() {
    std::array<std::uint32_t, 256U> result{};
    for (std::uint32_t index = 0U; index < 256U; ++index) {
        std::uint32_t value = index;
        for (std::uint32_t bit = 0U; bit < 8U; ++bit) {
            value = (value & 1U) != 0U
                ? (value >> 1U) ^ 0xEDB88320U
                : value >> 1U;
        }
        result[static_cast<std::size_t>(index)] = value;
    }
    return result;
}

inline constexpr auto crc32_table = make_crc32_table();

class Crc32 final {
public:
    void update(const std::span<const std::byte> bytes) noexcept {
        for (const auto byte : bytes) {
            const auto index = static_cast<std::uint8_t>(
                (state_ ^ std::to_integer<std::uint8_t>(byte)) & 0xFFU
            );
            state_ = (state_ >> 8U)
                ^ crc32_table[static_cast<std::size_t>(index)];
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
    Crc32 checksum;
    checksum.update(bytes);
    return checksum.finish();
}

void clear_error(std::string* const error) {
    if (error != nullptr) {
        error->clear();
    }
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

[[nodiscard]] bool to_size(
    const std::uint64_t value,
    std::size_t& result
) noexcept {
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        if (value > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max()
            )) {
            return false;
        }
    }
    result = static_cast<std::size_t>(value);
    return true;
}

[[nodiscard]] bool owner_is_valid(const PackedNoteOwner owner) noexcept {
    const auto value = static_cast<std::uint8_t>(owner);
    return value <= static_cast<std::uint8_t>(PackedNoteOwner::player);
}

template <typename Unsigned>
void append_unsigned_le(
    std::vector<std::byte>& output,
    const Unsigned value
) {
    static_assert(std::is_unsigned_v<Unsigned>);
    for (std::size_t index = 0U; index < sizeof(Unsigned); ++index) {
        const auto shift = static_cast<unsigned int>(index * 8U);
        const auto octet = static_cast<std::uint8_t>(
            (value >> shift) & static_cast<Unsigned>(0xFFU)
        );
        output.push_back(static_cast<std::byte>(octet));
    }
}

void append_u8(
    std::vector<std::byte>& output,
    const std::uint8_t value
) {
    append_unsigned_le(output, value);
}

void append_u16(
    std::vector<std::byte>& output,
    const std::uint16_t value
) {
    append_unsigned_le(output, value);
}

void append_u32(
    std::vector<std::byte>& output,
    const std::uint32_t value
) {
    append_unsigned_le(output, value);
}

void append_u64(
    std::vector<std::byte>& output,
    const std::uint64_t value
) {
    append_unsigned_le(output, value);
}

void append_i64(
    std::vector<std::byte>& output,
    const std::int64_t value
) {
    append_u64(output, std::bit_cast<std::uint64_t>(value));
}

class ByteCursor final {
public:
    explicit ByteCursor(const std::span<const std::byte> bytes) noexcept
        : bytes_(bytes) {}

    [[nodiscard]] bool read_u8(std::uint8_t& value) noexcept {
        return read_unsigned(value);
    }

    [[nodiscard]] bool read_u16(std::uint16_t& value) noexcept {
        return read_unsigned(value);
    }

    [[nodiscard]] bool read_u32(std::uint32_t& value) noexcept {
        return read_unsigned(value);
    }

    [[nodiscard]] bool read_u64(std::uint64_t& value) noexcept {
        return read_unsigned(value);
    }

    [[nodiscard]] bool read_i64(std::int64_t& value) noexcept {
        std::uint64_t encoded{};
        if (!read_u64(encoded)) {
            return false;
        }
        value = std::bit_cast<std::int64_t>(encoded);
        return true;
    }

    [[nodiscard]] bool read_bytes(
        const std::size_t count,
        std::span<const std::byte>& result
    ) noexcept {
        if (count > bytes_.size() - position_) {
            return false;
        }
        result = bytes_.subspan(position_, count);
        position_ += count;
        return true;
    }

    [[nodiscard]] bool skip(const std::size_t count) noexcept {
        std::span<const std::byte> ignored;
        return read_bytes(count, ignored);
    }

    [[nodiscard]] std::size_t position() const noexcept {
        return position_;
    }

    [[nodiscard]] bool at_end() const noexcept {
        return position_ == bytes_.size();
    }

private:
    template <typename Unsigned>
    [[nodiscard]] bool read_unsigned(Unsigned& value) noexcept {
        static_assert(std::is_unsigned_v<Unsigned>);
        if (sizeof(Unsigned) > bytes_.size() - position_) {
            return false;
        }

        Unsigned decoded{};
        for (std::size_t index = 0U; index < sizeof(Unsigned); ++index) {
            const auto octet = std::to_integer<std::uint8_t>(
                bytes_[position_ + index]
            );
            const auto shift = static_cast<unsigned int>(index * 8U);
            decoded |= static_cast<Unsigned>(octet) << shift;
        }
        position_ += sizeof(Unsigned);
        value = decoded;
        return true;
    }

    std::span<const std::byte> bytes_;
    std::size_t position_{};
};

void encode_note(
    std::vector<std::byte>& output,
    const PackedNote& note
) {
    append_i64(output, note.time_us);
    append_u64(output, note.duration_us);
    append_u16(output, note.lane);
    append_u8(output, static_cast<std::uint8_t>(note.owner));
    append_u16(output, note.flags);
    append_u32(output, note.kind_id);
}

[[nodiscard]] bool decode_note(
    ByteCursor& cursor,
    PackedNote& note
) noexcept {
    std::uint8_t owner{};
    if (!cursor.read_i64(note.time_us)
        || !cursor.read_u64(note.duration_us)
        || !cursor.read_u16(note.lane)
        || !cursor.read_u8(owner)
        || !cursor.read_u16(note.flags)
        || !cursor.read_u32(note.kind_id)) {
        return false;
    }
    note.owner = static_cast<PackedNoteOwner>(owner);
    return true;
}

struct HeaderFields {
    std::uint16_t version{};
    std::uint16_t header_size{};
    std::uint32_t endian_tag{};
    std::uint32_t flags{};
    std::uint16_t key_count{};
    std::uint16_t reserved{};
    std::uint64_t explicit_note_count{};
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
    std::uint32_t note_record_size{};
    std::uint32_t directory_record_size{};
    std::uint32_t pattern_record_size{};
    std::uint32_t dictionary_crc32{};
    std::uint32_t directory_crc32{};
    std::uint32_t pattern_crc32{};
    std::uint32_t header_crc32{};
};

[[nodiscard]] std::vector<std::byte> encode_header(
    const HeaderFields& header
) {
    std::vector<std::byte> output;
    output.reserve(packed_chart_header_size);
    output.insert(
        output.end(),
        packed_chart_magic.begin(),
        packed_chart_magic.end()
    );
    append_u16(output, packed_chart_version);
    append_u16(output, packed_chart_header_size);
    append_u32(output, packed_chart_endian_tag);
    append_u32(output, 0U);
    append_u16(output, header.key_count);
    append_u16(output, 0U);
    append_u64(output, header.explicit_note_count);
    append_u64(output, header.logical_note_count);
    append_u64(output, header.chunk_count);
    append_u64(output, header.kind_count);
    append_u64(output, header.pattern_count);
    append_u64(output, header.dictionary_offset);
    append_u64(output, header.dictionary_size);
    append_u64(output, header.directory_offset);
    append_u64(output, header.directory_size);
    append_u64(output, header.pattern_offset);
    append_u64(output, header.pattern_size);
    append_u64(output, header.note_data_offset);
    append_u64(output, header.note_data_size);
    append_u64(output, header.file_size);
    append_u32(output, header.max_notes_per_chunk);
    append_u32(output, packed_note_record_size);
    append_u32(output, packed_directory_record_size);
    append_u32(output, packed_pattern_record_size);
    append_u32(output, header.dictionary_crc32);
    append_u32(output, header.directory_crc32);
    append_u32(output, header.pattern_crc32);
    output.resize(header_crc_offset, std::byte{});
    append_u32(output, crc32(std::span<const std::byte>(output)));
    return output;
}

[[nodiscard]] bool decode_header(
    const std::span<const std::byte> bytes,
    HeaderFields& header,
    std::string* const error
) {
    if (bytes.size() != packed_chart_header_size) {
        return fail(error, "packed chart header is truncated");
    }
    if (!std::equal(
            packed_chart_magic.begin(),
            packed_chart_magic.end(),
            bytes.begin()
        )) {
        return fail(error, "packed chart magic is invalid");
    }

    ByteCursor cursor(bytes);
    if (!cursor.skip(packed_chart_magic.size())
        || !cursor.read_u16(header.version)
        || !cursor.read_u16(header.header_size)
        || !cursor.read_u32(header.endian_tag)
        || !cursor.read_u32(header.flags)
        || !cursor.read_u16(header.key_count)
        || !cursor.read_u16(header.reserved)
        || !cursor.read_u64(header.explicit_note_count)
        || !cursor.read_u64(header.logical_note_count)
        || !cursor.read_u64(header.chunk_count)
        || !cursor.read_u64(header.kind_count)
        || !cursor.read_u64(header.pattern_count)
        || !cursor.read_u64(header.dictionary_offset)
        || !cursor.read_u64(header.dictionary_size)
        || !cursor.read_u64(header.directory_offset)
        || !cursor.read_u64(header.directory_size)
        || !cursor.read_u64(header.pattern_offset)
        || !cursor.read_u64(header.pattern_size)
        || !cursor.read_u64(header.note_data_offset)
        || !cursor.read_u64(header.note_data_size)
        || !cursor.read_u64(header.file_size)
        || !cursor.read_u32(header.max_notes_per_chunk)
        || !cursor.read_u32(header.note_record_size)
        || !cursor.read_u32(header.directory_record_size)
        || !cursor.read_u32(header.pattern_record_size)
        || !cursor.read_u32(header.dictionary_crc32)
        || !cursor.read_u32(header.directory_crc32)
        || !cursor.read_u32(header.pattern_crc32)) {
        return fail(error, "packed chart header fields are truncated");
    }

    while (cursor.position() < header_crc_offset) {
        std::uint8_t reserved{};
        if (!cursor.read_u8(reserved)) {
            return fail(error, "packed chart header padding is truncated");
        }
        if (reserved != 0U) {
            return fail(error, "packed chart header has non-zero reserved data");
        }
    }
    if (!cursor.read_u32(header.header_crc32) || !cursor.at_end()) {
        return fail(error, "packed chart header checksum is truncated");
    }
    if (header.header_crc32 != crc32(bytes.first(header_crc_offset))) {
        return fail(error, "packed chart header CRC32 mismatch");
    }
    return true;
}

struct DirectoryRecord {
    PackedChartChunkInfo info;
    std::uint32_t reserved_a{};
    std::uint32_t reserved_b{};
    std::uint64_t reserved_c{};
};

[[nodiscard]] std::vector<std::byte> encode_directory_record(
    const PackedChartChunkInfo& info
) {
    std::vector<std::byte> output;
    output.reserve(packed_directory_record_size);
    append_i64(output, info.first_time_us);
    append_i64(output, info.last_time_us);
    append_u64(output, info.first_note_index);
    append_u32(output, info.note_count);
    append_u32(output, 0U);
    append_u64(output, info.file_offset);
    append_u64(output, info.byte_size);
    append_u32(output, info.crc32);
    append_u32(output, 0U);
    append_u64(output, 0U);
    return output;
}

[[nodiscard]] bool decode_directory_record(
    const std::span<const std::byte> bytes,
    DirectoryRecord& record,
    std::string* const error
) {
    ByteCursor cursor(bytes);
    if (bytes.size() != packed_directory_record_size
        || !cursor.read_i64(record.info.first_time_us)
        || !cursor.read_i64(record.info.last_time_us)
        || !cursor.read_u64(record.info.first_note_index)
        || !cursor.read_u32(record.info.note_count)
        || !cursor.read_u32(record.reserved_a)
        || !cursor.read_u64(record.info.file_offset)
        || !cursor.read_u64(record.info.byte_size)
        || !cursor.read_u32(record.info.crc32)
        || !cursor.read_u32(record.reserved_b)
        || !cursor.read_u64(record.reserved_c)
        || !cursor.at_end()) {
        return fail(error, "packed chart chunk directory record is truncated");
    }
    if (record.reserved_a != 0U
        || record.reserved_b != 0U
        || record.reserved_c != 0U) {
        return fail(error, "packed chart chunk directory has reserved data");
    }
    return true;
}

[[nodiscard]] bool seek_input(
    std::ifstream& input,
    const std::uint64_t offset,
    std::string* const error
) {
    const auto maximum = static_cast<std::uint64_t>(
        std::numeric_limits<std::streamoff>::max()
    );
    if (offset > maximum) {
        return fail(error, "packed chart offset is not seekable");
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input) {
        return fail(error, "failed to seek in packed chart");
    }
    return true;
}

[[nodiscard]] bool read_exact_at(
    std::ifstream& input,
    const std::uint64_t offset,
    const std::span<std::byte> output,
    std::string* const error
) {
    if (output.empty()) {
        return true;
    }
    if (!seek_input(input, offset, error)) {
        return false;
    }
    if (output.size() > static_cast<std::size_t>(
            std::numeric_limits<std::streamsize>::max()
        )) {
        return fail(error, "packed chart read is too large for the stream");
    }
    const auto count = static_cast<std::streamsize>(output.size());
    input.read(reinterpret_cast<char*>(output.data()), count);
    if (input.gcount() != count) {
        return fail(error, "packed chart is truncated");
    }
    return true;
}

[[nodiscard]] bool seek_output(
    std::ofstream& output,
    const std::uint64_t offset,
    std::string* const error
) {
    const auto maximum = static_cast<std::uint64_t>(
        std::numeric_limits<std::streamoff>::max()
    );
    if (offset > maximum) {
        return fail(error, "packed chart output offset is not seekable");
    }
    output.clear();
    output.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!output) {
        return fail(error, "failed to seek in packed chart output");
    }
    return true;
}

[[nodiscard]] bool write_bytes(
    std::ofstream& output,
    const std::span<const std::byte> bytes,
    std::string* const error
) {
    if (bytes.empty()) {
        return true;
    }
    if (bytes.size() > static_cast<std::size_t>(
            std::numeric_limits<std::streamsize>::max()
        )) {
        return fail(error, "packed chart write is too large for the stream");
    }
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    if (!output) {
        return fail(error, "failed to write packed chart");
    }
    return true;
}

[[nodiscard]] bool write_zero_bytes(
    std::ofstream& output,
    std::uint64_t count,
    std::string* const error
) {
    constexpr std::size_t block_size = 64U * 1024U;
    const std::array<std::byte, block_size> zeros{};
    while (count != 0U) {
        const auto current = static_cast<std::size_t>(std::min<std::uint64_t>(
            count,
            static_cast<std::uint64_t>(zeros.size())
        ));
        if (!write_bytes(
                output,
                std::span<const std::byte>(zeros.data(), current),
                error
            )) {
            return false;
        }
        count -= static_cast<std::uint64_t>(current);
    }
    return true;
}

[[nodiscard]] bool read_section(
    std::ifstream& input,
    const std::uint64_t offset,
    const std::uint64_t byte_size,
    const std::uint64_t limit,
    std::vector<std::byte>& output,
    std::string* const error
) {
    if (byte_size > limit) {
        return fail(error, "packed chart section exceeds its configured limit");
    }
    std::size_t size{};
    if (!to_size(byte_size, size)) {
        return fail(error, "packed chart section cannot fit in memory");
    }
    output.resize(size);
    return read_exact_at(input, offset, output, error);
}

[[nodiscard]] bool build_dictionary(
    const PackedChartData& chart,
    const PackedChartLimits& limits,
    std::vector<std::byte>& output,
    std::string* const error
) {
    const auto kind_count = static_cast<std::uint64_t>(chart.kinds.size());
    if (kind_count > limits.max_kinds) {
        return fail(error, "packed chart has too many note kinds");
    }

    std::unordered_set<std::string_view> unique_kinds;
    unique_kinds.reserve(chart.kinds.size());
    for (const auto& kind : chart.kinds) {
        if (kind.empty()) {
            return fail(error, "packed chart note kinds cannot be empty");
        }
        if (kind.size() > limits.max_kind_bytes
            || kind.size() > std::numeric_limits<std::uint32_t>::max()) {
            return fail(error, "packed chart note kind exceeds its byte limit");
        }
        if (!unique_kinds.insert(kind).second) {
            return fail(error, "packed chart note kinds must be unique");
        }

        std::uint64_t next_size{};
        if (!checked_add(
                static_cast<std::uint64_t>(output.size()),
                4U + static_cast<std::uint64_t>(kind.size()),
                next_size
            )
            || next_size > limits.max_dictionary_bytes) {
            return fail(error, "packed chart dictionary exceeds its byte limit");
        }
        append_u32(output, static_cast<std::uint32_t>(kind.size()));
        const auto* begin = reinterpret_cast<const std::byte*>(kind.data());
        output.insert(output.end(), begin, begin + kind.size());
    }
    return true;
}

[[nodiscard]] bool build_patterns(
    const PackedChartData& chart,
    const PackedChartLimits& limits,
    const std::uint64_t explicit_note_count,
    std::uint64_t& logical_note_count,
    std::vector<std::byte>& output,
    std::string* const error
) {
    const auto pattern_count = static_cast<std::uint64_t>(
        chart.patterns.size()
    );
    if (pattern_count > limits.max_patterns) {
        return fail(error, "packed chart has too many pattern runs");
    }

    std::uint64_t fixed_size{};
    if (!checked_multiply(
            pattern_count,
            packed_pattern_record_size,
            fixed_size
        )) {
        return fail(error, "packed chart pattern directory size overflows");
    }

    logical_note_count = explicit_note_count;
    if (logical_note_count > limits.max_logical_notes) {
        return fail(error, "packed chart logical note count exceeds its limit");
    }
    std::uint64_t total_lanes{};
    for (const auto& pattern : chart.patterns) {
        if (pattern.count == 0U) {
            return fail(error, "packed chart pattern runs cannot be empty");
        }
        if (pattern.lane_pattern.empty()) {
            return fail(error, "packed chart patterns require a lane pattern");
        }
        if (pattern.lane_pattern.size()
            > std::numeric_limits<std::uint32_t>::max()) {
            return fail(error, "packed chart lane pattern is too large");
        }
        if (!owner_is_valid(pattern.owner)) {
            return fail(error, "packed chart pattern has an invalid owner");
        }
        if (pattern.interval_denominator == 0U) {
            return fail(error, "packed chart pattern interval denominator is zero");
        }
        if (pattern.kind_id >= chart.kinds.size()) {
            return fail(error, "packed chart pattern has an invalid kind id");
        }
        for (const auto lane : pattern.lane_pattern) {
            if (lane >= chart.key_count) {
                return fail(error, "packed chart pattern has an invalid lane");
            }
        }
        if (!pattern.note_at(pattern.count - 1U).has_value()) {
            return fail(error, "packed chart pattern time arithmetic overflows");
        }
        if (!checked_add(
                logical_note_count,
                pattern.count,
                logical_note_count
            )
            || logical_note_count > limits.max_logical_notes) {
            return fail(error, "packed chart logical note count exceeds its limit");
        }
        if (!checked_add(
                total_lanes,
                static_cast<std::uint64_t>(pattern.lane_pattern.size()),
                total_lanes
            )
            || total_lanes > limits.max_pattern_lanes) {
            return fail(error, "packed chart pattern lanes exceed their limit");
        }
    }

    std::uint64_t lane_bytes{};
    std::uint64_t total_size{};
    if (!checked_multiply(total_lanes, 2U, lane_bytes)
        || !checked_add(fixed_size, lane_bytes, total_size)
        || total_size > limits.max_pattern_bytes) {
        return fail(error, "packed chart pattern section exceeds its limit");
    }
    std::size_t total_size_native{};
    std::size_t fixed_size_native{};
    if (!to_size(total_size, total_size_native)
        || !to_size(fixed_size, fixed_size_native)) {
        return fail(error, "packed chart pattern section cannot fit in memory");
    }

    std::vector<std::byte> records;
    std::vector<std::byte> lanes;
    records.reserve(fixed_size_native);
    lanes.reserve(total_size_native - fixed_size_native);
    std::uint64_t lane_offset = fixed_size;
    for (const auto& pattern : chart.patterns) {
        append_i64(records, pattern.start_us);
        append_u64(records, pattern.interval_us);
        append_u64(records, pattern.count);
        append_u64(records, pattern.duration_us);
        append_u32(records, pattern.kind_id);
        append_u16(records, pattern.flags);
        append_u8(records, static_cast<std::uint8_t>(pattern.owner));
        const bool rational_interval = pattern.interval_denominator != 1U;
        append_u8(records, rational_interval ? 1U : 0U);
        append_u32(
            records,
            static_cast<std::uint32_t>(pattern.lane_pattern.size())
        );
        append_u32(
            records,
            rational_interval ? pattern.interval_denominator : 0U
        );
        append_u64(records, lane_offset);

        for (const auto lane : pattern.lane_pattern) {
            append_u16(lanes, lane);
        }
        const auto current_lane_bytes = static_cast<std::uint64_t>(
            pattern.lane_pattern.size()
        ) * 2U;
        lane_offset += current_lane_bytes;
    }

    output = std::move(records);
    output.insert(output.end(), lanes.begin(), lanes.end());
    return true;
}

[[nodiscard]] bool validate_explicit_notes(
    const PackedChartData& chart,
    const PackedChartLimits& limits,
    std::string* const error
) {
    if (chart.key_count == 0U) {
        return fail(error, "packed chart key count cannot be zero");
    }
    const auto note_count = static_cast<std::uint64_t>(chart.notes.size());
    if (note_count > limits.max_explicit_notes) {
        return fail(error, "packed chart has too many explicit notes");
    }
    for (std::size_t index = 0U; index < chart.notes.size(); ++index) {
        const auto& note = chart.notes[index];
        if (!owner_is_valid(note.owner)) {
            return fail(error, "packed chart note has an invalid owner");
        }
        if (note.lane >= chart.key_count) {
            return fail(error, "packed chart note has an invalid lane");
        }
        if (note.kind_id >= chart.kinds.size()) {
            return fail(error, "packed chart note has an invalid kind id");
        }
        if (index != 0U
            && chart.notes[index - 1U].time_us > note.time_us) {
            return fail(error, "packed chart explicit notes must be time-sorted");
        }
    }
    return true;
}

[[nodiscard]] bool parse_dictionary(
    const std::span<const std::byte> bytes,
    const std::uint64_t kind_count,
    const PackedChartLimits& limits,
    std::vector<std::string>& output,
    std::string* const error
) {
    std::size_t count{};
    if (!to_size(kind_count, count)) {
        return fail(error, "packed chart kind count cannot fit in memory");
    }
    output.clear();
    output.reserve(count);
    std::unordered_set<std::string_view> unique_kinds;
    unique_kinds.reserve(count);

    ByteCursor cursor(bytes);
    for (std::size_t index = 0U; index < count; ++index) {
        std::uint32_t length{};
        if (!cursor.read_u32(length)) {
            return fail(error, "packed chart dictionary is truncated");
        }
        if (length == 0U || length > limits.max_kind_bytes) {
            return fail(error, "packed chart dictionary kind length is invalid");
        }
        std::span<const std::byte> encoded;
        if (!cursor.read_bytes(static_cast<std::size_t>(length), encoded)) {
            return fail(error, "packed chart dictionary string is truncated");
        }
        output.emplace_back(
            reinterpret_cast<const char*>(encoded.data()),
            encoded.size()
        );
        if (!unique_kinds.insert(output.back()).second) {
            return fail(error, "packed chart dictionary contains duplicates");
        }
    }
    if (!cursor.at_end()) {
        return fail(error, "packed chart dictionary has trailing data");
    }
    return true;
}

struct PatternDiskRecord {
    std::int64_t start_us{};
    std::uint64_t interval_us{};
    std::uint64_t count{};
    std::uint64_t duration_us{};
    std::uint32_t kind_id{};
    std::uint16_t flags{};
    std::uint8_t owner{};
    std::uint8_t reserved_a{};
    std::uint32_t lane_count{};
    std::uint32_t reserved_b{};
    std::uint64_t lane_offset{};
};

[[nodiscard]] bool decode_pattern_record(
    const std::span<const std::byte> bytes,
    PatternDiskRecord& record,
    std::string* const error
) {
    ByteCursor cursor(bytes);
    if (bytes.size() != packed_pattern_record_size
        || !cursor.read_i64(record.start_us)
        || !cursor.read_u64(record.interval_us)
        || !cursor.read_u64(record.count)
        || !cursor.read_u64(record.duration_us)
        || !cursor.read_u32(record.kind_id)
        || !cursor.read_u16(record.flags)
        || !cursor.read_u8(record.owner)
        || !cursor.read_u8(record.reserved_a)
        || !cursor.read_u32(record.lane_count)
        || !cursor.read_u32(record.reserved_b)
        || !cursor.read_u64(record.lane_offset)
        || !cursor.at_end()) {
        return fail(error, "packed chart pattern record is truncated");
    }
    const bool legacy = record.reserved_a == 0U && record.reserved_b == 0U;
    const bool rational = record.reserved_a == 1U && record.reserved_b != 0U;
    if (!legacy && !rational) {
        return fail(error, "packed chart pattern record has invalid extension data");
    }
    return true;
}

[[nodiscard]] bool parse_patterns(
    const std::span<const std::byte> bytes,
    const std::uint64_t pattern_count,
    const std::uint16_t key_count,
    const std::uint64_t kind_count,
    const std::uint64_t explicit_note_count,
    const std::uint64_t expected_logical_note_count,
    const PackedChartLimits& limits,
    std::vector<PatternRun>& output,
    std::string* const error
) {
    std::uint64_t fixed_size{};
    if (!checked_multiply(
            pattern_count,
            packed_pattern_record_size,
            fixed_size
        )
        || fixed_size > bytes.size()) {
        return fail(error, "packed chart pattern records are truncated");
    }
    std::size_t pattern_count_native{};
    if (!to_size(pattern_count, pattern_count_native)) {
        return fail(error, "packed chart pattern count cannot fit in memory");
    }
    output.clear();
    output.reserve(pattern_count_native);

    std::uint64_t expected_lane_offset = fixed_size;
    std::uint64_t total_lanes{};
    std::uint64_t logical_note_count = explicit_note_count;
    for (std::uint64_t index = 0U; index < pattern_count; ++index) {
        std::uint64_t record_offset{};
        if (!checked_multiply(index, packed_pattern_record_size, record_offset)) {
            return fail(error, "packed chart pattern offset overflows");
        }
        std::size_t record_offset_native{};
        if (!to_size(record_offset, record_offset_native)) {
            return fail(error, "packed chart pattern offset cannot fit in memory");
        }

        PatternDiskRecord record;
        if (!decode_pattern_record(
                bytes.subspan(
                    record_offset_native,
                    packed_pattern_record_size
                ),
                record,
                error
            )) {
            return false;
        }
        if (record.count == 0U || record.lane_count == 0U) {
            return fail(error, "packed chart pattern run is empty");
        }
        if (record.owner
            > static_cast<std::uint8_t>(PackedNoteOwner::player)) {
            return fail(error, "packed chart pattern owner is invalid");
        }
        if (record.kind_id >= kind_count) {
            return fail(error, "packed chart pattern kind id is invalid");
        }
        if (record.lane_offset != expected_lane_offset) {
            return fail(error, "packed chart pattern lane offsets are not canonical");
        }

        std::uint64_t current_lane_bytes{};
        std::uint64_t lane_end{};
        if (!checked_multiply(record.lane_count, 2U, current_lane_bytes)
            || !checked_add(record.lane_offset, current_lane_bytes, lane_end)
            || lane_end > bytes.size()) {
            return fail(error, "packed chart pattern lane data is truncated");
        }
        if (!checked_add(total_lanes, record.lane_count, total_lanes)
            || total_lanes > limits.max_pattern_lanes) {
            return fail(error, "packed chart pattern lanes exceed their limit");
        }
        if (!checked_add(logical_note_count, record.count, logical_note_count)
            || logical_note_count > limits.max_logical_notes) {
            return fail(error, "packed chart logical note count exceeds its limit");
        }

        PatternRun pattern;
        pattern.start_us = record.start_us;
        pattern.interval_us = record.interval_us;
        pattern.count = record.count;
        pattern.duration_us = record.duration_us;
        pattern.owner = static_cast<PackedNoteOwner>(record.owner);
        pattern.flags = record.flags;
        pattern.kind_id = record.kind_id;
        pattern.interval_denominator = record.reserved_a == 1U
            ? record.reserved_b
            : 1U;
        pattern.lane_pattern.resize(record.lane_count);

        std::size_t lane_offset_native{};
        std::size_t current_lane_bytes_native{};
        if (!to_size(record.lane_offset, lane_offset_native)
            || !to_size(current_lane_bytes, current_lane_bytes_native)) {
            return fail(error, "packed chart pattern lanes cannot fit in memory");
        }
        ByteCursor lane_cursor(bytes.subspan(
            lane_offset_native,
            current_lane_bytes_native
        ));
        for (auto& lane : pattern.lane_pattern) {
            if (!lane_cursor.read_u16(lane)) {
                return fail(error, "packed chart pattern lane data is truncated");
            }
            if (lane >= key_count) {
                return fail(error, "packed chart pattern lane is invalid");
            }
        }
        if (!lane_cursor.at_end()
            || !pattern.note_at(pattern.count - 1U).has_value()) {
            return fail(error, "packed chart pattern time arithmetic overflows");
        }
        output.push_back(std::move(pattern));
        expected_lane_offset = lane_end;
    }

    if (expected_lane_offset != bytes.size()) {
        return fail(error, "packed chart pattern section has trailing data");
    }
    if (logical_note_count != expected_logical_note_count) {
        return fail(error, "packed chart logical note count is inconsistent");
    }
    return true;
}

[[nodiscard]] bool read_directory_record_at(
    std::ifstream& input,
    const std::uint64_t directory_offset,
    const std::uint64_t chunk_index,
    DirectoryRecord& record,
    std::string* const error
) {
    std::uint64_t relative{};
    std::uint64_t offset{};
    if (!checked_multiply(
            chunk_index,
            packed_directory_record_size,
            relative
        )
        || !checked_add(directory_offset, relative, offset)) {
        return fail(error, "packed chart chunk directory offset overflows");
    }
    std::array<std::byte, packed_directory_record_size> bytes{};
    if (!read_exact_at(input, offset, bytes, error)) {
        return false;
    }
    return decode_directory_record(bytes, record, error);
}

[[nodiscard]] bool validate_chunk_identity(
    const PackedChartChunkInfo& info,
    const std::uint64_t chunk_index,
    const std::uint64_t explicit_note_count,
    const std::uint32_t max_notes_per_chunk,
    const std::uint64_t note_data_offset,
    const std::uint64_t note_data_size,
    const PackedChartLimits& limits,
    std::string* const error
) {
    std::uint64_t first_note_index{};
    if (!checked_multiply(
            chunk_index,
            max_notes_per_chunk,
            first_note_index
        )
        || first_note_index >= explicit_note_count) {
        return fail(error, "packed chart chunk index is inconsistent");
    }
    const auto remaining = explicit_note_count - first_note_index;
    const auto expected_count = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(remaining, max_notes_per_chunk)
    );
    std::uint64_t expected_byte_size{};
    std::uint64_t relative_offset{};
    std::uint64_t expected_file_offset{};
    std::uint64_t chunk_end{};
    std::uint64_t note_data_end{};
    if (!checked_multiply(expected_count, packed_note_record_size, expected_byte_size)
        || !checked_multiply(
            first_note_index,
            packed_note_record_size,
            relative_offset
        )
        || !checked_add(note_data_offset, relative_offset, expected_file_offset)
        || !checked_add(info.file_offset, info.byte_size, chunk_end)
        || !checked_add(note_data_offset, note_data_size, note_data_end)) {
        return fail(error, "packed chart chunk geometry overflows");
    }
    if (info.first_note_index != first_note_index
        || info.note_count != expected_count
        || info.byte_size != expected_byte_size
        || info.file_offset != expected_file_offset
        || info.byte_size > limits.max_chunk_bytes
        || chunk_end > note_data_end
        || info.first_time_us > info.last_time_us) {
        return fail(error, "packed chart chunk geometry is invalid");
    }
    return true;
}

[[nodiscard]] bool decode_chunk_payload(
    std::ifstream& input,
    const PackedChartChunkInfo& info,
    const std::uint16_t key_count,
    const std::size_t kind_count,
    const PackedChartLimits& limits,
    std::vector<PackedNote>& output,
    std::string* const error
) {
    if (info.byte_size > limits.max_chunk_bytes) {
        return fail(error, "packed chart chunk exceeds its byte limit");
    }
    std::size_t byte_size{};
    if (!to_size(info.byte_size, byte_size)) {
        return fail(error, "packed chart chunk cannot fit in memory");
    }
    std::vector<std::byte> bytes(byte_size);
    if (!read_exact_at(input, info.file_offset, bytes, error)) {
        return false;
    }
    if (crc32(bytes) != info.crc32) {
        return fail(error, "packed chart chunk CRC32 mismatch");
    }

    output.clear();
    output.reserve(info.note_count);
    ByteCursor cursor(bytes);
    for (std::uint32_t index = 0U; index < info.note_count; ++index) {
        PackedNote note;
        if (!decode_note(cursor, note)) {
            return fail(error, "packed chart note record is truncated");
        }
        if (!owner_is_valid(note.owner)) {
            return fail(error, "packed chart note owner is invalid");
        }
        if (note.lane >= key_count) {
            return fail(error, "packed chart note lane is invalid");
        }
        if (note.kind_id >= kind_count) {
            return fail(error, "packed chart note kind id is invalid");
        }
        if (!output.empty() && output.back().time_us > note.time_us) {
            return fail(error, "packed chart chunk notes are not time-sorted");
        }
        output.push_back(note);
    }
    if (!cursor.at_end()
        || output.empty()
        || output.front().time_us != info.first_time_us
        || output.back().time_us != info.last_time_us) {
        return fail(error, "packed chart chunk timestamps are inconsistent");
    }
    return true;
}

struct OwnedTemporaryPaths final {
    std::filesystem::path directory;
    std::filesystem::path file;
    bool owned{};

    ~OwnedTemporaryPaths() {
        if (!owned) {
            return;
        }
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        ignored.clear();
        std::filesystem::remove(directory, ignored);
    }
};

[[nodiscard]] bool destination_is_absent(
    const std::filesystem::path& destination,
    std::string* const error
) {
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(
        destination,
        status_error
    );
    if (status_error) {
        if (status_error == std::errc::no_such_file_or_directory) {
            return true;
        }
        return fail(error, "failed to inspect packed chart destination");
    }
    if (status.type() != std::filesystem::file_type::not_found) {
        return fail(error, "packed chart destination already exists");
    }
    return true;
}

}  // namespace

std::optional<PackedNote> PatternRun::note_at(
    const std::uint64_t index
) const noexcept {
    if (index >= count || lane_pattern.empty()) {
        return std::nullopt;
    }
    if (interval_denominator == 0U) {
        return std::nullopt;
    }
    // floor(index * interval_us / interval_denominator), computed without
    // compiler-specific 128-bit arithmetic. The residual product is bounded
    // because ar,br < denominator <= uint32_max.
    const auto divisor = static_cast<std::uint64_t>(interval_denominator);
    const auto aq = index / divisor;
    const auto ar = index % divisor;
    const auto bq = interval_us / divisor;
    const auto br = interval_us % divisor;
    if (interval_us != 0U
        && aq > std::numeric_limits<std::uint64_t>::max() / interval_us) {
        return std::nullopt;
    }
    std::uint64_t delta = aq * interval_us;
    if (bq != 0U
        && ar > (std::numeric_limits<std::uint64_t>::max() - delta) / bq) {
        return std::nullopt;
    }
    delta += ar * bq;
    const auto tail = (ar * br) / divisor;
    if (tail > std::numeric_limits<std::uint64_t>::max() - delta) {
        return std::nullopt;
    }
    delta += tail;

    std::int64_t time{};
    if (start_us >= 0) {
        const auto room = static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max() - start_us
        );
        if (delta > room) {
            return std::nullopt;
        }
        time = start_us + static_cast<std::int64_t>(delta);
    } else {
        const auto magnitude = static_cast<std::uint64_t>(-(start_us + 1)) + 1U;
        const auto room = static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()
        ) + magnitude;
        if (delta > room) {
            return std::nullopt;
        }
        if (delta < magnitude) {
            const auto remaining = magnitude - delta;
            const auto minimum_magnitude = std::uint64_t{1U} << 63U;
            time = remaining == minimum_magnitude
                ? std::numeric_limits<std::int64_t>::min()
                : -static_cast<std::int64_t>(remaining);
        } else {
            time = static_cast<std::int64_t>(delta - magnitude);
        }
    }

    const auto lane_count = static_cast<std::uint64_t>(lane_pattern.size());
    const auto lane_index = static_cast<std::size_t>(index % lane_count);
    return PackedNote{
        time,
        duration_us,
        lane_pattern[lane_index],
        owner,
        flags,
        kind_id,
    };
}

bool write_packed_chart(
    const std::filesystem::path& destination,
    const PackedChartData& chart,
    const PackedChartWriteOptions& options,
    std::string* const error
) {
    clear_error(error);
    try {
        if (destination.empty()) {
            return fail(error, "packed chart destination is empty");
        }
        if (options.max_notes_per_chunk == 0U) {
            return fail(error, "packed chart chunk note limit cannot be zero");
        }
        if (!validate_explicit_notes(chart, options.limits, error)) {
            return false;
        }

        std::vector<std::byte> dictionary;
        if (!build_dictionary(chart, options.limits, dictionary, error)) {
            return false;
        }

        const auto explicit_note_count = static_cast<std::uint64_t>(
            chart.notes.size()
        );
        std::uint64_t logical_note_count{};
        std::vector<std::byte> patterns;
        if (!build_patterns(
                chart,
                options.limits,
                explicit_note_count,
                logical_note_count,
                patterns,
                error
            )) {
            return false;
        }

        std::uint64_t maximum_chunk_bytes{};
        if (!checked_multiply(
                options.max_notes_per_chunk,
                packed_note_record_size,
                maximum_chunk_bytes
            )
            || maximum_chunk_bytes > options.limits.max_chunk_bytes) {
            return fail(error, "packed chart chunks exceed their byte limit");
        }

        const auto chunk_count = explicit_note_count == 0U
            ? 0U
            : ((explicit_note_count - 1U)
                / options.max_notes_per_chunk) + 1U;
        if (chunk_count > options.limits.max_chunks) {
            return fail(error, "packed chart has too many chunks");
        }

        std::uint64_t directory_size{};
        std::uint64_t note_data_size{};
        if (!checked_multiply(
                chunk_count,
                packed_directory_record_size,
                directory_size
            )
            || !checked_multiply(
                explicit_note_count,
                packed_note_record_size,
                note_data_size
            )) {
            return fail(error, "packed chart section size overflows");
        }

        HeaderFields header;
        header.key_count = chart.key_count;
        header.explicit_note_count = explicit_note_count;
        header.logical_note_count = logical_note_count;
        header.chunk_count = chunk_count;
        header.kind_count = static_cast<std::uint64_t>(chart.kinds.size());
        header.pattern_count = static_cast<std::uint64_t>(chart.patterns.size());
        header.dictionary_offset = packed_chart_header_size;
        header.dictionary_size = static_cast<std::uint64_t>(dictionary.size());
        if (!checked_add(
                header.dictionary_offset,
                header.dictionary_size,
                header.directory_offset
            )) {
            return fail(error, "packed chart dictionary offset overflows");
        }
        header.directory_size = directory_size;
        if (!checked_add(
                header.directory_offset,
                header.directory_size,
                header.pattern_offset
            )) {
            return fail(error, "packed chart directory offset overflows");
        }
        header.pattern_size = static_cast<std::uint64_t>(patterns.size());
        if (!checked_add(
                header.pattern_offset,
                header.pattern_size,
                header.note_data_offset
            )) {
            return fail(error, "packed chart pattern offset overflows");
        }
        header.note_data_size = note_data_size;
        if (!checked_add(
                header.note_data_offset,
                header.note_data_size,
                header.file_size
            )) {
            return fail(error, "packed chart file geometry overflows uint64");
        }
        if (header.file_size > options.limits.max_file_bytes) {
            return fail(
                error,
                "packed chart exceeds the configured file-size policy"
            );
        }
        if (header.file_size > static_cast<std::uint64_t>(
                std::numeric_limits<std::streamoff>::max()
            )) {
            return fail(
                error,
                "packed chart output is not seekable on this platform"
            );
        }
        header.max_notes_per_chunk = options.max_notes_per_chunk;
        header.dictionary_crc32 = crc32(dictionary);
        header.pattern_crc32 = crc32(patterns);

        std::error_code path_error;
        auto target = std::filesystem::absolute(destination, path_error);
        if (path_error) {
            return fail(error, "failed to resolve packed chart destination");
        }
        target = target.lexically_normal();
        if (target.filename().empty()) {
            return fail(error, "packed chart destination has no filename");
        }
        const auto parent = target.parent_path();
        const auto parent_status = std::filesystem::status(parent, path_error);
        if (path_error || !std::filesystem::is_directory(parent_status)) {
            return fail(error, "packed chart destination directory is unavailable");
        }
        if (!destination_is_absent(target, error)) {
            return false;
        }

        OwnedTemporaryPaths temporary;
        const auto nonce = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
        for (std::uint32_t attempt = 0U; attempt < 1'024U; ++attempt) {
            const auto name = std::string(".pulseforge-pfc1-")
                + std::to_string(nonce) + "-" + std::to_string(attempt);
            const auto candidate = parent / name;
            path_error.clear();
            if (std::filesystem::create_directory(candidate, path_error)) {
                temporary.directory = candidate;
                temporary.file = candidate / "payload.pfc1";
                temporary.owned = true;
                break;
            }
            if (path_error) {
                return fail(error, "failed to create packed chart temporary directory");
            }
        }
        if (!temporary.owned) {
            return fail(error, "could not reserve a packed chart temporary directory");
        }

        std::ofstream output(
            temporary.file,
            std::ios::binary | std::ios::trunc
        );
        if (!output) {
            return fail(error, "failed to create packed chart temporary file");
        }

        if (!write_zero_bytes(output, packed_chart_header_size, error)
            || !write_bytes(output, dictionary, error)
            || !write_zero_bytes(output, directory_size, error)
            || !write_bytes(output, patterns, error)) {
            return false;
        }

        Crc32 directory_checksum;
        std::size_t first_note = 0U;
        std::uint64_t chunk_index = 0U;
        const auto chunk_capacity = static_cast<std::size_t>(
            options.max_notes_per_chunk
        );
        while (first_note < chart.notes.size()) {
            const auto count_native = std::min(
                chunk_capacity,
                chart.notes.size() - first_note
            );
            const auto count = static_cast<std::uint32_t>(count_native);
            std::vector<std::byte> payload;
            payload.reserve(count_native * packed_note_record_size);
            for (std::size_t offset = 0U; offset < count_native; ++offset) {
                encode_note(payload, chart.notes[first_note + offset]);
            }

            std::uint64_t relative_payload_offset{};
            std::uint64_t payload_offset{};
            if (!checked_multiply(
                    static_cast<std::uint64_t>(first_note),
                    packed_note_record_size,
                    relative_payload_offset
                )
                || !checked_add(
                    header.note_data_offset,
                    relative_payload_offset,
                    payload_offset
                )
                || !seek_output(output, payload_offset, error)
                || !write_bytes(output, payload, error)) {
                return false;
            }

            const PackedChartChunkInfo info{
                chart.notes[first_note].time_us,
                chart.notes[first_note + count_native - 1U].time_us,
                static_cast<std::uint64_t>(first_note),
                count,
                payload_offset,
                static_cast<std::uint64_t>(payload.size()),
                crc32(payload),
            };
            const auto directory_record = encode_directory_record(info);
            directory_checksum.update(directory_record);

            std::uint64_t relative_directory_offset{};
            std::uint64_t directory_record_offset{};
            if (!checked_multiply(
                    chunk_index,
                    packed_directory_record_size,
                    relative_directory_offset
                )
                || !checked_add(
                    header.directory_offset,
                    relative_directory_offset,
                    directory_record_offset
                )
                || !seek_output(output, directory_record_offset, error)
                || !write_bytes(output, directory_record, error)) {
                return false;
            }

            first_note += count_native;
            ++chunk_index;
        }
        if (chunk_index != chunk_count) {
            return fail(error, "packed chart chunk count changed while writing");
        }

        header.directory_crc32 = directory_checksum.finish();
        const auto encoded_header = encode_header(header);
        if (!seek_output(output, 0U, error)
            || !write_bytes(output, encoded_header, error)) {
            return false;
        }
        output.flush();
        if (!output) {
            return fail(error, "failed to flush packed chart temporary file");
        }
        output.close();
        if (!output) {
            return fail(error, "failed to close packed chart temporary file");
        }

        path_error.clear();
        const auto written_size = std::filesystem::file_size(
            temporary.file,
            path_error
        );
        if (path_error || written_size != header.file_size) {
            return fail(error, "packed chart temporary file has the wrong size");
        }
        if (!destination_is_absent(target, error)) {
            return false;
        }
        path_error.clear();
        std::filesystem::rename(temporary.file, target, path_error);
        if (path_error) {
            return fail(error, "failed to commit packed chart with rename");
        }
        return true;
    } catch (const std::bad_alloc&) {
        if (error != nullptr) {
            *error = "packed chart write failed: insufficient memory for "
                "bounded dictionary/pattern/chunk state";
        }
        return false;
    } catch (const std::length_error&) {
        return fail(
            error,
            "packed chart write failed: metadata cannot fit in this process "
            "address space"
        );
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("packed chart write failed: ") + exception.what();
        }
        return false;
    } catch (...) {
        return fail(error, "packed chart write failed with an unknown error");
    }
}

std::optional<PackedChartReader> PackedChartReader::open(
    const std::filesystem::path& path,
    std::string* const error,
    const PackedChartLimits& limits
) {
    clear_error(error);
    try {
        std::error_code file_error;
        auto absolute_path = std::filesystem::absolute(path, file_error);
        if (file_error) {
            set_error(error, "failed to resolve packed chart path");
            return std::nullopt;
        }
        absolute_path = absolute_path.lexically_normal();
        const auto status = std::filesystem::status(absolute_path, file_error);
        if (file_error || !std::filesystem::is_regular_file(status)) {
            set_error(error, "packed chart path is not a regular file");
            return std::nullopt;
        }
        const auto actual_file_size = std::filesystem::file_size(
            absolute_path,
            file_error
        );
        if (file_error || actual_file_size < packed_chart_header_size
            || actual_file_size > static_cast<std::uintmax_t>(
                std::numeric_limits<std::uint64_t>::max()
            )) {
            set_error(error, "packed chart file size is invalid");
            return std::nullopt;
        }
        if (actual_file_size > limits.max_file_bytes) {
            set_error(
                error,
                "packed chart exceeds the configured file-size policy"
            );
            return std::nullopt;
        }
        const auto actual_file_size_u64 = static_cast<std::uint64_t>(
            actual_file_size
        );
        if (actual_file_size_u64 > static_cast<std::uint64_t>(
                std::numeric_limits<std::streamoff>::max()
            )) {
            set_error(error, "packed chart file is not seekable on this platform");
            return std::nullopt;
        }

        std::ifstream input(absolute_path, std::ios::binary);
        if (!input) {
            set_error(error, "failed to open packed chart");
            return std::nullopt;
        }
        std::array<std::byte, packed_chart_header_size> header_bytes{};
        if (!read_exact_at(input, 0U, header_bytes, error)) {
            return std::nullopt;
        }

        HeaderFields header;
        if (!decode_header(header_bytes, header, error)) {
            return std::nullopt;
        }
        if (header.version != packed_chart_version
            || header.header_size != packed_chart_header_size) {
            set_error(error, "packed chart version is unsupported");
            return std::nullopt;
        }
        if (header.endian_tag != packed_chart_endian_tag) {
            set_error(error, "packed chart endian tag is invalid");
            return std::nullopt;
        }
        if (header.flags != 0U || header.reserved != 0U) {
            set_error(error, "packed chart uses unsupported header flags");
            return std::nullopt;
        }
        if (header.key_count == 0U) {
            set_error(error, "packed chart key count is zero");
            return std::nullopt;
        }
        if (header.note_record_size != packed_note_record_size
            || header.directory_record_size != packed_directory_record_size
            || header.pattern_record_size != packed_pattern_record_size) {
            set_error(error, "packed chart record sizes are unsupported");
            return std::nullopt;
        }
        if (header.logical_note_count < header.explicit_note_count) {
            set_error(error, "packed chart logical count is smaller than its explicit count");
            return std::nullopt;
        }
        if (header.explicit_note_count > limits.max_explicit_notes) {
            set_error(error, "packed chart exceeds the configured explicit-note policy");
            return std::nullopt;
        }
        if (header.logical_note_count > limits.max_logical_notes) {
            set_error(error, "packed chart exceeds the configured logical-note policy");
            return std::nullopt;
        }
        if (header.chunk_count > limits.max_chunks) {
            set_error(error, "packed chart exceeds the configured chunk-count policy");
            return std::nullopt;
        }
        if (header.kind_count > limits.max_kinds) {
            set_error(error, "packed chart kind count is not representable by its policy/IDs");
            return std::nullopt;
        }
        if (header.pattern_count > limits.max_patterns) {
            set_error(error, "packed chart exceeds the configured PatternRun policy");
            return std::nullopt;
        }
        if (header.dictionary_size > limits.max_dictionary_bytes) {
            set_error(error, "packed chart dictionary exceeds the configured memory policy");
            return std::nullopt;
        }
        if (header.pattern_size > limits.max_pattern_bytes) {
            set_error(error, "packed chart patterns exceed the configured memory policy");
            return std::nullopt;
        }
        if (header.kind_count == 0U
            && (header.explicit_note_count != 0U
                || header.pattern_count != 0U)) {
            set_error(error, "packed chart notes require a kind dictionary");
            return std::nullopt;
        }
        if (header.max_notes_per_chunk == 0U) {
            set_error(error, "packed chart chunk note limit is zero");
            return std::nullopt;
        }

        std::uint64_t maximum_chunk_bytes{};
        std::uint64_t expected_directory_size{};
        std::uint64_t expected_pattern_records_size{};
        std::uint64_t expected_note_data_size{};
        if (!checked_multiply(
                header.max_notes_per_chunk,
                packed_note_record_size,
                maximum_chunk_bytes
            )
            || maximum_chunk_bytes > limits.max_chunk_bytes
            || !checked_multiply(
                header.chunk_count,
                packed_directory_record_size,
                expected_directory_size
            )
            || !checked_multiply(
                header.pattern_count,
                packed_pattern_record_size,
                expected_pattern_records_size
            )
            || !checked_multiply(
                header.explicit_note_count,
                packed_note_record_size,
                expected_note_data_size
            )) {
            set_error(error, "packed chart section geometry overflows");
            return std::nullopt;
        }
        const auto expected_chunk_count = header.explicit_note_count == 0U
            ? 0U
            : ((header.explicit_note_count - 1U)
                / header.max_notes_per_chunk) + 1U;
        if (header.chunk_count != expected_chunk_count
            || header.directory_size != expected_directory_size
            || header.pattern_size < expected_pattern_records_size
            || header.note_data_size != expected_note_data_size) {
            set_error(error, "packed chart section counts are inconsistent");
            return std::nullopt;
        }

        std::uint64_t expected_offset = packed_chart_header_size;
        if (header.dictionary_offset != expected_offset
            || !checked_add(
                expected_offset,
                header.dictionary_size,
                expected_offset
            )
            || header.directory_offset != expected_offset
            || !checked_add(
                expected_offset,
                header.directory_size,
                expected_offset
            )
            || header.pattern_offset != expected_offset
            || !checked_add(
                expected_offset,
                header.pattern_size,
                expected_offset
            )
            || header.note_data_offset != expected_offset
            || !checked_add(
                expected_offset,
                header.note_data_size,
                expected_offset
            )
            || header.file_size != expected_offset
            || header.file_size != actual_file_size_u64) {
            set_error(error, "packed chart sections overlap or are truncated");
            return std::nullopt;
        }

        std::vector<std::byte> dictionary_bytes;
        if (!read_section(
                input,
                header.dictionary_offset,
                header.dictionary_size,
                limits.max_dictionary_bytes,
                dictionary_bytes,
                error
            )
            || crc32(dictionary_bytes) != header.dictionary_crc32) {
            if (error != nullptr && error->empty()) {
                *error = "packed chart dictionary CRC32 mismatch";
            }
            return std::nullopt;
        }
        std::vector<std::string> kinds;
        if (!parse_dictionary(
                dictionary_bytes,
                header.kind_count,
                limits,
                kinds,
                error
            )) {
            return std::nullopt;
        }

        std::vector<std::byte> pattern_bytes;
        if (!read_section(
                input,
                header.pattern_offset,
                header.pattern_size,
                limits.max_pattern_bytes,
                pattern_bytes,
                error
            )
            || crc32(pattern_bytes) != header.pattern_crc32) {
            if (error != nullptr && error->empty()) {
                *error = "packed chart pattern CRC32 mismatch";
            }
            return std::nullopt;
        }
        std::vector<PatternRun> patterns;
        if (!parse_patterns(
                pattern_bytes,
                header.pattern_count,
                header.key_count,
                header.kind_count,
                header.explicit_note_count,
                header.logical_note_count,
                limits,
                patterns,
                error
            )) {
            return std::nullopt;
        }

        Crc32 directory_checksum;
        std::array<
            std::byte,
            packed_directory_record_size * directory_records_per_block
        > directory_block{};
        std::uint64_t processed = 0U;
        std::uint64_t expected_first_note = 0U;
        std::uint64_t expected_file_offset = header.note_data_offset;
        std::optional<std::int64_t> previous_last_time;
        while (processed < header.chunk_count) {
            const auto current_records = static_cast<std::size_t>(
                std::min<std::uint64_t>(
                    header.chunk_count - processed,
                    directory_records_per_block
                )
            );
            const auto current_bytes = current_records
                * packed_directory_record_size;
            std::uint64_t relative_offset{};
            std::uint64_t block_offset{};
            if (!checked_multiply(
                    processed,
                    packed_directory_record_size,
                    relative_offset
                )
                || !checked_add(
                    header.directory_offset,
                    relative_offset,
                    block_offset
                )
                || !read_exact_at(
                    input,
                    block_offset,
                    std::span<std::byte>(
                        directory_block.data(),
                        current_bytes
                    ),
                    error
                )) {
                return std::nullopt;
            }
            directory_checksum.update(std::span<const std::byte>(
                directory_block.data(),
                current_bytes
            ));

            for (std::size_t local = 0U; local < current_records; ++local) {
                DirectoryRecord record;
                const auto record_offset = local
                    * packed_directory_record_size;
                if (!decode_directory_record(
                        std::span<const std::byte>(
                            directory_block.data() + record_offset,
                            packed_directory_record_size
                        ),
                        record,
                        error
                    )) {
                    return std::nullopt;
                }
                const auto remaining = header.explicit_note_count
                    - expected_first_note;
                const auto expected_count = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(
                        remaining,
                        header.max_notes_per_chunk
                    )
                );
                std::uint64_t expected_byte_size{};
                if (!checked_multiply(
                        expected_count,
                        packed_note_record_size,
                        expected_byte_size
                    )
                    || record.info.first_note_index != expected_first_note
                    || record.info.note_count != expected_count
                    || record.info.file_offset != expected_file_offset
                    || record.info.byte_size != expected_byte_size
                    || record.info.byte_size > limits.max_chunk_bytes
                    || record.info.first_time_us > record.info.last_time_us
                    || (previous_last_time.has_value()
                        && *previous_last_time > record.info.first_time_us)) {
                    set_error(error, "packed chart chunk directory is inconsistent");
                    return std::nullopt;
                }
                if (!checked_add(
                        expected_first_note,
                        expected_count,
                        expected_first_note
                    )
                    || !checked_add(
                        expected_file_offset,
                        expected_byte_size,
                        expected_file_offset
                    )) {
                    set_error(error, "packed chart chunk directory overflows");
                    return std::nullopt;
                }
                previous_last_time = record.info.last_time_us;
            }
            processed += static_cast<std::uint64_t>(current_records);
        }
        std::uint64_t expected_note_data_end{};
        if (!checked_add(
                header.note_data_offset,
                header.note_data_size,
                expected_note_data_end
            )
            || expected_first_note != header.explicit_note_count
            || expected_file_offset != expected_note_data_end
            || directory_checksum.finish() != header.directory_crc32) {
            set_error(error, "packed chart chunk directory CRC32 or extent is invalid");
            return std::nullopt;
        }

        PackedChartReader reader;
        reader.path_ = std::move(absolute_path);
        reader.limits_ = limits;
        reader.key_count_ = header.key_count;
        reader.explicit_note_count_ = header.explicit_note_count;
        reader.logical_note_count_ = header.logical_note_count;
        reader.chunk_count_ = header.chunk_count;
        reader.directory_offset_ = header.directory_offset;
        reader.directory_size_ = header.directory_size;
        reader.note_data_offset_ = header.note_data_offset;
        reader.note_data_size_ = header.note_data_size;
        reader.file_size_ = header.file_size;
        reader.max_notes_per_chunk_ = header.max_notes_per_chunk;
        reader.kinds_ = std::move(kinds);
        reader.patterns_ = std::move(patterns);
        return reader;
    } catch (const std::bad_alloc&) {
        set_error(
            error,
            "packed chart open failed: insufficient memory for the eager "
            "dictionary/pattern metadata"
        );
        return std::nullopt;
    } catch (const std::length_error&) {
        set_error(
            error,
            "packed chart open failed: metadata cannot fit in this process "
            "address space"
        );
        return std::nullopt;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("packed chart open failed: ") + exception.what();
        }
        return std::nullopt;
    } catch (...) {
        set_error(error, "packed chart open failed with an unknown error");
        return std::nullopt;
    }
}

const std::filesystem::path& PackedChartReader::path() const noexcept {
    return path_;
}

std::uint16_t PackedChartReader::key_count() const noexcept {
    return key_count_;
}

std::uint64_t PackedChartReader::explicit_note_count() const noexcept {
    return explicit_note_count_;
}

std::uint64_t PackedChartReader::logical_note_count() const noexcept {
    return logical_note_count_;
}

std::uint64_t PackedChartReader::chunk_count() const noexcept {
    return chunk_count_;
}

std::span<const std::string> PackedChartReader::kinds() const noexcept {
    return kinds_;
}

std::span<const PatternRun> PackedChartReader::patterns() const noexcept {
    return patterns_;
}

std::optional<PackedChartChunkInfo> PackedChartReader::chunk_info(
    const std::uint64_t chunk_index,
    std::string* const error
) const {
    clear_error(error);
    if (chunk_index >= chunk_count_) {
        set_error(error, "packed chart chunk index is out of range");
        return std::nullopt;
    }
    try {
        std::ifstream input(path_, std::ios::binary);
        if (!input) {
            set_error(error, "failed to reopen packed chart");
            return std::nullopt;
        }
        DirectoryRecord record;
        if (!read_directory_record_at(
                input,
                directory_offset_,
                chunk_index,
                record,
                error
            )
            || !validate_chunk_identity(
                record.info,
                chunk_index,
                explicit_note_count_,
                max_notes_per_chunk_,
                note_data_offset_,
                note_data_size_,
                limits_,
                error
            )) {
            return std::nullopt;
        }
        return record.info;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("packed chart chunk lookup failed: ")
                + exception.what();
        }
        return std::nullopt;
    } catch (...) {
        set_error(error, "packed chart chunk lookup failed");
        return std::nullopt;
    }
}

PackedNoteReadResult PackedChartReader::read_chunk(
    const std::uint64_t chunk_index
) const {
    PackedNoteReadResult result;
    if (chunk_index >= chunk_count_) {
        result.error = "packed chart chunk index is out of range";
        return result;
    }
    try {
        std::ifstream input(path_, std::ios::binary);
        if (!input) {
            result.error = "failed to reopen packed chart";
            return result;
        }
        DirectoryRecord record;
        if (!read_directory_record_at(
                input,
                directory_offset_,
                chunk_index,
                record,
                &result.error
            )
            || !validate_chunk_identity(
                record.info,
                chunk_index,
                explicit_note_count_,
                max_notes_per_chunk_,
                note_data_offset_,
                note_data_size_,
                limits_,
                &result.error
            )
            || !decode_chunk_payload(
                input,
                record.info,
                key_count_,
                kinds_.size(),
                limits_,
                result.notes,
                &result.error
            )) {
            result.notes.clear();
        }
    } catch (const std::exception& exception) {
        result.notes.clear();
        result.error = std::string("packed chart chunk read failed: ")
            + exception.what();
    } catch (...) {
        result.notes.clear();
        result.error = "packed chart chunk read failed";
    }
    return result;
}

PackedNoteReadResult PackedChartReader::read_explicit_notes_in_range(
    const std::int64_t first_time_us,
    const std::int64_t last_time_us,
    const std::size_t max_results
) const {
    PackedNoteReadResult result;
    if (first_time_us > last_time_us || chunk_count_ == 0U) {
        return result;
    }
    const auto result_limit = max_results == 0U
        ? limits_.max_query_notes
        : max_results;
    if (result_limit > limits_.max_query_notes) {
        result.error = "packed chart query result limit exceeds reader policy";
        return result;
    }

    try {
        std::ifstream input(path_, std::ios::binary);
        if (!input) {
            result.error = "failed to reopen packed chart";
            return result;
        }

        std::uint64_t lower = 0U;
        std::uint64_t upper = chunk_count_;
        while (lower < upper) {
            const auto middle = lower + ((upper - lower) / 2U);
            DirectoryRecord record;
            if (!read_directory_record_at(
                    input,
                    directory_offset_,
                    middle,
                    record,
                    &result.error
                )
                || !validate_chunk_identity(
                    record.info,
                    middle,
                    explicit_note_count_,
                    max_notes_per_chunk_,
                    note_data_offset_,
                    note_data_size_,
                    limits_,
                    &result.error
                )) {
                return result;
            }
            if (record.info.last_time_us < first_time_us) {
                lower = middle + 1U;
            } else {
                upper = middle;
            }
        }

        for (std::uint64_t index = lower; index < chunk_count_; ++index) {
            DirectoryRecord record;
            if (!read_directory_record_at(
                    input,
                    directory_offset_,
                    index,
                    record,
                    &result.error
                )
                || !validate_chunk_identity(
                    record.info,
                    index,
                    explicit_note_count_,
                    max_notes_per_chunk_,
                    note_data_offset_,
                    note_data_size_,
                    limits_,
                    &result.error
                )) {
                result.notes.clear();
                return result;
            }
            if (record.info.first_time_us > last_time_us) {
                break;
            }

            std::vector<PackedNote> chunk_notes;
            if (!decode_chunk_payload(
                    input,
                    record.info,
                    key_count_,
                    kinds_.size(),
                    limits_,
                    chunk_notes,
                    &result.error
                )) {
                result.notes.clear();
                return result;
            }
            for (const auto& note : chunk_notes) {
                if (note.time_us < first_time_us) {
                    continue;
                }
                if (note.time_us > last_time_us) {
                    break;
                }
                if (result.notes.size() == result_limit) {
                    result.notes.clear();
                    result.error = "packed chart range query exceeds its result limit";
                    return result;
                }
                result.notes.push_back(note);
            }
        }
    } catch (const std::exception& exception) {
        result.notes.clear();
        result.error = std::string("packed chart range read failed: ")
            + exception.what();
    } catch (...) {
        result.notes.clear();
        result.error = "packed chart range read failed";
    }
    return result;
}

IndexedPackedNoteReadResult
PackedChartReader::read_indexed_explicit_notes_in_range(
    const std::int64_t first_time_us,
    const std::int64_t last_time_us,
    const std::size_t max_results
) const {
    IndexedPackedNoteReadResult result;
    if (first_time_us > last_time_us || chunk_count_ == 0U) {
        return result;
    }
    const auto result_limit = max_results == 0U
        ? limits_.max_query_notes
        : max_results;
    if (result_limit == 0U || result_limit > limits_.max_query_notes) {
        result.error = "packed chart query result limit exceeds reader policy";
        return result;
    }

    try {
        std::ifstream input(path_, std::ios::binary);
        if (!input) {
            result.error = "failed to reopen packed chart";
            return result;
        }

        std::uint64_t lower = 0U;
        std::uint64_t upper = chunk_count_;
        while (lower < upper) {
            const auto middle = lower + ((upper - lower) / 2U);
            DirectoryRecord record;
            if (!read_directory_record_at(
                    input,
                    directory_offset_,
                    middle,
                    record,
                    &result.error
                )
                || !validate_chunk_identity(
                    record.info,
                    middle,
                    explicit_note_count_,
                    max_notes_per_chunk_,
                    note_data_offset_,
                    note_data_size_,
                    limits_,
                    &result.error
                )) {
                return result;
            }
            if (record.info.last_time_us < first_time_us) {
                lower = middle + 1U;
            } else {
                upper = middle;
            }
        }

        for (std::uint64_t chunk_index = lower;
             chunk_index < chunk_count_;
             ++chunk_index) {
            DirectoryRecord record;
            if (!read_directory_record_at(
                    input,
                    directory_offset_,
                    chunk_index,
                    record,
                    &result.error
                )
                || !validate_chunk_identity(
                    record.info,
                    chunk_index,
                    explicit_note_count_,
                    max_notes_per_chunk_,
                    note_data_offset_,
                    note_data_size_,
                    limits_,
                    &result.error
                )) {
                result.notes.clear();
                return result;
            }
            if (record.info.first_time_us > last_time_us) {
                break;
            }

            std::vector<PackedNote> chunk_notes;
            if (!decode_chunk_payload(
                    input,
                    record.info,
                    key_count_,
                    kinds_.size(),
                    limits_,
                    chunk_notes,
                    &result.error
                )) {
                result.notes.clear();
                return result;
            }
            for (std::size_t offset = 0U;
                 offset < chunk_notes.size();
                 ++offset) {
                const auto& note = chunk_notes[offset];
                if (note.time_us < first_time_us) {
                    continue;
                }
                if (note.time_us > last_time_us) {
                    break;
                }
                if (result.notes.size() == result_limit) {
                    result.truncated = true;
                    return result;
                }
                result.notes.push_back(IndexedPackedNote{
                    record.info.first_note_index
                        + static_cast<std::uint64_t>(offset),
                    note,
                });
            }
        }
    } catch (const std::exception& exception) {
        result.notes.clear();
        result.error = std::string("packed chart indexed range read failed: ")
            + exception.what();
    } catch (...) {
        result.notes.clear();
        result.error = "packed chart indexed range read failed";
    }
    return result;
}

PackedNoteVisitResult PackedChartReader::visit_explicit_notes_in_range(
    const std::int64_t first_time_us,
    const std::int64_t last_time_us,
    void* const context,
    const PackedNoteVisitor visitor
) const {
    PackedNoteVisitResult result;
    if (visitor == nullptr) {
        result.error = "packed chart range visitor is null";
        return result;
    }
    if (first_time_us > last_time_us || chunk_count_ == 0U) {
        return result;
    }

    try {
        std::ifstream input(path_, std::ios::binary);
        if (!input) {
            result.error = "failed to reopen packed chart";
            return result;
        }

        std::uint64_t lower = 0U;
        std::uint64_t upper = chunk_count_;
        while (lower < upper) {
            const auto middle = lower + ((upper - lower) / 2U);
            DirectoryRecord record;
            if (!read_directory_record_at(
                    input,
                    directory_offset_,
                    middle,
                    record,
                    &result.error
                )
                || !validate_chunk_identity(
                    record.info,
                    middle,
                    explicit_note_count_,
                    max_notes_per_chunk_,
                    note_data_offset_,
                    note_data_size_,
                    limits_,
                    &result.error
                )) {
                return result;
            }
            if (record.info.last_time_us < first_time_us) {
                lower = middle + 1U;
            } else {
                upper = middle;
            }
        }

        for (std::uint64_t chunk = lower; chunk < chunk_count_; ++chunk) {
            DirectoryRecord record;
            if (!read_directory_record_at(
                    input,
                    directory_offset_,
                    chunk,
                    record,
                    &result.error
                )
                || !validate_chunk_identity(
                    record.info,
                    chunk,
                    explicit_note_count_,
                    max_notes_per_chunk_,
                    note_data_offset_,
                    note_data_size_,
                    limits_,
                    &result.error
                )) {
                return result;
            }
            if (record.info.first_time_us > last_time_us) {
                break;
            }
            std::vector<PackedNote> notes;
            if (!decode_chunk_payload(
                    input,
                    record.info,
                    key_count_,
                    kinds_.size(),
                    limits_,
                    notes,
                    &result.error
                )) {
                return result;
            }
            for (const auto& note : notes) {
                if (note.time_us < first_time_us) {
                    continue;
                }
                if (note.time_us > last_time_us) {
                    break;
                }
                visitor(context, note);
                if (result.notes_visited
                    != std::numeric_limits<std::uint64_t>::max()) {
                    ++result.notes_visited;
                }
            }
        }
    } catch (const std::exception& exception) {
        result.error = std::string("packed chart range visit failed: ")
            + exception.what();
    } catch (...) {
        result.error = "packed chart range visit failed";
    }
    return result;
}

}  // namespace pulseforge
