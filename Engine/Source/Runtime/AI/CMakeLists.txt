add_subdirectory(BehaviorTree)
add_subdirectory(Blackboard)
add_subdirectory(Navigation)
add_subdirectory(Nodes)

RUNTIME_ADD_SOURCE(
    AIFactory.h AIFactory.cpp
)
