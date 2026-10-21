#include "Hepch.h"
#include "Plugins/OfflineReferenceRenderer/Source/ProgressiveDenoiser.h"

#if HE_OFFLINE_OIDN
#include "Runtime/Platform/Interop/OidnDenoiser.h"
#endif

#include <chrono>
#include <stdexcept>

namespace HEngine
{
    struct ProgressiveDenoiser::Backend
    {
#if HE_OFFLINE_OIDN
        std::unique_ptr<OidnDenoiser> Denoiser;
        void Run(DenoiseSnapshot& snapshot)
        {
            const size_t size = size_t(snapshot.Width) * snapshot.Height * 4;
            if (snapshot.Color.size() != size || snapshot.Albedo.size() != size || snapshot.Normal.size() != size)
                throw std::invalid_argument("OIDN snapshot dimensions do not match its inputs");
            snapshot.Output.resize(size, 1.f);
            if (!Denoiser) Denoiser = std::make_unique<OidnDenoiser>();
            Denoiser->Run(snapshot.Width, snapshot.Height, snapshot.Color, snapshot.Albedo, snapshot.Normal, snapshot.Output);
        }
#endif
    };
    ProgressiveDenoiser::ProgressiveDenoiser() : mBackend(std::make_shared<Backend>()) {}
    ProgressiveDenoiser::~ProgressiveDenoiser() = default;
    bool ProgressiveDenoiser::Available() noexcept { return HE_OFFLINE_OIDN != 0; }
    bool ProgressiveDenoiser::Busy() const noexcept { return mFuture.valid(); }
    bool ProgressiveDenoiser::Submit(DenoiseSnapshot snapshot)
    {
        if (Busy() || !Available()) return false;
        mFuture = std::async(std::launch::async, [backend = mBackend, snapshot = std::move(snapshot)]() mutable
        {
            const auto started = std::chrono::steady_clock::now();
            try
            {
#if HE_OFFLINE_OIDN
                backend->Run(snapshot);
#endif
            }
            catch (const std::exception& error) { snapshot.Error = error.what(); }
            snapshot.Milliseconds = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            return snapshot;
        });
        return true;
    }
    std::unique_ptr<DenoiseSnapshot> ProgressiveDenoiser::Poll()
    {
        if (!mFuture.valid() || mFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return {};
        return std::make_unique<DenoiseSnapshot>(mFuture.get());
    }
}
