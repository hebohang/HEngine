add_subdirectory(Audio)
add_subdirectory(AI)
add_subdirectory(Basic)
add_subdirectory(Camera)
add_subdirectory(Character)
add_subdirectory(Editor)
add_subdirectory(Light)
add_subdirectory(Mesh)
add_subdirectory(PostProcess)
add_subdirectory(Physics)
add_subdirectory(Script)
add_subdirectory(Shape)
add_subdirectory(Transform)

RUNTIME_ADD_SOURCE(
    ComponentBase.h ComponentGroup.h RenderSettingsComponent.h
    Animation/AnimatorComponent.h Animation/AnimatorComponent.cpp
)
