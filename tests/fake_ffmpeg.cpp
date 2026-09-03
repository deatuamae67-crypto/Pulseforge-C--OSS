#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

[[nodiscard]] bool parse_u32(
    const std::string_view value,
    std::uint32_t& output
) {
    const auto result = std::from_chars(
        value.data(),
        value.data() + value.size(),
        output
    );
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "fake ffmpeg requires an output path\n";
        return 2;
    }
    bool fail_after_partial = false;
    bool dump_input = false;
    std::uint32_t slow_read_ms = 0U;
    std::uint32_t initial_delay_ms = 0U;
    std::uint32_t fail_after_bytes = 0U;
    for (int index = 1; index + 1 < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--fail-after-partial") {
            fail_after_partial = true;
        } else if (option == "--dump-input") {
            dump_input = true;
        } else if (option.starts_with("--slow-read-ms=")) {
            if (!parse_u32(option.substr(15U), slow_read_ms)) {
                return 6;
            }
        } else if (option.starts_with("--initial-delay-ms=")) {
            if (!parse_u32(option.substr(19U), initial_delay_ms)) {
                return 7;
            }
        } else if (option.starts_with("--fail-after-bytes=")) {
            if (!parse_u32(option.substr(19U), fail_after_bytes)) {
                return 8;
            }
        }
    }
#if defined(_WIN32)
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) {
        std::cerr << "cannot switch stdin to binary mode\n";
        return 3;
    }
#endif
    if (initial_delay_ms != 0U) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(initial_delay_ms)
        );
    }
    std::uint64_t bytes = 0U;
    std::vector<char> payload;
    char buffer[32U * 1024U]{};
    while (std::cin) {
        std::cin.read(buffer, static_cast<std::streamsize>(sizeof(buffer)));
        const auto received = std::cin.gcount();
        bytes += static_cast<std::uint64_t>(received);
        if (dump_input && received > 0) {
            payload.insert(payload.end(), buffer, buffer + received);
        }
        if (fail_after_bytes != 0U && bytes >= fail_after_bytes) {
            std::cerr << "intentional stdin failure after " << bytes
                      << " bytes\n";
            return 19;
        }
        if (slow_read_ms != 0U && received > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(slow_read_ms)
            );
        }
    }
    if (bytes == 0U) {
        std::cerr << "no raw frames received\n";
        return 4;
    }
    std::ofstream output(
        std::filesystem::path(argv[argc - 1]),
        std::ios::binary
    );
    if (dump_input) {
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    } else {
        output << "fake-mp4:" << bytes;
    }
    output.flush();
    if (!output) {
        return 5;
    }
    if (fail_after_partial) {
        std::cerr << "intentional encoder failure after partial output\n";
        return 17;
    }
    return 0;
}
