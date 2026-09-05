foreach(requiredVariable IN ITEMS
    ENGINE_CONTENT_DIR
    ENGINE_GENERATED_CONTENT_DIR
    PROJECT_DIR
    COOK_INPUT_DIR
    COOK_INPUT_PARENT
    PROJECT_ID)
    if(NOT DEFINED ${requiredVariable} OR "${${requiredVariable}}" STREQUAL "")
        message(FATAL_ERROR "${requiredVariable} is required")
    endif()
endforeach()

foreach(pathVariable IN ITEMS ENGINE_CONTENT_DIR ENGINE_GENERATED_CONTENT_DIR PROJECT_DIR COOK_INPUT_DIR COOK_INPUT_PARENT)
    if(NOT IS_ABSOLUTE "${${pathVariable}}")
        message(FATAL_ERROR "${pathVariable} must be an absolute path")
    endif()
endforeach()

get_filename_component(engineContentDirectory "${ENGINE_CONTENT_DIR}" ABSOLUTE)
get_filename_component(engineGeneratedContentDirectory "${ENGINE_GENERATED_CONTENT_DIR}" ABSOLUTE)
get_filename_component(projectDirectory "${PROJECT_DIR}" ABSOLUTE)
get_filename_component(cookInputDirectory "${COOK_INPUT_DIR}" ABSOLUTE)
get_filename_component(cookInputParent "${COOK_INPUT_PARENT}" ABSOLUTE)
get_filename_component(actualCookInputParent "${cookInputDirectory}" DIRECTORY)
get_filename_component(cookInputName "${cookInputDirectory}" NAME)

if(NOT actualCookInputParent STREQUAL cookInputParent OR
   NOT cookInputName MATCHES "^CookInput-[0-9a-f]+$")
    message(FATAL_ERROR "COOK_INPUT_DIR is outside the dedicated build boundary")
endif()
if(NOT IS_DIRECTORY "${engineContentDirectory}")
    message(FATAL_ERROR "Engine content directory does not exist: ${engineContentDirectory}")
endif()
if(NOT IS_DIRECTORY "${engineGeneratedContentDirectory}")
    message(FATAL_ERROR "Engine generated content directory does not exist: ${engineGeneratedContentDirectory}")
endif()
if(NOT IS_DIRECTORY "${projectDirectory}")
    message(FATAL_ERROR "Project directory does not exist: ${projectDirectory}")
endif()

set(expectedMarker "HEngine CookInput ${PROJECT_ID} v1")
if(EXISTS "${cookInputDirectory}")
    if(IS_SYMLINK "${cookInputDirectory}")
        message(FATAL_ERROR "Refusing to replace a symbolic-link cook input directory")
    endif()
    file(REAL_PATH "${cookInputDirectory}" realCookInputDirectory)
    file(REAL_PATH "${cookInputParent}" realCookInputParent)
    get_filename_component(realCookInputActualParent "${realCookInputDirectory}" DIRECTORY)
    get_filename_component(realCookInputName "${realCookInputDirectory}" NAME)
    string(TOLOWER "${realCookInputActualParent}" realCookInputActualParentKey)
    string(TOLOWER "${realCookInputParent}" realCookInputParentKey)
    string(TOLOWER "${realCookInputName}" realCookInputNameKey)
    string(TOLOWER "${cookInputName}" cookInputNameKey)
    if(NOT realCookInputActualParentKey STREQUAL realCookInputParentKey OR
       NOT realCookInputNameKey STREQUAL cookInputNameKey)
        message(FATAL_ERROR "Refusing to replace a junction or redirected cook input directory")
    endif()
    set(markerPath "${cookInputDirectory}/.he-cook-input")
    if(NOT EXISTS "${markerPath}" OR IS_DIRECTORY "${markerPath}" OR IS_SYMLINK "${markerPath}")
        message(FATAL_ERROR "Refusing to replace an unowned cook input directory")
    endif()
    file(READ "${markerPath}" actualMarker)
    string(STRIP "${actualMarker}" actualMarker)
    if(NOT actualMarker STREQUAL expectedMarker)
        message(FATAL_ERROR "Refusing to replace a cook input owned by another project")
    endif()
    file(REMOVE_RECURSE "${cookInputDirectory}")
endif()

file(MAKE_DIRECTORY "${cookInputDirectory}")
file(WRITE "${cookInputDirectory}/.he-cook-input" "${expectedMarker}\n")

function(he_assert_bounded_content_tree baseDirectory contentDirectory description)
    if(NOT IS_DIRECTORY "${contentDirectory}")
        return()
    endif()
    if(IS_SYMLINK "${contentDirectory}")
        message(FATAL_ERROR "${description} must not be a symbolic link")
    endif()

    file(REAL_PATH "${baseDirectory}" realBaseDirectory)
    file(REAL_PATH "${contentDirectory}" realContentDirectory)
    file(RELATIVE_PATH contentRelativeToBase
        "${realBaseDirectory}" "${realContentDirectory}")
    if(IS_ABSOLUTE "${contentRelativeToBase}" OR
       contentRelativeToBase MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "${description} resolves outside its declared root")
    endif()

    file(GLOB_RECURSE contentEntries LIST_DIRECTORIES true "${contentDirectory}/*")
    foreach(contentEntry IN LISTS contentEntries)
        if(IS_SYMLINK "${contentEntry}")
            message(FATAL_ERROR "${description} contains a symbolic link: ${contentEntry}")
        endif()
        file(REAL_PATH "${contentEntry}" realContentEntry)
        file(RELATIVE_PATH entryRelativeToContent
            "${realContentDirectory}" "${realContentEntry}")
        if(IS_ABSOLUTE "${entryRelativeToContent}" OR
           entryRelativeToContent MATCHES "(^|/)\\.\\.(/|$)")
            message(FATAL_ERROR "${description} contains a junction or redirected entry: ${contentEntry}")
        endif()
    endforeach()
endfunction()

function(he_overlay_copy_content_tree sourceDirectory destinationDirectory description)
    if(NOT IS_DIRECTORY "${sourceDirectory}")
        return()
    endif()

    # file(COPY) may treat equal timestamps as up-to-date. Git checkouts can
    # give an engine default and a project override identical timestamps, so
    # copy each payload by content to preserve deterministic overlay priority.
    file(GLOB_RECURSE sourceFiles
        LIST_DIRECTORIES false
        RELATIVE "${sourceDirectory}"
        "${sourceDirectory}/*")
    list(SORT sourceFiles)
    foreach(relativeFile IN LISTS sourceFiles)
        set(skipFile FALSE)
        string(TOLOWER "${relativeFile}" relativeFileKey)
        foreach(excludedPrefix IN LISTS ARGN)
            string(TOLOWER "${excludedPrefix}" excludedPrefixKey)
            if(NOT excludedPrefixKey)
                continue()
            endif()
            set(excludedPrefixBoundary "${excludedPrefixKey}/")
            string(FIND "${relativeFileKey}" "${excludedPrefixBoundary}"
                excludedPrefixPosition)
            if(relativeFileKey STREQUAL excludedPrefixKey OR
               excludedPrefixPosition EQUAL 0)
                set(skipFile TRUE)
                break()
            endif()
        endforeach()
        if(skipFile)
            continue()
        endif()
        set(sourceFile "${sourceDirectory}/${relativeFile}")
        set(destinationFile "${destinationDirectory}/${relativeFile}")
        get_filename_component(destinationParent "${destinationFile}" DIRECTORY)
        file(MAKE_DIRECTORY "${destinationParent}")
        if(IS_DIRECTORY "${destinationFile}")
            message(FATAL_ERROR
                "${description} file conflicts with an existing directory: ${relativeFile}")
        endif()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${sourceFile}" "${destinationFile}"
            RESULT_VARIABLE copyResult)
        if(NOT copyResult EQUAL 0)
            message(FATAL_ERROR
                "Could not overlay ${description} file: ${relativeFile}")
        endif()
    endforeach()
endfunction()

# Preserve the current HCooker dependency-closure format while giving it a
# deterministic overlay. Engine content is the fallback; project files may
# intentionally override the same logical path. SourceAssets, Saved, tools and
# distribution outputs never enter this boundary.
foreach(contentRoot IN ITEMS Assets Config Scripts Shaders)
    he_assert_bounded_content_tree(
        "${engineContentDirectory}"
        "${engineContentDirectory}/${contentRoot}"
        "Engine content ${contentRoot}")
    he_assert_bounded_content_tree(
        "${engineGeneratedContentDirectory}"
        "${engineGeneratedContentDirectory}/${contentRoot}"
        "Engine generated content ${contentRoot}")
    he_assert_bounded_content_tree(
        "${projectDirectory}"
        "${projectDirectory}/${contentRoot}"
        "Project content ${contentRoot}")
    set(engineExcludedPrefixes "DevOnly")
    set(generatedExcludedPrefixes "DevOnly")
    if(contentRoot STREQUAL "Config")
        # User settings are never shipping content. Generated content also
        # cannot replace the immutable Engine-owned product default.
        list(APPEND engineExcludedPrefixes "UserEngine.json")
        list(APPEND generatedExcludedPrefixes
            "DefaultEngine.json"
            "UserEngine.json")
    endif()
    he_overlay_copy_content_tree(
        "${engineContentDirectory}/${contentRoot}"
        "${cookInputDirectory}/${contentRoot}"
        "Engine content ${contentRoot}"
        ${engineExcludedPrefixes})
    he_overlay_copy_content_tree(
        "${engineGeneratedContentDirectory}/${contentRoot}"
        "${cookInputDirectory}/${contentRoot}"
        "Engine generated content ${contentRoot}"
        ${generatedExcludedPrefixes})
    set(projectExcludedPrefixes "DevOnly")
    if(contentRoot STREQUAL "Shaders")
        # Package shaders are freshly rebuilt into GeneratedContent below.
        # Never let a developer's loose/historical project cache overwrite it.
        list(APPEND projectExcludedPrefixes "CACHES")
    elseif(contentRoot STREQUAL "Scripts")
        # The embedded Python runtime is Engine-owned in both development and
        # packaged builds. Ignore legacy project copies and bytecode caches.
        list(APPEND projectExcludedPrefixes "Lib")
    elseif(contentRoot STREQUAL "Config")
        # Plugin enablement is a build-host input. Shipping must consume only
        # the generated Game host graph. DefaultEngine is immutable Engine
        # content, while UserEngine belongs under Saved and never ships.
        list(APPEND projectExcludedPrefixes
            "Plugins.json"
            "DefaultEngine.json"
            "UserEngine.json")
    endif()
    he_overlay_copy_content_tree(
        "${projectDirectory}/${contentRoot}"
        "${cookInputDirectory}/${contentRoot}"
        "Project content ${contentRoot}"
        ${projectExcludedPrefixes})
endforeach()

set(authoritativeDefaultEngine
    "${engineContentDirectory}/Config/DefaultEngine.json")
set(cookedDefaultEngine
    "${cookInputDirectory}/Config/DefaultEngine.json")
if(NOT EXISTS "${authoritativeDefaultEngine}" OR
   IS_DIRECTORY "${authoritativeDefaultEngine}" OR
   IS_SYMLINK "${authoritativeDefaultEngine}")
    message(FATAL_ERROR
        "Engine Config/DefaultEngine.json is missing or unsafe")
endif()
file(MAKE_DIRECTORY "${cookInputDirectory}/Config")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${authoritativeDefaultEngine}" "${cookedDefaultEngine}"
    RESULT_VARIABLE defaultEngineCopyResult)
if(NOT defaultEngineCopyResult EQUAL 0)
    message(FATAL_ERROR
        "Could not publish the authoritative Engine Config/DefaultEngine.json")
endif()
file(SHA256 "${authoritativeDefaultEngine}" authoritativeDefaultEngineHash)
file(SHA256 "${cookedDefaultEngine}" cookedDefaultEngineHash)
if(NOT cookedDefaultEngineHash STREQUAL authoritativeDefaultEngineHash)
    message(FATAL_ERROR
        "Cook input Config/DefaultEngine.json does not match the Engine default")
endif()
if(EXISTS "${cookInputDirectory}/Config/UserEngine.json")
    message(FATAL_ERROR
        "Cook input must never contain Config/UserEngine.json")
endif()

message(STATUS "Prepared isolated cook input for ${PROJECT_ID}: ${cookInputDirectory}")
