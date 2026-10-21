#include "WasmScriptServices.h"
#include "ScriptPlatformServices.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/Scripting/ScriptEngineApi.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <emscripten.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace HEngine
{
    std::string ScriptPlatformServices::Execute(std::uint64_t, const std::string& name, const std::string&)
    {
        throw std::invalid_argument("This Wasm scene host does not provide service: " + name);
    }
    namespace
    {
        ScriptInputSnapshot sInput;
        int sWidth = 1, sHeight = 1;
        EM_JS(int, PlatformSave, (int write, const char* key, const char* value, int length, char* output, int capacity), {
            try {
                if (!Module.hePlatform) throw new Error('Wasm platform storage adapter is missing.');
                const name = UTF8ToString(key);
                const decoder = Module.heScriptDecoder || (Module.heScriptDecoder = new TextDecoder('utf-8', {fatal:true}));
                let text = "";
                if (write) Module.hePlatform.writeSave(name, decoder.decode(HEAPU8.subarray(value, value + length)));
                else text = Module.hePlatform.readSave(name);
                const encoder = Module.heScriptEncoder || (Module.heScriptEncoder = new TextEncoder());
                const bytes = encoder.encode(text);
                if (bytes.length >= capacity) throw new Error('Script save exceeds the native buffer bound.');
                HEAPU8.set(bytes, output); HEAPU8[output + bytes.length] = 0;
                return bytes.length;
            } catch (error) { stringToUTF8(String(error.stack || error), output, capacity); return -1; }
        });
        std::string Save(bool write, const std::string& key, const std::string& value)
        {
            if (key.empty() || key.size() > 128 || Utils::IsWindowsReservedPathComponent(key) ||
                !std::all_of(key.begin(), key.end(), [](unsigned char c) {
                    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
                })) throw std::invalid_argument("Script save key must contain 1-128 ASCII letters, digits, '-' or '_'.");
            if (value.size() > 1024 * 1024) throw std::invalid_argument("Script save exceeds 1 MiB.");
            std::vector<char> output(1024 * 1024 + 1);
            const auto size = PlatformSave(write, key.c_str(), value.data(), static_cast<int>(value.size()), output.data(), static_cast<int>(output.size()));
            if (size < 0) throw std::runtime_error(output.data());
            return std::string(output.data(), static_cast<std::size_t>(size));
        }
    }
    void SetWasmScriptViewport(int width, int height) { sWidth = std::max(width, 1); sHeight = std::max(height, 1); }
    void SetWasmScriptInput(float moveX, float moveY, float pixelX, float pixelY, bool pressed)
    {
        if (!std::isfinite(moveX) || !std::isfinite(moveY) || !std::isfinite(pixelX) || !std::isfinite(pixelY)) return;
        if (pressed && !sInput.Pressed) ++sInput.PressSequence;
        sInput.MoveX = std::clamp(moveX, -1.0f, 1.0f); sInput.MoveY = std::clamp(moveY, -1.0f, 1.0f);
        sInput.PointerX = pixelX; sInput.PointerY = pixelY; sInput.Pressed = pressed;
    }
    ScriptInputSnapshot ScriptPlatformServices::ReadInput(Entity context)
    {
        auto input = sInput;
        float size = 10;
        glm::vec3 origin{};
        if (context)
        {
            auto cameras = context.GetLevel()->mRegistry.view<CameraComponent, TransformComponent>();
            for (const auto raw : cameras)
            {
                const auto& camera = cameras.get<CameraComponent>(raw);
                if (!camera.Primary) continue;
                size = camera.Camera.GetOrthographicSize();
                origin = cameras.get<TransformComponent>(raw).Translation; break;
            }
        }
        input.PointerX = (sInput.PointerX / sWidth - 0.5f) * size * sWidth / sHeight + origin.x;
        input.PointerY = (0.5f - sInput.PointerY / sHeight) * size + origin.y;
        return input;
    }
    void ScriptPlatformServices::ObserveEvent(Event&) { throw std::logic_error("The Wasm host must feed input through SetWasmScriptInput."); }
    std::string ScriptPlatformServices::ReadSave(const std::string& key) { return Save(false, key, {}); }
    void ScriptPlatformServices::WriteSave(const std::string& key, const std::string& value) { (void)Save(true, key, value); }
}

extern "C"
{
    EMSCRIPTEN_KEEPALIVE void he_script_input(float moveX, float moveY, float pixelX, float pixelY, int pressed)
    {
        HEngine::SetWasmScriptInput(moveX, moveY, pixelX, pixelY, pressed != 0);
    }
    // JS owns these engine allocations and copies through a .NET MemoryView.
    // Negative return values encode -(UTF-8 diagnostic length + 1).
    EMSCRIPTEN_KEEPALIVE int he_script_call(int operation, double entity, int flags, double sequence,
        const char* name, int nameLength, const char* input, int inputLength, double* data, char* output, int capacity)
    {
        if (!data || !output || capacity < 1 || !std::isfinite(entity) || entity < 0 || entity > 9007199254740991.0 || std::trunc(entity) != entity ||
            !std::isfinite(sequence) || sequence < 0 || sequence > 9007199254740991.0 || std::trunc(sequence) != sequence) return -1;
        HEngine::ScriptAbi::Request r{};
        r.Version = HEngine::ScriptAbi::Version; r.Operation = operation; r.Entity = static_cast<std::uint64_t>(entity);
        r.Flags = flags; r.Result = static_cast<std::uint64_t>(sequence);
        r.Name = reinterpret_cast<std::uintptr_t>(name); r.NameLength = nameLength;
        r.Input = reinterpret_cast<std::uintptr_t>(input); r.InputLength = inputLength;
        r.Output = reinterpret_cast<std::uintptr_t>(output); r.OutputCapacity = capacity;
        for (int i = 0; i < 16; ++i) r.Data[i] = static_cast<float>(data[i]);
        const auto status = HEngine::ScriptEngineApi::Call(&r);
        for (int i = 0; i < 16; ++i) data[i] = r.Data[i];
        data[16] = r.Flags; data[17] = static_cast<double>(r.Result);
        return status == 0 ? r.OutputLength : -(r.OutputLength + 1);
    }
}
