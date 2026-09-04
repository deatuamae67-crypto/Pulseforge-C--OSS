#pragma once

#include "pulseforge/application.hpp"

#include <cstdint>
#include <memory>

namespace pulseforge {

// Observable states of the desktop runtime. A CoreEngine instance is
// deliberately single-shot: construct one engine per gameplay/application run.
enum class CoreEngineState : std::uint8_t {
    ready,
    running,
    stopped,
    failed,
};

// Public ownership boundary for the SDL/audio/Lua application runtime.
//
// Construction is lightweight; charts, devices and the window are initialized
// lazily by run() on the calling thread. Destruction performs deterministic
// teardown through the private runtime implementation.
class CoreEngine final {
public:
    explicit CoreEngine(AppLaunchOptions options);
    ~CoreEngine();

    CoreEngine(const CoreEngine&) = delete;
    CoreEngine& operator=(const CoreEngine&) = delete;
    CoreEngine(CoreEngine&&) = delete;
    CoreEngine& operator=(CoreEngine&&) = delete;

    // Starts the platform runtime and blocks until the application exits.
    // Must be called exactly once, from the platform main thread.
    [[nodiscard]] int run();

    [[nodiscard]] CoreEngineState state() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pulseforge
