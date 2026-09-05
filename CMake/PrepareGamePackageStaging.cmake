foreach(requiredVariable IN ITEMS
    PROJECT_DIR PACKAGE_OUTPUT_ROOT PACKAGE_DIR STAGING_DIR
    PACKAGE_EXECUTABLE_NAME PACKAGE_PROJECT_ID PACKAGE_NONCE)
    if(NOT DEFINED ${requiredVariable} OR "${${requiredVariable}}" STREQUAL "")
        message(FATAL_ERROR "${requiredVariable} is required")
    endif()
endforeach()
if(NOT IS_ABSOLUTE "${PROJECT_DIR}" OR NOT IS_ABSOLUTE "${PACKAGE_OUTPUT_ROOT}" OR
   NOT IS_ABSOLUTE "${PACKAGE_DIR}" OR
   NOT IS_ABSOLUTE "${STAGING_DIR}")
    message(FATAL_ERROR "Project, package and staging directories must be absolute")
endif()
if(NOT PACKAGE_EXECUTABLE_NAME MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$" OR
   PACKAGE_EXECUTABLE_NAME MATCHES "[.]$")
    message(FATAL_ERROR "PACKAGE_EXECUTABLE_NAME must be a safe basename")
endif()
if(NOT PACKAGE_PROJECT_ID MATCHES "^[a-z0-9][a-z0-9._-]*$" OR
   PACKAGE_PROJECT_ID MATCHES "[.]$")
    message(FATAL_ERROR "PACKAGE_PROJECT_ID must be a safe identifier")
endif()
string(LENGTH "${PACKAGE_NONCE}" packageNonceLength)
if(NOT packageNonceLength EQUAL 12 OR NOT PACKAGE_NONCE MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "PACKAGE_NONCE must be twelve lowercase hexadecimal characters")
endif()

get_filename_component(packageDirectory "${PACKAGE_DIR}" ABSOLUTE)
get_filename_component(stagingDirectory "${STAGING_DIR}" ABSOLUTE)
get_filename_component(projectDirectory "${PROJECT_DIR}" ABSOLUTE)
get_filename_component(packageOutputRoot "${PACKAGE_OUTPUT_ROOT}" ABSOLUTE)
get_filename_component(packageParent "${packageDirectory}" DIRECTORY)
get_filename_component(stagingParent "${stagingDirectory}" DIRECTORY)
get_filename_component(packageName "${packageDirectory}" NAME)
get_filename_component(stagingName "${stagingDirectory}" NAME)
set(expectedStagingName "${PACKAGE_EXECUTABLE_NAME}.staging-${PACKAGE_NONCE}")
set(expectedPackageParent "${packageOutputRoot}")

if(WIN32)
    string(TOLOWER "${packageParent}" packageParentLexicalKey)
    string(TOLOWER "${expectedPackageParent}" expectedPackageParentLexicalKey)
else()
    set(packageParentLexicalKey "${packageParent}")
    set(expectedPackageParentLexicalKey "${expectedPackageParent}")
endif()

if(NOT packageName STREQUAL PACKAGE_EXECUTABLE_NAME OR
   NOT stagingName STREQUAL expectedStagingName OR
   NOT stagingParent STREQUAL packageParent OR
   NOT packageParentLexicalKey STREQUAL expectedPackageParentLexicalKey)
    message(FATAL_ERROR "Staging must be the exact nonce-owned sibling of the package directory")
endif()

# Never clear a pre-existing path here. A failed build is recovered by
# reconfiguring (which creates a new nonce); an attacker-controlled junction
# must not be traversed or overwritten before verification.
if(EXISTS "${stagingDirectory}" OR IS_SYMLINK "${stagingDirectory}")
    message(FATAL_ERROR
        "Package staging already exists; reconfigure to obtain a fresh nonce: ${stagingDirectory}")
endif()

if(EXISTS "${packageParent}" OR IS_SYMLINK "${packageParent}")
    if(NOT IS_DIRECTORY "${packageParent}" OR IS_SYMLINK "${packageParent}")
        message(FATAL_ERROR "Package output boundary is not a regular directory")
    endif()
endif()
file(MAKE_DIRECTORY "${packageParent}")
if(NOT IS_DIRECTORY "${packageParent}" OR IS_SYMLINK "${packageParent}")
    message(FATAL_ERROR "Could not create a regular package staging parent")
endif()
file(MAKE_DIRECTORY "${stagingDirectory}")
if(NOT IS_DIRECTORY "${stagingDirectory}" OR IS_SYMLINK "${stagingDirectory}")
    message(FATAL_ERROR "Package staging is not a regular directory")
endif()

file(REAL_PATH "${projectDirectory}" realProjectDirectory)
file(REAL_PATH "${packageParent}" realPackageParent)
file(REAL_PATH "${packageOutputRoot}" realPackageOutputRoot)
file(REAL_PATH "${stagingDirectory}" realStagingDirectory)
get_filename_component(realStagingParent "${realStagingDirectory}" DIRECTORY)
get_filename_component(realStagingName "${realStagingDirectory}" NAME)
if(WIN32)
    string(TOLOWER "${realPackageOutputRoot}" expectedRealPackageParentKey)
    string(TOLOWER "${realPackageParent}" realPackageParentKey)
    string(TOLOWER "${realStagingParent}" realStagingParentKey)
    string(TOLOWER "${realStagingName}" realStagingNameKey)
    string(TOLOWER "${expectedStagingName}" expectedStagingNameKey)
else()
    set(expectedRealPackageParentKey "${realPackageOutputRoot}")
    set(realPackageParentKey "${realPackageParent}")
    set(realStagingParentKey "${realStagingParent}")
    set(realStagingNameKey "${realStagingName}")
    set(expectedStagingNameKey "${expectedStagingName}")
endif()
if(NOT realPackageParentKey STREQUAL expectedRealPackageParentKey OR
   NOT realPackageParentKey STREQUAL realStagingParentKey OR
   NOT realStagingNameKey STREQUAL expectedStagingNameKey)
    message(FATAL_ERROR "Package staging resolves through a junction or redirected path")
endif()

file(GLOB stagingEntries LIST_DIRECTORIES true "${stagingDirectory}/*")
if(stagingEntries)
    message(FATAL_ERROR "New package staging directory is unexpectedly non-empty")
endif()
file(WRITE "${stagingDirectory}/.he-package-root"
    "HEngine ${PACKAGE_PROJECT_ID} Package v1\n")
message(STATUS "Prepared safe package staging boundary: ${stagingDirectory}")
