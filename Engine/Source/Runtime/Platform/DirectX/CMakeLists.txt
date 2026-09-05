add_subdirectory(DX12)

# DX12 contributes its source list to this directory. Forward the accumulated
# list once more so Runtime/Platform can propagate it to HEngineRuntime.
RUNTIME_ADD_SOURCE()
