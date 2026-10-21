set(SCENE_EDITING_SOURCES)
if(NOT PACK_GAME)
    list(APPEND SCENE_EDITING_SOURCES
        SceneEditingService.h SceneEditingService.cpp
        SceneEditingLevel.h SceneEditingLevel.cpp
    )
endif()
RUNTIME_ADD_SOURCE(
    SceneSerializer.h SceneSerializer.cpp
    SceneDocument.h SceneDocument.cpp
    PrefabSerializer.h PrefabSerializer.cpp
    PrefabDocument.h PrefabDocument.cpp
    ${SCENE_EDITING_SOURCES}
)
