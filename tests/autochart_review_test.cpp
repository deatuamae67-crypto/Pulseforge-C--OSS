#include "pulseforge/autochart_review.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create fixture file: " + path.string());
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) {
        throw std::runtime_error("cannot write fixture file: " + path.string());
    }
}

[[nodiscard]] std::filesystem::path unique_root() {
    const auto stamp = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    return std::filesystem::temp_directory_path()
        / ("pulseforge-autochart-review-" + std::to_string(stamp));
}

void test_direct_seek_and_schema_binding() {
    const auto root = unique_root();
    const auto review_dir = root / "review";
    const std::string record =
        R"({"schema":"pulseforge-autochart-note-review-v1","noteIndex":0,"timeMs":1000.0,"durationMs":0.0,"lane":2,"role":"primary","source":"dsp+beat","section":"verse","confidence":0.72,"supportScore":0.81,"ambiguity":0.28,"reviewPriority":0.61,"reviewBand":"medium","evidenceMask":3,"evidenceCount":2,"evidence":["dsp","beat"],"why":["strong onset"],"reviewConcerns":["lane ambiguity"],"timing":{"beatAlignment":0.95,"downbeatAlignment":0.25,"phraseAlignment":0.10,"quantizationDeltaMs":1.5},"selection":{"structuralPriority":0.75,"minimumConfidence":0.50,"priorityThreshold":0.45,"protectedAccent":true},"flags":{"polyphonic":false,"sustain":false,"chordSecondary":false}})";
    write_text(review_dir / "expert.jsonl", record + "\n");
    write_text(root / "chart.json", "{}\n");
    write_text(
        review_dir / "queue.json",
        std::string{R"({"schema":"pulseforge-autochart-review-queue-v1","highPriorityThreshold":0.65,"mediumPriorityThreshold":0.40,"entries":[{"difficulty":"expert","noteIndex":0,"timeMs":1000.0,"lane":2,"reviewPriority":0.61,"confidence":0.72,"ambiguity":0.28,"reviewFile":"review/expert.jsonl","byteOffset":0,"byteLength":)"}
            + std::to_string(record.size()) + "}]}"
    );
    write_text(
        review_dir / "index.json",
        R"({"schema":"pulseforge-autochart-review-index-v1","recordSchema":"pulseforge-autochart-note-review-v1","recordFormat":"jsonl","highPriorityThreshold":0.65,"mediumPriorityThreshold":0.40,"queue":"review/queue.json","difficulties":[{"name":"expert","chart":"chart.json","reviewFile":"review/expert.jsonl","noteCount":1,"reviewRecords":1,"highPriority":0,"mediumPriority":1,"meanReviewPriority":0.61,"maximumReviewPriority":0.61}]})"
    );

    std::string error;
    auto reader = pulseforge::AutoChartReviewReader::open(
        review_dir / "index.json",
        &error
    );
    require(reader.has_value(), "reader open failed: " + error);
    const auto queue = reader->load_queue("expert", 10U, &error);
    require(error.empty(), "queue load failed: " + error);
    require(queue.size() == 1U, "expected one review queue entry");
    const auto detail = reader->read_record(queue.front(), &error);
    require(detail.has_value(), "direct JSONL seek failed: " + error);
    require(detail->note_index == 0U, "wrong note index");
    require(detail->lane == 2U, "wrong lane");
    require(detail->review_concerns.size() == 1U, "review concerns were not decoded");
    require(detail->protected_accent, "selection flags were not decoded");

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
}

void test_path_traversal_is_rejected() {
    const auto root = unique_root();
    const auto review_dir = root / "review";
    write_text(
        review_dir / "index.json",
        R"({"schema":"pulseforge-autochart-review-index-v1","recordSchema":"pulseforge-autochart-note-review-v1","recordFormat":"jsonl","highPriorityThreshold":0.65,"mediumPriorityThreshold":0.40,"queue":"../outside.json","difficulties":[]})"
    );
    std::string error;
    const auto reader = pulseforge::AutoChartReviewReader::open(
        review_dir / "index.json",
        &error
    );
    require(!reader.has_value(), "path traversal review index was accepted");
    require(!error.empty(), "path traversal rejection produced no diagnostic");

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
}

}  // namespace

int main() {
    try {
        test_direct_seek_and_schema_binding();
        test_path_traversal_is_rejected();
        std::cout << "AutoChart review reader tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "AutoChart review reader test failed: " << exception.what() << '\n';
        return 1;
    }
}
