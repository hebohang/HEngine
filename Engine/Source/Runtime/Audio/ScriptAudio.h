#pragma once

#include "Runtime/Audio/AudioEngine.h"
#include "Runtime/Core/Meta/Json.h"

#include <string>

namespace HEngine
{
    // Shared by the scene ABI and its contract tests. No language runtime objects.
    AudioPlayParams ReadScriptAudioParams(const Json& args);
    Json ExecuteScriptAudioControl(AudioEngine& audio, const std::string& operation, const Json& args, AudioHandle handle = {});
}
