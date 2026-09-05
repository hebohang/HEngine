set(TARGET_NAME HEngineParser)
if(COMMAND ADD_PROGRAM)
    ADD_PROGRAM(${TARGET_NAME})
elseif(NOT TARGET ${TARGET_NAME})
    add_executable(${TARGET_NAME})
    set_target_properties(${TARGET_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/Programs/HEngineParser"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Programs/HEngineParser"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Programs/HEngineParser")
endif()

#set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../CMake")

file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "Parser/*.h" "Parser/*.hpp")
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "Parser/*.cpp")
list(FILTER HEADERS EXCLUDE REGEX "[/\\\\]Tests[/\\\\]")
list(FILTER SOURCES EXCLUDE REGEX "[/\\\\]Tests[/\\\\]")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${HEADERS} ${SOURCES})

# header files are superflous, but some IDEs (Visual Studio) don't include
# them in the solution explorer without them being added to the list of sources
target_sources(${TARGET_NAME} PRIVATE ${HEADERS} ${SOURCES})

set_target_properties(${TARGET_NAME} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_definitions(${TARGET_NAME} PRIVATE
    HE_CODEGEN_GENERATOR_VERSION="${PROJECT_VERSION}")

set(_libclang_root_default "$ENV{HE_CODEGEN_LIBCLANG_ROOT}")
set(HE_CODEGEN_LIBCLANG_ROOT "${_libclang_root_default}" CACHE PATH
    "Root containing bin/libclang.dll, lib/libclang.lib, and optionally include/clang-c for HEngineParser.")
set(_libclang_include_default "$ENV{HE_CODEGEN_LIBCLANG_INCLUDE_ROOT}")
set(HE_CODEGEN_LIBCLANG_INCLUDE_ROOT "${_libclang_include_default}" CACHE PATH
    "Optional clang-c include root matching HE_CODEGEN_LIBCLANG_ROOT.")

if (CMAKE_HOST_WIN32)
    function(he_probe_libclang candidate_root out_valid out_version out_major)
        set(candidate_clang "${candidate_root}/bin/clang.exe")
        set(candidate_dll "${candidate_root}/bin/libclang.dll")
        set(candidate_import "${candidate_root}/lib/libclang.lib")
        if(NOT EXISTS "${candidate_clang}" OR NOT EXISTS "${candidate_dll}" OR NOT EXISTS "${candidate_import}")
            set(${out_valid} FALSE PARENT_SCOPE)
            return()
        endif()

        execute_process(
            COMMAND "${candidate_clang}" --version
            RESULT_VARIABLE clang_version_result
            OUTPUT_VARIABLE clang_version_output
            ERROR_VARIABLE clang_version_error
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT clang_version_result EQUAL 0)
            set(${out_valid} FALSE PARENT_SCOPE)
            return()
        endif()
        string(REGEX MATCH "clang version ([0-9]+\\.[0-9]+\\.[0-9]+)" clang_version_match "${clang_version_output}")
        if(NOT clang_version_match)
            set(${out_valid} FALSE PARENT_SCOPE)
            return()
        endif()
        set(${out_valid} TRUE PARENT_SCOPE)
        set(${out_version} "${CMAKE_MATCH_1}" PARENT_SCOPE)
        string(REGEX MATCH "^[0-9]+" clang_version_major "${CMAKE_MATCH_1}")
        set(${out_major} "${clang_version_major}" PARENT_SCOPE)
    endfunction()

    set(libclang_candidates)
    if(HE_CODEGEN_LIBCLANG_ROOT)
        list(APPEND libclang_candidates "${HE_CODEGEN_LIBCLANG_ROOT}")
    else()
        if(DEFINED ENV{VSINSTALLDIR})
            list(APPEND libclang_candidates "$ENV{VSINSTALLDIR}/VC/Tools/Llvm/x64")
        endif()
        if(DEFINED ENV{VCToolsInstallDir})
            list(APPEND libclang_candidates "$ENV{VCToolsInstallDir}/../../Llvm/x64")
        endif()

        find_program(HE_CODEGEN_VSWHERE_EXECUTABLE
            NAMES vswhere.exe
            HINTS "C:/Program Files (x86)/Microsoft Visual Studio/Installer")
        if(HE_CODEGEN_VSWHERE_EXECUTABLE)
            execute_process(
                COMMAND "${HE_CODEGEN_VSWHERE_EXECUTABLE}" -all -products * -property installationPath
                OUTPUT_VARIABLE visual_studio_installations
                OUTPUT_STRIP_TRAILING_WHITESPACE)
            string(REPLACE "\r\n" ";" visual_studio_installations "${visual_studio_installations}")
            string(REPLACE "\n" ";" visual_studio_installations "${visual_studio_installations}")
            foreach(visual_studio_installation IN LISTS visual_studio_installations)
                if(visual_studio_installation)
                    list(APPEND libclang_candidates "${visual_studio_installation}/VC/Tools/Llvm/x64")
                endif()
            endforeach()
        endif()
        list(REMOVE_DUPLICATES libclang_candidates)
    endif()

    set(selected_libclang_root)
    set(selected_libclang_version)
    set(selected_libclang_major)
    foreach(libclang_candidate IN LISTS libclang_candidates)
        cmake_path(ABSOLUTE_PATH libclang_candidate NORMALIZE OUTPUT_VARIABLE normalized_libclang_candidate)
        he_probe_libclang("${normalized_libclang_candidate}" candidate_valid candidate_version candidate_major)
        if(candidate_valid AND candidate_major GREATER_EQUAL 20)
            set(selected_libclang_root "${normalized_libclang_candidate}")
            set(selected_libclang_version "${candidate_version}")
            set(selected_libclang_major "${candidate_major}")
            break()
        endif()
    endforeach()

    if(NOT selected_libclang_root)
        message(FATAL_ERROR
            "HEngineParser requires libclang >= 20 for the current MSVC STL and C++20 syntax. "
            "Install the Visual Studio LLVM/Clang component or set HE_CODEGEN_LIBCLANG_ROOT "
            "(cache or environment) to an x64 root containing bin/libclang.dll and lib/libclang.lib.")
    endif()
    set(HE_CODEGEN_LIBCLANG_ROOT "${selected_libclang_root}" CACHE PATH
        "Selected modern libclang root for HEngineParser." FORCE)

    if(NOT HE_CODEGEN_LIBCLANG_INCLUDE_ROOT AND
       EXISTS "${selected_libclang_root}/include/clang-c/Index.h")
        set(HE_CODEGEN_LIBCLANG_INCLUDE_ROOT "${selected_libclang_root}/include" CACHE PATH
            "clang-c include root for HEngineParser." FORCE)
    endif()
    if(HE_CODEGEN_LIBCLANG_INCLUDE_ROOT)
        cmake_path(ABSOLUTE_PATH HE_CODEGEN_LIBCLANG_INCLUDE_ROOT NORMALIZE
            OUTPUT_VARIABLE selected_libclang_include_root)
        if(NOT EXISTS "${selected_libclang_include_root}/clang-c/Index.h")
            message(FATAL_ERROR
                "HE_CODEGEN_LIBCLANG_INCLUDE_ROOT must contain clang-c/Index.h: "
                "${selected_libclang_include_root}")
        endif()
        message(STATUS
            "HEngineParser libclang ${selected_libclang_version}: ${selected_libclang_root} "
            "with explicit C API headers ${selected_libclang_include_root}")
    else()
        set(selected_libclang_include_root "${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/include")
        message(WARNING
            "HEngineParser libclang ${selected_libclang_version}: ${selected_libclang_root}; "
            "Visual Studio does not ship clang-c headers, so the repository's read-only LLVM 7 "
            "headers are used only as a backward-compatible C API declaration set. Runtime and "
            "import library are both libclang ${selected_libclang_version}.")
    endif()

    target_include_directories(${TARGET_NAME} PRIVATE
        "${selected_libclang_include_root}"
        "${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/mustache"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Parser")
    target_link_libraries(${TARGET_NAME} PRIVATE "${selected_libclang_root}/lib/libclang.lib")
    target_compile_definitions(${TARGET_NAME} PRIVATE
        TIXML_USE_STL
        HE_CODEGEN_LIBCLANG_CONFIGURED_VERSION="${selected_libclang_version}")
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${selected_libclang_root}/bin/libclang.dll"
            "$<TARGET_FILE_DIR:${TARGET_NAME}>/libclang.dll"
        VERBATIM)
elseif(${CMAKE_HOST_SYSTEM_NAME} STREQUAL "Linux")
    set(LLVM_LIBRARY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/lib/Linux)
    set(LLVM_SHARED_LIBRARY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/bin/Linux)
    target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/include
        ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/mustache
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Parser)
    set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O3")
    target_link_libraries(${TARGET_NAME}  ${LLVM_SHARED_LIBRARY_DIR}/libclang.so.12)
elseif(${CMAKE_HOST_SYSTEM_PROCESSOR} STREQUAL "arm64") 
    # link to XCode Toolchains' universal binary libclang.dylib 
    set(LLVM_LIBRARY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/lib/macOS)
    set(LLVM_SHARED_LIBRARY_DIR  ${OSX_DEVELOPER_ROOT}/Toolchains/XcodeDefault.xctoolchain/usr/lib)
    target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/include
        ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/mustache
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Parser)
    set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O3")
    target_link_libraries(${TARGET_NAME}  ${LLVM_SHARED_LIBRARY_DIR}/libclang.dylib)    
else()
    set(LLVM_LIBRARY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/lib/macOS)
    set(LLVM_SHARED_LIBRARY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/bin/macOS)
    target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/include
        ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/mustache
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Parser)
    set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O3")
    target_link_libraries(${TARGET_NAME}  ${LLVM_SHARED_LIBRARY_DIR}/libclang.dylib)
endif()

if(NOT CMAKE_HOST_WIN32)
    target_compile_definitions(${TARGET_NAME} PRIVATE TIXML_USE_STL)
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${LLVM_SHARED_LIBRARY_DIR}"
            $<TARGET_FILE_DIR:${TARGET_NAME}>)
endif()

