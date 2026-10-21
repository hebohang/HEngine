#pragma once

#include <cstdint>
#include <memory>
#include <span>

namespace HEngine
{
    // Built only by the offline plugin. Vendor/native types stay in Platform.
    class OidnDenoiser final
    {
    public:
        OidnDenoiser();
        ~OidnDenoiser();
        OidnDenoiser(const OidnDenoiser&) = delete;
        OidnDenoiser& operator=(const OidnDenoiser&) = delete;
        void Run(uint32_t width, uint32_t height, std::span<float> color,
            std::span<float> albedo, std::span<float> normal, std::span<float> output);
    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
