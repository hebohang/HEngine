# Engine-owned dependency targets. Assimp is an optional consumer of zlib.
if(NOT TARGET zlibstatic)
    set(zlibSources adler32 compress crc32 deflate gzclose gzlib gzread gzwrite
        infback inffast inflate inftrees trees uncompr zutil)
    list(TRANSFORM zlibSources PREPEND "zlib/")
    list(TRANSFORM zlibSources APPEND ".c")
    add_library(zlibstatic STATIC ${zlibSources})
    target_include_directories(zlibstatic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/zlib")
    target_compile_definitions(zlibstatic PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_library(HEArchive STATIC zip/src/zip.c unzip/ioapi.c unzip/unzip.c)
target_include_directories(HEArchive PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/zip/src" "${CMAKE_CURRENT_SOURCE_DIR}/unzip")
target_compile_definitions(HEArchive PRIVATE MINIZ_NO_TIME _CRT_SECURE_NO_WARNINGS)
target_link_libraries(HEArchive PUBLIC zlibstatic)
set_target_properties(HEArchive zlibstatic PROPERTIES FOLDER "ThirdParty/Compression")
