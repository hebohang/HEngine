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

if(CMAKE_HOST_WIN32)
    include("${CMAKE_CURRENT_SOURCE_DIR}/../../../../CMake/LibClang.cmake")
    target_include_directories(${TARGET_NAME} PRIVATE
        "${HE_LIBCLANG_IncludeRoot}"
        "${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/mustache"
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/Parser")
    target_link_libraries(${TARGET_NAME} PRIVATE "${HE_LIBCLANG_Root}/lib/libclang.lib")
    target_compile_definitions(${TARGET_NAME} PRIVATE
        HE_CODEGEN_LIBCLANG_CONFIGURED_VERSION="${HE_LIBCLANG_Version}"
        HE_CODEGEN_LIBCLANG_RESOURCE_DIR="${HE_LIBCLANG_ResourceRoot}")
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${HE_LIBCLANG_Root}/bin/libclang.dll" "$<TARGET_FILE_DIR:${TARGET_NAME}>/libclang.dll"
        VERBATIM)
elseif(APPLE)
    set(HE_LIBCLANG_ROOT "/opt/homebrew/opt/llvm" CACHE PATH "macOS LLVM installation providing libclang >=20")
    find_path(HE_LIBCLANG_INCLUDE_DIR clang-c/Index.h HINTS "${HE_LIBCLANG_ROOT}/include" REQUIRED)
    find_library(HE_LIBCLANG_LIBRARY clang HINTS "${HE_LIBCLANG_ROOT}/lib" REQUIRED)
    find_program(HE_LIBCLANG_COMPILER clang HINTS "${HE_LIBCLANG_ROOT}/bin" NO_DEFAULT_PATH REQUIRED)
    execute_process(COMMAND "${HE_LIBCLANG_COMPILER}" --version
        OUTPUT_VARIABLE heClangVersionText COMMAND_ERROR_IS_FATAL ANY)
    string(REGEX MATCH "clang version ([0-9]+[.][0-9]+[.][0-9]+)" heClangVersionMatch "${heClangVersionText}")
    set(heClangVersion "${CMAKE_MATCH_1}")
    if(NOT heClangVersion OR heClangVersion VERSION_LESS 20)
        message(FATAL_ERROR "HEngineParser requires libclang >=20: ${heClangVersionText}")
    endif()
    execute_process(COMMAND "${HE_LIBCLANG_COMPILER}" -print-resource-dir
        OUTPUT_VARIABLE heClangResourceDir OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    target_include_directories(${TARGET_NAME} PRIVATE
        "${HE_LIBCLANG_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/mustache"
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/Parser")
    target_link_libraries(${TARGET_NAME} PRIVATE "${HE_LIBCLANG_LIBRARY}")
    target_compile_definitions(${TARGET_NAME} PRIVATE
        HE_CODEGEN_LIBCLANG_CONFIGURED_VERSION="${heClangVersion}"
        HE_CODEGEN_LIBCLANG_RESOURCE_DIR="${heClangResourceDir}")
else()
    message(FATAL_ERROR "HEngineParser currently supports Windows and macOS hosts.")
endif()
