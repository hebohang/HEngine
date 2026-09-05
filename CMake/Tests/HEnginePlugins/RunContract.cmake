cmake_minimum_required(VERSION 3.20)

get_filename_component(repositoryRoot "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(pluginModule "${repositoryRoot}/CMake/HEnginePlugins.cmake")
set(probeScript "${CMAKE_CURRENT_LIST_DIR}/Probe.cmake")
set(fixtureProject "${CMAKE_CURRENT_LIST_DIR}/FixtureProject")
if(NOT DEFINED HE_PLUGIN_CONTRACT_BINARY_DIR)
    set(HE_PLUGIN_CONTRACT_BINARY_DIR
        "${repositoryRoot}/out/agent/plugin-manifest-contract")
endif()
get_filename_component(contractOutputRoot
    "${HE_PLUGIN_CONTRACT_BINARY_DIR}" ABSOLUTE)
file(RELATIVE_PATH contractOutputRelative "${repositoryRoot}" "${contractOutputRoot}")
string(REPLACE "\\" "/" contractOutputRelative "${contractOutputRelative}")
if(IS_ABSOLUTE "${contractOutputRelative}" OR
   contractOutputRelative MATCHES "(^|/)\\.\\.(/|$)")
    message(FATAL_ERROR
        "HE_PLUGIN_CONTRACT_BINARY_DIR must stay beneath the repository: ${contractOutputRoot}")
endif()
string(TIMESTAMP contractTimestamp "%Y%m%d-%H%M%S")
string(RANDOM LENGTH 8 ALPHABET 0123456789abcdef contractNonce)
set(contractRunRoot
    "${contractOutputRoot}/run-${contractTimestamp}-${contractNonce}")
file(MAKE_DIRECTORY "${contractRunRoot}")

function(_he_contract_json_string_array outputVariable)
    set(rendered "")
    foreach(value IN LISTS ARGN)
        if(rendered STREQUAL "")
            set(rendered "\"${value}\"")
        else()
            string(APPEND rendered ", \"${value}\"")
        endif()
    endforeach()
    set(${outputVariable} "[${rendered}]" PARENT_SCOPE)
endfunction()

function(_he_contract_write_plugin caseRoot moduleName pluginId)
    set(options "")
    set(oneValueArguments
        VERSION API_VERSION REQUIRED_HOST_API_VERSION LOAD_PHASE DISTRIBUTION
        PACKAGE_POLICY DEPENDENCIES_JSON OPTIONAL_DEPENDENCIES_JSON)
    set(multiValueArguments HOSTS)
    cmake_parse_arguments(PLUGIN
        "${options}" "${oneValueArguments}" "${multiValueArguments}" ${ARGN})
    if(PLUGIN_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "Contract plugin ${moduleName}: unknown arguments ${PLUGIN_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT PLUGIN_VERSION)
        set(PLUGIN_VERSION "1.0.0")
    endif()
    if(NOT PLUGIN_API_VERSION)
        set(PLUGIN_API_VERSION 1)
    endif()
    if(NOT PLUGIN_REQUIRED_HOST_API_VERSION)
        set(PLUGIN_REQUIRED_HOST_API_VERSION 1)
    endif()
    if(NOT PLUGIN_LOAD_PHASE)
        set(PLUGIN_LOAD_PHASE PostDevice)
    endif()
    if(NOT PLUGIN_DISTRIBUTION)
        set(PLUGIN_DISTRIBUTION DeveloperOnly)
    endif()
    if(NOT PLUGIN_PACKAGE_POLICY)
        set(PLUGIN_PACKAGE_POLICY Never)
    endif()
    if(NOT PLUGIN_DEPENDENCIES_JSON)
        set(PLUGIN_DEPENDENCIES_JSON "[]")
    endif()
    if(NOT PLUGIN_OPTIONAL_DEPENDENCIES_JSON)
        set(PLUGIN_OPTIONAL_DEPENDENCIES_JSON "[]")
    endif()
    if(NOT PLUGIN_HOSTS)
        set(PLUGIN_HOSTS Editor)
    endif()
    _he_contract_json_string_array(hostsJson ${PLUGIN_HOSTS})
    set(pluginDirectory "${caseRoot}/plugins/${moduleName}")
    file(MAKE_DIRECTORY "${pluginDirectory}")
    file(WRITE "${pluginDirectory}/plugin.json"
"{
  \"SchemaVersion\": 1,
  \"Id\": \"${pluginId}\",
  \"DisplayName\": \"${moduleName}\",
  \"Version\": \"${PLUGIN_VERSION}\",
  \"ApiVersion\": ${PLUGIN_API_VERSION},
  \"RequiredHostApiVersion\": ${PLUGIN_REQUIRED_HOST_API_VERSION},
  \"ModuleName\": \"${moduleName}\",
  \"EntrySymbol\": \"HEngineCreate${moduleName}PluginModule\",
  \"Dependencies\": ${PLUGIN_DEPENDENCIES_JSON},
  \"OptionalDependencies\": ${PLUGIN_OPTIONAL_DEPENDENCIES_JSON},
  \"LoadPhase\": \"${PLUGIN_LOAD_PHASE}\",
  \"SupportedHosts\": ${hostsJson},
  \"Distribution\": \"${PLUGIN_DISTRIBUTION}\",
  \"PackagePolicy\": \"${PLUGIN_PACKAGE_POLICY}\",
  \"ShaderRoots\": [],
  \"ContentRoots\": [],
  \"Platforms\": [\"Windows\"],
  \"Configurations\": [\"Debug\", \"Release\", \"RelWithDebInfo\", \"MinSizeRel\"]
}
")
    # The target-only fixture uses INTERFACE libraries so the contract remains
    # directly runnable with cmake -P and does not require a compiler toolchain.
    file(WRITE "${pluginDirectory}/CMakeLists.txt"
        "add_library(${moduleName} INTERFACE)\n"
        "add_library(Plugin::${moduleName} ALIAS ${moduleName})\n")
endfunction()

function(_he_contract_write_config caseRoot configJson)
    file(WRITE "${caseRoot}/Plugins.json" "${configJson}\n")
endfunction()

function(_he_contract_run_probe caseName expectSuccess expectedPattern)
    set(options SHIPPING)
    set(oneValueArguments INSPECT_HOST)
    set(multiValueArguments
        ACTIVE_HOSTS EXPECT_IDS EXPECT_ENABLED_IDS EXPECT_ANCHOR_TEXT)
    cmake_parse_arguments(PROBE
        "${options}" "${oneValueArguments}" "${multiValueArguments}" ${ARGN})
    set(caseRoot "${contractRunRoot}/${caseName}")
    set(probeGenerated "${caseRoot}/generated")
    set(commandArguments
        "-DHE_PLUGIN_MODULE=${pluginModule}"
        "-DHE_PLUGIN_ROOT=${caseRoot}/plugins"
        "-DHE_PLUGIN_CONFIG=${caseRoot}/Plugins.json"
        "-DHE_PLUGIN_GENERATED_DIR=${probeGenerated}")
    if(PROBE_ACTIVE_HOSTS)
        string(REPLACE ";" "," activeHostsCsv "${PROBE_ACTIVE_HOSTS}")
    else()
        set(activeHostsCsv Editor)
    endif()
    list(APPEND commandArguments "-DHE_PLUGIN_ACTIVE_HOSTS_CSV=${activeHostsCsv}")
    if(PROBE_SHIPPING)
        list(APPEND commandArguments "-DHE_PLUGIN_BUILD_DISTRIBUTION=Shipping")
    endif()
    if(PROBE_EXPECT_IDS)
        string(REPLACE ";" "," expectedIdsCsv "${PROBE_EXPECT_IDS}")
        list(APPEND commandArguments "-DHE_PLUGIN_EXPECT_IDS_CSV=${expectedIdsCsv}")
    endif()
    if(PROBE_EXPECT_ENABLED_IDS)
        string(REPLACE ";" "," enabledIdsCsv "${PROBE_EXPECT_ENABLED_IDS}")
        list(APPEND commandArguments
            "-DHE_PLUGIN_EXPECT_ENABLED_IDS_CSV=${enabledIdsCsv}")
    endif()
    if(PROBE_INSPECT_HOST)
        list(APPEND commandArguments
            "-DHE_PLUGIN_INSPECT_HOST=${PROBE_INSPECT_HOST}")
    endif()
    if(PROBE_EXPECT_ANCHOR_TEXT)
        string(REPLACE ";" "," anchorTextCsv "${PROBE_EXPECT_ANCHOR_TEXT}")
        list(APPEND commandArguments
            "-DHE_PLUGIN_EXPECT_ANCHOR_TEXT_CSV=${anchorTextCsv}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" ${commandArguments} -P "${probeScript}"
        RESULT_VARIABLE probeResult
        OUTPUT_VARIABLE probeOutput
        ERROR_VARIABLE probeError)
    set(probeCombined "${probeOutput}\n${probeError}")
    if(expectSuccess)
        if(NOT probeResult EQUAL 0)
            message(FATAL_ERROR
                "Contract case '${caseName}' unexpectedly failed:\n${probeCombined}")
        endif()
    else()
        if(probeResult EQUAL 0)
            message(FATAL_ERROR
                "Contract case '${caseName}' unexpectedly passed")
        endif()
        if(expectedPattern AND NOT probeCombined MATCHES "${expectedPattern}")
            message(FATAL_ERROR
                "Contract case '${caseName}' failed for the wrong reason; expected '${expectedPattern}':\n${probeCombined}")
        endif()
    endif()
    message(STATUS "HEngine plugin contract ${caseName}: PASS")
endfunction()

# Positive dependency ordering, version/API compatibility, optional dependency,
# generated descriptor ownership, and disabled-target behavior.
set(positiveRoot "${contractRunRoot}/positive")
_he_contract_write_plugin("${positiveRoot}" ContractCore he.contract.core
    VERSION 1.2.0 LOAD_PHASE PreDevice)
_he_contract_write_plugin("${positiveRoot}" ContractOptional he.contract.optional
    VERSION 1.1.0 LOAD_PHASE PostDevice)
_he_contract_write_plugin("${positiveRoot}" ContractFeature he.contract.feature
    VERSION 2.0.0 LOAD_PHASE PostEngine
    DEPENDENCIES_JSON
        "[{\"Id\":\"he.contract.core\",\"VersionRange\":\"^1.1.0\",\"MinimumApiVersion\":1}]"
    OPTIONAL_DEPENDENCIES_JSON
        "[{\"Id\":\"he.contract.optional\",\"VersionRange\":\">=1.0.0\",\"MinimumApiVersion\":1}]")
_he_contract_write_plugin("${positiveRoot}" ContractDisabled he.contract.disabled)
_he_contract_write_config("${positiveRoot}"
"{
  \"he.contract.core\": { \"Editor\": true },
  \"he.contract.optional\": { \"Editor\": true },
  \"he.contract.feature\": { \"Editor\": true },
  \"he.contract.disabled\": { \"Editor\": false }
}")
_he_contract_run_probe(positive TRUE ""
    ACTIVE_HOSTS Editor
    EXPECT_IDS
        "he.contract.core;he.contract.disabled;he.contract.optional;he.contract.feature"
    EXPECT_ENABLED_IDS
        "he.contract.core;he.contract.optional;he.contract.feature"
    INSPECT_HOST Editor
    EXPECT_ANCHOR_TEXT
        "pluginManager.Register"
        "PluginVersionRange"
        "&HEngineCreateContractFeaturePluginModule")

# The positive configure fixture proves disabled plugins never call their
# CMakeLists.txt and therefore never create a target.
set(positiveBuild "${contractRunRoot}/positive-configure")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${fixtureProject}"
        -B "${positiveBuild}"
        "-DHE_PLUGIN_MODULE=${pluginModule}"
        "-DHE_PLUGIN_ROOT=${positiveRoot}/plugins"
        "-DHE_PLUGIN_CONFIG=${positiveRoot}/Plugins.json"
        "-DHE_PLUGIN_GENERATED_DIR=${positiveBuild}/generated"
        "-DHE_PLUGIN_ACTIVE_HOSTS=Editor"
        "-DHE_PLUGIN_EXPECT_TARGETS=ContractCore;ContractOptional;ContractFeature"
        "-DHE_PLUGIN_EXPECT_NO_TARGETS=ContractDisabled"
    RESULT_VARIABLE positiveConfigureResult
    OUTPUT_VARIABLE positiveConfigureOutput
    ERROR_VARIABLE positiveConfigureError)
if(NOT positiveConfigureResult EQUAL 0)
    message(FATAL_ERROR
        "Positive target configure failed:\n${positiveConfigureOutput}\n${positiveConfigureError}")
endif()
message(STATUS "HEngine plugin contract disabled-target: PASS")

set(duplicateRoot "${contractRunRoot}/duplicate-id")
_he_contract_write_plugin("${duplicateRoot}" DuplicateOne he.contract.duplicate)
_he_contract_write_plugin("${duplicateRoot}" DuplicateTwo he.contract.duplicate)
_he_contract_write_config("${duplicateRoot}" "{}")
_he_contract_run_probe(duplicate-id FALSE "duplicate plugin Id")

set(unknownRoot "${contractRunRoot}/unknown-dependency")
_he_contract_write_plugin("${unknownRoot}" UnknownDependency he.contract.unknown
    DEPENDENCIES_JSON
        "[{\"Id\":\"he.contract.missing\",\"VersionRange\":\"1.0.0\",\"MinimumApiVersion\":1}]")
_he_contract_write_config("${unknownRoot}" "{}")
_he_contract_run_probe(unknown-dependency FALSE "unknown dependency")

set(cycleRoot "${contractRunRoot}/dependency-cycle")
_he_contract_write_plugin("${cycleRoot}" CycleA he.contract.cycle-a
    DEPENDENCIES_JSON
        "[{\"Id\":\"he.contract.cycle-b\",\"VersionRange\":\"1.0.0\",\"MinimumApiVersion\":1}]")
_he_contract_write_plugin("${cycleRoot}" CycleB he.contract.cycle-b
    DEPENDENCIES_JSON
        "[{\"Id\":\"he.contract.cycle-a\",\"VersionRange\":\"1.0.0\",\"MinimumApiVersion\":1}]")
_he_contract_write_config("${cycleRoot}" "{}")
_he_contract_run_probe(dependency-cycle FALSE "dependency cycle")

set(versionRoot "${contractRunRoot}/version-mismatch")
_he_contract_write_plugin("${versionRoot}" VersionCore he.contract.version-core
    VERSION 1.0.0 LOAD_PHASE PreDevice)
_he_contract_write_plugin("${versionRoot}" VersionFeature he.contract.version-feature
    LOAD_PHASE PostDevice
    DEPENDENCIES_JSON
        "[{\"Id\":\"he.contract.version-core\",\"VersionRange\":\"^2.0.0\",\"MinimumApiVersion\":1}]")
_he_contract_write_config("${versionRoot}" "{}")
_he_contract_run_probe(version-mismatch FALSE "requires.*version")

set(zeroVersionRoot "${contractRunRoot}/zero-major-version")
_he_contract_write_plugin(
    "${zeroVersionRoot}" ZeroVersion he.contract.zero-version
    VERSION 0.1.0)
_he_contract_write_config("${zeroVersionRoot}" "{}")
_he_contract_run_probe(zero-major-version FALSE "major must be non-zero")

set(apiRoot "${contractRunRoot}/host-api")
_he_contract_write_plugin("${apiRoot}" HostApi he.contract.host-api
    REQUIRED_HOST_API_VERSION 2)
_he_contract_write_config("${apiRoot}" "{}")
_he_contract_run_probe(host-api FALSE "RequiredHostApiVersion.*incompatible")

set(overrideRoot "${contractRunRoot}/policy-override")
_he_contract_write_plugin("${overrideRoot}" PolicyOverride he.contract.policy-override)
_he_contract_write_config("${overrideRoot}"
    "{ \"he.contract.policy-override\": { \"Editor\": true, \"PackagePolicy\": true } }")
_he_contract_run_probe(policy-override FALSE "PackagePolicy")

set(shippingRoot "${contractRunRoot}/shipping-always-disabled")
_he_contract_write_plugin("${shippingRoot}" ShippingAlways he.contract.shipping-always
    HOSTS Game DISTRIBUTION Runtime PACKAGE_POLICY Always)
_he_contract_write_config("${shippingRoot}"
    "{ \"he.contract.shipping-always\": { \"Game\": false } }")
_he_contract_run_probe(shipping-always-disabled FALSE
    "Shipping requires PackagePolicy Always" SHIPPING ACTIVE_HOSTS Game)

# A project cannot opt a Never/developer plugin into Shipping. The manifest
# owns its host and distribution policy; Plugins.json supplies enablement only.
set(shippingNeverRoot "${contractRunRoot}/shipping-never-requested")
_he_contract_write_plugin(
    "${shippingNeverRoot}" ShippingNever he.contract.shipping-never)
_he_contract_write_config("${shippingNeverRoot}"
    "{ \"he.contract.shipping-never\": { \"Game\": true } }")
_he_contract_run_probe(shipping-never-requested FALSE
    "enables unsupported host" SHIPPING ACTIVE_HOSTS Game)

message(STATUS
    "HEngine static plugin manifest contract PASS; fixtures: ${contractRunRoot}")
