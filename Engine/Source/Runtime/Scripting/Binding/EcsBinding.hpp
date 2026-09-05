#pragma once

#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/AI/Blackboard/Blackboard.h"
#include "Runtime/EcsFramework/Component/AI/BehaviorTreeComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Component/Script/PythonScriptComponent.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"
#include "Runtime/EcsFramework/Component/Character/CharacterMovementComponent.h"
#include "Runtime/EcsFramework/Component/Physics/ThreeD/Rigidbody3DComponent.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/Serializer/PrefabSerializer.h"
#include "Runtime/EcsFramework/System/Movement/CharacterMovementSystem.h"
#include "Runtime/EcsFramework/System/Script/PythonScriptSystem.h"
#include "Runtime/EcsFramework/System/Physics/PhysicSystem3D.h"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>

namespace py = pybind11;

namespace HEngine
{
    template<Component... C>
    static void BindingComponents(py::module_& HEngineModule)
    {
        ([&]()
            {
                C::PyBinding(HEngineModule);
            }(), ...);
    }

    template<Component... C>
    static void BindingComponents(ComponentGroup<C...>, py::module_& HEngineModule)
    {
        BindingComponents<C...>(HEngineModule);
    }

    static void BindingComponents(py::module_& HEngineModule)
    {
        BindingComponents(ComponentsPyBinding{}, HEngineModule);
    }

    // 生成的 PyBinding 仅暴露字段，此处补充 AnimatorComponent 方法绑定
    static void ExtendAnimatorComponentBinding(py::module_& HEngineModule)
    {
        py::object animCls = HEngineModule.attr("AnimatorComponent");
        animCls.attr("Play") = py::cpp_function(
            [](AnimatorComponent& self, int clipIndex) { self.Play(clipIndex); },
            py::is_method(animCls),
            py::arg("clipIndex") = -1
        );
        animCls.attr("Stop") = py::cpp_function(
            [](AnimatorComponent& self) { self.Stop(); },
            py::is_method(animCls)
        );
        animCls.attr("SetSpeed") = py::cpp_function(
            [](AnimatorComponent& self, float speed) { self.SetSpeed(speed); },
            py::is_method(animCls),
            py::arg("speed")
        );
        animCls.attr("GetControllerPath") = py::cpp_function(
            [](const AnimatorComponent& self) -> std::string { return self.GetControllerPath(); },
            py::is_method(animCls)
        );
        animCls.attr("SetControllerPath") = py::cpp_function(
            [](AnimatorComponent& self, const std::string& path) { self.SetControllerPath(path); },
            py::is_method(animCls),
            py::arg("path")
        );
        animCls.attr("get_controller_path") = py::cpp_function(
            [](const AnimatorComponent& self) -> std::string { return self.GetControllerPath(); },
            py::is_method(animCls)
        );
        animCls.attr("set_controller_path") = py::cpp_function(
            [](AnimatorComponent& self, const std::string& path) { self.SetControllerPath(path); },
            py::is_method(animCls),
            py::arg("path")
        );
        animCls.attr("GetClipCount") = py::cpp_function(
            [](const AnimatorComponent& self) -> int { return self.GetClipCount(); },
            py::is_method(animCls)
        );
        animCls.attr("GetClipName") = py::cpp_function(
            [](const AnimatorComponent& self, int index) -> std::string { return self.GetClipName(index); },
            py::is_method(animCls),
            py::arg("index")
        );
        animCls.attr("GetProgress") = py::cpp_function(
            [](const AnimatorComponent& self) -> float { return self.GetProgress(); },
            py::is_method(animCls)
        );
        animCls.attr("set_bool") = py::cpp_function(
            [](AnimatorComponent& self, const std::string& name, bool value) { self.SetBool(name, value); },
            py::is_method(animCls),
            py::arg("name"), py::arg("value")
        );
        animCls.attr("SetBool") = py::cpp_function(
            [](AnimatorComponent& self, const std::string& name, bool value) { self.SetBool(name, value); },
            py::is_method(animCls),
            py::arg("name"), py::arg("value")
        );
        animCls.attr("set_int") = py::cpp_function(
            [](AnimatorComponent& self, const std::string& name, int value) { self.SetInt(name, value); },
            py::is_method(animCls),
            py::arg("name"), py::arg("value")
        );
        animCls.attr("set_float") = py::cpp_function(
            [](AnimatorComponent& self, const std::string& name, float value) { self.SetFloat(name, value); },
            py::is_method(animCls),
            py::arg("name"), py::arg("value")
        );
        animCls.attr("set_trigger") = py::cpp_function(
            [](AnimatorComponent& self, const std::string& name) { self.SetTrigger(name); },
            py::is_method(animCls),
            py::arg("name")
        );
        animCls.attr("SetTrigger") = py::cpp_function(
            [](AnimatorComponent& self, const std::string& name) { self.SetTrigger(name); },
            py::is_method(animCls),
            py::arg("name")
        );
        animCls.attr("reset_trigger") = py::cpp_function(
            [](AnimatorComponent& self, const std::string& name) { self.ResetTrigger(name); },
            py::is_method(animCls),
            py::arg("name")
        );
        animCls.attr("has_parameter") = py::cpp_function(
            [](const AnimatorComponent& self, const std::string& name) -> bool { return self.HasParameter(name); },
            py::is_method(animCls),
            py::arg("name")
        );
        animCls.attr("get_bool") = py::cpp_function(
            [](const AnimatorComponent& self, const std::string& name, bool defaultValue) -> bool { return self.GetBool(name, defaultValue); },
            py::is_method(animCls),
            py::arg("name"), py::arg("default_value") = false
        );
        animCls.attr("get_int") = py::cpp_function(
            [](const AnimatorComponent& self, const std::string& name, int defaultValue) -> int { return self.GetInt(name, defaultValue); },
            py::is_method(animCls),
            py::arg("name"), py::arg("default_value") = 0
        );
        animCls.attr("get_float") = py::cpp_function(
            [](const AnimatorComponent& self, const std::string& name, float defaultValue) -> float { return self.GetFloat(name, defaultValue); },
            py::is_method(animCls),
            py::arg("name"), py::arg("default_value") = 0.0f
        );
        animCls.attr("get_trigger") = py::cpp_function(
            [](const AnimatorComponent& self, const std::string& name) -> bool { return self.GetTrigger(name); },
            py::is_method(animCls),
            py::arg("name")
        );
        animCls.attr("get_current_state") = py::cpp_function(
            [](const AnimatorComponent& self) -> std::string { return self.GetCurrentState(); },
            py::is_method(animCls)
        );
    }

    static void ExtendBehaviorTreeComponentBinding(py::module_& HEngineModule)
    {
        py::object btCls = HEngineModule.attr("BehaviorTreeComponent");
        btCls.attr("get_blackboard") = py::cpp_function(
            [](BehaviorTreeComponent& self) -> Blackboard& { return self.mBlackboard; },
            py::is_method(btCls),
            py::return_value_policy::reference_internal
        );
        btCls.attr("is_enabled") = py::cpp_function(
            [](const BehaviorTreeComponent& self) -> bool { return self.bEnabled; },
            py::is_method(btCls)
        );
        btCls.attr("set_enabled") = py::cpp_function(
            [](BehaviorTreeComponent& self, bool enabled) { self.bEnabled = enabled; },
            py::is_method(btCls),
            py::arg("enabled")
        );
        btCls.attr("get_bt_res_path") = py::cpp_function(
            [](const BehaviorTreeComponent& self) -> std::string { return self.btResPath; },
            py::is_method(btCls)
        );
        btCls.attr("set_bt_res_path") = py::cpp_function(
            [](BehaviorTreeComponent& self, const std::string& path) { self.btResPath = path; },
            py::is_method(btCls),
            py::arg("path")
        );
    }

    static void EcsBinding(py::module_& HEngineModule)
    {
        BindingComponents(HEngineModule);
        ExtendAnimatorComponentBinding(HEngineModule);

        py::class_<Blackboard>(HEngineModule, "Blackboard")
            .def("set_float", &Blackboard::SetFloat, py::arg("key"), py::arg("value"))
            .def("get_float", &Blackboard::GetFloat, py::arg("key"), py::arg("default_val") = 0.0f)
            .def("set_int", &Blackboard::SetInt, py::arg("key"), py::arg("value"))
            .def("get_int", &Blackboard::GetInt, py::arg("key"), py::arg("default_val") = 0)
            .def("set_bool", &Blackboard::SetBool, py::arg("key"), py::arg("value"))
            .def("get_bool", &Blackboard::GetBool, py::arg("key"), py::arg("default_val") = false)
            .def("set_vec3", &Blackboard::SetVec3, py::arg("key"), py::arg("value"))
            .def("get_vec3", &Blackboard::GetVec3, py::arg("key"), py::arg("default_val") = glm::vec3(0.0f))
            .def("set_entity_handle", &Blackboard::SetEntityHandle, py::arg("key"), py::arg("value"))
            .def("get_entity_handle", &Blackboard::GetEntityHandle, py::arg("key"), py::arg("default_val") = static_cast<uint32_t>(entt::null))
            .def("has_key", &Blackboard::HasKey, py::arg("key"))
            .def("remove_key", &Blackboard::RemoveKey, py::arg("key"))
            .def("clear", &Blackboard::Clear)
            .def("get_all_keys", &Blackboard::GetAllKeys);

        ExtendBehaviorTreeComponentBinding(HEngineModule);

        HEngineModule.def(
            "RaycastFloorHeight",
            [](const glm::vec3& position, float castDistanceAbove, float castDistanceBelow) -> py::object
            {
                Level* level = PythonScriptSystem::GetActiveLevel();
                if (!level)
                    return py::none();

                PhysicSystem3D* physicsSystem = level->TryGetSystem<PhysicSystem3D>();
                if (!physicsSystem)
                    return py::none();

                float floorHeight = 0.0f;
                if (!physicsSystem->RaycastFloorHeight(
                    position,
                    castDistanceAbove,
                    castDistanceBelow,
                    floorHeight))
                {
                    return py::none();
                }

                return py::float_(floorHeight);
            },
            py::arg("position"),
            py::arg("cast_distance_above") = 8.0f,
            py::arg("cast_distance_below") = 24.0f);

        py::class_<Entity>(HEngineModule, "Entity")
            .def(py::init())
            .def("GetComponent", [](Entity& entity, const std::string& compName) 
                -> std::variant<std::reference_wrapper<TransformComponent>, std::reference_wrapper<PythonScriptComponent>, 
                std::reference_wrapper<Rigidbody3DComponent>, std::reference_wrapper<TagComponent>,
                std::reference_wrapper<AnimatorComponent>, std::reference_wrapper<BehaviorTreeComponent>,
                std::reference_wrapper<CharacterMovementComponent>> {
                if (compName == "Transform" || compName == "TransformComponent")
                    return entity.GetComponent<TransformComponent>(); 
                else if (compName == "PythonScript" || compName == "PythonScriptComponent")
                    return entity.GetComponent<PythonScriptComponent>();
                else if (compName == "Tag" || compName == "TagComponent")
                    return entity.GetComponent<TagComponent>();
                else if (compName == "Rigidbody" || compName == "Rigidbody3DComponent")
                    return entity.GetComponent<Rigidbody3DComponent>();
                else if (compName == "Animator" || compName == "AnimatorComponent")
                    return entity.GetComponent<AnimatorComponent>();
                else if (compName == "BehaviorTree" || compName == "BehaviorTreeComponent")
                    return entity.GetComponent<BehaviorTreeComponent>();
                else if (compName == "CharacterMovement" || compName == "CharacterMovementComponent")
                    return entity.GetComponent<CharacterMovementComponent>();
                return entity.GetComponent<TransformComponent>();
                })
            .def("HasComponent", [](Entity& entity, const std::string& compName)
                {
                    if (compName == "Transform" || compName == "TransformComponent")
                        return entity.HasComponent<TransformComponent>();
                    if (compName == "PythonScript" || compName == "PythonScriptComponent")
                        return entity.HasComponent<PythonScriptComponent>();
                    if (compName == "CharacterMovement" || compName == "CharacterMovementComponent")
                        return entity.HasComponent<CharacterMovementComponent>();
                    if (compName == "Tag" || compName == "TagComponent")
                        return entity.HasComponent<TagComponent>();
                    if (compName == "Rigidbody" || compName == "Rigidbody3DComponent")
                        return entity.HasComponent<Rigidbody3DComponent>();
                    if (compName == "Animator" || compName == "AnimatorComponent")
                        return entity.HasComponent<AnimatorComponent>();
                    if (compName == "BehaviorTree" || compName == "BehaviorTreeComponent")
                        return entity.HasComponent<BehaviorTreeComponent>();
                    return false;
                })
            .def("SetMovementInputEnabled", [](Entity& entity, bool enabled)
                {
                    if (!entity.HasComponent<CharacterMovementComponent>())
                        return;

                    auto& movement = entity.GetComponent<CharacterMovementComponent>();
                    movement.bPlayerInputEnabled = enabled;
                    if (!enabled)
                    {
                        movement.RuntimeVelocity = glm::vec3(0.0f);
                        movement.RuntimeDesiredVelocity = glm::vec3(0.0f);
                        movement.RuntimeHasExternalVelocity = false;
                        movement.RuntimeHasLastInputYaw = false;
                    }
                })
            .def("SetVisual", [](Entity& entity,
                const std::string& meshPath,
                const std::string& animationControllerPath,
                float uniformScale)
                {
                    if (entity.HasComponent<MeshComponent>())
                    {
                        auto& mesh = entity.GetComponent<MeshComponent>();
                        if (mesh.Path != meshPath || !mesh.mMesh || mesh.mMesh->mSubMeshes.empty())
                        {
                            mesh.Path = meshPath;
                            mesh.meshResPath.clear();
                            mesh.mMesh = CreateRef<Mesh>(meshPath);
                        }
                    }

                    if (entity.HasComponent<AnimatorComponent>())
                    {
                        auto& animator = entity.GetComponent<AnimatorComponent>();
                        animator.SetControllerPath(animationControllerPath);
                        animator.bPlay = true;
                    }

                    if (entity.HasComponent<TransformComponent>())
                        entity.GetComponent<TransformComponent>().Scale = glm::vec3(uniformScale);
                },
                py::arg("mesh_path"),
                py::arg("animation_controller_path"),
                py::arg("uniform_scale"))
            .def("SetVisualSilhouette", [](Entity& entity, bool enabled)
                {
                    if (entity.HasComponent<MeshComponent>())
                        entity.GetComponent<MeshComponent>().bRuntimeBlackSilhouette = enabled;
                },
                py::arg("enabled"))
            .def("Teleport", [](Entity& entity, const glm::vec3& worldPosition)
                {
                    if (!entity.HasComponent<CharacterMovementComponent>())
                        return false;

                    Level* level = entity.GetLevel();
                    if (!level)
                        return false;

                    CharacterMovementSystem* movementSystem = level->TryGetSystem<CharacterMovementSystem>();
                    return movementSystem && movementSystem->Teleport(entity, worldPosition);
                })
            .def("Move", [](Entity& entity, const glm::vec3& velocity)
                {
                    if (entity.HasComponent<CharacterMovementComponent>())
                    {
                        auto& movement = entity.GetComponent<CharacterMovementComponent>();
                        movement.RuntimeDesiredVelocity = velocity;
                        movement.RuntimeHasExternalVelocity = true;
                        return;
                    }

                    if (entity.HasComponent<Rigidbody3DComponent>())
                    {
                        auto& rb3d = entity.GetComponent<Rigidbody3DComponent>();
                        if (rb3d.RuntimeBody)
                        {
                            if (Level* level = entity.GetLevel())
                            {
                                if (auto* physics = level->TryGetSystem<PhysicSystem3D>())
                                    physics->SetBodyLinearVelocity(rb3d.RuntimeBody, velocity);
                            }
                        }
                    }
                })
            .def("Move", [](Entity& entity, float velocityX, float velocityY, float velocityZ)
                {
                    const glm::vec3 velocity(velocityX, velocityY, velocityZ);

                    if (entity.HasComponent<CharacterMovementComponent>())
                    {
                        auto& movement = entity.GetComponent<CharacterMovementComponent>();
                        movement.RuntimeDesiredVelocity = velocity;
                        movement.RuntimeHasExternalVelocity = true;
                        return;
                    }

                    if (entity.HasComponent<Rigidbody3DComponent>())
                    {
                        auto& rb3d = entity.GetComponent<Rigidbody3DComponent>();
                        if (rb3d.RuntimeBody)
                        {
                            if (Level* level = entity.GetLevel())
                            {
                                if (auto* physics = level->TryGetSystem<PhysicSystem3D>())
                                    physics->SetBodyLinearVelocity(rb3d.RuntimeBody, velocity);
                            }
                        }
                    }
                });

        HEngineModule.def("Instantiate", [](const std::string& prefabPath) -> Entity
            {
                Level* level = PythonScriptSystem::GetActiveLevel();
                if (!level)
                {
                    HE_CORE_ERROR("[HEngine.Instantiate] No active level! Can only be called at runtime.");
                    return {};
                }

                return level->InstantiatePrefab(prefabPath);
            }, py::arg("prefabPath"));

        HEngineModule.def("Instantiate", [](const std::string& prefabPath, const glm::vec3& position) -> Entity
            {
                Level* level = PythonScriptSystem::GetActiveLevel();
                if (!level)
                {
                    HE_CORE_ERROR("[HEngine.Instantiate] No active level!");
                    return {};
                }

                return level->InstantiatePrefab(prefabPath, position);
            }, py::arg("prefabPath"), py::arg("position"));

        HEngineModule.def("Instantiate", [](const std::string& prefabPath, const glm::vec3& position, const glm::vec3& rotation) -> Entity
            {
                Level* level = PythonScriptSystem::GetActiveLevel();
                if (!level)
                {
                    HE_CORE_ERROR("[HEngine.Instantiate] No active level!");
                    return {};
                }

                return level->InstantiatePrefab(prefabPath, position, rotation);
            }, py::arg("prefabPath"), py::arg("position"), py::arg("rotation"));

        HEngineModule.def("Destroy", [](Entity entity)
            {
                Level* level = PythonScriptSystem::GetActiveLevel();
                if (!level)
                {
                    HE_CORE_ERROR("[HEngine.Destroy] No active level!");
                    return;
                }

                if (!entity.IsValid())
                {
                    HE_CORE_WARN("[HEngine.Destroy] Entity is not valid.");
                    return;
                }
                if (entity.GetLevel() != level)
                {
                    HE_CORE_WARN("[HEngine.Destroy] Entity does not belong to the active level.");
                    return;
                }

                level->DestroyEntity(entity);
            }, py::arg("entity"));

        HEngineModule.def("FindEntityByTag", [](const std::string& tag) -> Entity
            {
                Level* level = PythonScriptSystem::GetActiveLevel();
                if (!level)
                {
                    HE_CORE_ERROR("[HEngine.FindEntityByTag] No active level!");
                    return {};
                }

                auto view = level->mRegistry.view<TagComponent>();
                for (auto e : view)
                {
                    auto& tagComp = view.get<TagComponent>(e);
                    if (tagComp.Tag == tag)
                        return Entity{ e, level };
                }

                return {};
            }, py::arg("tag"));
    }
}
