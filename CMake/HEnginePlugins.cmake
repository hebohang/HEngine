include_guard(GLOBAL)

include(CMakeParseArguments)

set(HE_STATIC_PLUGIN_SCHEMA_VERSION 1)
set(HE_STATIC_PLUGIN_API_VERSION 1)
set(HE_STATIC_PLUGIN_HOST_API_VERSION 1)
set(HE_STATIC_PLUGIN_KNOWN_HOSTS
    Editor
    DevGame
    Game
    Tests
    Tools
    RenderSmoke)
set(HE_STATIC_PLUGIN_KNOWN_LOAD_PHASES
    PreDevice
    PostDevice
    PostEngine
    OnDemand)
set(HE_STATIC_PLUGIN_KNOWN_DISTRIBUTIONS
    Runtime
    EditorOnly
    DeveloperOnly)
set(HE_STATIC_PLUGIN_KNOWN_PACKAGE_POLICIES
    Always
    ProjectSelectable
    Never)
set(HE_STATIC_PLUGIN_KNOWN_PLATFORMS Windows Web WeChat Android macOS)
set(HE_STATIC_PLUGIN_KNOWN_CONFIGURATIONS
    Debug
    Release
    RelWithDebInfo
    MinSizeRel)
set(HE_STATIC_PLUGIN_KNOWN_BUILD_DISTRIBUTIONS
    Development
    Shipping)

function(_he_plugin_fail messageText)
    message(FATAL_ERROR "[HEnginePlugins] ${messageText}")
endfunction()

function(_he_plugin_require_no_unparsed prefix context)
    if(${prefix}_UNPARSED_ARGUMENTS)
        _he_plugin_fail("${context}: unknown arguments: ${${prefix}_UNPARSED_ARGUMENTS}")
    endif()
endfunction()

function(_he_plugin_validate_choice value context)
    set(allowedValues ${ARGN})
    list(FIND allowedValues "${value}" choiceIndex)
    if(choiceIndex EQUAL -1)
        string(JOIN ", " allowedText ${allowedValues})
        _he_plugin_fail("${context}: '${value}' is invalid; expected one of: ${allowedText}")
    endif()
endfunction()

function(_he_plugin_validate_unique_list context)
    set(values ${ARGN})
    set(seenValues "")
    foreach(value IN LISTS values)
        list(FIND seenValues "${value}" seenIndex)
        if(NOT seenIndex EQUAL -1)
            _he_plugin_fail("${context}: duplicate value '${value}'")
        endif()
        list(APPEND seenValues "${value}")
    endforeach()
endfunction()

function(_he_plugin_json_require outputVariable jsonText context memberName expectedType)
    string(JSON actualType ERROR_VARIABLE jsonError TYPE "${jsonText}" "${memberName}")
    if(NOT jsonError STREQUAL "NOTFOUND")
        _he_plugin_fail("${context}: missing required '${memberName}' member")
    endif()
    if(NOT actualType STREQUAL expectedType)
        _he_plugin_fail(
            "${context}: '${memberName}' must be ${expectedType}, got ${actualType}")
    endif()
    string(JSON memberValue GET "${jsonText}" "${memberName}")
    set(${outputVariable} "${memberValue}" PARENT_SCOPE)
endfunction()

function(_he_plugin_json_validate_members jsonText context)
    set(allowedMembers ${ARGN})
    string(JSON memberCount ERROR_VARIABLE jsonError LENGTH "${jsonText}")
    if(NOT jsonError STREQUAL "NOTFOUND")
        _he_plugin_fail("${context}: expected a JSON object")
    endif()
    if(memberCount GREATER 0)
        math(EXPR lastMemberIndex "${memberCount} - 1")
        foreach(memberIndex RANGE 0 ${lastMemberIndex})
            string(JSON memberName MEMBER "${jsonText}" ${memberIndex})
            list(FIND allowedMembers "${memberName}" allowedIndex)
            if(allowedIndex EQUAL -1)
                _he_plugin_fail("${context}: unknown member '${memberName}'")
            endif()
        endforeach()
    endif()
endfunction()

function(_he_plugin_json_string_array outputVariable jsonText context memberName)
    _he_plugin_json_require(arrayValue "${jsonText}" "${context}" "${memberName}" ARRAY)
    string(JSON arrayLength LENGTH "${jsonText}" "${memberName}")
    set(values "")
    if(arrayLength GREATER 0)
        math(EXPR lastArrayIndex "${arrayLength} - 1")
        foreach(arrayIndex RANGE 0 ${lastArrayIndex})
            string(JSON elementType TYPE "${jsonText}" "${memberName}" ${arrayIndex})
            if(NOT elementType STREQUAL "STRING")
                _he_plugin_fail(
                    "${context}: '${memberName}[${arrayIndex}]' must be STRING")
            endif()
            string(JSON elementValue GET "${jsonText}" "${memberName}" ${arrayIndex})
            if(elementValue STREQUAL "")
                _he_plugin_fail(
                    "${context}: '${memberName}[${arrayIndex}]' must not be empty")
            endif()
            list(APPEND values "${elementValue}")
        endforeach()
    endif()
    _he_plugin_validate_unique_list("${context}: '${memberName}'" ${values})
    set(${outputVariable} "${values}" PARENT_SCOPE)
endfunction()

function(_he_plugin_json_dependency_array
    outputIds outputVersionRanges outputMinimumApiVersions
    jsonText context memberName)
    _he_plugin_json_require(arrayValue "${jsonText}" "${context}" "${memberName}" ARRAY)
    string(JSON arrayLength LENGTH "${jsonText}" "${memberName}")
    set(dependencyIds "")
    set(dependencyVersionRanges "")
    set(dependencyMinimumApiVersions "")
    if(arrayLength GREATER 0)
        math(EXPR lastArrayIndex "${arrayLength} - 1")
        foreach(arrayIndex RANGE 0 ${lastArrayIndex})
            string(JSON elementType TYPE "${jsonText}" "${memberName}" ${arrayIndex})
            if(NOT elementType STREQUAL "OBJECT")
                _he_plugin_fail(
                    "${context}: '${memberName}[${arrayIndex}]' must be OBJECT")
            endif()
            string(JSON dependencyJson GET "${jsonText}" "${memberName}" ${arrayIndex})
            _he_plugin_json_validate_members(
                "${dependencyJson}" "${context}: ${memberName}[${arrayIndex}]"
                Id VersionRange MinimumApiVersion)
            _he_plugin_json_require(
                dependencyId "${dependencyJson}"
                "${context}: ${memberName}[${arrayIndex}]" Id STRING)
            _he_plugin_json_require(
                dependencyVersionRange "${dependencyJson}"
                "${context}: ${memberName}[${arrayIndex}]" VersionRange STRING)
            _he_plugin_json_require(
                dependencyMinimumApiVersion "${dependencyJson}"
                "${context}: ${memberName}[${arrayIndex}]" MinimumApiVersion NUMBER)
            if(NOT dependencyId MATCHES "^[a-z][a-z0-9]*(\\.[a-z][a-z0-9-]*)+$")
                _he_plugin_fail(
                    "${context}: '${memberName}[${arrayIndex}].Id' is invalid")
            endif()
            if(NOT dependencyVersionRange MATCHES "^(\\^|>=|=)?[0-9]+\\.[0-9]+\\.[0-9]+$")
                _he_plugin_fail(
                    "${context}: '${memberName}[${arrayIndex}].VersionRange' must be exact, =, >= or ^ major.minor.patch")
            endif()
            if(NOT dependencyMinimumApiVersion MATCHES "^[0-9]+$" OR
               dependencyMinimumApiVersion LESS 1)
                _he_plugin_fail(
                    "${context}: '${memberName}[${arrayIndex}].MinimumApiVersion' must be a positive integer")
            endif()
            list(APPEND dependencyIds "${dependencyId}")
            list(APPEND dependencyVersionRanges "${dependencyVersionRange}")
            list(APPEND dependencyMinimumApiVersions "${dependencyMinimumApiVersion}")
        endforeach()
    endif()
    _he_plugin_validate_unique_list(
        "${context}: '${memberName}' dependency Ids" ${dependencyIds})
    set(${outputIds} "${dependencyIds}" PARENT_SCOPE)
    set(${outputVersionRanges} "${dependencyVersionRanges}" PARENT_SCOPE)
    set(${outputMinimumApiVersions} "${dependencyMinimumApiVersions}" PARENT_SCOPE)
endfunction()

function(_he_plugin_version_satisfies outputVariable actualVersion versionRange)
    if(versionRange MATCHES "^\\^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
        set(requiredMajor "${CMAKE_MATCH_1}")
        set(requiredMinor "${CMAKE_MATCH_2}")
        set(requiredPatch "${CMAKE_MATCH_3}")
        set(requiredVersion "${requiredMajor}.${requiredMinor}.${requiredPatch}")
        string(REPLACE "." ";" actualParts "${actualVersion}")
        list(GET actualParts 0 actualMajor)
        list(GET actualParts 1 actualMinor)
        if(requiredMajor GREATER 0)
            set(compatibleSeries FALSE)
            if(actualMajor EQUAL requiredMajor)
                set(compatibleSeries TRUE)
            endif()
        else()
            set(compatibleSeries FALSE)
            if(actualMajor EQUAL 0 AND actualMinor EQUAL requiredMinor)
                set(compatibleSeries TRUE)
            endif()
        endif()
        if(compatibleSeries AND NOT "${actualVersion}" VERSION_LESS "${requiredVersion}")
            set(matches TRUE)
        else()
            set(matches FALSE)
        endif()
    elseif(versionRange MATCHES "^>=([0-9]+\\.[0-9]+\\.[0-9]+)$")
        set(requiredVersion "${CMAKE_MATCH_1}")
        if("${actualVersion}" VERSION_LESS "${requiredVersion}")
            set(matches FALSE)
        else()
            set(matches TRUE)
        endif()
    elseif(versionRange MATCHES "^=([0-9]+\\.[0-9]+\\.[0-9]+)$")
        if("${actualVersion}" VERSION_EQUAL "${CMAKE_MATCH_1}")
            set(matches TRUE)
        else()
            set(matches FALSE)
        endif()
    elseif("${actualVersion}" VERSION_EQUAL "${versionRange}")
        set(matches TRUE)
    else()
        set(matches FALSE)
    endif()
    set(${outputVariable} "${matches}" PARENT_SCOPE)
endfunction()

function(_he_plugin_key outputVariable pluginId)
    string(SHA256 pluginHash "${pluginId}")
    string(SUBSTRING "${pluginHash}" 0 16 shortHash)
    set(${outputVariable} "P_${shortHash}" PARENT_SCOPE)
endfunction()

function(_he_plugin_module_key outputVariable moduleName)
    string(SHA256 moduleHash "${moduleName}")
    string(SUBSTRING "${moduleHash}" 0 16 shortHash)
    set(${outputVariable} "M_${shortHash}" PARENT_SCOPE)
endfunction()

function(_he_plugin_set pluginId fieldName)
    _he_plugin_key(pluginKey "${pluginId}")
    set_property(GLOBAL PROPERTY
        "HE_STATIC_PLUGIN_${pluginKey}_${fieldName}" "${ARGN}")
endfunction()

function(_he_plugin_get outputVariable pluginId fieldName)
    _he_plugin_key(pluginKey "${pluginId}")
    get_property(propertyValue GLOBAL PROPERTY
        "HE_STATIC_PLUGIN_${pluginKey}_${fieldName}")
    set(${outputVariable} "${propertyValue}" PARENT_SCOPE)
endfunction()

function(_he_plugin_id_for_module outputVariable moduleName)
    _he_plugin_module_key(moduleKey "${moduleName}")
    get_property(pluginId GLOBAL PROPERTY
        "HE_STATIC_PLUGIN_MODULE_${moduleKey}_ID")
    set(${outputVariable} "${pluginId}" PARENT_SCOPE)
endfunction()

function(_he_plugin_set_host_enabled pluginId hostName enabled)
    _he_plugin_set("${pluginId}" "HOST_${hostName}_ENABLED" "${enabled}")
endfunction()

function(_he_plugin_get_host_enabled outputVariable pluginId hostName)
    _he_plugin_get(enabled "${pluginId}" "HOST_${hostName}_ENABLED")
    if(enabled)
        set(${outputVariable} TRUE PARENT_SCOPE)
    else()
        set(${outputVariable} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(_he_plugin_validate_relative_paths pluginId manifestDirectory fieldName)
    set(relativePaths ${ARGN})
    foreach(relativePath IN LISTS relativePaths)
        if(IS_ABSOLUTE "${relativePath}" OR
           relativePath MATCHES "(^|/)\\.\\.(/|$)" OR
           relativePath MATCHES "\\\\" OR
           relativePath MATCHES "[\n\r;]")
            _he_plugin_fail(
                "plugin '${pluginId}' ${fieldName} path must be a safe plugin-relative path: '${relativePath}'")
        endif()
        if(NOT EXISTS "${manifestDirectory}/${relativePath}")
            _he_plugin_fail(
                "plugin '${pluginId}' ${fieldName} path does not exist: '${relativePath}'")
        endif()
    endforeach()
endfunction()

function(_he_plugin_phase_rank outputVariable phase)
    list(FIND HE_STATIC_PLUGIN_KNOWN_LOAD_PHASES "${phase}" phaseRank)
    if(phaseRank EQUAL -1)
        _he_plugin_fail("internal error: unknown load phase '${phase}'")
    endif()
    set(${outputVariable} "${phaseRank}" PARENT_SCOPE)
endfunction()

function(_he_plugin_escape_cpp outputVariable value)
    set(escaped "${value}")
    string(REPLACE "\\" "\\\\" escaped "${escaped}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    string(REPLACE "\n" "\\n" escaped "${escaped}")
    string(REPLACE "\r" "\\r" escaped "${escaped}")
    set(${outputVariable} "${escaped}" PARENT_SCOPE)
endfunction()

function(_he_plugin_cpp_string_list outputVariable)
    set(rendered "")
    foreach(value IN LISTS ARGN)
        _he_plugin_escape_cpp(escapedValue "${value}")
        if(rendered STREQUAL "")
            set(rendered "\"${escapedValue}\"")
        else()
            string(APPEND rendered ", \"${escapedValue}\"")
        endif()
    endforeach()
    set(${outputVariable} "${rendered}" PARENT_SCOPE)
endfunction()

function(_he_plugin_cpp_enum_list outputVariable enumType)
    set(values ${ARGN})
    set(rendered "")
    foreach(value IN LISTS values)
        if(rendered STREQUAL "")
            set(rendered "${enumType}::${value}")
        else()
            string(APPEND rendered " | ${enumType}::${value}")
        endif()
    endforeach()
    if(rendered STREQUAL "")
        set(rendered "${enumType}::None")
    endif()
    set(${outputVariable} "${rendered}" PARENT_SCOPE)
endfunction()

function(_he_plugin_cpp_version_range outputVariable versionRange)
    set(maximumCpp "std::nullopt")
    if(versionRange MATCHES "^\\^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
        set(minimumMajor "${CMAKE_MATCH_1}")
        set(minimumMinor "${CMAKE_MATCH_2}")
        set(minimumPatch "${CMAKE_MATCH_3}")
        if(minimumMajor GREATER 0)
            math(EXPR maximumMajor "${minimumMajor} + 1")
            set(maximumMinor 0)
            set(maximumPatch 0)
        elseif(minimumMinor GREATER 0)
            set(maximumMajor 0)
            math(EXPR maximumMinor "${minimumMinor} + 1")
            set(maximumPatch 0)
        else()
            set(maximumMajor 0)
            set(maximumMinor 0)
            math(EXPR maximumPatch "${minimumPatch} + 1")
        endif()
        set(maximumCpp
            "PluginVersion{ ${maximumMajor}u, ${maximumMinor}u, ${maximumPatch}u }")
    elseif(versionRange MATCHES "^>=([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
        set(minimumMajor "${CMAKE_MATCH_1}")
        set(minimumMinor "${CMAKE_MATCH_2}")
        set(minimumPatch "${CMAKE_MATCH_3}")
    elseif(versionRange MATCHES "^=?([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
        set(minimumMajor "${CMAKE_MATCH_1}")
        set(minimumMinor "${CMAKE_MATCH_2}")
        set(minimumPatch "${CMAKE_MATCH_3}")
        set(maximumMajor "${minimumMajor}")
        set(maximumMinor "${minimumMinor}")
        math(EXPR maximumPatch "${minimumPatch} + 1")
        set(maximumCpp
            "PluginVersion{ ${maximumMajor}u, ${maximumMinor}u, ${maximumPatch}u }")
    else()
        _he_plugin_fail("internal error: unsupported VersionRange '${versionRange}'")
    endif()
    set(rendered
        "PluginVersionRange{ .MinimumInclusive = PluginVersion{ ${minimumMajor}u, ${minimumMinor}u, ${minimumPatch}u }, .MaximumExclusive = ${maximumCpp} }")
    set(${outputVariable} "${rendered}" PARENT_SCOPE)
endfunction()

function(_he_plugin_cpp_dependency_list
    outputVariable dependencyIds dependencyVersionRanges dependencyMinimumApiVersions)
    set(ids ${dependencyIds})
    set(versionRanges ${dependencyVersionRanges})
    set(minimumApiVersions ${dependencyMinimumApiVersions})
    list(LENGTH ids idCount)
    list(LENGTH versionRanges versionRangeCount)
    list(LENGTH minimumApiVersions minimumApiVersionCount)
    if(NOT idCount EQUAL versionRangeCount OR
       NOT idCount EQUAL minimumApiVersionCount)
        _he_plugin_fail("internal error: dependency metadata list length mismatch")
    endif()
    set(rendered "")
    if(idCount GREATER 0)
        math(EXPR lastDependencyIndex "${idCount} - 1")
        foreach(dependencyIndex RANGE 0 ${lastDependencyIndex})
            list(GET ids ${dependencyIndex} dependencyId)
            list(GET versionRanges ${dependencyIndex} dependencyVersionRange)
            list(GET minimumApiVersions ${dependencyIndex} dependencyMinimumApiVersion)
            _he_plugin_escape_cpp(dependencyIdCpp "${dependencyId}")
            _he_plugin_cpp_version_range(versionRangeCpp "${dependencyVersionRange}")
            set(dependencyCpp
                "PluginDependency{ .Id = \"${dependencyIdCpp}\", .VersionRange = ${versionRangeCpp}, .MinimumApiVersion = ${dependencyMinimumApiVersion}u }")
            if(rendered STREQUAL "")
                set(rendered "${dependencyCpp}")
            else()
                string(APPEND rendered ", ${dependencyCpp}")
            endif()
        endforeach()
    endif()
    set(${outputVariable} "${rendered}" PARENT_SCOPE)
endfunction()

# Keep the C++ ABI adaptation for generated registrations in this one function.
# The plugin translation unit supplies only Scope<IPluginModule> EntrySymbol().
function(_he_plugin_render_registration_cpp outputVariable pluginId)
    foreach(field IN ITEMS
        DISPLAY_NAME VERSION_MAJOR VERSION_MINOR VERSION_PATCH API_VERSION
        REQUIRED_HOST_API_VERSION MODULE_NAME ENTRY_SYMBOL DEPENDENCIES
        DEPENDENCY_VERSION_RANGES DEPENDENCY_MINIMUM_API_VERSIONS
        OPTIONAL_DEPENDENCIES OPTIONAL_DEPENDENCY_VERSION_RANGES
        OPTIONAL_DEPENDENCY_MINIMUM_API_VERSIONS LOAD_PHASE SUPPORTED_HOSTS
        DISTRIBUTION PACKAGE_POLICY
        SHADER_ROOTS CONTENT_ROOTS PLATFORMS CONFIGURATIONS)
        _he_plugin_get("plugin${field}" "${pluginId}" "${field}")
    endforeach()

    _he_plugin_escape_cpp(idCpp "${pluginId}")
    _he_plugin_escape_cpp(displayNameCpp "${pluginDISPLAY_NAME}")
    _he_plugin_escape_cpp(moduleCpp "${pluginMODULE_NAME}")
    _he_plugin_escape_cpp(entryCpp "${pluginENTRY_SYMBOL}")
    _he_plugin_cpp_dependency_list(
        dependenciesCpp "${pluginDEPENDENCIES}"
        "${pluginDEPENDENCY_VERSION_RANGES}"
        "${pluginDEPENDENCY_MINIMUM_API_VERSIONS}")
    _he_plugin_cpp_dependency_list(
        optionalDependenciesCpp
        "${pluginOPTIONAL_DEPENDENCIES}"
        "${pluginOPTIONAL_DEPENDENCY_VERSION_RANGES}"
        "${pluginOPTIONAL_DEPENDENCY_MINIMUM_API_VERSIONS}")
    _he_plugin_cpp_string_list(shaderRootsCpp ${pluginSHADER_ROOTS})
    _he_plugin_cpp_string_list(contentRootsCpp ${pluginCONTENT_ROOTS})
    _he_plugin_cpp_enum_list(hostsCpp PluginHost ${pluginSUPPORTED_HOSTS})
    _he_plugin_cpp_enum_list(platformsCpp PluginPlatform ${pluginPLATFORMS})
    _he_plugin_cpp_enum_list(configurationsCpp PluginConfiguration ${pluginCONFIGURATIONS})

    string(MAKE_C_IDENTIFIER "${pluginId}" pluginIdentifier)
    set(rendered
"    PluginDescriptor Make_${pluginIdentifier}_Descriptor()
    {
        return PluginDescriptor{
            .Id = \"${idCpp}\",
            .DisplayName = \"${displayNameCpp}\",
            .ModuleName = \"${moduleCpp}\",
            .EntrySymbol = \"${entryCpp}\",
            .Version = PluginVersion{ ${pluginVERSION_MAJOR}u, ${pluginVERSION_MINOR}u, ${pluginVERSION_PATCH}u },
            .ApiVersion = ${pluginAPI_VERSION}u,
            .RequiredHostApiVersion = ${pluginREQUIRED_HOST_API_VERSION}u,
            .SupportedHosts = ${hostsCpp},
            .Distribution = PluginDistribution::${pluginDISTRIBUTION},
            .LoadPhase = PluginLoadPhase::${pluginLOAD_PHASE},
            .PackagePolicy = PluginPackagePolicy::${pluginPACKAGE_POLICY},
            .Platforms = ${platformsCpp},
            .Configurations = ${configurationsCpp},
            .ShaderRoots = { ${shaderRootsCpp} },
            .ContentRoots = { ${contentRootsCpp} },
            .Dependencies = { ${dependenciesCpp} },
            .OptionalDependencies = { ${optionalDependenciesCpp} },
        };
    }
")
    set(${outputVariable} "${rendered}" PARENT_SCOPE)
endfunction()

function(_he_plugin_write_if_different outputPath content)
    set(writeFile TRUE)
    if(EXISTS "${outputPath}")
        file(READ "${outputPath}" existingContent)
        if(existingContent STREQUAL content)
            set(writeFile FALSE)
        endif()
    endif()
    if(writeFile)
        file(WRITE "${outputPath}" "${content}")
    endif()
endfunction()

function(_he_plugin_generate_host_anchor hostName generatedDirectory orderedPluginIds)
    set(hostPluginIds "")
    foreach(pluginId IN LISTS orderedPluginIds)
        _he_plugin_get_host_enabled(hostEnabled "${pluginId}" "${hostName}")
        if(hostEnabled)
            list(APPEND hostPluginIds "${pluginId}")
        endif()
    endforeach()

    set(anchorDeclarations "")
    set(descriptorBuilders "")
    set(registrationCalls "")
    foreach(pluginId IN LISTS hostPluginIds)
        _he_plugin_get(factoryAnchor "${pluginId}" ENTRY_SYMBOL)
        string(APPEND anchorDeclarations
            "Scope<IPluginModule> ${factoryAnchor}();\n")
        _he_plugin_render_registration_cpp(registrationCpp "${pluginId}")
        string(APPEND descriptorBuilders "${registrationCpp}\n")
        string(MAKE_C_IDENTIFIER "${pluginId}" pluginIdentifier)
        string(APPEND registrationCalls
            "    pluginManager.Register(StaticPluginRegistration{\n"
            "        .Descriptor = Make_${pluginIdentifier}_Descriptor(),\n"
            "        .Factory = &${factoryAnchor},\n"
            "    });\n")
    endforeach()

    set(anchorContent
"// Generated by CMake/HEnginePlugins.cmake. Do not edit.
#include \"Runtime/Core/Plugins/PluginManager.h\"

namespace HEngine
{
${anchorDeclarations}
namespace
{
${descriptorBuilders}}

void RegisterStaticPluginsFor${hostName}(PluginManager& pluginManager)
{
${registrationCalls}}
}
")

    set(anchorPath "${generatedDirectory}/HEngineStaticPlugins.${hostName}.cpp")
    _he_plugin_write_if_different("${anchorPath}" "${anchorContent}")
    set_property(GLOBAL PROPERTY "HE_STATIC_PLUGIN_ANCHOR_${hostName}" "${anchorPath}")
    set_property(GLOBAL PROPERTY "HE_STATIC_PLUGIN_HOST_${hostName}_IDS" "${hostPluginIds}")
endfunction()

function(he_discover_static_plugins)
    set(options "")
    set(oneValueArguments
        ROOT CONFIG GENERATED_DIR PLATFORM CONFIGURATION BUILD_DISTRIBUTION)
    set(multiValueArguments ACTIVE_HOSTS)
    cmake_parse_arguments(HEP
        "${options}" "${oneValueArguments}" "${multiValueArguments}" ${ARGN})
    _he_plugin_require_no_unparsed(HEP "he_discover_static_plugins")

    if(NOT HEP_ROOT OR NOT IS_DIRECTORY "${HEP_ROOT}")
        _he_plugin_fail("he_discover_static_plugins: ROOT must be an existing directory")
    endif()
    if(NOT HEP_CONFIG OR NOT EXISTS "${HEP_CONFIG}")
        _he_plugin_fail("he_discover_static_plugins: CONFIG must be an existing file")
    endif()
    if(NOT HEP_GENERATED_DIR)
        set(HEP_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated/plugins")
    endif()
    get_filename_component(pluginRoot "${HEP_ROOT}" ABSOLUTE)
    get_filename_component(pluginConfig "${HEP_CONFIG}" ABSOLUTE)
    get_filename_component(generatedDirectory "${HEP_GENERATED_DIR}" ABSOLUTE)
    file(MAKE_DIRECTORY "${generatedDirectory}")

    if(NOT HEP_ACTIVE_HOSTS)
        _he_plugin_fail("he_discover_static_plugins: ACTIVE_HOSTS must not be empty")
    endif()
    _he_plugin_validate_unique_list("he_discover_static_plugins ACTIVE_HOSTS" ${HEP_ACTIVE_HOSTS})
    foreach(hostName IN LISTS HEP_ACTIVE_HOSTS)
        _he_plugin_validate_choice("${hostName}" "active plugin host"
            ${HE_STATIC_PLUGIN_KNOWN_HOSTS})
    endforeach()
    if(NOT HEP_BUILD_DISTRIBUTION)
        set(HEP_BUILD_DISTRIBUTION Development)
    endif()
    _he_plugin_validate_choice(
        "${HEP_BUILD_DISTRIBUTION}" "plugin build distribution"
        ${HE_STATIC_PLUGIN_KNOWN_BUILD_DISTRIBUTIONS})
    if(HEP_BUILD_DISTRIBUTION STREQUAL "Shipping")
        set(nonGameHosts ${HEP_ACTIVE_HOSTS})
        list(REMOVE_ITEM nonGameHosts Game)
        list(FIND HEP_ACTIVE_HOSTS Game gameHostIndex)
        if(gameHostIndex EQUAL -1 OR nonGameHosts)
            _he_plugin_fail(
                "Shipping discovery is fail-closed and requires ACTIVE_HOSTS to contain only Game")
        endif()
    endif()

    if(NOT HEP_PLATFORM)
        if(WIN32)
            set(HEP_PLATFORM Windows)
        else()
            _he_plugin_fail(
                "he_discover_static_plugins: PLATFORM is required on unsupported hosts")
        endif()
    endif()
    _he_plugin_validate_choice("${HEP_PLATFORM}" "active plugin platform"
        ${HE_STATIC_PLUGIN_KNOWN_PLATFORMS})

    if(NOT HEP_CONFIGURATION)
        if(CMAKE_BUILD_TYPE)
            set(HEP_CONFIGURATION "${CMAKE_BUILD_TYPE}")
        elseif(CMAKE_CONFIGURATION_TYPES)
            set(HEP_CONFIGURATION "$<CONFIG>")
        else()
            _he_plugin_fail(
                "he_discover_static_plugins: CONFIGURATION is required")
        endif()
    endif()
    if(HEP_CONFIGURATION STREQUAL "$<CONFIG>")
        if(NOT CMAKE_CONFIGURATION_TYPES)
            _he_plugin_fail("CONFIGURATION $<CONFIG> requires a multi-config generator")
        endif()
        set(configurationsToValidate ${CMAKE_CONFIGURATION_TYPES})
    else()
        set(configurationsToValidate "${HEP_CONFIGURATION}")
    endif()
    foreach(configuration IN LISTS configurationsToValidate)
        _he_plugin_validate_choice("${configuration}" "active plugin configuration"
            ${HE_STATIC_PLUGIN_KNOWN_CONFIGURATIONS})
    endforeach()

    set_property(GLOBAL PROPERTY HE_STATIC_PLUGIN_DISCOVERED FALSE)
    set_property(GLOBAL PROPERTY HE_STATIC_PLUGIN_DIRECTORIES_ADDED FALSE)
    set_property(GLOBAL PROPERTY HE_STATIC_PLUGIN_IDS "")
    set_property(GLOBAL PROPERTY HE_STATIC_PLUGIN_MODULES "")
    set_property(GLOBAL PROPERTY HE_STATIC_PLUGIN_ENABLED_IDS "")
    set_property(GLOBAL PROPERTY HE_STATIC_PLUGIN_HEADER_FILES "")
    set_property(GLOBAL PROPERTY HE_STATIC_PLUGIN_SOURCE_FILES "")

    if(CMAKE_SCRIPT_MODE_FILE)
        file(GLOB pluginChildren LIST_DIRECTORIES TRUE "${pluginRoot}/*")
        file(GLOB manifestFiles "${pluginRoot}/*/plugin.json")
    else()
        file(GLOB pluginChildren CONFIGURE_DEPENDS LIST_DIRECTORIES TRUE "${pluginRoot}/*")
        file(GLOB manifestFiles CONFIGURE_DEPENDS "${pluginRoot}/*/plugin.json")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pluginConfig}")
    endif()
    list(SORT pluginChildren)
    list(SORT manifestFiles)

    foreach(pluginChild IN LISTS pluginChildren)
        if(IS_DIRECTORY "${pluginChild}" AND EXISTS "${pluginChild}/CMakeLists.txt" AND
           NOT EXISTS "${pluginChild}/plugin.json")
            _he_plugin_fail(
                "plugin directory has CMakeLists.txt but no single-source manifest: ${pluginChild}")
        endif()
    endforeach()
    if(NOT manifestFiles)
        _he_plugin_fail("no plugin.json manifests found beneath ${pluginRoot}")
    endif()

    set(allPluginIds "")
    set(allPluginModules "")
    set(allFactoryAnchors "")
    foreach(manifestPath IN LISTS manifestFiles)
        get_filename_component(manifestDirectory "${manifestPath}" DIRECTORY)
        if(NOT EXISTS "${manifestDirectory}/CMakeLists.txt")
            _he_plugin_fail("manifest has no sibling CMakeLists.txt: ${manifestPath}")
        endif()
        file(READ "${manifestPath}" manifestJson)
        set(manifestContext "manifest '${manifestPath}'")
        _he_plugin_json_validate_members("${manifestJson}" "${manifestContext}"
            SchemaVersion Id DisplayName Version ApiVersion RequiredHostApiVersion
            ModuleName EntrySymbol Dependencies OptionalDependencies LoadPhase
            SupportedHosts Distribution PackagePolicy ShaderRoots ContentRoots
            Platforms Configurations)

        _he_plugin_json_require(schemaVersion "${manifestJson}" "${manifestContext}" SchemaVersion NUMBER)
        _he_plugin_json_require(pluginId "${manifestJson}" "${manifestContext}" Id STRING)
        _he_plugin_json_require(displayName "${manifestJson}" "${manifestContext}" DisplayName STRING)
        _he_plugin_json_require(version "${manifestJson}" "${manifestContext}" Version STRING)
        _he_plugin_json_require(apiVersion "${manifestJson}" "${manifestContext}" ApiVersion NUMBER)
        _he_plugin_json_require(
            requiredHostApiVersion "${manifestJson}" "${manifestContext}"
            RequiredHostApiVersion NUMBER)
        _he_plugin_json_require(moduleName "${manifestJson}" "${manifestContext}" ModuleName STRING)
        _he_plugin_json_require(entrySymbol "${manifestJson}" "${manifestContext}" EntrySymbol STRING)
        _he_plugin_json_require(loadPhase "${manifestJson}" "${manifestContext}" LoadPhase STRING)
        _he_plugin_json_require(distribution "${manifestJson}" "${manifestContext}" Distribution STRING)
        _he_plugin_json_require(packagePolicy "${manifestJson}" "${manifestContext}" PackagePolicy STRING)
        _he_plugin_json_dependency_array(
            dependencies dependencyVersionRanges dependencyMinimumApiVersions
            "${manifestJson}" "${manifestContext}" Dependencies)
        _he_plugin_json_dependency_array(
            optionalDependencies optionalDependencyVersionRanges
            optionalDependencyMinimumApiVersions
            "${manifestJson}" "${manifestContext}" OptionalDependencies)
        _he_plugin_json_string_array(
            supportedHosts "${manifestJson}" "${manifestContext}" SupportedHosts)
        _he_plugin_json_string_array(
            shaderRoots "${manifestJson}" "${manifestContext}" ShaderRoots)
        _he_plugin_json_string_array(
            contentRoots "${manifestJson}" "${manifestContext}" ContentRoots)
        _he_plugin_json_string_array(platforms "${manifestJson}" "${manifestContext}" Platforms)
        _he_plugin_json_string_array(configurations "${manifestJson}" "${manifestContext}" Configurations)

        if(NOT schemaVersion MATCHES "^[0-9]+$" OR
           NOT schemaVersion EQUAL HE_STATIC_PLUGIN_SCHEMA_VERSION)
            _he_plugin_fail(
                "${manifestContext}: SchemaVersion ${schemaVersion} is unsupported; expected ${HE_STATIC_PLUGIN_SCHEMA_VERSION}")
        endif()
        if(NOT pluginId MATCHES "^[a-z][a-z0-9]*(\\.[a-z][a-z0-9-]*)+$")
            _he_plugin_fail(
                "${manifestContext}: Id must be a lowercase reverse-domain identifier")
        endif()
        string(LENGTH "${displayName}" displayNameLength)
        if(displayNameLength EQUAL 0 OR displayNameLength GREATER 128 OR
           displayName MATCHES "[\n\r;]")
            _he_plugin_fail("${manifestContext}: DisplayName is invalid")
        endif()
        if(NOT version MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
            _he_plugin_fail(
                "${manifestContext}: Version must use strict major.minor.patch syntax")
        endif()
        set(versionMajor "${CMAKE_MATCH_1}")
        set(versionMinor "${CMAKE_MATCH_2}")
        set(versionPatch "${CMAKE_MATCH_3}")
        if(versionMajor EQUAL 0)
            _he_plugin_fail(
                "${manifestContext}: Version major must be non-zero")
        endif()
        if(NOT apiVersion MATCHES "^[0-9]+$" OR
           NOT apiVersion EQUAL HE_STATIC_PLUGIN_API_VERSION)
            _he_plugin_fail(
                "${manifestContext}: ApiVersion ${apiVersion} is incompatible; expected ${HE_STATIC_PLUGIN_API_VERSION}")
        endif()
        if(NOT requiredHostApiVersion MATCHES "^[0-9]+$" OR
           requiredHostApiVersion LESS 1 OR
           requiredHostApiVersion GREATER HE_STATIC_PLUGIN_HOST_API_VERSION)
            _he_plugin_fail(
                "${manifestContext}: RequiredHostApiVersion ${requiredHostApiVersion} is incompatible; host exports ${HE_STATIC_PLUGIN_HOST_API_VERSION}")
        endif()
        if(NOT moduleName MATCHES "^[A-Za-z][A-Za-z0-9_]*$")
            _he_plugin_fail("${manifestContext}: Module is not a safe CMake target name")
        endif()
        get_filename_component(pluginDirectoryName "${manifestDirectory}" NAME)
        if(NOT moduleName STREQUAL pluginDirectoryName)
            _he_plugin_fail(
                "${manifestContext}: Module '${moduleName}' must match directory '${pluginDirectoryName}'")
        endif()
        if(NOT entrySymbol MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
            _he_plugin_fail("${manifestContext}: EntrySymbol is not a C++ identifier")
        endif()

        _he_plugin_validate_choice("${loadPhase}" "${manifestContext}: LoadPhase"
            ${HE_STATIC_PLUGIN_KNOWN_LOAD_PHASES})
        _he_plugin_validate_choice("${distribution}" "${manifestContext}: Distribution"
            ${HE_STATIC_PLUGIN_KNOWN_DISTRIBUTIONS})
        _he_plugin_validate_choice("${packagePolicy}" "${manifestContext}: PackagePolicy"
            ${HE_STATIC_PLUGIN_KNOWN_PACKAGE_POLICIES})
        if(NOT supportedHosts)
            _he_plugin_fail("${manifestContext}: SupportedHosts must not be empty")
        endif()
        foreach(hostName IN LISTS supportedHosts)
            _he_plugin_validate_choice("${hostName}" "${manifestContext}: SupportedHosts"
                ${HE_STATIC_PLUGIN_KNOWN_HOSTS})
        endforeach()
        if(NOT platforms)
            _he_plugin_fail("${manifestContext}: Platforms must not be empty")
        endif()
        foreach(platform IN LISTS platforms)
            _he_plugin_validate_choice("${platform}" "${manifestContext}: Platforms"
                ${HE_STATIC_PLUGIN_KNOWN_PLATFORMS})
        endforeach()
        if(NOT configurations)
            _he_plugin_fail("${manifestContext}: Configurations must not be empty")
        endif()
        foreach(configuration IN LISTS configurations)
            _he_plugin_validate_choice("${configuration}" "${manifestContext}: Configurations"
                ${HE_STATIC_PLUGIN_KNOWN_CONFIGURATIONS})
        endforeach()
        _he_plugin_validate_relative_paths(
            "${pluginId}" "${manifestDirectory}" ShaderRoots ${shaderRoots})
        _he_plugin_validate_relative_paths(
            "${pluginId}" "${manifestDirectory}" ContentRoots ${contentRoots})

        if((distribution STREQUAL "DeveloperOnly" OR
            distribution STREQUAL "EditorOnly") AND
           NOT packagePolicy STREQUAL "Never")
            _he_plugin_fail(
                "${manifestContext}: non-Runtime distribution requires PackagePolicy Never")
        endif()
        if((packagePolicy STREQUAL "Always" OR
            packagePolicy STREQUAL "ProjectSelectable") AND
           NOT distribution STREQUAL "Runtime")
            _he_plugin_fail(
                "${manifestContext}: package inclusion requires Distribution Runtime")
        endif()
        if(distribution STREQUAL "EditorOnly")
            foreach(hostName IN LISTS supportedHosts)
                if(NOT hostName STREQUAL "Editor")
                    _he_plugin_fail(
                        "${manifestContext}: EditorOnly supports only the Editor host")
                endif()
            endforeach()
        endif()
        list(FIND supportedHosts Game gameIndex)
        if(NOT gameIndex EQUAL -1 AND
           (NOT distribution STREQUAL "Runtime" OR
            packagePolicy STREQUAL "Never"))
            _he_plugin_fail(
                "${manifestContext}: Game host requires Runtime and non-Never PackagePolicy")
        endif()
        if(packagePolicy STREQUAL "Always" AND gameIndex EQUAL -1)
            _he_plugin_fail(
                "${manifestContext}: PackagePolicy Always requires Game in SupportedHosts")
        endif()

        list(FIND allPluginIds "${pluginId}" duplicateIdIndex)
        if(NOT duplicateIdIndex EQUAL -1)
            _he_plugin_fail("duplicate plugin Id '${pluginId}'")
        endif()
        list(FIND allPluginModules "${moduleName}" duplicateModuleIndex)
        if(NOT duplicateModuleIndex EQUAL -1)
            _he_plugin_fail("duplicate plugin Module '${moduleName}'")
        endif()
        list(FIND allFactoryAnchors "${entrySymbol}" duplicateAnchorIndex)
        if(NOT duplicateAnchorIndex EQUAL -1)
            _he_plugin_fail("duplicate plugin EntrySymbol '${entrySymbol}'")
        endif()
        list(APPEND allPluginIds "${pluginId}")
        list(APPEND allPluginModules "${moduleName}")
        list(APPEND allFactoryAnchors "${entrySymbol}")

        _he_plugin_set("${pluginId}" DISPLAY_NAME "${displayName}")
        _he_plugin_set("${pluginId}" VERSION "${version}")
        _he_plugin_set("${pluginId}" VERSION_MAJOR "${versionMajor}")
        _he_plugin_set("${pluginId}" VERSION_MINOR "${versionMinor}")
        _he_plugin_set("${pluginId}" VERSION_PATCH "${versionPatch}")
        _he_plugin_set("${pluginId}" API_VERSION "${apiVersion}")
        _he_plugin_set("${pluginId}" REQUIRED_HOST_API_VERSION "${requiredHostApiVersion}")
        _he_plugin_set("${pluginId}" MODULE_NAME "${moduleName}")
        _he_plugin_set("${pluginId}" ENTRY_SYMBOL "${entrySymbol}")
        _he_plugin_set("${pluginId}" DEPENDENCIES ${dependencies})
        _he_plugin_set("${pluginId}" DEPENDENCY_VERSION_RANGES ${dependencyVersionRanges})
        _he_plugin_set("${pluginId}" DEPENDENCY_MINIMUM_API_VERSIONS ${dependencyMinimumApiVersions})
        _he_plugin_set("${pluginId}" OPTIONAL_DEPENDENCIES ${optionalDependencies})
        _he_plugin_set("${pluginId}" OPTIONAL_DEPENDENCY_VERSION_RANGES ${optionalDependencyVersionRanges})
        _he_plugin_set("${pluginId}" OPTIONAL_DEPENDENCY_MINIMUM_API_VERSIONS ${optionalDependencyMinimumApiVersions})
        _he_plugin_set("${pluginId}" LOAD_PHASE "${loadPhase}")
        _he_plugin_set("${pluginId}" SUPPORTED_HOSTS ${supportedHosts})
        _he_plugin_set("${pluginId}" DISTRIBUTION "${distribution}")
        _he_plugin_set("${pluginId}" PACKAGE_POLICY "${packagePolicy}")
        _he_plugin_set("${pluginId}" SHADER_ROOTS ${shaderRoots})
        _he_plugin_set("${pluginId}" CONTENT_ROOTS ${contentRoots})
        _he_plugin_set("${pluginId}" PLATFORMS ${platforms})
        _he_plugin_set("${pluginId}" CONFIGURATIONS ${configurations})
        _he_plugin_set("${pluginId}" SOURCE_DIR "${manifestDirectory}")
        _he_plugin_set("${pluginId}" MANIFEST "${manifestPath}")
        _he_plugin_module_key(moduleKey "${moduleName}")
        set_property(GLOBAL PROPERTY "HE_STATIC_PLUGIN_MODULE_${moduleKey}_ID" "${pluginId}")
    endforeach()

    list(SORT allPluginIds)
    foreach(pluginId IN LISTS allPluginIds)
        _he_plugin_get(dependencies "${pluginId}" DEPENDENCIES)
        _he_plugin_get(dependencyVersionRanges "${pluginId}" DEPENDENCY_VERSION_RANGES)
        _he_plugin_get(dependencyMinimumApiVersions
            "${pluginId}" DEPENDENCY_MINIMUM_API_VERSIONS)
        _he_plugin_get(optionalDependencies "${pluginId}" OPTIONAL_DEPENDENCIES)
        _he_plugin_get(optionalDependencyVersionRanges
            "${pluginId}" OPTIONAL_DEPENDENCY_VERSION_RANGES)
        _he_plugin_get(optionalDependencyMinimumApiVersions
            "${pluginId}" OPTIONAL_DEPENDENCY_MINIMUM_API_VERSIONS)
        set(allDependencies ${dependencies} ${optionalDependencies})
        set(allDependencyVersionRanges
            ${dependencyVersionRanges} ${optionalDependencyVersionRanges})
        set(allDependencyMinimumApiVersions
            ${dependencyMinimumApiVersions} ${optionalDependencyMinimumApiVersions})
        _he_plugin_validate_unique_list(
            "plugin '${pluginId}' dependency lists" ${allDependencies})
        _he_plugin_get(pluginHosts "${pluginId}" SUPPORTED_HOSTS)
        _he_plugin_get(pluginPhase "${pluginId}" LOAD_PHASE)
        _he_plugin_phase_rank(pluginPhaseRank "${pluginPhase}")
        list(LENGTH allDependencies dependencyCount)
        if(dependencyCount GREATER 0)
            math(EXPR lastDependencyIndex "${dependencyCount} - 1")
            foreach(dependencyIndex RANGE 0 ${lastDependencyIndex})
                list(GET allDependencies ${dependencyIndex} dependencyId)
                list(GET allDependencyVersionRanges ${dependencyIndex} dependencyVersionRange)
                list(GET allDependencyMinimumApiVersions
                    ${dependencyIndex} dependencyMinimumApiVersion)
                if(dependencyId STREQUAL pluginId)
                    _he_plugin_fail("plugin '${pluginId}' cannot depend on itself")
                endif()
                list(FIND allPluginIds "${dependencyId}" knownDependencyIndex)
                if(knownDependencyIndex EQUAL -1)
                    _he_plugin_fail(
                        "plugin '${pluginId}' has unknown dependency '${dependencyId}'")
                endif()
                _he_plugin_get(dependencyVersion "${dependencyId}" VERSION)
                _he_plugin_version_satisfies(
                    versionMatches "${dependencyVersion}" "${dependencyVersionRange}")
                if(NOT versionMatches)
                    _he_plugin_fail(
                        "plugin '${pluginId}' requires '${dependencyId}' version '${dependencyVersionRange}', found ${dependencyVersion}")
                endif()
                _he_plugin_get(dependencyApiVersion "${dependencyId}" API_VERSION)
                if(dependencyApiVersion LESS dependencyMinimumApiVersion)
                    _he_plugin_fail(
                        "plugin '${pluginId}' requires '${dependencyId}' API >= ${dependencyMinimumApiVersion}, found ${dependencyApiVersion}")
                endif()
                _he_plugin_get(dependencyPhase "${dependencyId}" LOAD_PHASE)
                _he_plugin_phase_rank(dependencyPhaseRank "${dependencyPhase}")
                if(dependencyPhaseRank GREATER pluginPhaseRank)
                    _he_plugin_fail(
                        "plugin '${pluginId}' loads before dependency '${dependencyId}'")
                endif()
            endforeach()
        endif()
        foreach(dependencyId IN LISTS dependencies)
            _he_plugin_get(dependencyHosts "${dependencyId}" SUPPORTED_HOSTS)
            foreach(hostName IN LISTS pluginHosts)
                list(FIND dependencyHosts "${hostName}" dependencyHostIndex)
                if(dependencyHostIndex EQUAL -1)
                    _he_plugin_fail(
                        "plugin '${pluginId}' requires '${dependencyId}', which does not support host ${hostName}")
                endif()
            endforeach()
            _he_plugin_get(pluginPackagePolicy "${pluginId}" PACKAGE_POLICY)
            _he_plugin_get(dependencyPackagePolicy "${dependencyId}" PACKAGE_POLICY)
            if(NOT pluginPackagePolicy STREQUAL "Never" AND
               dependencyPackagePolicy STREQUAL "Never")
                _he_plugin_fail(
                    "package-capable plugin '${pluginId}' depends on Never plugin '${dependencyId}'")
            endif()
        endforeach()
    endforeach()

    # Stable dependency-first order. Both required and optional edges participate
    # so enabling an optional dependency cannot perturb registration unpredictably.
    set(remainingPluginIds ${allPluginIds})
    set(orderedPluginIds "")
    while(remainingPluginIds)
        set(progress FALSE)
        set(nextRemaining "")
        foreach(pluginId IN LISTS remainingPluginIds)
            _he_plugin_get(dependencies "${pluginId}" DEPENDENCIES)
            _he_plugin_get(optionalDependencies "${pluginId}" OPTIONAL_DEPENDENCIES)
            set(blocked FALSE)
            foreach(dependencyId IN LISTS dependencies optionalDependencies)
                list(FIND remainingPluginIds "${dependencyId}" remainingDependencyIndex)
                if(NOT remainingDependencyIndex EQUAL -1)
                    set(blocked TRUE)
                    break()
                endif()
            endforeach()
            if(blocked)
                list(APPEND nextRemaining "${pluginId}")
            else()
                list(APPEND orderedPluginIds "${pluginId}")
                set(progress TRUE)
            endif()
        endforeach()
        if(NOT progress)
            string(JOIN ", " cycleText ${remainingPluginIds})
            _he_plugin_fail("plugin dependency cycle detected among: ${cycleText}")
        endif()
        set(remainingPluginIds ${nextRemaining})
    endwhile()

    file(READ "${pluginConfig}" configJson)
    string(JSON configType ERROR_VARIABLE configError TYPE "${configJson}")
    if(NOT configError STREQUAL "NOTFOUND" OR NOT configType STREQUAL "OBJECT")
        _he_plugin_fail("plugin host config must be a JSON object: ${pluginConfig}")
    endif()
    foreach(pluginId IN LISTS allPluginIds)
        foreach(hostName IN LISTS HE_STATIC_PLUGIN_KNOWN_HOSTS)
            _he_plugin_set_host_enabled("${pluginId}" "${hostName}" FALSE)
        endforeach()
    endforeach()

    string(JSON configuredPluginCount LENGTH "${configJson}")
    if(configuredPluginCount GREATER 0)
        math(EXPR lastConfiguredPluginIndex "${configuredPluginCount} - 1")
        foreach(configuredPluginIndex RANGE 0 ${lastConfiguredPluginIndex})
            string(JSON configuredPluginId MEMBER
                "${configJson}" ${configuredPluginIndex})
            list(FIND allPluginIds "${configuredPluginId}" configuredPluginIndexInManifest)
            if(configuredPluginIndexInManifest EQUAL -1)
                _he_plugin_fail(
                    "plugin host config names unknown stable Id '${configuredPluginId}'")
            endif()
            string(JSON hostConfigType TYPE "${configJson}" "${configuredPluginId}")
            if(NOT hostConfigType STREQUAL "OBJECT")
                _he_plugin_fail(
                    "plugin host config '${configuredPluginId}' must be an object")
            endif()
            string(JSON hostConfigJson GET "${configJson}" "${configuredPluginId}")
            _he_plugin_json_validate_members(
                "${hostConfigJson}" "plugin host config '${configuredPluginId}'"
                ${HE_STATIC_PLUGIN_KNOWN_HOSTS})
            string(JSON configuredHostCount LENGTH "${hostConfigJson}")
            if(configuredHostCount GREATER 0)
                math(EXPR lastConfiguredHostIndex "${configuredHostCount} - 1")
                foreach(configuredHostIndex RANGE 0 ${lastConfiguredHostIndex})
                    string(JSON configuredHost MEMBER "${hostConfigJson}" ${configuredHostIndex})
                    string(JSON configuredHostType TYPE "${hostConfigJson}" "${configuredHost}")
                    if(NOT configuredHostType STREQUAL "BOOLEAN")
                        _he_plugin_fail(
                            "plugin host config '${configuredPluginId}.${configuredHost}' must be BOOLEAN")
                    endif()
                    string(JSON configuredHostValue GET "${hostConfigJson}" "${configuredHost}")
                    if(configuredHostValue)
                        _he_plugin_get(pluginHosts "${configuredPluginId}" SUPPORTED_HOSTS)
                        list(FIND pluginHosts "${configuredHost}" supportedHostIndex)
                        if(supportedHostIndex EQUAL -1)
                            _he_plugin_fail(
                                "plugin host config enables unsupported host '${configuredPluginId}.${configuredHost}'")
                        endif()
                        _he_plugin_set_host_enabled(
                            "${configuredPluginId}" "${configuredHost}" TRUE)
                    endif()
                endforeach()
            endif()
        endforeach()
    endif()

    # Required dependencies must be explicit in host config. Optional dependencies
    # participate only when independently enabled for that host.
    foreach(pluginId IN LISTS allPluginIds)
        _he_plugin_get(dependencies "${pluginId}" DEPENDENCIES)
        foreach(hostName IN LISTS HE_STATIC_PLUGIN_KNOWN_HOSTS)
            _he_plugin_get_host_enabled(pluginEnabled "${pluginId}" "${hostName}")
            if(pluginEnabled)
                foreach(dependencyId IN LISTS dependencies)
                    _he_plugin_get_host_enabled(
                        dependencyEnabled "${dependencyId}" "${hostName}")
                    if(NOT dependencyEnabled)
                        _he_plugin_fail(
                            "plugin '${pluginId}' requires '${dependencyId}' to be explicitly enabled for host ${hostName}")
                    endif()
                endforeach()
            endif()
        endforeach()
    endforeach()

    set(enabledPluginIds "")
    set(enabledPluginModules "")
    set(enabledPluginDirectories "")
    foreach(pluginId IN LISTS orderedPluginIds)
        if(HEP_BUILD_DISTRIBUTION STREQUAL "Shipping")
            _he_plugin_get(packagePolicy "${pluginId}" PACKAGE_POLICY)
            _he_plugin_get(supportedHosts "${pluginId}" SUPPORTED_HOSTS)
            list(FIND supportedHosts Game supportsGameIndex)
            if(packagePolicy STREQUAL "Always" AND NOT supportsGameIndex EQUAL -1)
                _he_plugin_get_host_enabled(alwaysEnabled "${pluginId}" Game)
                if(NOT alwaysEnabled)
                    _he_plugin_fail(
                        "Shipping requires PackagePolicy Always plugin '${pluginId}' to be enabled for Game")
                endif()
            endif()
        endif()
        set(enabledForActiveHost FALSE)
        foreach(hostName IN LISTS HEP_ACTIVE_HOSTS)
            _he_plugin_get_host_enabled(hostEnabled "${pluginId}" "${hostName}")
            if(hostEnabled)
                set(enabledForActiveHost TRUE)
            endif()
        endforeach()
        if(enabledForActiveHost)
            _he_plugin_get(platforms "${pluginId}" PLATFORMS)
            list(FIND platforms "${HEP_PLATFORM}" platformIndex)
            if(platformIndex EQUAL -1)
                _he_plugin_fail(
                    "enabled plugin '${pluginId}' does not support platform ${HEP_PLATFORM}")
            endif()
            _he_plugin_get(configurations "${pluginId}" CONFIGURATIONS)
            foreach(configuration IN LISTS configurationsToValidate)
                list(FIND configurations "${configuration}" configurationIndex)
                if(configurationIndex EQUAL -1)
                    _he_plugin_fail(
                        "enabled plugin '${pluginId}' does not support configuration ${configuration}")
                endif()
            endforeach()
            if(HEP_BUILD_DISTRIBUTION STREQUAL "Shipping")
                _he_plugin_get(distribution "${pluginId}" DISTRIBUTION)
                _he_plugin_get(packagePolicy "${pluginId}" PACKAGE_POLICY)
                if(NOT distribution STREQUAL "Runtime" OR
                   packagePolicy STREQUAL "Never")
                    _he_plugin_fail(
                        "Shipping refuses plugin '${pluginId}' without Runtime + Always/ProjectSelectable")
                endif()
            endif()
            _he_plugin_get(moduleName "${pluginId}" MODULE_NAME)
            _he_plugin_get(sourceDirectory "${pluginId}" SOURCE_DIR)
            list(APPEND enabledPluginIds "${pluginId}")
            list(APPEND enabledPluginModules "${moduleName}")
            list(APPEND enabledPluginDirectories "${sourceDirectory}")
        endif()
    endforeach()

    set_property(GLOBAL PROPERTY HE_STATIC_PLUGIN_IDS "${orderedPluginIds}")
    set_property(GLOBAL PROPERTY HE_STATIC_PLUGIN_MODULES "${allPluginModules}")
    set_property(GLOBAL PROPERTY HE_STATIC_PLUGIN_ENABLED_IDS "${enabledPluginIds}")
    set_property(GLOBAL PROPERTY HE_STATIC_PLUGIN_ENABLED_MODULES "${enabledPluginModules}")
    set_property(GLOBAL PROPERTY HE_STATIC_PLUGIN_ENABLED_DIRECTORIES "${enabledPluginDirectories}")
    set_property(GLOBAL PROPERTY HE_STATIC_PLUGIN_ACTIVE_HOSTS "${HEP_ACTIVE_HOSTS}")
    set_property(GLOBAL PROPERTY HE_STATIC_PLUGIN_CONFIGURATION "${HEP_CONFIGURATION}")
    set_property(GLOBAL PROPERTY HE_STATIC_PLUGIN_PLATFORM "${HEP_PLATFORM}")
    set_property(GLOBAL PROPERTY
        HE_STATIC_PLUGIN_BUILD_DISTRIBUTION "${HEP_BUILD_DISTRIBUTION}")
    set_property(GLOBAL PROPERTY HE_STATIC_PLUGIN_GENERATED_DIR "${generatedDirectory}")
    foreach(hostName IN LISTS HE_STATIC_PLUGIN_KNOWN_HOSTS)
        _he_plugin_generate_host_anchor(
            "${hostName}" "${generatedDirectory}" "${orderedPluginIds}")
    endforeach()
    set_property(GLOBAL PROPERTY HE_STATIC_PLUGIN_DISCOVERED TRUE)

    # Compatibility variables keep the existing graph configurable until each
    # host target is migrated to he_attach_static_plugins(). Runtime is
    # intentionally false: host ownership must remain explicit.
    set(PluginNames "${enabledPluginModules}" PARENT_SCOPE)
    foreach(pluginId IN LISTS allPluginIds)
        _he_plugin_get(moduleName "${pluginId}" MODULE_NAME)
        set("PluginsConfig.${moduleName}.Runtime" FALSE PARENT_SCOPE)
        foreach(hostName IN LISTS HE_STATIC_PLUGIN_KNOWN_HOSTS)
            _he_plugin_get_host_enabled(hostEnabled "${pluginId}" "${hostName}")
            set("PluginsConfig.${moduleName}.${hostName}" "${hostEnabled}" PARENT_SCOPE)
        endforeach()
    endforeach()
    set(HE_STATIC_PLUGIN_IDS "${orderedPluginIds}" PARENT_SCOPE)
    set(HE_ENABLED_STATIC_PLUGIN_IDS "${enabledPluginIds}" PARENT_SCOPE)
    set(HE_ENABLED_STATIC_PLUGIN_MODULES "${enabledPluginModules}" PARENT_SCOPE)
    set(HE_STATIC_PLUGIN_GENERATED_DIR "${generatedDirectory}" PARENT_SCOPE)
    set(HE_STATIC_PLUGIN_ACTIVE_HOSTS "${HEP_ACTIVE_HOSTS}" PARENT_SCOPE)
    set(HE_STATIC_PLUGIN_BUILD_DISTRIBUTION
        "${HEP_BUILD_DISTRIBUTION}" PARENT_SCOPE)
    set(HE_STATIC_PLUGIN_PLATFORM "${HEP_PLATFORM}" PARENT_SCOPE)
    set(HE_STATIC_PLUGIN_CONFIGURATION "${HEP_CONFIGURATION}" PARENT_SCOPE)
    foreach(hostName IN LISTS HE_STATIC_PLUGIN_KNOWN_HOSTS)
        get_property(hostAnchor GLOBAL PROPERTY "HE_STATIC_PLUGIN_ANCHOR_${hostName}")
        get_property(hostIds GLOBAL PROPERTY "HE_STATIC_PLUGIN_HOST_${hostName}_IDS")
        set("HE_STATIC_PLUGIN_ANCHOR_${hostName}" "${hostAnchor}" PARENT_SCOPE)
        set("HE_STATIC_PLUGIN_IDS_${hostName}" "${hostIds}" PARENT_SCOPE)
    endforeach()
endfunction()

macro(ADD_PLUGIN requestedPluginModule)
    if(NOT DEFINED HE_CURRENT_STATIC_PLUGIN_ID OR
       NOT DEFINED HE_CURRENT_STATIC_PLUGIN_MODULE OR
       NOT DEFINED HE_CURRENT_STATIC_PLUGIN_SOURCE_DIR)
        _he_plugin_fail(
            "ADD_PLUGIN(${requestedPluginModule}) must run through he_add_discovered_static_plugins")
    endif()
    if(NOT "${requestedPluginModule}" STREQUAL "${HE_CURRENT_STATIC_PLUGIN_MODULE}")
        _he_plugin_fail(
            "plugin '${HE_CURRENT_STATIC_PLUGIN_ID}' manifest ModuleName is '${HE_CURRENT_STATIC_PLUGIN_MODULE}', but CMake requested '${requestedPluginModule}'")
    endif()
    if(TARGET "${requestedPluginModule}")
        _he_plugin_fail("plugin target already exists: ${requestedPluginModule}")
    endif()
    add_library("${requestedPluginModule}" STATIC "")
    target_include_directories("${requestedPluginModule}" PRIVATE "${EngineSourceDir}")
    add_library("Plugin::${requestedPluginModule}" ALIAS "${requestedPluginModule}")
    set_target_properties("${requestedPluginModule}" PROPERTIES
        FOLDER "Plugins"
        HE_STATIC_PLUGIN_ID "${HE_CURRENT_STATIC_PLUGIN_ID}"
        HE_STATIC_PLUGIN_MANIFEST "${HE_CURRENT_STATIC_PLUGIN_MANIFEST}"
        HE_STATIC_PLUGIN_ENTRY_SYMBOL "${HE_CURRENT_STATIC_PLUGIN_ENTRY_SYMBOL}")
    set(PluginName "${requestedPluginModule}")
endmacro()

macro(PLUGIN_ADD_SOURCE)
    if(NOT DEFINED PluginName OR NOT TARGET "${PluginName}")
        _he_plugin_fail("PLUGIN_ADD_SOURCE requires ADD_PLUGIN in the plugin root")
    endif()
    set(pluginAddedFiles "")
    foreach(sourceFile ${ARGN})
        if(NOT sourceFile MATCHES "(\\.h$)|(\\.hpp$)|(\\.cpp$)|(\\.cc$)")
            _he_plugin_fail(
                "plugin '${HE_CURRENT_STATIC_PLUGIN_ID}' source has unsupported extension: ${sourceFile}")
        endif()
        set(absoluteSourceFile "${CMAKE_CURRENT_SOURCE_DIR}/${sourceFile}")
        if(NOT EXISTS "${absoluteSourceFile}")
            _he_plugin_fail(
                "plugin '${HE_CURRENT_STATIC_PLUGIN_ID}' source does not exist: ${absoluteSourceFile}")
        endif()
        list(APPEND pluginAddedFiles "${absoluteSourceFile}")
        if(sourceFile MATCHES "(\\.h$)|(\\.hpp$)")
            set_property(GLOBAL APPEND PROPERTY HE_STATIC_PLUGIN_HEADER_FILES "${absoluteSourceFile}")
        else()
            set_property(GLOBAL APPEND PROPERTY HE_STATIC_PLUGIN_SOURCE_FILES "${absoluteSourceFile}")
        endif()
    endforeach()
    if(pluginAddedFiles)
        target_sources("${PluginName}" PRIVATE ${pluginAddedFiles})
        set_property(TARGET "${PluginName}" APPEND PROPERTY
            HE_STATIC_PLUGIN_DECLARED_SOURCES "${pluginAddedFiles}")
    endif()
endmacro()

macro(END_PLUGIN)
    if(NOT DEFINED PluginName OR NOT TARGET "${PluginName}")
        _he_plugin_fail("END_PLUGIN requires ADD_PLUGIN in the plugin root")
    endif()
    get_target_property(pluginDeclaredSources "${PluginName}" HE_STATIC_PLUGIN_DECLARED_SOURCES)
    if(NOT pluginDeclaredSources OR pluginDeclaredSources STREQUAL "pluginDeclaredSources-NOTFOUND")
        _he_plugin_fail("plugin '${HE_CURRENT_STATIC_PLUGIN_ID}' declares no C++ sources")
    endif()
    source_group(TREE "${HE_CURRENT_STATIC_PLUGIN_SOURCE_DIR}" FILES ${pluginDeclaredSources})
endmacro()

function(he_add_discovered_static_plugins)
    if(CMAKE_SCRIPT_MODE_FILE)
        _he_plugin_fail("he_add_discovered_static_plugins is unavailable in script mode")
    endif()
    get_property(discovered GLOBAL PROPERTY HE_STATIC_PLUGIN_DISCOVERED)
    if(NOT discovered)
        _he_plugin_fail("he_add_discovered_static_plugins called before discovery")
    endif()
    get_property(alreadyAdded GLOBAL PROPERTY HE_STATIC_PLUGIN_DIRECTORIES_ADDED)
    if(alreadyAdded)
        _he_plugin_fail("static plugin directories were already added")
    endif()
    get_property(enabledPluginIds GLOBAL PROPERTY HE_STATIC_PLUGIN_ENABLED_IDS)
    foreach(pluginId IN LISTS enabledPluginIds)
        _he_plugin_get(moduleName "${pluginId}" MODULE_NAME)
        _he_plugin_get(sourceDirectory "${pluginId}" SOURCE_DIR)
        _he_plugin_get(manifestPath "${pluginId}" MANIFEST)
        _he_plugin_get(entrySymbol "${pluginId}" ENTRY_SYMBOL)
        set(HE_CURRENT_STATIC_PLUGIN_ID "${pluginId}")
        set(HE_CURRENT_STATIC_PLUGIN_MODULE "${moduleName}")
        set(HE_CURRENT_STATIC_PLUGIN_SOURCE_DIR "${sourceDirectory}")
        set(HE_CURRENT_STATIC_PLUGIN_MANIFEST "${manifestPath}")
        set(HE_CURRENT_STATIC_PLUGIN_ENTRY_SYMBOL "${entrySymbol}")
        add_subdirectory("${sourceDirectory}" "${moduleName}")
        if(NOT TARGET "${moduleName}")
            _he_plugin_fail(
                "enabled plugin '${pluginId}' did not create target '${moduleName}'")
        endif()
    endforeach()
    set_property(GLOBAL PROPERTY HE_STATIC_PLUGIN_DIRECTORIES_ADDED TRUE)
endfunction()

function(he_attach_static_plugins targetName hostName)
    if(ARGC GREATER 2)
        _he_plugin_fail("he_attach_static_plugins expects exactly TARGET and HOST")
    endif()
    if(NOT TARGET "${targetName}")
        _he_plugin_fail("he_attach_static_plugins target does not exist: ${targetName}")
    endif()
    _he_plugin_validate_choice("${hostName}" "he_attach_static_plugins host"
        ${HE_STATIC_PLUGIN_KNOWN_HOSTS})
    get_property(discovered GLOBAL PROPERTY HE_STATIC_PLUGIN_DISCOVERED)
    if(NOT discovered)
        _he_plugin_fail("he_attach_static_plugins called before discovery")
    endif()
    get_property(activeHosts GLOBAL PROPERTY HE_STATIC_PLUGIN_ACTIVE_HOSTS)
    list(FIND activeHosts "${hostName}" activeHostIndex)
    if(activeHostIndex EQUAL -1)
        _he_plugin_fail(
            "host ${hostName} is not active in this configure; active hosts: ${activeHosts}")
    endif()
    get_target_property(existingHost "${targetName}" HE_STATIC_PLUGIN_HOST)
    if(existingHost AND NOT existingHost STREQUAL "existingHost-NOTFOUND")
        _he_plugin_fail(
            "target '${targetName}' already has static plugin host ${existingHost}")
    endif()

    get_property(anchorPath GLOBAL PROPERTY "HE_STATIC_PLUGIN_ANCHOR_${hostName}")
    get_property(hostPluginIds GLOBAL PROPERTY "HE_STATIC_PLUGIN_HOST_${hostName}_IDS")
    get_property(buildDistribution GLOBAL PROPERTY
        HE_STATIC_PLUGIN_BUILD_DISTRIBUTION)
    get_property(pluginPlatform GLOBAL PROPERTY HE_STATIC_PLUGIN_PLATFORM)
    get_property(pluginConfiguration GLOBAL PROPERTY HE_STATIC_PLUGIN_CONFIGURATION)
    set(pluginTargets "")
    set(pluginShaderTargets "")
    # Restore runtime payloads on incremental builds even when no relink occurs.
    add_custom_target(${targetName}PluginRuntime)
    add_dependencies(${targetName} ${targetName}PluginRuntime)
    foreach(pluginId IN LISTS hostPluginIds)
        _he_plugin_get(moduleName "${pluginId}" MODULE_NAME)
        if(NOT TARGET "${moduleName}")
            _he_plugin_fail(
                "host ${hostName} plugin '${pluginId}' target '${moduleName}' was not created")
        endif()
        list(APPEND pluginTargets "Plugin::${moduleName}")
        # Only enabled hosts receive a plugin's declared runtime dependencies.
        get_target_property(pluginRuntimeFiles "${moduleName}" HE_PLUGIN_RUNTIME_FILES)
        if(pluginRuntimeFiles AND NOT pluginRuntimeFiles STREQUAL "pluginRuntimeFiles-NOTFOUND")
            get_target_property(pluginRuntimeDirectory "${moduleName}" HE_PLUGIN_RUNTIME_DIRECTORY)
            if(NOT pluginRuntimeDirectory OR pluginRuntimeDirectory STREQUAL "pluginRuntimeDirectory-NOTFOUND")
                set(pluginRuntimeDirectory ".")
            endif()
            add_custom_command(TARGET "${targetName}PluginRuntime" POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "$<TARGET_FILE_DIR:${targetName}>/${pluginRuntimeDirectory}"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    ${pluginRuntimeFiles} "$<TARGET_FILE_DIR:${targetName}>/${pluginRuntimeDirectory}"
                COMMAND_EXPAND_LISTS VERBATIM)
        endif()
        get_target_property(pluginSharedRuntime "${moduleName}" HE_PLUGIN_SHARED_RUNTIME_FILES)
        if(pluginSharedRuntime AND NOT pluginSharedRuntime STREQUAL "pluginSharedRuntime-NOTFOUND")
            add_custom_command(TARGET "${targetName}PluginRuntime" POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    ${pluginSharedRuntime} "$<TARGET_FILE_DIR:${targetName}>"
                COMMAND_EXPAND_LISTS VERBATIM)
        endif()
        get_target_property(pluginOidn "${moduleName}" HE_OIDN_ENABLED)
        if(pluginOidn)
            set_property(TARGET "${targetName}" PROPERTY HE_OIDN_ENABLED TRUE)
            if(WIN32)
                include("${CMAKE_SOURCE_DIR}/CMake/WindowsVendorRuntime.cmake")
                he_deploy_vendor_crt(${targetName})
            endif()
        endif()
        get_target_property(pluginLicenseFiles "${moduleName}" HE_PLUGIN_LICENSE_FILES)
        if(pluginLicenseFiles AND NOT pluginLicenseFiles STREQUAL "pluginLicenseFiles-NOTFOUND")
            add_custom_command(TARGET "${targetName}PluginRuntime" POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${targetName}>/licenses/${moduleName}"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    ${pluginLicenseFiles} "$<TARGET_FILE_DIR:${targetName}>/licenses/${moduleName}"
                COMMAND_EXPAND_LISTS VERBATIM)
        endif()
        get_target_property(pluginShaderTarget
            "${moduleName}" HE_PLUGIN_SHADER_TARGET)
        if(pluginShaderTarget AND
           NOT pluginShaderTarget STREQUAL "pluginShaderTarget-NOTFOUND")
            if(NOT TARGET "${pluginShaderTarget}")
                _he_plugin_fail(
                    "plugin '${pluginId}' shader target does not exist: ${pluginShaderTarget}")
            endif()
            list(APPEND pluginShaderTargets "${pluginShaderTarget}")
        endif()
    endforeach()

    target_sources("${targetName}" PRIVATE "${anchorPath}")
    if(pluginTargets)
        target_link_libraries("${targetName}" PRIVATE ${pluginTargets})
    endif()
    if(pluginShaderTargets)
        add_dependencies("${targetName}" ${pluginShaderTargets})
    endif()
    string(TOUPPER "${hostName}" hostUpper)
    target_compile_definitions("${targetName}" PRIVATE
        "HE_STATIC_PLUGIN_HOST_${hostUpper}=1")
    set_target_properties("${targetName}" PROPERTIES
        HE_STATIC_PLUGIN_HOST "${hostName}"
        HE_STATIC_PLUGIN_IDS "${hostPluginIds}"
        HE_STATIC_PLUGIN_ANCHOR "${anchorPath}"
        HE_STATIC_PLUGIN_BUILD_DISTRIBUTION "${buildDistribution}"
        HE_STATIC_PLUGIN_PLATFORM "${pluginPlatform}"
        HE_STATIC_PLUGIN_CONFIGURATION "${pluginConfiguration}")
endfunction()

function(he_get_static_plugin_property outputVariable pluginId propertyName)
    get_property(allPluginIds GLOBAL PROPERTY HE_STATIC_PLUGIN_IDS)
    list(FIND allPluginIds "${pluginId}" pluginIndex)
    if(pluginIndex EQUAL -1)
        _he_plugin_fail("unknown plugin Id '${pluginId}'")
    endif()
    _he_plugin_get(propertyValue "${pluginId}" "${propertyName}")
    set(${outputVariable} "${propertyValue}" PARENT_SCOPE)
endfunction()
