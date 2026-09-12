#include "android_runtime_bridge.hpp"

#include <charconv>
#include <string>

#if defined(__ANDROID__)
#include <jni.h>
#include <SDL3/SDL_system.h>
#endif

namespace pulseforge::detail {
namespace {

#if defined(__ANDROID__)
class AndroidLocalFrame final {
public:
    explicit AndroidLocalFrame(JNIEnv* const env) noexcept : env_(env) {
        valid_ = env_ != nullptr && env_->PushLocalFrame(64) == JNI_OK;
    }
    ~AndroidLocalFrame() {
        if (valid_) env_->PopLocalFrame(nullptr);
    }
    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    JNIEnv* env_{};
    bool valid_{};
};

[[nodiscard]] bool clear_exception(JNIEnv* const env) noexcept {
    if (env == nullptr || !env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

[[nodiscard]] jobject activity(JNIEnv* const env) noexcept {
    return env == nullptr ? nullptr
                          : static_cast<jobject>(SDL_GetAndroidActivity());
}

[[nodiscard]] jmethodID method(
    JNIEnv* const env,
    jobject const object,
    const char* const name,
    const char* const signature
) noexcept {
    if (env == nullptr || object == nullptr) return nullptr;
    const auto cls = env->GetObjectClass(object);
    if (cls == nullptr || clear_exception(env)) return nullptr;
    const auto result = env->GetMethodID(cls, name, signature);
    return clear_exception(env) ? nullptr : result;
}

[[nodiscard]] std::string java_string(
    JNIEnv* const env,
    jstring const value
) {
    if (env == nullptr || value == nullptr) return {};
    const char* const chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr || clear_exception(env)) return {};
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    if (clear_exception(env)) return {};
    return result;
}

[[nodiscard]] jobjectArray java_string_array(
    JNIEnv* const env,
    const std::span<const std::string> values
) {
    if (env == nullptr || values.size() > static_cast<std::size_t>(INT32_MAX)) {
        return nullptr;
    }
    const auto string_class = env->FindClass("java/lang/String");
    if (string_class == nullptr || clear_exception(env)) return nullptr;
    const auto result = env->NewObjectArray(
        static_cast<jsize>(values.size()),
        string_class,
        nullptr
    );
    if (result == nullptr || clear_exception(env)) return nullptr;
    for (std::size_t index = 0U; index < values.size(); ++index) {
        const auto value = env->NewStringUTF(values[index].c_str());
        if (value == nullptr || clear_exception(env)) return nullptr;
        env->SetObjectArrayElement(result, static_cast<jsize>(index), value);
        if (clear_exception(env)) return nullptr;
        env->DeleteLocalRef(value);
    }
    return result;
}
#endif

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {
        reinterpret_cast<const char*>(value.data()),
        value.size(),
    };
}

void assign_error(std::string* const error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

}  // namespace

std::string android_runtime_diagnostics() {
#if defined(__ANDROID__)
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) return {};
    const auto owner = activity(env);
    if (owner == nullptr) return {};
    const auto call = method(
        env,
        owner,
        "getPulseForgeRuntimeStats",
        "()Ljava/lang/String;"
    );
    if (call == nullptr) return {};
    const auto value = static_cast<jstring>(env->CallObjectMethod(owner, call));
    if (clear_exception(env)) return {};
    return java_string(env, value);
#else
    return {};
#endif
}

std::string publish_file_to_downloads(
    const std::filesystem::path& source,
    const std::string_view display_name,
    const std::string_view mime_type
) {
#if defined(__ANDROID__)
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) return {};
    const auto owner = activity(env);
    if (owner == nullptr) return {};
    const auto call = method(
        env,
        owner,
        "publishPulseForgeDownload",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"
    );
    if (call == nullptr) return {};

    const auto source_text = path_utf8(source);
    const std::string name(display_name);
    const std::string mime(mime_type);
    const auto java_source = env->NewStringUTF(source_text.c_str());
    const auto java_name = env->NewStringUTF(name.c_str());
    const auto java_mime = env->NewStringUTF(mime.c_str());
    if (java_source == nullptr || java_name == nullptr || java_mime == nullptr
        || clear_exception(env)) {
        return {};
    }
    const auto value = static_cast<jstring>(env->CallObjectMethod(
        owner,
        call,
        java_source,
        java_name,
        java_mime
    ));
    if (clear_exception(env)) return {};
    return java_string(env, value);
#else
    static_cast<void>(display_name);
    static_cast<void>(mime_type);
    return path_utf8(source);
#endif
}

std::optional<AndroidFfmpegSession> start_android_ffmpeg(
    const std::span<const std::string> arguments,
    std::string* const error
) {
#if defined(__ANDROID__)
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) {
        assign_error(error, "Android JNI is unavailable for FFmpegKit");
        return std::nullopt;
    }
    const auto owner = activity(env);
    if (owner == nullptr) {
        assign_error(error, "Android activity is unavailable for FFmpegKit");
        return std::nullopt;
    }
    const auto call = method(
        env,
        owner,
        "beginPulseForgeFfmpeg",
        "([Ljava/lang/String;)Ljava/lang/String;"
    );
    if (call == nullptr) {
        assign_error(error, "embedded Android FFmpegKit bridge is unavailable");
        return std::nullopt;
    }
    const auto java_arguments = java_string_array(env, arguments);
    if (java_arguments == nullptr) {
        assign_error(error, "could not marshal FFmpegKit arguments");
        return std::nullopt;
    }
    const auto value = static_cast<jstring>(env->CallObjectMethod(
        owner,
        call,
        java_arguments
    ));
    if (clear_exception(env)) {
        assign_error(error, "FFmpegKit failed while creating the Android encode session");
        return std::nullopt;
    }
    const auto result = java_string(env, value);
    const auto newline = result.find('\n');
    if (newline == std::string::npos || newline == 0U
        || newline + 1U >= result.size()) {
        assign_error(
            error,
            result.empty()
                ? "FFmpegKit did not return a raw-video pipe"
                : result
        );
        return std::nullopt;
    }
    std::int64_t id{-1};
    const auto parsed = std::from_chars(
        result.data(), result.data() + newline, id
    );
    if (parsed.ec != std::errc{} || parsed.ptr != result.data() + newline
        || id < 0) {
        assign_error(error, "FFmpegKit returned an invalid session id");
        return std::nullopt;
    }
    if (error != nullptr) error->clear();
    return AndroidFfmpegSession{
        id,
        std::filesystem::path(result.substr(newline + 1U)),
    };
#else
    static_cast<void>(arguments);
    assign_error(error, "embedded FFmpegKit is available only on Android");
    return std::nullopt;
#endif
}

bool finish_android_ffmpeg(
    const std::int64_t session_id,
    int& exit_code,
    std::string& diagnostic_output,
    std::string* const error
) {
#if defined(__ANDROID__)
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) {
        assign_error(error, "Android JNI is unavailable while waiting for FFmpegKit");
        return false;
    }
    const auto owner = activity(env);
    const auto call = method(
        env,
        owner,
        "finishPulseForgeFfmpeg",
        "(J)Ljava/lang/String;"
    );
    if (call == nullptr) {
        assign_error(error, "embedded Android FFmpegKit wait bridge is unavailable");
        return false;
    }
    const auto value = static_cast<jstring>(env->CallObjectMethod(
        owner,
        call,
        static_cast<jlong>(session_id)
    ));
    if (clear_exception(env)) {
        assign_error(error, "FFmpegKit failed while finalizing the Android encode session");
        return false;
    }
    const auto result = java_string(env, value);
    const auto newline = result.find('\n');
    const auto code_text = result.substr(0U, newline);
    const auto parsed = std::from_chars(
        code_text.data(), code_text.data() + code_text.size(), exit_code
    );
    if (parsed.ec != std::errc{} || parsed.ptr != code_text.data() + code_text.size()) {
        assign_error(
            error,
            result.empty() ? "FFmpegKit returned no completion status" : result
        );
        return false;
    }
    diagnostic_output = newline == std::string::npos
        ? std::string{}
        : result.substr(newline + 1U);
    if (error != nullptr) error->clear();
    return true;
#else
    static_cast<void>(session_id);
    static_cast<void>(exit_code);
    static_cast<void>(diagnostic_output);
    assign_error(error, "embedded FFmpegKit is available only on Android");
    return false;
#endif
}

void cancel_android_ffmpeg(const std::int64_t session_id) noexcept {
#if defined(__ANDROID__)
    if (session_id < 0) return;
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) return;
    const auto owner = activity(env);
    const auto call = method(
        env,
        owner,
        "cancelPulseForgeFfmpeg",
        "(J)V"
    );
    if (call == nullptr) return;
    env->CallVoidMethod(owner, call, static_cast<jlong>(session_id));
    static_cast<void>(clear_exception(env));
#else
    static_cast<void>(session_id);
#endif
}

}  // namespace pulseforge::detail
