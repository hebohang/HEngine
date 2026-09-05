ADD_PROGRAM(HEngineImGuiDX12Smoke)

target_sources(${TARGET_NAME} PRIVATE
    Main.cpp
)

target_include_directories(${TARGET_NAME} PRIVATE
    "${EngineSourceDir}"
    "${EngineRuntimeDir}"
)

target_link_libraries(${TARGET_NAME} PRIVATE HEngineRuntime nvrhi imgui)
set_target_properties(${TARGET_NAME} PROPERTIES CXX_STANDARD 20)

# Runtime, Game, Editor and the focused smoke programs share one authoritative
# shader generation edge. This target name remains as a compatibility alias for
# validation targets that already depend on it.
add_custom_target(HEngineImGuiDX12SmokeShaders)
add_dependencies(HEngineImGuiDX12SmokeShaders HEngineRuntimeShaders)
add_dependencies(${TARGET_NAME} HEngineImGuiDX12SmokeShaders)
