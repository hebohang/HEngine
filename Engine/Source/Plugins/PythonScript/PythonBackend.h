#pragma once

#include "Runtime/Scripting/ScriptCore.h"

#include <memory>

namespace HEngine
{
    class PythonBackend final : public IScriptBackend, public IScriptPhysicsBackend
    {
    public:
        PythonBackend();
        ~PythonBackend() override;
        const ScriptBackendDescriptor& Descriptor() const override;
        std::span<const ScriptType> Types() const override;
        ScriptInstanceHandle Create(const std::string&, ScriptEntityHandle) override;
        void Write(ScriptInstanceHandle, const std::string&, const ScriptValue&) override;
        ScriptValue Read(ScriptInstanceHandle, const std::string&) override;
        void Invoke(ScriptInstanceHandle, ScriptCallback, double) override;
        void Contact(ScriptInstanceHandle, const ScriptContactEvent&) override;
        void Release(ScriptInstanceHandle) noexcept override;
        static bool HasLiveInstances();
    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
