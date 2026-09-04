#include "pulseforge/autochart.hpp"

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] std::vector<std::string> split_csv(const std::string_view text) {
    std::vector<std::string> result;
    std::size_t begin = 0U;
    while (begin <= text.size()) {
        const auto comma = text.find(',', begin);
        const auto end = comma == std::string_view::npos ? text.size() : comma;
        if (end > begin) {
            result.emplace_back(text.substr(begin, end - begin));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1U;
    }
    return result;
}

[[nodiscard]] std::optional<unsigned int> parse_unsigned(const std::string_view text) {
    unsigned int value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<double> parse_double(const std::string& text) {
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == nullptr || end != text.c_str() + text.size()) {
        return std::nullopt;
    }
    return value;
}

void usage() {
    std::cout
        << "PulseForge AutoChart\n\n"
        << "Usage:\n"
        << "  pulseforge-autochart <audio-or-video> [options]\n"
        << "  pulseforge-autochart --ml-health [ML path/device options]\n"
        << "  pulseforge-autochart --ml-health-deep [ML path/device options]\n\n"
        << "Input is content-probed by FFmpeg, so MP3/OGG/WAV/FLAC/AAC/M4A and\n"
        << "MP4/MKV/MOV/AVI/WMV/WebM (plus other FFmpeg-supported media) work\n"
        << "without manual conversion. The first audio stream is charted.\n\n"
        << "Options:\n"
        << "  --mode fast|accurate|maximum\n"
        << "  --video-analysis off|auto|on\n"
        << "  --ml off|auto|on               optional Demucs/BeatThis/BasicPitch backend\n"
        << "  --ml-device auto|cpu|cuda\n"
        << "  --ml-python PATH               explicit Python executable\n"
        << "  --ml-backend PATH              explicit ml_backend.py\n"
        << "  --ml-cache-root PATH           persistent stem/model/result cache\n"
        << "  --no-ml-cache\n"
        << "  --no-analysis-cache             disable native FFT/video feature cache\n"
        << "  --analysis-cache-root PATH      persistent native feature cache\n"
        << "  --no-source-separation\n"
        << "  --no-neural-beats\n"
        << "  --no-drum-transcription\n"
        << "  --no-pitch-transcription\n"
        << "  --no-vocal-refinement          disable phoneme/syllable timing evidence\n"
        << "  --ml-health                    import/runtime preflight only\n"
        << "  --ml-health-deep               execute bounded model inference fixtures\n"
        << "  --no-structure                  disable phrase/section-aware density\n"
        << "  --greedy-lanes                  disable beam-search lane optimizer\n"
        << "  --no-review                     do not emit uncertainty/review artifacts\n"
        << "  --no-html-review                skip interactive HTML review page\n"
        << "  --review-limit N                max retained uncertain notes (default 20000)\n"
        << "  --keys N                       1..18 (default 4)\n"
        << "  --difficulty NAME             easy|normal|hard|expert|insane\n"
        << "  --difficulties CSV            e.g. easy,normal,hard,expert\n"
        << "  --title TEXT\n"
        << "  --artist TEXT\n"
        << "  --charter TEXT\n"
        << "  --id MOD_ID\n"
        << "  --scroll-speed N\n"
        << "  --mods-root PATH              default: mods\n"
        << "  --output PATH                 exact output mod directory\n"
        << "  --ffmpeg PATH                 explicit ffmpeg executable\n"
        << "  --variable-tempo              conservative tempo segmentation\n"
        << "  --overwrite\n"
        << "  --no-add-to-mods              write beside source unless --output\n"
        << "  --help\n";
}

[[nodiscard]] int run(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return EXIT_FAILURE;
    }
    const std::string first = argv[1];
    if (first == "--help" || first == "-h") {
        usage();
        return EXIT_SUCCESS;
    }

    if (first == "--ml-health" || first == "--ml-health-deep") {
        pulseforge::AutoChartOptions options;
        const bool deep = first == "--ml-health-deep";
        for (int index = 2; index < argc; ++index) {
            const std::string argument = argv[index];
            const auto require_value = [&](const char* name) -> std::string {
                if (index + 1 >= argc) {
                    throw std::runtime_error(std::string(name) + " requires a value");
                }
                ++index;
                return argv[index];
            };
            if (argument == "--ml-device") {
                options.ml_device = require_value("--ml-device");
            } else if (argument == "--ml-python") {
                options.ml_python_path = require_value("--ml-python");
            } else if (argument == "--ml-backend") {
                options.ml_backend_script = require_value("--ml-backend");
            } else if (argument == "--ml-cache-root") {
                options.ml_cache_root = require_value("--ml-cache-root");
            } else if (argument == "--ffmpeg") {
                options.ffmpeg_path = require_value("--ffmpeg");
            } else if (argument == "--help" || argument == "-h") {
                usage();
                return EXIT_SUCCESS;
            } else {
                throw std::runtime_error("unknown ML health option: " + argument);
            }
        }
        const auto report = pulseforge::inspect_autochart_ml_backend(options, deep);
        std::cout << "AutoChart ML health: " << (report.ok ? "OK" : "INCOMPLETE")
                  << (deep ? " (deep)" : " (preflight)") << '\n'
                  << "  Python: " << (report.python_version.empty() ? "unknown" : report.python_version) << '\n'
                  << "  Device: " << (report.device.empty() ? "unknown" : report.device) << '\n';
        for (const auto& stage : report.stages) {
            std::cout << "  [" << (stage.available ? "OK" : "--") << "] "
                      << stage.name
                      << (stage.tested ? " tested" : " available")
                      << " (" << stage.latency_ms << " ms)";
            if (!stage.detail.empty()) {
                std::cout << " - " << stage.detail;
            }
            std::cout << '\n';
        }
        if (!report.ok && !report.error.empty()) {
            std::cerr << "  Error: " << report.error << '\n';
        }
        return report.ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    const std::filesystem::path media = first;
    pulseforge::AutoChartOptions options;
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto require_value = [&](const char* name) -> std::string {
            if (index + 1 >= argc) {
                throw std::runtime_error(std::string(name) + " requires a value");
            }
            ++index;
            return argv[index];
        };
        if (argument == "--help" || argument == "-h") {
            usage();
            return EXIT_SUCCESS;
        }
        if (argument == "--mode") {
            const auto value = require_value("--mode");
            if (value == "fast") {
                options.mode = pulseforge::AutoChartMode::fast;
            } else if (value == "maximum") {
                options.mode = pulseforge::AutoChartMode::maximum;
            } else if (value == "accurate") {
                options.mode = pulseforge::AutoChartMode::accurate;
            } else {
                throw std::runtime_error("--mode must be fast, accurate or maximum");
            }
        } else if (argument == "--video-analysis") {
            const auto value = require_value("--video-analysis");
            if (value == "off") {
                options.video_mode = pulseforge::AutoChartVideoMode::off;
            } else if (value == "on") {
                options.video_mode = pulseforge::AutoChartVideoMode::on;
            } else if (value == "auto") {
                options.video_mode = pulseforge::AutoChartVideoMode::automatic;
            } else {
                throw std::runtime_error("--video-analysis must be off, auto or on");
            }
        } else if (argument == "--ml") {
            const auto value = require_value("--ml");
            if (value == "off") {
                options.ml_mode = pulseforge::AutoChartMlMode::off;
            } else if (value == "on") {
                options.ml_mode = pulseforge::AutoChartMlMode::on;
            } else if (value == "auto") {
                options.ml_mode = pulseforge::AutoChartMlMode::automatic;
            } else {
                throw std::runtime_error("--ml must be off, auto or on");
            }
        } else if (argument == "--ml-device") {
            options.ml_device = require_value("--ml-device");
        } else if (argument == "--ml-python") {
            options.ml_python_path = require_value("--ml-python");
        } else if (argument == "--ml-backend") {
            options.ml_backend_script = require_value("--ml-backend");
        } else if (argument == "--ml-cache-root") {
            options.ml_cache_root = require_value("--ml-cache-root");
        } else if (argument == "--no-ml-cache") {
            options.ml_cache = false;
        } else if (argument == "--no-analysis-cache") {
            options.analysis_cache = false;
        } else if (argument == "--analysis-cache-root") {
            options.analysis_cache_root = require_value("--analysis-cache-root");
        } else if (argument == "--no-source-separation") {
            options.ml_source_separation = false;
        } else if (argument == "--no-neural-beats") {
            options.ml_beat_tracking = false;
        } else if (argument == "--no-drum-transcription") {
            options.ml_drum_transcription = false;
        } else if (argument == "--no-pitch-transcription") {
            options.ml_pitch_transcription = false;
        } else if (argument == "--no-vocal-refinement") {
            options.ml_vocal_refinement = false;
        } else if (argument == "--no-structure") {
            options.structural_charting = false;
        } else if (argument == "--greedy-lanes") {
            options.beam_lane_optimizer = false;
        } else if (argument == "--no-review") {
            options.write_review_artifacts = false;
            options.write_html_review = false;
        } else if (argument == "--no-html-review") {
            options.write_html_review = false;
        } else if (argument == "--review-limit") {
            const auto value = parse_unsigned(require_value("--review-limit"));
            if (!value.has_value() || *value > 200'000U) {
                throw std::runtime_error("--review-limit must be an integer from 0 to 200000");
            }
            options.maximum_review_notes = static_cast<std::uint32_t>(*value);
        } else if (argument == "--keys") {
            const auto value = parse_unsigned(require_value("--keys"));
            if (!value.has_value() || *value == 0U || *value > 18U) {
                throw std::runtime_error("--keys must be an integer from 1 to 18");
            }
            options.key_count = static_cast<std::uint16_t>(*value);
        } else if (argument == "--difficulty") {
            options.difficulties = {require_value("--difficulty")};
        } else if (argument == "--difficulties") {
            options.difficulties = split_csv(require_value("--difficulties"));
        } else if (argument == "--title") {
            options.title = require_value("--title");
        } else if (argument == "--artist") {
            options.artist = require_value("--artist");
        } else if (argument == "--charter") {
            options.charter = require_value("--charter");
        } else if (argument == "--id") {
            options.mod_id = require_value("--id");
        } else if (argument == "--scroll-speed") {
            const auto value = require_value("--scroll-speed");
            const auto parsed = parse_double(value);
            if (!parsed.has_value()) {
                throw std::runtime_error("--scroll-speed expects a number");
            }
            options.scroll_speed = *parsed;
        } else if (argument == "--mods-root") {
            options.mods_root = require_value("--mods-root");
        } else if (argument == "--output") {
            options.output_root = require_value("--output");
        } else if (argument == "--ffmpeg") {
            options.ffmpeg_path = require_value("--ffmpeg");
        } else if (argument == "--variable-tempo") {
            options.variable_tempo = true;
        } else if (argument == "--overwrite") {
            options.overwrite = true;
        } else if (argument == "--no-add-to-mods") {
            options.add_to_mods = false;
        } else {
            throw std::runtime_error("unknown AutoChart option: " + argument);
        }
    }

    const auto result = pulseforge::generate_autochart_mod(media, options);
    if (!result.ok) {
        std::cerr << "AutoChart failed: " << result.error << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "\nAutoChart complete\n"
              << "  Mod: " << result.mod_root.string() << '\n'
              << "  Audio: " << result.audio_path.string() << '\n'
              << "  BPM: " << result.detected_bpm << '\n'
              << "  Beat confidence: " << result.beat_confidence << '\n'
              << "  Candidates: " << result.candidate_count << '\n'
              << "  Video assist: " << (result.video_assist_used ? "yes" : "no") << '\n'
              << "  Native analysis cache: " << (result.analysis_cache_hit ? "hit" : "miss") << '\n'
              << "  ML: " << (result.ml_used ? "yes" : "no")
              << (result.ml_cache_hit ? " (cache hit)" : "") << '\n'
              << "  ML device: " << (result.ml_device.empty() ? "n/a" : result.ml_device) << '\n'
              << "  Source separation: " << (result.source_separation_used ? "yes" : "no") << '\n'
              << "  Neural beats: " << (result.neural_beat_used ? "yes" : "no") << '\n'
              << "  Drum transcription: " << (result.drum_transcription_used ? "yes" : "no") << '\n'
              << "  Pitch transcription: " << (result.pitch_transcription_used ? "yes" : "no") << '\n'
              << "  Vocal refinement: " << (result.vocal_refinement_used ? "yes" : "no")
              << " (" << result.syllable_event_count << " syllables, "
              << result.phoneme_event_count << " phonemes)\n"
              << "  Structural analysis: " << (result.structural_analysis_used ? "yes" : "no")
              << " (" << result.structural_section_count << " sections, "
              << result.phrase_count << " phrases, confidence "
              << result.structure_confidence << ")\n"
              << "  Lane optimizer: " << (result.beam_lane_optimizer_used ? "beam" : "greedy")
              << (result.beam_lane_optimizer_used
                    ? " (width " + std::to_string(result.lane_beam_width) + ")"
                    : std::string{})
              << '\n'
              << "  Candidates DSP/stem/fused: " << result.dsp_candidate_count << '/'
              << result.stem_candidate_count << '/' << result.candidate_count << '\n'
              << "  ML drum events: " << result.drum_event_count << '\n'
              << "  ML pitch events: " << result.pitch_event_count << '\n'
              << "  Overall quality: " << result.overall_quality_score << "/100\n"
              << "  Review queue: " << result.review_note_count
              << " (low confidence " << result.low_confidence_note_count << ")\n";
    for (const auto& difficulty : result.difficulties) {
        std::cout << "  " << difficulty.difficulty << ": "
                  << difficulty.note_count << " notes, avg "
                  << difficulty.average_nps << " NPS, peak "
                  << difficulty.peak_nps << " NPS, quality "
                  << difficulty.quality_score << "/100\n";
    }
    std::cout << "  Report: " << result.report_path.string() << '\n';
    if (!result.review_html_path.empty()) {
        std::cout << "  Review: " << result.review_html_path.string() << '\n';
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& exception) {
        std::cerr << "AutoChart argument error: " << exception.what() << '\n';
        usage();
        return EXIT_FAILURE;
    }
}
