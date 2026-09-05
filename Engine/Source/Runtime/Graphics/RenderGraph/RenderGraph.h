#pragma once

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace HEngine
{
    inline constexpr uint32_t InvalidRenderGraphIndex = std::numeric_limits<uint32_t>::max();

    struct TextureHandle
    {
        uint32_t Index = InvalidRenderGraphIndex;
        uint32_t Generation = 0;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return Index != InvalidRenderGraphIndex && Generation != 0;
        }

        [[nodiscard]] bool operator==(const TextureHandle&) const = default;
    };

    struct BufferHandle
    {
        uint32_t Index = InvalidRenderGraphIndex;
        uint32_t Generation = 0;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return Index != InvalidRenderGraphIndex && Generation != 0;
        }

        [[nodiscard]] bool operator==(const BufferHandle&) const = default;
    };

    struct AccelerationStructureHandle
    {
        uint32_t Index = InvalidRenderGraphIndex;
        uint32_t Generation = 0;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return Index != InvalidRenderGraphIndex && Generation != 0;
        }

        [[nodiscard]] bool operator==(const AccelerationStructureHandle&) const = default;
    };

    struct RenderGraphPassHandle
    {
        uint32_t Index = InvalidRenderGraphIndex;
        uint32_t Generation = 0;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return Index != InvalidRenderGraphIndex && Generation != 0;
        }

        [[nodiscard]] bool operator==(const RenderGraphPassHandle&) const = default;
    };

    enum class RenderGraphResourceLifetime : uint8_t
    {
        Imported,
        Persistent,
        Transient,
    };

    enum class RenderGraphQueue : uint8_t
    {
        Graphics,
        Compute,
        Copy,
    };

    enum class RenderGraphAccess : uint8_t
    {
        Read,
        Write,
        ReadWrite,
    };

    enum class RenderGraphUsage : uint32_t
    {
        None = 0,
        RenderTarget = 1u << 0,
        DepthRead = 1u << 1,
        DepthWrite = 1u << 2,
        ShaderResource = 1u << 3,
        UnorderedAccess = 1u << 4,
        CopySource = 1u << 5,
        CopyDest = 1u << 6,
        Present = 1u << 7,
        VertexBuffer = 1u << 8,
        IndexBuffer = 1u << 9,
        ConstantBuffer = 1u << 10,
        IndirectArgument = 1u << 11,
        AccelerationStructureBuildInput = 1u << 12,
        AccelerationStructureBuildWrite = 1u << 13,
        AccelerationStructureRead = 1u << 14,
        ResolveSource = 1u << 15,
        ResolveDest = 1u << 16,
    };

    [[nodiscard]] constexpr RenderGraphUsage operator|(RenderGraphUsage left, RenderGraphUsage right) noexcept
    {
        return static_cast<RenderGraphUsage>(
            static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
    }

    [[nodiscard]] constexpr RenderGraphUsage operator&(RenderGraphUsage left, RenderGraphUsage right) noexcept
    {
        return static_cast<RenderGraphUsage>(
            static_cast<uint32_t>(left) & static_cast<uint32_t>(right));
    }

    constexpr RenderGraphUsage& operator|=(RenderGraphUsage& left, RenderGraphUsage right) noexcept
    {
        left = left | right;
        return left;
    }

    [[nodiscard]] constexpr bool HasRenderGraphUsage(
        RenderGraphUsage value,
        RenderGraphUsage flag) noexcept
    {
        return (value & flag) != RenderGraphUsage::None;
    }

    struct RenderGraphSubresourceRange
    {
        static constexpr uint32_t AllRemaining = std::numeric_limits<uint32_t>::max();

        uint32_t BaseMipLevel = 0;
        uint32_t MipLevelCount = AllRemaining;
        uint32_t BaseArraySlice = 0;
        uint32_t ArraySliceCount = AllRemaining;

        [[nodiscard]] bool operator==(const RenderGraphSubresourceRange&) const = default;
    };

    struct RenderGraphTextureDesc
    {
        std::string DebugName;
        uint32_t Width = 1;
        uint32_t Height = 1;
        uint32_t Depth = 1;
        uint32_t MipLevels = 1;
        uint32_t ArrayLayers = 1;
        uint32_t SampleCount = 1;
        nvrhi::Format Format = nvrhi::Format::UNKNOWN;
        nvrhi::TextureDimension Dimension = nvrhi::TextureDimension::Texture2D;
        nvrhi::Color ClearValue;
        bool bUseClearValue = false;

        // Relative resources are resized when RenderGraph::Resize is called.
        bool bRelativeToGraph = false;
        float WidthScale = 1.0f;
        float HeightScale = 1.0f;

        // Allocation capabilities needed by commands recorded inside a pass but
        // not represented by that pass's externally visible resource state.
        // This does not declare an access, dependency, or state transition.
        RenderGraphUsage CapabilityUsage = RenderGraphUsage::None;
    };

    struct RenderGraphBufferDesc
    {
        std::string DebugName;
        uint64_t ByteSize = 0;
        uint32_t StructStride = 0;
    };

    struct RenderGraphAccelerationStructureDesc
    {
        std::string DebugName;
    };

    struct RenderGraphResourceLifetimeInfo
    {
        uint32_t FirstUse = InvalidRenderGraphIndex;
        uint32_t LastUse = InvalidRenderGraphIndex;

        [[nodiscard]] bool IsUsed() const noexcept
        {
            return FirstUse != InvalidRenderGraphIndex;
        }
    };

    enum class RenderGraphDiagnosticResourceKind : uint8_t
    {
        Texture,
        Buffer,
        AccelerationStructure,
    };

    // Diagnostic IDs are local to one detached snapshot. PassId addresses the
    // Passes list; ResourceId is local to its ResourceKind. They are deliberately
    // not RenderGraph handles and cannot be used to access runtime resources.
    struct RenderGraphDiagnosticPassSnapshot
    {
        uint32_t PassId = InvalidRenderGraphIndex;
        uint32_t TopologicalIndex = InvalidRenderGraphIndex;
        uint32_t ExecutionIndex = InvalidRenderGraphIndex;
        std::string Name;
        RenderGraphQueue Queue = RenderGraphQueue::Graphics;
        bool bSideEffect = false;
        bool bSetupComplete = false;
        bool bCulled = true;
    };

    struct RenderGraphDiagnosticResourceSnapshot
    {
        uint32_t ResourceId = InvalidRenderGraphIndex;
        RenderGraphDiagnosticResourceKind Kind =
            RenderGraphDiagnosticResourceKind::Texture;
        std::string Name;
        RenderGraphResourceLifetime Lifetime = RenderGraphResourceLifetime::Transient;
        RenderGraphResourceLifetimeInfo PassInterval;
        RenderGraphUsage RequiredUsage = RenderGraphUsage::None;
        bool bInitiallyInitialized = false;
        bool bExported = false;

        // Texture-only descriptor values. FormatName is copied from NVRHI into
        // owned text so the snapshot itself contains no NVRHI type or object.
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t Depth = 0;
        uint32_t MipLevels = 0;
        uint32_t ArrayLayers = 0;
        uint32_t SampleCount = 0;
        std::string FormatName;
        bool bRelativeToGraph = false;
        float WidthScale = 0.0f;
        float HeightScale = 0.0f;

        // Buffer-only descriptor values.
        uint64_t ByteSize = 0;
        uint32_t StructStride = 0;
    };

    struct RenderGraphDiagnosticAccessSnapshot
    {
        uint32_t PassId = InvalidRenderGraphIndex;
        RenderGraphDiagnosticResourceKind ResourceKind =
            RenderGraphDiagnosticResourceKind::Texture;
        uint32_t ResourceId = InvalidRenderGraphIndex;
        RenderGraphAccess Access = RenderGraphAccess::Read;
        RenderGraphUsage Usage = RenderGraphUsage::None;
        RenderGraphSubresourceRange Subresources;
        bool bHasSubresources = false;
    };

    struct RenderGraphDiagnosticDependencySnapshot
    {
        uint32_t PrerequisitePassId = InvalidRenderGraphIndex;
        uint32_t DependentPassId = InvalidRenderGraphIndex;
        bool bExplicit = false;
    };

    struct RenderGraphDiagnosticCompileErrorSnapshot
    {
        std::string Message;
    };

    // An owned, immutable-by-convention value snapshot. Consumers may retain
    // or copy it after the graph changes without keeping any graph/NVRHI object
    // alive. All IDs describe relationships only within this snapshot.
    struct RenderGraphDiagnosticSnapshot
    {
        bool bCompiled = false;
        bool bDirty = true;
        uint32_t Width = 0;
        uint32_t Height = 0;
        std::vector<uint32_t> TopologicalOrder;
        std::vector<uint32_t> ExecutionOrder;
        std::vector<RenderGraphDiagnosticPassSnapshot> Passes;
        std::vector<RenderGraphDiagnosticResourceSnapshot> Resources;
        std::vector<RenderGraphDiagnosticAccessSnapshot> Accesses;
        std::vector<RenderGraphDiagnosticDependencySnapshot> Dependencies;
        std::vector<RenderGraphDiagnosticCompileErrorSnapshot> CompileErrors;
    };

    class RenderGraphValidationError : public std::logic_error
    {
    public:
        using std::logic_error::logic_error;
    };

    class RenderGraph;
    class RenderGraphResourcePool;

    class IRenderGraphInstrumentation
    {
    public:
        virtual ~IRenderGraphInstrumentation() = default;

        // Query ownership stays with the instrumentation implementation. Returning
        // nullptr disables timestamps for the pass without affecting markers.
        [[nodiscard]] virtual nvrhi::ITimerQuery* GetTimerQuery(
            uint32_t executionIndex,
            std::string_view passName)
        {
            (void)executionIndex;
            (void)passName;
            return nullptr;
        }

        virtual void OnPassBegin(uint32_t executionIndex, std::string_view passName)
        {
            (void)executionIndex;
            (void)passName;
        }

        virtual void OnPassEnd(uint32_t executionIndex, std::string_view passName)
        {
            (void)executionIndex;
            (void)passName;
        }
    };

    struct RenderGraphExecutionParams
    {
        nvrhi::ICommandList* CommandList = nullptr;
        RenderGraphResourcePool* ResourcePool = nullptr;
        IRenderGraphInstrumentation* Instrumentation = nullptr;
        uint32_t FrameSlot = 0;
    };

    class RenderGraphContext
    {
    public:
        [[nodiscard]] nvrhi::ICommandList* GetCommandList() const noexcept { return mCommandList; }
        [[nodiscard]] std::string_view GetPassName() const;
        [[nodiscard]] RenderGraphQueue GetQueue() const;

        [[nodiscard]] nvrhi::ITexture* GetTexture(
            TextureHandle handle,
            RenderGraphAccess requiredAccess = RenderGraphAccess::Read) const;
        [[nodiscard]] nvrhi::IBuffer* GetBuffer(
            BufferHandle handle,
            RenderGraphAccess requiredAccess = RenderGraphAccess::Read) const;
        [[nodiscard]] nvrhi::rt::IAccelStruct* GetAccelerationStructure(
            AccelerationStructureHandle handle,
            RenderGraphAccess requiredAccess = RenderGraphAccess::Read) const;

        [[nodiscard]] const RenderGraphTextureDesc& GetTextureDesc(TextureHandle handle) const;
        [[nodiscard]] const RenderGraphBufferDesc& GetBufferDesc(BufferHandle handle) const;
        [[nodiscard]] const RenderGraphAccelerationStructureDesc& GetAccelerationStructureDesc(
            AccelerationStructureHandle handle) const;
        [[nodiscard]] bool IsDeclared(TextureHandle handle, RenderGraphAccess requiredAccess) const;
        [[nodiscard]] bool IsDeclared(BufferHandle handle, RenderGraphAccess requiredAccess) const;
        [[nodiscard]] bool IsDeclared(
            AccelerationStructureHandle handle,
            RenderGraphAccess requiredAccess) const;

    private:
        friend class RenderGraph;

        RenderGraphContext(
            const RenderGraph& graph,
            RenderGraphPassHandle pass,
            nvrhi::ICommandList* commandList)
            : mGraph(graph), mPass(pass), mCommandList(commandList)
        {
        }

        const RenderGraph& mGraph;
        RenderGraphPassHandle mPass;
        nvrhi::ICommandList* mCommandList = nullptr;
    };

    class RenderGraphBuilder
    {
    public:
        [[nodiscard]] TextureHandle CreateTexture(
            const RenderGraphTextureDesc& desc,
            RenderGraphResourceLifetime lifetime = RenderGraphResourceLifetime::Transient,
            bool bInitiallyInitialized = false);
        [[nodiscard]] BufferHandle CreateBuffer(
            const RenderGraphBufferDesc& desc,
            RenderGraphResourceLifetime lifetime = RenderGraphResourceLifetime::Transient,
            bool bInitiallyInitialized = false);

        [[nodiscard]] TextureHandle ImportTexture(
            const RenderGraphTextureDesc& desc,
            const nvrhi::TextureHandle& texture,
            nvrhi::ResourceStates initialState,
            nvrhi::ResourceStates finalState);
        [[nodiscard]] BufferHandle ImportBuffer(
            const RenderGraphBufferDesc& desc,
            const nvrhi::BufferHandle& buffer,
            nvrhi::ResourceStates initialState,
            nvrhi::ResourceStates finalState);
        [[nodiscard]] AccelerationStructureHandle ImportAccelerationStructure(
            const RenderGraphAccelerationStructureDesc& desc,
            const nvrhi::rt::AccelStructHandle& accelerationStructure,
            nvrhi::ResourceStates initialState,
            nvrhi::ResourceStates finalState,
            bool bInitiallyBuilt = true);

        TextureHandle Read(
            TextureHandle handle,
            RenderGraphUsage usage = RenderGraphUsage::ShaderResource,
            RenderGraphSubresourceRange subresources = {});
        BufferHandle Read(
            BufferHandle handle,
            RenderGraphUsage usage = RenderGraphUsage::ShaderResource);
        AccelerationStructureHandle Read(
            AccelerationStructureHandle handle,
            RenderGraphUsage usage = RenderGraphUsage::AccelerationStructureRead);
        TextureHandle Write(
            TextureHandle handle,
            RenderGraphUsage usage = RenderGraphUsage::UnorderedAccess,
            RenderGraphSubresourceRange subresources = {});
        BufferHandle Write(
            BufferHandle handle,
            RenderGraphUsage usage = RenderGraphUsage::UnorderedAccess);
        AccelerationStructureHandle Write(
            AccelerationStructureHandle handle,
            RenderGraphUsage usage = RenderGraphUsage::AccelerationStructureBuildWrite);
        TextureHandle ReadWrite(
            TextureHandle handle,
            RenderGraphUsage usage = RenderGraphUsage::UnorderedAccess,
            RenderGraphSubresourceRange subresources = {});
        BufferHandle ReadWrite(
            BufferHandle handle,
            RenderGraphUsage usage = RenderGraphUsage::UnorderedAccess);

        TextureHandle RenderTarget(TextureHandle handle, RenderGraphSubresourceRange subresources = {});
        TextureHandle DepthRead(TextureHandle handle, RenderGraphSubresourceRange subresources = {});
        TextureHandle DepthWrite(TextureHandle handle, RenderGraphSubresourceRange subresources = {});
        TextureHandle ShaderResource(TextureHandle handle, RenderGraphSubresourceRange subresources = {});
        BufferHandle ShaderResource(BufferHandle handle);
        TextureHandle UnorderedAccess(TextureHandle handle, RenderGraphSubresourceRange subresources = {});
        BufferHandle UnorderedAccess(BufferHandle handle);
        TextureHandle CopySource(TextureHandle handle, RenderGraphSubresourceRange subresources = {});
        BufferHandle CopySource(BufferHandle handle);
        TextureHandle CopyDest(TextureHandle handle, RenderGraphSubresourceRange subresources = {});
        BufferHandle CopyDest(BufferHandle handle);
        TextureHandle ResolveSource(
            TextureHandle handle,
            RenderGraphSubresourceRange subresources = {});
        TextureHandle ResolveDest(
            TextureHandle handle,
            RenderGraphSubresourceRange subresources = {});
        BufferHandle VertexBuffer(BufferHandle handle);
        BufferHandle IndexBuffer(BufferHandle handle);
        BufferHandle ConstantBuffer(BufferHandle handle);
        BufferHandle IndirectArgument(BufferHandle handle);
        BufferHandle AccelerationStructureBuildInput(BufferHandle handle);
        AccelerationStructureHandle AccelerationStructureBuildWrite(
            AccelerationStructureHandle handle);
        AccelerationStructureHandle AccelerationStructureRead(
            AccelerationStructureHandle handle);
        TextureHandle Present(TextureHandle handle);

        void Export(TextureHandle handle);
        void Export(BufferHandle handle);
        void Export(AccelerationStructureHandle handle);
        void SetSideEffect(bool bSideEffect = true);
        void SetQueue(RenderGraphQueue queue);
        void DependsOn(RenderGraphPassHandle prerequisite);

    private:
        friend class RenderGraph;

        RenderGraphBuilder(RenderGraph& graph, RenderGraphPassHandle pass)
            : mGraph(graph), mPass(pass)
        {
        }

        RenderGraph& mGraph;
        RenderGraphPassHandle mPass;
    };

    class RenderGraph
    {
    public:
        RenderGraph();
        ~RenderGraph();

        RenderGraph(const RenderGraph&) = delete;
        RenderGraph& operator=(const RenderGraph&) = delete;

        template<typename PassData, typename SetupFunc, typename ExecuteFunc>
        RenderGraphPassHandle AddPass(
            std::string name,
            SetupFunc&& setup,
            ExecuteFunc&& execute)
        {
            return AddPass<PassData>(
                std::move(name),
                RenderGraphQueue::Graphics,
                std::forward<SetupFunc>(setup),
                std::forward<ExecuteFunc>(execute));
        }

        template<typename PassData, typename SetupFunc, typename ExecuteFunc>
        RenderGraphPassHandle AddPass(
            std::string name,
            RenderGraphQueue queue,
            SetupFunc&& setup,
            ExecuteFunc&& execute)
        {
            static_assert(std::is_default_constructible_v<PassData>,
                "RenderGraph pass data must be default constructible");
            static_assert(std::is_invocable_v<SetupFunc&, RenderGraphBuilder&, PassData&>,
                "RenderGraph setup must be invocable with (RenderGraphBuilder&, PassData&)");
            static_assert(std::is_invocable_v<ExecuteFunc&, RenderGraphContext&, const PassData&>,
                "RenderGraph execute must be invocable with (RenderGraphContext&, const PassData&)");

            auto passData = std::make_shared<PassData>();
            auto executeFunction =
                std::make_shared<std::decay_t<ExecuteFunc>>(std::forward<ExecuteFunc>(execute));
            RenderGraphPassHandle pass = AddPassInternal(
                std::move(name),
                queue,
                [passData, executeFunction](RenderGraphContext& context)
                {
                    std::invoke(*executeFunction, context, std::as_const(*passData));
                });

            try
            {
                RenderGraphBuilder builder(*this, pass);
                std::invoke(setup, builder, *passData);
                FinishPassSetup(pass);
            }
            catch (...)
            {
                AbortPassSetup(pass);
                throw;
            }

            return pass;
        }

        [[nodiscard]] TextureHandle CreateTexture(
            const RenderGraphTextureDesc& desc,
            RenderGraphResourceLifetime lifetime = RenderGraphResourceLifetime::Transient,
            bool bInitiallyInitialized = false);
        [[nodiscard]] BufferHandle CreateBuffer(
            const RenderGraphBufferDesc& desc,
            RenderGraphResourceLifetime lifetime = RenderGraphResourceLifetime::Transient,
            bool bInitiallyInitialized = false);
        [[nodiscard]] TextureHandle ImportTexture(
            const RenderGraphTextureDesc& desc,
            const nvrhi::TextureHandle& texture,
            nvrhi::ResourceStates initialState,
            nvrhi::ResourceStates finalState);
        [[nodiscard]] BufferHandle ImportBuffer(
            const RenderGraphBufferDesc& desc,
            const nvrhi::BufferHandle& buffer,
            nvrhi::ResourceStates initialState,
            nvrhi::ResourceStates finalState);
        [[nodiscard]] AccelerationStructureHandle ImportAccelerationStructure(
            const RenderGraphAccelerationStructureDesc& desc,
            const nvrhi::rt::AccelStructHandle& accelerationStructure,
            nvrhi::ResourceStates initialState,
            nvrhi::ResourceStates finalState,
            bool bInitiallyBuilt = true);

        void SetTextureResource(TextureHandle handle, const nvrhi::TextureHandle& texture);
        void SetBufferResource(BufferHandle handle, const nvrhi::BufferHandle& buffer);
        void SetAccelerationStructureResource(
            AccelerationStructureHandle handle,
            const nvrhi::rt::AccelStructHandle& accelerationStructure);
        void Export(TextureHandle handle);
        void Export(BufferHandle handle);
        void Export(AccelerationStructureHandle handle);
        void AddDependency(RenderGraphPassHandle prerequisite, RenderGraphPassHandle dependent);

        [[nodiscard]] bool Compile();
        void CompileOrThrow();
        void Execute(
            nvrhi::ICommandList* commandList,
            IRenderGraphInstrumentation* instrumentation = nullptr);
        void Execute(const RenderGraphExecutionParams& params);

        void Resize(uint32_t width, uint32_t height);
        void Reserve(size_t passCount);
        void Clear();

        [[nodiscard]] bool NeedsCompile() const noexcept;
        [[nodiscard]] bool IsCompiled() const noexcept;
        [[nodiscard]] bool IsValid(TextureHandle handle) const noexcept;
        [[nodiscard]] bool IsValid(BufferHandle handle) const noexcept;
        [[nodiscard]] bool IsValid(AccelerationStructureHandle handle) const noexcept;
        [[nodiscard]] bool IsValid(RenderGraphPassHandle handle) const noexcept;
        [[nodiscard]] bool IsPassCulled(RenderGraphPassHandle handle) const;

        [[nodiscard]] const RenderGraphTextureDesc& GetTextureDesc(TextureHandle handle) const;
        [[nodiscard]] const RenderGraphBufferDesc& GetBufferDesc(BufferHandle handle) const;
        [[nodiscard]] const RenderGraphAccelerationStructureDesc& GetAccelerationStructureDesc(
            AccelerationStructureHandle handle) const;
        [[nodiscard]] RenderGraphResourceLifetime GetLifetime(TextureHandle handle) const;
        [[nodiscard]] RenderGraphResourceLifetime GetLifetime(BufferHandle handle) const;
        [[nodiscard]] RenderGraphResourceLifetime GetLifetime(
            AccelerationStructureHandle handle) const;
        [[nodiscard]] RenderGraphResourceLifetimeInfo GetLifetimeInfo(TextureHandle handle) const;
        [[nodiscard]] RenderGraphResourceLifetimeInfo GetLifetimeInfo(BufferHandle handle) const;
        [[nodiscard]] RenderGraphResourceLifetimeInfo GetLifetimeInfo(
            AccelerationStructureHandle handle) const;
        [[nodiscard]] std::vector<std::string> GetCompiledPassNames() const;
        [[nodiscard]] const std::vector<std::string>& GetCompileErrors() const noexcept;
        [[nodiscard]] RenderGraphDiagnosticSnapshot BuildDiagnosticSnapshot() const;
        [[nodiscard]] std::string BuildDebugDump() const;

        [[nodiscard]] static nvrhi::ResourceStates MapUsageToResourceStates(RenderGraphUsage usage);

    private:
        friend class RenderGraphBuilder;
        friend class RenderGraphContext;

        struct Impl;

        RenderGraphPassHandle AddPassInternal(
            std::string name,
            RenderGraphQueue queue,
            std::function<void(RenderGraphContext&)> execute);
        void FinishPassSetup(RenderGraphPassHandle pass);
        void AbortPassSetup(RenderGraphPassHandle pass) noexcept;
        void SetPassSideEffect(RenderGraphPassHandle pass, bool bSideEffect);
        void SetPassQueue(RenderGraphPassHandle pass, RenderGraphQueue queue);

        void DeclareTextureAccess(
            RenderGraphPassHandle pass,
            TextureHandle handle,
            RenderGraphAccess access,
            RenderGraphUsage usage,
            RenderGraphSubresourceRange subresources);
        void DeclareBufferAccess(
            RenderGraphPassHandle pass,
            BufferHandle handle,
            RenderGraphAccess access,
            RenderGraphUsage usage);
        void DeclareAccelerationStructureAccess(
            RenderGraphPassHandle pass,
            AccelerationStructureHandle handle,
            RenderGraphAccess access,
            RenderGraphUsage usage);

        [[nodiscard]] bool IsTextureDeclared(
            RenderGraphPassHandle pass,
            TextureHandle handle,
            RenderGraphAccess requiredAccess) const;
        [[nodiscard]] bool IsBufferDeclared(
            RenderGraphPassHandle pass,
            BufferHandle handle,
            RenderGraphAccess requiredAccess) const;
        [[nodiscard]] bool IsAccelerationStructureDeclared(
            RenderGraphPassHandle pass,
            AccelerationStructureHandle handle,
            RenderGraphAccess requiredAccess) const;
        [[nodiscard]] nvrhi::ITexture* ResolveTexture(TextureHandle handle) const;
        [[nodiscard]] nvrhi::IBuffer* ResolveBuffer(BufferHandle handle) const;
        [[nodiscard]] nvrhi::rt::IAccelStruct* ResolveAccelerationStructure(
            AccelerationStructureHandle handle) const;
        [[nodiscard]] std::string_view GetPassName(RenderGraphPassHandle pass) const;
        [[nodiscard]] RenderGraphQueue GetPassQueue(RenderGraphPassHandle pass) const;
        void RealizeResources(RenderGraphResourcePool& resourcePool, uint32_t frameSlot);
        void ValidatePhysicalResources() const;
        void ExecuteCompiled(
            nvrhi::ICommandList* commandList,
            IRenderGraphInstrumentation* instrumentation);

        std::unique_ptr<Impl> mImpl;
    };
}
