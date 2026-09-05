RUNTIME_ADD_SOURCE(
    GamepadCodes.h Input.h KeyCodes.h MouseCodes.h
)

if(HE_BUILD_DEV_GAME AND NOT PACK_GAME)
    RUNTIME_ADD_SOURCE(SyntheticKeyboardState.cpp)
endif()
