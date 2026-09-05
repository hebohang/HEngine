RUNTIME_ADD_SOURCE(
    AudioBinding.hpp
    CoreBinding.hpp
    EcsBinding.hpp
    InputBinding.hpp
    LogBinding.hpp
    RmlUIBinding.hpp
    RuntimeResourceWarmupBinding.hpp
    SettingsBinding.hpp
    UIBinding.hpp
    VFXBinding.hpp
)

if(HE_ENABLE_GM)
    RUNTIME_ADD_SOURCE(GMBinding.hpp GMBinding.cpp)
endif()
