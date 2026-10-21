#include "Hepch.h"

#include "Runtime/Camera/Camera.h"
#include "Runtime/Core/Meta/Reflection/ReflectionRegister.h"
#include "Runtime/Core/Meta/Serializer/Serializer.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/ResType/Animation.h"
#include "Runtime/Resource/ResType/AnimController.h"
#include "Runtime/Resource/ResType/BehaviorTreeRes.h"
#include "Runtime/Resource/ResType/Material.h"
#include "Runtime/Resource/ResType/Skeleton.h"
#include "Runtime/Resource/ResType/UIRes.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "_generated/serializer/Animation.gen.h"
#include "_generated/serializer/AnimController.gen.h"
#include "_generated/serializer/BehaviorTreeRes.gen.h"
#include "_generated/serializer/Material.gen.h"
#include "_generated/serializer/Skeleton.gen.h"
#include "_generated/serializer/UIRes.gen.h"

#include <filesystem>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>

namespace HEngine::Tests
{
    namespace
    {
        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        std::filesystem::path FindRepositoryRoot()
        {
            std::filesystem::path current = std::filesystem::current_path();
            for (;;)
            {
                if (std::filesystem::is_regular_file(current / "Project/Project.heproject") &&
                    std::filesystem::is_directory(current / "Engine/Content"))
                {
                    return current;
                }

                const std::filesystem::path parent = current.parent_path();
                if (parent == current)
                    break;
                current = parent;
            }

            throw std::runtime_error("Could not locate the HEngine repository root.");
        }

        class ScopedProjectConfiguration final
        {
        public:
            ScopedProjectConfiguration()
            {
                const std::filesystem::path repositoryRoot = FindRepositoryRoot();
                const std::filesystem::path fixtureRoot = repositoryRoot / "Examples/RenderingTests";
                ConfigManager::GetInstance().Clear();
                ConfigManager::GetInstance().Init(
                    fixtureRoot / "Project.heproject",
                    repositoryRoot / "Engine/Content",
                    fixtureRoot / "Saved",
                    repositoryRoot / "Engine/Content");
            }

            ~ScopedProjectConfiguration()
            {
                ConfigManager::GetInstance().Clear();
            }
        };

        class ScopedReflectionRegistration final
        {
        public:
            ScopedReflectionRegistration()
            {
                Reflection::TypeMetaRegister::metaRegister();
            }

            ~ScopedReflectionRegistration()
            {
                Reflection::TypeMetaRegister::metaUnregister();
            }
        };

        void RegisterSyntheticReflectionClass(const char* fullyQualifiedName)
        {
            auto* classFunctions = new ClassFunctionTuple(
                [](Reflection::ReflectionInstance*& outList, void*)
                {
                    outList = nullptr;
                    return 0;
                },
                [](const Json&) -> void*
                {
                    return nullptr;
                },
                [](void*)
                {
                    return Json::object();
                });
            Reflection::TypeMetaRegisterinterface::registerToClassMap(
                fullyQualifiedName, classFunctions);
        }

        std::filesystem::path ProjectAsset(std::string_view relativePath)
        {
            const std::filesystem::path fullPath =
                ConfigManager::GetProjectFullPath(std::string(relativePath));
            Require(std::filesystem::is_regular_file(fullPath),
                "A reflected resource schema fixture is missing: " + std::string(relativePath));
            return fullPath;
        }

        void RequireKeys(
            const Json& json,
            std::initializer_list<std::string_view> keys,
            std::string_view message)
        {
            Require(json.is_object(), message);
            for (const std::string_view key : keys)
                Require(json.contains(std::string(key)), message);
        }

        template<typename T>
        Json SerializeGenerated(const T& resource)
        {
            Json json;
            Serializer::Serialize(json, resource);
            return json;
        }
    }

    void ReflectedResourceSchemaCompatibilityContract()
    {
        ScopedReflectionRegistration reflectionRegistration;
        ScopedProjectConfiguration configuration;

        Reflection::TypeMeta cameraMeta =
            Reflection::TypeMeta::newMetaFromName("HEngine::Camera");
        Require(cameraMeta.isValid(),
            "A registered zero-field reflected type was reported as invalid.");
        Reflection::FieldAccessor* cameraFields = nullptr;
        const int cameraFieldCount = cameraMeta.getFieldsList(cameraFields);
        delete[] cameraFields;
        Require(cameraFieldCount == 0,
            "The zero-field Camera reflection contract unexpectedly gained fields.");

        constexpr const char* shortAlias = "ReflectedSchemaCollisionProbe";
        constexpr const char* firstQualifiedName =
            "HEngine::SchemaProbeA::ReflectedSchemaCollisionProbe";
        constexpr const char* secondQualifiedName =
            "HEngine::SchemaProbeB::ReflectedSchemaCollisionProbe";
        RegisterSyntheticReflectionClass(firstQualifiedName);
        Reflection::TypeMetaRegisterinterface::registerTypeAlias(
            shortAlias, firstQualifiedName);
        Require(Reflection::TypeMeta::newMetaFromName(shortAlias).isValid(),
            "A unique short reflection alias did not resolve its FQN.");

        RegisterSyntheticReflectionClass(secondQualifiedName);
        Reflection::TypeMetaRegisterinterface::registerTypeAlias(
            shortAlias, secondQualifiedName);
        Require(Reflection::TypeMeta::newMetaFromName(firstQualifiedName).isValid() &&
            Reflection::TypeMeta::newMetaFromName(secondQualifiedName).isValid(),
            "Fully qualified reflection identities did not remain independently valid.");
        Require(!Reflection::TypeMeta::newMetaFromName(shortAlias).isValid(),
            "An ambiguous short reflection alias resolved to the wrong FQN.");

        const Json materialJson = Utils::LoadJsonFile(ProjectAsset(
            "Assets/Material/TransparentPulse.hmat"));
        RequireKeys(materialJson,
            { "SchemaVersion", "Type", "PassName", "ShaderPath", "VariantKeywords",
              "Domain", "ShadingModel", "BlendMode", "CullMode", "DepthWrite",
              "AlphaCutoff", "Parameters", "Textures", "OrphanedParameters", "Properties" },
            "The persisted Material schema changed.");
        MaterialRes material;
        Serializer::Deserialize(materialJson, material);
        Require(material.SchemaVersion == 2 && material.Type == "Custom" &&
            material.PassName == "TransparentPulse" &&
            material.ShaderPath == "Shaders/TransparentPulse.hlsl" &&
            material.BlendMode == "Transparent" && !material.DepthWrite,
            "The generated Material serializer did not preserve the authored identity.");
        Require(material.Parameters.is_object() &&
            material.Parameters.contains("g_Opacity") &&
            material.Parameters["g_Opacity"].value("StableId", "") == "pulse.opacity" &&
            material.Properties.is_object() &&
            material.Properties.value("name", "") == "Transparent Pulse",
            "The generated Material serializer lost nested parameter data.");
        RequireKeys(SerializeGenerated(material),
            { "SchemaVersion", "Type", "PassName", "ShaderPath", "VariantKeywords",
              "Domain", "ShadingModel", "BlendMode", "CullMode", "DepthWrite",
              "AlphaCutoff", "Parameters", "Textures", "OrphanedParameters", "Properties" },
            "The generated Material serializer emitted incompatible keys.");

        const Json animationJson = Utils::LoadJsonFile(ProjectAsset(
            "Assets/Models/Quaternius/UltimateMonsters/Cat/CharacterArmature_Death.hanim"));
        RequireKeys(animationJson,
            { "version", "name", "meshResPath", "targetSkeletonPath", "boneInfos",
              "rootNode", "clips" },
            "The persisted Animation schema changed.");
        RequireKeys(animationJson.at("clips").at(0),
            { "name", "duration", "ticksPerSecond", "channels" },
            "The persisted Animation clip schema changed.");
        AnimRes animation;
        Serializer::Deserialize(animationJson, animation);
        Require(animation.version == 1 && animation.name == "CharacterArmature|Death" &&
            animation.targetSkeletonPath ==
                "Assets/Models/Quaternius/UltimateMonsters/Cat/Cat_Skeleton.hskel" &&
            animation.boneInfos.size() == 4 && animation.rootNode.name == "RootNode" &&
            animation.clips.size() == 1 && animation.clips[0].channels.size() == 5,
            "The generated Animation serializer lost clip or skeleton structure.");
        RequireKeys(SerializeGenerated(animation),
            { "version", "name", "meshResPath", "targetSkeletonPath", "boneInfos",
              "rootNode", "clips" },
            "The generated Animation serializer emitted incompatible keys.");

        AnimControllerRes controller;
        std::string loadError;
        Require(LoadAnimControllerRes(ProjectAsset(
                "Assets/Models/Quaternius/UltimateMonsters/Orc/Orc_Combat.hanimctrl"),
                controller, &loadError),
            "The existing AnimController loader rejected a persisted asset.");
        Require(controller.version == 1 && controller.name == "OrcCombat" &&
            controller.entryNodeId == 1 && controller.parameters.size() == 3 &&
            controller.parameters[0].name == "Attacking" &&
            controller.parameters[0].type == AnimControllerParamType::Bool &&
            controller.nodes.size() == 4 && controller.nodes[0].type == "Entry" &&
            controller.transitions.size() == 5 &&
            controller.transitions[0].fromNodeId == 1 &&
            controller.transitions[0].toNodeId == 2,
            "The generated AnimController serializer lost graph structure.");
        const Json controllerRoundTrip = SerializeGenerated(controller);
        RequireKeys(controllerRoundTrip,
            { "version", "name", "targetSkeletonPath", "parameters", "nodes",
              "transitions", "entryNodeId" },
            "The generated AnimController serializer emitted incompatible keys.");
        RequireKeys(controllerRoundTrip.at("transitions").at(1).at("conditions").at(0),
            { "param", "op", "value" },
            "The generated AnimController condition schema changed.");

        BehaviorTreeRes behaviorTree;
        loadError.clear();
        Require(LoadBehaviorTreeRes(ProjectAsset("Assets/AI/SimpleChaseMonster.hbt"),
                behaviorTree, &loadError),
            "The existing BehaviorTree loader rejected a persisted asset.");
        Require(behaviorTree.version == 1 && behaviorTree.name == "SimpleChaseMonster" &&
            behaviorTree.rootNodeId == 1 && behaviorTree.nodes.size() == 14 &&
            behaviorTree.nodes[0].type == "Selector" &&
            behaviorTree.nodes[0].parentId == -1 &&
            behaviorTree.blackboardKeys.size() == 4 &&
            behaviorTree.blackboardKeys[0].key == "Health",
            "The generated BehaviorTree serializer lost node or blackboard structure.");
        const Json behaviorTreeRoundTrip = SerializeGenerated(behaviorTree);
        RequireKeys(behaviorTreeRoundTrip,
            { "version", "name", "rootNodeId", "nodes", "blackboardKeys" },
            "The generated BehaviorTree serializer emitted incompatible keys.");
        RequireKeys(behaviorTreeRoundTrip.at("nodes").at(2).at("properties").at(0),
            { "key", "type", "value" },
            "The generated BehaviorTree property schema changed.");

        UIRes ui;
        loadError.clear();
        Require(LoadUIRes(ProjectAsset("Assets/UI/ExampleHUD.hui"), ui, &loadError),
            "The existing UI loader rejected a persisted asset.");
        Require(ui.Version == 1 && ui.Name == "ExampleHUD" && ui.Nodes.size() == 7 &&
            ui.Nodes[0].Id == 1 && ui.Nodes[0].Type == UIWidgetType::Canvas &&
            ui.Nodes[2].ElementId == "score_label" &&
            ui.Nodes[2].Style.ClassName == "score-label" &&
            ui.Nodes[2].Text == "Score: 1234",
            "The generated UI serializer lost node, layout, or style structure.");
        const Json uiRoundTrip = SerializeGenerated(ui);
        RequireKeys(uiRoundTrip,
            { "Version", "Name", "GeneratedRmlPath", "GeneratedRcssPath", "Nodes" },
            "The generated UI serializer emitted incompatible keys.");
        RequireKeys(uiRoundTrip.at("Nodes").at(2),
            { "Id", "ParentId", "Type", "Name", "ElementId", "Text", "ImagePath",
              "bInteractable", "bVisible", "Layout", "Style" },
            "The generated UI node schema changed.");

        SkeletonRes skeleton;
        loadError.clear();
        Require(LoadSkeletonRes(ProjectAsset(
                "Assets/Models/Quaternius/UltimateMonsters/Yeti/Yeti_Skeleton.hskel"),
                skeleton, &loadError),
            "The existing Skeleton loader rejected a persisted asset.");
        Require(skeleton.version == 1 && skeleton.name == "Yeti_Skeleton" &&
            skeleton.bones.size() == 4 && skeleton.inverseBindMatrices.size() == 4 &&
            skeleton.bones[0].index == 0 && skeleton.bones[0].name == "Body" &&
            skeleton.bones[0].parentIndex == -1 &&
            skeleton.inverseBindMatrices[0].size() == 16,
            "The generated Skeleton serializer lost hierarchy or bind-matrix structure.");
        const Json skeletonRoundTrip = SerializeGenerated(skeleton);
        RequireKeys(skeletonRoundTrip,
            { "version", "name", "bones", "inverseBindMatrices" },
            "The generated Skeleton serializer emitted incompatible keys.");
        RequireKeys(skeletonRoundTrip.at("bones").at(0),
            { "index", "name", "parentIndex", "localBindT", "localBindR", "localBindS" },
            "The generated Skeleton bone schema changed.");
    }
}
