#pragma once 

#include "BasePanel.h"
#include "Editor/Preview/MaterialPreviewScene.h"
#include "Runtime/Core/Base/Base.h"

#include <string>
#include <string_view>
#include <cstdint>

namespace HEngine
{
	class Event;
    class Material;
	struct MaterialShaderManifest;
	struct MaterialShaderProgramIdentity;

    class MaterialPanel : public BasePanel
    {
    public:
        MaterialPanel(EditorLayer* parent, std::string matPath);

        void OnImGuiRender() override;
        void OnImGuiRenderImpl() override;
		void OnEvent(Event& event) override;

		[[nodiscard]] bool UsesShader(std::string_view shaderPath) const;
		[[nodiscard]] bool RefreshShaderLayoutFromPublishedProgram(
			const MaterialShaderManifest& manifest,
			const MaterialShaderProgramIdentity& identity,
			uint64_t generation);
		void NotifyShaderReloadFailed(
			const MaterialShaderProgramIdentity* lastGoodIdentity,
			uint64_t lastGoodGeneration,
			std::string message);
		[[nodiscard]] const std::string& GetMaterialPath() const noexcept
		{
			return mMaterialPath;
		}

        bool mOpen = true;
    private:
		enum class CompileState
		{
			Ready,
			Compiling,
			Succeeded,
			Failed,
		};

		void DrawToolbar();
		void DrawPreview();
		void DrawInspector();
		[[nodiscard]] bool DrawShaderSection();
		[[nodiscard]] bool DrawRenderState();
		[[nodiscard]] bool DrawProperties();
		void SaveMaterial();
		void RevertMaterial();

        bool mFirstOpen = true;
		bool mDirty = false;
		std::string mMaterialPath;
		Ref<Material> mMaterial;
		MaterialPreviewScene mPreviewScene;
		CompileState mCompileState = CompileState::Ready;
		std::string mCompileMessage = "Using the serialized shader.";
		uint64_t mProgramGeneration = 0;
		std::string mArtifactIdentityHash;
		bool mUsingLastKnownGood = false;
		std::string mPreviewRuntimeError;
    };
}
