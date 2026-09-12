#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace pulseforge::detail {

// Returns a compact platform/runtime snapshot suitable for performance reports.
// Android includes ART/GC counters and Java heap figures; other platforms return
// an empty string so the gameplay capture remains completely portable.
[[nodiscard]] std::string android_runtime_diagnostics();

// Publishes an app-private file into the user-visible Downloads/PulseForge
// collection on Android. Returns the published content URI/path on success.
// Other platforms simply return the original filesystem path.
[[nodiscard]] std::string publish_file_to_downloads(
    const std::filesystem::path& source,
    std::string_view display_name,
    std::string_view mime_type
);

}  // namespace pulseforge::detail
