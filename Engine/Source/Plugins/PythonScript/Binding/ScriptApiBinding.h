#pragma once

#include <cstdint>

namespace pybind11 { class module_; }
namespace HEngine
{
    // The Python wrapper owns a session handle, never an Entity/Level pointer.
    struct PythonScriptEntity { std::uint64_t Handle = 0; };
    class PythonScriptContext final
    {
    public:
        explicit PythonScriptContext(std::uint64_t entity);
        ~PythonScriptContext();
        PythonScriptContext(const PythonScriptContext&) = delete;
        PythonScriptContext& operator=(const PythonScriptContext&) = delete;
    private:
        std::uint64_t mPrevious;
    };
    void ScriptApiBinding(pybind11::module_& module);
}
