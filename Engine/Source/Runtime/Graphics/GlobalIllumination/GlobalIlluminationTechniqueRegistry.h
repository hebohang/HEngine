#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIllumination.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace HEngine
{
    enum class GlobalIlluminationTechniqueCapabilityRequirements : uint32_t
    {
        None = 0,
        GraphicsQueue = 1u << 0u,
        ComputeQueue = 1u << 1u,
        RayTracingAccelerationStructure = 1u << 2u,
        RayTracingPipeline = 1u << 3u,
        RayQuery = 1u << 4u,
    };

    [[nodiscard]] constexpr GlobalIlluminationTechniqueCapabilityRequirements
        operator|(
            GlobalIlluminationTechniqueCapabilityRequirements left,
            GlobalIlluminationTechniqueCapabilityRequirements right) noexcept
    {
        return static_cast<GlobalIlluminationTechniqueCapabilityRequirements>(
            static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
    }

    [[nodiscard]] constexpr bool HasGlobalIlluminationTechniqueCapabilityRequirement(
        GlobalIlluminationTechniqueCapabilityRequirements requirements,
        GlobalIlluminationTechniqueCapabilityRequirements requirement) noexcept
    {
        return (static_cast<uint32_t>(requirements) &
                static_cast<uint32_t>(requirement)) != 0u;
    }

    enum class GlobalIlluminationTechniqueRequiredInput : uint64_t
    {
        None = 0,
        SceneDepth = 1ull << 0u,
        NormalRoughness = 1ull << 1u,
        BaseColorMetalness = 1ull << 2u,
        Emissive = 1ull << 3u,
        MotionVectors = 1ull << 4u,
        MaterialId = 1ull << 5u,
        PreviousDepth = 1ull << 6u,
        SceneTlas = 1ull << 7u,
        FrameBuffer = 1ull << 8u,
        ObjectBuffer = 1ull << 9u,
        MaterialBuffer = 1ull << 10u,
        GeometryBuffer = 1ull << 11u,
        LightBuffer = 1ull << 12u,
        RayTracingVertexBuffer = 1ull << 13u,
        RayTracingIndexBuffer = 1ull << 14u,
        RayTracingInstanceBuffer = 1ull << 15u,
        OpaqueDirectHdr = 1ull << 16u,
        DepthPyramid = 1ull << 17u,
    };

    [[nodiscard]] constexpr GlobalIlluminationTechniqueRequiredInput operator|(
        GlobalIlluminationTechniqueRequiredInput left,
        GlobalIlluminationTechniqueRequiredInput right) noexcept
    {
        return static_cast<GlobalIlluminationTechniqueRequiredInput>(
            static_cast<uint64_t>(left) | static_cast<uint64_t>(right));
    }

    [[nodiscard]] constexpr bool HasGlobalIlluminationTechniqueRequiredInput(
        GlobalIlluminationTechniqueRequiredInput inputs,
        GlobalIlluminationTechniqueRequiredInput input) noexcept
    {
        return (static_cast<uint64_t>(inputs) & static_cast<uint64_t>(input)) != 0u;
    }

    enum class GlobalIlluminationTechniqueOutput : uint32_t
    {
        None = 0,
        DiffuseIndirectRadiance = 1u << 0u,
        DirectDiffuseRadiance = 1u << 1u,
        SpecularIndirectRadiance = 1u << 2u,
        HitDistance = 1u << 3u,
        Confidence = 1u << 4u,
        CoverageMask = 1u << 5u,
        HistoryLength = 1u << 6u,
        Variance = 1u << 7u,
    };

    [[nodiscard]] constexpr GlobalIlluminationTechniqueOutput operator|(
        GlobalIlluminationTechniqueOutput left,
        GlobalIlluminationTechniqueOutput right) noexcept
    {
        return static_cast<GlobalIlluminationTechniqueOutput>(
            static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
    }

    [[nodiscard]] constexpr bool HasGlobalIlluminationTechniqueOutput(
        GlobalIlluminationTechniqueOutput outputs,
        GlobalIlluminationTechniqueOutput output) noexcept
    {
        return (static_cast<uint32_t>(outputs) & static_cast<uint32_t>(output)) != 0u;
    }

    struct GlobalIlluminationTechniqueOutputSemantics
    {
        GlobalIlluminationTechniqueOutput Outputs =
            GlobalIlluminationTechniqueOutput::None;
        bool bSceneLinearHdr = true;
        bool bPreExposed = true;
        bool bPreTonemap = true;
    };

    using GlobalIlluminationTechniqueFactory =
        std::function<Scope<IGlobalIlluminationTechnique>()>;

    struct GlobalIlluminationTechniqueDescriptor
    {
        GlobalIlluminationTechniqueKind Id =
            GlobalIlluminationTechniqueKind::Off;
        std::string DisplayName;
        GlobalIlluminationTechniqueFactory Factory;
        GlobalIlluminationTechniqueCapabilityRequirements CapabilityRequirements =
            GlobalIlluminationTechniqueCapabilityRequirements::None;
        GlobalIlluminationTechniqueRequiredInput RequiredInputs =
            GlobalIlluminationTechniqueRequiredInput::None;
        GlobalIlluminationTechniqueOutputSemantics OutputSemantics;
        std::vector<GlobalIlluminationDebugView> SupportedDebugViews;

        [[nodiscard]] bool AreCapabilitiesSatisfied(
            const RenderCapabilities& capabilities) const noexcept;
    };

    namespace Detail
    {
        struct GlobalIlluminationTechniqueRegistryState;
    }

    class GlobalIlluminationTechniqueRegistry;

    // A lease keeps its descriptor registered while the technique is active.
    // Releasing or destroying the lease destroys the technique before making
    // its descriptor available for unregistration.
    class GlobalIlluminationTechniqueLease final
    {
    public:
        GlobalIlluminationTechniqueLease() noexcept = default;
        ~GlobalIlluminationTechniqueLease();

        GlobalIlluminationTechniqueLease(
            const GlobalIlluminationTechniqueLease&) = delete;
        GlobalIlluminationTechniqueLease& operator=(
            const GlobalIlluminationTechniqueLease&) = delete;
        GlobalIlluminationTechniqueLease(
            GlobalIlluminationTechniqueLease&& other) noexcept;
        GlobalIlluminationTechniqueLease& operator=(
            GlobalIlluminationTechniqueLease&& other) noexcept;

        [[nodiscard]] IGlobalIlluminationTechnique* Get() const noexcept;
        [[nodiscard]] IGlobalIlluminationTechnique* operator->() const noexcept;
        [[nodiscard]] IGlobalIlluminationTechnique& operator*() const;
        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] GlobalIlluminationTechniqueKind GetId() const noexcept;
        void Release() noexcept;

    private:
        friend class GlobalIlluminationTechniqueRegistry;

        GlobalIlluminationTechniqueLease(
            Scope<IGlobalIlluminationTechnique> technique,
            std::shared_ptr<Detail::GlobalIlluminationTechniqueRegistryState> state,
            GlobalIlluminationTechniqueKind id) noexcept;

        Scope<IGlobalIlluminationTechnique> mTechnique;
        std::shared_ptr<Detail::GlobalIlluminationTechniqueRegistryState> mState;
        GlobalIlluminationTechniqueKind mId =
            GlobalIlluminationTechniqueKind::Off;
    };

    class GlobalIlluminationTechniqueRegistry final
    {
    public:
        GlobalIlluminationTechniqueRegistry();
        ~GlobalIlluminationTechniqueRegistry();

        GlobalIlluminationTechniqueRegistry(
            const GlobalIlluminationTechniqueRegistry&) = delete;
        GlobalIlluminationTechniqueRegistry& operator=(
            const GlobalIlluminationTechniqueRegistry&) = delete;

        void Register(GlobalIlluminationTechniqueDescriptor descriptor);
        void Unregister(GlobalIlluminationTechniqueKind id);
        [[nodiscard]] bool Contains(GlobalIlluminationTechniqueKind id) const;
        [[nodiscard]] std::vector<GlobalIlluminationTechniqueDescriptor>
            Enumerate() const;
        [[nodiscard]] GlobalIlluminationTechniqueLease Create(
            GlobalIlluminationTechniqueKind id);
        [[nodiscard]] size_t GetActiveLeaseCount(
            GlobalIlluminationTechniqueKind id) const;

    private:
        std::shared_ptr<Detail::GlobalIlluminationTechniqueRegistryState> mState;
    };
}
