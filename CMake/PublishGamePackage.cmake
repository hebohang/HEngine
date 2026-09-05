if(NOT DEFINED PACKAGE_OUTPUT_ROOT OR NOT IS_ABSOLUTE "${PACKAGE_OUTPUT_ROOT}")
    message(FATAL_ERROR "PACKAGE_OUTPUT_ROOT must be an absolute path")
endif()
if(NOT DEFINED PACKAGE_DIR OR NOT IS_ABSOLUTE "${PACKAGE_DIR}")
    message(FATAL_ERROR "PACKAGE_DIR must be an absolute path")
endif()
if(NOT DEFINED STAGING_DIR OR NOT IS_ABSOLUTE "${STAGING_DIR}")
    message(FATAL_ERROR "STAGING_DIR must be an absolute path")
endif()
if(NOT DEFINED PACKAGE_EXECUTABLE_NAME OR
   NOT PACKAGE_EXECUTABLE_NAME MATCHES "^[A-Za-z0-9._-]+$" OR
   PACKAGE_EXECUTABLE_NAME STREQUAL "." OR PACKAGE_EXECUTABLE_NAME STREQUAL "..")
    message(FATAL_ERROR "PACKAGE_EXECUTABLE_NAME must be a safe executable basename")
endif()
if(NOT DEFINED PACKAGE_PROJECT_ID OR
   NOT PACKAGE_PROJECT_ID MATCHES "^[a-z0-9][a-z0-9._-]*$")
    message(FATAL_ERROR "PACKAGE_PROJECT_ID must be a safe project identifier")
endif()

get_filename_component(packageDirectory "${PACKAGE_DIR}" ABSOLUTE)
get_filename_component(stagingDirectory "${STAGING_DIR}" ABSOLUTE)
get_filename_component(packageOutputRoot "${PACKAGE_OUTPUT_ROOT}" ABSOLUTE)
get_filename_component(packageParent "${packageDirectory}" DIRECTORY)
get_filename_component(stagingParent "${stagingDirectory}" DIRECTORY)
get_filename_component(packageName "${packageDirectory}" NAME)

if(WIN32)
    string(TOLOWER "${packageParent}" packageParentKey)
    string(TOLOWER "${packageOutputRoot}" packageOutputRootKey)
else()
    set(packageParentKey "${packageParent}")
    set(packageOutputRootKey "${packageOutputRoot}")
endif()
if(NOT packageName STREQUAL PACKAGE_EXECUTABLE_NAME OR
   NOT packageParentKey STREQUAL packageOutputRootKey)
    message(FATAL_ERROR "Refusing to publish to an unsafe package directory: ${packageDirectory}")
endif()
string(FIND "${stagingDirectory}" "${packageDirectory}.staging-" stagingPrefixPosition)
if(NOT packageParent STREQUAL stagingParent OR NOT stagingPrefixPosition EQUAL 0 OR
   stagingDirectory STREQUAL "${packageDirectory}.staging-")
    message(FATAL_ERROR "STAGING_DIR must be a unique package staging sibling")
endif()
if(NOT IS_DIRECTORY "${stagingDirectory}")
    message(FATAL_ERROR "Verified staging directory is missing: ${stagingDirectory}")
endif()
if(NOT IS_DIRECTORY "${packageOutputRoot}" OR IS_SYMLINK "${packageOutputRoot}")
    message(FATAL_ERROR "Package output boundary is missing or unsafe")
endif()
file(REAL_PATH "${packageOutputRoot}" realPackageOutputRoot)
file(REAL_PATH "${packageParent}" realPackageParent)
if(WIN32)
    string(TOLOWER "${realPackageOutputRoot}" realPackageOutputRootKey)
    string(TOLOWER "${realPackageParent}" realPackageParentKey)
else()
    set(realPackageOutputRootKey "${realPackageOutputRoot}")
    set(realPackageParentKey "${realPackageParent}")
endif()
if(NOT realPackageOutputRootKey STREQUAL realPackageParentKey)
    message(FATAL_ERROR "Package output boundary resolves through a redirected path")
endif()
set(expectedMarker "HEngine ${PACKAGE_PROJECT_ID} Package v1")
file(READ "${stagingDirectory}/.he-package-root" stagingMarker)
string(STRIP "${stagingMarker}" stagingMarker)
if(NOT stagingMarker STREQUAL expectedMarker)
    message(FATAL_ERROR "Verified staging directory has no valid ownership marker")
endif()

function(he_verify_owned_package directory description)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -DPACKAGE_DIR=${directory}
            -DPACKAGE_EXECUTABLE_NAME=${PACKAGE_EXECUTABLE_NAME}
            -DPACKAGE_PROJECT_ID=${PACKAGE_PROJECT_ID}
            -P "${CMAKE_CURRENT_LIST_DIR}/VerifyOwnedGamePackage.cmake"
        RESULT_VARIABLE validationResult)
    if(NOT validationResult EQUAL 0)
        message(FATAL_ERROR "Refusing to use a ${description} with unexpected contents: ${directory}")
    endif()
endfunction()

function(he_rename_package_directory sourceDirectory destinationDirectory resultVariable)
    set(renameResult 1)
    # Windows can retain a directory handle briefly after the full-start
    # verifier exits. Retry only the already validated fixed package paths;
    # this never broadens or recomputes the move target.
    foreach(renameAttempt RANGE 1 20)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E rename
                "${sourceDirectory}" "${destinationDirectory}"
            RESULT_VARIABLE renameResult
            ERROR_QUIET)
        if(renameResult EQUAL 0)
            break()
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.25)
    endforeach()
    set(${resultVariable} "${renameResult}" PARENT_SCOPE)
endfunction()

function(he_remove_owned_package directory description)
    he_verify_owned_package("${directory}" "${description}")
    get_filename_component(deleteParent "${directory}" DIRECTORY)
    get_filename_component(deleteName "${directory}" NAME)
    string(RANDOM LENGTH 24 ALPHABET 0123456789abcdef deleteNonce)
    set(deleteTombstone
        "${deleteParent}/.${deleteName}.deleting-${deleteNonce}")
    if(EXISTS "${deleteTombstone}" OR IS_SYMLINK "${deleteTombstone}")
        message(FATAL_ERROR "Could not allocate a package deletion tombstone")
    endif()
    he_rename_package_directory(
        "${directory}" "${deleteTombstone}" tombstoneResult)
    if(NOT tombstoneResult EQUAL 0 OR EXISTS "${directory}")
        message(FATAL_ERROR
            "Could not retire the verified ${description}: ${directory}")
    endif()

    # Once renamed, an interrupted recursive delete can leave only a uniquely
    # named tombstone; it can no longer poison the fixed .previous recovery
    # slot or block a later publish.
    file(REMOVE_RECURSE "${deleteTombstone}")
    if(EXISTS "${deleteTombstone}")
        message(WARNING
            "A retired package tombstone could not be fully removed: ${deleteTombstone}")
    endif()
endfunction()

set(backupDirectory "${packageDirectory}.previous")
set(bHadPreviousPackage FALSE)

if(EXISTS "${backupDirectory}")
    he_verify_owned_package("${backupDirectory}" "previous-package backup")
endif()

if(EXISTS "${packageDirectory}")
    he_verify_owned_package("${packageDirectory}" "existing package")
    if(EXISTS "${backupDirectory}")
        # The current final package is valid, so a stale verified backup is no longer needed.
        he_remove_owned_package("${backupDirectory}" "stale previous-package backup")
    endif()
    he_rename_package_directory(
        "${packageDirectory}" "${backupDirectory}" backupResult)
    if(NOT backupResult EQUAL 0)
        message(FATAL_ERROR "Could not preserve the previous package: ${backupResult}")
    endif()
    set(bHadPreviousPackage TRUE)
elseif(EXISTS "${backupDirectory}")
    # Preserve an interrupted publish's only valid package until the new one is live.
    set(bHadPreviousPackage TRUE)
endif()

he_rename_package_directory(
    "${stagingDirectory}" "${packageDirectory}" publishResult)
if(NOT publishResult EQUAL 0)
    if(bHadPreviousPackage)
        he_rename_package_directory(
            "${backupDirectory}" "${packageDirectory}" rollbackResult)
        if(NOT rollbackResult EQUAL 0)
            message(FATAL_ERROR
                "Could not publish the verified package (${publishResult}); rollback also failed "
                "(${rollbackResult}). The previous package remains at: ${backupDirectory}")
        endif()
    endif()
    message(FATAL_ERROR "Could not publish the verified package: ${publishResult}")
endif()

if(EXISTS "${backupDirectory}")
    he_remove_owned_package("${backupDirectory}" "published-package backup")
endif()

message(STATUS "Published verified standalone package: ${packageDirectory}")
