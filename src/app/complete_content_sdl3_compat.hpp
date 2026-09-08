#pragma once

// SDL3 tightened several platform-handle APIs to use explicit pointer types.
// Keep those conversions at the Complete runtime boundary instead of weakening
// compiler diagnostics or spreading casts through the transport implementation.

#include <SDL3/SDL.h>

#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif

namespace pulseforge::detail::complete_sdl3_compat {

#if !defined(_WIN32) && !defined(__ANDROID__)
[[nodiscard]] inline void* load_function(void* const object, const char* const name) {
    return SDL_LoadFunction(static_cast<SDL_SharedObject*>(object), name);
}

inline void unload_object(void* const object) {
    SDL_UnloadObject(static_cast<SDL_SharedObject*>(object));
}
#endif

#if defined(__ANDROID__)
[[nodiscard]] inline jobject android_activity() {
    return static_cast<jobject>(SDL_GetAndroidActivity());
}
#endif

}  // namespace pulseforge::detail::complete_sdl3_compat

#if !defined(_WIN32) && !defined(__ANDROID__)
#define SDL_LoadFunction(object, name) \
    ::pulseforge::detail::complete_sdl3_compat::load_function((object), (name))
#define SDL_UnloadObject(object) \
    ::pulseforge::detail::complete_sdl3_compat::unload_object((object))
#endif

#if defined(__ANDROID__)
#define SDL_GetAndroidActivity() \
    ::pulseforge::detail::complete_sdl3_compat::android_activity()
#endif

#include "complete_content_gate.hpp"

#if defined(__ANDROID__)
#undef SDL_GetAndroidActivity
#endif

#if !defined(_WIN32) && !defined(__ANDROID__)
#undef SDL_UnloadObject
#undef SDL_LoadFunction
#endif
