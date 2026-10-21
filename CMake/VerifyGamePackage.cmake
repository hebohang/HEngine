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
if(NOT DEFINED PACKAGE_LICENSE_LIST_FILE OR
   NOT IS_ABSOLUTE "${PACKAGE_LICENSE_LIST_FILE}" OR
   NOT EXISTS "${PACKAGE_LICENSE_LIST_FILE}")
    message(FATAL_ERROR "PACKAGE_LICENSE_LIST_FILE must name the generated license allow-list")
endif()

if(NOT IS_DIRECTORY "${PACKAGE_DIR}")
    message(FATAL_ERROR "Package directory does not exist: ${PACKAGE_DIR}")
endif()
if(IS_SYMLINK "${PACKAGE_DIR}")
    message(FATAL_ERROR "Package directory must not be a symbolic link or junction")
endif()

set(requiredRootEntries
    .he-package-root
    D3D12
    Game.manifest.json
    Game.pak
    HEngine.Runtime.json
    LICENSE.txt
    Licenses
    README.txt
    ${PACKAGE_EXECUTABLE_NAME}.exe
    THIRD_PARTY_NOTICES.txt
)
file(READ "${PACKAGE_DIR}/HEngine.Runtime.json" runtimeContract)
string(JSON runtimeSchema GET "${runtimeContract}" schema)
if(NOT runtimeSchema EQUAL 1)
    message(FATAL_ERROR "Unsupported native runtime dependency contract")
endif()
foreach(flag fsr dx12 vulkan oidn)
    string(JSON runtimeType TYPE "${runtimeContract}" ${flag})
    if(NOT runtimeType STREQUAL "BOOLEAN")
        message(FATAL_ERROR "Invalid native runtime flag: ${flag}")
    endif()
    string(JSON runtime_${flag} GET "${runtimeContract}" ${flag})
endforeach()
if(runtime_oidn)
    message(FATAL_ERROR "OIDN/offline renderer is forbidden in Shipping")
endif()
if(runtime_fsr)
    string(JSON runtimeArchitecture ERROR_VARIABLE architectureError GET "${runtimeContract}" architecture)
    list(APPEND requiredRootEntries msvcp140.dll vcruntime140.dll)
    if(NOT runtimeArchitecture STREQUAL "arm64" OR EXISTS "${PACKAGE_DIR}/vcruntime140_1.dll")
        list(APPEND requiredRootEntries vcruntime140_1.dll)
    endif()
    foreach(backend Dx12 Vulkan)
        string(TOLOWER "${backend}" graphicsFlag)
        string(JSON fsrBackendType ERROR_VARIABLE backendError TYPE "${runtimeContract}" "fsr${backend}")
        if(backendError)
            if(runtimeArchitecture STREQUAL "arm64")
                message(FATAL_ERROR "ARM64 requires explicit FSR backend capabilities")
            endif()
            set(runtime_fsr${backend} ${runtime_${graphicsFlag}})
        else()
            if(NOT fsrBackendType STREQUAL "BOOLEAN")
                message(FATAL_ERROR "Invalid FSR backend capability")
            endif()
            string(JSON runtime_fsr${backend} GET "${runtimeContract}" "fsr${backend}")
        endif()
    endforeach()
    if(runtime_fsrDx12)
        list(APPEND requiredRootEntries amd_fidelityfx_dx12.dll)
    endif()
    if(runtime_fsrVulkan)
        list(APPEND requiredRootEntries amd_fidelityfx_vk.dll)
    endif()
endif()
if(NOT DEFINED PACKAGE_PYTHON_PLUGIN)
    set(PACKAGE_PYTHON_PLUGIN enabled)
endif()
if(NOT DEFINED PACKAGE_CSHARP_PLUGIN)
    set(PACKAGE_CSHARP_PLUGIN disabled)
endif()
foreach(mode PACKAGE_PYTHON_PLUGIN PACKAGE_CSHARP_PLUGIN)
    if(NOT "${${mode}}" MATCHES "^(enabled|disabled)$")
        message(FATAL_ERROR "Invalid script plugin package mode: ${mode}")
    endif()
endforeach()
if(PACKAGE_PYTHON_PLUGIN STREQUAL "enabled")
    list(APPEND requiredRootEntries python311.dll vcruntime140.dll)
endif()
list(REMOVE_DUPLICATES requiredRootEntries)
if(PACKAGE_CSHARP_PLUGIN STREQUAL "enabled")
    list(APPEND requiredRootEntries Scripting)
    file(GLOB scriptDirectories RELATIVE "${PACKAGE_DIR}/Scripting" "${PACKAGE_DIR}/Scripting/*")
    if(NOT scriptDirectories STREQUAL "CSharp")
        message(FATAL_ERROR "Scripting package must contain only the selected CSharp runtime")
    endif()
endif()

file(READ "${PACKAGE_DIR}/.he-package-root" packageMarker)
string(STRIP "${packageMarker}" packageMarker)
if(NOT packageMarker STREQUAL "HEngine ${PACKAGE_PROJECT_ID} Package v1")
    message(FATAL_ERROR "Package ownership marker is missing or invalid")
endif()

file(GLOB actualRootEntries RELATIVE "${PACKAGE_DIR}" "${PACKAGE_DIR}/*")
list(SORT actualRootEntries)
list(SORT requiredRootEntries)
if(NOT actualRootEntries STREQUAL requiredRootEntries)
    message(FATAL_ERROR
        "Package root contains missing or unexpected entries.\n"
        "Expected: ${requiredRootEntries}\n"
        "Actual:   ${actualRootEntries}")
endif()

foreach(fileName IN LISTS requiredRootEntries)
    if(fileName STREQUAL "Licenses" OR fileName STREQUAL "D3D12" OR fileName STREQUAL "Scripting")
        if(NOT IS_DIRECTORY "${PACKAGE_DIR}/${fileName}" OR IS_SYMLINK "${PACKAGE_DIR}/${fileName}")
            message(FATAL_ERROR "Required package directory is missing or unsafe: ${fileName}")
        endif()
        continue()
    endif()
    if(NOT EXISTS "${PACKAGE_DIR}/${fileName}" OR IS_DIRECTORY "${PACKAGE_DIR}/${fileName}" OR
       IS_SYMLINK "${PACKAGE_DIR}/${fileName}")
        message(FATAL_ERROR "Required package file is missing: ${fileName}")
    endif()
    file(SIZE "${PACKAGE_DIR}/${fileName}" fileSize)
    if(fileSize EQUAL 0)
        message(FATAL_ERROR "Required package file is empty: ${fileName}")
    endif()
endforeach()

if(NOT DEFINED PACKAGE_POLICY_TOKENS)
    set(PACKAGE_POLICY_TOKENS
        OfflineReferenceRenderer
        he.offline-reference-renderer
        ReferencePathTracer
        HardwareRayTracingReference
        PackageNever)
endif()
if(NOT DEFINED PACKAGE_POLICY_CONTENT_PREFIXES)
    set(PACKAGE_POLICY_CONTENT_PREFIXES
        Assets/DevOnly
        Config/DevOnly
        Config/Plugins.json
        Config/UserEngine.json
        Saved/Config/UserEngine.json
        Scripts/DevOnly
        Shaders/DevOnly)
endif()

function(he_assert_package_policy_value value context)
    string(TOLOWER "${value}" foldedValue)
    foreach(policyToken IN LISTS PACKAGE_POLICY_TOKENS)
        string(TOLOWER "${policyToken}" foldedToken)
        string(FIND "${foldedValue}" "${foldedToken}" policyTokenPosition)
        if(NOT policyTokenPosition EQUAL -1)
            message(FATAL_ERROR
                "${context} contains forbidden package token '${policyToken}'")
        endif()
    endforeach()

    string(REPLACE "\\" "/" normalizedValue "${value}")
    string(TOLOWER "${normalizedValue}" foldedPath)
    foreach(policyPrefix IN LISTS PACKAGE_POLICY_CONTENT_PREFIXES)
        string(REPLACE "\\" "/" normalizedPrefix "${policyPrefix}")
        string(REGEX REPLACE "^/+|/+$" "" normalizedPrefix "${normalizedPrefix}")
        string(TOLOWER "${normalizedPrefix}" foldedPrefix)
        string(FIND "${foldedPath}" "${foldedPrefix}/" prefixAtStart)
        string(FIND "${foldedPath}" "/${foldedPrefix}/" prefixInside)
        string(FIND "${foldedPath}" "/${foldedPrefix}" prefixAtEnd)
        if(foldedPath STREQUAL foldedPrefix OR prefixAtStart EQUAL 0 OR
           NOT prefixInside EQUAL -1 OR
           (NOT prefixAtEnd EQUAL -1 AND
            foldedPath MATCHES "/${foldedPrefix}$"))
            message(FATAL_ERROR
                "${context} enters forbidden developer-only content prefix '${policyPrefix}'")
        endif()
    endforeach()
endfunction()

file(READ "${PACKAGE_DIR}/Game.manifest.json" packageManifest)
string(JSON packageManifestType ERROR_VARIABLE packageManifestTypeError
    TYPE "${packageManifest}")
if(packageManifestTypeError OR NOT packageManifestType STREQUAL "OBJECT")
    message(FATAL_ERROR "Game.manifest.json must contain a JSON object")
endif()

foreach(topLevelString IN ITEMS GameName EntryScene PakFile ContentVersion)
    string(JSON topLevelType ERROR_VARIABLE topLevelTypeError
        TYPE "${packageManifest}" ${topLevelString})
    if(NOT topLevelTypeError)
        if(NOT topLevelType STREQUAL "STRING")
            message(FATAL_ERROR
                "Game.manifest.json ${topLevelString} must be a string")
        endif()
        string(JSON topLevelValue GET "${packageManifest}" ${topLevelString})
        he_assert_package_policy_value(
            "${topLevelValue}"
            "Game.manifest.json ${topLevelString}")
    endif()
endforeach()

string(JSON manifestFileCount ERROR_VARIABLE manifestFilesError
    LENGTH "${packageManifest}" Files)
if(manifestFilesError)
    message(FATAL_ERROR "Game.manifest.json package policy requires a Files array")
endif()

# Build the complete path index before validating dependency edges. A folded
# SHA-256 variable key keeps Windows lookup case-insensitive without putting
# manifest paths into CMake list values.
if(manifestFileCount GREATER 0)
    math(EXPR manifestFileLast "${manifestFileCount} - 1")
    foreach(fileIndex RANGE ${manifestFileLast})
        # Parse the large manifest once per file, then inspect the small entry.
        string(JSON fileEntry GET "${packageManifest}" Files ${fileIndex})
        set(manifestFileEntryByIndex_${fileIndex} "${fileEntry}")
        string(JSON filePathType ERROR_VARIABLE filePathTypeError
            TYPE "${fileEntry}" Path)
        if(filePathTypeError OR NOT filePathType STREQUAL "STRING")
            message(FATAL_ERROR
                "Game.manifest.json Files[${fileIndex}].Path must be a string")
        endif()
        string(JSON filePath GET "${fileEntry}" Path)
        he_assert_package_policy_value(
            "${filePath}"
            "Game.manifest.json Files[${fileIndex}].Path")
        string(REPLACE "\\" "/" normalizedFilePath "${filePath}")
        if(NOT "${filePath}" STREQUAL "${normalizedFilePath}")
            message(FATAL_ERROR
                "Game.manifest.json Files[${fileIndex}].Path must use canonical forward slashes: ${filePath}")
        endif()
        string(TOLOWER "${normalizedFilePath}" normalizedFilePathKey)
        string(SHA256 normalizedFilePathHash "${normalizedFilePathKey}")
        set(filePathVariable "manifestFilePath_${normalizedFilePathHash}")
        set(fileIndexVariable "manifestFileIndex_${normalizedFilePathHash}")
        if(DEFINED ${filePathVariable})
            message(FATAL_ERROR
                "Game.manifest.json Files[${fileIndex}].Path '${normalizedFilePath}' "
                "collides case-insensitively with Files[${${fileIndexVariable}}].Path "
                "'${${filePathVariable}}'")
        endif()
        set(${filePathVariable} "${normalizedFilePath}")
        set(${fileIndexVariable} "${fileIndex}")
        set(manifestFilePathByIndex_${fileIndex} "${normalizedFilePath}")
    endforeach()
endif()

set(defaultEngineSettingsCount 0)
set(projectEngineSettingsCount 0)
set(userEngineSettingsCount 0)
set(dxilShaderArtifactCount 0)
set(spirvShaderArtifactCount 0)
set(dxilMaterialArtifactCount 0)
set(spirvMaterialArtifactCount 0)
if(manifestFileCount GREATER 0)
    math(EXPR manifestFileLast "${manifestFileCount} - 1")
    foreach(fileIndex RANGE ${manifestFileLast})
        set(fileEntry "${manifestFileEntryByIndex_${fileIndex}}")
        set(normalizedFilePath "${manifestFilePathByIndex_${fileIndex}}")
        string(TOLOWER "${normalizedFilePath}" normalizedFilePathKey)
        string(FIND "${normalizedFilePathKey}" "shaders/caches/dxil/" dxilArtifactPosition)
        string(FIND "${normalizedFilePathKey}" "shaders/caches/spirv/" spirvArtifactPosition)
        if(dxilArtifactPosition EQUAL 0)
            math(EXPR dxilShaderArtifactCount "${dxilShaderArtifactCount} + 1")
            string(FIND "${normalizedFilePathKey}" "shaders/caches/dxil/materials/" dxilMaterialPosition)
            if(dxilMaterialPosition EQUAL 0)
                math(EXPR dxilMaterialArtifactCount "${dxilMaterialArtifactCount} + 1")
            endif()
        elseif(spirvArtifactPosition EQUAL 0)
            math(EXPR spirvShaderArtifactCount "${spirvShaderArtifactCount} + 1")
            string(FIND "${normalizedFilePathKey}" "shaders/caches/spirv/materials/" spirvMaterialPosition)
            if(spirvMaterialPosition EQUAL 0)
                math(EXPR spirvMaterialArtifactCount "${spirvMaterialArtifactCount} + 1")
            endif()
        endif()
        if(normalizedFilePathKey STREQUAL "config/defaultengine.json")
            math(EXPR defaultEngineSettingsCount
                "${defaultEngineSettingsCount} + 1")
        elseif(normalizedFilePathKey STREQUAL "config/baseengine.json")
            math(EXPR projectEngineSettingsCount
                "${projectEngineSettingsCount} + 1")
        elseif(normalizedFilePathKey STREQUAL "config/userengine.json")
            math(EXPR userEngineSettingsCount
                "${userEngineSettingsCount} + 1")
        endif()

        string(JSON fileTypeType ERROR_VARIABLE fileTypeTypeError
            TYPE "${fileEntry}" Type)
        if(NOT fileTypeTypeError)
            if(NOT fileTypeType STREQUAL "STRING")
                message(FATAL_ERROR
                    "Game.manifest.json Files[${fileIndex}].Type must be a string")
            endif()
            string(JSON fileType GET "${fileEntry}" Type)
            he_assert_package_policy_value(
                "${fileType}"
                "Game.manifest.json Files[${fileIndex}].Type")
        endif()

        string(JSON dependencyCount ERROR_VARIABLE dependenciesError
            LENGTH "${fileEntry}" Dependencies)
        if(dependenciesError)
            message(FATAL_ERROR
                "Game.manifest.json Files[${fileIndex}].Dependencies must be an array")
        endif()
        if(dependencyCount GREATER 0)
            math(EXPR dependencyLast "${dependencyCount} - 1")
            foreach(dependencyIndex RANGE ${dependencyLast})
                set(dependencyPath "")
                foreach(dependencyField IN ITEMS Path Kind Source)
                    set(dependencyContext
                        "Game.manifest.json Files[${fileIndex}].Dependencies[${dependencyIndex}].${dependencyField}")
                    string(JSON dependencyFieldType
                        ERROR_VARIABLE dependencyFieldTypeError
                        TYPE "${fileEntry}"
                            Dependencies ${dependencyIndex} ${dependencyField})
                    if(NOT dependencyFieldTypeError)
                        if(NOT dependencyFieldType STREQUAL "STRING")
                            message(FATAL_ERROR
                                "${dependencyContext} must be a string")
                        endif()
                        string(JSON dependencyFieldValue GET "${fileEntry}" Dependencies ${dependencyIndex}
                            ${dependencyField})
                        he_assert_package_policy_value(
                            "${dependencyFieldValue}"
                            "${dependencyContext}")
                        if(dependencyField STREQUAL "Path")
                            set(dependencyPath "${dependencyFieldValue}")
                        endif()
                    else()
                        message(FATAL_ERROR
                            "${dependencyContext} is required")
                    endif()
                endforeach()

                string(REPLACE "\\" "/" normalizedDependencyPath
                    "${dependencyPath}")
                if(NOT "${dependencyPath}" STREQUAL "${normalizedDependencyPath}")
                    message(FATAL_ERROR
                        "Game.manifest.json Files[${fileIndex}].Dependencies[${dependencyIndex}].Path "
                        "must use canonical forward slashes: ${dependencyPath}")
                endif()
                string(TOLOWER "${normalizedDependencyPath}"
                    normalizedDependencyPathKey)
                string(SHA256 normalizedDependencyPathHash
                    "${normalizedDependencyPathKey}")
                set(dependencyTargetVariable
                    "manifestFilePath_${normalizedDependencyPathHash}")
                if(NOT DEFINED ${dependencyTargetVariable})
                    message(FATAL_ERROR
                        "Game.manifest.json Files[${fileIndex}].Dependencies[${dependencyIndex}].Path "
                        "references a file that is absent from Files: '${normalizedDependencyPath}' "
                        "(owner: '${normalizedFilePath}')")
                endif()
                if(NOT "${normalizedDependencyPath}" STREQUAL
                   "${${dependencyTargetVariable}}")
                    message(FATAL_ERROR
                        "Game.manifest.json Files[${fileIndex}].Dependencies[${dependencyIndex}].Path "
                        "uses non-canonical casing '${normalizedDependencyPath}'; expected "
                        "'${${dependencyTargetVariable}}' (owner: '${normalizedFilePath}')")
                endif()
            endforeach()
        endif()

        string(JSON tagCount ERROR_VARIABLE tagsError
            LENGTH "${fileEntry}" Tags)
        if(tagsError)
            message(FATAL_ERROR
                "Game.manifest.json Files[${fileIndex}].Tags must be an array")
        endif()
        if(tagCount GREATER 0)
            math(EXPR tagLast "${tagCount} - 1")
            foreach(tagIndex RANGE ${tagLast})
                string(JSON tagType ERROR_VARIABLE tagTypeError
                    TYPE "${fileEntry}" Tags ${tagIndex})
                if(tagTypeError OR NOT tagType STREQUAL "STRING")
                    message(FATAL_ERROR "Game.manifest.json tag must be a string")
                endif()
                string(JSON tagValue GET
                    "${fileEntry}" Tags ${tagIndex})
                he_assert_package_policy_value(
                    "${tagValue}"
                    "Game.manifest.json tag")
            endforeach()
        endif()
    endforeach()
endif()

if(NOT defaultEngineSettingsCount EQUAL 1)
    message(FATAL_ERROR
        "Game.manifest.json must contain exactly one Config/DefaultEngine.json")
endif()
if(NOT projectEngineSettingsCount EQUAL 1)
    message(FATAL_ERROR
        "Game.manifest.json must contain exactly one Config/BaseEngine.json")
endif()
if(NOT userEngineSettingsCount EQUAL 0)
    message(FATAL_ERROR
        "Game.manifest.json must not contain Config/UserEngine.json")
endif()
if(dxilShaderArtifactCount EQUAL 0 OR spirvShaderArtifactCount EQUAL 0)
    message(FATAL_ERROR
        "Game.manifest.json must contain both DXIL and SPIR-V runtime shader artifacts")
endif()
if(dxilMaterialArtifactCount EQUAL 0 OR spirvMaterialArtifactCount EQUAL 0)
    message(FATAL_ERROR
        "Game.manifest.json must contain both DXIL and SPIR-V material artifacts")
endif()

file(GLOB actualAgilityFiles RELATIVE "${PACKAGE_DIR}/D3D12" "${PACKAGE_DIR}/D3D12/*")
set(requiredAgilityFiles D3D12Core.dll)
list(SORT actualAgilityFiles)
if(NOT actualAgilityFiles STREQUAL requiredAgilityFiles)
    message(FATAL_ERROR
        "Package D3D12 directory contains missing or unexpected files.\n"
        "Expected: ${requiredAgilityFiles}\n"
        "Actual:   ${actualAgilityFiles}")
endif()
if(NOT EXISTS "${PACKAGE_DIR}/D3D12/D3D12Core.dll" OR
   IS_DIRECTORY "${PACKAGE_DIR}/D3D12/D3D12Core.dll" OR
   IS_SYMLINK "${PACKAGE_DIR}/D3D12/D3D12Core.dll")
    message(FATAL_ERROR "Packaged Agility runtime is missing or unsafe")
endif()
file(SIZE "${PACKAGE_DIR}/D3D12/D3D12Core.dll" agilityCoreSize)
if(agilityCoreSize EQUAL 0)
    message(FATAL_ERROR "Packaged D3D12Core.dll is empty")
endif()

file(STRINGS "${PACKAGE_LICENSE_LIST_FILE}" requiredLicenseFiles)
if(NOT requiredLicenseFiles)
    message(FATAL_ERROR "The generated package license allow-list is empty")
endif()

file(GLOB actualLicenseFiles RELATIVE "${PACKAGE_DIR}/Licenses" "${PACKAGE_DIR}/Licenses/*")
list(SORT actualLicenseFiles)
list(SORT requiredLicenseFiles)
if(NOT actualLicenseFiles STREQUAL requiredLicenseFiles)
    message(FATAL_ERROR
        "Package license set is incomplete or contains stale files.\n"
        "Expected: ${requiredLicenseFiles}\n"
        "Actual:   ${actualLicenseFiles}")
endif()

foreach(fileName IN LISTS requiredLicenseFiles)
    set(licensePath "${PACKAGE_DIR}/Licenses/${fileName}")
    if(NOT EXISTS "${licensePath}" OR IS_DIRECTORY "${licensePath}" OR IS_SYMLINK "${licensePath}")
        message(FATAL_ERROR "Required package license is missing or unsafe: ${fileName}")
    endif()
    file(SIZE "${licensePath}" licenseSize)
    if(licenseSize EQUAL 0)
        message(FATAL_ERROR "Required package license is empty: ${fileName}")
    endif()
endforeach()

message(STATUS "Verified standalone package allow-list: ${PACKAGE_DIR}")
