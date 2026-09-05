#pragma once

#include "Runtime/Core/Templates/PublicSingleton.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include <pybind11/embed.h>
#include <glm/glm.hpp>

namespace HEngine
{
    class UUID;

    class ScriptEngine final : public PublicSingleton<ScriptEngine>
    {
    public:
        ScriptEngine() = default;
        ~ScriptEngine() {}

        void Init();
        void Shutdown();

        // scriptPath example: Scripts.MoveRight.py
        static bool RegisterModule(const UUID& uuid, const std::string& scriptPath);
        [[nodiscard]] static std::string GetLastRegistrationError(const UUID& uuid);
        [[nodiscard]] static bool IsModuleRegistered(const UUID& uuid);
        [[nodiscard]] static bool IsClassRegistered(const UUID& uuid);
        [[nodiscard]] static pybind11::module_& GetPyModule(const UUID& uuid);
        [[nodiscard]] static std::string GetPyClassName(const UUID& uuid);
        [[nodiscard]] static pybind11::object& GetPyClass(const UUID& uuid);
        static void ReloadModule(const UUID& uuid);
        static void ReloadModuleAndClass(const UUID& uuid);
        static void UnregisterModule(const UUID& uuid);

    public:
        [[nodiscard]] static std::string GetPyObjType(const pybind11::handle& pyObj);

        template<typename T>
        static T PyObjToCpp(const pybind11::handle& pyObj);
    };

    template<typename T>
    inline T ScriptEngine::PyObjToCpp(const pybind11::handle& pyObj)
    {
        if (GetPyObjType(pyObj) == "int")
            return pyObj.cast<int>();
        else if (GetPyObjType(pyObj) == "float")
            return pyObj.cast<float>();
        else if (GetPyObjType(pyObj) == "bool")
            return pyObj.cast<bool>();
        else if (GetPyObjType(pyObj) == "str")
            return pyObj.cast<std::string>();
        else if (GetPyObjType(pyObj) == "HEngine.vec2")
            return pyObj.cast<glm::vec2>();
        else if (GetPyObjType(pyObj) == "HEngine.vec3")
            return pyObj.cast<glm::vec3>();
        else if (GetPyObjType(pyObj) == "HEngine.vec4")
            return pyObj.cast<glm::vec4>();
        else if (GetPyObjType(pyObj) == "HEngine.TransformComponent")
            return pyObj.cast<TransformComponent>();
        HE_CORE_ASSERT(false, "We do not support this type!");
        return false;
    }
}
