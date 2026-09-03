#pragma once

#include "pulseforge/packed_chart.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pulseforge {

// Description known before a streamed PFC1 write starts. Unlike
// PackedChartData, this does not own the explicit notes.
struct PackedChartStreamSpec {
    std::uint16_t key_count{4U};

    // Physically stored PFC1 note records. Logical notes represented by
    // PatternRun do not contribute to this count.
    std::uint64_t explicit_note_count{};
    std::vector<std::string> kinds;

    // Procedural/compressed logical notes. PatternRun lets a dense coincident
    // stack or arithmetic sequence remain constant-storage in PFC1.
    std::vector<PatternRun> patterns;
};

// Atomic, bounded PFC1 writer for already time-ordered notes. It retains at
// most one encoded chunk in memory and reserves the on-disk directory before
// note payloads are received. The destination must not already exist.
class PackedChartStreamWriter final {
public:
    PackedChartStreamWriter() noexcept;
    ~PackedChartStreamWriter();

    PackedChartStreamWriter(PackedChartStreamWriter&&) noexcept;
    PackedChartStreamWriter& operator=(PackedChartStreamWriter&&) noexcept;

    PackedChartStreamWriter(const PackedChartStreamWriter&) = delete;
    PackedChartStreamWriter& operator=(const PackedChartStreamWriter&) = delete;

    [[nodiscard]] static std::optional<PackedChartStreamWriter> create(
        const std::filesystem::path& destination,
        const PackedChartStreamSpec& spec,
        const PackedChartWriteOptions& options = {},
        std::string* error = nullptr
    );

    [[nodiscard]] bool append(
        const PackedNote& note,
        std::string* error = nullptr
    );
    [[nodiscard]] bool append(
        std::span<const PackedNote> notes,
        std::string* error = nullptr
    );

    // Flushes the last chunk and atomically renames the private temporary
    // file. A writer that is destroyed before finish() leaves no destination.
    [[nodiscard]] bool finish(std::string* error = nullptr);

    [[nodiscard]] std::uint64_t notes_received() const noexcept;
    [[nodiscard]] std::uint64_t chunks_written() const noexcept;

private:
    class Impl;
    explicit PackedChartStreamWriter(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;
};

}  // namespace pulseforge
