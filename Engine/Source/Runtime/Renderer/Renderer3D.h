#pragma once

#include "Runtime/Camera/EditorCamera.h"
#include "Runtime/EcsFramework/Component/Mesh/StaticMeshComponent.h"
#include "Runtime/Renderer/Texture.h"

namespace HEngine
{
    class Renderer3D
    {
    public:
        static void Init();
        static void Shutdown();

        static void DrawModel(const glm::mat4& transform, StaticMeshComponent& MeshComponent, int EntityID);

        static void BeginScene(const Camera& camera, const glm::mat4& transform);
        static void BeginScene(const EditorCamera& camera);
        static void EndScene();

        // give to Editor SceneSettingsPanel to use
        static Ref<CubeMapTexture> GetSkyBox();
        static Ref<CubeMapTexture> GetDefaultSkyBox();

        // TODO: Move to Environment System
        static void DrawSkyBox(const EditorCamera& camera);
    };
}