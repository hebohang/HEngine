#pragma once
#include <filesystem>
#include <optional>
#include "Runtime/Scripting/ScriptCore.h"
#include <set>
namespace HEngine
{
    struct ScriptCompileCompletion { std::filesystem::path Directory; bool RestartPlay = false; };
    std::optional<ScriptCompileCompletion> PollScriptCompilation();
    void StopScriptCompilation();
    class ScriptComponent;
    class Entity;
    // Editor-thread-only extension. Owners must unregister before unloading their plugin.
    // The drawer edits a value draft; returning true participates in the normal validation/Undo transaction.
    using ScriptFieldDrawer = std::function<bool(ScriptValue&, const ScriptField&, Entity)>;
    void RegisterScriptFieldDrawer(std::string stableType, ScriptFieldDrawer drawer);
    void UnregisterScriptFieldDrawer(const std::string& stableType);
    bool DrawScriptInspector(ScriptComponent& component, Entity owner, std::span<const ScriptFields> selection = {}, std::set<std::string>* editedFields = nullptr, bool* resetPerformed = nullptr);
}
