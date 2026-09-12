#include "android_runtime_bridge.hpp"

#include <string>
#include <utility>

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
        valid_ = env_ != nullptr && env_->PushLocalFrame(96) == JNI_OK;
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

[[nodiscard]] std::string java_string(JNIEnv* const env, jstring const value) {
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
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

void assign_error(std::string* const error, std::string value) {
    if (error != nullptr) *error = std::move(value);
}

}  // namespace

std::string android_runtime_diagnostics() {
#if defined(__ANDROID__)
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) return {};
    const auto owner = activity(env);
    if (owner == nullptr) return {};
    const auto call = method(env, owner, "getPulseForgeRuntimeStats", "()Ljava/lang/String;");
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
        env, owner, "publishPulseForgeDownload",
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
        || clear_exception(env)) return {};
    const auto value = static_cast<jstring>(env->CallObjectMethod(
        owner, call, java_source, java_name, java_mime
    ));
    if (clear_exception(env)) return {};
    return java_string(env, value);
#else
    static_cast<void>(display_name);
    static_cast<void>(mime_type);
    return path_utf8(source);
#endif
}

bool android_ffmpeg_available() noexcept {
#if defined(__ANDROID__)
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) return false;
    const auto owner = activity(env);
    return owner != nullptr
        && method(env, owner, "createPulseForgeFfmpegPipe", "()Ljava/lang/String;") != nullptr
        && method(env, owner, "startPulseForgeFfmpeg", "([Ljava/lang/String;)J") != nullptr;
#else
    return false;
#endif
}

AndroidFfmpegSession start_android_ffmpeg(
    const std::span<const std::string> arguments,
    std::string* const error
) {
#if defined(__ANDROID__)
    AndroidFfmpegSession result;
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) {
        assign_error(error, "Android JNI environment is unavailable");
        return result;
    }
    const auto owner = activity(env);
    if (owner == nullptr) {
        assign_error(error, "Android SDL activity is unavailable");
        return result;
    }
    const auto create_pipe = method(env, owner, "createPulseForgeFfmpegPipe", "()Ljava/lang/String;");
    const auto start = method(env, owner, "startPulseForgeFfmpeg", "([Ljava/lang/String;)J");
    if (create_pipe == nullptr || start == nullptr) {
        assign_error(error, "Android FFmpegKit bridge is unavailable");
        return result;
    }
    const auto pipe_value = static_cast<jstring>(env->CallObjectMethod(owner, create_pipe));
    if (clear_exception(env) || pipe_value == nullptr) {
        assign_error(error, "Android FFmpegKit could not create its raw-video pipe");
        return result;
    }
    result.input_pipe = java_string(env, pipe_value);
    if (result.input_pipe.empty()) {
        assign_error(error, "Android FFmpegKit returned an empty raw-video pipe");
        return result;
    }

    jclass string_class = env->FindClass("java/lang/String");
    if (string_class == nullptr || clear_exception(env)) {
        assign_error(error, "Android JNI could not resolve java.lang.String");
        close_android_ffmpeg_pipe(result.input_pipe);
        result.input_pipe.clear();
        return result;
    }
    const auto offset = !arguments.empty() ? std::size_t{1U} : std::size_t{0U};
    const auto count = arguments.size() - offset;
    auto array = env->NewObjectArray(static_cast<jsize>(count), string_class, nullptr);
    if (array == nullptr || clear_exception(env)) {
        assign_error(error, "Android JNI could not allocate FFmpeg arguments");
        close_android_ffmpeg_pipe(result.input_pipe);
        result.input_pipe.clear();
        return result;
    }
    for (std::size_t index = 0U; index < count; ++index) {
        std::string value = arguments[index + offset];
        if (value == "pipe:0") value = result.input_pipe;
        const auto java_value = env->NewStringUTF(value.c_str());
        if (java_value == nullptr || clear_exception(env)) {
            assign_error(error, "Android JNI could not encode an FFmpeg argument");
            close_android_ffmpeg_pipe(result.input_pipe);
            result.input_pipe.clear();
            return result;
        }
        env->SetObjectArrayElement(array, static_cast<jsize>(index), java_value);
        if (clear_exception(env)) {
            assign_error(error, "Android JNI could not populate FFmpeg arguments");
            close_android_ffmpeg_pipe(result.input_pipe);
            result.input_pipe.clear();
            return result;
        }
    }
    result.session_id = static_cast<std::int64_t>(env->CallLongMethod(owner, start, array));
    if (clear_exception(env) || result.session_id < 0) {
        assign_error(error, "Android FFmpegKit failed to start the encoding session");
        close_android_ffmpeg_pipe(result.input_pipe);
        result.input_pipe.clear();
        result.session_id = -1;
        return result;
    }
    if (error != nullptr) error->clear();
    return result;
#else
    static_cast<void>(arguments);
    assign_error(error, "Android FFmpegKit is unavailable on this platform");
    return {};
#endif
}

int wait_android_ffmpeg(const std::int64_t session_id, std::string* const output) {
#if defined(__ANDROID__)
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) return -32010;
    const auto owner = activity(env);
    if (owner == nullptr) return -32011;
    const auto wait_call = method(env, owner, "waitPulseForgeFfmpeg", "(J)I");
    const auto output_call = method(env, owner, "getPulseForgeFfmpegOutput", "(J)Ljava/lang/String;");
    if (wait_call == nullptr) return -32012;
    const int code = static_cast<int>(env->CallIntMethod(owner, wait_call, static_cast<jlong>(session_id)));
    if (clear_exception(env)) return -32013;
    if (output != nullptr && output_call != nullptr) {
        const auto value = static_cast<jstring>(env->CallObjectMethod(
            owner, output_call, static_cast<jlong>(session_id)
        ));
        if (!clear_exception(env)) *output = java_string(env, value);
    }
    return code;
#else
    static_cast<void>(session_id);
    if (output != nullptr) output->clear();
    return -32014;
#endif
}

void cancel_android_ffmpeg(const std::int64_t session_id) noexcept {
#if defined(__ANDROID__)
    if (session_id < 0) return;
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) return;
    const auto owner = activity(env);
    if (owner == nullptr) return;
    const auto call = method(env, owner, "cancelPulseForgeFfmpeg", "(J)V");
    if (call == nullptr) return;
    env->CallVoidMethod(owner, call, static_cast<jlong>(session_id));
    static_cast<void>(clear_exception(env));
#else
    static_cast<void>(session_id);
#endif
}

void close_android_ffmpeg_pipe(const std::string_view path) noexcept {
#if defined(__ANDROID__)
    if (path.empty()) return;
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) return;
    const auto owner = activity(env);
    if (owner == nullptr) return;
    const auto call = method(env, owner, "closePulseForgeFfmpegPipe", "(Ljava/lang/String;)V");
    if (call == nullptr) return;
    const std::string copy(path);
    const auto java_path = env->NewStringUTF(copy.c_str());
    if (java_path == nullptr || clear_exception(env)) return;
    env->CallVoidMethod(owner, call, java_path);
    static_cast<void>(clear_exception(env));
#else
    static_cast<void>(path);
#endif
}

}  // namespace pulseforge::detail
