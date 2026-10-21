#pragma once

#include <pybind11/pybind11.h>

namespace HEngine
{
    void BindReflectedPythonComponents(pybind11::module_& module);
    void EnsurePythonModuleLinked();
}
