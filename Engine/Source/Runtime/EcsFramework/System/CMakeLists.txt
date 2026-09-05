add_subdirectory(Audio)
add_subdirectory(AI)
add_subdirectory(Camera)
add_subdirectory(Movement)
add_subdirectory(Navigation)
add_subdirectory(Physics)
add_subdirectory(Render)
add_subdirectory(Script)
add_subdirectory(UI)
add_subdirectory(Embree)

RUNTIME_ADD_SOURCE(
    System.h SystemGroup.h SystemLifecycle.h
    Animation/AnimationSystem.h Animation/AnimationSystem.cpp
)
