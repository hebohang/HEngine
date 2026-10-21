# Imported before the legacy x64 vendor wrappers; sources stay outside ThirdParty.
if(HE_WITH_EDITOR)
    find_package(embree 4.4 REQUIRED CONFIG PATHS "${HE_WINDOWS_ARM64_ROOT}/embree" NO_DEFAULT_PATH)
endif()
