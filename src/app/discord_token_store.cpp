#include "discord_token_store.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <wincrypt.h>
#elif defined(__APPLE__)
#include <Security/Security.h>
#elif defined(__ANDROID__)
#include <jni.h>
#include <SDL3/SDL_system.h>
#endif

namespace pulseforge::detail {
namespace {

void set_error(std::string* error, std::string message) noexcept {
    if (error == nullptr) return;
    try {
        *error = std::move(message);
    } catch (...) {
    }
}

void clear_error(std::string* error) noexcept {
    if (error != nullptr) error->clear();
}

[[nodiscard]] std::string application_text(const std::uint64_t application_id) {
    return std::to_string(application_id);
}

#if defined(_WIN32)
[[nodiscard]] std::optional<std::filesystem::path> windows_environment_path(
    const wchar_t* const name
) noexcept {
    try {
        if (name == nullptr || *name == L'\0') return std::nullopt;
        const DWORD required = GetEnvironmentVariableW(name, nullptr, 0U);
        if (required == 0U) return std::nullopt;

        std::wstring value(static_cast<std::size_t>(required), L'\0');
        const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
        if (written == 0U || written >= required) return std::nullopt;
        value.resize(static_cast<std::size_t>(written));
        return std::filesystem::path(std::move(value));
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::filesystem::path> windows_store_root() noexcept {
    try {
        auto base = windows_environment_path(L"LOCALAPPDATA");
        if (!base.has_value()) {
            base = windows_environment_path(L"APPDATA");
        }
        if (!base.has_value() || base->empty()) return std::nullopt;
        return *base / L"PulseForge" / L"credentials" / L"discord";
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::vector<std::byte> entropy_for(
    const std::uint64_t application_id
) {
    const auto text = std::string{"PulseForge Discord refresh token v1:"}
        + application_text(application_id);
    std::vector<std::byte> bytes(text.size());
    for (std::size_t index = 0U; index < text.size(); ++index) {
        bytes[index] = static_cast<std::byte>(
            static_cast<unsigned char>(text[index])
        );
    }
    return bytes;
}

[[nodiscard]] bool write_atomic(
    const std::filesystem::path& target,
    const std::span<const std::byte> bytes,
    std::string* error
) noexcept {
    try {
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            // A concurrent creator can race with create_directories(). Treat an
            // actually-existing directory as success; otherwise preserve the OS
            // error code so the UI gives an actionable diagnostic.
            std::error_code exists_ec;
            const bool directory_exists = std::filesystem::is_directory(
                target.parent_path(),
                exists_ec
            );
            if (!directory_exists) {
                std::string message = "could not create the Windows credential directory";
                message += " [" + std::to_string(ec.value()) + "]";
                if (!ec.message().empty()) {
                    message += ": " + ec.message();
                }
                set_error(error, std::move(message));
                return false;
            }
            ec.clear();
        }
        auto temporary = target;
        temporary += ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                set_error(error, "could not open the Windows credential file");
                return false;
            }
            output.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size())
            );
            output.flush();
            if (!output) {
                set_error(error, "could not write the Windows credential file");
                return false;
            }
        }
        std::filesystem::remove(target, ec);
        ec.clear();
        std::filesystem::rename(temporary, target, ec);
        if (ec) {
            std::filesystem::remove(temporary, ec);
            set_error(error, "could not publish the Windows credential file");
            return false;
        }
        return true;
    } catch (...) {
        set_error(error, "Windows credential storage failed");
        return false;
    }
}
#endif

#if defined(__linux__) && !defined(__ANDROID__)
[[nodiscard]] bool linux_secret_tool_available() noexcept {
    const int status = std::system(
        "command -v secret-tool >/dev/null 2>&1"
    );
    return status == 0;
}

[[nodiscard]] std::string linux_secret_command(
    const std::string_view operation,
    const std::uint64_t application_id
) {
    return "secret-tool " + std::string(operation)
        + " service org.pulseforge.engine.discord application_id "
        + application_text(application_id);
}
#endif

#if defined(__ANDROID__)
class AndroidLocalFrame final {
public:
    explicit AndroidLocalFrame(JNIEnv* env) : env_(env) {
        valid_ = env_ != nullptr && env_->PushLocalFrame(16) == JNI_OK;
    }
    ~AndroidLocalFrame() {
        if (valid_) env_->PopLocalFrame(nullptr);
    }
    [[nodiscard]] bool valid() const noexcept { return valid_; }
private:
    JNIEnv* env_{};
    bool valid_{};
};

[[nodiscard]] bool android_clear_exception(JNIEnv* env) noexcept {
    if (env == nullptr || !env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

[[nodiscard]] jobject android_activity(JNIEnv* env) noexcept {
    if (env == nullptr) return nullptr;
    return static_cast<jobject>(SDL_GetAndroidActivity());
}

[[nodiscard]] jmethodID android_method(
    JNIEnv* env,
    jobject activity,
    const char* name,
    const char* signature
) noexcept {
    if (env == nullptr || activity == nullptr) return nullptr;
    const auto cls = env->GetObjectClass(activity);
    if (cls == nullptr || android_clear_exception(env)) return nullptr;
    const auto method = env->GetMethodID(cls, name, signature);
    if (android_clear_exception(env)) return nullptr;
    return method;
}
#endif

#if defined(__APPLE__)
constexpr const char* apple_service = "org.pulseforge.engine.discord";
#endif

}  // namespace

bool discord_secure_token_store_available() noexcept {
#if defined(_WIN32) || defined(__APPLE__) || defined(__ANDROID__)
    return true;
#elif defined(__linux__)
    return linux_secret_tool_available();
#else
    return false;
#endif
}

std::optional<std::string> discord_secure_token_load(
    const std::uint64_t application_id,
    std::string* error
) noexcept {
    clear_error(error);
    if (application_id == 0U) {
        set_error(error, "Discord Application ID is zero");
        return std::nullopt;
    }
#if defined(_WIN32)
    try {
        const auto root = windows_store_root();
        if (!root.has_value()) {
            set_error(error, "Windows user credential directory is unavailable");
            return std::nullopt;
        }
        const auto path = *root / (application_text(application_id) + ".dpapi");
        std::ifstream input(path, std::ios::binary);
        if (!input) return std::nullopt;
        const std::vector<unsigned char> encrypted(
            (std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>()
        );
        if (encrypted.empty()
            || encrypted.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
            set_error(error, "stored Discord credential is invalid");
            return std::nullopt;
        }
        auto entropy = entropy_for(application_id);
        DATA_BLOB input_blob{
            static_cast<DWORD>(encrypted.size()),
            const_cast<BYTE*>(encrypted.data())
        };
        DATA_BLOB entropy_blob{
            static_cast<DWORD>(entropy.size()),
            reinterpret_cast<BYTE*>(entropy.data())
        };
        DATA_BLOB output_blob{};
        if (!CryptUnprotectData(
                &input_blob,
                nullptr,
                &entropy_blob,
                nullptr,
                nullptr,
                CRYPTPROTECT_UI_FORBIDDEN,
                &output_blob
            )) {
            set_error(error, "Windows DPAPI could not decrypt the Discord credential");
            return std::nullopt;
        }
        std::string result(
            reinterpret_cast<const char*>(output_blob.pbData),
            static_cast<std::size_t>(output_blob.cbData)
        );
        if (output_blob.pbData != nullptr && output_blob.cbData != 0U) {
            SecureZeroMemory(output_blob.pbData, output_blob.cbData);
        }
        LocalFree(output_blob.pbData);
        if (result.empty()) return std::nullopt;
        return result;
    } catch (...) {
        set_error(error, "Windows DPAPI credential load failed");
        return std::nullopt;
    }
#elif defined(__APPLE__)
    const auto account = application_text(application_id);
    void* data = nullptr;
    UInt32 size = 0U;
    SecKeychainItemRef item = nullptr;
    const OSStatus status = SecKeychainFindGenericPassword(
        nullptr,
        static_cast<UInt32>(std::char_traits<char>::length(apple_service)),
        apple_service,
        static_cast<UInt32>(account.size()),
        account.data(),
        &size,
        &data,
        &item
    );
    if (item != nullptr) CFRelease(item);
    if (status == errSecItemNotFound) return std::nullopt;
    if (status != errSecSuccess || data == nullptr) {
        set_error(error, "Apple Keychain could not load the Discord credential");
        return std::nullopt;
    }
    std::string result(static_cast<const char*>(data), static_cast<std::size_t>(size));
    SecKeychainItemFreeContent(nullptr, data);
    return result.empty() ? std::nullopt : std::optional<std::string>{std::move(result)};
#elif defined(__ANDROID__)
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) {
        set_error(error, "Android JNI is unavailable for secure Discord storage");
        return std::nullopt;
    }
    const auto activity = android_activity(env);
    if (activity == nullptr) {
        set_error(error, "Android activity is unavailable for secure Discord storage");
        return std::nullopt;
    }
    const auto method = android_method(
        env, activity, "loadDiscordRefreshToken", "(Ljava/lang/String;)Ljava/lang/String;"
    );
    if (method == nullptr) {
        set_error(error, "Android secure Discord storage bridge is unavailable");
        return std::nullopt;
    }
    const auto app = env->NewStringUTF(application_text(application_id).c_str());
    const auto value = static_cast<jstring>(env->CallObjectMethod(activity, method, app));
    if (android_clear_exception(env)) {
        set_error(error, "Android Keystore could not load the Discord credential");
        return std::nullopt;
    }
    if (value == nullptr) return std::nullopt;
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr || android_clear_exception(env)) {
        set_error(error, "Android credential could not be decoded");
        return std::nullopt;
    }
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result.empty() ? std::nullopt : std::optional<std::string>{std::move(result)};
#elif defined(__linux__)
    if (!linux_secret_tool_available()) {
        set_error(error, "Secret Service persistence is unavailable (install secret-tool/libsecret-tools)");
        return std::nullopt;
    }
    const auto command = linux_secret_command("lookup", application_id);
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        set_error(error, "Secret Service credential lookup could not start");
        return std::nullopt;
    }
    std::string result;
    std::array<char, 512U> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result += buffer.data();
        if (result.size() > 64U * 1024U) break;
    }
    const int status = pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    if (status != 0 || result.empty()) return std::nullopt;
    return result;
#else
    set_error(error, "secure Discord credential persistence is unavailable on this platform");
    return std::nullopt;
#endif
}

bool discord_secure_token_save(
    const std::uint64_t application_id,
    const std::string_view refresh_token,
    std::string* error
) noexcept {
    clear_error(error);
    if (application_id == 0U || refresh_token.empty()) {
        set_error(error, "Discord refresh token or Application ID is empty");
        return false;
    }
#if defined(_WIN32)
    try {
        if (refresh_token.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
            set_error(error, "Discord refresh token is too large for Windows DPAPI");
            return false;
        }
        const auto root = windows_store_root();
        if (!root.has_value()) {
            set_error(error, "Windows user credential directory is unavailable");
            return false;
        }
        auto entropy = entropy_for(application_id);
        DATA_BLOB input_blob{
            static_cast<DWORD>(refresh_token.size()),
            reinterpret_cast<BYTE*>(const_cast<char*>(refresh_token.data()))
        };
        DATA_BLOB entropy_blob{
            static_cast<DWORD>(entropy.size()),
            reinterpret_cast<BYTE*>(entropy.data())
        };
        DATA_BLOB output_blob{};
        if (!CryptProtectData(
                &input_blob,
                L"PulseForge Discord refresh token",
                &entropy_blob,
                nullptr,
                nullptr,
                CRYPTPROTECT_UI_FORBIDDEN,
                &output_blob
            )) {
            set_error(error, "Windows DPAPI could not encrypt the Discord credential");
            return false;
        }
        std::vector<std::byte> encrypted(output_blob.cbData);
        for (DWORD index = 0U; index < output_blob.cbData; ++index) {
            encrypted[static_cast<std::size_t>(index)] =
                static_cast<std::byte>(output_blob.pbData[index]);
        }
        if (output_blob.pbData != nullptr && output_blob.cbData != 0U) {
            SecureZeroMemory(output_blob.pbData, output_blob.cbData);
        }
        LocalFree(output_blob.pbData);
        return write_atomic(
            *root / (application_text(application_id) + ".dpapi"),
            encrypted,
            error
        );
    } catch (...) {
        set_error(error, "Windows DPAPI credential save failed");
        return false;
    }
#elif defined(__APPLE__)
    const auto account = application_text(application_id);
    SecKeychainItemRef item = nullptr;
    void* old_data = nullptr;
    UInt32 old_size = 0U;
    const OSStatus find_status = SecKeychainFindGenericPassword(
        nullptr,
        static_cast<UInt32>(std::char_traits<char>::length(apple_service)),
        apple_service,
        static_cast<UInt32>(account.size()),
        account.data(),
        &old_size,
        &old_data,
        &item
    );
    if (old_data != nullptr) SecKeychainItemFreeContent(nullptr, old_data);
    OSStatus status = errSecSuccess;
    if (find_status == errSecSuccess && item != nullptr) {
        status = SecKeychainItemModifyAttributesAndData(
            item,
            nullptr,
            static_cast<UInt32>(refresh_token.size()),
            refresh_token.data()
        );
        CFRelease(item);
    } else if (find_status == errSecItemNotFound) {
        status = SecKeychainAddGenericPassword(
            nullptr,
            static_cast<UInt32>(std::char_traits<char>::length(apple_service)),
            apple_service,
            static_cast<UInt32>(account.size()),
            account.data(),
            static_cast<UInt32>(refresh_token.size()),
            refresh_token.data(),
            nullptr
        );
    } else {
        if (item != nullptr) CFRelease(item);
        status = find_status;
    }
    if (status != errSecSuccess) {
        set_error(error, "Apple Keychain could not save the Discord credential");
        return false;
    }
    return true;
#elif defined(__ANDROID__)
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) {
        set_error(error, "Android JNI is unavailable for secure Discord storage");
        return false;
    }
    const auto activity = android_activity(env);
    const auto method = android_method(
        env, activity, "storeDiscordRefreshToken", "(Ljava/lang/String;Ljava/lang/String;)Z"
    );
    if (activity == nullptr || method == nullptr) {
        set_error(error, "Android secure Discord storage bridge is unavailable");
        return false;
    }
    const auto app = env->NewStringUTF(application_text(application_id).c_str());
    const auto token = env->NewStringUTF(std::string(refresh_token).c_str());
    const auto ok = env->CallBooleanMethod(activity, method, app, token);
    if (android_clear_exception(env) || ok != JNI_TRUE) {
        set_error(error, "Android Keystore could not save the Discord credential");
        return false;
    }
    return true;
#elif defined(__linux__)
    if (!linux_secret_tool_available()) {
        set_error(error, "Secret Service persistence is unavailable (install secret-tool/libsecret-tools)");
        return false;
    }
    const auto command = linux_secret_command(
        "store --label='PulseForge Discord'",
        application_id
    );
    FILE* pipe = popen(command.c_str(), "w");
    if (pipe == nullptr) {
        set_error(error, "Secret Service credential store could not start");
        return false;
    }
    const auto written = std::fwrite(refresh_token.data(), 1U, refresh_token.size(), pipe);
    static_cast<void>(std::fputc('\n', pipe));
    const int status = pclose(pipe);
    if (written != refresh_token.size() || status != 0) {
        set_error(error, "Secret Service could not save the Discord credential");
        return false;
    }
    return true;
#else
    set_error(error, "secure Discord credential persistence is unavailable on this platform");
    return false;
#endif
}

bool discord_secure_token_erase(
    const std::uint64_t application_id,
    std::string* error
) noexcept {
    clear_error(error);
    if (application_id == 0U) return true;
#if defined(_WIN32)
    try {
        const auto root = windows_store_root();
        if (!root.has_value()) return true;
        std::error_code ec;
        std::filesystem::remove(
            *root / (application_text(application_id) + ".dpapi"), ec
        );
        if (ec) {
            set_error(error, "Windows credential file could not be removed");
            return false;
        }
        return true;
    } catch (...) {
        set_error(error, "Windows credential erase failed");
        return false;
    }
#elif defined(__APPLE__)
    const auto account = application_text(application_id);
    SecKeychainItemRef item = nullptr;
    const OSStatus status = SecKeychainFindGenericPassword(
        nullptr,
        static_cast<UInt32>(std::char_traits<char>::length(apple_service)),
        apple_service,
        static_cast<UInt32>(account.size()),
        account.data(),
        nullptr,
        nullptr,
        &item
    );
    if (status == errSecItemNotFound) return true;
    if (status != errSecSuccess || item == nullptr) {
        set_error(error, "Apple Keychain could not locate the Discord credential");
        return false;
    }
    const OSStatus delete_status = SecKeychainItemDelete(item);
    CFRelease(item);
    if (delete_status != errSecSuccess) {
        set_error(error, "Apple Keychain could not erase the Discord credential");
        return false;
    }
    return true;
#elif defined(__ANDROID__)
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) {
        set_error(error, "Android JNI is unavailable for secure Discord storage");
        return false;
    }
    const auto activity = android_activity(env);
    const auto method = android_method(
        env, activity, "eraseDiscordRefreshToken", "(Ljava/lang/String;)Z"
    );
    if (activity == nullptr || method == nullptr) {
        set_error(error, "Android secure Discord storage bridge is unavailable");
        return false;
    }
    const auto app = env->NewStringUTF(application_text(application_id).c_str());
    const auto ok = env->CallBooleanMethod(activity, method, app);
    if (android_clear_exception(env) || ok != JNI_TRUE) {
        set_error(error, "Android Keystore could not erase the Discord credential");
        return false;
    }
    return true;
#elif defined(__linux__)
    if (!linux_secret_tool_available()) return true;
    const auto command = linux_secret_command("clear", application_id);
    const int status = std::system((command + " >/dev/null 2>&1").c_str());
    if (status != 0) {
        set_error(error, "Secret Service could not erase the Discord credential");
        return false;
    }
    return true;
#else
    return true;
#endif
}

}  // namespace pulseforge::detail
