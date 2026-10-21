add_subdirectory(Meta)
add_subdirectory(Templates)
add_subdirectory(Modules)
add_subdirectory(Plugins)
add_subdirectory(AppFramework)
add_subdirectory(Input)
add_subdirectory(Window)
add_subdirectory(Base)
add_subdirectory(Log)
add_subdirectory(Math)

RUNTIME_ADD_SOURCE(
    Timer.h Timestep.h
    SimulationClock.h SimulationClock.cpp
    FrameStageGraph.h
    TaskScheduler.h TaskScheduler.cpp
    PresentationClock.h PresentationClock.cpp
    UUID.h UUID.cpp
    Buffer.h
)
