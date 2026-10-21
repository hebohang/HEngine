#pragma once
#include <ScriptApi.h>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>
#if WITH_EDITOR
#include <filesystem>
#include <optional>
#endif

namespace HEngine
{
    // Values cross language boundaries by copy. References use persisted IDs,
    // never native object addresses. Runtime handles are scoped to one session.
    struct ScriptEntityReference { std::string Id; };
    struct ScriptResourceReference { std::string Id; };
    struct ScriptAggregate { std::string Kind, Type, Json; };
    struct ScriptEnum { std::string Type; std::int64_t Value = 0; };
    using ScriptValue = std::variant<bool, std::int64_t, double, std::string,
        std::array<float, 2>, std::array<float, 3>, std::array<float, 4>,
        ScriptEntityReference, ScriptResourceReference, ScriptEnum, ScriptAggregate>;
    using ScriptFields = std::map<std::string, ScriptValue>;

    struct ScriptField
    {
        std::string Id;
        std::string Name;
        ScriptValue Default;
        std::vector<std::string> FormerIds;
        std::string Tooltip;
        std::string Header;
        std::string ResourceType;
        std::vector<std::pair<std::string, std::int64_t>> EnumValues;
        double Space = 0;
        double Min = -1.7976931348623157e308;
        double Max = 1.7976931348623157e308;
        bool HasRange = false;
        bool ReadOnly = false;
        bool HideInInspector = false;
        bool Multiline = false;
        bool Color = false;
        bool AllowNull = true;
    };

    enum class ScriptPlatform { Windows, Web, WeChat, Android };
    // Existing ABI callback numbers are immutable. Append new phases only.
    using ScriptCallback = ScriptAbi::ScriptLifecycle;
    enum class ScriptErrorKind
    {
        MissingBackend, UnsupportedPlatform, MissingType, InvalidEntity,
        InvalidField, RuntimeFailure, Busy
    };
    struct ScriptError
    {
        ScriptErrorKind Kind = ScriptErrorKind::RuntimeFailure;
        std::string Message;
        std::string Stack;
    };
    // Native C++ boundary only. Language exceptions are caught inside adapters;
    // diagnostics own their UTF-8 text and never retain runtime objects.
    class ScriptException final : public std::runtime_error
    {
    public:
        explicit ScriptException(const ScriptError& error)
            : std::runtime_error(error.Stack.empty() ? error.Message : error.Message + "\n" + error.Stack), mError(error) {}
        const ScriptError& Error() const noexcept { return mError; }
    private:
        ScriptError mError;
    };
    enum class ScriptRuntimeLoading { None, LinkedInterpreter, BackendHosted, HostProvided };
    struct ScriptBackendDescriptor
    {
        std::string Id;
        std::uint32_t ApiVersion = 1;
        std::vector<ScriptPlatform> Platforms;
        std::vector<std::string> Dependencies;
        bool CanReload = false;
        bool CanExecuteAsync = false;
        ScriptRuntimeLoading RuntimeLoading = ScriptRuntimeLoading::None;
    };
    struct ScriptType
    {
        std::string Id;
        std::string Resource;
        std::vector<std::string> Dependencies;
        std::vector<ScriptField> Fields;
        int ExecutionOrder = 0;
        std::string ModuleId;
    };

    using ScriptInstanceHandle = std::uint64_t;
    using ScriptEntityHandle = std::uint64_t;
    struct ScriptContactEvent
    {
        ScriptEntityHandle Other = 0;
        std::uint32_t Phase = 0; // Enter=0, Stay=1, Exit=2.
        bool Trigger = false;
        std::array<float, 3> Point{};
        std::array<float, 3> Normal{};
    };
    class IScriptPhysicsBackend
    {
    public:
        virtual ~IScriptPhysicsBackend() = default;
        virtual void Contact(ScriptInstanceHandle instance, const ScriptContactEvent& contact) = 0;
    };
#if WITH_EDITOR
    struct ScriptCompileCommand
    {
        std::filesystem::path Executable;
        std::vector<std::string> Arguments;
    };
#endif
    class IScriptBackend
    {
    public:
        virtual ~IScriptBackend() = default;
        virtual const ScriptBackendDescriptor& Descriptor() const = 0;
        virtual std::span<const ScriptType> Types() const = 0;
        virtual ScriptInstanceHandle Create(const std::string& type, ScriptEntityHandle entity) = 0;
        virtual void Write(ScriptInstanceHandle instance, const std::string& field, const ScriptValue& value) = 0;
        virtual ScriptValue Read(ScriptInstanceHandle instance, const std::string& field) = 0;
        virtual void Invoke(ScriptInstanceHandle instance, ScriptCallback callback, double seconds) = 0;
        // Release is required even when construction callbacks or Destroy fail.
        virtual void Release(ScriptInstanceHandle instance) noexcept = 0;
    };

#if WITH_EDITOR
    // Keep Editor capabilities out of IScriptBackend's ABI: ScriptCore is built
    // without Editor definitions even when linked into an Editor executable.
    class IScriptCompiler
    {
    public:
        virtual ~IScriptCompiler() = default;
        // Results are staged; running code follows the declared reload workflow.
        virtual std::optional<ScriptCompileCommand> CompileCommand() const = 0;
    };
    class IScriptReloader
    {
    public:
        virtual ~IScriptReloader() = default;
        // Requires all scenes to have stopped. Empty path resets the current
        // generation (including static state) for a new Play session.
        virtual void Reload(const std::filesystem::path& directory = {}) = 0;
    };
    class IScriptEditorFields
    {
    public:
        virtual ~IScriptEditorFields() = default;
        virtual ScriptFields EditFields(const std::string& type, const ScriptFields& fields, bool reset) = 0;
    };
#endif

    // Optional atomic field snapshot. Implementations return exactly the declared
    // fields by value; getters must not invoke gameplay or mutate entity lifetime.
    class IScriptFieldSnapshot
    {
    public:
        virtual ~IScriptFieldSnapshot() = default;
        virtual ScriptFields ReadFields(ScriptInstanceHandle instance) = 0;
    };

    class IScriptFrameScheduler
    {
    public:
        virtual ~IScriptFrameScheduler() = default;
        virtual void BeginFrame(double seconds, bool paused) = 0;
    };

    // Single engine thread. Reentrant Remove/SetEnabled/Stop are deferred until
    // the callback returns. Never keep registry/component references in callbacks.
    class ScriptCore final
    {
    public:
        using EntityValidator = std::function<bool(ScriptEntityHandle)>;
        using ErrorSink = std::function<void(ScriptInstanceHandle, const ScriptError&)>;
        ScriptCore(ScriptPlatform platform, EntityValidator validator, ErrorSink sink);
        ~ScriptCore();
        ScriptCore(const ScriptCore&) = delete;
        ScriptCore& operator=(const ScriptCore&) = delete;
        void Register(std::shared_ptr<IScriptBackend> backend);
        void Unregister(const std::string& backend);
        ScriptInstanceHandle Add(const std::string& backend, const std::string& type,
            ScriptEntityHandle entity, const ScriptFields& fields, bool enabled = true);
        void SetEnabled(ScriptInstanceHandle instance, bool enabled);
        void Remove(ScriptInstanceHandle instance);
        void Update(double seconds);
        void SetPaused(bool paused);
        bool IsPaused() const noexcept { return mPaused; }
        void Step(double seconds);
        void Notify(ScriptCallback callback);
        void Contact(ScriptInstanceHandle instance, const ScriptContactEvent& contact, bool singleStep = false);
        void Stop();
        ScriptFields Save(ScriptInstanceHandle instance);
        void RestoreFields(ScriptInstanceHandle instance, const ScriptFields& fields);
        std::size_t InstanceCount() const noexcept { return mInstances.size(); }
        bool Contains(ScriptInstanceHandle instance) const noexcept { return mInstances.contains(instance); }

    private:
        struct Instance
        {
            std::shared_ptr<IScriptBackend> Backend;
            ScriptType Type;
            ScriptInstanceHandle Handle = 0;
            ScriptEntityHandle Entity = 0;
            ScriptFields Preserved;
            bool Enabled = false;
            bool WantedEnabled = true;
            bool Initialized = false;
            bool Started = false;
            bool UnloadNotified = false;
            bool Faulted = false;
            bool Removing = false;
        };
        bool Call(ScriptInstanceHandle id, ScriptCallback callback, double seconds = 0);
        void Drain();
        std::vector<ScriptInstanceHandle> OrderedInstances() const;
        void RunPhase(ScriptCallback callback, double seconds);
        void Advance(double seconds);
        void Report(ScriptInstanceHandle id, ScriptErrorKind kind, const std::string& message) noexcept;
        void Report(ScriptInstanceHandle id, const std::exception& error) noexcept;
        ScriptPlatform mPlatform;
        EntityValidator mValidator;
        ErrorSink mSink;
        std::map<std::string, std::shared_ptr<IScriptBackend>> mBackends;
        std::map<ScriptInstanceHandle, Instance> mInstances;
        ScriptInstanceHandle mNextId = 1;
        unsigned mCallbackDepth = 0;
        bool mDraining = false;
        bool mStopping = false;
        bool mPaused = false;
        bool mAdvancing = false;
        double mFixedAccumulator = 0;
        static constexpr double FixedSeconds = 1.0 / 60.0;
    };
}
