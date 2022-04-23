set(ThirdPartyFolder "ThirdParty")

include(${CMAKE_SOURCE_DIR}/cmake/Tools.cmake)

if(NOT TARGET Glad)
    include(Glad.cmake)
    set_target_properties(Glad PROPERTIES FOLDER ${ThirdPartyFolder}/Glad)
    NoWarning(Glad)
endif()

# GLFW with the same name of project GLFW, so named "glfw"
if(NOT TARGET glfw)
    option(GLFW_BUILD_EXAMPLES "" OFF)
    option(GLFW_BUILD_TESTS "" OFF)
    option(GLFW_BUILD_DOCS "" OFF)
    option(GLFW_INSTALL "" OFF)
    add_subdirectory(glfw)
    set_target_properties(glfw PROPERTIES FOLDER ${ThirdPartyFolder}/GLFW)
    NoWarning(glfw)
endif()

if(NOT TARGET glm)
    option(BUILD_STATIC_LIBS "" ON)
    option(BUILD_TESTING "" OFF)
    option(GLM_TEST_ENABLE "" OFF)
    add_subdirectory(glm)
    set_target_properties(glm PROPERTIES FOLDER ${ThirdPartyFolder}/glm)
    NoWarningInterface(glm)
endif()

if(NOT TARGET imgui)
    include(imgui.cmake)
    set_target_properties(imgui PROPERTIES FOLDER ${ThirdPartyFolder}/imgui)
    NoWarning(imgui)
endif()

if(NOT TARGET yaml-cpp)
    include(yaml-cpp.cmake)
    set_target_properties(yaml-cpp PROPERTIES FOLDER ${ThirdPartyFolder}/yaml-cpp)
    NoWarning(yaml-cpp)
endif()

if(NOT TARGET spdlog)
    option(SPDLOG_BUILD_EXAMPLE "" OFF)
    option(SPDLOG_INSTALL "" OFF)
    add_subdirectory(spdlog)
    set_target_properties(spdlog PROPERTIES FOLDER ${ThirdPartyFolder}/spdlog)
    NoWarning(spdlog)
endif()

if(NOT TARGET Box2D)
    include(Box2D.cmake)
    set_target_properties(Box2D PROPERTIES FOLDER ${ThirdPartyFolder}/Box2D)
    NoWarning(Box2D)
endif()

if(NOT TARGET stb)
    include(stb.cmake)
    set_target_properties(stb PROPERTIES FOLDER ${ThirdPartyFolder}/stb)
    NoWarning(stb)
endif()

if(NOT TARGET ImGuizmo)
    include(ImGuizmo.cmake)
    set_target_properties(ImGuizmo PROPERTIES FOLDER ${ThirdPartyFolder}/ImGuizmo)
    NoWarning(ImGuizmo)
endif()

if(NOT TARGET assimp)
    OPTION( BUILD_SHARED_LIBS
      "Build package with shared libraries."
      OFF
    )
    OPTION( ASSIMP_BUILD_FRAMEWORK
      "Build package as Mac OS X Framework bundle."
      OFF
    )
    OPTION( ASSIMP_DOUBLE_PRECISION
      "Set to ON to enable double precision processing"
      OFF
    )
    OPTION( ASSIMP_OPT_BUILD_PACKAGES
      "Set to ON to generate CPack configuration files and packaging targets"
      OFF
    )
    OPTION( ASSIMP_ANDROID_JNIIOSYSTEM
      "Android JNI IOSystem support is active"
      OFF
    )
    OPTION( ASSIMP_NO_EXPORT
      "Disable Assimp's export functionality."
      OFF
    )
    OPTION( ASSIMP_BUILD_ZLIB
      "Build your own zlib"
      OFF
    )
    OPTION( ASSIMP_BUILD_ASSIMP_TOOLS
      "If the supplementary tools for Assimp are built in addition to the library."
      OFF
    )
    OPTION ( ASSIMP_BUILD_SAMPLES
      "If the official samples are built as well (needs Glut)."
      OFF
    )
    OPTION ( ASSIMP_BUILD_TESTS
      "If the test suite for Assimp is built in addition to the library."
      OFF
    )
    OPTION ( ASSIMP_COVERALLS
      "Enable this to measure test coverage."
      OFF
    )
    OPTION( ASSIMP_INSTALL
      "Disable this if you want to use assimp as a submodule."
      OFF
    )
    OPTION ( ASSIMP_ERROR_MAX
      "Enable all warnings."
      OFF
    )
    OPTION ( ASSIMP_ASAN
      "Enable AddressSanitizer."
      OFF
    )
    OPTION ( ASSIMP_UBSAN
      "Enable Undefined Behavior sanitizer."
      OFF
    )
    OPTION ( ASSIMP_BUILD_DOCS
      "Build documentation using Doxygen."
      OFF
    )
    OPTION( ASSIMP_INJECT_DEBUG_POSTFIX
      "Inject debug postfix in .a/.so/.dll lib names"
      OFF
    )

    OPTION ( ASSIMP_IGNORE_GIT_HASH
       "Don't call git to get the hash."
       OFF
    )
    add_subdirectory(assimp)
    set_target_properties(ImGuizmo PROPERTIES FOLDER ${ThirdPartyFolder}/assimp)
    NoWarning(assimp)
endif()

if(NOT TARGET spirv-cross)
    add_subdirectory(SPIRV-Cross)
    set_target_properties(spirv-cross PROPERTIES FOLDER ${ThirdPartyFolder}/SPIRV-Cross)
endif()

# Learned from https://github.com/Mike-Leo-Smith/LearnVulkan/blob/master/CMakeLists.txt
add_subdirectory(SPIRV-Headers)
add_subdirectory(SPIRV-Tools)
add_subdirectory(glslang)

if(NOT TARGET shaderc)
    set(SHADERC_SKIP_TESTS ON CACHE BOOL "" FORCE)
    set(SHADERC_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
    set(SHADERC_SKIP_COPYRIGHT_CHECK ON CACHE BOOL "" FORCE)
    set(SHADERC_ENABLE_SHARED_CRT ON CACHE BOOL "" FORCE)
    add_subdirectory(shaderc)
    set_target_properties(shaderc PROPERTIES FOLDER ${ThirdPartyFolder}/shaderc)
    set_target_properties(shaderc_combined_genfile PROPERTIES EXCLUDE_FROM_ALL ON) # Problematic on M1 Macs
endif()

if(NOT TARGET headers)
    add_library(headers INTERFACE)
    target_include_directories(headers INTERFACE "${ThirdPartyDir}")
    target_include_directories(headers INTERFACE "${ThirdPartyDir}/entt/include")
    target_include_directories(headers INTERFACE "${ThirdPartyDir}/assimp/include")
    target_include_directories(headers INTERFACE "${ThirdPartyDir}/SPIRV-Cross")
    NoWarningInterface(headers)
endif()