#pragma once

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace HEngine
{
    // Immutable CPU snapshot: the worker never touches the renderer, ECS or GPU.
    struct DenoiseSnapshot
    {
        uint64_t Generation = 0;
        uint64_t Samples = 0;
        uint32_t Width = 0, Height = 0;
        std::vector<float> Color, Albedo, Normal, Output;
        double Milliseconds = 0;
        std::string Error;
    };

    class ProgressiveDenoiser
    {
    public:
        ProgressiveDenoiser();
        ~ProgressiveDenoiser();
        [[nodiscard]] static bool Available() noexcept;
        [[nodiscard]] bool Busy() const noexcept;
        bool Submit(DenoiseSnapshot snapshot);
        [[nodiscard]] std::unique_ptr<DenoiseSnapshot> Poll();
    private:
        struct Backend;
        std::shared_ptr<Backend> mBackend;
        std::future<DenoiseSnapshot> mFuture;
    };
}
