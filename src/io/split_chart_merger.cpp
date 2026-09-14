#include "pulseforge/split_chart_merger.hpp"

#include "pulseforge/ascii_number.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace pulseforge {
namespace {

constexpr std::string_view section_notes_key{"sectionNotes"};
constexpr std::size_t minimum_buffer_bytes = 4U * 1024U;
constexpr std::size_t maximum_buffer_bytes = 64U * 1024U * 1024U;
constexpr std::size_t minimum_sort_memory_bytes = 1U * 1024U * 1024U;
constexpr std::size_t maximum_open_runs_hard = 128U;

struct ArrayRange final {
    std::uint64_t content_start{};
    std::uint64_t content_end{};
};

struct IndexedChart final {
    std::filesystem::path path;
    std::vector<ArrayRange> sections;
    std::uint64_t size{};
};

[[nodiscard]] std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a')
                                    : static_cast<char>(c);
    });
    return value;
}

[[nodiscard]] bool json_extension(const std::filesystem::path& path) {
    return lower_ascii(path.extension().string()) == ".json";
}

[[nodiscard]] bool natural_less(
    const std::filesystem::path& left_path,
    const std::filesystem::path& right_path
) {
    const auto left = lower_ascii(left_path.filename().string());
    const auto right = lower_ascii(right_path.filename().string());
    std::size_t li = 0U;
    std::size_t ri = 0U;
    while (li < left.size() && ri < right.size()) {
        const bool ld = left[li] >= '0' && left[li] <= '9';
        const bool rd = right[ri] >= '0' && right[ri] <= '9';
        if (ld && rd) {
            std::size_t le = li;
            std::size_t re = ri;
            while (le < left.size() && left[le] >= '0' && left[le] <= '9') ++le;
            while (re < right.size() && right[re] >= '0' && right[re] <= '9') ++re;
            std::size_t lz = li;
            std::size_t rz = ri;
            while (lz < le && left[lz] == '0') ++lz;
            while (rz < re && right[rz] == '0') ++rz;
            const auto llen = le - lz;
            const auto rlen = re - rz;
            if (llen != rlen) return llen < rlen;
            const int numeric = left.compare(lz, llen, right, rz, rlen);
            if (numeric != 0) return numeric < 0;
            // Same numeric value: fewer leading zeroes sorts first.
            const auto lraw = le - li;
            const auto rraw = re - ri;
            if (lraw != rraw) return lraw < rraw;
            li = le;
            ri = re;
            continue;
        }
        if (left[li] != right[ri]) return left[li] < right[ri];
        ++li;
        ++ri;
    }
    if (li != left.size() || ri != right.size()) return li == left.size();
    return left_path.generic_string() < right_path.generic_string();
}

class BufferedReader final {
public:
    BufferedReader(
        const std::filesystem::path& path,
        const std::size_t requested_buffer
    ) : stream_(path, std::ios::binary),
        buffer_(std::clamp(
            requested_buffer,
            minimum_buffer_bytes,
            maximum_buffer_bytes
        )) {
        if (!stream_) {
            error_ = "cannot open input file: " + path.string();
            return;
        }
        std::error_code ec;
        const auto raw_size = std::filesystem::file_size(path, ec);
        if (ec || raw_size > std::numeric_limits<std::uint64_t>::max()) {
            error_ = "cannot determine input file size: " + path.string();
            return;
        }
        size_ = static_cast<std::uint64_t>(raw_size);
    }

    [[nodiscard]] bool good() const noexcept { return error_.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] std::uint64_t position() const noexcept { return absolute_; }

    bool seek(const std::uint64_t position) {
        if (position > size_) return false;
        absolute_ = position;
        buffer_start_ = position;
        buffer_size_ = 0U;
        buffer_offset_ = 0U;
        stream_.clear();
        return true;
    }

    [[nodiscard]] std::optional<unsigned char> get() {
        if (absolute_ >= size_) return std::nullopt;
        if (buffer_offset_ >= buffer_size_ || absolute_ < buffer_start_
            || absolute_ >= buffer_start_ + buffer_size_) {
            if (!refill()) return std::nullopt;
        }
        const auto offset = static_cast<std::size_t>(absolute_ - buffer_start_);
        if (offset >= buffer_size_) return std::nullopt;
        const auto value = static_cast<unsigned char>(buffer_[offset]);
        ++absolute_;
        buffer_offset_ = offset + 1U;
        return value;
    }

private:
    [[nodiscard]] bool refill() {
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(absolute_), std::ios::beg);
        if (!stream_) return false;
        stream_.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
        const auto count = stream_.gcount();
        if (count <= 0) return false;
        buffer_start_ = absolute_;
        buffer_size_ = static_cast<std::size_t>(count);
        buffer_offset_ = 0U;
        return true;
    }

    std::ifstream stream_;
    std::vector<char> buffer_;
    std::string error_;
    std::uint64_t size_{};
    std::uint64_t absolute_{};
    std::uint64_t buffer_start_{};
    std::size_t buffer_size_{};
    std::size_t buffer_offset_{};
};

[[nodiscard]] bool is_space(const unsigned char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

[[nodiscard]] std::optional<unsigned char> next_non_space(BufferedReader& reader) {
    while (const auto value = reader.get()) {
        if (!is_space(*value)) return value;
    }
    return std::nullopt;
}

[[nodiscard]] bool cancelled(const SplitChartMergeOptions& options) noexcept {
    return options.cancel != nullptr
        && options.cancel->load(std::memory_order_relaxed);
}

void set_progress_detail(
    SplitChartMergeProgress* const progress,
    std::string detail
) {
    if (progress == nullptr) return;
    std::scoped_lock lock(progress->detail_mutex);
    progress->detail = std::move(detail);
}

void finish_progress(
    SplitChartMergeProgress* const progress,
    const SplitChartMergeResult& result
) {
    if (progress == nullptr) return;
    progress->success.store(result.success, std::memory_order_release);
    progress->cancelled.store(result.cancelled, std::memory_order_release);
    {
        std::scoped_lock lock(progress->detail_mutex);
        progress->error = result.error;
        progress->detail = result.success ? "Complete"
            : result.cancelled ? "Cancelled" : "Failed";
    }
    progress->finished.store(true, std::memory_order_release);
}

[[nodiscard]] std::optional<std::uint64_t> scan_array_end(
    BufferedReader& reader,
    std::string& error
) {
    std::uint64_t depth = 1U;
    bool in_string = false;
    bool escaped = false;
    while (const auto value = reader.get()) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (*value == '\\') {
                escaped = true;
            } else if (*value == '"') {
                in_string = false;
            }
            continue;
        }
        if (*value == '"') {
            in_string = true;
        } else if (*value == '[') {
            ++depth;
        } else if (*value == ']') {
            if (--depth == 0U) {
                return reader.position() - 1U;
            }
        }
    }
    error = "unterminated sectionNotes array";
    return std::nullopt;
}

[[nodiscard]] std::optional<IndexedChart> index_chart(
    const std::filesystem::path& path,
    const SplitChartMergeOptions& options,
    std::string& error
) {
    BufferedReader reader(path, options.input_buffer_bytes);
    if (!reader.good()) {
        error = reader.error();
        return std::nullopt;
    }
    if (reader.size() == 0U) {
        error = "empty chart: " + path.string();
        return std::nullopt;
    }

    IndexedChart indexed;
    indexed.path = path;
    indexed.size = reader.size();
    while (reader.position() < reader.size()) {
        if (cancelled(options)) {
            error = "cancelled";
            return std::nullopt;
        }
        const auto opening = reader.get();
        if (!opening) break;
        if (*opening != '"') continue;

        std::size_t matched = 0U;
        bool exact = true;
        bool escaped = false;
        bool closed = false;
        while (const auto value = reader.get()) {
            if (escaped) {
                escaped = false;
                exact = false;
                continue;
            }
            if (*value == '\\') {
                escaped = true;
                exact = false;
                continue;
            }
            if (*value == '"') {
                closed = true;
                break;
            }
            if (matched >= section_notes_key.size()
                || *value != static_cast<unsigned char>(section_notes_key[matched])) {
                exact = false;
            }
            ++matched;
        }
        if (!closed) {
            error = "unterminated JSON string while indexing " + path.string();
            return std::nullopt;
        }
        if (!exact || matched != section_notes_key.size()) continue;

        const auto colon = next_non_space(reader);
        if (!colon) break;
        if (*colon != ':') continue;
        const auto array_open = next_non_space(reader);
        if (!array_open || *array_open != '[') {
            error = "sectionNotes is not an array in " + path.string();
            return std::nullopt;
        }
        const auto content_start = reader.position();
        const auto content_end = scan_array_end(reader, error);
        if (!content_end) {
            error = path.string() + ": " + error;
            return std::nullopt;
        }
        indexed.sections.push_back({content_start, *content_end});
    }

    if (indexed.sections.empty()) {
        error = "no sectionNotes arrays found in " + path.string();
        return std::nullopt;
    }
    return indexed;
}

class ArrayElementReader final {
public:
    ArrayElementReader(BufferedReader& reader, const ArrayRange range)
        : reader_(&reader), end_(range.content_end) {
        valid_ = reader_->seek(range.content_start);
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }

    [[nodiscard]] std::optional<std::string> next(std::string& error) {
        if (!valid_ || reader_ == nullptr) return std::nullopt;
        std::optional<unsigned char> first;
        while (reader_->position() < end_) {
            const auto value = reader_->get();
            if (!value) break;
            if (!is_space(*value) && *value != ',') {
                first = value;
                break;
            }
        }
        if (!first) return std::nullopt;

        std::string raw;
        raw.reserve(96U);
        raw.push_back(static_cast<char>(*first));
        bool in_string = *first == '"';
        bool escaped = false;
        std::uint64_t braces = *first == '{' ? 1U : 0U;
        std::uint64_t brackets = *first == '[' ? 1U : 0U;
        const bool primitive = !in_string && braces == 0U && brackets == 0U;

        while (reader_->position() < end_) {
            const auto value = reader_->get();
            if (!value) break;
            if (in_string) {
                raw.push_back(static_cast<char>(*value));
                if (escaped) {
                    escaped = false;
                } else if (*value == '\\') {
                    escaped = true;
                } else if (*value == '"') {
                    in_string = false;
                    if (braces == 0U && brackets == 0U) return raw;
                }
                continue;
            }

            if (*value == '"') {
                in_string = true;
                raw.push_back('"');
            } else if (*value == '{') {
                ++braces;
                raw.push_back('{');
            } else if (*value == '}') {
                if (braces == 0U) {
                    error = "unbalanced object inside sectionNotes";
                    return std::nullopt;
                }
                --braces;
                raw.push_back('}');
                if (braces == 0U && brackets == 0U) return raw;
            } else if (*value == '[') {
                ++brackets;
                raw.push_back('[');
            } else if (*value == ']') {
                if (brackets == 0U) {
                    error = "unbalanced nested array inside sectionNotes";
                    return std::nullopt;
                }
                --brackets;
                raw.push_back(']');
                if (braces == 0U && brackets == 0U) return raw;
            } else if (primitive && braces == 0U && brackets == 0U
                       && (is_space(*value) || *value == ',')) {
                return raw;
            } else {
                raw.push_back(static_cast<char>(*value));
            }
        }
        if (in_string || braces != 0U || brackets != 0U) {
            error = "truncated JSON value inside sectionNotes";
            return std::nullopt;
        }
        return raw.empty() ? std::nullopt : std::optional<std::string>{std::move(raw)};
    }

private:
    BufferedReader* reader_{};
    std::uint64_t end_{};
    bool valid_{};
};

[[nodiscard]] std::optional<double> note_time(
    const std::string_view raw,
    std::string& error
) {
    std::size_t offset = 0U;
    while (offset < raw.size() && is_space(static_cast<unsigned char>(raw[offset]))) ++offset;
    if (offset >= raw.size()) {
        error = "empty note JSON value";
        return std::nullopt;
    }
    if (raw[offset] == '[') {
        ++offset;
        while (offset < raw.size() && is_space(static_cast<unsigned char>(raw[offset]))) ++offset;
        const auto start = offset;
        while (offset < raw.size() && raw[offset] != ',' && raw[offset] != ']'
               && !is_space(static_cast<unsigned char>(raw[offset]))) {
            ++offset;
        }
        if (start == offset) {
            error = "note is missing strumTime";
            return std::nullopt;
        }
        double value = 0.0;
        if (!parse_ascii_floating<double>(raw.substr(start, offset - start), value)
            || !std::isfinite(value)) {
            error = "invalid note strumTime";
            return std::nullopt;
        }
        return value;
    }
    if (raw[offset] == '{') {
        try {
            const auto value = nlohmann::json::parse(raw.begin(), raw.end());
            for (const auto* key : {"time_ms", "timeMs", "strumTime", "time"}) {
                const auto found = value.find(key);
                if (found != value.end() && found->is_number()) {
                    const auto time = found->get<double>();
                    if (std::isfinite(time)) return time;
                }
            }
        } catch (const std::exception&) {
            error = "invalid object note JSON";
            return std::nullopt;
        }
        error = "object note is missing a supported time field";
        return std::nullopt;
    }
    error = "sectionNotes element is not an array or object";
    return std::nullopt;
}

[[nodiscard]] bool copy_range(
    std::ifstream& source,
    std::ofstream& output,
    const std::uint64_t start,
    const std::uint64_t end,
    std::vector<char>& buffer,
    std::string& error
) {
    if (end < start) {
        error = "invalid template copy range";
        return false;
    }
    source.clear();
    source.seekg(static_cast<std::streamoff>(start), std::ios::beg);
    if (!source) {
        error = "cannot seek template chart";
        return false;
    }
    auto remaining = end - start;
    while (remaining != 0U) {
        const auto request = static_cast<std::streamsize>(
            std::min<std::uint64_t>(remaining, buffer.size())
        );
        source.read(buffer.data(), request);
        const auto count = source.gcount();
        if (count <= 0) {
            error = "template ended while copying merged chart";
            return false;
        }
        output.write(buffer.data(), count);
        if (!output) {
            error = "failed while writing merged chart";
            return false;
        }
        remaining -= static_cast<std::uint64_t>(count);
    }
    return true;
}

[[nodiscard]] std::filesystem::path make_temp_directory(
    const std::filesystem::path& parent,
    std::string& error
) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        error = "cannot create temporary parent: " + ec.message();
        return {};
    }
    const auto seed = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    for (std::uint32_t attempt = 0U; attempt < 128U; ++attempt) {
        const auto candidate = parent
            / (".pulseforge-chart-merge-" + std::to_string(seed)
               + "-" + std::to_string(attempt));
        ec.clear();
        if (std::filesystem::create_directory(candidate, ec)) return candidate;
        if (ec && ec != std::errc::file_exists) {
            error = "cannot create temporary merge directory: " + ec.message();
            return {};
        }
    }
    error = "cannot allocate a unique temporary merge directory";
    return {};
}

[[nodiscard]] bool publish_atomic(
    const std::filesystem::path& temporary,
    const std::filesystem::path& target,
    std::string& error
) {
#if defined(_WIN32)
    if (MoveFileExW(
            temporary.c_str(),
            target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
        ) != 0) {
        return true;
    }
    error = "cannot publish merged chart (MoveFileEx failed: "
        + std::to_string(static_cast<unsigned long>(GetLastError())) + ")";
    return false;
#else
    std::error_code ec;
    std::filesystem::rename(temporary, target, ec);
    if (!ec) return true;
    error = "cannot publish merged chart: " + ec.message();
    return false;
#endif
}

struct SortRecord final {
    double key{};
    std::uint64_t serial{};
    std::string raw;
};

struct RunRecord final {
    double key{};
    std::uint64_t serial{};
    std::string raw;
};

void write_u64(std::ofstream& stream, const std::uint64_t value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

[[nodiscard]] bool write_run_record(
    std::ofstream& stream,
    const SortRecord& record
) {
    stream.write(reinterpret_cast<const char*>(&record.key), sizeof(record.key));
    write_u64(stream, record.serial);
    write_u64(stream, static_cast<std::uint64_t>(record.raw.size()));
    if (!record.raw.empty()) {
        stream.write(record.raw.data(), static_cast<std::streamsize>(record.raw.size()));
    }
    return static_cast<bool>(stream);
}

[[nodiscard]] bool write_run_record(
    std::ofstream& stream,
    const RunRecord& record
) {
    stream.write(reinterpret_cast<const char*>(&record.key), sizeof(record.key));
    write_u64(stream, record.serial);
    write_u64(stream, static_cast<std::uint64_t>(record.raw.size()));
    if (!record.raw.empty()) {
        stream.write(record.raw.data(), static_cast<std::streamsize>(record.raw.size()));
    }
    return static_cast<bool>(stream);
}

[[nodiscard]] bool read_run_record(
    std::ifstream& stream,
    RunRecord& record,
    std::string& error
) {
    double key = 0.0;
    stream.read(reinterpret_cast<char*>(&key), sizeof(key));
    if (stream.eof() && stream.gcount() == 0) return false;
    if (stream.gcount() != static_cast<std::streamsize>(sizeof(key))) {
        error = "truncated external-sort run header";
        return false;
    }
    std::uint64_t serial = 0U;
    std::uint64_t length = 0U;
    stream.read(reinterpret_cast<char*>(&serial), sizeof(serial));
    stream.read(reinterpret_cast<char*>(&length), sizeof(length));
    if (!stream) {
        error = "truncated external-sort run header";
        return false;
    }
    if (length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        error = "external-sort note payload exceeds addressable memory";
        return false;
    }
    record.key = key;
    record.serial = serial;
    record.raw.resize(static_cast<std::size_t>(length));
    if (length != 0U) {
        stream.read(record.raw.data(), static_cast<std::streamsize>(length));
        if (!stream) {
            error = "truncated external-sort note payload";
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool flush_sorted_run(
    std::vector<SortRecord>& records,
    const std::filesystem::path& path,
    std::string& error
) {
    std::stable_sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
        if (left.key != right.key) return left.key < right.key;
        return left.serial < right.serial;
    });
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot create external-sort run: " + path.string();
        return false;
    }
    for (const auto& record : records) {
        if (!write_run_record(stream, record)) {
            error = "failed writing external-sort run: " + path.string();
            return false;
        }
    }
    stream.flush();
    if (!stream) {
        error = "failed flushing external-sort run: " + path.string();
        return false;
    }
    records.clear();
    return true;
}

struct RunHeapItem final {
    RunRecord record;
    std::size_t source{};
};

struct RunHeapGreater final {
    bool operator()(const RunHeapItem& left, const RunHeapItem& right) const noexcept {
        if (left.record.key != right.record.key) return left.record.key > right.record.key;
        return left.record.serial > right.record.serial;
    }
};

[[nodiscard]] bool merge_run_group(
    const std::span<const std::filesystem::path> inputs,
    const std::filesystem::path& output,
    const SplitChartMergeOptions& options,
    std::string& error
) {
    std::vector<std::ifstream> streams;
    streams.reserve(inputs.size());
    for (const auto& path : inputs) {
        streams.emplace_back(path, std::ios::binary);
        if (!streams.back()) {
            error = "cannot reopen external-sort run: " + path.string();
            return false;
        }
    }
    std::ofstream destination(output, std::ios::binary | std::ios::trunc);
    if (!destination) {
        error = "cannot create merged external-sort run: " + output.string();
        return false;
    }

    std::priority_queue<RunHeapItem, std::vector<RunHeapItem>, RunHeapGreater> heap;
    for (std::size_t index = 0U; index < streams.size(); ++index) {
        RunRecord record;
        std::string read_error;
        if (read_run_record(streams[index], record, read_error)) {
            heap.push({std::move(record), index});
        } else if (!read_error.empty()) {
            error = std::move(read_error);
            return false;
        }
    }
    while (!heap.empty()) {
        if (cancelled(options)) {
            error = "cancelled";
            return false;
        }
        auto item = heap.top();
        heap.pop();
        if (!write_run_record(destination, item.record)) {
            error = "failed while merging external-sort runs";
            return false;
        }
        RunRecord next;
        std::string read_error;
        if (read_run_record(streams[item.source], next, read_error)) {
            heap.push({std::move(next), item.source});
        } else if (!read_error.empty()) {
            error = std::move(read_error);
            return false;
        }
    }
    destination.flush();
    if (!destination) {
        error = "failed flushing merged external-sort run";
        return false;
    }
    return true;
}

[[nodiscard]] bool reduce_runs(
    std::vector<std::filesystem::path>& runs,
    const std::filesystem::path& section_temp,
    const SplitChartMergeOptions& options,
    std::string& error
) {
    const auto fan_in = std::clamp<std::size_t>(
        options.maximum_open_runs,
        2U,
        maximum_open_runs_hard
    );
    std::uint64_t pass = 0U;
    while (runs.size() > fan_in) {
        std::vector<std::filesystem::path> next;
        for (std::size_t begin = 0U; begin < runs.size(); begin += fan_in) {
            const auto end = std::min(runs.size(), begin + fan_in);
            const auto output = section_temp
                / ("merge-" + std::to_string(pass) + "-"
                   + std::to_string(next.size()) + ".bin");
            if (!merge_run_group(
                    std::span<const std::filesystem::path>{
                        runs.data() + begin,
                        end - begin,
                    },
                    output,
                    options,
                    error
                )) {
                return false;
            }
            next.push_back(output);
            for (std::size_t index = begin; index < end; ++index) {
                std::error_code remove_error;
                std::filesystem::remove(runs[index], remove_error);
            }
        }
        runs = std::move(next);
        ++pass;
    }
    return true;
}

[[nodiscard]] bool write_external_sorted_section(
    std::ofstream& output,
    std::vector<std::unique_ptr<BufferedReader>>& readers,
    const std::vector<IndexedChart>& indexed,
    const std::size_t section,
    const std::filesystem::path& section_temp,
    const SplitChartMergeOptions& options,
    bool& first_output,
    std::uint64_t& note_count,
    std::string& error
) {
    std::error_code ec;
    std::filesystem::create_directories(section_temp, ec);
    if (ec) {
        error = "cannot create external-sort section directory: " + ec.message();
        return false;
    }

    const auto memory_budget = std::max(options.sort_memory_bytes, minimum_sort_memory_bytes);
    std::vector<SortRecord> records;
    records.reserve(32'768U);
    std::vector<std::filesystem::path> runs;
    std::size_t memory_used = 0U;
    std::uint64_t serial = 0U;

    const auto flush = [&]() -> bool {
        if (records.empty()) return true;
        const auto path = section_temp / ("run-" + std::to_string(runs.size()) + ".bin");
        if (!flush_sorted_run(records, path, error)) return false;
        runs.push_back(path);
        memory_used = 0U;
        return true;
    };

    for (std::size_t source = 0U; source < indexed.size(); ++source) {
        ArrayElementReader elements(*readers[source], indexed[source].sections[section]);
        if (!elements.valid()) {
            error = "cannot seek split chart section";
            return false;
        }
        while (true) {
            if (cancelled(options)) {
                error = "cancelled";
                return false;
            }
            std::string element_error;
            auto raw = elements.next(element_error);
            if (!raw) {
                if (!element_error.empty()) {
                    error = std::move(element_error);
                    return false;
                }
                break;
            }
            auto key = note_time(*raw, error);
            if (!key) return false;
            memory_used += raw->size() + sizeof(SortRecord) + 32U;
            records.push_back({*key, serial++, std::move(*raw)});
            if (memory_used >= memory_budget && !flush()) return false;
        }
    }
    if (!flush()) return false;
    if (runs.empty()) return true;
    if (!reduce_runs(runs, section_temp, options, error)) return false;

    // The reduced set is bounded by maximum_open_runs; merge it directly into
    // JSON output instead of producing another potentially enormous temp file.
    std::vector<std::ifstream> streams;
    streams.reserve(runs.size());
    for (const auto& path : runs) {
        streams.emplace_back(path, std::ios::binary);
        if (!streams.back()) {
            error = "cannot reopen external-sort run: " + path.string();
            return false;
        }
    }
    std::priority_queue<RunHeapItem, std::vector<RunHeapItem>, RunHeapGreater> heap;
    for (std::size_t index = 0U; index < streams.size(); ++index) {
        RunRecord record;
        std::string read_error;
        if (read_run_record(streams[index], record, read_error)) {
            heap.push({std::move(record), index});
        } else if (!read_error.empty()) {
            error = std::move(read_error);
            return false;
        }
    }
    while (!heap.empty()) {
        if (cancelled(options)) {
            error = "cancelled";
            return false;
        }
        auto item = heap.top();
        heap.pop();
        if (!first_output) output.put(',');
        output.write(item.record.raw.data(), static_cast<std::streamsize>(item.record.raw.size()));
        if (!output) {
            error = "failed writing externally sorted note";
            return false;
        }
        first_output = false;
        ++note_count;
        if (options.progress != nullptr) {
            options.progress->notes_written.store(note_count, std::memory_order_relaxed);
        }
        RunRecord next;
        std::string read_error;
        if (read_run_record(streams[item.source], next, read_error)) {
            heap.push({std::move(next), item.source});
        } else if (!read_error.empty()) {
            error = std::move(read_error);
            return false;
        }
    }
    return true;
}

struct NoteHeapItem final {
    double time{};
    std::uint64_t serial{};
    std::size_t source{};
    std::string raw;
};

struct NoteHeapGreater final {
    bool operator()(const NoteHeapItem& left, const NoteHeapItem& right) const noexcept {
        if (left.time != right.time) return left.time > right.time;
        return left.serial > right.serial;
    }
};

[[nodiscard]] bool write_kway_section(
    std::ofstream& output,
    std::vector<std::unique_ptr<BufferedReader>>& readers,
    const std::vector<IndexedChart>& indexed,
    const std::size_t section,
    const SplitChartMergeOptions& options,
    bool& first_output,
    std::uint64_t& note_count,
    std::string& error
) {
    std::vector<std::unique_ptr<ArrayElementReader>> elements;
    elements.reserve(indexed.size());
    std::vector<std::optional<double>> previous(indexed.size());
    std::priority_queue<NoteHeapItem, std::vector<NoteHeapItem>, NoteHeapGreater> heap;
    std::uint64_t serial = 0U;

    for (std::size_t source = 0U; source < indexed.size(); ++source) {
        elements.push_back(std::make_unique<ArrayElementReader>(
            *readers[source], indexed[source].sections[section]
        ));
        if (!elements.back()->valid()) {
            error = "cannot seek split chart section";
            return false;
        }
        std::string element_error;
        auto raw = elements.back()->next(element_error);
        if (!raw) {
            if (!element_error.empty()) {
                error = std::move(element_error);
                return false;
            }
            continue;
        }
        auto time = note_time(*raw, error);
        if (!time) return false;
        previous[source] = *time;
        heap.push({*time, serial++, source, std::move(*raw)});
    }

    while (!heap.empty()) {
        if (cancelled(options)) {
            error = "cancelled";
            return false;
        }
        auto item = heap.top();
        heap.pop();
        if (!first_output) output.put(',');
        output.write(item.raw.data(), static_cast<std::streamsize>(item.raw.size()));
        if (!output) {
            error = "failed writing k-way merged note";
            return false;
        }
        first_output = false;
        ++note_count;
        if (options.progress != nullptr) {
            options.progress->notes_written.store(note_count, std::memory_order_relaxed);
        }

        std::string element_error;
        auto raw = elements[item.source]->next(element_error);
        if (!raw) {
            if (!element_error.empty()) {
                error = std::move(element_error);
                return false;
            }
            continue;
        }
        auto time = note_time(*raw, error);
        if (!time) return false;
        if (previous[item.source].has_value() && *time < *previous[item.source]) {
            error = "source section is not time-sorted; use external_sort strategy";
            return false;
        }
        previous[item.source] = *time;
        heap.push({*time, serial++, item.source, std::move(*raw)});
    }
    return true;
}

[[nodiscard]] bool write_concatenated_section(
    std::ofstream& output,
    std::vector<std::unique_ptr<BufferedReader>>& readers,
    const std::vector<IndexedChart>& indexed,
    const std::size_t section,
    const SplitChartMergeOptions& options,
    bool& first_output,
    std::uint64_t& note_count,
    std::string& error
) {
    for (std::size_t source = 0U; source < indexed.size(); ++source) {
        ArrayElementReader elements(*readers[source], indexed[source].sections[section]);
        if (!elements.valid()) {
            error = "cannot seek split chart section";
            return false;
        }
        while (true) {
            if (cancelled(options)) {
                error = "cancelled";
                return false;
            }
            std::string element_error;
            auto raw = elements.next(element_error);
            if (!raw) {
                if (!element_error.empty()) {
                    error = std::move(element_error);
                    return false;
                }
                break;
            }
            if (!first_output) output.put(',');
            output.write(raw->data(), static_cast<std::streamsize>(raw->size()));
            if (!output) {
                error = "failed writing concatenated note";
                return false;
            }
            first_output = false;
            ++note_count;
            if (options.progress != nullptr) {
                options.progress->notes_written.store(note_count, std::memory_order_relaxed);
            }
        }
    }
    return true;
}

}  // namespace

std::vector<std::filesystem::path> discover_split_chart_parts(
    const std::filesystem::path& directory
) {
    std::vector<std::filesystem::path> result;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec) || ec) return result;
    std::filesystem::directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        ec
    );
    const std::filesystem::directory_iterator end;
    while (!ec && iterator != end) {
        std::error_code item_error;
        if (iterator->is_regular_file(item_error) && !item_error
            && json_extension(iterator->path())) {
            result.push_back(iterator->path());
        }
        iterator.increment(ec);
    }
    std::sort(result.begin(), result.end(), natural_less);
    return result;
}

SplitChartMergeResult merge_split_charts(
    const std::span<const std::filesystem::path> inputs,
    const std::filesystem::path& output,
    const SplitChartMergeOptions& options
) {
    SplitChartMergeResult result;
    result.output_path = output;
    result.input_count = static_cast<std::uint64_t>(inputs.size());
    result.used_external_sort = options.strategy == SplitChartMergeStrategy::external_sort;
    const auto finish = [&]() {
        finish_progress(options.progress, result);
        return result;
    };

    if (inputs.size() < 2U) {
        result.error = "at least two split chart inputs are required";
        return finish();
    }
    if (output.empty()) {
        result.error = "merged chart output path is empty";
        return finish();
    }

    std::error_code ec;
    const auto output_absolute = std::filesystem::absolute(output, ec).lexically_normal();
    if (ec) {
        result.error = "cannot normalize output path: " + ec.message();
        return finish();
    }

    std::uint64_t total_source_bytes = 0U;
    std::vector<IndexedChart> indexed;
    indexed.reserve(inputs.size());
    for (const auto& input : inputs) {
        ec.clear();
        if (!std::filesystem::is_regular_file(input, ec) || ec || !json_extension(input)) {
            result.error = "split chart input is not a regular JSON file: " + input.string();
            return finish();
        }
        const auto input_absolute = std::filesystem::absolute(input, ec).lexically_normal();
        if (ec || input_absolute == output_absolute) {
            result.error = "output must not overwrite a split chart input";
            return finish();
        }
        const auto size = std::filesystem::file_size(input, ec);
        if (ec || size > std::numeric_limits<std::uint64_t>::max()
            || total_source_bytes > std::numeric_limits<std::uint64_t>::max()
                - static_cast<std::uint64_t>(size)) {
            result.error = "split chart source byte count overflow";
            return finish();
        }
        total_source_bytes += static_cast<std::uint64_t>(size);
    }
    result.source_bytes = total_source_bytes;
    if (options.progress != nullptr) {
        options.progress->source_bytes_total.store(total_source_bytes, std::memory_order_relaxed);
        options.progress->source_bytes_indexed.store(0U, std::memory_order_relaxed);
        options.progress->notes_written.store(0U, std::memory_order_relaxed);
        options.progress->sections_done.store(0U, std::memory_order_relaxed);
        options.progress->sections_total.store(0U, std::memory_order_relaxed);
        options.progress->finished.store(false, std::memory_order_relaxed);
        options.progress->success.store(false, std::memory_order_relaxed);
        options.progress->cancelled.store(false, std::memory_order_relaxed);
    }

    std::uint64_t indexed_bytes = 0U;
    set_progress_detail(options.progress, "Indexing split charts");
    for (const auto& input : inputs) {
        if (cancelled(options)) {
            result.cancelled = true;
            return finish();
        }
        std::string error;
        auto chart = index_chart(input, options, error);
        if (!chart) {
            if (error == "cancelled") result.cancelled = true;
            else result.error = std::move(error);
            return finish();
        }
        indexed_bytes += chart->size;
        if (options.progress != nullptr) {
            options.progress->source_bytes_indexed.store(indexed_bytes, std::memory_order_relaxed);
        }
        indexed.push_back(std::move(*chart));
    }

    const auto section_count = indexed.front().sections.size();
    for (const auto& chart : indexed) {
        if (chart.sections.size() != section_count) {
            result.error = "split chart section-count mismatch: " + chart.path.string()
                + " has " + std::to_string(chart.sections.size()) + ", expected "
                + std::to_string(section_count);
            return finish();
        }
    }
    result.section_count = static_cast<std::uint64_t>(section_count);
    result.template_path = indexed.front().path;
    if (options.progress != nullptr) {
        options.progress->sections_total.store(result.section_count, std::memory_order_relaxed);
    }

    ec.clear();
    const auto output_parent = output.parent_path().empty()
        ? std::filesystem::current_path()
        : output.parent_path();
    std::filesystem::create_directories(output_parent, ec);
    if (ec) {
        result.error = "cannot create merged chart output directory: " + ec.message();
        return finish();
    }
    const auto temporary_parent = options.temporary_root.empty()
        ? output_parent
        : options.temporary_root;
    std::string temp_error;
    const auto temp_root = make_temp_directory(temporary_parent, temp_error);
    if (temp_root.empty()) {
        result.error = std::move(temp_error);
        return finish();
    }
    const auto partial = temp_root / "merged.partial.json";

    std::ifstream template_stream(indexed.front().path, std::ios::binary);
    std::ofstream output_stream(partial, std::ios::binary | std::ios::trunc);
    if (!template_stream || !output_stream) {
        result.error = "cannot open merge template or temporary output";
        std::filesystem::remove_all(temp_root, ec);
        return finish();
    }

    std::vector<std::unique_ptr<BufferedReader>> readers;
    readers.reserve(indexed.size());
    for (const auto& chart : indexed) {
        auto reader = std::make_unique<BufferedReader>(chart.path, options.input_buffer_bytes);
        if (!reader->good()) {
            result.error = reader->error();
            std::filesystem::remove_all(temp_root, ec);
            return finish();
        }
        readers.push_back(std::move(reader));
    }

    std::vector<char> copy_buffer(std::clamp(
        options.copy_buffer_bytes,
        minimum_buffer_bytes,
        maximum_buffer_bytes
    ));
    std::uint64_t template_cursor = 0U;
    std::uint64_t note_count = 0U;
    bool merge_ok = true;
    std::string merge_error;

    for (std::size_t section = 0U; section < section_count && merge_ok; ++section) {
        if (cancelled(options)) {
            result.cancelled = true;
            merge_ok = false;
            break;
        }
        set_progress_detail(
            options.progress,
            "Merging section " + std::to_string(section + 1U)
                + "/" + std::to_string(section_count)
        );
        const auto range = indexed.front().sections[section];
        if (!copy_range(
                template_stream,
                output_stream,
                template_cursor,
                range.content_start,
                copy_buffer,
                merge_error
            )) {
            merge_ok = false;
            break;
        }
        bool first_output = true;
        switch (options.strategy) {
        case SplitChartMergeStrategy::k_way:
            merge_ok = write_kway_section(
                output_stream,
                readers,
                indexed,
                section,
                options,
                first_output,
                note_count,
                merge_error
            );
            break;
        case SplitChartMergeStrategy::external_sort:
            merge_ok = write_external_sorted_section(
                output_stream,
                readers,
                indexed,
                section,
                temp_root / ("section-" + std::to_string(section)),
                options,
                first_output,
                note_count,
                merge_error
            );
            break;
        case SplitChartMergeStrategy::concatenate:
            merge_ok = write_concatenated_section(
                output_stream,
                readers,
                indexed,
                section,
                options,
                first_output,
                note_count,
                merge_error
            );
            break;
        }
        template_cursor = range.content_end;
        if (merge_ok && options.progress != nullptr) {
            options.progress->sections_done.store(
                static_cast<std::uint64_t>(section + 1U),
                std::memory_order_relaxed
            );
        }
    }

    if (merge_ok && !copy_range(
            template_stream,
            output_stream,
            template_cursor,
            indexed.front().size,
            copy_buffer,
            merge_error
        )) {
        merge_ok = false;
    }
    output_stream.flush();
    if (merge_ok && !output_stream) {
        merge_ok = false;
        merge_error = "failed flushing merged chart";
    }
    output_stream.close();
    template_stream.close();

    if (!merge_ok) {
        if (cancelled(options) || merge_error == "cancelled") result.cancelled = true;
        else result.error = std::move(merge_error);
        std::filesystem::remove_all(temp_root, ec);
        return finish();
    }

    set_progress_detail(options.progress, "Publishing merged chart");
    std::string publish_error;
    if (!publish_atomic(partial, output, publish_error)) {
        result.error = std::move(publish_error);
        std::filesystem::remove_all(temp_root, ec);
        return finish();
    }
    std::filesystem::remove_all(temp_root, ec);
    result.note_count = note_count;
    result.success = true;
    return finish();
}

}  // namespace pulseforge
