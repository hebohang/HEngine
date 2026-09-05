function(NoWarning TargetName)
    target_compile_options(${TargetName} PRIVATE
         $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
              -w>
         $<$<CXX_COMPILER_ID:MSVC>:
              /W0>)
endfunction()

function(NoWarningInterface TargetName)
    target_compile_options(${TargetName} INTERFACE
         $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
              -w>
         $<$<CXX_COMPILER_ID:MSVC>:
              /W0>)
endfunction()

function(EnableWarning TargetName)
    target_compile_options(${TargetName} PRIVATE
         $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
              -Wall>
         $<$<CXX_COMPILER_ID:MSVC>:
              /W4>)
endfunction()

# refer to https://stackoverflow.com/questions/7787823/cmake-how-to-get-the-name-of-all-subdirectories-of-a-directory
MACRO(SUBDIRLIST result curdir)
    FILE(GLOB children RELATIVE ${curdir} ${curdir}/*)
    SET(dirlist "")
    FOREACH(child ${children})
        IF(IS_DIRECTORY ${curdir}/${child})
            LIST(APPEND dirlist ${child})
        ENDIF()
    ENDFOREACH()
    SET(${result} ${dirlist})
ENDMACRO()