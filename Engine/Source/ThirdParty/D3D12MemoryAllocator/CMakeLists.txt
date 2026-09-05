add_library(D3D12MemoryAllocator STATIC "${ThirdPartyDir}/D3D12MemoryAllocator/src/D3D12MemAlloc.cpp")
target_include_directories(D3D12MemoryAllocator PUBLIC "${ThirdPartyDir}/D3D12MemoryAllocator/include")
