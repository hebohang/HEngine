cmake_minimum_required(VERSION 3.24)
if(NOT EXISTS "${HE_DXC_INPUT}" OR NOT HE_DXC_OUTPUT)
    message(FATAL_ERROR "DXC deployment requires HE_DXC_INPUT and HE_DXC_OUTPUT")
endif()

# Keep the compiler self-contained alongside HShaderCompiler, including when
# invoked from Editor.app. No SDK install or DYLD_LIBRARY_PATH is required.
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${HE_DXC_INPUT}"
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    POST_EXCLUDE_REGEXES "^/System/Library/" "^/usr/lib/")
if(unresolved)
    message(FATAL_ERROR "Unresolved native DXC dependencies: ${unresolved}")
endif()
file(MAKE_DIRECTORY "${HE_DXC_OUTPUT}")
file(COPY_FILE "${HE_DXC_INPUT}" "${HE_DXC_OUTPUT}/dxc" ONLY_IF_DIFFERENT)
set(outputs "${HE_DXC_OUTPUT}/dxc")
foreach(dependency IN LISTS resolved)
    get_filename_component(name "${dependency}" NAME)
    file(COPY_FILE "${dependency}" "${HE_DXC_OUTPUT}/${name}" ONLY_IF_DIFFERENT)
    list(APPEND outputs "${HE_DXC_OUTPUT}/${name}")
endforeach()
foreach(binary IN LISTS outputs)
    file(CHMOD "${binary}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    # DXC also opens its compiler library through @rpath at runtime.
    execute_process(COMMAND otool -l "${binary}" OUTPUT_VARIABLE loadCommands COMMAND_ERROR_IS_FATAL ANY)
    if(NOT loadCommands MATCHES "path @loader_path ")
        execute_process(COMMAND install_name_tool -add_rpath "@loader_path" "${binary}" COMMAND_ERROR_IS_FATAL ANY)
    endif()
    execute_process(COMMAND otool -L "${binary}" OUTPUT_VARIABLE linkage COMMAND_ERROR_IS_FATAL ANY)
    string(REPLACE "\n" ";" lines "${linkage}")
    foreach(line IN LISTS lines)
        if(line MATCHES "^[ \t]+([^ \t]+) ")
            set(original "${CMAKE_MATCH_1}")
            get_filename_component(name "${original}" NAME)
            if(EXISTS "${HE_DXC_OUTPUT}/${name}" AND NOT original STREQUAL "@loader_path/${name}")
                execute_process(COMMAND install_name_tool -change "${original}" "@loader_path/${name}" "${binary}"
                    COMMAND_ERROR_IS_FATAL ANY)
            endif()
        endif()
    endforeach()
    if(binary MATCHES "\\.dylib$")
        get_filename_component(name "${binary}" NAME)
        execute_process(COMMAND install_name_tool -id "@loader_path/${name}" "${binary}" COMMAND_ERROR_IS_FATAL ANY)
    endif()
    execute_process(COMMAND codesign --force --sign - "${binary}" COMMAND_ERROR_IS_FATAL ANY)
endforeach()
