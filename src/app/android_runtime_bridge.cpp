#include "android_runtime_bridge.hpp"

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
        valid_ = env_ != nullptr && env_->PushLocalFrame(24) == JNI_OK;
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
#endif

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {
        reinterpret_cast<const char*>(value.data()),
        value.size(),
    };
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

}  // namespace pulseforge::detail
