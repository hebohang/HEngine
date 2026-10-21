#include "Hepch.h"
#include "CoreBinding.hpp"

namespace HEngine
{
    // Referenced by the runtime so static-library linking keeps the embedded
    // module's registration constructor, including in shipping builds.
    void EnsurePythonModuleLinked() {}
}
