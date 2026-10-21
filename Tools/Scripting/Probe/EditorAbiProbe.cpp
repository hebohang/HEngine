#include "Runtime/Scripting/ScriptCore.h"

#include <iostream>
#include <stdexcept>

namespace
{
    // Deliberately compiled WITH_EDITOR=1 against the runtime-free library,
    // which is compiled without Editor definitions, as in the actual build.
    class Backend final : public HEngine::IScriptBackend, public HEngine::IScriptFieldSnapshot, public HEngine::IScriptCompiler
    {
    public:
        HEngine::ScriptBackendDescriptor Info{"editor.abi.fixture", 1, {HEngine::ScriptPlatform::Windows}};
        std::vector<HEngine::ScriptType> Registered{{"fixture", "", {}, {}}};
        int Callbacks = 0;
        int Snapshots = 0;
        bool Released = false;
        const HEngine::ScriptBackendDescriptor& Descriptor() const override { return Info; }
        std::span<const HEngine::ScriptType> Types() const override { return Registered; }
        std::optional<HEngine::ScriptCompileCommand> CompileCommand() const override
        { throw std::runtime_error("CompileCommand called from runtime lifecycle: ABI mismatch"); }
        HEngine::ScriptInstanceHandle Create(const std::string&, HEngine::ScriptEntityHandle) override { return 1; }
        void Write(HEngine::ScriptInstanceHandle, const std::string&, const HEngine::ScriptValue&) override
        { throw std::runtime_error("Unexpected field write"); }
        HEngine::ScriptValue Read(HEngine::ScriptInstanceHandle, const std::string&) override
        { throw std::runtime_error("Unexpected field read"); }
        HEngine::ScriptFields ReadFields(HEngine::ScriptInstanceHandle instance) override
        {
            if (instance != 1) throw std::runtime_error("Wrong snapshot handle");
            ++Snapshots;
            return {};
        }
        void Invoke(HEngine::ScriptInstanceHandle instance, HEngine::ScriptCallback, double) override
        {
            if (instance != 1) throw std::runtime_error("Wrong backend handle");
            ++Callbacks;
        }
        void Release(HEngine::ScriptInstanceHandle) noexcept override { Released = true; }
    };
}

int main()
{
    try
    {
        HEngine::ScriptCore core(HEngine::ScriptPlatform::Windows, [](auto) { return true; },
            [](auto, const auto& error) { std::cerr << error.Message << '\n'; });
        auto backend = std::make_shared<Backend>();
        core.Register(backend);
        if (!dynamic_cast<HEngine::IScriptCompiler*>(static_cast<HEngine::IScriptBackend*>(backend.get())))
            throw std::runtime_error("Editor compiler capability missing");
        const auto instance = core.Add("editor.abi.fixture", "fixture", 1, {});
        if (!instance) throw std::runtime_error("Editor backend creation failed across runtime-free boundary");
        if (!core.Save(instance).empty() || backend->Snapshots != 1)
            throw std::runtime_error("Optional snapshot dispatch failed across Editor boundary");
        core.Update(0.016);
        core.SetEnabled(instance, false);
        core.SetEnabled(instance, true);
        core.Remove(instance);
        if (backend->Callbacks != 9 || !backend->Released || core.InstanceCount())
            throw std::runtime_error("Editor/runtime lifecycle dispatch mismatch");
        std::cout << "PASS: mixed Editor/runtime-free script ABI\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
