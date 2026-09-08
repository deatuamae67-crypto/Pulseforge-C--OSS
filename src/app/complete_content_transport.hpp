#pragma once

#include "complete_content_runtime.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#elif defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif

namespace pulseforge::detail {

struct CompleteTransferProgress {
    std::atomic<std::uint64_t> files_done{0U};
    std::atomic<std::uint64_t> total_files{0U};
    std::atomic<std::uint64_t> mods_done{0U};
    std::atomic<std::uint64_t> total_mods{0U};
    std::atomic<std::uint64_t> bytes_received{0U};
    std::atomic<bool> finished{false};
    std::atomic<bool> success{false};
    std::atomic<bool> cancelled{false};
    std::mutex detail_mutex;
    std::string detail;
    std::string error;
};

struct CompleteTransferResult {
    bool success{};
    bool cancelled{};
    std::uint64_t installed_mods{};
    std::uint64_t downloaded_files{};
    std::string error;
};

namespace complete_transport_detail {

constexpr std::size_t maximum_confirmation_html = 4U * 1024U * 1024U;
constexpr std::string_view browser_user_agent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/139.0.0.0 Safari/537.36 PulseForge/1.0";

[[nodiscard]] inline std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

[[nodiscard]] inline std::string trim_ascii(std::string value) {
    const auto whitespace = [](const unsigned char c) {
        return std::isspace(c) != 0;
    };
    const auto first = std::find_if_not(value.begin(), value.end(), whitespace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), whitespace).base();
    if (first >= last) {
        return {};
    }
    return {first, last};
}

[[nodiscard]] inline std::string replace_all(
    std::string value,
    const std::string_view from,
    const std::string_view to
) {
    if (from.empty()) {
        return value;
    }
    std::size_t position = 0U;
    while ((position = value.find(from, position)) != std::string::npos) {
        value.replace(position, from.size(), to);
        position += to.size();
    }
    return value;
}

[[nodiscard]] inline std::string html_unescape(std::string value) {
    value = replace_all(std::move(value), "&amp;", "&");
    value = replace_all(std::move(value), "&quot;", "\"");
    value = replace_all(std::move(value), "&#39;", "'");
    value = replace_all(std::move(value), "&lt;", "<");
    value = replace_all(std::move(value), "&gt;", ">");
    return value;
}

[[nodiscard]] inline std::string json_url_unescape(std::string value) {
    value = replace_all(std::move(value), "\\u003d", "=");
    value = replace_all(std::move(value), "\\u0026", "&");
    value = replace_all(std::move(value), "\\u002f", "/");
    value = replace_all(std::move(value), "\\/", "/");
    return value;
}

[[nodiscard]] inline std::optional<std::string> attribute_value(
    const std::string_view tag,
    const std::string_view attribute
) {
    const auto lower = lowercase_ascii(std::string(tag));
    const auto needle = lowercase_ascii(std::string(attribute)) + "=";
    auto position = lower.find(needle);
    if (position == std::string::npos) {
        return std::nullopt;
    }
    position += needle.size();
    if (position >= tag.size()) {
        return std::nullopt;
    }
    const char quote = tag[position];
    if (quote == '\'' || quote == '"') {
        const auto end = tag.find(quote, position + 1U);
        if (end == std::string_view::npos) {
            return std::nullopt;
        }
        return html_unescape(std::string(tag.substr(position + 1U, end - position - 1U)));
    }
    const auto end = tag.find_first_of(" \t\r\n>", position);
    return html_unescape(std::string(tag.substr(position, end - position)));
}

[[nodiscard]] inline std::string url_encode_component(const std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(value.size() + value.size() / 4U);
    for (const unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            output.push_back(static_cast<char>(c));
        } else {
            output.push_back('%');
            output.push_back(hex[(c >> 4U) & 0x0FU]);
            output.push_back(hex[c & 0x0FU]);
        }
    }
    return output;
}

[[nodiscard]] inline std::optional<std::string> drive_confirmation_url(
    const std::string_view html
) {
    // gdown-compatible confirmation route 1: an explicit /uc download link.
    constexpr std::string_view href_prefix = "href=\"/uc?export=download";
    auto position = html.find(href_prefix);
    if (position != std::string_view::npos) {
        position += std::string_view("href=\"").size();
        const auto end = html.find('"', position);
        if (end != std::string_view::npos) {
            return "https://docs.google.com"
                + html_unescape(std::string(html.substr(position, end - position)));
        }
    }

    // Route 2: current Google Drive warning pages use a download-form with
    // hidden confirmation inputs. Reconstruct the GET exactly from those fields.
    const auto form_id = html.find("id=\"download-form\"");
    if (form_id != std::string_view::npos) {
        const auto form_begin = html.rfind("<form", form_id);
        const auto form_end = html.find('>', form_id);
        if (form_begin != std::string_view::npos && form_end != std::string_view::npos) {
            const auto form_tag = html.substr(form_begin, form_end - form_begin + 1U);
            auto action = attribute_value(form_tag, "action");
            if (action.has_value() && action->starts_with("https://")) {
                std::string url = *action;
                bool first = url.find('?') == std::string::npos;
                auto cursor = form_end + 1U;
                const auto close = html.find("</form>", cursor);
                const auto limit = close == std::string_view::npos ? html.size() : close;
                while (cursor < limit) {
                    const auto input_begin = html.find("<input", cursor);
                    if (input_begin == std::string_view::npos || input_begin >= limit) {
                        break;
                    }
                    const auto input_end = html.find('>', input_begin);
                    if (input_end == std::string_view::npos || input_end > limit) {
                        break;
                    }
                    const auto input_tag = html.substr(
                        input_begin,
                        input_end - input_begin + 1U
                    );
                    const auto name = attribute_value(input_tag, "name");
                    const auto value = attribute_value(input_tag, "value");
                    if (name.has_value() && value.has_value()) {
                        url += first ? '?' : '&';
                        first = false;
                        url += url_encode_component(*name);
                        url += '=';
                        url += url_encode_component(*value);
                    }
                    cursor = input_end + 1U;
                }
                return url;
            }
        }
    }

    // Route 3: Google sometimes embeds a JSON-escaped direct download URL.
    constexpr std::string_view json_prefix = "\"downloadUrl\":\"";
    position = html.find(json_prefix);
    if (position != std::string_view::npos) {
        position += json_prefix.size();
        const auto end = html.find('"', position);
        if (end != std::string_view::npos) {
            auto value = json_url_unescape(std::string(html.substr(position, end - position)));
            if (value.starts_with("https://")) {
                return value;
            }
        }
    }
    return std::nullopt;
}

inline void set_progress_detail(
    CompleteTransferProgress* const progress,
    std::string value
) {
    if (progress == nullptr) {
        return;
    }
    std::scoped_lock lock(progress->detail_mutex);
    progress->detail = std::move(value);
}

[[nodiscard]] inline std::string path_utf8_transport(
    const std::filesystem::path& path
) {
    const auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}

inline bool promote_part_file(
    const std::filesystem::path& part,
    const std::filesystem::path& destination,
    std::string& error
) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(destination.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "cannot create Complete download directory: " + filesystem_error.message();
        return false;
    }
    std::filesystem::remove(destination, filesystem_error);
    filesystem_error.clear();
    std::filesystem::rename(part, destination, filesystem_error);
    if (filesystem_error) {
        error = "cannot promote Complete download: " + filesystem_error.message();
        return false;
    }
    return true;
}

#if !defined(_WIN32) && !defined(__ANDROID__)

// Linux and macOS ship libcurl as a platform library. Resolve it at runtime so
// PulseForge does not require a curl executable, Python, gdown, or development
// headers on the user's machine. TLS verification remains libcurl's default.
class DynamicCurl final {
public:
    using Easy = void;
    using GlobalInit = int (*)(long);
    using EasyInit = Easy* (*)();
    using EasySetopt = int (*)(Easy*, int, ...);
    using EasyPerform = int (*)(Easy*);
    using EasyCleanup = void (*)(Easy*);
    using EasyStrerror = const char* (*)(int);

    DynamicCurl() {
#if defined(__APPLE__)
        constexpr const char* candidates[]{
            "/usr/lib/libcurl.4.dylib",
            "libcurl.4.dylib",
            "libcurl.dylib",
        };
#else
        constexpr const char* candidates[]{"libcurl.so.4", "libcurl.so"};
#endif
        for (const char* candidate : candidates) {
            module_ = SDL_LoadObject(candidate);
            if (module_ != nullptr) {
                break;
            }
        }
        if (module_ == nullptr) {
            error_ = "native libcurl is unavailable";
            return;
        }
        global_init = symbol<GlobalInit>("curl_global_init");
        easy_init = symbol<EasyInit>("curl_easy_init");
        easy_setopt = symbol<EasySetopt>("curl_easy_setopt");
        easy_perform = symbol<EasyPerform>("curl_easy_perform");
        easy_cleanup = symbol<EasyCleanup>("curl_easy_cleanup");
        easy_strerror = symbol<EasyStrerror>("curl_easy_strerror");
        if (global_init == nullptr || easy_init == nullptr || easy_setopt == nullptr
            || easy_perform == nullptr || easy_cleanup == nullptr
            || easy_strerror == nullptr) {
            error_ = "native libcurl is missing required symbols";
            SDL_UnloadObject(module_);
            module_ = nullptr;
            return;
        }
        if (global_init(3L) != 0) {
            error_ = "native libcurl global initialization failed";
            SDL_UnloadObject(module_);
            module_ = nullptr;
        }
    }

    ~DynamicCurl() {
        if (module_ != nullptr) {
            SDL_UnloadObject(module_);
        }
    }

    DynamicCurl(const DynamicCurl&) = delete;
    DynamicCurl& operator=(const DynamicCurl&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept {
        return module_ != nullptr;
    }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    GlobalInit global_init{};
    EasyInit easy_init{};
    EasySetopt easy_setopt{};
    EasyPerform easy_perform{};
    EasyCleanup easy_cleanup{};
    EasyStrerror easy_strerror{};

private:
    template <typename Type>
    [[nodiscard]] Type symbol(const char* name) {
        return reinterpret_cast<Type>(SDL_LoadFunction(module_, name));
    }

    void* module_{};
    std::string error_;
};

inline constexpr int curlopt_writedata = 10001;
inline constexpr int curlopt_url = 10002;
inline constexpr int curlopt_range = 10007;
inline constexpr int curlopt_writefunction = 20011;
inline constexpr int curlopt_useragent = 10018;
inline constexpr int curlopt_low_speed_limit = 19;
inline constexpr int curlopt_low_speed_time = 20;
inline constexpr int curlopt_cookiefile = 10031;
inline constexpr int curlopt_followlocation = 52;
inline constexpr int curlopt_maxredirs = 68;
inline constexpr int curlopt_connecttimeout = 78;
inline constexpr int curlopt_headerdata = 10029;
inline constexpr int curlopt_headerfunction = 20079;
inline constexpr int curlopt_nosignal = 99;
inline constexpr int curlopt_accept_encoding = 10102;
inline constexpr int curle_ok = 0;
inline constexpr int curle_write_error = 23;

struct CurlHeaderState {
    long status{};
    bool file_response{};
};

inline std::size_t curl_header_callback(
    char* data,
    const std::size_t size,
    const std::size_t count,
    void* opaque
) noexcept {
    const auto total = size * count;
    if (opaque == nullptr || data == nullptr) {
        return total;
    }
    auto& state = *static_cast<CurlHeaderState*>(opaque);
    try {
        const std::string line(data, total);
        const auto lower = lowercase_ascii(line);
        if (lower.starts_with("http/")) {
            const auto space = lower.find(' ');
            if (space != std::string::npos) {
                state.status = std::strtol(lower.c_str() + space + 1U, nullptr, 10);
            }
            state.file_response = false;
        } else if (lower.starts_with("content-disposition:")) {
            state.file_response = true;
        }
    } catch (...) {
        return 0U;
    }
    return total;
}

struct CurlProbeState {
    CurlHeaderState* header{};
    std::atomic<bool>* cancel{};
    std::string body;
    bool overflow{};
};

inline std::size_t curl_probe_callback(
    char* data,
    const std::size_t size,
    const std::size_t count,
    void* opaque
) noexcept {
    const auto total = size * count;
    if (opaque == nullptr || data == nullptr) {
        return 0U;
    }
    auto& state = *static_cast<CurlProbeState*>(opaque);
    if ((state.cancel != nullptr && state.cancel->load(std::memory_order_relaxed))
        || (state.header != nullptr && state.header->file_response)) {
        return 0U;
    }
    if (state.body.size() > maximum_confirmation_html - std::min(
            maximum_confirmation_html,
            total
        )) {
        state.overflow = true;
        return 0U;
    }
    try {
        state.body.append(data, total);
    } catch (...) {
        state.overflow = true;
        return 0U;
    }
    return total;
}

struct CurlFileState {
    std::filesystem::path part;
    CurlHeaderState* header{};
    std::uint64_t resume_from{};
    std::atomic<bool>* cancel{};
    CompleteTransferProgress* progress{};
    std::ofstream output;
    bool opened{};
    bool failed{};
};

inline std::size_t curl_file_callback(
    char* data,
    const std::size_t size,
    const std::size_t count,
    void* opaque
) noexcept {
    const auto total = size * count;
    if (opaque == nullptr || data == nullptr) {
        return 0U;
    }
    auto& state = *static_cast<CurlFileState*>(opaque);
    if (state.cancel != nullptr && state.cancel->load(std::memory_order_relaxed)) {
        return 0U;
    }
    try {
        if (!state.opened) {
            const bool append = state.resume_from != 0U && state.header != nullptr
                && state.header->status == 206L;
            state.output.open(
                state.part,
                std::ios::binary | (append ? std::ios::app : std::ios::trunc)
            );
            state.opened = true;
            if (!state.output.is_open()) {
                state.failed = true;
                return 0U;
            }
        }
        state.output.write(data, static_cast<std::streamsize>(total));
        if (!state.output) {
            state.failed = true;
            return 0U;
        }
        if (state.progress != nullptr) {
            state.progress->bytes_received.fetch_add(
                static_cast<std::uint64_t>(total),
                std::memory_order_relaxed
            );
        }
    } catch (...) {
        state.failed = true;
        return 0U;
    }
    return total;
}

inline bool curl_setopt_common(
    DynamicCurl& library,
    DynamicCurl::Easy* easy,
    std::string& error
) {
    const auto set_long = [&](const int option, const long value) {
        return library.easy_setopt(easy, option, value) == curle_ok;
    };
    if (!set_long(curlopt_followlocation, 1L)
        || !set_long(curlopt_maxredirs, 10L)
        || !set_long(curlopt_connecttimeout, 30L)
        || !set_long(curlopt_low_speed_limit, 1L)
        || !set_long(curlopt_low_speed_time, 60L)
        || !set_long(curlopt_nosignal, 1L)
        || library.easy_setopt(easy, curlopt_useragent, browser_user_agent.data()) != curle_ok
        || library.easy_setopt(easy, curlopt_cookiefile, "") != curle_ok
        || library.easy_setopt(easy, curlopt_accept_encoding, "") != curle_ok) {
        error = "could not configure native HTTPS transport";
        return false;
    }
    return true;
}

inline bool resolve_drive_url_with_curl(
    DynamicCurl& library,
    DynamicCurl::Easy* easy,
    std::string url,
    std::atomic<bool>* cancel,
    std::string& resolved,
    std::string& error
) {
    for (int round = 0; round < 6; ++round) {
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            error = "Complete download cancelled";
            return false;
        }
        CurlHeaderState header;
        CurlProbeState probe{.header = &header, .cancel = cancel};
        library.easy_setopt(easy, curlopt_url, url.c_str());
        library.easy_setopt(easy, curlopt_range, static_cast<const char*>(nullptr));
        library.easy_setopt(easy, curlopt_headerdata, &header);
        library.easy_setopt(easy, curlopt_headerfunction, &curl_header_callback);
        library.easy_setopt(easy, curlopt_writedata, &probe);
        library.easy_setopt(easy, curlopt_writefunction, &curl_probe_callback);
        const int code = library.easy_perform(easy);
        if (header.file_response && (code == curle_ok || code == curle_write_error)) {
            resolved = std::move(url);
            return true;
        }
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            error = "Complete download cancelled";
            return false;
        }
        if (probe.overflow) {
            error = "Google Drive confirmation page exceeded the safety limit";
            return false;
        }
        if (code != curle_ok) {
            error = std::string("Google Drive probe failed: ") + library.easy_strerror(code);
            return false;
        }
        if (header.status >= 400L) {
            error = "Google Drive returned HTTP " + std::to_string(header.status);
            return false;
        }
        const auto confirmation = drive_confirmation_url(probe.body);
        if (!confirmation.has_value()) {
            error = "Google Drive did not provide a downloadable file response";
            return false;
        }
        url = *confirmation;
    }
    error = "Google Drive confirmation loop exceeded the safety limit";
    return false;
}

inline bool download_with_curl(
    const std::string& url,
    const std::filesystem::path& destination,
    std::atomic<bool>* cancel,
    CompleteTransferProgress* progress,
    std::string& error
) {
    static DynamicCurl library;
    if (!library) {
        error = library.error();
        return false;
    }
    auto* easy = library.easy_init();
    if (easy == nullptr) {
        error = "native libcurl could not create a transfer";
        return false;
    }
    struct Cleanup final {
        DynamicCurl* library{};
        DynamicCurl::Easy* easy{};
        ~Cleanup() { if (library != nullptr && easy != nullptr) library->easy_cleanup(easy); }
    } cleanup{&library, easy};
    if (!curl_setopt_common(library, easy, error)) {
        return false;
    }
    std::string resolved;
    if (!resolve_drive_url_with_curl(library, easy, url, cancel, resolved, error)) {
        return false;
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(destination.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "cannot create Complete download directory";
        return false;
    }
    const auto part = std::filesystem::path(destination.string() + ".part");
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            error = "Complete download cancelled";
            return false;
        }
        filesystem_error.clear();
        const auto resume_from = std::filesystem::is_regular_file(part, filesystem_error)
            && !filesystem_error ? std::filesystem::file_size(part, filesystem_error) : 0U;
        const std::string range = resume_from == 0U
            ? std::string{}
            : std::to_string(resume_from) + '-';
        CurlHeaderState header;
        CurlFileState file{
            .part = part,
            .header = &header,
            .resume_from = static_cast<std::uint64_t>(resume_from),
            .cancel = cancel,
            .progress = progress,
        };
        library.easy_setopt(easy, curlopt_url, resolved.c_str());
        library.easy_setopt(
            easy,
            curlopt_range,
            range.empty() ? static_cast<const char*>(nullptr) : range.c_str()
        );
        library.easy_setopt(easy, curlopt_headerdata, &header);
        library.easy_setopt(easy, curlopt_headerfunction, &curl_header_callback);
        library.easy_setopt(easy, curlopt_writedata, &file);
        library.easy_setopt(easy, curlopt_writefunction, &curl_file_callback);
        const int code = library.easy_perform(easy);
        if (file.output.is_open()) {
            file.output.flush();
            file.output.close();
        }
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            error = "Complete download cancelled";
            return false;
        }
        const bool http_ok = header.status >= 200L && header.status < 300L;
        if (code == curle_ok && http_ok && !file.failed) {
            return promote_part_file(part, destination, error);
        }
        if (header.status == 416L) {
            std::filesystem::remove(part, filesystem_error);
        }
        if (attempt == 3) {
            error = code == curle_ok
                ? "Google Drive download returned HTTP " + std::to_string(header.status)
                : std::string("Google Drive download failed: ") + library.easy_strerror(code);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500 * (attempt + 1)));
    }
    return false;
}

#endif  // desktop dynamic curl

#if defined(_WIN32)

[[nodiscard]] inline std::wstring utf8_to_wide(const std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0
    );
    if (required <= 0) {
        return {};
    }
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            output.data(),
            required
        ) != required) {
        return {};
    }
    return output;
}

[[nodiscard]] inline std::string wide_to_utf8(const std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (required <= 0) {
        return {};
    }
    std::string output(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            output.data(),
            required,
            nullptr,
            nullptr
        ) != required) {
        return {};
    }
    return output;
}

class DynamicWinHttp final {
public:
    DynamicWinHttp() {
        module_ = LoadLibraryW(L"winhttp.dll");
        if (module_ == nullptr) {
            error_ = "Windows WinHTTP is unavailable";
            return;
        }
#define PULSEFORGE_WINHTTP_SYMBOL(member, symbol_name) \
        member = reinterpret_cast<decltype(member)>(GetProcAddress(module_, symbol_name)); \
        if (member == nullptr) { error_ = "WinHTTP is missing " symbol_name; return; }
        PULSEFORGE_WINHTTP_SYMBOL(open, "WinHttpOpen")
        PULSEFORGE_WINHTTP_SYMBOL(connect, "WinHttpConnect")
        PULSEFORGE_WINHTTP_SYMBOL(open_request, "WinHttpOpenRequest")
        PULSEFORGE_WINHTTP_SYMBOL(send_request, "WinHttpSendRequest")
        PULSEFORGE_WINHTTP_SYMBOL(receive_response, "WinHttpReceiveResponse")
        PULSEFORGE_WINHTTP_SYMBOL(query_headers, "WinHttpQueryHeaders")
        PULSEFORGE_WINHTTP_SYMBOL(query_data_available, "WinHttpQueryDataAvailable")
        PULSEFORGE_WINHTTP_SYMBOL(read_data, "WinHttpReadData")
        PULSEFORGE_WINHTTP_SYMBOL(close_handle, "WinHttpCloseHandle")
        PULSEFORGE_WINHTTP_SYMBOL(crack_url, "WinHttpCrackUrl")
        PULSEFORGE_WINHTTP_SYMBOL(add_request_headers, "WinHttpAddRequestHeaders")
        PULSEFORGE_WINHTTP_SYMBOL(set_timeouts, "WinHttpSetTimeouts")
#undef PULSEFORGE_WINHTTP_SYMBOL
    }

    ~DynamicWinHttp() {
        if (module_ != nullptr) FreeLibrary(module_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return module_ != nullptr && error_.empty();
    }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    decltype(&::WinHttpOpen) open{};
    decltype(&::WinHttpConnect) connect{};
    decltype(&::WinHttpOpenRequest) open_request{};
    decltype(&::WinHttpSendRequest) send_request{};
    decltype(&::WinHttpReceiveResponse) receive_response{};
    decltype(&::WinHttpQueryHeaders) query_headers{};
    decltype(&::WinHttpQueryDataAvailable) query_data_available{};
    decltype(&::WinHttpReadData) read_data{};
    decltype(&::WinHttpCloseHandle) close_handle{};
    decltype(&::WinHttpCrackUrl) crack_url{};
    decltype(&::WinHttpAddRequestHeaders) add_request_headers{};
    decltype(&::WinHttpSetTimeouts) set_timeouts{};

private:
    HMODULE module_{};
    std::string error_;
};

struct WinHttpHandle final {
    DynamicWinHttp* api{};
    HINTERNET value{};
    ~WinHttpHandle() { if (api != nullptr && value != nullptr) api->close_handle(value); }
    WinHttpHandle() = default;
    WinHttpHandle(DynamicWinHttp* api_value, HINTERNET value_value)
        : api(api_value), value(value_value) {}
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& other) noexcept
        : api(other.api), value(other.value) { other.value = nullptr; }
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            if (api != nullptr && value != nullptr) api->close_handle(value);
            api = other.api;
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
};

struct WinHttpResponse {
    WinHttpHandle connect;
    WinHttpHandle request;
    DWORD status{};
    bool file_response{};
};

inline bool open_winhttp_response(
    DynamicWinHttp& api,
    HINTERNET session,
    const std::string& url,
    const std::optional<std::uint64_t> range_start,
    WinHttpResponse& response,
    std::string& error
) {
    const auto wide_url = utf8_to_wide(url);
    if (wide_url.empty()) {
        error = "invalid UTF-8 Google Drive URL";
        return false;
    }
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!api.crack_url(wide_url.c_str(), 0U, 0U, &components)) {
        error = "WinHTTP could not parse the Google Drive URL";
        return false;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTPS) {
        error = "Complete transport refused a non-HTTPS URL";
        return false;
    }
    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring target(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.lpszExtraInfo != nullptr && components.dwExtraInfoLength != 0U) {
        target.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    response.connect = WinHttpHandle(
        &api,
        api.connect(session, host.c_str(), components.nPort, 0U)
    );
    if (response.connect.value == nullptr) {
        error = "WinHTTP could not connect to Google Drive";
        return false;
    }
    response.request = WinHttpHandle(
        &api,
        api.open_request(
            response.connect.value,
            L"GET",
            target.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE
        )
    );
    if (response.request.value == nullptr) {
        error = "WinHTTP could not create a Google Drive request";
        return false;
    }
    if (range_start.has_value() && *range_start != 0U) {
        const auto range = std::wstring(L"Range: bytes=")
            + std::to_wstring(*range_start) + L"-";
        if (!api.add_request_headers(
                response.request.value,
                range.c_str(),
                static_cast<DWORD>(-1),
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE
            )) {
            error = "WinHTTP could not configure download resume";
            return false;
        }
    }
    if (!api.send_request(
            response.request.value,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0U,
            WINHTTP_NO_REQUEST_DATA,
            0U,
            0U,
            0U
        ) || !api.receive_response(response.request.value, nullptr)) {
        error = "WinHTTP request failed";
        return false;
    }
    DWORD status_size = sizeof(response.status);
    if (!api.query_headers(
            response.request.value,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &response.status,
            &status_size,
            WINHTTP_NO_HEADER_INDEX
        )) {
        error = "WinHTTP could not read the HTTP status";
        return false;
    }
    DWORD raw_size = 0U;
    api.query_headers(
        response.request.value,
        WINHTTP_QUERY_RAW_HEADERS_CRLF,
        WINHTTP_HEADER_NAME_BY_INDEX,
        WINHTTP_NO_OUTPUT_BUFFER,
        &raw_size,
        WINHTTP_NO_HEADER_INDEX
    );
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && raw_size != 0U) {
        std::wstring raw(raw_size / sizeof(wchar_t), L'\0');
        if (api.query_headers(
                response.request.value,
                WINHTTP_QUERY_RAW_HEADERS_CRLF,
                WINHTTP_HEADER_NAME_BY_INDEX,
                raw.data(),
                &raw_size,
                WINHTTP_NO_HEADER_INDEX
            )) {
            const auto lower = lowercase_ascii(wide_to_utf8(raw));
            response.file_response = lower.find("content-disposition:")
                != std::string::npos;
        }
    }
    return true;
}

inline bool read_winhttp_body(
    DynamicWinHttp& api,
    HINTERNET request,
    std::atomic<bool>* cancel,
    std::string& body,
    const std::size_t maximum,
    std::string& error
) {
    for (;;) {
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            error = "Complete download cancelled";
            return false;
        }
        DWORD available = 0U;
        if (!api.query_data_available(request, &available)) {
            error = "WinHTTP could not query response data";
            return false;
        }
        if (available == 0U) {
            return true;
        }
        if (body.size() > maximum - std::min(maximum, static_cast<std::size_t>(available))) {
            error = "Google Drive confirmation page exceeded the safety limit";
            return false;
        }
        std::vector<char> buffer(std::min<DWORD>(available, 256U * 1024U));
        DWORD read = 0U;
        if (!api.read_data(
                request,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &read
            )) {
            error = "WinHTTP could not read response data";
            return false;
        }
        if (read == 0U) {
            return true;
        }
        body.append(buffer.data(), read);
    }
}

inline bool resolve_drive_url_with_winhttp(
    DynamicWinHttp& api,
    HINTERNET session,
    std::string url,
    std::atomic<bool>* cancel,
    std::string& resolved,
    std::string& error
) {
    for (int round = 0; round < 6; ++round) {
        WinHttpResponse response;
        if (!open_winhttp_response(api, session, url, std::nullopt, response, error)) {
            return false;
        }
        if (response.status >= 400U) {
            error = "Google Drive returned HTTP " + std::to_string(response.status);
            return false;
        }
        if (response.file_response) {
            resolved = std::move(url);
            return true;
        }
        std::string body;
        if (!read_winhttp_body(
                api,
                response.request.value,
                cancel,
                body,
                maximum_confirmation_html,
                error
            )) {
            return false;
        }
        const auto confirmation = drive_confirmation_url(body);
        if (!confirmation.has_value()) {
            error = "Google Drive did not provide a downloadable file response";
            return false;
        }
        url = *confirmation;
    }
    error = "Google Drive confirmation loop exceeded the safety limit";
    return false;
}

inline bool download_with_winhttp(
    const std::string& url,
    const std::filesystem::path& destination,
    std::atomic<bool>* cancel,
    CompleteTransferProgress* progress,
    std::string& error
) {
    static DynamicWinHttp api;
    if (!api) {
        error = api.error();
        return false;
    }
    const auto agent = utf8_to_wide(browser_user_agent);
    WinHttpHandle session(
        &api,
        api.open(
            agent.c_str(),
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0U
        )
    );
    if (session.value == nullptr) {
        error = "WinHTTP could not create an HTTPS session";
        return false;
    }
    api.set_timeouts(session.value, 30'000, 30'000, 30'000, 60'000);
    std::string resolved;
    if (!resolve_drive_url_with_winhttp(api, session.value, url, cancel, resolved, error)) {
        return false;
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(destination.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "cannot create Complete download directory";
        return false;
    }
    const auto part = std::filesystem::path(destination.string() + ".part");
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            error = "Complete download cancelled";
            return false;
        }
        filesystem_error.clear();
        const auto resume = std::filesystem::is_regular_file(part, filesystem_error)
            && !filesystem_error ? std::filesystem::file_size(part, filesystem_error) : 0U;
        WinHttpResponse response;
        if (!open_winhttp_response(
                api,
                session.value,
                resolved,
                static_cast<std::uint64_t>(resume),
                response,
                error
            )) {
            if (attempt == 3) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(500 * (attempt + 1)));
            continue;
        }
        if (response.status == 416U) {
            std::filesystem::remove(part, filesystem_error);
            continue;
        }
        if (response.status < 200U || response.status >= 300U) {
            error = "Google Drive download returned HTTP " + std::to_string(response.status);
            if (attempt == 3) return false;
            continue;
        }
        const bool append = resume != 0U && response.status == 206U;
        std::ofstream output(
            part,
            std::ios::binary | (append ? std::ios::app : std::ios::trunc)
        );
        if (!output.is_open()) {
            error = "cannot open Complete partial download";
            return false;
        }
        bool failed = false;
        for (;;) {
            if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
                error = "Complete download cancelled";
                return false;
            }
            DWORD available = 0U;
            if (!api.query_data_available(response.request.value, &available)) {
                failed = true;
                break;
            }
            if (available == 0U) break;
            std::vector<char> buffer(std::min<DWORD>(available, 256U * 1024U));
            DWORD read = 0U;
            if (!api.read_data(
                    response.request.value,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &read
                )) {
                failed = true;
                break;
            }
            if (read == 0U) break;
            output.write(buffer.data(), read);
            if (!output) {
                failed = true;
                break;
            }
            if (progress != nullptr) {
                progress->bytes_received.fetch_add(read, std::memory_order_relaxed);
            }
        }
        output.flush();
        output.close();
        if (!failed) {
            return promote_part_file(part, destination, error);
        }
        if (attempt == 3) {
            error = "WinHTTP download was interrupted";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500 * (attempt + 1)));
    }
    return false;
}

#endif  // _WIN32

#if defined(__ANDROID__)

inline jclass load_android_bridge_class(JNIEnv* env, jobject activity, std::string& error) {
    if (env == nullptr || activity == nullptr) {
        error = "Android JNI environment is unavailable";
        return nullptr;
    }
    jclass activity_class = env->GetObjectClass(activity);
    if (activity_class == nullptr) {
        error = "Android activity class is unavailable";
        return nullptr;
    }
    const jmethodID get_loader = env->GetMethodID(
        activity_class,
        "getClassLoader",
        "()Ljava/lang/ClassLoader;"
    );
    if (get_loader == nullptr) {
        env->DeleteLocalRef(activity_class);
        error = "Android class loader is unavailable";
        return nullptr;
    }
    jobject loader = env->CallObjectMethod(activity, get_loader);
    env->DeleteLocalRef(activity_class);
    if (loader == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        error = "Android class loader could not be obtained";
        return nullptr;
    }
    jclass loader_class = env->GetObjectClass(loader);
    const jmethodID load_class = loader_class == nullptr ? nullptr : env->GetMethodID(
        loader_class,
        "loadClass",
        "(Ljava/lang/String;)Ljava/lang/Class;"
    );
    if (load_class == nullptr) {
        if (loader_class != nullptr) env->DeleteLocalRef(loader_class);
        env->DeleteLocalRef(loader);
        error = "Android loadClass method is unavailable";
        return nullptr;
    }
    jstring name = env->NewStringUTF("org.pulseforge.engine.CompleteDownloadBridge");
    jobject loaded = env->CallObjectMethod(loader, load_class, name);
    env->DeleteLocalRef(name);
    env->DeleteLocalRef(loader_class);
    env->DeleteLocalRef(loader);
    if (env->ExceptionCheck() || loaded == nullptr) {
        env->ExceptionClear();
        error = "Android CompleteDownloadBridge could not be loaded";
        return nullptr;
    }
    return static_cast<jclass>(loaded);
}

inline bool download_with_android_bridge(
    const std::string& url,
    const std::filesystem::path& destination,
    std::atomic<bool>* cancel,
    CompleteTransferProgress*,
    std::string& error
) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
        error = "Complete download cancelled";
        return false;
    }
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject activity = SDL_GetAndroidActivity();
    jclass bridge = load_android_bridge_class(env, activity, error);
    if (bridge == nullptr) {
        return false;
    }
    const jmethodID method = env->GetStaticMethodID(
        bridge,
        "download",
        "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"
    );
    if (method == nullptr) {
        env->DeleteLocalRef(bridge);
        error = "Android CompleteDownloadBridge.download is unavailable";
        return false;
    }
    const auto path = path_utf8_transport(destination);
    jstring jurl = env->NewStringUTF(url.c_str());
    jstring jpath = env->NewStringUTF(path.c_str());
    auto* result = static_cast<jstring>(
        env->CallStaticObjectMethod(bridge, method, jurl, jpath)
    );
    env->DeleteLocalRef(jurl);
    env->DeleteLocalRef(jpath);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        env->DeleteLocalRef(bridge);
        error = "Android HTTPS download raised a Java exception";
        return false;
    }
    if (result != nullptr) {
        const char* text = env->GetStringUTFChars(result, nullptr);
        if (text != nullptr) {
            error = text;
            env->ReleaseStringUTFChars(result, text);
        } else {
            error = "Android HTTPS download failed";
        }
        env->DeleteLocalRef(result);
        env->DeleteLocalRef(bridge);
        return false;
    }
    env->DeleteLocalRef(bridge);
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
        error = "Complete download cancelled";
        return false;
    }
    return true;
}

#endif  // __ANDROID__

inline bool download_drive_file(
    const std::string& url,
    const std::filesystem::path& destination,
    std::atomic<bool>* cancel,
    CompleteTransferProgress* progress,
    std::string& error
) {
#if defined(_WIN32)
    return download_with_winhttp(url, destination, cancel, progress, error);
#elif defined(__ANDROID__)
    return download_with_android_bridge(url, destination, cancel, progress, error);
#else
    return download_with_curl(url, destination, cancel, progress, error);
#endif
}

}  // namespace complete_transport_detail

[[nodiscard]] inline CompleteTransferResult install_pending_complete_runtime(
    const CompleteRuntimeManifest& manifest,
    const CompleteRuntimePlan& plan,
    const std::filesystem::path& mods_root,
    CompleteTransferProgress* const progress,
    std::atomic<bool>* const cancel
) {
    using namespace complete_transport_detail;
    CompleteTransferResult result;
    std::uint64_t total_files = 0U;
    std::uint64_t total_mods = 0U;
    for (const auto& entry : plan.entries) {
        if (entry.requires_install() && entry.mod != nullptr) {
            ++total_mods;
            total_files += static_cast<std::uint64_t>(entry.mod->files.size());
        }
    }
    if (progress != nullptr) {
        progress->total_files.store(total_files, std::memory_order_relaxed);
        progress->total_mods.store(total_mods, std::memory_order_relaxed);
    }

    const auto staging_root = mods_root / ".pulseforge-complete-staging";
    std::error_code filesystem_error;
    std::filesystem::create_directories(staging_root, filesystem_error);
    if (filesystem_error) {
        result.error = "cannot create Complete staging root: " + filesystem_error.message();
        return result;
    }

    for (const auto& entry : plan.entries) {
        if (!entry.requires_install() || entry.mod == nullptr) {
            continue;
        }
        const auto& mod = *entry.mod;
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            result.error = "Complete installation cancelled";
            return result;
        }
        const auto revision_prefix = mod.revision.substr(0U, 16U);
        const auto staging = staging_root / (mod.slug + "-" + revision_prefix);
        std::filesystem::create_directories(staging, filesystem_error);
        if (filesystem_error) {
            result.error = "cannot create Complete mod staging: " + filesystem_error.message();
            return result;
        }
        set_progress_detail(progress, "A transferir " + mod.name);
        for (const auto& file : mod.files) {
            if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
                result.cancelled = true;
                result.error = "Complete installation cancelled";
                return result;
            }
            const auto destination = staging / file.relative;
            std::string download_error;
            if (!download_drive_file(
                    file.url,
                    destination,
                    cancel,
                    progress,
                    download_error
                )) {
                if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
                    result.cancelled = true;
                }
                result.error = mod.name + ": " + download_error;
                return result;
            }
            ++result.downloaded_files;
            if (progress != nullptr) {
                progress->files_done.fetch_add(1U, std::memory_order_relaxed);
            }
        }
        set_progress_detail(progress, "A validar e instalar " + mod.name);
        const auto installed = install_complete_runtime_staging(
            manifest,
            mod,
            staging,
            mods_root
        );
        if (!installed.success) {
            result.error = mod.name + ": " + installed.error;
            return result;
        }
        ++result.installed_mods;
        if (progress != nullptr) {
            progress->mods_done.fetch_add(1U, std::memory_order_relaxed);
        }
        std::filesystem::remove_all(staging, filesystem_error);
        filesystem_error.clear();
    }

    result.success = true;
    return result;
}

}  // namespace pulseforge::detail
