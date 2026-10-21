#include "Hepch.h"
#include "ReflectedComponents.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"

#include <pybind11/embed.h>
#include <pybind11/stl.h>
#define HE_PYTHON_BINDINGS_ONLY 1
#include "_generated/reflection/all_reflection.h"
#undef HE_PYTHON_BINDINGS_ONLY

namespace HEngine
{
    namespace py = pybind11;
    template<Component... C>
    static void BindingComponents(py::module_& HEngineModule)
    {
        ([&]()
            {
                BindPythonComponent(static_cast<C*>(nullptr), HEngineModule);
            }(), ...);
    }

    template<Component... C>
    static void BindingComponents(ComponentGroup<C...>, py::module_& HEngineModule)
    {
        BindingComponents<C...>(HEngineModule);
    }

    static void BindingComponents(py::module_& HEngineModule)
    {
        BindingComponents(ComponentsPyBinding{}, HEngineModule);
    }


    void BindReflectedPythonComponents(pybind11::module_& module) { BindingComponents(module); }
}
