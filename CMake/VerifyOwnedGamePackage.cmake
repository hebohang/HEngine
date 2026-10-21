if(NOT DEFINED PACKAGE_DIR OR NOT IS_ABSOLUTE "${PACKAGE_DIR}")
    message(FATAL_ERROR "PACKAGE_DIR must be an absolute path")
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
if(NOT IS_DIRECTORY "${PACKAGE_DIR}" OR IS_SYMLINK "${PACKAGE_DIR}")
    message(FATAL_ERROR "Owned package must be a regular directory: ${PACKAGE_DIR}")
endif()

set(expectedMarker "HEngine ${PACKAGE_PROJECT_ID} Package v1")
if(NOT EXISTS "${PACKAGE_DIR}/.he-package-root" OR
   IS_DIRECTORY "${PACKAGE_DIR}/.he-package-root" OR
   IS_SYMLINK "${PACKAGE_DIR}/.he-package-root")
    message(FATAL_ERROR "Package ownership marker is missing or unsafe")
endif()
file(READ "${PACKAGE_DIR}/.he-package-root" packageMarker)
string(STRIP "${packageMarker}" packageMarker)
if(NOT packageMarker STREQUAL expectedMarker)
    message(FATAL_ERROR "Package ownership marker is invalid")
endif()

# This is intentionally a safe superset rather than the current release's
# exact schema. It lets a newer package replace an older owned package while
# still refusing to delete unrelated user files.
set(ownedRootEntries
    .he-package-root
    CookReport.json
    D3D12
    Game.manifest.json
    Game.pak
    LICENSE.txt
    Licenses
    msvcp140.dll
    python311.dll
    README.txt
    Scripting
    ${PACKAGE_EXECUTABLE_NAME}.exe
    THIRD_PARTY_NOTICES.txt
    vcruntime140.dll
)

file(GLOB actualRootEntries RELATIVE "${PACKAGE_DIR}" "${PACKAGE_DIR}/*")
foreach(entry IN LISTS actualRootEntries)
    list(FIND ownedRootEntries "${entry}" ownedIndex)
    if(ownedIndex EQUAL -1)
        message(FATAL_ERROR "Owned package contains an unexpected root entry: ${entry}")
    endif()

    set(entryPath "${PACKAGE_DIR}/${entry}")
    if(IS_SYMLINK "${entryPath}")
        message(FATAL_ERROR "Owned package contains a symbolic link or junction: ${entry}")
    endif()
    if(entry STREQUAL "Licenses")
        if(NOT IS_DIRECTORY "${entryPath}")
            message(FATAL_ERROR "Owned package Licenses entry is not a directory")
        endif()
        file(GLOB licenseEntries RELATIVE "${entryPath}" "${entryPath}/*")
        foreach(licenseEntry IN LISTS licenseEntries)
            set(licensePath "${entryPath}/${licenseEntry}")
            if(IS_DIRECTORY "${licensePath}" OR IS_SYMLINK "${licensePath}")
                message(FATAL_ERROR "Owned package contains an unsafe license entry: ${licenseEntry}")
            endif()
        endforeach()
    elseif(entry STREQUAL "D3D12")
        if(NOT IS_DIRECTORY "${entryPath}")
            message(FATAL_ERROR "Owned package D3D12 entry is not a directory")
        endif()
        file(GLOB agilityEntries RELATIVE "${entryPath}" "${entryPath}/*")
        foreach(agilityEntry IN LISTS agilityEntries)
            set(agilityPath "${entryPath}/${agilityEntry}")
            if(NOT agilityEntry STREQUAL "D3D12Core.dll" OR
               IS_DIRECTORY "${agilityPath}" OR IS_SYMLINK "${agilityPath}")
                message(FATAL_ERROR "Owned package contains an unsafe D3D12 entry: ${agilityEntry}")
            endif()
        endforeach()
    elseif(entry STREQUAL "Scripting")
        file(GLOB scriptEntries RELATIVE "${entryPath}" "${entryPath}/*")
        if(NOT IS_DIRECTORY "${entryPath}" OR NOT scriptEntries STREQUAL "CSharp" OR
           NOT IS_DIRECTORY "${entryPath}/CSharp" OR IS_SYMLINK "${entryPath}/CSharp")
            message(FATAL_ERROR "Owned package has unexpected scripting contents")
        endif()
        find_program(scriptVerifierShell NAMES powershell.exe pwsh REQUIRED)
        execute_process(
            COMMAND "${scriptVerifierShell}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_LIST_DIR}/../Tools/Scripting/VerifyPackageRuntime.ps1"
                -Directory "${entryPath}/CSharp"
            RESULT_VARIABLE scriptValidationResult)
        if(NOT scriptValidationResult EQUAL 0)
            message(FATAL_ERROR "Owned C# package failed its runtime/build manifest verification")
        endif()
    elseif(IS_DIRECTORY "${entryPath}")
        message(FATAL_ERROR "Owned package contains an unexpected directory: ${entry}")
    endif()
endforeach()

message(STATUS "Verified owned package safety boundary: ${PACKAGE_DIR}")
