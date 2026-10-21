#pragma once

#include "Runtime/Scripting/ScriptCore.h"
#if !defined(HE_SCRIPT_CSHARP_WASM) && !defined(__ANDROID__)
#include "Runtime/Platform/Scripting/CoreClrHost.h"
#endif
#include <ScriptApi.h>

#include <set>

namespace HEngine
{
    class CSharpBackend final : public IScriptBackend, public IScriptFieldSnapshot, public IScriptFrameScheduler, public IScriptPhysicsBackend
#if WITH_EDITOR
        , public IScriptCompiler, public IScriptReloader, public IScriptEditorFields
#endif
    {
    public:
        const ScriptBackendDescriptor& Descriptor() const override { return mDescriptor; }
        std::span<const ScriptType> Types() const override;
#if WITH_EDITOR
        std::optional<ScriptCompileCommand> CompileCommand() const override;
        void Reload(const std::filesystem::path& directory = {}) override;
        ScriptFields EditFields(const std::string& type, const ScriptFields& fields, bool reset) override;
#endif
        ScriptInstanceHandle Create(const std::string& type, ScriptEntityHandle entity) override;
        void Write(ScriptInstanceHandle instance, const std::string& field, const ScriptValue& value) override;
        ScriptValue Read(ScriptInstanceHandle instance, const std::string& field) override;
        ScriptFields ReadFields(ScriptInstanceHandle instance) override;
        void BeginFrame(double seconds, bool paused) override;
        void Invoke(ScriptInstanceHandle instance, ScriptCallback callback, double seconds) override;
        void Contact(ScriptInstanceHandle instance, const ScriptContactEvent& contact) override;
        void Release(ScriptInstanceHandle instance) noexcept override;
        void Shutdown();
        std::uint64_t Collect();
        std::size_t LiveInstanceCount() const noexcept { return mInstances.size(); }

    private:
        using Entry = int(*)(ScriptAbi::Request*);
        void EnsureLoaded() const;
        std::string Call(ScriptAbi::Request& request, const std::string& name = {}, const std::string& input = {}) const;
#if defined(HE_SCRIPT_CSHARP_WASM)
        ScriptBackendDescriptor mDescriptor{"he.script.csharp", 1, {ScriptPlatform::Web, ScriptPlatform::WeChat}, {"Microsoft.NETCore.App.Runtime.Mono.browser-wasm/10.0.10"}, false, false, ScriptRuntimeLoading::HostProvided};
#elif defined(__ANDROID__)
        ScriptBackendDescriptor mDescriptor{"he.script.csharp", 1, {ScriptPlatform::Android}, {"Microsoft.Android"}, false, false, ScriptRuntimeLoading::HostProvided};
#else
        ScriptBackendDescriptor mDescriptor{"he.script.csharp", 1, {ScriptPlatform::Windows}, {"Microsoft.NETCore.App/10.0.10"},
#if WITH_EDITOR
            true,
#else
            false,
#endif
            false, ScriptRuntimeLoading::BackendHosted};
        mutable std::unique_ptr<CoreClrHost> mHost;
#endif
        mutable Entry mEntry = nullptr;
        mutable bool mInitialized = false;
        mutable std::vector<ScriptType> mTypes;
        mutable std::vector<char> mOutput = std::vector<char>(1024 * 1024);
        std::set<ScriptInstanceHandle> mInstances;
    };
}
