function(NoWarning TargetName)
    target_compile_options(${TargetName} PRIVATE
         $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
              -w>
         $<$<CXX_COMPILER_ID:MSVC>:
              /W3>)
endfunction()

function(NoWarningInterface TargetName)
    target_compile_options(${TargetName} INTERFACE
         $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
              -w>
         $<$<CXX_COMPILER_ID:MSVC>:
              /W3>)
endfunction()

function(EnableWarning TargetName)
    target_compile_options(${TargetName} PRIVATE
         $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
              -Wall>
         $<$<CXX_COMPILER_ID:MSVC>:
              /W4>)
endfunction()