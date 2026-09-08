#include "pulseforge/CoreEngine.h"

#include "application_runner.hpp"
#include "complete_content_gate.hpp"

#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace pulseforge {

class CoreEngine::Impl final {
public:
    explicit Impl(AppLaunchOptions options)
        : options_(std::move(options)) {}

    [[nodiscard]] int run() {
        if (state_ != CoreEngineState::ready) {
            throw std::logic_error("CoreEngine::run() may only be called once");
        }

        state_ = CoreEngineState::running;
        try {
            // Complete releases may ship a lightweight Drive manifest instead
            // of embedding the heavy mod corpus. The gate is optional and
            // fail-open; ordinary OSS/dev builds without that manifest follow
            // the exact previous startup path.
            detail::run_complete_content_gate(options_);
            runner_ = detail::make_application(std::move(options_));
            if (runner_ == nullptr) {
                throw std::runtime_error("failed to create the application runtime");
            }
            const int exit_code = runner_->run();
            // Tear down window, devices, audio and Lua before returning to the
            // embedding application, rather than waiting for CoreEngine's
            // destructor. For a gameplay runner this reset also completes any
            // requested bidirectional SDL platform handoff.
            runner_.reset();
            state_ = exit_code == EXIT_SUCCESS
                ? CoreEngineState::stopped
                : CoreEngineState::failed;
            return exit_code;
        } catch (...) {
            state_ = CoreEngineState::failed;
            runner_.reset();
            throw;
        }
    }

    [[nodiscard]] CoreEngineState state() const noexcept {
        return state_;
    }

private:
    AppLaunchOptions options_;
    std::unique_ptr<detail::ApplicationRunner> runner_;
    CoreEngineState state_{CoreEngineState::ready};
};

CoreEngine::CoreEngine(AppLaunchOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

CoreEngine::~CoreEngine() = default;

int CoreEngine::run() {
    return impl_->run();
}

CoreEngineState CoreEngine::state() const noexcept {
    return impl_->state();
}

int run_application(const AppLaunchOptions& options) {
    CoreEngine engine(options);
    return engine.run();
}

}  // namespace pulseforge
