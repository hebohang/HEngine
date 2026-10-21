# Apple Silicon Shipping uses the existing cooker and manifest contract, with
# platform-specific bundle/dependency verification instead of Windows PE gates.
if(NOT PACK_GAME)
    return()
endif()
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "macOS Shipping packages require CMAKE_BUILD_TYPE=Release")
endif()
if(HE_ENABLE_GM OR HE_BUILD_DEV_GAME)
    message(FATAL_ERROR "Development/Agent hosts cannot enter macOS Shipping")
endif()
if(NOT TARGET HCooker)
    message(FATAL_ERROR "HCooker is required for macOS Shipping")
endif()
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
find_program(HE_MACOS_POWERSHELL NAMES pwsh REQUIRED)
set(HE_PACKAGE_DISTRIBUTION_ROOT "${RootDir}/dist" CACHE PATH "Root for owned package outputs")
string(SHA256 packageIdentity "${HE_PROJECT_ID}\n${ProjectRootDir}")
string(SUBSTRING "${packageIdentity}" 0 16 packageKey)
set(HE_MACOS_PACKAGE_OUTPUT "" CACHE FILEPATH "Optional owned .app output selected by the publish profile")
if(HE_MACOS_PACKAGE_OUTPUT)
    set(macOSPackageDirectory "${HE_MACOS_PACKAGE_OUTPUT}")
else()
    set(macOSPackageDirectory "${HE_PACKAGE_DISTRIBUTION_ROOT}/projects/${packageKey}/${HE_PROJECT_EXECUTABLE_NAME}.app")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/GamePackage.directory" "${macOSPackageDirectory}\n")
set(macOSPackageArguments
    --engine "${RootDir}"
    --project "${ProjectRootDir}"
    --build "${CMAKE_BINARY_DIR}"
    --executable "$<TARGET_FILE:HGame>"
    --cooker "$<TARGET_FILE:HCooker>"
    --generated "${EngineGeneratedContentDir}"
    --output "${macOSPackageDirectory}"
    --game-config "${HE_PROJECT_GAME_CONFIG}"
    --cmake "${CMAKE_COMMAND}"
    --pwsh "${HE_MACOS_POWERSHELL}")
if(HE_GAME_PYTHON_ENABLED)
    list(APPEND macOSPackageArguments --python-config "${BINARY_ROOT_DIR}/PythonEmbed.json")
endif()
if(TARGET CSharpScript)
    list(APPEND macOSPackageArguments --script-dir "${BINARY_ROOT_DIR}/Scripting/CSharp")
endif()
if(HE_PORTABLE_PLAYER)
    list(APPEND macOSPackageArguments --player-contract "${CMAKE_BINARY_DIR}/PlayerTemplate-$<CONFIG>.json")
endif()
if(NOT HE_MOLTENVK_LICENSE AND EXISTS "${HE_MOLTENVK_ROOT}/LICENSE")
    set(HE_MOLTENVK_LICENSE "${HE_MOLTENVK_ROOT}/LICENSE")
endif()
if(DEFINED HE_MOLTENVK_LICENSE AND EXISTS "${HE_MOLTENVK_LICENSE}")
    list(APPEND macOSPackageArguments --moltenvk-license "${HE_MOLTENVK_LICENSE}")
else()
    message(FATAL_ERROR "Shipping requires HE_MOLTENVK_LICENSE or the LICENSE under HE_MOLTENVK_ROOT")
endif()
set(macOSProjectShaderCommands "")
if(IS_DIRECTORY "${ProjectShadersDir}")
    file(GLOB_RECURSE macOSProjectShaderSources CONFIGURE_DEPENDS "${ProjectShadersDir}/*.hlsl")
    list(FILTER macOSProjectShaderSources EXCLUDE REGEX "[/\\]DevOnly([/\\]|$)")
    foreach(shaderSource IN LISTS macOSProjectShaderSources)
        list(APPEND macOSProjectShaderCommands
            COMMAND $<TARGET_FILE:HShaderCompiler>
                --shader "${shaderSource}"
                --rootPath "${ProjectShadersDir}"
                --outputPath "${EngineGeneratedShadersDir}"
                --includePath "${EngineContentShadersDir}"
                --binary-format spirv)
    endforeach()
endif()
add_custom_target(PackageGame
    ${macOSProjectShaderCommands}
    COMMAND "${Python3_EXECUTABLE}" "${RootDir}/Tools/macOS/Package.py" ${macOSPackageArguments}
    DEPENDS HGame HCooker HShaderCompiler HEngineRuntimeShaders
    USES_TERMINAL VERBATIM)
set_target_properties(PackageGame PROPERTIES FOLDER "Packaging")
