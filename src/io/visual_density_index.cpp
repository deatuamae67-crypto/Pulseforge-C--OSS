#include "pulseforge/visual_density_index.hpp"
#include "pulseforge/note_types.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace pulseforge {
namespace {

constexpr std::array<char, 4> pvd_magic{'P', 'V', 'D', '1'};
constexpr std::uint16_t pvd_version = 2U;
constexpr std::uint16_t pvd_header_size = 512U;
constexpr std::uint32_t pvd_record_size = 56U;
constexpr std::int64_t pvd_sustain_checkpoint_buckets = 256;
// PVD is an optional derivative. Refuse hostile duration gaps long before a
// sparse sustain checkpoint loop could approach the 256 GiB container cap;
// callers retain the authoritative PFC and fall back to exact visits.
constexpr std::uint64_t pvd_max_generated_checkpoint_records = 10'000'000U;
constexpr std::uint64_t pvd_max_file_bytes = 256ULL * 1024ULL * 1024ULL * 1024ULL;

struct Accum final {
    std::uint64_t normal_heads{};
    std::uint64_t hurt_heads{};
    std::uint64_t sustain_starts{};
    std::uint64_t sustain_ends{};

    [[nodiscard]] bool empty() const noexcept {
        return normal_heads == 0U && hurt_heads == 0U
            && sustain_starts == 0U && sustain_ends == 0U;
    }
};

struct DiskRecord final {
    std::int64_t bucket_index{};
    std::uint16_t lane{};
    PackedNoteOwner owner{PackedNoteOwner::player};
    Accum counts;
    std::uint64_t active_sustains_at_bucket_start{};
};

struct DiskLevel final {
    std::uint64_t bucket_width_us{};
    std::uint64_t file_offset{};
    std::uint64_t record_count{};
    std::uint64_t byte_size{};
};

[[nodiscard]] constexpr std::uint64_t saturated_add(
    const std::uint64_t left,
    const std::uint64_t right
) noexcept {
    return right > std::numeric_limits<std::uint64_t>::max() - left
        ? std::numeric_limits<std::uint64_t>::max()
        : left + right;
}

void merge_accum(Accum& destination, const Accum& source) noexcept {
    destination.normal_heads = saturated_add(
        destination.normal_heads,
        source.normal_heads
    );
    destination.hurt_heads = saturated_add(
        destination.hurt_heads,
        source.hurt_heads
    );
    destination.sustain_starts = saturated_add(
        destination.sustain_starts,
        source.sustain_starts
    );
    destination.sustain_ends = saturated_add(
        destination.sustain_ends,
        source.sustain_ends
    );
}

[[nodiscard]] constexpr std::int64_t floor_div(
    const std::int64_t value,
    const std::uint64_t divisor
) noexcept {
    if (divisor == 0U || divisor > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return 0;
    }
    const auto d = static_cast<std::int64_t>(divisor);
    auto quotient = value / d;
    const auto remainder = value % d;
    if (remainder < 0) {
        --quotient;
    }
    return quotient;
}

[[nodiscard]] constexpr std::int64_t floor_div_two(
    const std::int64_t value
) noexcept {
    auto quotient = value / 2;
    if (value < 0 && (value % 2) != 0) {
        --quotient;
    }
    return quotient;
}

template <typename Unsigned>
void write_le(char*& output, Unsigned value) noexcept {
    static_assert(std::is_unsigned_v<Unsigned>);
    for (std::size_t index = 0U; index < sizeof(Unsigned); ++index) {
        *output++ = static_cast<char>(value & 0xFFU);
        value >>= 8U;
    }
}

template <typename Unsigned>
[[nodiscard]] Unsigned read_le(const char*& input) noexcept {
    static_assert(std::is_unsigned_v<Unsigned>);
    Unsigned value{};
    for (std::size_t index = 0U; index < sizeof(Unsigned); ++index) {
        value |= static_cast<Unsigned>(
            static_cast<std::uint8_t>(*input++)
        ) << static_cast<unsigned int>(index * 8U);
    }
    return value;
}

[[nodiscard]] std::array<char, pvd_record_size> encode_record(
    const DiskRecord& record
) noexcept {
    std::array<char, pvd_record_size> bytes{};
    auto* output = bytes.data();
    write_le(output, std::bit_cast<std::uint64_t>(record.bucket_index));
    write_le(output, record.lane);
    write_le(output, static_cast<std::uint8_t>(record.owner));
    write_le(output, std::uint8_t{0U});
    write_le(output, std::uint32_t{0U});
    write_le(output, record.counts.normal_heads);
    write_le(output, record.counts.hurt_heads);
    write_le(output, record.counts.sustain_starts);
    write_le(output, record.counts.sustain_ends);
    write_le(output, record.active_sustains_at_bucket_start);
    return bytes;
}

[[nodiscard]] DiskRecord decode_record(
    const std::array<char, pvd_record_size>& bytes
) noexcept {
    const auto* input = bytes.data();
    DiskRecord record;
    record.bucket_index = std::bit_cast<std::int64_t>(
        read_le<std::uint64_t>(input)
    );
    record.lane = read_le<std::uint16_t>(input);
    record.owner = static_cast<PackedNoteOwner>(
        read_le<std::uint8_t>(input)
    );
    (void)read_le<std::uint8_t>(input);
    (void)read_le<std::uint32_t>(input);
    record.counts.normal_heads = read_le<std::uint64_t>(input);
    record.counts.hurt_heads = read_le<std::uint64_t>(input);
    record.counts.sustain_starts = read_le<std::uint64_t>(input);
    record.counts.sustain_ends = read_le<std::uint64_t>(input);
    record.active_sustains_at_bucket_start = read_le<std::uint64_t>(input);
    return record;
}

[[nodiscard]] bool write_record(
    std::ostream& output,
    const DiskRecord& record
) {
    const auto bytes = encode_record(record);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

[[nodiscard]] bool read_record(std::istream& input, DiskRecord& record) {
    std::array<char, pvd_record_size> bytes{};
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() == 0 && input.eof()) {
        return false;
    }
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())
        || input.bad()) {
        throw std::runtime_error("visual density index record is truncated");
    }
    record = decode_record(bytes);
    return true;
}

[[nodiscard]] std::uint64_t safe_end_time(
    const PackedNote& note
) noexcept {
    if (note.time_us < 0) {
        const auto magnitude = static_cast<std::uint64_t>(-(note.time_us + 1)) + 1U;
        if (note.duration_us < magnitude) {
            return 0U;
        }
        return note.duration_us - magnitude;
    }
    const auto start = static_cast<std::uint64_t>(note.time_us);
    return note.duration_us > std::numeric_limits<std::uint64_t>::max() - start
        ? std::numeric_limits<std::uint64_t>::max()
        : start + note.duration_us;
}

[[nodiscard]] bool hurt_kind(const std::string_view kind) noexcept {
    return builtin_note_type_causes_miss(kind);
}

[[nodiscard]] std::filesystem::path unique_path(
    const std::filesystem::path& directory,
    const std::string_view stem,
    const std::uint64_t nonce,
    const std::uint32_t sequence
) {
    return directory / (
        std::string(stem) + "-" + std::to_string(nonce)
        + "-" + std::to_string(sequence) + ".bin"
    );
}

[[nodiscard]] std::vector<char> encode_header(
    const std::uint16_t key_count,
    const std::uint64_t file_size,
    const std::vector<DiskLevel>& levels
) {
    std::vector<char> header(pvd_header_size, '\0');
    std::copy(pvd_magic.begin(), pvd_magic.end(), header.begin());
    auto* output = header.data() + 4;
    write_le(output, pvd_version);
    write_le(output, pvd_header_size);
    write_le(output, key_count);
    write_le(output, static_cast<std::uint16_t>(levels.size()));
    write_le(output, visual_density_base_bucket_us);
    write_le(output, pvd_record_size);
    write_le(output, file_size);
    write_le(output, std::uint64_t{0U});
    for (const auto& level : levels) {
        write_le(output, level.bucket_width_us);
        write_le(output, level.file_offset);
        write_le(output, level.record_count);
        write_le(output, level.byte_size);
    }
    return header;
}

}  // namespace

class VisualDensityIndexBuilder::Impl final {
public:
    Impl(
        const std::uint16_t key_count,
        const std::span<const std::string> kinds,
        std::filesystem::path working_directory
    ) : key_count_(key_count) {
        if (key_count_ == 0U) {
            throw std::invalid_argument("visual density key count cannot be zero");
        }
        if (working_directory.empty()) {
            std::error_code error;
            working_directory = std::filesystem::temp_directory_path(error);
            if (error) {
                throw std::runtime_error("cannot resolve visual density temp directory");
            }
        }
        std::error_code error;
        working_directory_ = std::filesystem::absolute(
            working_directory,
            error
        ).lexically_normal();
        if (error || !std::filesystem::is_directory(working_directory_)) {
            throw std::runtime_error("visual density working directory is invalid");
        }
        hurt_kinds_.resize(kinds.size(), false);
        for (std::size_t index = 0U; index < kinds.size(); ++index) {
            hurt_kinds_[index] = hurt_kind(kinds[index]);
        }
        nonce_ = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
        level_paths_.push_back(unique_path(
            working_directory_, ".pulseforge-pvd-level", nonce_, 0U
        ));
        level_zero_.open(level_paths_.front(), std::ios::binary | std::ios::trunc);
        if (!level_zero_) {
            throw std::runtime_error("cannot create visual density level 0");
        }
    }

    ~Impl() {
        cleanup();
    }

    void add(const PackedNote& note) {
        if (finished_) {
            throw std::logic_error("visual density builder is already finished");
        }
        if (note.lane >= key_count_) {
            throw std::runtime_error("visual density note lane is invalid");
        }
        if (last_time_.has_value() && note.time_us < *last_time_) {
            throw std::runtime_error("visual density notes are not time ordered");
        }
        last_time_ = note.time_us;
        const auto bucket = floor_div(note.time_us, visual_density_base_bucket_us);
        flush_before(bucket);
        auto& current = pending_[bucket];
        ensure_cells(current);
        const auto cell = cell_index(note.lane, note.owner);
        const bool hurt = note.kind_id < hurt_kinds_.size()
            && hurt_kinds_[note.kind_id];
        if (hurt) {
            current[cell].hurt_heads = saturated_add(current[cell].hurt_heads, 1U);
        } else {
            current[cell].normal_heads = saturated_add(current[cell].normal_heads, 1U);
        }
        if (note.duration_us != 0U) {
            current[cell].sustain_starts = saturated_add(
                current[cell].sustain_starts,
                1U
            );
            std::int64_t end_time{};
            if (note.time_us >= 0) {
                const auto unsigned_end = safe_end_time(note);
                end_time = unsigned_end > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())
                    ? std::numeric_limits<std::int64_t>::max()
                    : static_cast<std::int64_t>(unsigned_end);
            } else if (note.duration_us > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
                end_time = std::numeric_limits<std::int64_t>::max();
            } else {
                const auto duration = static_cast<std::int64_t>(note.duration_us);
                end_time = duration > std::numeric_limits<std::int64_t>::max() - note.time_us
                    ? std::numeric_limits<std::int64_t>::max()
                    : note.time_us + duration;
            }
            const auto end_bucket = floor_div(end_time, visual_density_base_bucket_us);
            if (!pending_.contains(end_bucket)
                && pending_.size()
                    >= visual_density_max_pending_end_buckets) {
                throw std::runtime_error(
                    "visual density pending sustain ends exceed optional sidecar budget"
                );
            }
            auto& ending = pending_[end_bucket];
            ensure_cells(ending);
            ending[cell].sustain_ends = saturated_add(
                ending[cell].sustain_ends,
                1U
            );
        }
    }

    [[nodiscard]] bool finish(
        const std::filesystem::path& destination,
        std::string* const error
    ) noexcept {
        try {
            if (finished_) {
                throw std::logic_error("visual density builder is already finished");
            }
            flush_all();
            level_zero_.flush();
            if (!level_zero_) {
                throw std::runtime_error("cannot flush visual density level 0");
            }
            level_zero_.close();

            level_counts_.push_back(level_zero_count_);
            for (std::uint16_t level = 1U;
                 level < visual_density_level_count;
                 ++level) {
                build_next_level(level);
            }

            std::error_code filesystem_error;
            const auto absolute = std::filesystem::absolute(
                destination,
                filesystem_error
            ).lexically_normal();
            if (filesystem_error || absolute.empty()) {
                throw std::runtime_error("cannot resolve visual density destination");
            }
            if (!std::filesystem::is_directory(absolute.parent_path())) {
                throw std::runtime_error("visual density destination directory is invalid");
            }
            if (std::filesystem::exists(absolute, filesystem_error)) {
                throw std::runtime_error("visual density destination already exists");
            }

            auto temporary = absolute;
            temporary += ".tmp-" + std::to_string(nonce_);
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error("cannot create visual density destination");
            }
            std::vector<char> zero_header(pvd_header_size, '\0');
            output.write(zero_header.data(), static_cast<std::streamsize>(zero_header.size()));
            if (!output) {
                throw std::runtime_error("cannot reserve visual density header");
            }

            std::vector<DiskLevel> levels;
            levels.reserve(level_paths_.size());
            std::uint64_t offset = pvd_header_size;
            for (std::size_t index = 0U; index < level_paths_.size(); ++index) {
                const auto count = level_counts_.at(index);
                const auto bytes = count > std::numeric_limits<std::uint64_t>::max()
                        / pvd_record_size
                    ? throw std::runtime_error("visual density level size overflows")
                    : count * pvd_record_size;
                levels.push_back({
                    static_cast<std::uint64_t>(visual_density_base_bucket_us) << index,
                    offset,
                    count,
                    bytes,
                });
                std::ifstream input(level_paths_[index], std::ios::binary);
                if (!input) {
                    throw std::runtime_error("cannot reopen visual density level");
                }
                // A one-megabyte automatic array exhausts the default Windows
                // thread stack before the first huge-chart cache can finish.
                // Keep the transfer buffer on the heap and reuse it per level.
                std::vector<char> buffer(1U << 20U);
                while (input) {
                    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                    const auto amount = input.gcount();
                    if (amount < 0 || input.bad()) {
                        throw std::runtime_error("cannot read visual density level");
                    }
                    if (amount != 0) {
                        output.write(buffer.data(), amount);
                        if (!output) {
                            throw std::runtime_error("cannot write visual density level");
                        }
                    }
                }
                if (bytes > pvd_max_file_bytes - std::min(pvd_max_file_bytes, offset)) {
                    throw std::runtime_error("visual density file exceeds its limit");
                }
                offset += bytes;
            }
            const auto header = encode_header(key_count_, offset, levels);
            output.seekp(0, std::ios::beg);
            output.write(header.data(), static_cast<std::streamsize>(header.size()));
            output.flush();
            if (!output) {
                throw std::runtime_error("cannot finalize visual density file");
            }
            output.close();
            std::filesystem::rename(temporary, absolute, filesystem_error);
            if (filesystem_error) {
                std::filesystem::remove(temporary, filesystem_error);
                throw std::runtime_error("cannot atomically publish visual density file");
            }
            finished_ = true;
            cleanup_levels();
            return true;
        } catch (const std::exception& exception) {
            if (error != nullptr) {
                try {
                    *error = exception.what();
                } catch (...) {
                    // PVD is derivative data. Even error reporting must not
                    // turn memory pressure into failure of the PFC import.
                }
            }
            return false;
        } catch (...) {
            if (error != nullptr) {
                try {
                    *error = "visual density finalization failed";
                } catch (...) {
                }
            }
            return false;
        }
    }

private:
    [[nodiscard]] std::size_t cell_index(
        const std::uint16_t lane,
        const PackedNoteOwner owner
    ) const noexcept {
        const auto owner_index = owner == PackedNoteOwner::player ? 1U : 0U;
        return owner_index * static_cast<std::size_t>(key_count_) + lane;
    }

    void ensure_cells(std::vector<Accum>& cells) const {
        if (cells.empty()) {
            cells.resize(static_cast<std::size_t>(key_count_) * 2U);
        }
    }

    void flush_before(const std::int64_t bucket) {
        while (!pending_.empty() && pending_.begin()->first < bucket) {
            flush_one(pending_.begin());
        }
    }

    void flush_all() {
        while (!pending_.empty()) {
            flush_one(pending_.begin());
        }
    }

    [[nodiscard]] bool any_active_sustains() const noexcept {
        return std::any_of(
            active_sustains_.begin(),
            active_sustains_.end(),
            [](const std::uint64_t count) { return count != 0U; }
        );
    }

    void schedule_checkpoint_after(const std::int64_t bucket) noexcept {
        if (bucket > std::numeric_limits<std::int64_t>::max()
                - pvd_sustain_checkpoint_buckets) {
            next_sustain_checkpoint_.reset();
            return;
        }
        next_sustain_checkpoint_ = bucket + pvd_sustain_checkpoint_buckets;
    }

    void advance_checkpoint() noexcept {
        if (!next_sustain_checkpoint_.has_value()) {
            return;
        }
        schedule_checkpoint_after(*next_sustain_checkpoint_);
    }

    void write_active_checkpoint(const std::int64_t bucket) {
        for (std::uint16_t lane = 0U; lane < key_count_; ++lane) {
            for (const auto owner : {
                    PackedNoteOwner::opponent,
                    PackedNoteOwner::player,
                }) {
                const auto cell = cell_index(lane, owner);
                if (active_sustains_[cell] == 0U) {
                    continue;
                }
                if (!write_record(level_zero_, {
                        bucket,
                        lane,
                        owner,
                        {},
                        active_sustains_[cell],
                    })) {
                    throw std::runtime_error(
                        "cannot write visual density sustain checkpoint"
                    );
                }
                ++level_zero_count_;
            }
        }
    }

    void flush_one(std::map<std::int64_t, std::vector<Accum>>::iterator iterator) {
        const auto bucket = iterator->first;
        const auto cells = std::move(iterator->second);
        pending_.erase(iterator);
        if (active_sustains_.empty()) {
            active_sustains_.resize(static_cast<std::size_t>(key_count_) * 2U);
        }
        if (!any_active_sustains()) {
            next_sustain_checkpoint_.reset();
        }
        while (next_sustain_checkpoint_.has_value()
            && *next_sustain_checkpoint_ < bucket) {
            const auto active_cells = static_cast<std::uint64_t>(
                std::count_if(
                    active_sustains_.begin(),
                    active_sustains_.end(),
                    [](const std::uint64_t count) { return count != 0U; }
                )
            );
            const auto distance = static_cast<std::uint64_t>(bucket)
                - static_cast<std::uint64_t>(*next_sustain_checkpoint_);
            const auto checkpoints = distance
                    / static_cast<std::uint64_t>(
                        pvd_sustain_checkpoint_buckets
                    )
                + 1U;
            if (active_cells != 0U
                && checkpoints > (
                    pvd_max_generated_checkpoint_records
                        - std::min(
                            pvd_max_generated_checkpoint_records,
                            generated_checkpoint_records_
                        )
                ) / active_cells) {
                throw std::runtime_error(
                    "visual density sustain timeline exceeds optional sidecar budget"
                );
            }
            write_active_checkpoint(*next_sustain_checkpoint_);
            generated_checkpoint_records_ += active_cells;
            advance_checkpoint();
        }
        const bool checkpoint_at_bucket = next_sustain_checkpoint_.has_value()
            && *next_sustain_checkpoint_ == bucket;
        for (std::uint16_t lane = 0U; lane < key_count_; ++lane) {
            for (const auto owner : {PackedNoteOwner::opponent, PackedNoteOwner::player}) {
                const auto cell = cell_index(lane, owner);
                const auto& counts = cells[cell];
                if (counts.empty()) {
                    continue;
                }
                if (!write_record(level_zero_, {
                        bucket,
                        lane,
                        owner,
                        counts,
                        active_sustains_[cell],
                    })) {
                    throw std::runtime_error("cannot write visual density level 0");
                }
                ++level_zero_count_;
                active_sustains_[cell] = saturated_add(
                    active_sustains_[cell],
                    counts.sustain_starts
                );
                active_sustains_[cell] = counts.sustain_ends
                        > active_sustains_[cell]
                    ? 0U
                    : active_sustains_[cell] - counts.sustain_ends;
            }
        }
        if (checkpoint_at_bucket) {
            for (std::uint16_t lane = 0U; lane < key_count_; ++lane) {
                for (const auto owner : {
                        PackedNoteOwner::opponent,
                        PackedNoteOwner::player,
                    }) {
                    const auto cell = cell_index(lane, owner);
                    if (!cells[cell].empty() || active_sustains_[cell] == 0U) {
                        continue;
                    }
                    if (!write_record(level_zero_, {
                            bucket,
                            lane,
                            owner,
                            {},
                            active_sustains_[cell],
                        })) {
                        throw std::runtime_error(
                            "cannot write visual density sustain checkpoint"
                        );
                    }
                    ++level_zero_count_;
                }
            }
            advance_checkpoint();
        }
        if (any_active_sustains()) {
            if (!next_sustain_checkpoint_.has_value()) {
                schedule_checkpoint_after(bucket);
            }
        } else {
            next_sustain_checkpoint_.reset();
        }
    }

    void build_next_level(const std::uint16_t level) {
        const auto input_path = level_paths_.at(level - 1U);
        const auto output_path = unique_path(
            working_directory_, ".pulseforge-pvd-level", nonce_, level
        );
        std::ifstream input(input_path, std::ios::binary);
        std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
        if (!input || !output) {
            throw std::runtime_error("cannot create coarser visual density level");
        }

        struct AggregateCell final {
            Accum counts;
            std::uint64_t active_sustains_at_bucket_start{};
            bool seen{};
        };
        std::optional<std::int64_t> aggregate_bucket;
        std::vector<AggregateCell> aggregate_cells(
            static_cast<std::size_t>(key_count_) * 2U
        );
        std::uint64_t output_count{};
        const auto flush_bucket = [&]() {
            if (!aggregate_bucket.has_value()) {
                return;
            }
            for (std::uint16_t lane = 0U; lane < key_count_; ++lane) {
                for (const auto owner : {
                        PackedNoteOwner::opponent,
                        PackedNoteOwner::player,
                    }) {
                    auto& cell = aggregate_cells[cell_index(lane, owner)];
                    if (cell.seen && (!cell.counts.empty()
                            || cell.active_sustains_at_bucket_start != 0U)) {
                        if (!write_record(output, {
                                *aggregate_bucket,
                                lane,
                                owner,
                                cell.counts,
                                cell.active_sustains_at_bucket_start,
                            })) {
                            throw std::runtime_error(
                                "cannot write coarser visual density level"
                            );
                        }
                        ++output_count;
                    }
                    cell = {};
                }
            }
        };
        DiskRecord record;
        while (read_record(input, record)) {
            const auto parent_bucket = floor_div_two(record.bucket_index);
            if (aggregate_bucket.has_value()
                && *aggregate_bucket != parent_bucket) {
                flush_bucket();
            }
            aggregate_bucket = parent_bucket;
            auto& cell = aggregate_cells[
                cell_index(record.lane, record.owner)
            ];
            if (!cell.seen) {
                cell.active_sustains_at_bucket_start =
                    record.active_sustains_at_bucket_start;
                cell.seen = true;
            }
            merge_accum(cell.counts, record.counts);
        }
        flush_bucket();
        output.flush();
        if (!output) {
            throw std::runtime_error("cannot flush coarser visual density level");
        }
        level_paths_.push_back(output_path);
        level_counts_.push_back(output_count);
    }

    void cleanup_levels() noexcept {
        std::error_code ignored;
        for (const auto& path : level_paths_) {
            std::filesystem::remove(path, ignored);
            ignored.clear();
        }
        level_paths_.clear();
    }

    void cleanup() noexcept {
        if (level_zero_.is_open()) {
            level_zero_.close();
        }
        cleanup_levels();
    }

    std::uint16_t key_count_{};
    std::filesystem::path working_directory_;
    std::vector<bool> hurt_kinds_;
    std::uint64_t nonce_{};
    std::ofstream level_zero_;
    std::vector<std::filesystem::path> level_paths_;
    std::vector<std::uint64_t> level_counts_;
    std::map<std::int64_t, std::vector<Accum>> pending_;
    std::vector<std::uint64_t> active_sustains_;
    std::optional<std::int64_t> next_sustain_checkpoint_;
    std::optional<std::int64_t> last_time_;
    std::uint64_t level_zero_count_{};
    std::uint64_t generated_checkpoint_records_{};
    bool finished_{};
};

VisualDensityIndexBuilder::VisualDensityIndexBuilder(
    const std::uint16_t key_count,
    const std::span<const std::string> kinds,
    const std::filesystem::path& working_directory
) : implementation_(std::make_unique<Impl>(key_count, kinds, working_directory)) {}

VisualDensityIndexBuilder::~VisualDensityIndexBuilder() = default;
VisualDensityIndexBuilder::VisualDensityIndexBuilder(VisualDensityIndexBuilder&&) noexcept = default;
VisualDensityIndexBuilder& VisualDensityIndexBuilder::operator=(VisualDensityIndexBuilder&&) noexcept = default;

bool VisualDensityIndexBuilder::add(
    const PackedNote& note,
    std::string* const error
) noexcept {
    try {
        implementation_->add(note);
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            try {
                *error = exception.what();
            } catch (...) {
                // Reporting is best effort; add() is noexcept so low-memory
                // sidecar shutdown can never terminate the authoritative PFC
                // compilation.
            }
        }
        return false;
    } catch (...) {
        if (error != nullptr) {
            try {
                *error = "visual density add failed";
            } catch (...) {
            }
        }
        return false;
    }
}

bool VisualDensityIndexBuilder::finish(
    const std::filesystem::path& destination,
    std::string* const error
) noexcept {
    return implementation_->finish(destination, error);
}

std::optional<VisualDensityIndexReader> VisualDensityIndexReader::open(
    const std::filesystem::path& path,
    std::string* const error
) {
    try {
        std::error_code filesystem_error;
        const auto absolute = std::filesystem::absolute(path, filesystem_error)
            .lexically_normal();
        const auto file_size = std::filesystem::file_size(absolute, filesystem_error);
        if (filesystem_error || file_size < pvd_header_size
            || file_size > pvd_max_file_bytes) {
            throw std::runtime_error("visual density file size is invalid");
        }
        std::ifstream input(absolute, std::ios::binary);
        if (!input) {
            throw std::runtime_error("cannot open visual density file");
        }
        std::array<char, pvd_header_size> header{};
        input.read(header.data(), static_cast<std::streamsize>(header.size()));
        if (input.gcount() != static_cast<std::streamsize>(header.size())) {
            throw std::runtime_error("visual density header is truncated");
        }
        if (!std::equal(pvd_magic.begin(), pvd_magic.end(), header.begin())) {
            throw std::runtime_error("visual density magic is invalid");
        }
        const auto* cursor = header.data() + 4;
        const auto version = read_le<std::uint16_t>(cursor);
        const auto header_size = read_le<std::uint16_t>(cursor);
        const auto key_count = read_le<std::uint16_t>(cursor);
        const auto level_count = read_le<std::uint16_t>(cursor);
        const auto base_bucket = read_le<std::uint32_t>(cursor);
        const auto record_size = read_le<std::uint32_t>(cursor);
        const auto declared_file_size = read_le<std::uint64_t>(cursor);
        (void)read_le<std::uint64_t>(cursor);
        if (version != pvd_version || header_size != pvd_header_size
            || key_count == 0U || level_count == 0U
            || level_count > visual_density_level_count
            || base_bucket != visual_density_base_bucket_us
            || record_size != pvd_record_size
            || declared_file_size != file_size) {
            throw std::runtime_error("visual density header is incompatible");
        }
        VisualDensityIndexReader reader;
        reader.path_ = absolute;
        reader.key_count_ = key_count;
        reader.level_count_ = level_count;
        reader.base_bucket_us_ = base_bucket;
        reader.file_size_ = file_size;
        reader.levels_.reserve(level_count);
        std::uint64_t previous_end = pvd_header_size;
        for (std::uint16_t level = 0U; level < level_count; ++level) {
            LevelInfo info;
            info.bucket_width_us = read_le<std::uint64_t>(cursor);
            info.file_offset = read_le<std::uint64_t>(cursor);
            info.record_count = read_le<std::uint64_t>(cursor);
            info.byte_size = read_le<std::uint64_t>(cursor);
            const auto expected_width = static_cast<std::uint64_t>(base_bucket) << level;
            const bool record_size_overflow = info.record_count
                > std::numeric_limits<std::uint64_t>::max() / pvd_record_size;
            if (info.bucket_width_us != expected_width
                || info.file_offset < previous_end
                || record_size_overflow
                || (!record_size_overflow
                    && info.byte_size != info.record_count * pvd_record_size)
                || info.file_offset > file_size
                || info.byte_size > file_size - info.file_offset) {
                throw std::runtime_error("visual density level geometry is invalid");
            }
            previous_end = info.file_offset + info.byte_size;
            reader.levels_.push_back(info);
        }
        return reader;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = exception.what();
        }
        return std::nullopt;
    }
}

VisualDensityVisitResult VisualDensityIndexReader::visit(
    const std::int64_t first_time_us,
    const std::int64_t last_time_us,
    const std::uint64_t target_bucket_us,
    void* const context,
    const VisualDensityVisitor visitor
) const {
    VisualDensityVisitResult result;
    try {
        if (visitor == nullptr) {
            throw std::runtime_error("visual density visitor is null");
        }
        if (first_time_us > last_time_us || levels_.empty()) {
            return result;
        }
        std::size_t selected = 0U;
        for (std::size_t index = 1U; index < levels_.size(); ++index) {
            if (levels_[index].bucket_width_us > target_bucket_us) {
                break;
            }
            selected = index;
        }
        const auto& level = levels_[selected];
        result.selected_bucket_us = level.bucket_width_us;
        if (level.record_count == 0U) {
            return result;
        }
        const auto first_bucket = floor_div(first_time_us, level.bucket_width_us);
        const auto last_bucket = floor_div(last_time_us, level.bucket_width_us);
        std::ifstream input(path_, std::ios::binary);
        if (!input) {
            throw std::runtime_error("cannot open visual density level");
        }

        auto read_at = [&](const std::uint64_t index) {
            if (index >= level.record_count) {
                throw std::runtime_error("visual density record index is invalid");
            }
            input.clear();
            input.seekg(
                static_cast<std::streamoff>(
                    level.file_offset + index * pvd_record_size
                ),
                std::ios::beg
            );
            if (!input) {
                throw std::runtime_error("cannot seek visual density record");
            }
            DiskRecord record;
            if (!read_record(input, record)) {
                throw std::runtime_error("cannot read visual density record");
            }
            return record;
        };

        const auto lower_bound_bucket = [&](const std::int64_t bucket) {
            std::uint64_t low = 0U;
            std::uint64_t high = level.record_count;
            while (low < high) {
                const auto middle = low + (high - low) / 2U;
                const auto record = read_at(middle);
                if (record.bucket_index < bucket) {
                    low = middle + 1U;
                } else {
                    high = middle;
                }
            }
            return low;
        };
        const auto low = lower_bound_bucket(first_bucket);

        // Periodic active-sustain checkpoints make the prefix reconstruction
        // bounded even when a long hold begins far before this viewport.
        const auto level_scale = std::uint64_t{1U} << selected;
        const auto checkpoint_span = std::max<std::uint64_t>(
            1U,
            (static_cast<std::uint64_t>(pvd_sustain_checkpoint_buckets)
                    + level_scale - 1U)
                / level_scale
        );
        const auto room_before_first = static_cast<std::uint64_t>(first_bucket)
            - static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::min()
            );
        const auto warmup_bucket = checkpoint_span > room_before_first
            ? std::numeric_limits<std::int64_t>::min()
            : first_bucket - static_cast<std::int64_t>(checkpoint_span);
        const auto warmup_low = lower_bound_bucket(warmup_bucket);
        std::uint64_t range_low = low;
        while (range_low > 0U) {
            const auto previous = read_at(range_low - 1U);
            if (previous.bucket_index != first_bucket) {
                break;
            }
            --range_low;
        }
        std::vector<std::uint64_t> active_sustains(
            static_cast<std::size_t>(key_count_) * 2U
        );
        input.clear();
        input.seekg(
            static_cast<std::streamoff>(
                level.file_offset + warmup_low * pvd_record_size
            ),
            std::ios::beg
        );
        for (auto index = warmup_low; index < range_low; ++index) {
            DiskRecord record;
            if (!read_record(input, record)) {
                break;
            }
            if (record.bucket_index >= first_bucket) {
                break;
            }
            if (record.lane >= key_count_
                || static_cast<std::uint8_t>(record.owner) > 1U) {
                throw std::runtime_error("visual density record is invalid");
            }
            const auto owner_index = record.owner == PackedNoteOwner::player
                ? std::size_t{1U}
                : std::size_t{0U};
            const auto cell = owner_index * static_cast<std::size_t>(key_count_)
                + record.lane;
            auto active = saturated_add(
                record.active_sustains_at_bucket_start,
                record.counts.sustain_starts
            );
            active = record.counts.sustain_ends > active
                ? 0U
                : active - record.counts.sustain_ends;
            active_sustains[cell] = active;
        }
        for (std::uint16_t lane = 0U; lane < key_count_; ++lane) {
            for (const auto owner : {
                    PackedNoteOwner::opponent,
                    PackedNoteOwner::player,
                }) {
                const auto owner_index = owner == PackedNoteOwner::player
                    ? std::size_t{1U}
                    : std::size_t{0U};
                const auto active = active_sustains[
                    owner_index * static_cast<std::size_t>(key_count_) + lane
                ];
                if (active == 0U) {
                    continue;
                }
                visitor(context, VisualDensityBucket{
                    first_bucket,
                    level.bucket_width_us,
                    lane,
                    owner,
                    0U,
                    0U,
                    0U,
                    0U,
                    active,
                });
            }
        }

        input.clear();
        input.seekg(
            static_cast<std::streamoff>(
                level.file_offset + range_low * pvd_record_size
            ),
            std::ios::beg
        );
        for (auto index = range_low; index < level.record_count; ++index) {
            DiskRecord record;
            if (!read_record(input, record)) {
                break;
            }
            if (record.bucket_index > last_bucket) {
                break;
            }
            if (record.lane >= key_count_
                || static_cast<std::uint8_t>(record.owner) > 1U) {
                throw std::runtime_error("visual density record is invalid");
            }
            visitor(context, VisualDensityBucket{
                record.bucket_index,
                level.bucket_width_us,
                record.lane,
                record.owner,
                record.counts.normal_heads,
                record.counts.hurt_heads,
                record.counts.sustain_starts,
                record.counts.sustain_ends,
                record.active_sustains_at_bucket_start,
            });
            ++result.buckets_visited;
        }
        return result;
    } catch (const std::exception& exception) {
        result.error = exception.what();
        return result;
    }
}

std::uint16_t VisualDensityIndexReader::key_count() const noexcept {
    return key_count_;
}

std::uint16_t VisualDensityIndexReader::level_count() const noexcept {
    return level_count_;
}

std::uint32_t VisualDensityIndexReader::base_bucket_us() const noexcept {
    return base_bucket_us_;
}

const std::filesystem::path& VisualDensityIndexReader::path() const noexcept {
    return path_;
}

}  // namespace pulseforge
