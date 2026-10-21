# This contract is also used by standalone codegen and hectl deps.
set(HE_CODEGEN_LIBCLANG_ROOT "$ENV{HE_CODEGEN_LIBCLANG_ROOT}" CACHE PATH
    "Explicit versioned libclang SDK override; empty selects HEngine's bundled Win64 SDK.")
find_program(HE_LIBCLANG_POWERSHELL NAMES powershell.exe pwsh REQUIRED)
execute_process(COMMAND "${HE_LIBCLANG_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_LIST_DIR}/../Tools/Agent/Resolve-LibClang.ps1"
    -Root "${HE_CODEGEN_LIBCLANG_ROOT}"
    RESULT_VARIABLE heLibClangResult OUTPUT_VARIABLE heLibClangJson ERROR_VARIABLE heLibClangError)
if(NOT heLibClangResult EQUAL 0)
    message(FATAL_ERROR "libclang SDK validation failed: ${heLibClangError}")
endif()
foreach(property IN ITEMS Root Version IncludeRoot ResourceRoot)
    string(JSON HE_LIBCLANG_${property} GET "${heLibClangJson}" "${property}")
endforeach()
message(STATUS "HEngineParser libclang ${HE_LIBCLANG_Version}: ${HE_LIBCLANG_Root}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${HE_LIBCLANG_Root}/manifest.json"
    "${CMAKE_CURRENT_LIST_DIR}/../Tools/Agent/LibClang.ps1")
