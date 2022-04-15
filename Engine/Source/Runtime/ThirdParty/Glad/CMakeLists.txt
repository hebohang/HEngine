cmake_minimum_required(VERSION 3.5)
project(GLAD)

file(GLOB GladSrc 
"include/glad/glad.h"
"include/KHR/khrplatform.h"
"src/glad.c"
)

add_library(GLAD STATIC ${GladSrc})

target_include_directories(GLAD PUBLIC "include")
