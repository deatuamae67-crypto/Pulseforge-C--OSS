#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

int main(const int argc, char** argv) {
    std::filesystem::path output;
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string_view(argv[index]) == "--out") {
            output = std::filesystem::path(argv[index + 1]);
            break;
        }
    }
    if (output.empty()) {
        std::cerr << "fake SNIFF: missing --out\n";
        return 2;
    }
    std::ofstream chart(output, std::ios::binary | std::ios::trunc);
    chart << R"({"song":{"song":"bridge-test","bpm":120,"speed":1,"notes":[]}})";
    chart.flush();
    if (!chart) {
        return 3;
    }
    char* marker_text = nullptr;
    std::size_t marker_size = 0U;
#if defined(_WIN32)
    if (_dupenv_s(
            &marker_text,
            &marker_size,
            "PULSEFORGE_FAKE_SNIFF_MARKER"
        ) == 0 && marker_text != nullptr) {
        std::ofstream marker(
            std::filesystem::path(marker_text),
            std::ios::binary | std::ios::trunc
        );
        marker << "invoked";
    }
    std::free(marker_text);
#else
    if (const char* marker = std::getenv("PULSEFORGE_FAKE_SNIFF_MARKER")) {
        std::ofstream marker_output(
            std::filesystem::path(marker),
            std::ios::binary | std::ios::trunc
        );
        marker_output << "invoked";
    }
#endif
    return 0;
}
