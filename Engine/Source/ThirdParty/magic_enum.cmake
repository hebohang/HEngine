set(MagicEnumSourceDir ${ThirdPartyDir}/magic_enum)

add_library(magic_enum INTERFACE "${MagicEnumSourceDir}/include/magic_enum.hpp")
target_include_directories(magic_enum INTERFACE "${MagicEnumSourceDir}/include")
