add_library(miniaudio STATIC miniaudio.c)

target_include_directories(miniaudio
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
)

set_source_files_properties(miniaudio.c PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
)

set_target_properties(miniaudio PROPERTIES
    FOLDER "ThirdParty/miniaudio"
)
