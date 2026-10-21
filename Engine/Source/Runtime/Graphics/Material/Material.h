#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/Material/MaterialParameterPacking.h"
#include "Runtime/Graphics/Scene/RenderWorld.h"
#include "Runtime/Resource/ResType/Material.h"
#include "ShaderPropertyParser.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace HEngine
{
    struct MaterialShaderProgramIdentity;

    struct MaterialShaderLayoutRefreshResult
    {
        bool bSucceeded = false;
        // Program-only changes still advance the runtime snapshot identity,
        // but do not require saving the .hmat document.
        bool bPersistentStateChanged = false;
        std::string Error;
    };

    enum class MaterialType
    {
        Default = 0,
        Custom,
        CustomPBR,
        PostProcess
    };

    struct MaterialRuntimeProperty
    {
        MaterialPropertyDecl Decl;
        float FloatValue = 0.0f;
        glm::vec4 ColorValue = glm::vec4(1.0f);
        int IntValue = 0;
        bool BoolValue = false;
        // Texture properties are asset references. GPU ownership belongs to
        // the renderer's texture cache, never to the serialized Material.
        std::string TextureValue;
        TextureSampling Sampling;
    };

    enum class MaterialTextureSemantic
    {
        BaseColor,
        Normal,
        MetallicRoughness,
        Occlusion,
        Emissive
    };

    struct MaterialTextureReference
    {
        std::string AssetPath;
        TextureSampling Sampling;
        // Stable built-in token (white/black/normal) used when AssetPath is
        // empty or unavailable; the renderer maps it to its default handles.
        std::string Fallback;
        bool bEnabled = false;
    };

    class Material
    {
    private:
        // A mutex belongs to the Material object, not to the state being moved.
        // Moving this wrapper therefore creates a fresh unlocked mutex while
        // preserving Material's existing move-only value semantics.
        class MovableRecursiveMutex final
        {
        public:
            MovableRecursiveMutex() = default;
            MovableRecursiveMutex(MovableRecursiveMutex&&) noexcept {}
            MovableRecursiveMutex& operator=(MovableRecursiveMutex&&) noexcept
            {
                return *this;
            }

            MovableRecursiveMutex(const MovableRecursiveMutex&) = delete;
            MovableRecursiveMutex& operator=(const MovableRecursiveMutex&) = delete;

            void lock() { mMutex.lock(); }
            [[nodiscard]] bool try_lock() { return mMutex.try_lock(); }
            void unlock() { mMutex.unlock(); }

        private:
            std::recursive_mutex mMutex;
        };

    public:
        class RuntimeStateEditGuard final
        {
        public:
            RuntimeStateEditGuard(RuntimeStateEditGuard&&) noexcept = default;
            RuntimeStateEditGuard& operator=(RuntimeStateEditGuard&&) noexcept = default;

            RuntimeStateEditGuard(const RuntimeStateEditGuard&) = delete;
            RuntimeStateEditGuard& operator=(const RuntimeStateEditGuard&) = delete;

        private:
            friend class Material;
            explicit RuntimeStateEditGuard(MovableRecursiveMutex& mutex)
                : mLock(mutex)
            {
            }

            std::unique_lock<MovableRecursiveMutex> mLock;
        };

        Material() = default;
        explicit Material(std::string resPath);
        Material(Material&&) = default;

#if WITH_EDITOR
        void SaveToRes();
        bool SetShaderPath(const std::string& shaderPath);
#endif

        [[nodiscard]] std::string GetResourcePath() const;
        [[nodiscard]] std::string GetShaderPath() const;
        [[nodiscard]] int GetSchemaVersion() const;
        [[nodiscard]] uint64_t GetRuntimeRevision() const;
        [[nodiscard]] std::string GetExpectedRecursiveSourceHash() const;
        [[nodiscard]] std::string GetExpectedBindingSignature() const;
        [[nodiscard]] uint32_t GetExpectedMaterialAbiVersion() const;
        [[nodiscard]] std::string GetExpectedArtifactIdentityHash() const;
        [[nodiscard]] uint64_t GetExpectedProgramGeneration() const;
        [[nodiscard]] bool IsCustomSurfaceShader() const;
        [[nodiscard]] Json GetOrphanedParameters() const;
        [[nodiscard]] bool HasExposedProperties() const;
        [[nodiscard]] bool IsDoubleSided() const;
        // Editor draft widgets intentionally edit the legacy public view in
        // place. Hold this guard across that direct access, then call
        // ApplyRuntimePropertyChanges() to publish one coherent revision.
        [[nodiscard]] RuntimeStateEditGuard LockRuntimeStateForEdit()
        {
            return RuntimeStateEditGuard(mRuntimeMutex);
        }
        [[nodiscard]] std::vector<MaterialRuntimeProperty>& GetExposedProperties() { return mShaderProperties; }
        [[nodiscard]] const std::vector<MaterialRuntimeProperty>& GetExposedProperties() const { return mShaderProperties; }
        [[nodiscard]] std::vector<MaterialParameterValue> BuildParameterValues() const;
        [[nodiscard]] RenderMeshSection::MaterialInstanceSnapshot BuildRenderSnapshot() const;
        [[nodiscard]] MaterialTextureReference GetTextureReference(MaterialTextureSemantic semantic) const;

        // Updates the CPU asset reference used by serialization and by the
        // renderer's resource cache. No GPU resource is created here.
        void SetTexturePath(MaterialTextureSemantic semantic, std::string assetPath, bool bEnabled = true);
        // Call after direct edits to exposed values. This synchronizes the
        // legacy PBR view immediately and advances the immutable render revision.
        void ApplyRuntimePropertyChanges();
        // Rebuilds the exposed layout from the last successfully published
        // manifest. The current layout and render state remain unchanged when
        // loading or migration fails.
        [[nodiscard]] bool RefreshShaderLayoutFromManifest(std::string* error = nullptr);
        // Adopts the exact manifest/identity that the renderer successfully
        // published. No file is re-read, preventing a manifest change between
        // GPU publication and Material migration.
        [[nodiscard]] MaterialShaderLayoutRefreshResult RefreshShaderLayoutFromPublishedProgram(
            const MaterialShaderManifest& manifest,
            const MaterialShaderProgramIdentity& identity,
            uint64_t generation);
        // Builds the V2 document without writing it, allowing headless
        // serialization validation to exercise the exact Editor save path.
        [[nodiscard]] Json BuildSerializedDocument();
        void CopyRuntimeStateFrom(const Material& other);
        // Cache publication keeps an existing live object identity while
        // forcing a new render revision for the newly published contents.
        void PublishRuntimeStateFrom(const Material& other);
        void ResetTexturePropertyToDefault(MaterialRuntimeProperty& property);

    private:
        void LoadExposedPropertiesFromMaterialJson(const Json& propertiesJson);
        void LoadTypedPropertiesFromMaterialJson(const Json& materialJson);
        void MigrateRuntimeProperties(
            const std::vector<MaterialRuntimeProperty>& previousProperties,
            const Json& previousOrphans);
        [[nodiscard]] Json SerializeTypedParameters() const;
        [[nodiscard]] Json SerializeTypedTextures() const;
        void BuildExposedPropertiesFromShader();
        void SyncExposedPropertiesFromLegacyPbr();
        void SyncLegacyPbrFromExposedProperties();
        void SyncRenderStateToExposedProperties();
        [[nodiscard]] std::string ResolveTexturePathFromToken(const std::string& token) const;
        void CopyRuntimeStateFromUnlocked(const Material& other);

    public:
        MaterialType mType = MaterialType::Default;
        std::string mMatResPath;
        std::string mPassName = "BasePBR";
        std::string mShaderPath = "Shaders\\BasePBR.hlsl";
        MaterialRes mMatRes;
        PbrMaterialRes mPbrMatRes;

        bool bUseAlbedoMap = false;
        glm::vec4 baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };

        bool bUseNormalMap = false;

        bool bUseMetallicRoughnessMap = false;
        float metallicFactor = 1.0f;
        float roughnessFactor = 1.0f;

        bool bUseAoMap = false;
        float aoStrength = 1.0f;

        bool bUseEmissiveMap = false;
        glm::vec3 emissiveFactor = { 0.0f, 0.0f, 0.0f };

        bool bUseShaderProperties = false;
        std::vector<MaterialRuntimeProperty> mShaderProperties;
        int mSchemaVersion = 1;
        Json mSourceDocument;
        Json mOrphanedParameters = Json::object();
        uint64_t mRuntimeRevision = 1;
        std::string mExpectedRecursiveSourceHash;
        std::string mExpectedBindingSignature;
        uint32_t mExpectedMaterialAbiVersion = 0;
        std::string mExpectedArtifactIdentityHash;
        uint64_t mExpectedProgramGeneration = 0;

    private:
        mutable MovableRecursiveMutex mRuntimeMutex;
    };
}
