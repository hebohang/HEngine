#include "Hepch.h"

#include "PipelineCache.h"

#include <array>
#include <bit>
#include <cstddef>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace HEngine
{
    namespace
    {
        class FingerprintBuilder final
        {
        public:
            template<typename T>
            void Add(T value) noexcept
                requires std::is_trivially_copyable_v<T>
            {
                const auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
                for (const std::byte byte : bytes)
                {
                    mValue ^= static_cast<uint8_t>(byte);
                    mValue *= 1099511628211ull;
                }
            }

            template<typename T>
            void AddPointer(const T* value) noexcept
            {
                Add(reinterpret_cast<uintptr_t>(value));
            }

            [[nodiscard]] uint64_t Get() const noexcept { return mValue; }

        private:
            uint64_t mValue = 14695981039346656037ull;
        };

        void AddStencilState(
            FingerprintBuilder& fingerprint,
            const nvrhi::DepthStencilState::StencilOpDesc& desc)
        {
            fingerprint.Add(desc.failOp);
            fingerprint.Add(desc.depthFailOp);
            fingerprint.Add(desc.passOp);
            fingerprint.Add(desc.stencilFunc);
        }

        void AddRenderState(FingerprintBuilder& fingerprint, const nvrhi::RenderState& state)
        {
            fingerprint.Add(state.blendState.alphaToCoverageEnable);
            for (const nvrhi::BlendState::RenderTarget& target : state.blendState.targets)
            {
                fingerprint.Add(target.blendEnable);
                fingerprint.Add(target.srcBlend);
                fingerprint.Add(target.destBlend);
                fingerprint.Add(target.blendOp);
                fingerprint.Add(target.srcBlendAlpha);
                fingerprint.Add(target.destBlendAlpha);
                fingerprint.Add(target.blendOpAlpha);
                fingerprint.Add(target.colorWriteMask);
            }

            const nvrhi::DepthStencilState& depth = state.depthStencilState;
            fingerprint.Add(depth.depthTestEnable);
            fingerprint.Add(depth.depthWriteEnable);
            fingerprint.Add(depth.depthFunc);
            fingerprint.Add(depth.stencilEnable);
            fingerprint.Add(depth.stencilReadMask);
            fingerprint.Add(depth.stencilWriteMask);
            fingerprint.Add(depth.stencilRefValue);
            fingerprint.Add(depth.dynamicStencilRef);
            AddStencilState(fingerprint, depth.frontFaceStencil);
            AddStencilState(fingerprint, depth.backFaceStencil);

            const nvrhi::RasterState& raster = state.rasterState;
            fingerprint.Add(raster.fillMode);
            fingerprint.Add(raster.cullMode);
            fingerprint.Add(raster.frontCounterClockwise);
            fingerprint.Add(raster.depthClipEnable);
            fingerprint.Add(raster.scissorEnable);
            fingerprint.Add(raster.multisampleEnable);
            fingerprint.Add(raster.antialiasedLineEnable);
            fingerprint.Add(raster.depthBias);
            fingerprint.Add(raster.depthBiasClamp);
            fingerprint.Add(raster.slopeScaledDepthBias);
            fingerprint.Add(raster.forcedSampleCount);
            fingerprint.Add(raster.programmableSamplePositionsEnable);
            fingerprint.Add(raster.conservativeRasterEnable);
            fingerprint.Add(raster.quadFillEnable);
            for (const char position : raster.samplePositionsX)
                fingerprint.Add(position);
            for (const char position : raster.samplePositionsY)
                fingerprint.Add(position);

            fingerprint.Add(state.singlePassStereo.enabled);
            fingerprint.Add(state.singlePassStereo.independentViewportMask);
            fingerprint.Add(state.singlePassStereo.renderTargetIndexOffset);
        }

        void AddFramebufferInfo(
            FingerprintBuilder& fingerprint,
            const nvrhi::FramebufferInfo& framebufferInfo)
        {
            fingerprint.Add(framebufferInfo.colorFormats.size());
            for (const nvrhi::Format format : framebufferInfo.colorFormats)
                fingerprint.Add(format);
            fingerprint.Add(framebufferInfo.depthFormat);
            fingerprint.Add(framebufferInfo.sampleCount);
            fingerprint.Add(framebufferInfo.sampleQuality);
        }

        uint64_t Fingerprint(const nvrhi::BindingLayoutDesc& desc)
        {
            FingerprintBuilder fingerprint;
            fingerprint.Add(uint32_t{ 0x4c41594f });
            fingerprint.Add(desc.visibility);
            fingerprint.Add(desc.registerSpace);
            fingerprint.Add(desc.registerSpaceIsDescriptorSet);
            fingerprint.Add(desc.bindingOffsets.shaderResource);
            fingerprint.Add(desc.bindingOffsets.sampler);
            fingerprint.Add(desc.bindingOffsets.constantBuffer);
            fingerprint.Add(desc.bindingOffsets.unorderedAccess);
            fingerprint.Add(desc.bindings.size());
            for (const nvrhi::BindingLayoutItem& binding : desc.bindings)
            {
                fingerprint.Add(binding.slot);
                fingerprint.Add(binding.type);
                fingerprint.Add(binding.size);
            }
            return fingerprint.Get();
        }

        uint64_t Fingerprint(const nvrhi::BindlessLayoutDesc& desc)
        {
            FingerprintBuilder fingerprint;
            fingerprint.Add(uint32_t{ 0x42494e44 });
            fingerprint.Add(desc.visibility);
            fingerprint.Add(desc.firstSlot);
            fingerprint.Add(desc.maxCapacity);
            fingerprint.Add(desc.layoutType);
            fingerprint.Add(desc.registerSpaces.size());
            for (const nvrhi::BindingLayoutItem& binding : desc.registerSpaces)
            {
                fingerprint.Add(binding.slot);
                fingerprint.Add(binding.type);
                fingerprint.Add(binding.size);
            }
            return fingerprint.Get();
        }

        uint64_t Fingerprint(
            const nvrhi::BindingSetDesc& desc,
            const nvrhi::BindingLayoutHandle& layout)
        {
            FingerprintBuilder fingerprint;
            fingerprint.Add(uint32_t{ 0x53455421 });
            fingerprint.AddPointer(layout.Get());
            fingerprint.Add(desc.trackLiveness);
            fingerprint.Add(desc.bindings.size());
            for (const nvrhi::BindingSetItem& binding : desc.bindings)
            {
                fingerprint.AddPointer(binding.resourceHandle);
                fingerprint.Add(binding.slot);
                fingerprint.Add(binding.arrayElement);
                fingerprint.Add(binding.type);
                fingerprint.Add(binding.dimension);
                fingerprint.Add(binding.format);
                fingerprint.Add(binding.rawData[0]);
                fingerprint.Add(binding.rawData[1]);
            }
            return fingerprint.Get();
        }

        uint64_t Fingerprint(
            const nvrhi::GraphicsPipelineDesc& desc,
            const nvrhi::FramebufferInfo& framebufferInfo)
        {
            FingerprintBuilder fingerprint;
            fingerprint.Add(uint32_t{ 0x4750534f });
            fingerprint.Add(desc.primType);
            fingerprint.Add(desc.patchControlPoints);
            fingerprint.AddPointer(desc.inputLayout.Get());
            fingerprint.AddPointer(desc.VS.Get());
            fingerprint.AddPointer(desc.HS.Get());
            fingerprint.AddPointer(desc.DS.Get());
            fingerprint.AddPointer(desc.GS.Get());
            fingerprint.AddPointer(desc.PS.Get());
            AddRenderState(fingerprint, desc.renderState);
            fingerprint.Add(desc.shadingRateState.enabled);
            fingerprint.Add(desc.shadingRateState.shadingRate);
            fingerprint.Add(desc.shadingRateState.pipelinePrimitiveCombiner);
            fingerprint.Add(desc.shadingRateState.imageCombiner);
            fingerprint.Add(desc.bindingLayouts.size());
            for (const nvrhi::BindingLayoutHandle& layout : desc.bindingLayouts)
                fingerprint.AddPointer(layout.Get());
            AddFramebufferInfo(fingerprint, framebufferInfo);
            return fingerprint.Get();
        }

        [[nodiscard]] GraphicsPipelineKey ResolveFramebufferSampleVariant(
            GraphicsPipelineKey key,
            const nvrhi::FramebufferInfo& framebufferInfo) noexcept
        {
            uint64_t value = key.Value;
            value ^= static_cast<uint64_t>(framebufferInfo.sampleCount) +
                0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u);
            value ^= static_cast<uint64_t>(framebufferInfo.sampleQuality) +
                0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u);
            return { value == 0 ? 1u : value };
        }

        uint64_t Fingerprint(const nvrhi::ComputePipelineDesc& desc)
        {
            FingerprintBuilder fingerprint;
            fingerprint.Add(uint32_t{ 0x4350534f });
            fingerprint.AddPointer(desc.CS.Get());
            fingerprint.Add(desc.bindingLayouts.size());
            for (const nvrhi::BindingLayoutHandle& layout : desc.bindingLayouts)
                fingerprint.AddPointer(layout.Get());
            return fingerprint.Get();
        }

        void ValidateKey(uint64_t value, std::string_view category)
        {
            if (value == 0)
                throw std::invalid_argument(std::string(category) + " cache key must be non-zero");
        }

        void ValidateCollision(
            uint64_t key,
            uint64_t cachedFingerprint,
            uint64_t requestedFingerprint,
            std::string_view category,
            std::string_view cachedIdentity,
            std::string_view requestedIdentity)
        {
#ifdef HE_DEBUG
            if (cachedFingerprint != requestedFingerprint)
            {
                throw std::logic_error(
                    std::string("PipelineCache ") + std::string(category) +
                    " key collision: key=" + std::to_string(key) +
                    ", cached='" + std::string(cachedIdentity) +
                    "', requested='" + std::string(requestedIdentity) + "'");
            }
#else
            (void)key;
            (void)cachedFingerprint;
            (void)requestedFingerprint;
            (void)category;
            (void)cachedIdentity;
            (void)requestedIdentity;
#endif
        }

        [[nodiscard]] std::string CreationFailure(
            std::string_view category,
            uint64_t key,
            std::string_view debugIdentity)
        {
            return "NVRHI failed to create " + std::string(category) +
                " (key=" + std::to_string(key) +
                ", identity='" + std::string(debugIdentity) + "')";
        }
    }

    PipelineCache::PipelineCache(nvrhi::IDevice* device)
        : mDevice(device)
    {
        if (!mDevice)
            throw std::invalid_argument("PipelineCache requires a valid NVRHI device");
    }

    nvrhi::BindingLayoutHandle PipelineCache::GetOrCreateBindingLayout(
        BindingLayoutKey key,
        const nvrhi::BindingLayoutDesc& desc,
        std::string_view debugIdentity)
    {
        ValidateKey(key.Value, "binding layout");
        const uint64_t fingerprint = Fingerprint(desc);
        std::scoped_lock lock(mMutex);

        if (const auto it = mBindingLayouts.find(key); it != mBindingLayouts.end())
        {
            ValidateCollision(
                key.Value,
                it->second.DescriptorFingerprint,
                fingerprint,
                "binding layout",
                it->second.DebugIdentity,
                debugIdentity);
            return it->second.Resource;
        }

        nvrhi::BindingLayoutHandle layout = mDevice->createBindingLayout(desc);
        if (!layout)
            throw std::runtime_error(CreationFailure("binding layout", key.Value, debugIdentity));

        mBindingLayouts.emplace(
            key,
            CacheEntry<nvrhi::BindingLayoutHandle>{
                .Resource = layout,
                .DescriptorFingerprint = fingerprint,
                .DebugIdentity = std::string(debugIdentity),
            });
        return layout;
    }

    nvrhi::BindingLayoutHandle PipelineCache::GetOrCreateBindlessLayout(
        BindlessLayoutKey key,
        const nvrhi::BindlessLayoutDesc& desc,
        std::string_view debugIdentity)
    {
        ValidateKey(key.Value, "bindless layout");
        const uint64_t fingerprint = Fingerprint(desc);
        std::scoped_lock lock(mMutex);

        if (const auto it = mBindlessLayouts.find(key); it != mBindlessLayouts.end())
        {
            ValidateCollision(
                key.Value,
                it->second.DescriptorFingerprint,
                fingerprint,
                "bindless layout",
                it->second.DebugIdentity,
                debugIdentity);
            return it->second.Resource;
        }

        nvrhi::BindingLayoutHandle layout = mDevice->createBindlessLayout(desc);
        if (!layout)
            throw std::runtime_error(CreationFailure("bindless layout", key.Value, debugIdentity));

        mBindlessLayouts.emplace(
            key,
            CacheEntry<nvrhi::BindingLayoutHandle>{
                .Resource = layout,
                .DescriptorFingerprint = fingerprint,
                .DebugIdentity = std::string(debugIdentity),
            });
        return layout;
    }

    nvrhi::BindingSetHandle PipelineCache::GetOrCreateStaticBindingSet(
        StaticBindingSetKey key,
        const nvrhi::BindingSetDesc& desc,
        const nvrhi::BindingLayoutHandle& layout,
        std::string_view debugIdentity)
    {
        ValidateKey(key.Value, "static binding set");
        if (!layout)
            throw std::invalid_argument("Static binding set requires a valid NVRHI binding layout");

        const uint64_t fingerprint = Fingerprint(desc, layout);
        std::scoped_lock lock(mMutex);

        if (const auto it = mStaticBindingSets.find(key); it != mStaticBindingSets.end())
        {
            ValidateCollision(
                key.Value,
                it->second.DescriptorFingerprint,
                fingerprint,
                "static binding set",
                it->second.DebugIdentity,
                debugIdentity);
            return it->second.Resource;
        }

        nvrhi::BindingSetHandle bindingSet = mDevice->createBindingSet(desc, layout);
        if (!bindingSet)
            throw std::runtime_error(CreationFailure("static binding set", key.Value, debugIdentity));

        mStaticBindingSets.emplace(
            key,
            CacheEntry<nvrhi::BindingSetHandle>{
                .Resource = bindingSet,
                .DescriptorFingerprint = fingerprint,
                .DebugIdentity = std::string(debugIdentity),
            });
        return bindingSet;
    }

    nvrhi::GraphicsPipelineHandle PipelineCache::GetOrCreateGraphicsPipeline(
        GraphicsPipelineKey key,
        const nvrhi::GraphicsPipelineDesc& desc,
        const nvrhi::FramebufferInfo& framebufferInfo,
        std::string_view debugIdentity)
    {
        ValidateKey(key.Value, "graphics pipeline");
        nvrhi::GraphicsPipelineDesc effectiveDesc = desc;
        if (framebufferInfo.sampleCount > 1)
            effectiveDesc.renderState.rasterState.enableMultisample();
        const GraphicsPipelineKey effectiveKey =
            ResolveFramebufferSampleVariant(key, framebufferInfo);
        const uint64_t fingerprint = Fingerprint(effectiveDesc, framebufferInfo);
        std::scoped_lock lock(mMutex);

        if (const auto it = mGraphicsPipelines.find(effectiveKey);
            it != mGraphicsPipelines.end())
        {
            ValidateCollision(
                effectiveKey.Value,
                it->second.DescriptorFingerprint,
                fingerprint,
                "graphics pipeline",
                it->second.DebugIdentity,
                debugIdentity);
            return it->second.Resource;
        }

        nvrhi::GraphicsPipelineHandle pipeline = mDevice->createGraphicsPipeline(
            effectiveDesc,
            framebufferInfo);
        if (!pipeline)
        {
            throw std::runtime_error(CreationFailure(
                "graphics pipeline",
                effectiveKey.Value,
                debugIdentity));
        }

        mGraphicsPipelines.emplace(
            effectiveKey,
            CacheEntry<nvrhi::GraphicsPipelineHandle>{
                .Resource = pipeline,
                .DescriptorFingerprint = fingerprint,
                .DebugIdentity = std::string(debugIdentity),
            });
        return pipeline;
    }

    nvrhi::ComputePipelineHandle PipelineCache::GetOrCreateComputePipeline(
        ComputePipelineKey key,
        const nvrhi::ComputePipelineDesc& desc,
        std::string_view debugIdentity)
    {
        ValidateKey(key.Value, "compute pipeline");
        const uint64_t fingerprint = Fingerprint(desc);
        std::scoped_lock lock(mMutex);

        if (const auto it = mComputePipelines.find(key); it != mComputePipelines.end())
        {
            ValidateCollision(
                key.Value,
                it->second.DescriptorFingerprint,
                fingerprint,
                "compute pipeline",
                it->second.DebugIdentity,
                debugIdentity);
            return it->second.Resource;
        }

        nvrhi::ComputePipelineHandle pipeline = mDevice->createComputePipeline(desc);
        if (!pipeline)
            throw std::runtime_error(CreationFailure("compute pipeline", key.Value, debugIdentity));

        mComputePipelines.emplace(
            key,
            CacheEntry<nvrhi::ComputePipelineHandle>{
                .Resource = pipeline,
                .DescriptorFingerprint = fingerprint,
                .DebugIdentity = std::string(debugIdentity),
            });
        return pipeline;
    }

    void PipelineCache::Clear()
    {
        std::scoped_lock lock(mMutex);
        mStaticBindingSets.clear();
        mGraphicsPipelines.clear();
        mComputePipelines.clear();
        mBindlessLayouts.clear();
        mBindingLayouts.clear();
    }

    size_t PipelineCache::GetBindingLayoutCount() const
    {
        std::scoped_lock lock(mMutex);
        return mBindingLayouts.size();
    }

    size_t PipelineCache::GetBindlessLayoutCount() const
    {
        std::scoped_lock lock(mMutex);
        return mBindlessLayouts.size();
    }

    size_t PipelineCache::GetStaticBindingSetCount() const
    {
        std::scoped_lock lock(mMutex);
        return mStaticBindingSets.size();
    }

    size_t PipelineCache::GetGraphicsPipelineCount() const
    {
        std::scoped_lock lock(mMutex);
        return mGraphicsPipelines.size();
    }

    size_t PipelineCache::GetComputePipelineCount() const
    {
        std::scoped_lock lock(mMutex);
        return mComputePipelines.size();
    }
}
