#include "ScriptEngineApi.h"
#include "ScriptWorld.h"
#include "ScriptFieldsJson.h"
#include "ScriptInputContext.h"
#if !defined(__EMSCRIPTEN__)
#include "Runtime/Core/Input/InputActions.h"
#endif
#include "Runtime/Core/Log/Log.h"
#include "Runtime/EcsFramework/Component/Script/ScriptComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Component/Shape/SpriteRendererComponent.h"
#include "Runtime/Platform/Scripting/ScriptPlatformServices.h"
#include "Runtime/Profile/Profile.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace HEngine
{
    namespace
    {
        std::thread::id sThread;
        std::string Text(std::uintptr_t pointer, std::int32_t length)
        {
            if (length < 0 || length > 1024 * 1024 || (!pointer && length)) throw std::invalid_argument("Invalid UTF-8 buffer.");
            return length ? std::string(reinterpret_cast<const char*>(pointer), length) : std::string{};
        }
        void Output(ScriptAbi::Request& request, const std::string& text)
        {
            if (request.OutputCapacity < 0 || text.size() > static_cast<std::size_t>(request.OutputCapacity) || (!request.Output && !text.empty()))
                throw std::invalid_argument("Script output buffer is too small.");
            if (!text.empty()) std::memcpy(reinterpret_cast<void*>(request.Output), text.data(), text.size());
            request.OutputLength = static_cast<std::int32_t>(text.size());
        }
        Entity RequireEntity(std::uint64_t handle)
        {
            auto entity = ScriptWorld::Resolve(handle);
            if (!entity) throw std::invalid_argument("Script entity handle is stale or pending destruction.");
            return entity;
        }
    }

    void ScriptEngineApi::BindThread()
    {
        if (sThread != std::thread::id{} && sThread != std::this_thread::get_id())
            throw std::logic_error("Script API already belongs to another engine thread.");
        sThread = std::this_thread::get_id();
    }

    int ScriptEngineApi::Call(ScriptAbi::Request* pointer) noexcept
    {
        HE_PROFILE_SECTION("Script.EngineApi");
        if (!pointer) return -1;
        auto& r = *pointer;
        r.OutputLength = 0;
        try
        {
            if (r.Version != ScriptAbi::Version || sThread != std::this_thread::get_id())
                throw std::invalid_argument("Script ABI version or calling thread differs.");
            using Op = ScriptAbi::EngineOperation;
            switch (static_cast<Op>(r.Operation))
            {
            case Op::EntityValid: r.Result = bool(ScriptWorld::Resolve(r.Entity)); break;
            case Op::PrefabRequest:
                r.Result = ScriptWorld::PrefabRequest(r.Entity, r.Instance, r.Flags, Text(r.Name, r.NameLength));
                break;
            case Op::ComponentAccess:
            {
                auto entity = RequireEntity(r.Entity);
                const auto kind = r.Flags & 255u;
                const auto operation = r.Flags >> 8;
                if ((kind != 1 && kind != 2) || operation > 2) throw std::invalid_argument("Invalid component binding operation.");
                if (operation == 1 && kind == 2 && !entity.HasComponent<SpriteRendererComponent>()) entity.AddComponent<SpriteRendererComponent>();
                if (operation == 2)
                {
                    if (kind == 1) throw std::invalid_argument("Transform is required and cannot be removed.");
                    r.Result = entity.HasComponent<SpriteRendererComponent>();
                    if (r.Result) entity.RemoveComponent<SpriteRendererComponent>();
                }
                else r.Result = ScriptWorld::ComponentGeneration(r.Entity, kind);
                break;
            }
            case Op::CreateEntity:
            {
#if defined(__EMSCRIPTEN__)
                throw std::invalid_argument("Dynamic hierarchy creation is unavailable in the constrained Wasm scene host.");
#else
                auto context = RequireEntity(r.Entity);
                Entity created;
                if (r.Result)
                {
                    auto source = RequireEntity(r.Result);
                    if (source.GetLevel() != context.GetLevel()) throw std::invalid_argument("Cannot instantiate across script scenes.");
                    auto* level = context.GetLevel();
                    std::vector<Entity> copies;
                    std::map<std::string, std::string> ids;
                    try
                    {
                        const auto duplicate = [&](const auto& self, Entity original, std::size_t depth) -> Entity {
                            if (depth > 64 || copies.size() >= 4096 || ids.contains(original.UUID().str()))
                                throw std::invalid_argument("Instantiation exceeds 4096 entities / depth 64 or contains a hierarchy cycle.");
                            auto copy = level->DuplicateEntity(original, false);
                            copies.push_back(copy); ids.emplace(original.UUID().str(), copy.UUID().str());
                            for (auto child : level->GetChildren(original))
                            {
                                const auto childCopy = self(self, child, depth + 1);
                                (void)level->SetEntityParent(childCopy, copy, false);
                            }
                            return copy;
                        };
                        created = duplicate(duplicate, source, 0);
                        for (auto copy : copies)
                            if (copy.HasComponent<ScriptComponent>())
                            {
                                auto& script = copy.GetComponent<ScriptComponent>();
                                script.Fields = RemapScriptEntityReferences(script.Fields, ids);
                            }
                    }
                    catch (...)
                    {
                        for (auto copy = copies.rbegin(); copy != copies.rend(); ++copy) if (*copy) level->DestroyEntity(*copy);
                        throw;
                    }
                }
                else created = context.GetLevel()->CreateEntity(Text(r.Name, r.NameLength));
                r.Result = ScriptWorld::BindInContext(r.Entity, created);
                break;
#endif
            }
            case Op::Hierarchy:
            {
#if defined(__EMSCRIPTEN__)
                throw std::invalid_argument("Hierarchy mutation is unavailable in the constrained Wasm scene host.");
#else
                auto entity = RequireEntity(r.Entity);
                auto* level = entity.GetLevel();
                if (r.Flags == 0) r.Result = ScriptWorld::BindInContext(r.Entity, level->GetParent(entity));
                else if (r.Flags == 1)
                {
                    const auto parent = r.Result ? RequireEntity(r.Result) : Entity{};
                    if ((parent && parent.GetLevel() != level) || !level->SetEntityParent(entity, parent, true))
                        throw std::invalid_argument("Invalid parent, cross-scene reference or hierarchy cycle.");
                }
                else if (r.Flags == 2)
                {
                    const auto children = level->GetChildren(entity);
                    if (children.size() > 4096) throw std::invalid_argument("Hierarchy query exceeds 4096 children.");
                    auto values = nlohmann::json::array();
                    for (auto child : children) values.push_back(ScriptWorld::BindInContext(r.Entity, child));
                    Output(r, values.dump());
                }
                else throw std::invalid_argument("Invalid hierarchy operation.");
                break;
#endif
            }
            case Op::ReadEntity:
            {
                auto entity = RequireEntity(r.Entity);
                if (!entity.HasComponent<TransformComponent>()) throw std::invalid_argument("Entity has no Transform.");
                const auto& transform = entity.GetComponent<TransformComponent>();
                std::copy_n(&transform.Translation.x, 3, r.Data);
                std::copy_n(&transform.Rotation.x, 3, r.Data + 3);
                std::copy_n(&transform.Scale.x, 3, r.Data + 6);
                r.Flags = 1;
                if (entity.HasComponent<SpriteRendererComponent>())
                {
                    std::copy_n(&entity.GetComponent<SpriteRendererComponent>().Color.x, 4, r.Data + 9);
                    r.Flags |= 2;
                }
                break;
            }
            case Op::WriteEntity:
            {
                auto entity = RequireEntity(r.Entity);
                if ((r.Flags & ~3u) || !std::all_of(r.Data, r.Data + 13, [](float value) { return std::isfinite(value); }))
                    throw std::invalid_argument("Invalid entity write mask or non-finite component.");
                if ((r.Flags & 1) && !entity.HasComponent<TransformComponent>()) throw std::invalid_argument("Entity has no Transform.");
                if ((r.Flags & 2) && !entity.HasComponent<SpriteRendererComponent>()) throw std::invalid_argument("Entity has no Sprite.");
                if (r.Flags & 1)
                {
                    auto& transform = entity.GetComponent<TransformComponent>();
                    transform.Translation = {r.Data[0], r.Data[1], r.Data[2]};
                    transform.Rotation = {r.Data[3], r.Data[4], r.Data[5]};
                    transform.Scale = {r.Data[6], r.Data[7], r.Data[8]};
                }
                if (r.Flags & 2) entity.GetComponent<SpriteRendererComponent>().Color = {r.Data[9], r.Data[10], r.Data[11], r.Data[12]};
                break;
            }
            case Op::ReadInput:
            {
                const auto input = ScriptPlatformServices::ReadInput(ScriptWorld::Resolve(r.Entity));
                r.Data[0] = input.MoveX; r.Data[1] = input.MoveY; r.Data[2] = input.PointerX; r.Data[3] = input.PointerY;
                r.Flags = input.Pressed ? 1 : 0; r.Result = input.PressSequence;
                break;
            }
            case Op::InputAction:
            {
#if defined(__EMSCRIPTEN__)
                throw std::invalid_argument("Input actions require the desktop host.");
#else
                (void)RequireEntity(r.Entity);
                auto& actions = InputActions::Get();
                switch (r.Flags)
                {
                case 0:
                {
                    auto state = actions.Read(Text(r.Name, r.NameLength));
                    const auto& region = ScriptInputContext::Region;
                    if (!region.Enabled || !region.KeyboardEnabled) state = {};
                    r.Data[0] = state.Value.x; r.Data[1] = state.Value.y;
                    r.Data[2] = static_cast<float>(state.PressCount);
                    r.Result = (state.Pressed ? 1u : 0u) | (state.Released ? 2u : 0u) | (state.Held ? 4u : 0u) |
                        (state.HoldPerformed ? 8u : 0u) | (state.DoubleTapped ? 16u : 0u);
                    break;
                }
                case 1: actions.SetContext(Text(r.Name, r.NameLength)); break;
                case 2:
                    if (r.Instance > 32 || !std::isfinite(r.Data[0]) || r.Data[0] < 0 || r.Data[0] > 348 ||
                        std::floor(r.Data[0]) != r.Data[0] || !std::isfinite(r.Data[1]) ||
                        r.Data[1] < 0 || r.Data[1] > 1 || std::floor(r.Data[1]) != r.Data[1])
                        throw std::invalid_argument("Invalid input binding index or code.");
                    actions.Rebind(Text(r.Name, r.NameLength), static_cast<size_t>(r.Instance),
                        {Text(r.Input, r.InputLength), static_cast<int>(r.Data[0]), static_cast<int>(r.Data[1]), r.Data[2]});
                    break;
                case 3: Output(r, actions.ExportMap()); break;
                case 4: actions.ImportMap(Text(r.Input, r.InputLength)); break;
                default: throw std::invalid_argument("Unknown input action operation.");
                }
#endif
                break;
            }
            case Op::DestroyEntity:
                if (!ScriptWorld::RequestDestroy(r.Entity)) throw std::invalid_argument("Cannot destroy a stale script entity.");
                break;
            case Op::FindEntity: r.Result = ScriptWorld::Find(r.Entity, Text(r.Name, r.NameLength)); break;
            case Op::SetEnabled:
            {
                auto entity = RequireEntity(r.Entity);
                if (!entity.HasComponent<ScriptComponent>()) throw std::invalid_argument("Entity has no ScriptComponent.");
                entity.GetComponent<ScriptComponent>().Enabled = r.Flags != 0;
                break;
            }
            case Op::ReadTexture:
            case Op::WriteTexture:
            {
                auto entity = RequireEntity(r.Entity);
                if (!entity.HasComponent<SpriteRendererComponent>()) throw std::invalid_argument("Entity has no Sprite.");
                auto& sprite = entity.GetComponent<SpriteRendererComponent>();
                if (static_cast<Op>(r.Operation) == Op::ReadTexture) Output(r, sprite.Texture);
                else
                {
                    std::string normalized;
                    const auto result = Utils::TryNormalizeProjectRelativePath(Text(r.Name, r.NameLength), normalized);
                    if (!result) throw std::invalid_argument(result.Message);
                    sprite.Texture = std::move(normalized);
                }
                break;
            }
            case Op::Log: HE_CORE_INFO("{0}", Text(r.Input, r.InputLength)); break;
            case Op::PlatformService:
                Output(r, ScriptPlatformServices::Execute(r.Entity, Text(r.Name, r.NameLength), Text(r.Input, r.InputLength)));
                break;
            case Op::ReadSave: Output(r, ScriptPlatformServices::ReadSave(Text(r.Name, r.NameLength))); break;
            case Op::WriteSave: ScriptPlatformServices::WriteSave(Text(r.Name, r.NameLength), Text(r.Input, r.InputLength)); break;
            case Op::EmitEvent:
            {
                ScriptWorld::EmitEvent(r.Entity, Text(r.Name, r.NameLength), Text(r.Input, r.InputLength));
                break;
            }
            case Op::ReadEvent:
            {
                const auto event = ScriptWorld::ReadEvent(r.Entity, Text(r.Name, r.NameLength), r.Result);
                r.Result = event.Sequence;
                Output(r, event.Payload);
                break;
            }
            default: throw std::invalid_argument("Unknown engine scripting operation.");
            }
            return 0;
        }
        catch (const std::exception& error)
        {
            try { Output(r, error.what()); } catch (...) { r.OutputLength = 0; }
            return -1;
        }
        catch (...) { return -1; }
    }
}
