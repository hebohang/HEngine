set(EmbreeSourceDir ${ThirdPartyDir}/embree)

set(EmbreeLibFile
    "${EmbreeSourceDir}/lib/x64/embree4.lib"
    "${EmbreeSourceDir}/lib/x64/tbb.lib"
)

set(EmbreeDllFile
    "${EmbreeSourceDir}/dll/x64/embree4.dll"
    "${EmbreeSourceDir}/dll/x64/tbb12.dll"
    "${EmbreeSourceDir}/dll/x64/tbbmalloc.dll"
)

add_library(embree INTERFACE)
target_include_directories(embree INTERFACE "${EmbreeSourceDir}/include")
target_link_libraries(embree INTERFACE ${EmbreeLibFile})

if (WIN32)
    file(COPY ${EmbreeDllFile} DESTINATION ${BINARY_ROOT_DIR})
endif()
