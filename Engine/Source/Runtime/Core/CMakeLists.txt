add_subdirectory(AppFramework)
add_subdirectory(Base)
add_subdirectory(Log)
add_subdirectory(Math)

RUNTIME_ADD_SOURCE(
    Timer.h Timestep.h
    UUID.h UUID.cpp
    Window.h
)
