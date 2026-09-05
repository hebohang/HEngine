#include "MaterialPanel.h"

#include "Editor/EditorLayer.h"
#include "Editor/ImGuiWrapper/ImGuiWrapper.h"
#include "Runtime/Graphics/Material/Material.h"
#include "Runtime/Graphics/Material/MaterialResourceCache.h"
#include "Runtime/Graphics/Material/MaterialShaderManager.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/Misc/ShaderUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <glm/gtc/type_ptr.hpp>
#include <imgui/imgui.h>
#include <imgui-notify-v2/imgui_notify.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <exception>
#include <filesystem>
#include <string>

namespace HEngine
{
	namespace
	{
		[[nodiscard]] bool IsTextureFile(const std::filesystem::path& path)
		{
			std::string extension = path.extension().string();
			std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value)
			{
				return static_cast<char>(std::tolower(value));
			});
			return extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
				extension == ".tga" || extension == ".bmp" || extension == ".hdr";
		}

		[[nodiscard]] std::string NormalizeDroppedPath(const std::filesystem::path& payloadPath)
		{
			std::filesystem::path candidate = payloadPath;
			if (candidate.is_absolute())
			{
				candidate = Utils::Utf8ToPath(ConfigManager::GetRelativePath(candidate));
			}
			else if (!candidate.empty())
			{
				const std::filesystem::path fromAssets = std::filesystem::path("Assets") / candidate;
				if (std::filesystem::exists(ConfigManager::GetFullPath(fromAssets.generic_string())))
					candidate = fromAssets;
			}
			return Utils::NormalizeAssetPath(candidate.generic_string());
		}

		[[nodiscard]] const char* PropertyLabel(const MaterialRuntimeProperty& property)
		{
			return property.Decl.DisplayName.empty()
				? property.Decl.Name.c_str()
				: property.Decl.DisplayName.c_str();
		}
	}

	MaterialPanel::MaterialPanel(EditorLayer* parent, std::string matPath)
		: BasePanel(parent, matPath, &mOpen),
		  mMaterialPath(Utils::NormalizeAssetPath(matPath)),
		  mMaterial(CreateRef<Material>(mMaterialPath)),
		  mPreviewScene("Editor Material Preview " + mMaterialPath, mMaterial)
	{
		mProgramGeneration = mMaterial->GetExpectedProgramGeneration();
		mArtifactIdentityHash = mMaterial->GetExpectedArtifactIdentityHash();
	}

	bool MaterialPanel::UsesShader(std::string_view shaderPath) const
	{
		return mMaterial && mMaterial->IsCustomSurfaceShader() &&
			Utils::MakeProjectPathCacheKey(mMaterial->GetShaderPath()) ==
			Utils::MakeProjectPathCacheKey(shaderPath);
	}

	bool MaterialPanel::RefreshShaderLayoutFromPublishedProgram(
		const MaterialShaderManifest& manifest,
		const MaterialShaderProgramIdentity& identity,
		uint64_t generation)
	{
		if (!mMaterial)
			return false;

		MaterialShaderLayoutRefreshResult refresh =
			mMaterial->RefreshShaderLayoutFromPublishedProgram(
				manifest,
				identity,
				generation);
		if (!refresh.bSucceeded)
		{
			mCompileState = CompileState::Failed;
			mCompileMessage = refresh.Error.empty()
				? "Shader compiled, but its material layout could not be refreshed."
				: std::move(refresh.Error);
			mUsingLastKnownGood = true;
			return false;
		}

		mPreviewScene.SetMaterial(mMaterial);
		mDirty = mDirty || refresh.bPersistentStateChanged;
		mCompileState = CompileState::Succeeded;
		mCompileMessage = refresh.bPersistentStateChanged
			? "Shader recompiled; material properties and render state were migrated."
			: "Shader recompiled; program updated without material document changes.";
		mProgramGeneration = generation;
		mArtifactIdentityHash = identity.ArtifactIdentityHash;
		mUsingLastKnownGood = false;
		mPreviewRuntimeError.clear();
		return true;
	}

	void MaterialPanel::NotifyShaderReloadFailed(
		const MaterialShaderProgramIdentity* lastGoodIdentity,
		uint64_t lastGoodGeneration,
		std::string message)
	{
		mCompileState = CompileState::Failed;
		mCompileMessage = message.empty()
			? "Shader reload failed; the last-known-good program remains active."
			: std::move(message);
		mUsingLastKnownGood = true;
		if (lastGoodIdentity)
		{
			mProgramGeneration = lastGoodGeneration;
			mArtifactIdentityHash = lastGoodIdentity->ArtifactIdentityHash;
		}
	}

	void MaterialPanel::OnImGuiRender()
	{
		if (!(*pOpen))
		{
			mPreviewScene.SetViewportHovered(false);
			mPreviewScene.Release();
			return;
		}

		ImGuiWrapper::SetNextWindowSizeConstraints(
			ImVec2(820.0f, 560.0f),
			ImVec2(FLT_MAX, FLT_MAX));
		if (mFirstOpen)
		{
			ImGuiWrapper::SetNextWindowSize(ImVec2(1180.0f, 780.0f), ImGuiCond_Once);
			mFirstOpen = false;
		}

		if (!ImGui::Begin(mName.c_str(), pOpen))
		{
			ImGui::End();
			mPreviewScene.SetViewportHovered(false);
			if (!(*pOpen))
				mPreviewScene.Release();
			return;
		}

		OnImGuiRenderImpl();
		ImGui::End();
		if (!(*pOpen))
		{
			mPreviewScene.SetViewportHovered(false);
			mPreviewScene.Release();
		}
	}

	void MaterialPanel::OnImGuiRenderImpl()
	{
		DrawToolbar();
		ImGui::Separator();

		if (ImGuiWrapper::BeginResizableSplit(
			"MaterialEditorMainSplit",
			2.2f,
			1.0f,
			ImGuiTableFlags_BordersInnerV))
		{
			DrawPreview();
			ImGui::TableSetColumnIndex(1);
			DrawInspector();
			ImGuiWrapper::EndResizableSplit();
		}
	}

	void MaterialPanel::OnEvent(Event& event)
	{
		if (*pOpen)
			mPreviewScene.OnEvent(event);
	}

	void MaterialPanel::DrawToolbar()
	{
		if (ImGui::Button(
			(std::string(EditorLayer::GetText("Reset View")) + "###MaterialResetView").c_str(),
			ImGuiWrapper::S(104.0f, 0.0f)))
		{
			mPreviewScene.ResetCamera();
		}

		ImGuiWrapper::SameLine(0.0f, 8.0f);
		if (ImGui::Button(
			(std::string(EditorLayer::GetText("Save Material")) + "###MaterialSave").c_str(),
			ImGuiWrapper::S(116.0f, 0.0f)))
		{
			SaveMaterial();
		}

		ImGuiWrapper::SameLine(0.0f, 8.0f);
		if (ImGui::Button(
			(std::string(EditorLayer::GetText("Revert")) + "###MaterialRevert").c_str(),
			ImGuiWrapper::S(92.0f, 0.0f)))
		{
			RevertMaterial();
		}

		ImGuiWrapper::SameLine(0.0f, 14.0f);
		ImGui::TextColored(
			mDirty ? ImVec4(1.0f, 0.72f, 0.25f, 1.0f) : ImVec4(0.45f, 0.85f, 0.55f, 1.0f),
			"%s",
			mDirty ? EditorLayer::GetText("Dirty") : EditorLayer::GetText("Saved"));
	}

	void MaterialPanel::DrawPreview()
	{
		if (!ImGui::BeginChild("##MaterialPreviewViewport", ImVec2(0.0f, 0.0f), false))
		{
			ImGui::EndChild();
			mPreviewScene.SetViewportHovered(false);
			return;
		}

		ImVec2 viewportSize = ImGui::GetContentRegionAvail();
		viewportSize.x = std::max(viewportSize.x, ImGuiWrapper::S(96.0f));
		viewportSize.y = std::max(viewportSize.y, ImGuiWrapper::S(96.0f));

		bool bRendered = false;
		try
		{
			bRendered = mPreviewScene.Render(
				Timestep(ImGui::GetIO().DeltaTime),
				static_cast<uint32_t>(std::max(1.0f, viewportSize.x)),
				static_cast<uint32_t>(std::max(1.0f, viewportSize.y)));
			mPreviewRuntimeError.clear();
		}
		catch (const std::exception& exception)
		{
			mPreviewRuntimeError = exception.what();
			mPreviewScene.Release();
		}

		const ImGuiTextureHandle texture = bRendered
			? mPreviewScene.GetImGuiTexture()
			: ImGuiTextureHandle{};
		if (texture)
			ImGui::Image(NvrhiImGuiRenderer::ToImTextureID(texture), viewportSize);
		else
			ImGui::Dummy(viewportSize);

		mPreviewScene.SetViewportHovered(ImGui::IsItemHovered());
		const std::string& sceneError = mPreviewScene.GetError();
		const std::string& error = mPreviewRuntimeError.empty() ? sceneError : mPreviewRuntimeError;
		if (!error.empty())
		{
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const ImVec2 itemMin = ImGui::GetItemRectMin();
			const ImVec2 messageInset = ImGuiWrapper::S(16.0f, 16.0f);
			const ImVec2 messagePosition(
				itemMin.x + messageInset.x,
				itemMin.y + messageInset.y);
			drawList->AddText(
				messagePosition,
				ImGui::GetColorU32(ImVec4(1.0f, 0.35f, 0.30f, 1.0f)),
				error.c_str());
		}

		ImGui::EndChild();
	}

	void MaterialPanel::DrawInspector()
	{
		if (!ImGui::BeginChild(
			"##MaterialInspector",
			ImVec2(0.0f, 0.0f),
			false,
			ImGuiWindowFlags_AlwaysVerticalScrollbar))
		{
			ImGui::EndChild();
			return;
		}

		ImGui::Text("%s: %d", EditorLayer::GetText("Schema Version"), mMaterial->GetSchemaVersion());
		ImGui::Text("%s: %llu", EditorLayer::GetText("Revision"),
			static_cast<unsigned long long>(mMaterial->GetRuntimeRevision()));
		ImGui::TextColored(
			mDirty ? ImVec4(1.0f, 0.72f, 0.25f, 1.0f) : ImVec4(0.45f, 0.85f, 0.55f, 1.0f),
			"%s",
			mDirty ? EditorLayer::GetText("Unsaved changes") : EditorLayer::GetText("Saved on disk"));
		ImGui::Separator();

		bool bMaterialChanged = DrawShaderSection();
		{
			// Keep direct draft edits and their revision publication atomic.
			// Shader compilation stays outside this guard because it performs IO.
			auto runtimeStateLock = mMaterial->LockRuntimeStateForEdit();
			bMaterialChanged |= DrawRenderState();
			bMaterialChanged |= DrawProperties();
			if (bMaterialChanged)
				mMaterial->ApplyRuntimePropertyChanges();
		}
		if (bMaterialChanged)
		{
			mPreviewScene.SetMaterial(mMaterial);
			mDirty = true;
		}

		ImGui::EndChild();
	}

	bool MaterialPanel::DrawShaderSection()
	{
		if (!ImGui::CollapsingHeader(
			EditorLayer::GetText("Shader"),
			ImGuiTreeNodeFlags_DefaultOpen))
		{
			return false;
		}

		ImGui::TextWrapped("%s", mMaterial->GetShaderPath().empty()
			? EditorLayer::GetText("(None)")
			: mMaterial->GetShaderPath().c_str());

		const std::string dropLabel = std::string(EditorLayer::GetText("Drop Shader(.hlsl) To Replace")) +
			"###MaterialDropShader";
		ImGui::Button(dropLabel.c_str(), ImVec2(-1.0f, ImGuiWrapper::S(42.0f)));

		bool bChanged = false;
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
			{
				const auto payloadPath = std::filesystem::path(static_cast<const wchar_t*>(payload->Data));
				const std::string shaderPath = NormalizeDroppedPath(payloadPath);
				if (std::filesystem::path(shaderPath).extension() != ".hlsl")
				{
					mCompileState = CompileState::Failed;
					mCompileMessage = "Only .hlsl shaders are supported.";
				}
				else
				{
					mCompileState = CompileState::Compiling;
					try
					{
						if (Utils::CompileShader(ConfigManager::GetFullPath(shaderPath)) &&
							mMaterial->SetShaderPath(shaderPath))
						{
							mCompileState = CompileState::Succeeded;
							mCompileMessage = "Shader compiled and bound successfully.";
							bChanged = true;
							ImGui::InsertNotification({ ImGuiToastType_Success, 3000,
								EditorLayer::GetText("Shader bound and properties parsed.") });
						}
						else
						{
							mCompileState = CompileState::Failed;
							mCompileMessage = "Compilation failed; the previous material remains active.";
							ImGui::InsertNotification({ ImGuiToastType_Error, 5000,
								EditorLayer::GetText("Failed to bind shader.") });
						}
					}
					catch (const std::exception& exception)
					{
						mCompileState = CompileState::Failed;
						mCompileMessage = exception.what();
					}
				}
			}
			ImGui::EndDragDropTarget();
		}

		ImVec4 statusColor(0.70f, 0.78f, 0.90f, 1.0f);
		const char* status = EditorLayer::GetText("Ready");
		if (mCompileState == CompileState::Compiling)
		{
			status = EditorLayer::GetText("Compiling");
			statusColor = ImVec4(0.95f, 0.78f, 0.30f, 1.0f);
		}
		else if (mCompileState == CompileState::Succeeded)
		{
			status = EditorLayer::GetText("Success");
			statusColor = ImVec4(0.45f, 0.85f, 0.55f, 1.0f);
		}
		else if (mCompileState == CompileState::Failed)
		{
			status = EditorLayer::GetText("Failed");
			statusColor = ImVec4(1.0f, 0.35f, 0.30f, 1.0f);
		}
		ImGui::TextColored(statusColor, "%s: %s", EditorLayer::GetText("Compile"), status);
		ImGui::TextWrapped("%s", mCompileMessage.c_str());
		if (mCompileState == CompileState::Failed && !mCompileMessage.empty())
		{
			if (ImGui::Button("Copy Diagnostics###MaterialCopyDiagnostics"))
				ImGui::SetClipboardText(mCompileMessage.c_str());
		}
		const std::string shortArtifact = mArtifactIdentityHash.empty()
			? std::string("unbound")
			: mArtifactIdentityHash.substr(
				0,
				std::min<std::size_t>(12, mArtifactIdentityHash.size()));
		ImGui::Text(
			"Program: gen %llu / %s",
			static_cast<unsigned long long>(mProgramGeneration),
			shortArtifact.c_str());
		if (mUsingLastKnownGood)
		{
			ImGui::TextColored(
				ImVec4(0.95f, 0.72f, 0.28f, 1.0f),
				"Last-known-good program active");
		}
		ImGui::Separator();
		return bChanged;
	}

	bool MaterialPanel::DrawRenderState()
	{
		if (!ImGui::CollapsingHeader(
			EditorLayer::GetText("Render State"),
			ImGuiTreeNodeFlags_DefaultOpen))
		{
			return false;
		}

		bool bChanged = false;
		const bool bShaderDefinedState = mMaterial->IsCustomSurfaceShader();
		if (bShaderDefinedState)
			ImGui::BeginDisabled();
		static constexpr const char* BlendModes[] = { "OPAQUE", "MASK", "BLEND" };
		const char* currentBlend = mMaterial->mPbrMatRes.alphaMode.c_str();
		if (ImGui::BeginCombo(EditorLayer::GetText("Blend Mode"), currentBlend))
		{
			for (const char* blendMode : BlendModes)
			{
				const bool bSelected = mMaterial->mPbrMatRes.alphaMode == blendMode;
				if (ImGui::Selectable(blendMode, bSelected))
				{
					mMaterial->mPbrMatRes.alphaMode = blendMode;
					bChanged = true;
				}
				if (bSelected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}

		bChanged |= ImGui::Checkbox(
			EditorLayer::GetText("Double Sided"),
			&mMaterial->mPbrMatRes.doubleSided);
		if (mMaterial->mPbrMatRes.alphaMode == "MASK")
		{
			bChanged |= ImGui::SliderFloat(
				EditorLayer::GetText("Alpha Cutoff"),
				&mMaterial->mPbrMatRes.alphaCutoff,
				0.0f,
				1.0f);
		}
		if (bShaderDefinedState)
			ImGui::EndDisabled();

		bool bDepthWrite = mMaterial->mPbrMatRes.alphaMode != "BLEND";
		ImGui::BeginDisabled();
		ImGui::Checkbox(EditorLayer::GetText("Depth Write"), &bDepthWrite);
		ImGui::EndDisabled();
		ImGui::Text("%s: %s", EditorLayer::GetText("Domain"), mMaterial->mMatRes.Domain.c_str());
		ImGui::Text("%s: %s", EditorLayer::GetText("Shading Model"), mMaterial->mMatRes.ShadingModel.c_str());
		if (bShaderDefinedState)
			ImGui::TextDisabled("%s", EditorLayer::GetText("Custom Shader render state is defined by its manifest."));
		ImGui::Separator();
		return bChanged;
	}

	bool MaterialPanel::DrawProperties()
	{
		if (!ImGui::CollapsingHeader(
			EditorLayer::GetText("Properties"),
			ImGuiTreeNodeFlags_DefaultOpen))
		{
			return false;
		}

		if (!mMaterial->HasExposedProperties())
		{
			ImGui::TextDisabled("%s", EditorLayer::GetText("No exposed shader properties found."));
			return false;
		}

		bool bChanged = false;
		std::string currentCategory;
		for (MaterialRuntimeProperty& property : mMaterial->GetExposedProperties())
		{
			if (property.Decl.Category != currentCategory)
			{
				currentCategory = property.Decl.Category;
				if (!currentCategory.empty())
					ImGuiWrapper::SeparatorWithText(currentCategory.c_str());
			}

			ImGui::PushID(property.Decl.Name.c_str());
			const char* label = PropertyLabel(property);
			switch (property.Decl.Type)
			{
			case MaterialPropertyType::Float:
				if (property.Decl.bHasRange)
					bChanged |= ImGui::SliderFloat(label, &property.FloatValue, property.Decl.RangeMin, property.Decl.RangeMax);
				else
					bChanged |= ImGui::DragFloat(label, &property.FloatValue, 0.01f);
				break;
			case MaterialPropertyType::Float2:
				if (property.Decl.bHasRange)
					bChanged |= ImGui::SliderFloat2(label, glm::value_ptr(property.ColorValue), property.Decl.RangeMin, property.Decl.RangeMax);
				else
					bChanged |= ImGui::DragFloat2(label, glm::value_ptr(property.ColorValue), 0.01f);
				break;
			case MaterialPropertyType::Float3:
				if (property.Decl.bHasRange)
					bChanged |= ImGui::SliderFloat3(label, glm::value_ptr(property.ColorValue), property.Decl.RangeMin, property.Decl.RangeMax);
				else
					bChanged |= ImGui::DragFloat3(label, glm::value_ptr(property.ColorValue), 0.01f);
				break;
			case MaterialPropertyType::Color:
				bChanged |= ImGui::ColorEdit4(label, glm::value_ptr(property.ColorValue));
				break;
			case MaterialPropertyType::Bool:
				bChanged |= ImGui::Checkbox(label, &property.BoolValue);
				break;
			case MaterialPropertyType::Int:
				if (property.Decl.bHasRange)
				{
					const int minimum = static_cast<int>(std::floor(property.Decl.RangeMin));
					const int maximum = static_cast<int>(std::ceil(property.Decl.RangeMax));
					bChanged |= ImGui::SliderInt(label, &property.IntValue, minimum, std::max(minimum, maximum));
				}
				else
				{
					bChanged |= ImGui::DragInt(label, &property.IntValue);
				}
				break;
			case MaterialPropertyType::Texture:
			{
				ImGui::TextUnformatted(label);
				ImGui::TextDisabled("%s", property.TextureValue.empty()
					? EditorLayer::GetText("(None)")
					: property.TextureValue.c_str());
				const std::string dropLabel = std::string(EditorLayer::GetText("Drop Texture Here")) + "###DropTexture";
				ImGui::Button(dropLabel.c_str(), ImVec2(-1.0f, ImGuiWrapper::S(32.0f)));
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
					{
						const auto payloadPath = std::filesystem::path(static_cast<const wchar_t*>(payload->Data));
						const std::string texturePath = NormalizeDroppedPath(payloadPath);
						if (IsTextureFile(texturePath) && std::filesystem::exists(ConfigManager::GetFullPath(texturePath)))
						{
							property.TextureValue = texturePath;
							bChanged = true;
						}
					}
					ImGui::EndDragDropTarget();
				}

				const std::string resetLabel = std::string(EditorLayer::GetText("Reset To Default")) + "###ResetTexture";
				if (ImGui::Button(resetLabel.c_str(), ImVec2(-1.0f, ImGuiWrapper::S(28.0f))))
				{
					mMaterial->ResetTexturePropertyToDefault(property);
					bChanged = true;
				}
				break;
			}
			}
			ImGui::PopID();
		}
		return bChanged;
	}

	void MaterialPanel::SaveMaterial()
	{
		try
		{
			mMaterial->SaveToRes();
			(void)MaterialResourceCache::GetInstance().Publish(
				mMaterialPath,
				mMaterial);
			mParent->PreviewMaterialAsset(mMaterialPath, *mMaterial);
			mDirty = false;
			ImGui::InsertNotification({ ImGuiToastType_Success, 3000,
				EditorLayer::GetText("Material saved successfully!") });
		}
		catch (const std::exception& exception)
		{
			ImGui::InsertNotification({ ImGuiToastType_Error, 6000, "%s", exception.what() });
		}
	}

	void MaterialPanel::RevertMaterial()
	{
		try
		{
			Ref<Material> reverted = CreateRef<Material>(mMaterialPath);
			mMaterial = std::move(reverted);
			mPreviewScene.SetMaterial(mMaterial);
			mDirty = false;
			mCompileState = CompileState::Ready;
			mCompileMessage = "Reverted to the material saved on disk.";
			mPreviewRuntimeError.clear();
			ImGui::InsertNotification({ ImGuiToastType_Success, 3000,
				EditorLayer::GetText("Material reverted successfully.") });
		}
		catch (const std::exception& exception)
		{
			ImGui::InsertNotification({ ImGuiToastType_Error, 6000, "%s", exception.what() });
		}
	}
}
