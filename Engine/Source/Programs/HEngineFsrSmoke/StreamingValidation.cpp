#include "StreamingValidation.h"

#include "Runtime/Graphics/RHI/TextureManager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
    void Require(bool success, const char* message)
    {
        if (!success) throw std::runtime_error(message);
    }

    void WriteDds(const std::filesystem::path& path, uint32_t dimension)
    {
        std::array<unsigned char, 148> header{};
        const auto write = [&](size_t offset, uint32_t value)
        {
            for (size_t index = 0; index < 4; ++index)
                header[offset + index] = static_cast<unsigned char>(value >> (index * 8));
        };
        uint32_t mipCount = 1;
        for (auto size = dimension; size > 1; size >>= 1) ++mipCount;
        write(0, 0x20534444); write(4, 124); write(8, 0xa1007);
        write(12, dimension); write(16, dimension); write(20, dimension * dimension);
        write(28, mipCount); write(76, 32); write(80, 4); write(84, 0x30315844);
        write(108, 0x401008); write(128, 98); write(132, 3); write(140, 1); write(144, 1);
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(header.data()), header.size());
        // Valid constant BC7 mode-6 blocks; no source asset or decoder dependency.
        std::array<unsigned char, 64 * 1024> blocks{};
        for (size_t index = 0; index < blocks.size(); index += 16) blocks[index] = 0x40;
        for (uint32_t mip = 0; mip < mipCount; ++mip)
        {
            uint64_t remaining = uint64_t((dimension + 3) / 4) * ((dimension + 3) / 4) * 16;
            while (remaining)
            {
                const auto count = static_cast<std::streamsize>(std::min<uint64_t>(remaining, blocks.size()));
                stream.write(reinterpret_cast<const char*>(blocks.data()), count);
                remaining -= count;
            }
            dimension = std::max(1u, dimension >> 1);
        }
        Require(bool(stream), "Failed to write streaming DDS fixture");
    }

    void WriteTga(const std::filesystem::path& path)
    {
        std::array<unsigned char, 18> header{};
        header[2] = 2; header[12] = 64; header[14] = 64;
        header[16] = 32; header[17] = 0x28;
        std::vector<unsigned char> pixels(64 * 64 * 4, 128);
        for (size_t index = 3; index < pixels.size(); index += 4) pixels[index] = 255;
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(header.data()), header.size());
        stream.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
        Require(bool(stream), "Failed to write streaming image fixture");
    }

    template<class Step, class Condition>
    void Until(Step&& step, Condition&& condition, const char* failure)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do
        {
            step();
            if (condition()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error(failure);
    }
}

void VerifyTextureStreaming(nvrhi::IDevice* device, HEngine::RenderBackend backend,
    const std::filesystem::path& evidence)
{
    using namespace HEngine;
    const auto root = evidence / "Streaming" / (backend == RenderBackend::DX12 ? "DX12" : "Vulkan");
    std::filesystem::create_directories(root);
    WriteDds(root / "large.dds", 8192);
    WriteDds(root / "cancel.dds", 8192);
    const auto access = backend == RenderBackend::DX12
        ? ShaderDescriptorAccess::HeapDirectlyIndexed : ShaderDescriptorAccess::BoundedTable;
    TextureManager manager({.Device = device, .AssetRootDirectory = root,
        .Bindless = {.Textures = {.Capacity = 16, .Access = access},
            .Samplers = {.Capacity = 4, .Access = access}}});
    const auto baseline = manager.GetResidentTexturePayloadBytes();
    const auto collect = [&] { manager.Collect(0, 0); device->runGarbageCollection(); };
    const auto queueMarker = [&]
    {
        auto commands = device->createCommandList();
        Require(bool(commands), "Failed to create queue marker");
        commands->open(); commands->close();
        const auto submission = device->executeCommandList(commands);
        Require(submission != 0, "Queue marker submission failed");
        return submission;
    };
    TextureLoadDesc desc;
    desc.AssetPath = "large.dds";
    desc.bStreamable = true;
    auto handle = manager.LoadTexture(desc);
    const auto same = manager.LoadTexture(desc);
    Require(handle == same, "Repeated streaming load did not share a managed handle");
    manager.ReleaseTexture(same, 0);
    Until(collect, [&] { return manager.GetUploadStage(handle) == TextureUploadStage::Ready; },
        "Large initial upload did not become ready");
    const auto fullBytes = manager.GetTexture(handle).PayloadBytes;
    Require(fullBytes > 64ull * 1024 * 1024 && manager.GetTexture(handle).Width == 8192,
        "Fixture did not upload a resource larger than 64 MiB");
    const auto residencyStep = [&](uint64_t budget)
    {
        collect();
        manager.BeginStreamingFrame();
        manager.RequestTextureDetail(handle, 8192);
        manager.EndStreamingFrame(budget);
    };
    Until([&] { residencyStep(8ull * 1024 * 1024); }, [&]
    {
        return manager.GetUploadStage(handle) == TextureUploadStage::Ready &&
            manager.GetTexture(handle).FirstResidentMip > 0 &&
            manager.GetResidentTexturePayloadBytes() <= baseline + 8ull * 1024 * 1024;
    }, "Large texture did not demote under pressure");
    const auto demotedBytes = manager.GetTexture(handle).PayloadBytes;
    Until([&] { residencyStep(128ull * 1024 * 1024); }, [&]
    {
        return manager.GetUploadStage(handle) == TextureUploadStage::Ready &&
            manager.GetTexture(handle).FirstResidentMip == 0 &&
            manager.GetTexture(handle).PayloadBytes == fullBytes;
    }, "Texture larger than scratch headroom could not recover");

    // Corrupt replacement must not destroy the already published resource.
    const auto* published = manager.GetTexture(handle).Texture.Get();
    const auto revision = manager.GetRevision();
    { std::ofstream broken(root / "large.dds", std::ios::binary | std::ios::trunc); broken << "broken"; }
    Require(manager.RequestReload("large.dds") == 1, "Live hot reload was not queued");
    Until(collect, [&] { return manager.GetUploadStage(handle) == TextureUploadStage::Failed; },
        "Corrupt hot reload did not report failure");
    Require(manager.GetTexture(handle).Texture.Get() == published &&
        manager.GetTexture(handle).PayloadBytes == fullBytes && manager.GetRevision() == revision,
        "Failed reload replaced the visible resource or advanced its publication revision");
    WriteDds(root / "large.dds", 4096);
    Require(manager.InvalidateFailedLoads("large.dds") == 1, "Failed asset did not invalidate for retry");
    Until(collect, [&] { return manager.GetUploadStage(handle) == TextureUploadStage::Ready; },
        "Repaired hot reload failed to recover");
    Require(manager.GetTexture(handle).SourceWidth == 4096 && manager.GetTexture(handle).Width == 4096 &&
        manager.GetTexture(handle).Texture.Get() != published && manager.GetRevision() > revision,
        "Hot reload did not publish changed dimensions on the original managed handle");

    desc.AssetPath = "cancel.dds";
    const auto cancelled = manager.LoadTexture(desc);
    Require(manager.GetUploadStage(cancelled) == TextureUploadStage::Preparing,
        "Cancellation fixture did not enter preparation");
    manager.ReleaseTexture(cancelled, 0);
    Require(!manager.IsValid(cancelled), "Cancelled preparation left a valid handle");
    collect();
    const auto uploading = manager.LoadTexture(desc);
    Require(uploading != cancelled, "Cancelled slot was reused without a new generation");
    Until(collect, [&] { return manager.GetUploadStage(uploading) == TextureUploadStage::Submitted; },
        "Could not observe the real pending GPU upload for cancellation");
    manager.ReleaseTexture(uploading, 0);
    Require(!manager.IsValid(uploading), "Cancelled upload left a valid handle");
    Until(collect, [&] { return manager.GetUploadStatistics().Pending == 0; },
        "Cancelled GPU upload was not retired after its completion query");
    const auto remaining = manager.GetTexture(handle).PayloadBytes;
    Require(manager.GetResidentTexturePayloadBytes() == baseline + remaining,
        "Cancelled GPU upload retained accounted payload");
    Require(manager.RequestReload("large.dds") == 1, "Reload cancellation fixture did not queue");
    manager.ReleaseTexture(handle, 0);
    Until(collect, [&] { return manager.GetUploadStatistics().Pending == 0 &&
        manager.GetUploadStatistics().PreparedBytes == 0 &&
        manager.GetResidentTexturePayloadBytes() == baseline; },
        "Cancelled live reload leaked prepared or resident payload");
    Require(manager.GetResidentTextureCount() == 3 && !manager.IsValid(handle),
        "Released fixtures did not return to defaults-only residency");
    // Actual open failure, followed by a newly imported asset at the same path.
    desc.AssetPath = "added-later-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".dds";
    const auto missing = manager.LoadTexture(desc);
    Until(collect, [&] { return manager.GetUploadStage(missing) == TextureUploadStage::Failed; },
        "Missing source did not report an IO failure");
    Require(manager.GetTexture(missing).Texture == manager.GetDefaultTexture(DefaultBindlessTexture::White).Texture,
        "Missing asset did not retain its default fallback");
    WriteDds(root / desc.AssetPath, 4);
    Require(manager.InvalidateFailedLoads(desc.AssetPath) == 1, "Hot-added file did not clear its cached failure");
    Until(collect, [&] { return manager.GetUploadStage(missing) == TextureUploadStage::Ready; },
        "Hot-added asset did not recover on its existing handle");
    Require(manager.GetTexture(missing).Format == nvrhi::Format::BC7_UNORM,
        "Hot-added asset remained on the fallback resource");
    manager.ReleaseTexture(missing, 0);
    collect();
    Require(manager.GetResidentTexturePayloadBytes() == baseline && manager.GetResidentTextureCount() == 3,
        "Hot-added asset did not return to defaults-only residency");

    // Exercise both upload implementations at allocation, command construction,
    // tracking allocation and local submission rejection boundaries. No simulated
    // point claims to reproduce a driver/device-loss recovery.
    WriteDds(root / "fault.dds", 64);
    WriteTga(root / "fault.tga");
    uint64_t injectedFailures = 0;
    for (const auto* asset : {"fault.dds", "fault.tga"})
    {
        desc.AssetPath = asset;
        for (const auto failure : { TextureUploadFailure::TextureAllocation,
            TextureUploadFailure::CommandListCreation, TextureUploadFailure::CompletionQueryCreation,
            TextureUploadFailure::BeforeSubmission })
        {
            auto faulted = manager.LoadTexture(desc);
            for (bool reload : {false, true})
            {
                if (reload) Require(manager.RequestReload(asset) == 1, "GPU-failure reload was not queued");
                const auto visible = manager.GetTexture(faulted); // retain old resource across assertions
                const auto beforeRevision = manager.GetRevision();
                const auto beforeBytes = manager.GetResidentTexturePayloadBytes();
                const auto beforeSubmitted = manager.GetUploadStatistics().SubmittedBytes;
                const auto beforeQueue = queueMarker();
                Require(manager.InjectUploadFailureForValidation(faulted, failure), "Could not arm upload fault");
                Require(!manager.InjectUploadFailureForValidation(faulted, failure), "Overwrote a pending upload fault");
                Until(collect, [&] { return manager.GetUploadStage(faulted) == TextureUploadStage::Failed; },
                    "Injected upload did not fail");
                // This owned smoke device has no other queue submitter. Consecutive
                // marker IDs prove no hidden upload occurred before failure.
                Require(queueMarker() == beforeQueue + 1, "Failed upload submitted work without completion tracking");
                ++injectedFailures;
                const auto& failed = manager.GetTexture(faulted);
                const auto failureStats = manager.GetUploadStatistics();
                Require(failed.Texture == visible.Texture && failed.PayloadBytes == visible.PayloadBytes &&
                    failed.ShaderDescriptorIndex == visible.ShaderDescriptorIndex &&
                    manager.GetRevision() == beforeRevision, "Upload failure altered visible texture/descriptor/revision");
                Require(failureStats.Pending == 0 && failureStats.PreparedBytes == 0 &&
                    failureStats.PendingBytes == 0 && failureStats.SubmittedBytes == beforeSubmitted &&
                    manager.GetResidentTexturePayloadBytes() == beforeBytes,
                    "Rejected upload retained prepared/pending payload or was counted as submitted");
                Require(manager.InvalidateFailedLoads(asset) == 1, "Upload failure did not invalidate for retry");
                Until(collect, [&] { return manager.GetUploadStage(faulted) == TextureUploadStage::Ready; },
                    "Upload retry did not recover");
                Require(manager.GetTexture(faulted).Width == 64 && manager.GetRevision() > beforeRevision &&
                    manager.GetTexture(faulted).Texture != visible.Texture,
                    "Retry did not publish the uploaded texture on its original handle");
                collect();
            }
            manager.ReleaseTexture(faulted, 0);
            collect();
            Require(manager.GetResidentTexturePayloadBytes() == baseline && manager.GetResidentTextureCount() == 3,
                "GPU-failure/retry cycle retained manager resources");
        }
        // Ready consumers share the same query-before-submit path.
        auto readyDesc = desc;
        readyDesc.bStreamable = false; readyDesc.bRequireReady = true;
        const auto ready = manager.LoadTexture(readyDesc);
        Require(manager.GetUploadStage(ready) == TextureUploadStage::Ready && manager.GetTexture(ready).Width == 64,
            "Synchronous ready upload did not publish");
        manager.ReleaseTexture(ready, 0);
        collect();
    }
    const auto stats = manager.GetUploadStatistics();
    Require(stats.Failed == 2 + injectedFailures && injectedFailures == 16, "Unexpected streaming upload failure count");
    Require(manager.GetResidentTexturePayloadBytes() == baseline && manager.GetResidentTextureCount() == 3,
        "Upload fault validation did not restore defaults-only residency");
    Require(stats.GlobalWaits == 1, "Streaming introduced a global idle wait beyond default initialization");
    std::ofstream report(root / "result.json");
    report << "{\"status\":\"PASS\",\"fullBytes\":" << fullBytes << ",\"demotedBytes\":" << demotedBytes
        << ",\"reloadedBytes\":" << remaining << ",\"expectedFailures\":" << stats.Failed
        << ",\"transitions\":" << stats.StreamingTransitions << ",\"finalPayloadBytes\":" << baseline
        << ",\"cancelPreparing\":true,\"cancelSubmitted\":true,\"cancelReload\":true,\"missingFileRetry\":true"
        << ",\"injectedUploadFailures\":" << injectedFailures
        << ",\"queueGapChecks\":" << injectedFailures << ",\"synchronousReadyPaths\":2}\n";
    Require(bool(report), "Could not save streaming verification report");
    std::cout << "TEXTURE_STREAMING_PASS: fullBytes=" << fullBytes << " demotedBytes=" << demotedBytes
        << " recovered >64MiB; corrupt reload and missing-file retry, preparation/upload/reload cancellation; "
        << injectedFailures << " injected GPU-boundary failures recovered\n";
}
