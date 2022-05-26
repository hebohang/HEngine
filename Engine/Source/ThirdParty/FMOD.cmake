set(FmodSourceDir ${ThirdPartyDir}/FMOD)

file(GLOB FmodLibFile
    "${FmodSourceDir}/api/core/lib/x64/fmod_vc.lib"
    "${FmodSourceDir}/api/fsbank/lib/x64/fsbank_vc.lib"
    "${FmodSourceDir}/api/studio/lib/x64/fmodstudio_vc.lib"
)

file(GLOB FmodDllFile
    "${FmodSourceDir}/api/core/lib/x64/fmod.dll"
    "${FmodSourceDir}/api/fsbank/lib/x64/fsbank.dll"
    "${FmodSourceDir}/api/studio/lib/x64/fmodstudio.dll"
)

file(GLOB FmodIncludeDir
    "${FmodSourceDir}/api/core/inc"
    "${FmodSourceDir}/api/fsbank/inc"
    "${FmodSourceDir}/api/studio/inc"
)

add_library(FMOD_CORE SHARED IMPORTED GLOBAL)
add_library(FMOD::Core ALIAS FMOD_CORE)
# set_target_properties(FMOD_CORE PROPERTIES IMPORTED_LOCATION "${FmodSourceDir}/api/core/lib/x64/fmod.dll")
set_target_properties(FMOD_CORE PROPERTIES IMPORTED_IMPLIB "${FmodSourceDir}/api/core/lib/x64/fmod_vc.lib")
set_target_properties(FMOD_CORE PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${FmodSourceDir}/api/core/inc")

add_library(FMOD_FSBANK SHARED IMPORTED GLOBAL)
add_library(FMOD::Fsbank ALIAS FMOD_FSBANK)
# set_target_properties(FMOD_FSBANK PROPERTIES IMPORTED_LOCATION "${FmodSourceDir}/api/core/lib/x64/fmod.dll")
set_target_properties(FMOD_FSBANK PROPERTIES IMPORTED_IMPLIB "${FmodSourceDir}/api/fsbank/lib/x64/fsbank_vc.lib")
set_target_properties(FMOD_FSBANK PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${FmodSourceDir}/api/fsbank/inc")

add_library(FMOD_STUDIO SHARED IMPORTED GLOBAL)
add_library(FMOD::Studio ALIAS FMOD_STUDIO)
# set_target_properties(FMOD_STUDIO PROPERTIES IMPORTED_LOCATION "${FmodSourceDir}/api/core/lib/x64/fmod.dll")
set_target_properties(FMOD_STUDIO PROPERTIES IMPORTED_IMPLIB "${FmodSourceDir}/api/studio/lib/x64/fmodstudio_vc.lib")
set_target_properties(FMOD_STUDIO PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${FmodSourceDir}/api/studio/inc")

file(COPY ${FmodDllFile}
DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})