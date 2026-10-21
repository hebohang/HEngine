#pragma once

#include "Runtime/Graphics/VFX/EffekseerRecordLeases.h"

#include <EffekseerRendererCommon/EffekseerRenderer.Renderer_Impl.h>
#include <EffekseerRendererCommon/VertexBuffer.h>
#include <EffekseerRendererLLGI/Common.h>

#include <memory>
#include <stdexcept>
#include <vector>

namespace HEngine
{
    class EffekseerFrameResources
    {
    public:
        struct Record
        {
            // Reverse destruction releases retained command references first.
            std::shared_ptr<EffekseerRenderer::VertexBuffer> VertexBuffer;
            Effekseer::RefPtr<EffekseerRenderer::SingleFrameMemoryPool> MemoryPool;
            Effekseer::RefPtr<EffekseerRenderer::CommandList> CommandList;
        };
        void Initialize(uint32_t count)
        {
            mLeases = EffekseerRecordLeases(count);
            mRecords.resize(count);
        }
        void Begin(uint32_t slot, uint64_t completed) { mLeases.Begin(slot, completed); }
        void Submitted(uint32_t slot, uint64_t submission) noexcept { mLeases.Submitted(slot, submission); }
        void Abort(uint32_t slot) noexcept { mLeases.Abort(slot); }
        template<class Renderer, class Device>
        Record& Acquire(Renderer& renderer, const Device& device)
        {
            const auto lease = mLeases.Acquire();
            auto& bank = mRecords[lease.FrameSlot];
            // A failed allocation consumes a lease but must not make a retry
            // index beyond the storage. Publish only fully constructed records.
            if (lease.Record >= bank.size())
                bank.resize(lease.Record + 1);
            if (!bank[lease.Record])
            {
                auto record = std::make_unique<Record>();
                record->VertexBuffer = std::make_shared<EffekseerRenderer::VertexBufferRing>(
                    renderer.GetGraphicsDevice(), renderer.GetImpl()->InternalVertexBuffer->GetSize(),
                    3, EffekseerRenderer::VertexBufferRingMode::ExpandableForCommandList);
                if (!record->VertexBuffer->GetIsValid())
                    throw std::runtime_error("Effekseer record vertex buffer creation failed");
                record->MemoryPool = EffekseerRenderer::CreateSingleFrameMemoryPool(device);
                if (record->MemoryPool == nullptr)
                    throw std::runtime_error("Effekseer record memory pool creation failed");
                record->CommandList = EffekseerRenderer::CreateCommandList(device, record->MemoryPool);
                if (record->CommandList == nullptr)
                    throw std::runtime_error("Effekseer record command list creation failed");
                bank[lease.Record] = std::move(record);
            }
            return *bank[lease.Record];
        }
        void Clear() noexcept { mRecords.clear(); mLeases = EffekseerRecordLeases(); }
    private:
        EffekseerRecordLeases mLeases;
        std::vector<std::vector<std::unique_ptr<Record>>> mRecords;
    };
}
