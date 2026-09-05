#pragma once
#include "Precompiled.h"

class Class;
class Global;
class Function;
class Enum;

struct SchemaMoudle
{
    std::string name;

    std::string sourceFile;
    std::string logicalSource;
    std::string sourceInclude;
    std::string outputIdentity;

    std::vector<std::shared_ptr<Class>> classes;
};
