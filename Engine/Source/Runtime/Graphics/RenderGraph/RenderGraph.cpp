#include "Hepch.h"

#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include "Runtime/Graphics/RenderGraph/RenderGraphResourcePool.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <exception>
#include <optional>
#include <queue>
#include <sstream>
#include <unordered_set>

namespace HEngine
{
    namespace
    {
        class CpuPhaseScope
        {
        public:
            CpuPhaseScope(IRenderGraphCpuObserver* observer, RenderGraphCpuPhase phase,
                uint32_t index = InvalidRenderGraphIndex, std::string_view name = {}) noexcept
                : mObserver(observer), mPhase(phase), mIndex(index), mName(name)
            {
                if (mObserver)
                {
                    mExceptions = std::uncaught_exceptions();
                    mStart = std::chrono::steady_clock::now();
                }
            }
            ~CpuPhaseScope()
            {
                if (mObserver)
                    mObserver->OnCpuPhase(mPhase, mIndex, mName,
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - mStart).count(),
                        std::uncaught_exceptions() == mExceptions);
            }
        private:
            IRenderGraphCpuObserver* mObserver;
            RenderGraphCpuPhase mPhase;
            uint32_t mIndex;
            std::string_view mName;
            int mExceptions = 0;
            std::chrono::steady_clock::time_point mStart{};
        };

        [[nodiscard]] uint64_t AllocateResourceNamespace() noexcept
        {
            static std::atomic_uint64_t sNextNamespace = 1;
            uint64_t result = sNextNamespace.fetch_add(1, std::memory_order_relaxed);
            if (result == 0)
                result = sNextNamespace.fetch_add(1, std::memory_order_relaxed);
            return result;
        }

        enum class ResourceKind : uint8_t
        {
            Texture,
            Buffer,
            AccelerationStructure,
        };

        [[nodiscard]] const char* ToString(RenderGraphResourceLifetime lifetime)
        {
            switch (lifetime)
            {
            case RenderGraphResourceLifetime::Imported:   return "Imported";
            case RenderGraphResourceLifetime::Persistent: return "Persistent";
            case RenderGraphResourceLifetime::Transient:  return "Transient";
            }
            return "Unknown";
        }

        [[nodiscard]] const char* ToString(RenderGraphQueue queue)
        {
            switch (queue)
            {
            case RenderGraphQueue::Graphics: return "Graphics";
            case RenderGraphQueue::Compute:  return "Compute";
            case RenderGraphQueue::Copy:     return "Copy";
            }
            return "Unknown";
        }

        [[nodiscard]] const char* ToString(RenderGraphAccess access)
        {
            switch (access)
            {
            case RenderGraphAccess::Read:      return "Read";
            case RenderGraphAccess::Write:     return "Write";
            case RenderGraphAccess::ReadWrite: return "ReadWrite";
            }
            return "Unknown";
        }

        [[nodiscard]] RenderGraphDiagnosticResourceKind ToDiagnosticResourceKind(
            ResourceKind kind)
        {
            switch (kind)
            {
            case ResourceKind::Texture:
                return RenderGraphDiagnosticResourceKind::Texture;
            case ResourceKind::Buffer:
                return RenderGraphDiagnosticResourceKind::Buffer;
            case ResourceKind::AccelerationStructure:
                return RenderGraphDiagnosticResourceKind::AccelerationStructure;
            }
            return RenderGraphDiagnosticResourceKind::Texture;
        }

        [[nodiscard]] bool AccessIncludesRead(RenderGraphAccess access)
        {
            return access == RenderGraphAccess::Read || access == RenderGraphAccess::ReadWrite;
        }

        [[nodiscard]] bool AccessIncludesWrite(RenderGraphAccess access)
        {
            return access == RenderGraphAccess::Write || access == RenderGraphAccess::ReadWrite;
        }

        [[nodiscard]] bool AccessAllows(RenderGraphAccess declared, RenderGraphAccess required)
        {
            if (declared == RenderGraphAccess::ReadWrite)
                return true;
            return declared == required;
        }

        [[nodiscard]] RenderGraphAccess MergeAccess(RenderGraphAccess left, RenderGraphAccess right)
        {
            if (left == right)
                return left;
            return RenderGraphAccess::ReadWrite;
        }

        [[nodiscard]] bool IsReadOnlyUsage(RenderGraphUsage usage)
        {
            constexpr RenderGraphUsage writeUsages =
                RenderGraphUsage::RenderTarget |
                RenderGraphUsage::DepthWrite |
                RenderGraphUsage::UnorderedAccess |
                RenderGraphUsage::CopyDest |
                RenderGraphUsage::ResolveDest |
                RenderGraphUsage::AccelerationStructureBuildWrite;
            return (usage & writeUsages) == RenderGraphUsage::None;
        }

        [[nodiscard]] bool IsWriteOnlyUsage(RenderGraphUsage usage)
        {
            constexpr RenderGraphUsage readUsages =
                RenderGraphUsage::DepthRead |
                RenderGraphUsage::ShaderResource |
                RenderGraphUsage::CopySource |
                RenderGraphUsage::ResolveSource |
                RenderGraphUsage::Present |
                RenderGraphUsage::VertexBuffer |
                RenderGraphUsage::IndexBuffer |
                RenderGraphUsage::ConstantBuffer |
                RenderGraphUsage::IndirectArgument |
                RenderGraphUsage::AccelerationStructureBuildInput |
                RenderGraphUsage::AccelerationStructureRead;
            return (usage & readUsages) == RenderGraphUsage::None;
        }

        void ValidateAccessUsage(RenderGraphAccess access, RenderGraphUsage usage, ResourceKind kind)
        {
            if (access != RenderGraphAccess::Read && access != RenderGraphAccess::Write &&
                access != RenderGraphAccess::ReadWrite)
                throw RenderGraphValidationError("RenderGraph resource access enum is invalid.");
            if (usage == RenderGraphUsage::None)
                throw RenderGraphValidationError("RenderGraph resource access must declare a usage.");
            constexpr uint32_t knownUsageBits = (1u << 17) - 1u;
            if ((static_cast<uint32_t>(usage) & ~knownUsageBits) != 0)
                throw RenderGraphValidationError("RenderGraph resource usage contains unknown flags.");

            constexpr RenderGraphUsage textureOnly =
                RenderGraphUsage::RenderTarget |
                RenderGraphUsage::DepthRead |
                RenderGraphUsage::DepthWrite |
                RenderGraphUsage::ResolveSource |
                RenderGraphUsage::ResolveDest |
                RenderGraphUsage::Present;
            constexpr RenderGraphUsage bufferOnly =
                RenderGraphUsage::VertexBuffer |
                RenderGraphUsage::IndexBuffer |
                RenderGraphUsage::ConstantBuffer |
                RenderGraphUsage::IndirectArgument |
                RenderGraphUsage::AccelerationStructureBuildInput;
            constexpr RenderGraphUsage accelerationStructureOnly =
                RenderGraphUsage::AccelerationStructureBuildWrite |
                RenderGraphUsage::AccelerationStructureRead;

            if (kind == ResourceKind::Buffer)
            {
                if ((usage & (textureOnly | accelerationStructureOnly)) != RenderGraphUsage::None)
                    throw RenderGraphValidationError("RenderGraph buffer declared with a non-buffer usage.");
            }
            else if (kind == ResourceKind::Texture)
            {
                if ((usage & (bufferOnly | accelerationStructureOnly)) != RenderGraphUsage::None)
                    throw RenderGraphValidationError("RenderGraph texture declared with a non-texture usage.");
            }
            else
            {
                const RenderGraphUsage invalidUsages = usage &
                    (textureOnly |
                        bufferOnly |
                        RenderGraphUsage::ShaderResource |
                         RenderGraphUsage::UnorderedAccess |
                         RenderGraphUsage::CopySource |
                         RenderGraphUsage::CopyDest |
                         RenderGraphUsage::ResolveSource |
                         RenderGraphUsage::ResolveDest);
                if (invalidUsages != RenderGraphUsage::None)
                {
                    throw RenderGraphValidationError(
                        "RenderGraph acceleration structure declared with a non-AS usage.");
                }
            }

            if (access == RenderGraphAccess::Read && !IsReadOnlyUsage(usage))
                throw RenderGraphValidationError("RenderGraph read access contains a write-only usage.");
            if (access == RenderGraphAccess::Write && !IsWriteOnlyUsage(usage))
                throw RenderGraphValidationError("RenderGraph write access contains a read-only usage.");
            if (access == RenderGraphAccess::ReadWrite &&
                usage != RenderGraphUsage::UnorderedAccess &&
                usage != RenderGraphUsage::DepthWrite &&
                usage != RenderGraphUsage::RenderTarget)
            {
                throw RenderGraphValidationError(
                    "RenderGraph ReadWrite access requires UnorderedAccess, DepthWrite or RenderTarget usage.");
            }
        }

        [[nodiscard]] bool ContainsSubresource(const RenderGraphSubresourceRange& range,
            uint32_t mip, uint32_t slice)
        {
            return mip >= range.BaseMipLevel && slice >= range.BaseArraySlice &&
                (range.MipLevelCount == RenderGraphSubresourceRange::AllRemaining ||
                    mip - range.BaseMipLevel < range.MipLevelCount) &&
                (range.ArraySliceCount == RenderGraphSubresourceRange::AllRemaining ||
                    slice - range.BaseArraySlice < range.ArraySliceCount);
        }

        [[nodiscard]] nvrhi::TextureSubresourceSet ToNvrhiSubresources(
            const RenderGraphSubresourceRange& range)
        {
            const uint32_t mipCount = range.MipLevelCount == RenderGraphSubresourceRange::AllRemaining
                ? nvrhi::TextureSubresourceSet::AllMipLevels
                : range.MipLevelCount;
            const uint32_t arrayCount = range.ArraySliceCount == RenderGraphSubresourceRange::AllRemaining
                ? nvrhi::TextureSubresourceSet::AllArraySlices
                : range.ArraySliceCount;
            return {
                range.BaseMipLevel,
                mipCount,
                range.BaseArraySlice,
                arrayCount
            };
        }
    }

    struct RenderGraph::Impl
    {
        struct AccessDeclaration
        {
            ResourceKind Kind = ResourceKind::Texture;
            uint32_t ResourceIndex = InvalidRenderGraphIndex;
            uint32_t ResourceGeneration = 0;
            RenderGraphAccess Access = RenderGraphAccess::Read;
            RenderGraphUsage Usage = RenderGraphUsage::None;
            RenderGraphSubresourceRange Subresources;
            bool bAttachment = false;
            RenderGraphAttachmentOps AttachmentOps;
        };

        struct TextureRecord
        {
            uint32_t Generation = 0;
            RenderGraphTextureDesc Desc;
            RenderGraphResourceLifetime Lifetime = RenderGraphResourceLifetime::Transient;
            nvrhi::ResourceStates InitialState = nvrhi::ResourceStates::Common;
            nvrhi::ResourceStates FinalState = nvrhi::ResourceStates::Unknown;
            nvrhi::TextureHandle Resource;
            RenderGraphResourceLifetimeInfo LifetimeInfo;
            RenderGraphUsage RequiredUsage = RenderGraphUsage::None;
            bool bInitiallyInitialized = false;
            bool bExported = false;
        };

        struct BufferRecord
        {
            uint32_t Generation = 0;
            RenderGraphBufferDesc Desc;
            RenderGraphResourceLifetime Lifetime = RenderGraphResourceLifetime::Transient;
            nvrhi::ResourceStates InitialState = nvrhi::ResourceStates::Common;
            nvrhi::ResourceStates FinalState = nvrhi::ResourceStates::Unknown;
            nvrhi::BufferHandle Resource;
            RenderGraphResourceLifetimeInfo LifetimeInfo;
            RenderGraphUsage RequiredUsage = RenderGraphUsage::None;
            bool bInitiallyInitialized = false;
            bool bExported = false;
        };

        struct AccelerationStructureRecord
        {
            uint32_t Generation = 0;
            RenderGraphAccelerationStructureDesc Desc;
            nvrhi::ResourceStates InitialState = nvrhi::ResourceStates::Unknown;
            nvrhi::ResourceStates FinalState = nvrhi::ResourceStates::Unknown;
            nvrhi::rt::AccelStructHandle Resource;
            RenderGraphResourceLifetimeInfo LifetimeInfo;
            RenderGraphUsage RequiredUsage = RenderGraphUsage::None;
            bool bInitiallyBuilt = true;
            bool bExported = false;
        };

        struct PassRecord
        {
            std::string FeatureTypeId;
            std::string FeatureInstanceId;
            uint32_t Generation = 0;
            std::string Name;
            RenderGraphQueue Queue = RenderGraphQueue::Graphics;
            std::function<void(RenderGraphContext&)> Execute;
            std::vector<AccessDeclaration> Accesses;
            std::vector<uint32_t> ExplicitPrerequisites;
            size_t TextureCheckpoint = 0;
            size_t BufferCheckpoint = 0;
            size_t AccelerationStructureCheckpoint = 0;
            bool bSideEffect = false;
            bool bSetupComplete = false;
            bool bCulled = true;
            bool bHasRecordedCommands = false;
            RenderGraphQueue RecordedQueue = RenderGraphQueue::Graphics;
        };

        uint32_t NextGeneration = 1;
        std::string CurrentFeatureTypeId;
        std::string CurrentFeatureInstanceId;
        uint64_t ResourceNamespace = AllocateResourceNamespace();
        std::vector<uint64_t> StaleResourceNamespaces;
        uint32_t Width = 1;
        uint32_t Height = 1;
        bool bDirty = true;
        bool bCompiled = false;

        std::vector<TextureRecord> Textures;
        std::vector<BufferRecord> Buffers;
        std::vector<AccelerationStructureRecord> AccelerationStructures;
        std::vector<PassRecord> Passes;
        std::vector<uint32_t> TopologicalOrder;
        std::vector<uint32_t> ExecutionOrder;
        std::vector<std::unordered_set<uint32_t>> Predecessors;
        std::vector<std::unordered_set<uint32_t>> ContentPredecessors;
        std::vector<std::unordered_set<uint32_t>> Successors;
        std::vector<std::string> CompileErrors;

        [[nodiscard]] uint32_t AllocateGeneration()
        {
            const uint32_t generation = NextGeneration++;
            if (NextGeneration == 0)
                NextGeneration = 1;
            return generation == 0 ? AllocateGeneration() : generation;
        }

        void MarkDirty()
        {
            bDirty = true;
            bCompiled = false;
            TopologicalOrder.clear();
            ExecutionOrder.clear();
            CompileErrors.clear();
        }

        void AddError(std::string error)
        {
            CompileErrors.push_back(std::move(error));
        }

        void AddEdge(uint32_t prerequisite, uint32_t dependent, bool bAllowSelf = false,
            bool bContentsRequired = true)
        {
            if (prerequisite == dependent && !bAllowSelf)
                return;
            if (Successors[prerequisite].insert(dependent).second)
                Predecessors[dependent].insert(prerequisite);
            if (bContentsRequired)
                ContentPredecessors[dependent].insert(prerequisite);
        }
    };

    RenderGraph::RenderGraph()
        : mImpl(std::make_unique<Impl>())
    {
    }

    RenderGraph::~RenderGraph() = default;

    void RenderGraph::Swap(RenderGraph& other) noexcept
    {
        mImpl.swap(other.mImpl);
    }

    void RenderGraph::PrepareResources(RenderGraphResourcePool& resourcePool, uint32_t frameSlot)
    {
        CompileOrThrow();
        RealizeResources(resourcePool, frameSlot);
        ValidatePhysicalResources();
    }

    nvrhi::TextureHandle RenderGraph::GetPhysicalTexture(TextureHandle handle) const
    {
        if (!IsValid(handle))
            throw RenderGraphValidationError("Physical texture lookup used a stale graph handle.");
        return mImpl->Textures[handle.Index].Resource;
    }

    TextureHandle RenderGraph::CreateTexture(
        const RenderGraphTextureDesc& desc,
        RenderGraphResourceLifetime lifetime,
        bool bInitiallyInitialized)
    {
        if (lifetime == RenderGraphResourceLifetime::Imported)
            throw RenderGraphValidationError("Use ImportTexture to create an imported RenderGraph resource.");
        if (desc.Width == 0 || desc.Height == 0 || desc.Depth == 0 ||
            desc.MipLevels == 0 || desc.ArrayLayers == 0 || desc.SampleCount == 0)
        {
            throw RenderGraphValidationError("RenderGraph texture dimensions must be non-zero.");
        }
        if (desc.bRelativeToGraph && (desc.WidthScale <= 0.0f || desc.HeightScale <= 0.0f))
            throw RenderGraphValidationError("Relative RenderGraph texture scale must be positive.");

        Impl::TextureRecord record;
        record.Generation = mImpl->AllocateGeneration();
        record.Desc = desc;
        record.Lifetime = lifetime;
        record.InitialState = nvrhi::ResourceStates::Common;
        // Pool-owned allocations return to Common at the graph boundary. This
        // makes the next frame's explicit beginTracking* state true even when a
        // transient allocation is reused from another frame slot.
        record.FinalState = nvrhi::ResourceStates::Common;
        record.bInitiallyInitialized = bInitiallyInitialized;

        if (record.Desc.bRelativeToGraph)
        {
            record.Desc.Width = std::max(
                1u,
                static_cast<uint32_t>(std::lround(
                    static_cast<double>(mImpl->Width) * record.Desc.WidthScale)));
            record.Desc.Height = std::max(
                1u,
                static_cast<uint32_t>(std::lround(
                    static_cast<double>(mImpl->Height) * record.Desc.HeightScale)));
        }

        const TextureHandle handle{
            static_cast<uint32_t>(mImpl->Textures.size()),
            record.Generation
        };
        mImpl->Textures.push_back(std::move(record));
        mImpl->MarkDirty();
        return handle;
    }

    BufferHandle RenderGraph::CreateBuffer(
        const RenderGraphBufferDesc& desc,
        RenderGraphResourceLifetime lifetime,
        bool bInitiallyInitialized)
    {
        if (lifetime == RenderGraphResourceLifetime::Imported)
            throw RenderGraphValidationError("Use ImportBuffer to create an imported RenderGraph resource.");
        if (desc.ByteSize == 0)
            throw RenderGraphValidationError("RenderGraph buffer byte size must be non-zero.");
        if (desc.StructStride > desc.ByteSize)
            throw RenderGraphValidationError("RenderGraph buffer stride exceeds its byte size.");

        Impl::BufferRecord record;
        record.Generation = mImpl->AllocateGeneration();
        record.Desc = desc;
        record.Lifetime = lifetime;
        record.InitialState = nvrhi::ResourceStates::Common;
        record.FinalState = nvrhi::ResourceStates::Common;
        record.bInitiallyInitialized = bInitiallyInitialized;

        const BufferHandle handle{
            static_cast<uint32_t>(mImpl->Buffers.size()),
            record.Generation
        };
        mImpl->Buffers.push_back(std::move(record));
        mImpl->MarkDirty();
        return handle;
    }

    TextureHandle RenderGraph::ImportTexture(
        const RenderGraphTextureDesc& desc,
        const nvrhi::TextureHandle& texture,
        nvrhi::ResourceStates initialState,
        nvrhi::ResourceStates finalState)
    {
        if (desc.Width == 0 || desc.Height == 0 || desc.Depth == 0 ||
            desc.MipLevels == 0 || desc.ArrayLayers == 0 || desc.SampleCount == 0)
        {
            throw RenderGraphValidationError("Imported RenderGraph texture dimensions must be non-zero.");
        }

        Impl::TextureRecord record;
        record.Generation = mImpl->AllocateGeneration();
        record.Desc = desc;
        record.Lifetime = RenderGraphResourceLifetime::Imported;
        record.InitialState = initialState;
        record.FinalState = finalState;
        record.Resource = texture;
        record.bInitiallyInitialized = true;

        const TextureHandle handle{
            static_cast<uint32_t>(mImpl->Textures.size()),
            record.Generation
        };
        mImpl->Textures.push_back(std::move(record));
        mImpl->MarkDirty();
        return handle;
    }

    BufferHandle RenderGraph::ImportBuffer(
        const RenderGraphBufferDesc& desc,
        const nvrhi::BufferHandle& buffer,
        nvrhi::ResourceStates initialState,
        nvrhi::ResourceStates finalState)
    {
        if (desc.ByteSize == 0)
            throw RenderGraphValidationError("Imported RenderGraph buffer byte size must be non-zero.");

        Impl::BufferRecord record;
        record.Generation = mImpl->AllocateGeneration();
        record.Desc = desc;
        record.Lifetime = RenderGraphResourceLifetime::Imported;
        record.InitialState = initialState;
        record.FinalState = finalState;
        record.Resource = buffer;
        record.bInitiallyInitialized = true;

        const BufferHandle handle{
            static_cast<uint32_t>(mImpl->Buffers.size()),
            record.Generation
        };
        mImpl->Buffers.push_back(std::move(record));
        mImpl->MarkDirty();
        return handle;
    }

    AccelerationStructureHandle RenderGraph::ImportAccelerationStructure(
        const RenderGraphAccelerationStructureDesc& desc,
        const nvrhi::rt::AccelStructHandle& accelerationStructure,
        nvrhi::ResourceStates initialState,
        nvrhi::ResourceStates finalState,
        bool bInitiallyBuilt)
    {
        Impl::AccelerationStructureRecord record;
        record.Generation = mImpl->AllocateGeneration();
        record.Desc = desc;
        record.InitialState = initialState;
        record.FinalState = finalState;
        record.Resource = accelerationStructure;
        record.bInitiallyBuilt = bInitiallyBuilt;

        const AccelerationStructureHandle handle{
            static_cast<uint32_t>(mImpl->AccelerationStructures.size()),
            record.Generation
        };
        mImpl->AccelerationStructures.push_back(std::move(record));
        mImpl->MarkDirty();
        return handle;
    }

    void RenderGraph::SetTextureResource(TextureHandle handle, const nvrhi::TextureHandle& texture,
        nvrhi::ResourceStates initialState)
    {
        if (!IsValid(handle))
            throw RenderGraphValidationError("Invalid or stale RenderGraph texture handle.");
        auto& record = mImpl->Textures[handle.Index];
        if (initialState != nvrhi::ResourceStates::Unknown)
        {
            if (record.Lifetime != RenderGraphResourceLifetime::Imported)
                throw RenderGraphValidationError("Only imported textures can update their externally established state.");
            if (record.InitialState != initialState)
            {
                record.InitialState = initialState;
                mImpl->MarkDirty();
            }
        }
        record.Resource = texture;
    }

    void RenderGraph::SetBufferResource(BufferHandle handle, const nvrhi::BufferHandle& buffer)
    {
        if (!IsValid(handle))
            throw RenderGraphValidationError("Invalid or stale RenderGraph buffer handle.");
        mImpl->Buffers[handle.Index].Resource = buffer;
    }

    void RenderGraph::SetAccelerationStructureResource(
        AccelerationStructureHandle handle,
        const nvrhi::rt::AccelStructHandle& accelerationStructure)
    {
        if (!IsValid(handle))
        {
            throw RenderGraphValidationError(
                "Invalid or stale RenderGraph acceleration structure handle.");
        }
        mImpl->AccelerationStructures[handle.Index].Resource = accelerationStructure;
    }

    void RenderGraph::Export(TextureHandle handle)
    {
        if (!IsValid(handle))
            throw RenderGraphValidationError("Cannot export an invalid or stale RenderGraph texture handle.");
        mImpl->Textures[handle.Index].bExported = true;
        mImpl->MarkDirty();
    }

    void RenderGraph::Export(BufferHandle handle)
    {
        if (!IsValid(handle))
            throw RenderGraphValidationError("Cannot export an invalid or stale RenderGraph buffer handle.");
        mImpl->Buffers[handle.Index].bExported = true;
        mImpl->MarkDirty();
    }

    void RenderGraph::Export(AccelerationStructureHandle handle)
    {
        if (!IsValid(handle))
        {
            throw RenderGraphValidationError(
                "Cannot export an invalid or stale RenderGraph acceleration structure handle.");
        }
        mImpl->AccelerationStructures[handle.Index].bExported = true;
        mImpl->MarkDirty();
    }

    RenderGraphFeatureScope::RenderGraphFeatureScope(RenderGraph& graph,
        std::string typeId, std::string instanceId)
        : mGraph(graph), mPreviousTypeId(std::move(typeId)), mPreviousInstanceId(std::move(instanceId))
    {
        mPreviousTypeId.swap(mGraph.mImpl->CurrentFeatureTypeId);
        mPreviousInstanceId.swap(mGraph.mImpl->CurrentFeatureInstanceId);
    }

    RenderGraphFeatureScope::~RenderGraphFeatureScope()
    {
        mPreviousTypeId.swap(mGraph.mImpl->CurrentFeatureTypeId);
        mPreviousInstanceId.swap(mGraph.mImpl->CurrentFeatureInstanceId);
    }

    RenderGraphPassHandle RenderGraph::AddPassInternal(
        std::string name,
        RenderGraphQueue queue,
        std::function<void(RenderGraphContext&)> execute)
    {
        if (name.empty())
            throw RenderGraphValidationError("RenderGraph pass name cannot be empty.");
        if (!execute)
            throw RenderGraphValidationError("RenderGraph pass execute function cannot be empty.");

        Impl::PassRecord record;
        record.Generation = mImpl->AllocateGeneration();
        record.Name = std::move(name);
        record.FeatureTypeId = mImpl->CurrentFeatureTypeId;
        record.FeatureInstanceId = mImpl->CurrentFeatureInstanceId;
        record.Queue = queue;
        record.Execute = std::move(execute);
        record.TextureCheckpoint = mImpl->Textures.size();
        record.BufferCheckpoint = mImpl->Buffers.size();
        record.AccelerationStructureCheckpoint = mImpl->AccelerationStructures.size();

        const RenderGraphPassHandle handle{
            static_cast<uint32_t>(mImpl->Passes.size()),
            record.Generation
        };
        mImpl->Passes.push_back(std::move(record));
        mImpl->MarkDirty();
        return handle;
    }

    void RenderGraph::FinishPassSetup(RenderGraphPassHandle pass)
    {
        if (!IsValid(pass))
            throw RenderGraphValidationError("Invalid RenderGraph pass after setup.");
        mImpl->Passes[pass.Index].bSetupComplete = true;
    }

    void RenderGraph::AbortPassSetup(RenderGraphPassHandle pass) noexcept
    {
        if (pass.Index >= mImpl->Passes.size() || pass.Index + 1 != mImpl->Passes.size())
            return;
        const Impl::PassRecord& record = mImpl->Passes.back();
        if (record.Generation != pass.Generation)
            return;
        mImpl->Textures.resize(record.TextureCheckpoint);
        mImpl->Buffers.resize(record.BufferCheckpoint);
        mImpl->AccelerationStructures.resize(record.AccelerationStructureCheckpoint);
        mImpl->Passes.pop_back();
        mImpl->MarkDirty();
    }

    void RenderGraph::SetPassSideEffect(RenderGraphPassHandle pass, bool bSideEffect)
    {
        if (!IsValid(pass))
            throw RenderGraphValidationError("Invalid or stale RenderGraph pass handle.");
        mImpl->Passes[pass.Index].bSideEffect = bSideEffect;
        mImpl->MarkDirty();
    }

    void RenderGraph::SetPassQueue(RenderGraphPassHandle pass, RenderGraphQueue queue)
    {
        if (!IsValid(pass))
            throw RenderGraphValidationError("Invalid or stale RenderGraph pass handle.");
        mImpl->Passes[pass.Index].Queue = queue;
        mImpl->MarkDirty();
    }

    void RenderGraph::AddDependency(
        RenderGraphPassHandle prerequisite,
        RenderGraphPassHandle dependent)
    {
        if (!IsValid(prerequisite) || !IsValid(dependent))
            throw RenderGraphValidationError("RenderGraph dependency contains an invalid pass handle.");

        auto& dependencies = mImpl->Passes[dependent.Index].ExplicitPrerequisites;
        if (std::find(dependencies.begin(), dependencies.end(), prerequisite.Index) == dependencies.end())
            dependencies.push_back(prerequisite.Index);
        mImpl->MarkDirty();
    }

    void RenderGraph::DeclareTextureAccess(
        RenderGraphPassHandle pass,
        TextureHandle handle,
        RenderGraphAccess access,
        RenderGraphUsage usage,
        RenderGraphSubresourceRange subresources)
    {
        if (!IsValid(pass))
            throw RenderGraphValidationError("Invalid or stale RenderGraph pass handle.");
        if (!IsValid(handle))
            throw RenderGraphValidationError("Invalid or stale RenderGraph texture handle.");
        ValidateAccessUsage(access, usage, ResourceKind::Texture);

        const RenderGraphTextureDesc& desc = mImpl->Textures[handle.Index].Desc;
        if (subresources.BaseMipLevel >= desc.MipLevels ||
            subresources.BaseArraySlice >= desc.ArrayLayers)
        {
            throw RenderGraphValidationError("RenderGraph texture subresource base is out of range.");
        }
        if (subresources.MipLevelCount == 0 || subresources.ArraySliceCount == 0)
            throw RenderGraphValidationError("RenderGraph texture subresource count cannot be zero.");
        if (subresources.MipLevelCount != RenderGraphSubresourceRange::AllRemaining &&
            subresources.MipLevelCount > desc.MipLevels - subresources.BaseMipLevel)
        {
            throw RenderGraphValidationError("RenderGraph texture mip range is out of bounds.");
        }
        if (subresources.ArraySliceCount != RenderGraphSubresourceRange::AllRemaining &&
            subresources.ArraySliceCount > desc.ArrayLayers - subresources.BaseArraySlice)
        {
            throw RenderGraphValidationError("RenderGraph texture array range is out of bounds.");
        }

        auto& accesses = mImpl->Passes[pass.Index].Accesses;
        // Differently spelled ranges can overlap even when they are not equal.
        // Reject incompatible simultaneous states before command recording.
        const uint32_t mipEnd = subresources.MipLevelCount == RenderGraphSubresourceRange::AllRemaining
            ? desc.MipLevels : subresources.BaseMipLevel + subresources.MipLevelCount;
        const uint32_t sliceEnd = subresources.ArraySliceCount == RenderGraphSubresourceRange::AllRemaining
            ? desc.ArrayLayers : subresources.BaseArraySlice + subresources.ArraySliceCount;
        for (const auto& declaration : accesses)
        {
            if (declaration.Kind != ResourceKind::Texture || declaration.ResourceIndex != handle.Index)
                continue;
            if (declaration.bAttachment)
                throw RenderGraphValidationError("Color attachment cannot be redeclared in the same pass.");
            const auto& range = declaration.Subresources;
            const uint32_t otherMipEnd = range.MipLevelCount == RenderGraphSubresourceRange::AllRemaining
                ? desc.MipLevels : range.BaseMipLevel + range.MipLevelCount;
            const uint32_t otherSliceEnd = range.ArraySliceCount == RenderGraphSubresourceRange::AllRemaining
                ? desc.ArrayLayers : range.BaseArraySlice + range.ArraySliceCount;
            if (subresources.BaseMipLevel < otherMipEnd && range.BaseMipLevel < mipEnd &&
                subresources.BaseArraySlice < otherSliceEnd && range.BaseArraySlice < sliceEnd)
            {
                ValidateAccessUsage(MergeAccess(declaration.Access, access),
                    declaration.Usage | usage, ResourceKind::Texture);
            }
        }
        auto existing = std::find_if(accesses.begin(), accesses.end(),
            [handle, &subresources](const Impl::AccessDeclaration& declaration)
            {
                return declaration.Kind == ResourceKind::Texture &&
                    declaration.ResourceIndex == handle.Index &&
                    declaration.ResourceGeneration == handle.Generation &&
                    declaration.Subresources == subresources;
            });
        if (existing != accesses.end())
        {
            const RenderGraphAccess mergedAccess = MergeAccess(existing->Access, access);
            const RenderGraphUsage mergedUsage = existing->Usage | usage;
            ValidateAccessUsage(mergedAccess, mergedUsage, ResourceKind::Texture);
            existing->Access = mergedAccess;
            existing->Usage = mergedUsage;
        }
        else
        {
            accesses.push_back({
                ResourceKind::Texture,
                handle.Index,
                handle.Generation,
                access,
                usage,
                subresources
            });
        }
        mImpl->MarkDirty();
    }

    void RenderGraph::DeclareAttachment(RenderGraphPassHandle pass, TextureHandle handle,
        const RenderGraphAttachmentOps& ops, RenderGraphSubresourceRange subresources, bool bDepth)
    {
        if (!IsValid(pass) || !IsValid(handle))
            throw RenderGraphValidationError("Invalid or stale attachment handle.");
        if ((ops.Load != RenderGraphAttachmentLoad::Load && ops.Load != RenderGraphAttachmentLoad::Clear &&
                ops.Load != RenderGraphAttachmentLoad::Discard) ||
            (ops.Store != RenderGraphAttachmentStore::Store && ops.Store != RenderGraphAttachmentStore::Discard))
            throw RenderGraphValidationError("Invalid attachment load/store operation.");
        if (ops.Load == RenderGraphAttachmentLoad::Discard &&
            ops.Store == RenderGraphAttachmentStore::Store && !ops.bFullOverwrite)
            throw RenderGraphValidationError("Discard/store attachment requires full overwrite.");
        const auto format = mImpl->Textures[handle.Index].Desc.Format;
        if (format != nvrhi::Format::UNKNOWN && nvrhi::getFormatInfo(format).hasDepth != bDepth)
            throw RenderGraphValidationError("Attachment format does not match its color/depth usage.");
        if (bDepth && !(ops.ClearDepth >= 0.0f && ops.ClearDepth <= 1.0f))
            throw RenderGraphValidationError("Attachment clear depth must be finite and in [0, 1].");
        for (const auto& existing : mImpl->Passes[pass.Index].Accesses)
        {
            if (existing.Kind == ResourceKind::Texture && existing.ResourceIndex == handle.Index)
                throw RenderGraphValidationError("Attachment must be declared once per texture in a pass.");
        }
        DeclareTextureAccess(pass, handle,
            ops.Load == RenderGraphAttachmentLoad::Load ? RenderGraphAccess::ReadWrite : RenderGraphAccess::Write,
            bDepth ? RenderGraphUsage::DepthWrite : RenderGraphUsage::RenderTarget, subresources);
        auto& declaration = mImpl->Passes[pass.Index].Accesses.back();
        declaration.bAttachment = true;
        declaration.AttachmentOps = ops;
    }

    void RenderGraph::DeclareBufferAccess(
        RenderGraphPassHandle pass,
        BufferHandle handle,
        RenderGraphAccess access,
        RenderGraphUsage usage)
    {
        if (!IsValid(pass))
            throw RenderGraphValidationError("Invalid or stale RenderGraph pass handle.");
        if (!IsValid(handle))
            throw RenderGraphValidationError("Invalid or stale RenderGraph buffer handle.");
        ValidateAccessUsage(access, usage, ResourceKind::Buffer);

        auto& accesses = mImpl->Passes[pass.Index].Accesses;
        auto existing = std::find_if(accesses.begin(), accesses.end(),
            [handle](const Impl::AccessDeclaration& declaration)
            {
                return declaration.Kind == ResourceKind::Buffer &&
                    declaration.ResourceIndex == handle.Index &&
                    declaration.ResourceGeneration == handle.Generation;
            });
        if (existing != accesses.end())
        {
            const RenderGraphAccess mergedAccess = MergeAccess(existing->Access, access);
            const RenderGraphUsage mergedUsage = existing->Usage | usage;
            ValidateAccessUsage(mergedAccess, mergedUsage, ResourceKind::Buffer);
            existing->Access = mergedAccess;
            existing->Usage = mergedUsage;
        }
        else
        {
            accesses.push_back({
                ResourceKind::Buffer,
                handle.Index,
                handle.Generation,
                access,
                usage,
                {}
            });
        }
        mImpl->MarkDirty();
    }

    void RenderGraph::DeclareAccelerationStructureAccess(
        RenderGraphPassHandle pass,
        AccelerationStructureHandle handle,
        RenderGraphAccess access,
        RenderGraphUsage usage)
    {
        if (!IsValid(pass))
            throw RenderGraphValidationError("Invalid or stale RenderGraph pass handle.");
        if (!IsValid(handle))
        {
            throw RenderGraphValidationError(
                "Invalid or stale RenderGraph acceleration structure handle.");
        }
        ValidateAccessUsage(access, usage, ResourceKind::AccelerationStructure);

        auto& accesses = mImpl->Passes[pass.Index].Accesses;
        auto existing = std::find_if(accesses.begin(), accesses.end(),
            [handle](const Impl::AccessDeclaration& declaration)
            {
                return declaration.Kind == ResourceKind::AccelerationStructure &&
                    declaration.ResourceIndex == handle.Index &&
                    declaration.ResourceGeneration == handle.Generation;
            });
        if (existing != accesses.end())
        {
            const RenderGraphAccess mergedAccess = MergeAccess(existing->Access, access);
            const RenderGraphUsage mergedUsage = existing->Usage | usage;
            ValidateAccessUsage(
                mergedAccess,
                mergedUsage,
                ResourceKind::AccelerationStructure);
            existing->Access = mergedAccess;
            existing->Usage = mergedUsage;
        }
        else
        {
            accesses.push_back({
                ResourceKind::AccelerationStructure,
                handle.Index,
                handle.Generation,
                access,
                usage,
                {}
            });
        }
        mImpl->MarkDirty();
    }

    bool RenderGraph::Compile()
    {
        mImpl->CompileErrors.clear();
        mImpl->TopologicalOrder.clear();
        mImpl->ExecutionOrder.clear();
        mImpl->Predecessors.assign(mImpl->Passes.size(), {});
        mImpl->ContentPredecessors.assign(mImpl->Passes.size(), {});
        mImpl->Successors.assign(mImpl->Passes.size(), {});
        for (Impl::PassRecord& pass : mImpl->Passes)
        {
            pass.bCulled = true;
            pass.bHasRecordedCommands = false;
        }
        for (Impl::TextureRecord& texture : mImpl->Textures)
        {
            texture.LifetimeInfo = {};
            texture.RequiredUsage = RenderGraphUsage::None;
        }
        for (Impl::BufferRecord& buffer : mImpl->Buffers)
        {
            buffer.LifetimeInfo = {};
            buffer.RequiredUsage = RenderGraphUsage::None;
        }
        for (Impl::AccelerationStructureRecord& accelerationStructure :
            mImpl->AccelerationStructures)
        {
            accelerationStructure.LifetimeInfo = {};
            accelerationStructure.RequiredUsage = RenderGraphUsage::None;
        }

        for (uint32_t passIndex = 0; passIndex < mImpl->Passes.size(); ++passIndex)
        {
            const Impl::PassRecord& pass = mImpl->Passes[passIndex];
            if (!pass.bSetupComplete)
                mImpl->AddError("RenderGraph pass setup did not complete: " + pass.Name);
            for (uint32_t prerequisite : pass.ExplicitPrerequisites)
                mImpl->AddEdge(prerequisite, passIndex, true);
            for (const Impl::AccessDeclaration& access : pass.Accesses)
            {
                if (access.Kind == ResourceKind::Texture &&
                    HasRenderGraphUsage(access.Usage, RenderGraphUsage::Present) &&
                    mImpl->Textures[access.ResourceIndex].Lifetime !=
                        RenderGraphResourceLifetime::Imported)
                {
                    mImpl->AddError(
                        "Present usage requires an imported RenderGraph texture: " + pass.Name);
                }
            }
        }

        for (const Impl::TextureRecord& texture : mImpl->Textures)
        {
            if (texture.Lifetime == RenderGraphResourceLifetime::Imported &&
                (texture.InitialState == nvrhi::ResourceStates::Unknown ||
                    texture.FinalState == nvrhi::ResourceStates::Unknown))
            {
                mImpl->AddError(
                    "Imported RenderGraph texture requires explicit initial and final states: " +
                    texture.Desc.DebugName);
            }
        }
        for (const Impl::BufferRecord& buffer : mImpl->Buffers)
        {
            if (buffer.Lifetime == RenderGraphResourceLifetime::Imported &&
                (buffer.InitialState == nvrhi::ResourceStates::Unknown ||
                    buffer.FinalState == nvrhi::ResourceStates::Unknown))
            {
                mImpl->AddError(
                    "Imported RenderGraph buffer requires explicit initial and final states: " +
                    buffer.Desc.DebugName);
            }
        }
        for (const Impl::AccelerationStructureRecord& accelerationStructure :
            mImpl->AccelerationStructures)
        {
            if (accelerationStructure.InitialState == nvrhi::ResourceStates::Unknown ||
                accelerationStructure.FinalState == nvrhi::ResourceStates::Unknown)
            {
                mImpl->AddError(
                    "Imported RenderGraph acceleration structure requires explicit initial and final states: " +
                    accelerationStructure.Desc.DebugName);
            }
        }

        struct ResourcePassAccess
        {
            uint32_t PassIndex = InvalidRenderGraphIndex;
            RenderGraphAccess Access = RenderGraphAccess::Read;
            bool bDiscardAfter = false;
            bool bFullOverwrite = false;
        };

        // Index once by resource rather than rescanning every pass declaration
        // for each resource, mip and array slice. Entries retain declaration order
        // so same-pass merging and read-before-first-writer rules are unchanged.
        struct IndexedAccess
        {
            uint32_t PassIndex;
            const Impl::AccessDeclaration* Declaration;
        };
        std::vector<std::vector<IndexedAccess>> textureAccesses(mImpl->Textures.size());
        std::vector<std::vector<IndexedAccess>> bufferAccesses(mImpl->Buffers.size());
        std::vector<std::vector<IndexedAccess>> accelerationStructureAccesses(
            mImpl->AccelerationStructures.size());
        const auto resourceAccesses = [&](ResourceKind kind, uint32_t index)
            -> std::vector<IndexedAccess>&
        {
            switch (kind)
            {
            case ResourceKind::Texture: return textureAccesses[index];
            case ResourceKind::Buffer: return bufferAccesses[index];
            default: return accelerationStructureAccesses[index];
            }
        };
        for (uint32_t passIndex = 0; passIndex < mImpl->Passes.size(); ++passIndex)
            for (const Impl::AccessDeclaration& declaration : mImpl->Passes[passIndex].Accesses)
                resourceAccesses(declaration.Kind, declaration.ResourceIndex).push_back(
                    { passIndex, &declaration });

        auto buildResourceDependencies = [this, &resourceAccesses](
            ResourceKind kind,
            uint32_t resourceIndex,
            bool bInitiallyInitialized,
            std::string_view resourceName,
            uint32_t mip = 0,
            uint32_t slice = 0)
        {
            std::vector<ResourcePassAccess> accesses;
            const auto& indexedAccesses = resourceAccesses(kind, resourceIndex);
            for (size_t index = 0; index < indexedAccesses.size();)
            {
                const uint32_t passIndex = indexedAccesses[index].PassIndex;
                std::optional<RenderGraphAccess> merged;
                bool bDiscardAfter = false;
                bool bFullOverwrite = false;
                do
                {
                    const Impl::AccessDeclaration& declaration = *indexedAccesses[index++].Declaration;
                    if (kind == ResourceKind::Texture && !ContainsSubresource(declaration.Subresources, mip, slice))
                        continue;
                    merged = merged.has_value()
                        ? MergeAccess(*merged, declaration.Access)
                        : declaration.Access;
                    bDiscardAfter |= declaration.bAttachment &&
                        declaration.AttachmentOps.Store == RenderGraphAttachmentStore::Discard;
                    bFullOverwrite |= declaration.bAttachment &&
                        (declaration.AttachmentOps.Load == RenderGraphAttachmentLoad::Clear ||
                            (declaration.AttachmentOps.Load == RenderGraphAttachmentLoad::Discard &&
                                declaration.AttachmentOps.bFullOverwrite));
                } while (index < indexedAccesses.size() && indexedAccesses[index].PassIndex == passIndex);
                if (merged.has_value())
                    accesses.push_back({ passIndex, *merged, bDiscardAfter,
                        bFullOverwrite && *merged == RenderGraphAccess::Write });
            }

            if (accesses.empty())
            {
                if (kind == ResourceKind::Texture && mImpl->Textures[resourceIndex].bExported && !bInitiallyInitialized)
                    mImpl->AddError("RenderGraph exports uninitialized subresource: " + std::string(resourceName));
                return;
            }

            size_t startIndex = 0;
            std::optional<uint32_t> lastWriter;
            std::vector<uint32_t> readers;
            bool bContentsAvailable = bInitiallyInitialized;

            if (!bInitiallyInitialized)
            {
                const auto firstWriter = std::find_if(
                    accesses.begin(),
                    accesses.end(),
                    [](const ResourcePassAccess& access)
                    {
                        return AccessIncludesWrite(access.Access);
                    });
                if (firstWriter == accesses.end())
                {
                    mImpl->AddError(
                        "RenderGraph resource is read before initialization and has no writer: " +
                        std::string(resourceName));
                    return;
                }
                if (firstWriter->Access == RenderGraphAccess::ReadWrite)
                {
                    mImpl->AddError(
                        "RenderGraph resource's first writer requires uninitialized contents: " +
                        std::string(resourceName));
                    return;
                }

                const size_t writerIndex = static_cast<size_t>(
                    std::distance(accesses.begin(), firstWriter));
                if (writerIndex > 0 && firstWriter->bDiscardAfter)
                {
                    mImpl->AddError("RenderGraph reads discarded attachment contents: " + std::string(resourceName));
                    return;
                }
                for (size_t index = 0; index < writerIndex; ++index)
                {
                    mImpl->AddEdge(firstWriter->PassIndex, accesses[index].PassIndex);
                    readers.push_back(accesses[index].PassIndex);
                }

                lastWriter = firstWriter->PassIndex;
                bContentsAvailable = !firstWriter->bDiscardAfter;
                startIndex = writerIndex + 1;
            }

            for (size_t index = startIndex; index < accesses.size(); ++index)
            {
                const ResourcePassAccess& current = accesses[index];
                if (AccessIncludesRead(current.Access) && !bContentsAvailable)
                {
                    mImpl->AddError("RenderGraph reads discarded attachment contents: " + std::string(resourceName));
                    return;
                }
                switch (current.Access)
                {
                case RenderGraphAccess::Read:
                    if (lastWriter.has_value())
                        mImpl->AddEdge(*lastWriter, current.PassIndex);
                    if (std::find(readers.begin(), readers.end(), current.PassIndex) == readers.end())
                        readers.push_back(current.PassIndex);
                    break;
                case RenderGraphAccess::Write:
                case RenderGraphAccess::ReadWrite:
                    if (lastWriter.has_value())
                        mImpl->AddEdge(*lastWriter, current.PassIndex, false, !current.bFullOverwrite);
                    for (uint32_t reader : readers)
                        mImpl->AddEdge(reader, current.PassIndex, false, false);
                    readers.clear();
                    lastWriter = current.PassIndex;
                    bContentsAvailable = !current.bDiscardAfter;
                    break;
                }
            }
            if (kind == ResourceKind::Texture && mImpl->Textures[resourceIndex].bExported && !bContentsAvailable)
                mImpl->AddError("RenderGraph exports discarded attachment contents: " + std::string(resourceName));
        };

        for (uint32_t index = 0; index < mImpl->Textures.size(); ++index)
        {
            const Impl::TextureRecord& texture = mImpl->Textures[index];
            // Initialization and hazards are properties of each mip/slice, not
            // of the allocation. A depth-pyramid mip does not initialize its siblings.
            for (uint32_t slice = 0; slice < texture.Desc.ArrayLayers; ++slice)
            {
                for (uint32_t mip = 0; mip < texture.Desc.MipLevels; ++mip)
                {
                    const std::string name =
                        (texture.Desc.DebugName.empty() ? "<unnamed texture>" : texture.Desc.DebugName) +
                        " [mip " + std::to_string(mip) + ", slice " + std::to_string(slice) + "]";
                    buildResourceDependencies(
                        ResourceKind::Texture, index, texture.bInitiallyInitialized, name, mip, slice);
                }
            }
        }
        for (uint32_t index = 0; index < mImpl->Buffers.size(); ++index)
        {
            const Impl::BufferRecord& buffer = mImpl->Buffers[index];
            buildResourceDependencies(
                ResourceKind::Buffer,
                index,
                buffer.bInitiallyInitialized,
                buffer.Desc.DebugName.empty() ? "<unnamed buffer>" : buffer.Desc.DebugName);
        }
        for (uint32_t index = 0; index < mImpl->AccelerationStructures.size(); ++index)
        {
            const Impl::AccelerationStructureRecord& accelerationStructure =
                mImpl->AccelerationStructures[index];
            buildResourceDependencies(
                ResourceKind::AccelerationStructure,
                index,
                accelerationStructure.bInitiallyBuilt,
                accelerationStructure.Desc.DebugName.empty()
                    ? "<unnamed acceleration structure>"
                    : accelerationStructure.Desc.DebugName);
        }

        std::vector<uint32_t> indegree(mImpl->Passes.size(), 0);
        std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<>> ready;
        for (uint32_t passIndex = 0; passIndex < mImpl->Passes.size(); ++passIndex)
        {
            indegree[passIndex] = static_cast<uint32_t>(mImpl->Predecessors[passIndex].size());
            if (indegree[passIndex] == 0)
                ready.push(passIndex);
        }

        while (!ready.empty())
        {
            const uint32_t passIndex = ready.top();
            ready.pop();
            mImpl->TopologicalOrder.push_back(passIndex);

            std::vector<uint32_t> successors(
                mImpl->Successors[passIndex].begin(),
                mImpl->Successors[passIndex].end());
            std::sort(successors.begin(), successors.end());
            for (uint32_t successor : successors)
            {
                if (--indegree[successor] == 0)
                    ready.push(successor);
            }
        }

        if (mImpl->TopologicalOrder.size() != mImpl->Passes.size())
        {
            std::ostringstream cycle;
            cycle << "RenderGraph dependency cycle detected among passes:";
            for (uint32_t passIndex = 0; passIndex < indegree.size(); ++passIndex)
            {
                if (indegree[passIndex] != 0)
                    cycle << ' ' << mImpl->Passes[passIndex].Name;
            }
            mImpl->AddError(cycle.str());
        }

        if (!mImpl->CompileErrors.empty())
        {
            mImpl->bCompiled = false;
            mImpl->bDirty = true;
            return false;
        }

        std::vector<uint32_t> topologicalPosition(mImpl->Passes.size(), InvalidRenderGraphIndex);
        for (uint32_t position = 0; position < mImpl->TopologicalOrder.size(); ++position)
            topologicalPosition[mImpl->TopologicalOrder[position]] = position;

        std::vector<bool> active(mImpl->Passes.size(), false);
        std::vector<uint32_t> pendingRoots;
        auto markRoot = [&active, &pendingRoots](uint32_t passIndex)
        {
            if (!active[passIndex])
            {
                active[passIndex] = true;
                pendingRoots.push_back(passIndex);
            }
        };

        for (uint32_t passIndex = 0; passIndex < mImpl->Passes.size(); ++passIndex)
        {
            const Impl::PassRecord& pass = mImpl->Passes[passIndex];
            if (pass.bSideEffect)
                markRoot(passIndex);
            for (const Impl::AccessDeclaration& access : pass.Accesses)
            {
                if (HasRenderGraphUsage(access.Usage, RenderGraphUsage::Present))
                    markRoot(passIndex);
            }
        }

        auto markObservableLastAccess = [&resourceAccesses, &topologicalPosition, &markRoot](
            ResourceKind kind,
            uint32_t resourceIndex,
            bool bObservable,
            uint32_t mip = 0,
            uint32_t slice = 0)
        {
            if (!bObservable)
                return;

            uint32_t lastPass = InvalidRenderGraphIndex;
            uint32_t lastPosition = 0;
            for (const IndexedAccess& indexed : resourceAccesses(kind, resourceIndex))
            {
                if (kind == ResourceKind::Texture &&
                    !ContainsSubresource(indexed.Declaration->Subresources, mip, slice))
                    continue;
                const uint32_t passIndex = indexed.PassIndex;
                if (lastPass == InvalidRenderGraphIndex ||
                    topologicalPosition[passIndex] > lastPosition)
                {
                    lastPass = passIndex;
                    lastPosition = topologicalPosition[passIndex];
                }
            }
            if (lastPass != InvalidRenderGraphIndex)
                markRoot(lastPass);
        };

        for (uint32_t index = 0; index < mImpl->Textures.size(); ++index)
        {
            const Impl::TextureRecord& texture = mImpl->Textures[index];
            const bool bFinalStateObservable =
                texture.Lifetime == RenderGraphResourceLifetime::Imported &&
                texture.FinalState != texture.InitialState;
            for (uint32_t slice = 0; slice < texture.Desc.ArrayLayers; ++slice)
                for (uint32_t mip = 0; mip < texture.Desc.MipLevels; ++mip)
                    markObservableLastAccess(ResourceKind::Texture, index,
                        texture.bExported || texture.Lifetime == RenderGraphResourceLifetime::Persistent ||
                            bFinalStateObservable, mip, slice);
        }
        for (uint32_t index = 0; index < mImpl->Buffers.size(); ++index)
        {
            const Impl::BufferRecord& buffer = mImpl->Buffers[index];
            const bool bFinalStateObservable =
                buffer.Lifetime == RenderGraphResourceLifetime::Imported &&
                buffer.FinalState != buffer.InitialState;
            markObservableLastAccess(
                ResourceKind::Buffer,
                index,
                buffer.bExported ||
                    buffer.Lifetime == RenderGraphResourceLifetime::Persistent ||
                    bFinalStateObservable);
        }
        for (uint32_t index = 0; index < mImpl->AccelerationStructures.size(); ++index)
        {
            const Impl::AccelerationStructureRecord& accelerationStructure =
                mImpl->AccelerationStructures[index];
            const bool bFinalStateObservable =
                accelerationStructure.FinalState != accelerationStructure.InitialState;
            markObservableLastAccess(
                ResourceKind::AccelerationStructure,
                index,
                accelerationStructure.bExported || bFinalStateObservable);
        }

        while (!pendingRoots.empty())
        {
            const uint32_t passIndex = pendingRoots.back();
            pendingRoots.pop_back();
            // Ordering hazards remain in the topological graph, but only data
            // dependencies and explicit prerequisites make earlier work live.
            for (uint32_t predecessor : mImpl->ContentPredecessors[passIndex])
                markRoot(predecessor);
        }

        for (uint32_t passIndex : mImpl->TopologicalOrder)
        {
            Impl::PassRecord& pass = mImpl->Passes[passIndex];
            pass.bCulled = !active[passIndex];
            if (active[passIndex])
                mImpl->ExecutionOrder.push_back(passIndex);
        }

        for (uint32_t executionIndex = 0; executionIndex < mImpl->ExecutionOrder.size(); ++executionIndex)
        {
            const Impl::PassRecord& pass = mImpl->Passes[mImpl->ExecutionOrder[executionIndex]];
            for (const Impl::AccessDeclaration& access : pass.Accesses)
            {
                RenderGraphResourceLifetimeInfo* lifetime = nullptr;
                RenderGraphUsage* requiredUsage = nullptr;
                switch (access.Kind)
                {
                case ResourceKind::Texture:
                    lifetime = &mImpl->Textures[access.ResourceIndex].LifetimeInfo;
                    requiredUsage = &mImpl->Textures[access.ResourceIndex].RequiredUsage;
                    break;
                case ResourceKind::Buffer:
                    lifetime = &mImpl->Buffers[access.ResourceIndex].LifetimeInfo;
                    requiredUsage = &mImpl->Buffers[access.ResourceIndex].RequiredUsage;
                    break;
                case ResourceKind::AccelerationStructure:
                    lifetime = &mImpl->AccelerationStructures[access.ResourceIndex].LifetimeInfo;
                    requiredUsage = &mImpl->AccelerationStructures[access.ResourceIndex].RequiredUsage;
                    break;
                }
                if (!lifetime->IsUsed())
                    lifetime->FirstUse = executionIndex;
                lifetime->LastUse = executionIndex;
                *requiredUsage |= access.Usage;
            }
        }

        mImpl->bDirty = false;
        mImpl->bCompiled = true;
        return true;
    }

    void RenderGraph::CompileOrThrow()
    {
        if (!NeedsCompile() && IsCompiled())
            return;
        if (Compile())
            return;

        std::ostringstream message;
        message << "RenderGraph compilation failed";
        for (const std::string& error : mImpl->CompileErrors)
            message << "\n - " << error;
        throw RenderGraphValidationError(message.str());
    }

    void RenderGraph::Execute(
        nvrhi::ICommandList* commandList,
        IRenderGraphInstrumentation* instrumentation)
    {
        Execute(RenderGraphExecutionParams{
            .CommandList = commandList,
            .Instrumentation = instrumentation
        });
    }

    void RenderGraph::Execute(const RenderGraphExecutionParams& params)
    {
        if (!params.CommandList)
            throw RenderGraphValidationError("RenderGraph execution requires an NVRHI command list.");
        {
            CpuPhaseScope timing(params.CpuObserver, RenderGraphCpuPhase::Compile);
            CompileOrThrow();
        }

        {
            CpuPhaseScope timing(params.CpuObserver, RenderGraphCpuPhase::Realize);
            if (params.ResourcePool)
            {
                if (params.CommandList->getDevice() != params.ResourcePool->GetDevice())
                    throw RenderGraphValidationError("RenderGraph command list and resource pool belong to different NVRHI devices.");
                RealizeResources(*params.ResourcePool, params.FrameSlot);
            }
        }
        {
            CpuPhaseScope timing(params.CpuObserver, RenderGraphCpuPhase::Validate);
            ValidateBackendResourceUsages(params.CommandList->getDevice()->getGraphicsAPI());
            ValidatePhysicalResources();
        }
        ExecuteCompiled(params.CommandList, params.Instrumentation, params.CpuObserver);
    }

    void RenderGraph::RealizeResources(
        RenderGraphResourcePool& resourcePool,
        uint32_t frameSlot)
    {
        ValidateBackendResourceUsages(resourcePool.GetDevice()->getGraphicsAPI());
        if (!resourcePool.IsFrameActive(frameSlot))
        {
            throw RenderGraphValidationError(
                "RenderGraph execution requires RenderGraphResourcePool::BeginFrame for its frame slot.");
        }

        for (uint64_t staleNamespace : mImpl->StaleResourceNamespaces)
            resourcePool.ReleasePersistentResources(staleNamespace);
        mImpl->StaleResourceNamespaces.clear();

        for (uint32_t index = 0; index < mImpl->Textures.size(); ++index)
        {
            Impl::TextureRecord& texture = mImpl->Textures[index];
            if (texture.Lifetime == RenderGraphResourceLifetime::Imported ||
                !texture.LifetimeInfo.IsUsed())
            {
                continue;
            }

            texture.Resource = resourcePool.AcquireTexture(
                frameSlot,
                RenderGraphPoolResourceKey{
                    .GraphNamespace = mImpl->ResourceNamespace,
                    .ResourceIndex = index,
                    .ResourceGeneration = texture.Generation,
                    .Type = RenderGraphPoolResourceType::Texture
                },
                texture.Desc,
                texture.Lifetime,
                texture.RequiredUsage,
                texture.LifetimeInfo,
                texture.Lifetime == RenderGraphResourceLifetime::Transient &&
                    !texture.bExported && !texture.bInitiallyInitialized);
        }

        for (uint32_t index = 0; index < mImpl->Buffers.size(); ++index)
        {
            Impl::BufferRecord& buffer = mImpl->Buffers[index];
            if (buffer.Lifetime == RenderGraphResourceLifetime::Imported ||
                !buffer.LifetimeInfo.IsUsed())
            {
                continue;
            }

            buffer.Resource = resourcePool.AcquireBuffer(
                frameSlot,
                RenderGraphPoolResourceKey{
                    .GraphNamespace = mImpl->ResourceNamespace,
                    .ResourceIndex = index,
                    .ResourceGeneration = buffer.Generation,
                    .Type = RenderGraphPoolResourceType::Buffer
                },
                buffer.Desc,
                buffer.Lifetime,
                buffer.RequiredUsage,
                buffer.LifetimeInfo);
        }
    }

    void RenderGraph::ValidatePhysicalResources() const
    {
        for (const Impl::TextureRecord& texture : mImpl->Textures)
        {
            const bool bRequiresResource =
                texture.Lifetime == RenderGraphResourceLifetime::Imported ||
                texture.LifetimeInfo.IsUsed();
            if (bRequiresResource && !texture.Resource)
            {
                throw RenderGraphValidationError(
                    "RenderGraph texture has no physical NVRHI resource: " + texture.Desc.DebugName);
            }
        }
        for (const Impl::BufferRecord& buffer : mImpl->Buffers)
        {
            const bool bRequiresResource =
                buffer.Lifetime == RenderGraphResourceLifetime::Imported ||
                buffer.LifetimeInfo.IsUsed();
            if (bRequiresResource && !buffer.Resource)
            {
                throw RenderGraphValidationError(
                    "RenderGraph buffer has no physical NVRHI resource: " + buffer.Desc.DebugName);
            }
        }
        for (const Impl::AccelerationStructureRecord& accelerationStructure :
            mImpl->AccelerationStructures)
        {
            if (!accelerationStructure.Resource)
            {
                throw RenderGraphValidationError(
                    "RenderGraph acceleration structure has no physical NVRHI resource: " +
                    accelerationStructure.Desc.DebugName);
            }
        }
    }

    void RenderGraph::ExecuteCompiled(
        nvrhi::ICommandList* commandList,
        IRenderGraphInstrumentation* instrumentation,
        IRenderGraphCpuObserver* cpuObserver)
    {
        std::optional<CpuPhaseScope> setupTiming;
        if (cpuObserver) setupTiming.emplace(cpuObserver, RenderGraphCpuPhase::Setup);
        for (auto& pass : mImpl->Passes)
            pass.bHasRecordedCommands = false;
        const auto queue = commandList->getDesc().queueType;
        const RenderGraphQueue recordedQueue = queue == nvrhi::CommandQueue::Compute
            ? RenderGraphQueue::Compute : queue == nvrhi::CommandQueue::Copy
                ? RenderGraphQueue::Copy : RenderGraphQueue::Graphics;
        std::vector<bool> trackedTextures(mImpl->Textures.size(), false);
        std::unordered_set<nvrhi::ITexture*> trackedTransientTextures;
        std::vector<bool> trackedBuffers(mImpl->Buffers.size(), false);

        setupTiming.reset();

        for (uint32_t executionIndex = 0; executionIndex < mImpl->ExecutionOrder.size(); ++executionIndex)
        {
            const uint32_t passIndex = mImpl->ExecutionOrder[executionIndex];
            Impl::PassRecord& pass = mImpl->Passes[passIndex];
            {
                CpuPhaseScope timing(cpuObserver, RenderGraphCpuPhase::Barriers, executionIndex, pass.Name);
                bool bHasPendingBarriers = false;
                for (const Impl::AccessDeclaration& access : pass.Accesses)
                {
                    const nvrhi::ResourceStates state = MapUsageToResourceStates(access.Usage);
                    if (access.Kind == ResourceKind::Texture)
                    {
                        Impl::TextureRecord& texture = mImpl->Textures[access.ResourceIndex];
                        const bool bTransient = texture.Lifetime == RenderGraphResourceLifetime::Transient;
                        if (!trackedTextures[access.ResourceIndex] &&
                            (!bTransient || trackedTransientTextures.insert(texture.Resource.Get()).second))
                        {
                            commandList->beginTrackingTextureState(
                                texture.Resource,
                                nvrhi::TextureSubresourceSet(
                                    0,
                                    nvrhi::TextureSubresourceSet::AllMipLevels,
                                    0,
                                    nvrhi::TextureSubresourceSet::AllArraySlices),
                                texture.InitialState);
                            trackedTextures[access.ResourceIndex] = true;
                        }
                        commandList->setTextureState(
                            texture.Resource,
                            ToNvrhiSubresources(access.Subresources),
                            MapTextureUsageToResourceStates(access.Usage, texture.Desc.Format,
                                commandList->getDevice()->getGraphicsAPI()));
                        bHasPendingBarriers = true;
                    }
                    else if (access.Kind == ResourceKind::Buffer)
                    {
                        Impl::BufferRecord& buffer = mImpl->Buffers[access.ResourceIndex];
                        if (!trackedBuffers[access.ResourceIndex])
                        {
                            commandList->beginTrackingBufferState(buffer.Resource, buffer.InitialState);
                            trackedBuffers[access.ResourceIndex] = true;
                        }
                        commandList->setBufferState(buffer.Resource, state);
                        bHasPendingBarriers = true;
                    }
                    else
                    {
                        Impl::AccelerationStructureRecord& accelerationStructure =
                            mImpl->AccelerationStructures[access.ResourceIndex];
                        commandList->setAccelStructState(accelerationStructure.Resource, state);
                        bHasPendingBarriers = true;
                    }
                }
                if (bHasPendingBarriers)
                    commandList->commitBarriers();
            }

            // Record includes attachment clears, marker/query commands and the pass callback.
            CpuPhaseScope recordTiming(cpuObserver, RenderGraphCpuPhase::Record, executionIndex, pass.Name);
            commandList->beginMarker(pass.Name.c_str());
            nvrhi::ITimerQuery* timerQuery = instrumentation
                ? instrumentation->GetTimerQuery(executionIndex, pass.Name)
                : nullptr;
            if (instrumentation)
                instrumentation->OnPassBegin(executionIndex, pass.Name);
            if (timerQuery)
                commandList->beginTimerQuery(timerQuery);

            RenderGraphContext context(
                *this,
                RenderGraphPassHandle{ passIndex, pass.Generation },
                commandList);
            try
            {
                for (const auto& access : pass.Accesses)
                {
                    if (access.bAttachment && access.AttachmentOps.Load == RenderGraphAttachmentLoad::Clear)
                    {
                        const auto& texture = mImpl->Textures[access.ResourceIndex];
                        if (access.Usage == RenderGraphUsage::DepthWrite)
                            commandList->clearDepthStencilTexture(texture.Resource,
                                ToNvrhiSubresources(access.Subresources), true, access.AttachmentOps.ClearDepth,
                                nvrhi::getFormatInfo(texture.Desc.Format).hasStencil, access.AttachmentOps.ClearStencil);
                        else
                            commandList->clearTextureFloat(texture.Resource,
                                ToNvrhiSubresources(access.Subresources), access.AttachmentOps.ClearColor);
                    }
                }
                pass.Execute(context);
                pass.RecordedQueue = recordedQueue;
                pass.bHasRecordedCommands = true;
            }
            catch (...)
            {
                if (timerQuery)
                    commandList->endTimerQuery(timerQuery);
                if (instrumentation)
                    instrumentation->OnPassEnd(executionIndex, pass.Name);
                commandList->endMarker();
                throw;
            }

            if (timerQuery)
                commandList->endTimerQuery(timerQuery);
            if (instrumentation)
                instrumentation->OnPassEnd(executionIndex, pass.Name);
            commandList->endMarker();
        }

        CpuPhaseScope trailingTiming(cpuObserver, RenderGraphCpuPhase::Trailing);
        bool bHasFinalBarriers = false;
        std::unordered_set<nvrhi::ITexture*> finalizedTransientTextures;
        const bool bVulkan = commandList->getDevice()->getGraphicsAPI() ==
            nvrhi::GraphicsAPI::VULKAN;
        for (uint32_t index = 0; index < mImpl->Textures.size(); ++index)
        {
            Impl::TextureRecord& texture = mImpl->Textures[index];
            if (texture.FinalState == nvrhi::ResourceStates::Unknown)
                continue;
            // NVRHI represents a newly created Vulkan image's undefined layout
            // as Common. Undefined is valid only as an old layout, never as a
            // transition destination. Pool textures are overwritten according
            // to graph initialization contracts when their frame slot returns,
            // so leave their last valid layout in place instead of emitting an
            // invalid transition to Common at the graph boundary.
            if (bVulkan &&
                texture.FinalState == nvrhi::ResourceStates::Common)
            {
                continue;
            }
            if (!texture.Resource)
            {
                if (texture.LifetimeInfo.IsUsed() ||
                    texture.Lifetime == RenderGraphResourceLifetime::Imported)
                {
                    throw RenderGraphValidationError(
                        "RenderGraph texture lost its physical resource before final barriers: " +
                        texture.Desc.DebugName);
                }
                continue;
            }
            if (texture.Lifetime == RenderGraphResourceLifetime::Transient &&
                !finalizedTransientTextures.insert(texture.Resource.Get()).second)
                continue;
            if (!trackedTextures[index] &&
                (texture.Lifetime != RenderGraphResourceLifetime::Transient ||
                    trackedTransientTextures.insert(texture.Resource.Get()).second))
            {
                commandList->beginTrackingTextureState(
                    texture.Resource,
                    nvrhi::TextureSubresourceSet(
                        0,
                        nvrhi::TextureSubresourceSet::AllMipLevels,
                        0,
                        nvrhi::TextureSubresourceSet::AllArraySlices),
                    texture.InitialState);
            }
            commandList->setTextureState(
                texture.Resource,
                nvrhi::TextureSubresourceSet(
                    0,
                    nvrhi::TextureSubresourceSet::AllMipLevels,
                    0,
                    nvrhi::TextureSubresourceSet::AllArraySlices),
                texture.FinalState);
            bHasFinalBarriers = true;
        }
        for (uint32_t index = 0; index < mImpl->Buffers.size(); ++index)
        {
            Impl::BufferRecord& buffer = mImpl->Buffers[index];
            if (buffer.FinalState == nvrhi::ResourceStates::Unknown)
                continue;
            if (!buffer.Resource)
            {
                if (buffer.LifetimeInfo.IsUsed() ||
                    buffer.Lifetime == RenderGraphResourceLifetime::Imported)
                {
                    throw RenderGraphValidationError(
                        "RenderGraph buffer lost its physical resource before final barriers: " +
                        buffer.Desc.DebugName);
                }
                continue;
            }
            if (!trackedBuffers[index])
                commandList->beginTrackingBufferState(buffer.Resource, buffer.InitialState);
            commandList->setBufferState(buffer.Resource, buffer.FinalState);
            bHasFinalBarriers = true;
        }
        for (Impl::AccelerationStructureRecord& accelerationStructure :
            mImpl->AccelerationStructures)
        {
            if (accelerationStructure.FinalState == nvrhi::ResourceStates::Unknown)
                continue;
            if (!accelerationStructure.Resource)
            {
                throw RenderGraphValidationError(
                    "RenderGraph acceleration structure lost its physical resource before final barriers: " +
                    accelerationStructure.Desc.DebugName);
            }
            commandList->setAccelStructState(
                accelerationStructure.Resource,
                accelerationStructure.FinalState);
            bHasFinalBarriers = true;
        }
        if (bHasFinalBarriers)
            commandList->commitBarriers();
    }

    void RenderGraph::Resize(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
            throw RenderGraphValidationError("RenderGraph dimensions must be non-zero.");
        if (mImpl->Width == width && mImpl->Height == height)
            return;

        mImpl->Width = width;
        mImpl->Height = height;
        for (Impl::TextureRecord& texture : mImpl->Textures)
        {
            if (!texture.Desc.bRelativeToGraph ||
                texture.Lifetime == RenderGraphResourceLifetime::Imported)
            {
                continue;
            }
            texture.Desc.Width = std::max(
                1u,
                static_cast<uint32_t>(std::lround(
                    static_cast<double>(width) * texture.Desc.WidthScale)));
            texture.Desc.Height = std::max(
                1u,
                static_cast<uint32_t>(std::lround(
                    static_cast<double>(height) * texture.Desc.HeightScale)));
        }
        mImpl->MarkDirty();
    }

    void RenderGraph::Reserve(size_t passCount)
    {
        mImpl->Passes.reserve(passCount);
    }

    void RenderGraph::Clear()
    {
        if (!mImpl->Textures.empty() || !mImpl->Buffers.empty())
            mImpl->StaleResourceNamespaces.push_back(mImpl->ResourceNamespace);
        mImpl->Textures.clear();
        mImpl->Buffers.clear();
        mImpl->AccelerationStructures.clear();
        mImpl->Passes.clear();
        mImpl->Predecessors.clear();
        mImpl->ContentPredecessors.clear();
        mImpl->Successors.clear();
        mImpl->ResourceNamespace = AllocateResourceNamespace();
        mImpl->MarkDirty();
    }

    bool RenderGraph::NeedsCompile() const noexcept
    {
        return mImpl->bDirty;
    }

    bool RenderGraph::IsCompiled() const noexcept
    {
        return mImpl->bCompiled;
    }

    bool RenderGraph::IsValid(TextureHandle handle) const noexcept
    {
        return handle && handle.Index < mImpl->Textures.size() &&
            mImpl->Textures[handle.Index].Generation == handle.Generation;
    }

    bool RenderGraph::IsValid(BufferHandle handle) const noexcept
    {
        return handle && handle.Index < mImpl->Buffers.size() &&
            mImpl->Buffers[handle.Index].Generation == handle.Generation;
    }

    bool RenderGraph::IsValid(AccelerationStructureHandle handle) const noexcept
    {
        return handle && handle.Index < mImpl->AccelerationStructures.size() &&
            mImpl->AccelerationStructures[handle.Index].Generation == handle.Generation;
    }

    bool RenderGraph::IsValid(RenderGraphPassHandle handle) const noexcept
    {
        return handle && handle.Index < mImpl->Passes.size() &&
            mImpl->Passes[handle.Index].Generation == handle.Generation;
    }

    bool RenderGraph::IsPassCulled(RenderGraphPassHandle handle) const
    {
        if (!IsValid(handle))
            throw RenderGraphValidationError("Invalid or stale RenderGraph pass handle.");
        if (!IsCompiled())
            throw RenderGraphValidationError("RenderGraph must be compiled before querying pass culling.");
        return mImpl->Passes[handle.Index].bCulled;
    }

    const RenderGraphTextureDesc& RenderGraph::GetTextureDesc(TextureHandle handle) const
    {
        if (!IsValid(handle))
            throw RenderGraphValidationError("Invalid or stale RenderGraph texture handle.");
        return mImpl->Textures[handle.Index].Desc;
    }

    const RenderGraphBufferDesc& RenderGraph::GetBufferDesc(BufferHandle handle) const
    {
        if (!IsValid(handle))
            throw RenderGraphValidationError("Invalid or stale RenderGraph buffer handle.");
        return mImpl->Buffers[handle.Index].Desc;
    }

    const RenderGraphAccelerationStructureDesc&
        RenderGraph::GetAccelerationStructureDesc(AccelerationStructureHandle handle) const
    {
        if (!IsValid(handle))
        {
            throw RenderGraphValidationError(
                "Invalid or stale RenderGraph acceleration structure handle.");
        }
        return mImpl->AccelerationStructures[handle.Index].Desc;
    }

    RenderGraphResourceLifetime RenderGraph::GetLifetime(TextureHandle handle) const
    {
        if (!IsValid(handle))
            throw RenderGraphValidationError("Invalid or stale RenderGraph texture handle.");
        return mImpl->Textures[handle.Index].Lifetime;
    }

    RenderGraphResourceLifetime RenderGraph::GetLifetime(BufferHandle handle) const
    {
        if (!IsValid(handle))
            throw RenderGraphValidationError("Invalid or stale RenderGraph buffer handle.");
        return mImpl->Buffers[handle.Index].Lifetime;
    }

    RenderGraphResourceLifetime RenderGraph::GetLifetime(
        AccelerationStructureHandle handle) const
    {
        if (!IsValid(handle))
        {
            throw RenderGraphValidationError(
                "Invalid or stale RenderGraph acceleration structure handle.");
        }
        return RenderGraphResourceLifetime::Imported;
    }

    RenderGraphResourceLifetimeInfo RenderGraph::GetLifetimeInfo(TextureHandle handle) const
    {
        if (!IsValid(handle))
            throw RenderGraphValidationError("Invalid or stale RenderGraph texture handle.");
        if (!IsCompiled())
            throw RenderGraphValidationError("RenderGraph must be compiled before querying resource lifetime.");
        return mImpl->Textures[handle.Index].LifetimeInfo;
    }

    RenderGraphResourceLifetimeInfo RenderGraph::GetLifetimeInfo(BufferHandle handle) const
    {
        if (!IsValid(handle))
            throw RenderGraphValidationError("Invalid or stale RenderGraph buffer handle.");
        if (!IsCompiled())
            throw RenderGraphValidationError("RenderGraph must be compiled before querying resource lifetime.");
        return mImpl->Buffers[handle.Index].LifetimeInfo;
    }

    RenderGraphResourceLifetimeInfo RenderGraph::GetLifetimeInfo(
        AccelerationStructureHandle handle) const
    {
        if (!IsValid(handle))
        {
            throw RenderGraphValidationError(
                "Invalid or stale RenderGraph acceleration structure handle.");
        }
        if (!IsCompiled())
        {
            throw RenderGraphValidationError(
                "RenderGraph must be compiled before querying resource lifetime.");
        }
        return mImpl->AccelerationStructures[handle.Index].LifetimeInfo;
    }

    std::vector<std::string> RenderGraph::GetCompiledPassNames() const
    {
        std::vector<std::string> names;
        names.reserve(mImpl->ExecutionOrder.size());
        for (uint32_t passIndex : mImpl->ExecutionOrder)
            names.push_back(mImpl->Passes[passIndex].Name);
        return names;
    }

    const std::vector<std::string>& RenderGraph::GetCompileErrors() const noexcept
    {
        return mImpl->CompileErrors;
    }

    RenderGraphDiagnosticSnapshot RenderGraph::BuildDiagnosticSnapshot() const
    {
        RenderGraphDiagnosticSnapshot snapshot;
        snapshot.bCompiled = mImpl->bCompiled;
        snapshot.bDirty = mImpl->bDirty;
        snapshot.Width = mImpl->Width;
        snapshot.Height = mImpl->Height;
        snapshot.TopologicalOrder = mImpl->TopologicalOrder;
        snapshot.ExecutionOrder = mImpl->ExecutionOrder;

        std::vector<uint32_t> topologicalIndices(
            mImpl->Passes.size(),
            InvalidRenderGraphIndex);
        for (uint32_t index = 0; index < mImpl->TopologicalOrder.size(); ++index)
            topologicalIndices[mImpl->TopologicalOrder[index]] = index;

        std::vector<uint32_t> executionIndices(
            mImpl->Passes.size(),
            InvalidRenderGraphIndex);
        for (uint32_t index = 0; index < mImpl->ExecutionOrder.size(); ++index)
            executionIndices[mImpl->ExecutionOrder[index]] = index;

        snapshot.Passes.reserve(mImpl->Passes.size());
        for (uint32_t passId = 0; passId < mImpl->Passes.size(); ++passId)
        {
            const Impl::PassRecord& pass = mImpl->Passes[passId];
            snapshot.Passes.push_back(RenderGraphDiagnosticPassSnapshot{
                .PassId = passId,
                .TopologicalIndex = topologicalIndices[passId],
                .ExecutionIndex = executionIndices[passId],
                .Name = pass.Name,
                .FeatureTypeId = pass.FeatureTypeId,
                .FeatureInstanceId = pass.FeatureInstanceId,
                .Queue = pass.Queue,
                .bSideEffect = pass.bSideEffect,
                .bSetupComplete = pass.bSetupComplete,
                .bCulled = mImpl->bCompiled ? pass.bCulled : true,
                .bHasRecordedCommands = mImpl->bCompiled && !mImpl->bDirty && pass.bHasRecordedCommands,
                .RecordedQueue = pass.RecordedQueue,
            });

            for (const Impl::AccessDeclaration& access : pass.Accesses)
            {
                snapshot.Accesses.push_back(RenderGraphDiagnosticAccessSnapshot{
                    .PassId = passId,
                    .ResourceKind = ToDiagnosticResourceKind(access.Kind),
                    .ResourceId = access.ResourceIndex,
                    .Access = access.Access,
                    .Usage = access.Usage,
                    .Subresources = access.Subresources,
                    .bHasSubresources = access.Kind == ResourceKind::Texture,
                    .bAttachment = access.bAttachment,
                    .AttachmentOps = access.AttachmentOps,
                });
            }
        }

        snapshot.Resources.reserve(
            mImpl->Textures.size() +
            mImpl->Buffers.size() +
            mImpl->AccelerationStructures.size());
        std::unordered_map<nvrhi::ITexture*, uint32_t> physicalTextureGroups;
        for (uint32_t resourceId = 0; resourceId < mImpl->Textures.size(); ++resourceId)
        {
            const Impl::TextureRecord& texture = mImpl->Textures[resourceId];
            const nvrhi::FormatInfo& format = nvrhi::getFormatInfo(texture.Desc.Format);
            uint32_t physicalTextureGroup = 0;
            if (texture.Resource)
                physicalTextureGroup = physicalTextureGroups.try_emplace(texture.Resource.Get(),
                    static_cast<uint32_t>(physicalTextureGroups.size() + 1)).first->second;
            snapshot.Resources.push_back(RenderGraphDiagnosticResourceSnapshot{
                .ResourceId = resourceId,
                .Kind = RenderGraphDiagnosticResourceKind::Texture,
                .Name = texture.Desc.DebugName,
                .Lifetime = texture.Lifetime,
                .PassInterval = mImpl->bCompiled
                    ? texture.LifetimeInfo
                    : RenderGraphResourceLifetimeInfo{},
                .RequiredUsage = mImpl->bCompiled
                    ? texture.RequiredUsage
                    : RenderGraphUsage::None,
                .bInitiallyInitialized = texture.bInitiallyInitialized,
                .bExported = texture.bExported,
                .PhysicalTextureGroup = physicalTextureGroup,
                .Width = texture.Desc.Width,
                .Height = texture.Desc.Height,
                .Depth = texture.Desc.Depth,
                .MipLevels = texture.Desc.MipLevels,
                .ArrayLayers = texture.Desc.ArrayLayers,
                .SampleCount = texture.Desc.SampleCount,
                .FormatName = format.name ? format.name : "Unknown",
                .bRelativeToGraph = texture.Desc.bRelativeToGraph,
                .WidthScale = texture.Desc.WidthScale,
                .HeightScale = texture.Desc.HeightScale,
            });
        }
        for (uint32_t resourceId = 0; resourceId < mImpl->Buffers.size(); ++resourceId)
        {
            const Impl::BufferRecord& buffer = mImpl->Buffers[resourceId];
            snapshot.Resources.push_back(RenderGraphDiagnosticResourceSnapshot{
                .ResourceId = resourceId,
                .Kind = RenderGraphDiagnosticResourceKind::Buffer,
                .Name = buffer.Desc.DebugName,
                .Lifetime = buffer.Lifetime,
                .PassInterval = mImpl->bCompiled
                    ? buffer.LifetimeInfo
                    : RenderGraphResourceLifetimeInfo{},
                .RequiredUsage = mImpl->bCompiled
                    ? buffer.RequiredUsage
                    : RenderGraphUsage::None,
                .bInitiallyInitialized = buffer.bInitiallyInitialized,
                .bExported = buffer.bExported,
                .ByteSize = buffer.Desc.ByteSize,
                .StructStride = buffer.Desc.StructStride,
            });
        }
        for (uint32_t resourceId = 0;
            resourceId < mImpl->AccelerationStructures.size();
            ++resourceId)
        {
            const Impl::AccelerationStructureRecord& accelerationStructure =
                mImpl->AccelerationStructures[resourceId];
            snapshot.Resources.push_back(RenderGraphDiagnosticResourceSnapshot{
                .ResourceId = resourceId,
                .Kind = RenderGraphDiagnosticResourceKind::AccelerationStructure,
                .Name = accelerationStructure.Desc.DebugName,
                .Lifetime = RenderGraphResourceLifetime::Imported,
                .PassInterval = mImpl->bCompiled
                    ? accelerationStructure.LifetimeInfo
                    : RenderGraphResourceLifetimeInfo{},
                .RequiredUsage = mImpl->bCompiled
                    ? accelerationStructure.RequiredUsage
                    : RenderGraphUsage::None,
                .bInitiallyInitialized = accelerationStructure.bInitiallyBuilt,
                .bExported = accelerationStructure.bExported,
            });
        }

        if (mImpl->bCompiled)
        {
            for (uint32_t prerequisite = 0;
                prerequisite < mImpl->Successors.size();
                ++prerequisite)
            {
                std::vector<uint32_t> dependents(
                    mImpl->Successors[prerequisite].begin(),
                    mImpl->Successors[prerequisite].end());
                std::sort(dependents.begin(), dependents.end());
                for (uint32_t dependent : dependents)
                {
                    const std::vector<uint32_t>& explicitPrerequisites =
                        mImpl->Passes[dependent].ExplicitPrerequisites;
                    snapshot.Dependencies.push_back(RenderGraphDiagnosticDependencySnapshot{
                        .PrerequisitePassId = prerequisite,
                        .DependentPassId = dependent,
                        .bExplicit = std::ranges::find(
                            explicitPrerequisites,
                            prerequisite) != explicitPrerequisites.end(),
                    });
                }
            }
        }

        snapshot.CompileErrors.reserve(mImpl->CompileErrors.size());
        for (const std::string& error : mImpl->CompileErrors)
        {
            snapshot.CompileErrors.push_back(
                RenderGraphDiagnosticCompileErrorSnapshot{ .Message = error });
        }
        return snapshot;
    }

    std::string RenderGraph::BuildDebugDump() const
    {
        const RenderGraphDiagnosticSnapshot snapshot = BuildDiagnosticSnapshot();
        std::ostringstream dump;
        dump << "RenderGraph compiled=" << (snapshot.bCompiled ? "true" : "false")
             << " dirty=" << (snapshot.bDirty ? "true" : "false") << '\n';
        if (!snapshot.CompileErrors.empty())
        {
            dump << "Errors:\n";
            for (const RenderGraphDiagnosticCompileErrorSnapshot& error :
                snapshot.CompileErrors)
            {
                dump << "  - " << error.Message << '\n';
            }
        }

        dump << "ExecutionOrder:\n";
        for (uint32_t passId : snapshot.ExecutionOrder)
        {
            const RenderGraphDiagnosticPassSnapshot& pass = snapshot.Passes[passId];
            dump << "  [" << pass.ExecutionIndex << "] " << pass.Name
                 << " queue=" << ToString(pass.Queue) << '\n';
        }

        dump << "CulledPasses:\n";
        for (const RenderGraphDiagnosticPassSnapshot& pass : snapshot.Passes)
        {
            if (pass.bCulled)
                dump << "  " << pass.Name << '\n';
        }

        dump << "Textures:\n";
        for (const RenderGraphDiagnosticResourceSnapshot& resource : snapshot.Resources)
        {
            if (resource.Kind != RenderGraphDiagnosticResourceKind::Texture)
                continue;
            dump << "  [" << resource.ResourceId << "] "
                 << (resource.Name.empty() ? "<unnamed>" : resource.Name)
                 << " lifetime=" << ToString(resource.Lifetime)
                 << " first=" << resource.PassInterval.FirstUse
                 << " last=" << resource.PassInterval.LastUse << '\n';
        }

        dump << "Buffers:\n";
        for (const RenderGraphDiagnosticResourceSnapshot& resource : snapshot.Resources)
        {
            if (resource.Kind != RenderGraphDiagnosticResourceKind::Buffer)
                continue;
            dump << "  [" << resource.ResourceId << "] "
                 << (resource.Name.empty() ? "<unnamed>" : resource.Name)
                 << " lifetime=" << ToString(resource.Lifetime)
                 << " first=" << resource.PassInterval.FirstUse
                 << " last=" << resource.PassInterval.LastUse << '\n';
        }

        dump << "AccelerationStructures:\n";
        for (const RenderGraphDiagnosticResourceSnapshot& resource : snapshot.Resources)
        {
            if (resource.Kind !=
                RenderGraphDiagnosticResourceKind::AccelerationStructure)
            {
                continue;
            }
            dump << "  [" << resource.ResourceId << "] "
                 << (resource.Name.empty() ? "<unnamed>" : resource.Name)
                 << " lifetime=" << ToString(resource.Lifetime)
                 << " first=" << resource.PassInterval.FirstUse
                 << " last=" << resource.PassInterval.LastUse << '\n';
        }
        return dump.str();
    }

    void RenderGraph::ValidateBackendResourceUsages(nvrhi::GraphicsAPI api) const
    {
        if (api != nvrhi::GraphicsAPI::D3D12) return;
        for (const auto passIndex : mImpl->ExecutionOrder)
        {
            const auto& pass = mImpl->Passes[passIndex];
            for (const auto& access : pass.Accesses)
            {
                if (access.Kind != ResourceKind::Texture || !HasRenderGraphUsage(access.Usage, RenderGraphUsage::DepthRead)) continue;
                const auto& texture = mImpl->Textures[access.ResourceIndex];
                for (uint32_t slice = 0; slice < texture.Desc.ArrayLayers; ++slice)
                    for (uint32_t mip = 0; mip < texture.Desc.MipLevels; ++mip)
                    {
                        if (!ContainsSubresource(access.Subresources, mip, slice)) continue;
                        auto usage = access.Usage;
                        for (const auto& other : pass.Accesses)
                            if (other.Kind == ResourceKind::Texture && other.ResourceIndex == access.ResourceIndex &&
                                ContainsSubresource(other.Subresources, mip, slice)) usage = usage | other.Usage;
                        try { (void)MapTextureUsageToResourceStates(usage, texture.Desc.Format, api); }
                        catch (const RenderGraphValidationError& error)
                        {
                            throw RenderGraphValidationError("Pass '" + pass.Name + "', texture '" + texture.Desc.DebugName + "': " + error.what());
                        }
                    }
            }
        }
    }

    nvrhi::ResourceStates RenderGraph::MapTextureUsageToResourceStates(
        RenderGraphUsage usage, nvrhi::Format format, nvrhi::GraphicsAPI api)
    {
        if (api == nvrhi::GraphicsAPI::D3D12 && HasRenderGraphUsage(usage, RenderGraphUsage::DepthRead) &&
            usage != RenderGraphUsage::DepthRead)
            throw RenderGraphValidationError("D3D12 enhanced barriers do not support combining depth attachment reads with another texture layout; split passes or sample a depth snapshot");
        auto states = MapUsageToResourceStates(usage);
        const auto& info = nvrhi::getFormatInfo(format);
        // NVRHI depth/stencil SRV descriptors use the depth-read layout, even
        // without a simultaneous depth attachment. Keep the graph's explicit
        // barriers consistent while retaining shader-stage access visibility.
        if (api == nvrhi::GraphicsAPI::VULKAN && HasRenderGraphUsage(usage, RenderGraphUsage::ShaderResource) &&
            (info.hasDepth || info.hasStencil))
            states = states | nvrhi::ResourceStates::DepthRead;
        return states;
    }

    nvrhi::ResourceStates RenderGraph::MapUsageToResourceStates(RenderGraphUsage usage)
    {
        nvrhi::ResourceStates states = nvrhi::ResourceStates::Unknown;
        if (HasRenderGraphUsage(usage, RenderGraphUsage::RenderTarget))
            states = states | nvrhi::ResourceStates::RenderTarget;
        if (HasRenderGraphUsage(usage, RenderGraphUsage::DepthRead))
            states = states | nvrhi::ResourceStates::DepthRead;
        if (HasRenderGraphUsage(usage, RenderGraphUsage::DepthWrite))
            states = states | nvrhi::ResourceStates::DepthWrite;
        if (HasRenderGraphUsage(usage, RenderGraphUsage::ShaderResource))
            states = states | nvrhi::ResourceStates::ShaderResource;
        if (HasRenderGraphUsage(usage, RenderGraphUsage::UnorderedAccess))
            states = states | nvrhi::ResourceStates::UnorderedAccess;
        if (HasRenderGraphUsage(usage, RenderGraphUsage::CopySource))
            states = states | nvrhi::ResourceStates::CopySource;
        if (HasRenderGraphUsage(usage, RenderGraphUsage::CopyDest))
            states = states | nvrhi::ResourceStates::CopyDest;
        if (HasRenderGraphUsage(usage, RenderGraphUsage::ResolveSource))
            states = states | nvrhi::ResourceStates::ResolveSource;
        if (HasRenderGraphUsage(usage, RenderGraphUsage::ResolveDest))
            states = states | nvrhi::ResourceStates::ResolveDest;
        if (HasRenderGraphUsage(usage, RenderGraphUsage::Present))
            states = states | nvrhi::ResourceStates::Present;
        if (HasRenderGraphUsage(usage, RenderGraphUsage::VertexBuffer))
            states = states | nvrhi::ResourceStates::VertexBuffer;
        if (HasRenderGraphUsage(usage, RenderGraphUsage::IndexBuffer))
            states = states | nvrhi::ResourceStates::IndexBuffer;
        if (HasRenderGraphUsage(usage, RenderGraphUsage::ConstantBuffer))
            states = states | nvrhi::ResourceStates::ConstantBuffer;
        if (HasRenderGraphUsage(usage, RenderGraphUsage::IndirectArgument))
            states = states | nvrhi::ResourceStates::IndirectArgument;
        if (HasRenderGraphUsage(usage, RenderGraphUsage::AccelerationStructureBuildInput))
            states = states | nvrhi::ResourceStates::AccelStructBuildInput;
        if (HasRenderGraphUsage(usage, RenderGraphUsage::AccelerationStructureBuildWrite))
            states = states | nvrhi::ResourceStates::AccelStructWrite;
        if (HasRenderGraphUsage(usage, RenderGraphUsage::AccelerationStructureRead))
            states = states | nvrhi::ResourceStates::AccelStructRead;
        return states;
    }

    bool RenderGraph::IsTextureDeclared(
        RenderGraphPassHandle pass,
        TextureHandle handle,
        RenderGraphAccess requiredAccess) const
    {
        if (!IsValid(pass) || !IsValid(handle))
            return false;
        for (const Impl::AccessDeclaration& access : mImpl->Passes[pass.Index].Accesses)
        {
            if (access.Kind == ResourceKind::Texture &&
                access.ResourceIndex == handle.Index &&
                access.ResourceGeneration == handle.Generation &&
                AccessAllows(access.Access, requiredAccess))
            {
                return true;
            }
        }
        return false;
    }

    bool RenderGraph::IsBufferDeclared(
        RenderGraphPassHandle pass,
        BufferHandle handle,
        RenderGraphAccess requiredAccess) const
    {
        if (!IsValid(pass) || !IsValid(handle))
            return false;
        for (const Impl::AccessDeclaration& access : mImpl->Passes[pass.Index].Accesses)
        {
            if (access.Kind == ResourceKind::Buffer &&
                access.ResourceIndex == handle.Index &&
                access.ResourceGeneration == handle.Generation &&
                AccessAllows(access.Access, requiredAccess))
            {
                return true;
            }
        }
        return false;
    }

    bool RenderGraph::IsAccelerationStructureDeclared(
        RenderGraphPassHandle pass,
        AccelerationStructureHandle handle,
        RenderGraphAccess requiredAccess) const
    {
        if (!IsValid(pass) || !IsValid(handle))
            return false;
        for (const Impl::AccessDeclaration& access : mImpl->Passes[pass.Index].Accesses)
        {
            if (access.Kind == ResourceKind::AccelerationStructure &&
                access.ResourceIndex == handle.Index &&
                access.ResourceGeneration == handle.Generation &&
                AccessAllows(access.Access, requiredAccess))
            {
                return true;
            }
        }
        return false;
    }

    nvrhi::ITexture* RenderGraph::ResolveTexture(TextureHandle handle) const
    {
        if (!IsValid(handle))
            throw RenderGraphValidationError("Invalid or stale RenderGraph texture handle.");
        nvrhi::ITexture* texture = mImpl->Textures[handle.Index].Resource;
        if (!texture)
            throw RenderGraphValidationError("RenderGraph texture has no physical NVRHI resource.");
        return texture;
    }

    nvrhi::IBuffer* RenderGraph::ResolveBuffer(BufferHandle handle) const
    {
        if (!IsValid(handle))
            throw RenderGraphValidationError("Invalid or stale RenderGraph buffer handle.");
        nvrhi::IBuffer* buffer = mImpl->Buffers[handle.Index].Resource;
        if (!buffer)
            throw RenderGraphValidationError("RenderGraph buffer has no physical NVRHI resource.");
        return buffer;
    }

    nvrhi::rt::IAccelStruct* RenderGraph::ResolveAccelerationStructure(
        AccelerationStructureHandle handle) const
    {
        if (!IsValid(handle))
        {
            throw RenderGraphValidationError(
                "Invalid or stale RenderGraph acceleration structure handle.");
        }
        nvrhi::rt::IAccelStruct* accelerationStructure =
            mImpl->AccelerationStructures[handle.Index].Resource;
        if (!accelerationStructure)
        {
            throw RenderGraphValidationError(
                "RenderGraph acceleration structure has no physical NVRHI resource.");
        }
        return accelerationStructure;
    }

    std::string_view RenderGraph::GetPassName(RenderGraphPassHandle pass) const
    {
        if (!IsValid(pass))
            throw RenderGraphValidationError("Invalid or stale RenderGraph pass handle.");
        return mImpl->Passes[pass.Index].Name;
    }

    RenderGraphQueue RenderGraph::GetPassQueue(RenderGraphPassHandle pass) const
    {
        if (!IsValid(pass))
            throw RenderGraphValidationError("Invalid or stale RenderGraph pass handle.");
        return mImpl->Passes[pass.Index].Queue;
    }

    std::string_view RenderGraphContext::GetPassName() const
    {
        return mGraph.GetPassName(mPass);
    }

    RenderGraphQueue RenderGraphContext::GetQueue() const
    {
        return mGraph.GetPassQueue(mPass);
    }

    nvrhi::ITexture* RenderGraphContext::GetTexture(
        TextureHandle handle,
        RenderGraphAccess requiredAccess) const
    {
        if (!IsDeclared(handle, requiredAccess))
            throw RenderGraphValidationError("RenderGraph pass '" + std::string(GetPassName()) +
                "' accessed an undeclared texture (index " + std::to_string(handle.Index) + ").");
        return mGraph.ResolveTexture(handle);
    }

    nvrhi::IBuffer* RenderGraphContext::GetBuffer(
        BufferHandle handle,
        RenderGraphAccess requiredAccess) const
    {
        if (!IsDeclared(handle, requiredAccess))
            throw RenderGraphValidationError("RenderGraph pass accessed an undeclared buffer.");
        return mGraph.ResolveBuffer(handle);
    }

    nvrhi::rt::IAccelStruct* RenderGraphContext::GetAccelerationStructure(
        AccelerationStructureHandle handle,
        RenderGraphAccess requiredAccess) const
    {
        if (!IsDeclared(handle, requiredAccess))
        {
            throw RenderGraphValidationError(
                "RenderGraph pass accessed an undeclared acceleration structure.");
        }
        return mGraph.ResolveAccelerationStructure(handle);
    }

    const RenderGraphTextureDesc& RenderGraphContext::GetTextureDesc(TextureHandle handle) const
    {
        if (!mGraph.IsValid(handle))
            throw RenderGraphValidationError("Invalid or stale RenderGraph texture handle.");
        return mGraph.GetTextureDesc(handle);
    }

    const RenderGraphBufferDesc& RenderGraphContext::GetBufferDesc(BufferHandle handle) const
    {
        if (!mGraph.IsValid(handle))
            throw RenderGraphValidationError("Invalid or stale RenderGraph buffer handle.");
        return mGraph.GetBufferDesc(handle);
    }

    const RenderGraphAccelerationStructureDesc&
        RenderGraphContext::GetAccelerationStructureDesc(
            AccelerationStructureHandle handle) const
    {
        if (!mGraph.IsValid(handle))
        {
            throw RenderGraphValidationError(
                "Invalid or stale RenderGraph acceleration structure handle.");
        }
        return mGraph.GetAccelerationStructureDesc(handle);
    }

    bool RenderGraphContext::IsDeclared(
        TextureHandle handle,
        RenderGraphAccess requiredAccess) const
    {
        return mGraph.IsTextureDeclared(mPass, handle, requiredAccess);
    }

    bool RenderGraphContext::IsDeclared(
        BufferHandle handle,
        RenderGraphAccess requiredAccess) const
    {
        return mGraph.IsBufferDeclared(mPass, handle, requiredAccess);
    }

    bool RenderGraphContext::IsDeclared(
        AccelerationStructureHandle handle,
        RenderGraphAccess requiredAccess) const
    {
        return mGraph.IsAccelerationStructureDeclared(mPass, handle, requiredAccess);
    }

    TextureHandle RenderGraphBuilder::CreateTexture(
        const RenderGraphTextureDesc& desc,
        RenderGraphResourceLifetime lifetime,
        bool bInitiallyInitialized)
    {
        return mGraph.CreateTexture(desc, lifetime, bInitiallyInitialized);
    }

    BufferHandle RenderGraphBuilder::CreateBuffer(
        const RenderGraphBufferDesc& desc,
        RenderGraphResourceLifetime lifetime,
        bool bInitiallyInitialized)
    {
        return mGraph.CreateBuffer(desc, lifetime, bInitiallyInitialized);
    }

    TextureHandle RenderGraphBuilder::ImportTexture(
        const RenderGraphTextureDesc& desc,
        const nvrhi::TextureHandle& texture,
        nvrhi::ResourceStates initialState,
        nvrhi::ResourceStates finalState)
    {
        return mGraph.ImportTexture(desc, texture, initialState, finalState);
    }

    BufferHandle RenderGraphBuilder::ImportBuffer(
        const RenderGraphBufferDesc& desc,
        const nvrhi::BufferHandle& buffer,
        nvrhi::ResourceStates initialState,
        nvrhi::ResourceStates finalState)
    {
        return mGraph.ImportBuffer(desc, buffer, initialState, finalState);
    }

    AccelerationStructureHandle RenderGraphBuilder::ImportAccelerationStructure(
        const RenderGraphAccelerationStructureDesc& desc,
        const nvrhi::rt::AccelStructHandle& accelerationStructure,
        nvrhi::ResourceStates initialState,
        nvrhi::ResourceStates finalState,
        bool bInitiallyBuilt)
    {
        return mGraph.ImportAccelerationStructure(
            desc,
            accelerationStructure,
            initialState,
            finalState,
            bInitiallyBuilt);
    }

    TextureHandle RenderGraphBuilder::Read(
        TextureHandle handle,
        RenderGraphUsage usage,
        RenderGraphSubresourceRange subresources)
    {
        mGraph.DeclareTextureAccess(mPass, handle, RenderGraphAccess::Read, usage, subresources);
        return handle;
    }

    BufferHandle RenderGraphBuilder::Read(BufferHandle handle, RenderGraphUsage usage)
    {
        mGraph.DeclareBufferAccess(mPass, handle, RenderGraphAccess::Read, usage);
        return handle;
    }

    AccelerationStructureHandle RenderGraphBuilder::Read(
        AccelerationStructureHandle handle,
        RenderGraphUsage usage)
    {
        mGraph.DeclareAccelerationStructureAccess(
            mPass,
            handle,
            RenderGraphAccess::Read,
            usage);
        return handle;
    }

    TextureHandle RenderGraphBuilder::Write(
        TextureHandle handle,
        RenderGraphUsage usage,
        RenderGraphSubresourceRange subresources)
    {
        mGraph.DeclareTextureAccess(mPass, handle, RenderGraphAccess::Write, usage, subresources);
        return handle;
    }

    BufferHandle RenderGraphBuilder::Write(BufferHandle handle, RenderGraphUsage usage)
    {
        mGraph.DeclareBufferAccess(mPass, handle, RenderGraphAccess::Write, usage);
        return handle;
    }

    AccelerationStructureHandle RenderGraphBuilder::Write(
        AccelerationStructureHandle handle,
        RenderGraphUsage usage)
    {
        mGraph.DeclareAccelerationStructureAccess(
            mPass,
            handle,
            RenderGraphAccess::Write,
            usage);
        return handle;
    }

    TextureHandle RenderGraphBuilder::ReadWrite(
        TextureHandle handle,
        RenderGraphUsage usage,
        RenderGraphSubresourceRange subresources)
    {
        mGraph.DeclareTextureAccess(mPass, handle, RenderGraphAccess::ReadWrite, usage, subresources);
        return handle;
    }

    BufferHandle RenderGraphBuilder::ReadWrite(BufferHandle handle, RenderGraphUsage usage)
    {
        mGraph.DeclareBufferAccess(mPass, handle, RenderGraphAccess::ReadWrite, usage);
        return handle;
    }

    TextureHandle RenderGraphBuilder::RenderTarget(
        TextureHandle handle,
        RenderGraphSubresourceRange subresources)
    {
        return Write(handle, RenderGraphUsage::RenderTarget, subresources);
    }

    TextureHandle RenderGraphBuilder::ColorAttachment(TextureHandle handle,
        const RenderGraphAttachmentOps& ops, RenderGraphSubresourceRange subresources)
    {
        mGraph.DeclareAttachment(mPass, handle, ops, subresources, false);
        return handle;
    }

    TextureHandle RenderGraphBuilder::DepthAttachment(TextureHandle handle,
        const RenderGraphAttachmentOps& ops, RenderGraphSubresourceRange subresources)
    {
        mGraph.DeclareAttachment(mPass, handle, ops, subresources, true);
        return handle;
    }

    TextureHandle RenderGraphBuilder::DepthRead(
        TextureHandle handle,
        RenderGraphSubresourceRange subresources)
    {
        return Read(handle, RenderGraphUsage::DepthRead, subresources);
    }

    TextureHandle RenderGraphBuilder::DepthWrite(
        TextureHandle handle,
        RenderGraphSubresourceRange subresources)
    {
        return Write(handle, RenderGraphUsage::DepthWrite, subresources);
    }

    TextureHandle RenderGraphBuilder::ShaderResource(
        TextureHandle handle,
        RenderGraphSubresourceRange subresources)
    {
        return Read(handle, RenderGraphUsage::ShaderResource, subresources);
    }

    BufferHandle RenderGraphBuilder::ShaderResource(BufferHandle handle)
    {
        return Read(handle, RenderGraphUsage::ShaderResource);
    }

    TextureHandle RenderGraphBuilder::UnorderedAccess(
        TextureHandle handle,
        RenderGraphSubresourceRange subresources)
    {
        return ReadWrite(handle, RenderGraphUsage::UnorderedAccess, subresources);
    }

    BufferHandle RenderGraphBuilder::UnorderedAccess(BufferHandle handle)
    {
        return ReadWrite(handle, RenderGraphUsage::UnorderedAccess);
    }

    TextureHandle RenderGraphBuilder::CopySource(
        TextureHandle handle,
        RenderGraphSubresourceRange subresources)
    {
        return Read(handle, RenderGraphUsage::CopySource, subresources);
    }

    BufferHandle RenderGraphBuilder::CopySource(BufferHandle handle)
    {
        return Read(handle, RenderGraphUsage::CopySource);
    }

    TextureHandle RenderGraphBuilder::CopyDest(
        TextureHandle handle,
        RenderGraphSubresourceRange subresources)
    {
        return Write(handle, RenderGraphUsage::CopyDest, subresources);
    }

    BufferHandle RenderGraphBuilder::CopyDest(BufferHandle handle)
    {
        return Write(handle, RenderGraphUsage::CopyDest);
    }

    TextureHandle RenderGraphBuilder::ResolveSource(
        TextureHandle handle,
        RenderGraphSubresourceRange subresources)
    {
        return Read(handle, RenderGraphUsage::ResolveSource, subresources);
    }

    TextureHandle RenderGraphBuilder::ResolveDest(
        TextureHandle handle,
        RenderGraphSubresourceRange subresources)
    {
        return Write(handle, RenderGraphUsage::ResolveDest, subresources);
    }

    BufferHandle RenderGraphBuilder::VertexBuffer(BufferHandle handle)
    {
        return Read(handle, RenderGraphUsage::VertexBuffer);
    }

    BufferHandle RenderGraphBuilder::IndexBuffer(BufferHandle handle)
    {
        return Read(handle, RenderGraphUsage::IndexBuffer);
    }

    BufferHandle RenderGraphBuilder::ConstantBuffer(BufferHandle handle)
    {
        return Read(handle, RenderGraphUsage::ConstantBuffer);
    }

    BufferHandle RenderGraphBuilder::IndirectArgument(BufferHandle handle)
    {
        return Read(handle, RenderGraphUsage::IndirectArgument);
    }

    BufferHandle RenderGraphBuilder::AccelerationStructureBuildInput(BufferHandle handle)
    {
        return Read(handle, RenderGraphUsage::AccelerationStructureBuildInput);
    }

    AccelerationStructureHandle RenderGraphBuilder::AccelerationStructureBuildWrite(
        AccelerationStructureHandle handle)
    {
        return Write(handle, RenderGraphUsage::AccelerationStructureBuildWrite);
    }

    AccelerationStructureHandle RenderGraphBuilder::AccelerationStructureRead(
        AccelerationStructureHandle handle)
    {
        return Read(handle, RenderGraphUsage::AccelerationStructureRead);
    }

    TextureHandle RenderGraphBuilder::Present(TextureHandle handle)
    {
        return Read(handle, RenderGraphUsage::Present);
    }

    void RenderGraphBuilder::Export(TextureHandle handle)
    {
        mGraph.Export(handle);
    }

    void RenderGraphBuilder::Export(BufferHandle handle)
    {
        mGraph.Export(handle);
    }

    void RenderGraphBuilder::Export(AccelerationStructureHandle handle)
    {
        mGraph.Export(handle);
    }

    void RenderGraphBuilder::SetSideEffect(bool bSideEffect)
    {
        mGraph.SetPassSideEffect(mPass, bSideEffect);
    }

    void RenderGraphBuilder::SetQueue(RenderGraphQueue queue)
    {
        mGraph.SetPassQueue(mPass, queue);
    }

    void RenderGraphBuilder::DependsOn(RenderGraphPassHandle prerequisite)
    {
        mGraph.AddDependency(prerequisite, mPass);
    }
}
