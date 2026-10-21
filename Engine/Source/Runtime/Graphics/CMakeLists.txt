add_subdirectory(Renderer)
add_subdirectory(RHI)
add_subdirectory(RenderGraph)
add_subdirectory(Scene)
add_subdirectory(Gui)
add_subdirectory(Mesh)
add_subdirectory(Material)
add_subdirectory(RayTracing)
add_subdirectory(GlobalIllumination)
add_subdirectory(Settings)

RUNTIME_ADD_SOURCE(
    RenderingArchitecture.md
    VFX/EffectTransformBatch.h
    VFX/EffectTransformBatch.cpp
    VFX/EffekseerPackage.h
    VFX/EffekseerRegressionManifest.h
    VFX/EffekseerRegressionManifest.cpp
    VFX/EffekseerCamera.h
    VFX/EffekseerRuntime.h
    VFX/EffekseerRuntime.cpp
    VFX/EffekseerRenderGraphPass.h
    VFX/EffekseerRenderGraphPass.cpp
    Renderer2D.cpp
    Renderer2D.h
    Image.h Image.cpp
    ResourceFormat.h
)
