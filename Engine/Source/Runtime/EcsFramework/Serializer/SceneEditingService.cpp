#include "Hepch.h"
#include "Runtime/EcsFramework/Serializer/SceneEditingService.h"
#include "Runtime/EcsFramework/Serializer/SceneDocument.h"
#include "Runtime/EcsFramework/Serializer/PrefabDocument.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/Component/Basic/TagComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Component/Light/PointLightComponent.h"
#include "Runtime/EcsFramework/Component/Light/DirectionalLightComponent.h"
#include "Runtime/EcsFramework/Component/Light/EnvironmentLightComponent.h"
#include "Runtime/EcsFramework/Component/PostProcess/PostProcessingComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/Core/UUID.h"
#include "Runtime/Scripting/ScriptBackendRegistry.h"
#include "Runtime/Scripting/ScriptFieldsJson.h"
#include "Runtime/Core/Meta/Serializer/Serializer.h"
#include "Runtime/Graphics/Mesh/MeshLoader.h"
#include "Runtime/Graphics/Material/MaterialShaderPath.h"
#include "Runtime/Resource/ResType/Material.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimePrefabResource.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/MathUtils/MathUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "all_serializer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace HEngine
{
    namespace
    {
        constexpr size_t MaxHistory = 64, MaxHistoryBytes = 16 * 1024 * 1024;
        const std::vector<std::string> Components = { "TagComponent", "TransformComponent",
            "MeshComponent", "PointLightComponent", "DirectionalLightComponent", "EnvironmentLightComponent", "PostProcessingComponent", "CameraComponent",
            "ScriptComponent", "PythonScriptComponent", "FollowCameraComponent", "CapsuleCollider3DComponent", "Rigidbody3DComponent",
            "AnimatorComponent", "SoundComponent", "CharacterMovementComponent", "SpriteRendererComponent", "SequenceComponent" };
        const std::vector<std::string> Methods = { "describe", "query", "plan", "apply", "diff",
            "validate", "save", "undo", "redo", "export_prefab" };
        struct Error : std::runtime_error
        {
            std::string Code, Entity, Field;
            int Index = -1;
            Error(std::string code, std::string message, std::string field = {})
                : std::runtime_error(message), Code(std::move(code)), Field(std::move(field)) {}
        };
        void Require(bool test, const char* code, const std::string& message, const std::string& field = {})
        { if (!test) throw Error(code, message, field); }
        void Keys(const Json& value, std::initializer_list<const char*> allowed)
        {
            Require(value.is_object(), "invalid_request", "Expected an object.");
            for (auto& [key, unused] : value.items())
                Require(std::find(allowed.begin(), allowed.end(), key) != allowed.end(),
                    "unknown_field", "Unknown field: " + key, key);
        }
        bool Supported(const std::string& name)
        { return std::find(Components.begin(), Components.end(), name) != Components.end(); }
        Json OperationSchema(const std::string& op)
        {
            Json properties = {{"op",{{"type","string"},{"const",op}}}};
            Json required = Json::array({"op"});
            auto field = [&](const char* name, const char* type, bool needed = false) {
                properties[name] = {{"type",type}}; if (needed) required.push_back(name);
            };
            if (op == "create" || op == "prefab")
            {
                field("temporaryId","string"); field("name","string"); field("parent","string"); field("components","object");
                if (op == "prefab") field("path","string",true);
            }
            else
            {
                field("entity","string"); field("entities","array");
                if (op == "rename") field("name","string",true);
                else if (op == "prefab_sync") field("revision","string");
                else if (op == "prefab_unpack") { }
                else if (op == "prefab_override") { field("pointer","string",true); field("mode","string",true); }
                else if (op == "add" || op == "remove" || op == "patch")
                { field("component","string",true); if (op != "remove") field("patch","object",true); }
                else if (op == "transform")
                {
                    field("patch","object",true); field("space","string"); field("mode","string");
                    properties["space"]["enum"] = {"local","world"}; properties["mode"]["enum"] = {"absolute","relative"};
                    properties["space"]["default"] = "local"; properties["mode"]["default"] = "absolute";
                }
                else if (op == "reparent")
                { field("parent","string",true); field("mode","string",true); properties["mode"]["enum"] = {"keepLocal","keepWorld"}; }
                else if (op == "material")
                { field("slot","integer",true); field("path","string",true); properties["slot"]["minimum"] = 0; }
                else if (op == "delete" || op == "duplicate")
                { field("children","string",true); properties["children"]["enum"] = {"subtree","reject"}; if (op == "duplicate") field("temporaryId","string"); }
                else throw Error("unknown_operation", "Unsupported operation: " + op);
            }
            Json schema = {{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}};
            if (op != "create" && op != "prefab") schema["oneOf"] = Json::array({{{"required",{"entity"}}},{{"required",{"entities"}}}});
            return schema;
        }
        void CheckOperation(const Json& value, const Json& schema)
        {
            for (const auto& field : schema.at("required")) Require(value.contains(field.get<std::string>()), "required_field", "Required operation field is missing.", field);
            for (auto& [field, item] : value.items())
            {
                Require(schema.at("properties").contains(field), "unknown_field", "Field is not valid for this operation.", field);
                const Json& descriptor = schema.at("properties").at(field); const std::string type = descriptor.at("type");
                Require((type == "string" && item.is_string()) || (type == "array" && item.is_array()) ||
                    (type == "object" && item.is_object()) || (type == "integer" && item.is_number_integer()), "field_type", "Invalid operation field type.", field);
                if (descriptor.contains("enum")) Require(std::find(descriptor.at("enum").begin(),descriptor.at("enum").end(),item) != descriptor.at("enum").end(), "field_enum", "Value is not in the operation enum.", field);
            }
            if (schema.contains("oneOf")) Require(value.contains("entity") != value.contains("entities"), "invalid_targets", "Supply exactly one of entity or entities.");
        }
        Json FieldSchema(const std::string& component, const std::string& field, const Json& value)
        {
            Json result = {{"type",value.is_number_integer() ? "integer" : value.type_name()},{"default",value},
                {"readOnly",field == "ParentUuid" || field == "MaterialOverrides" || field == "meshResPath"}};
            if (component == "CameraComponent" && field == "Camera")
            {
                result["properties"] = Json::object();
                for (auto& [name,item] : value.items()) result["properties"][name] = {{"type",item.is_number_integer() ? "integer" : item.type_name()},{"default",item},{"readOnly",name == "mAspectRatio"}};
                result["properties"]["mProjectionType"]["enum"] = {0,1};
                result["constraints"] = "Projection: 0 perspective, 1 orthographic. FOV radians (0,pi), perspective near>0, far>near, orthographic size>0. Aspect is viewport-owned.";
            }
            if (value.is_number()) { result["minimum"] = component == "TransformComponent" ? -1e9 : 0; result["maximum"] = 1e9; }
            if (field == "CascadeCount") { result["minimum"] = 1; result["maximum"] = 4; }
            if (field == "CascadeLambda" || field == "ShadowStrength") result["maximum"] = 1;
            if (field == "IntensityUnits") result["enum"] = {0, 1, 2};
            if (field == "ExposureEV100") { result["minimum"] = -24; result["maximum"] = 32; result["unit"] = "EV100"; }
            if (field == "ExposureCompensation") { result["minimum"] = -24; result["maximum"] = 24; result["unit"] = "stops"; }
            if (component == "PostProcessingComponent" && field == "Temperature")
            { result["minimum"] = -1; result["maximum"] = 1; }
            if (component == "PostProcessingComponent" && field == "VignetteIntensity")
                result["maximum"] = 1;
            if (field == "Intensity") { result["unit"] = component == "DirectionalLightComponent" ? "lux" : "IntensityUnits"; result["maximum"] = double(std::numeric_limits<float>::max()); }
            if (field == "ShadowMapSize") { result["minimum"] = 256; result["maximum"] = 8192; }
            if (field == "Tag") { result["minLength"] = 1; result["maxUtf8Bytes"] = 256; }
            auto owner = Reflection::TypeMeta::newMetaFromName(component);
            auto accessor = owner.getFieldByName(field.c_str());
            const std::string reflectedType = accessor.getFieldTypeName();
            result["reflectedType"] = reflectedType;
            if (value.is_array() && reflectedType.starts_with("std::vector<"))
                result["items"] = {{"reflectedType", reflectedType.substr(12, reflectedType.size() - 13)}};
            else if (value.is_array() && component != "MeshComponent")
            { result["length"] = value.size(); result["items"] = {{"type","number"},{"finite",true}}; }
            if (component == "PostProcessingComponent" && field == "ColorGain")
            { result["items"]["minimum"] = 0; result["items"]["maximum"] = 2; }
            if (component == "PostProcessingComponent" && field == "ColorOffset")
            { result["items"]["minimum"] = -0.25; result["items"]["maximum"] = 0.25; }
            if (field == "Translation" || field == "Radius" || field == "ShadowDistance") result["unit"] = "meters";
            if (field == "Rotation") { result["unit"] = "radians"; result["representation"] = "Euler XYZ via glm::quat(vec3)"; }
            if (component == "TransformComponent") result["space"] = "local; transform operation also supports world";
            if (field == "ParentUuid") result["reference"] = "entity UUID in this scene; empty means root";
            if (field == "Path") result["resource"] = {{"type","Mesh"},{"extension",".hmesh"},{"scope","project-relative"}};
            if (field == "MaterialOverrides") result["resource"] = {{"type","Material"},{"extension",".hmat"},{"slot","decoded mesh material index"},{"empty","authored material"}};
            return result;
        }
        Json ExportPrefabSchema()
        {
            return {{"type","object"},{"additionalProperties",false},
                {"required",{"sceneId","baseRevision","requestId","entity","path"}},
                {"properties",{{"sceneId",{{"type","string"}}},{"baseRevision",{{"type","integer"},{"minimum",0}}},
                    {"requestId",{{"type","string"},{"minLength",1},{"maxLength",128}}},
                    {"entity",{{"type","string"}}},{"path",{{"type","string"}}}}}};
        }
        std::string SchemaFingerprint()
        {
            static const std::string fingerprint = [] {
                Json schemas;
                for (const auto& component : Components)
                { const Json defaults = SceneEditingService::ComponentDefaults(component); for (auto& [field,value] : defaults.items()) schemas[component][field] = FieldSchema(component,field,value); }
                for (const char* operation : {"create","rename","delete","duplicate","add","remove","patch","transform","reparent","material","prefab","prefab_sync","prefab_override","prefab_unpack"}) schemas[operation] = OperationSchema(operation);
                schemas["scene.export_prefab"] = ExportPrefabSchema();
                uint64_t hash = 14695981039346656037ull;
                for (unsigned char byte : schemas.dump()) { hash ^= byte; hash *= 1099511628211ull; }
                return std::to_string(hash);
            }();
            return fingerprint;
        }
        template<class T> Json Defaults()
        { T value; Json result; Serializer::Serialize(result, value); return result; }
        template<class T> Json Canonical(const Json& source)
        { T value; Serializer::Deserialize(source,value); Json result; Serializer::Serialize(result,value); return result; }
        Json CanonicalComponent(const std::string& component, const Json& source)
        {
            if (component == "TagComponent") return Canonical<TagComponent>(source);
            if (component == "TransformComponent") return Canonical<TransformComponent>(source);
            if (component == "MeshComponent") return Canonical<MeshComponent>(source);
            if (component == "SpriteRendererComponent") return Canonical<SpriteRendererComponent>(source);
            if (component == "CameraComponent") return Canonical<CameraComponent>(source);
            if (component == "PointLightComponent") return Canonical<PointLightComponent>(source);
            if (component == "DirectionalLightComponent") return Canonical<DirectionalLightComponent>(source);
            if (component == "EnvironmentLightComponent") return Canonical<EnvironmentLightComponent>(source);
            if (component == "PostProcessingComponent") return Canonical<PostProcessingComponent>(source);
            if (component == "PythonScriptComponent") return Canonical<PythonScriptComponent>(source);
            if (component == "ScriptComponent")
            {
                auto result = Canonical<ScriptComponent>(source);
                (void)DecodeScriptFields(result.at("Fields").get<std::string>());
                return result;
            }
            if (component == "FollowCameraComponent")
            {
                Json result = Canonical<FollowCameraComponent>(source);
                if (!result.value("bHasTarget",false)) result["TargetUUID"] = "00000000-0000-0000-0000-000000000000";
                return result;
            }
            if (component == "CapsuleCollider3DComponent") return Canonical<CapsuleCollider3DComponent>(source);
            if (component == "Rigidbody3DComponent") return Canonical<Rigidbody3DComponent>(source);
            if (component == "AnimatorComponent") return Canonical<AnimatorComponent>(source);
            if (component == "SequenceComponent") return Canonical<SequenceComponent>(source);
            if (component == "SoundComponent") return Canonical<SoundComponent>(source);
            if (component == "CharacterMovementComponent") return Canonical<CharacterMovementComponent>(source);
            throw Error("unsupported_component","No canonical authoring adapter: " + component);
        }
        std::string Parent(const Json& value)
        {
            return value.contains("TransformComponent") ?
                value.at("TransformComponent").value("ParentUuid", std::string()) : std::string();
        }
        void Shape(const Json& value, const Json& example, const std::string& field, std::string type = {})
        {
            if (type.empty())
            {
                const auto dot = field.find('.');
                if (dot != std::string::npos)
                {
                    auto owner = Reflection::TypeMeta::newMetaFromName(field.substr(0, dot));
                    auto accessor = owner.getFieldByName(field.substr(dot + 1).c_str());
                    if (accessor.getFieldName() == field.substr(dot + 1)) type = accessor.getFieldTypeName();
                }
            }
            type.erase(std::remove(type.begin(), type.end(), ' '), type.end());
            if (type.starts_with("std::vector<"))
            {
                Require(value.is_array(), "field_type", "Expected a reflected dynamic array.", field);
                const auto element = type.substr(12, type.size() - 13);
                for (size_t i = 0; i < value.size(); ++i)
                    Shape(value[i], Json(), field + "[" + std::to_string(i) + "]", element);
                return;
            }
            if (type.starts_with("std::map<") || type.starts_with("std::unordered_map<"))
            {
                const auto begin = type.find('<') + 1;
                size_t comma = begin; int depth = 0;
                for (; comma < type.size(); ++comma)
                {
                    if (type[comma] == '<') ++depth;
                    else if (type[comma] == '>') --depth;
                    else if (type[comma] == ',' && depth == 0) break;
                }
                Require(type.substr(begin, comma - begin) == "std::string", "field_type", "Only string-keyed maps are authorable.", field);
                Require(value.is_object(), "field_type", "Expected a reflected map.", field);
                const auto element = type.substr(comma + 1, type.size() - comma - 2);
                for (auto& [key, item] : value.items()) Shape(item, Json(), field + "." + key, element);
                return;
            }
            auto meta = Reflection::TypeMeta::newMetaFromName(type);
            if (meta.isValid())
            {
                Require(value.is_object(), "field_type", "Expected a reflected object.", field);
                for (auto& [key, item] : value.items())
                {
                    auto accessor = meta.getFieldByName(key.c_str());
                    Require(accessor.getFieldName() == key, "unknown_field", "Unknown reflected field.", field + "." + key);
                    Shape(item, example.is_object() && example.contains(key) ? example.at(key) : Json(),
                        field + "." + key, accessor.getFieldTypeName());
                }
                return;
            }
            if (example.is_null())
            {
                Json primitive;
                if (type == "float" || type == "double") primitive = 0.0;
                else if (type == "int" || type == "unsignedint" || type == "int32_t" || type == "uint32_t") primitive = 0;
                else if (type == "bool") primitive = false;
                else if (type == "std::string") primitive = "";
                else if (type == "glm::vec2") primitive = Json::array({0.0,0.0});
                else if (type == "glm::vec3") primitive = Json::array({0.0,0.0,0.0});
                else if (type == "glm::vec4" || type == "glm::quat") primitive = Json::array({0.0,0.0,0.0,0.0});
                Require(!primitive.is_null(), "field_type", "Unsupported reflected leaf type: " + type, field);
                Shape(value, primitive, field, type);
                return;
            }
            Require((example.is_number() && value.is_number()) || value.type() == example.type(),
                "field_type", "Value must match the reflected field type.", field);
            if (example.is_number_integer()) Require(value.is_number_integer(), "field_type", "Expected an integer.", field);
            if (value.is_number())
                Require(std::isfinite(value.get<double>()) && std::abs(value.get<double>()) <= (field == "Intensity" ? double(std::numeric_limits<float>::max()) : 1e9),
                    "field_range", "Expected a finite number within the field range.", field);
            if (example.is_array())
            {
                Require(value.size() == example.size(), "field_type", "Vector length mismatch.", field);
                for (size_t i = 0; i < value.size(); ++i) Shape(value[i], example[i], field);
            }
            if (example.is_object())
                for (auto& [key, item] : value.items())
                {
                    Require(example.contains(key), "unknown_field", "Unknown reflected field.", field + "." + key);
                    Shape(item, example.at(key), field + "." + key);
                }
        }
        void PatchComponent(Json& value, const std::string& name, const Json& patch)
        {
            Require(Supported(name), "unsupported_component", "No safe authoring adapter: " + name, name);
            const Json defaults = SceneEditingService::ComponentDefaults(name);
            Require(patch.is_object(), "field_type", "Component patch must be an object.", name);
            for (auto& [field, item] : patch.items())
            {
                Require(defaults.contains(field), "unknown_field", "Unknown reflected field.", name + "." + field);
                Require(field != "ParentUuid" && field != "MaterialOverrides" && field != "meshResPath", "read_only_field",
                    "Use the dedicated operation.", name + "." + field);
                Shape(item, defaults.at(field), name + "." + field);
                if (name == "CameraComponent" && field == "Camera")
                {
                    Require(!item.contains("mAspectRatio"), "read_only_field", "Viewport owns camera aspect ratio.", "Camera.mAspectRatio");
                    value[field].merge_patch(item);
                    const Json& camera = value.at(field);
                    Require(camera.at("mProjectionType") == 0 || camera.at("mProjectionType") == 1, "field_enum", "Projection: 0 perspective, 1 orthographic.", "Camera.mProjectionType");
                    Require(camera.at("mPerspectiveFOV").get<double>() > 0 && camera.at("mPerspectiveFOV").get<double>() < 3.141592653589793 &&
                        camera.at("mPerspectiveNear").get<double>() > 0 && camera.at("mPerspectiveFar").get<double>() > camera.at("mPerspectiveNear").get<double>() &&
                        camera.at("mOrthographicSize").get<double>() > 0 && camera.at("mOrthographicFar").get<double>() > camera.at("mOrthographicNear").get<double>(),
                        "field_range", "Invalid camera FOV, size or clip interval.", "Camera");
                    continue;
                }
                if (item.is_number() && name != "TransformComponent" && field != "ExposureEV100" &&
                    field != "ExposureCompensation" && field != "SkyExposureCompensation" && field != "FogBaseHeight" && field != "SecondaryFogBaseHeight" &&
                    !(name == "PostProcessingComponent" && field == "Temperature"))
                    Require(item.get<double>() >= 0, "field_range", "Expected a nonnegative value.", field);
                if (name == "PostProcessingComponent" && field == "Temperature")
                    Require(item >= -1 && item <= 1, "field_range", "Color temperature: -1..1", field);
                if (name == "PostProcessingComponent" && field == "VignetteIntensity")
                    Require(item >= 0 && item <= 1, "field_range", "Vignette intensity: 0..1", field);
                if (name == "PostProcessingComponent" && (field == "ColorGain" || field == "ColorOffset"))
                    for (const auto& channel : item)
                        Require(channel.get<double>() >= (field == "ColorGain" ? 0.0 : -0.25) &&
                            channel.get<double>() <= (field == "ColorGain" ? 2.0 : 0.25),
                            "field_range", "Color gain: 0..2; offset: -0.25..0.25 per channel.", field);
                if (name == "EnvironmentLightComponent")
                {
                    if (field == "FogColor" || field == "FogDirectionalColor" || field == "SkyColorTint" || field == "SecondaryFogColor")
                        for (const auto& channel : item)
                            Require(channel.get<double>() >= 0, "field_range", "Color channels must be nonnegative.", field);
                    if (field == "SkyColorTint")
                        for (const auto& channel : item)
                            Require(channel.get<double>() <= 4, "field_range", "Sky tint: 0..4 per channel.", field);
                    if (field == "SkyExposureCompensation") Require(item >= -8 && item <= 8, "field_range", "Sky exposure: -8..8", field);
                    if (field == "FogDensity") Require(item <= 2, "field_range", "Fog density: 0..2", field);
                    if (field == "FogStartDistance") Require(item <= 10000, "field_range", "Fog start: 0..10000m", field);
                    if (field == "FogBaseHeight") Require(item >= -10000 && item <= 10000, "field_range", "Fog base: -10000..10000m", field);
                    if (field == "FogHeightFalloff") Require(item <= 10, "field_range", "Fog height falloff: 0..10", field);
                    if (field == "SecondaryFogDensity") Require(item <= 2, "field_range", "Secondary fog density: 0..2", field);
                    if (field == "SecondaryFogStartDistance") Require(item <= 10000, "field_range", "Secondary fog start: 0..10000m", field);
                    if (field == "SecondaryFogBaseHeight") Require(item >= -10000 && item <= 10000, "field_range", "Secondary fog base: -10000..10000m", field);
                    if (field == "SecondaryFogHeightFalloff") Require(item <= 10, "field_range", "Secondary fog falloff: 0..10", field);
                    if (field == "FogDirectionalIntensity") Require(item <= 16, "field_range", "Sunward fog intensity: 0..16", field);
                    if (field == "FogDirectionalExponent") Require(item >= 1 && item <= 64, "field_range", "Sunward fog exponent: 1..64", field);
                }
                if (field == "CascadeCount") Require(item >= 1 && item <= 4, "field_range", "CascadeCount: 1..4", field);
                if (field == "ShadowMapSize") Require(item >= 256 && item <= 8192, "field_range", "ShadowMapSize: 256..8192", field);
                if (field == "IntensityUnits") Require(item == 0 || item == 1 || item == 2, "field_enum", "IntensityUnits: 0 unitless, 1 candela, 2 lumen", field);
                if (field == "ExposureEV100") Require(item >= -24 && item <= 32, "field_range", "EV100: -24..32", field);
                if (field == "ExposureCompensation") Require(item >= -24 && item <= 24, "field_range", "Exposure compensation: -24..24", field);
                if (field == "CascadeLambda" || field == "ShadowStrength") Require(item <= 1, "field_range", "Expected 0..1", field);
                if (field == "Tag") Require(!item.get<std::string>().empty() && item.get<std::string>().size() <= 256, "field_range", "Name: 1..256 UTF-8 bytes.", field);
                value[field] = item;
            }
        }
        std::filesystem::path ResourcePath(const std::filesystem::path& root, const std::string& path, const char* extension,
            const std::filesystem::path& engineRoot = {})
        {
            std::filesystem::path full; std::string normalized;
            Require(path.size() <= 1024, "resource_path", "Resource path exceeds 1024 UTF-8 bytes.", "path");
            const auto checked = Utils::TryResolveProjectRelativePath(root, path, full, normalized);
            Require(static_cast<bool>(checked), "resource_path", checked.Message, "path");
            Require(Utils::MakeProjectPathCacheKey(normalized).ends_with(extension), "resource_type", std::string("Expected ") + extension, "path");
            if (!std::filesystem::exists(full) && !engineRoot.empty())
            {
                const auto engineChecked = Utils::TryResolveProjectRelativePath(engineRoot,path,full,normalized);
                Require(static_cast<bool>(engineChecked), "resource_path", engineChecked.Message, "path");
            }
            Require(std::filesystem::is_regular_file(full), "resource_missing", "Resource does not exist: " + path, "path");
            return full;
        }
        void CheckMaterial(const std::filesystem::path& root, const std::string& path, const std::filesystem::path& engineRoot = {})
        {
            const auto full = ResourcePath(root,path,".hmat",engineRoot);
            try
            {
                const Json document = Utils::LoadJsonFile(full);
                Require(document.is_object(), "resource_invalid", "Material document must be an object.", "path");
                MaterialRes material; Serializer::Deserialize(document, material);
                const int version = document.value("SchemaVersion", 1);
                Require(version == 1 || version == 2, "resource_invalid", "Material SchemaVersion must be 1 or 2.", "path");
                Require(material.Type == "Default" || material.Type == "Custom" || material.Type == "CustomPBR" || material.Type == "PostProcess",
                    "resource_type", "Unknown material Type.", "path");
                std::string normalized, error;
                Require(TryNormalizeMaterialShaderPath(material.ShaderPath, normalized, error), "resource_invalid", error, "path");
                if (version == 2)
                    Require(material.Domain == "Surface" && (material.ShadingModel == "Lit" || material.ShadingModel == "Unlit") &&
                        (material.BlendMode == "Opaque" || material.BlendMode == "Mask" || material.BlendMode == "Transparent") &&
                        (material.CullMode == "Back" || material.CullMode == "None") && material.VariantKeywords.empty() &&
                        material.Parameters.is_object() && material.Textures.is_object() && material.Properties.is_object() &&
                        material.OrphanedParameters.is_object() && std::isfinite(material.AlphaCutoff), "resource_invalid", "Invalid Material V2 render state.", "path");
            }
            catch (const Error&) { throw; }
            catch (const std::exception& error) { throw Error("resource_invalid", error.what(), "path"); }
        }
        void PublishPreparedValues(Json& document, Json& prepared)
        {
            auto& destination = document.get_ref<Json::object_t&>();
            auto& source = prepared.get_ref<Json::object_t&>();
            // Both maps use the same allocator. Node transfer and Json::swap
            // publish only preallocated state after the host commit.
            while (!source.empty())
            {
                auto node = source.extract(source.begin());
                if (node.mapped().is_null()) destination.erase(node.key());
                else if (auto found = destination.find(node.key()); found != destination.end()) found->second.swap(node.mapped());
                else destination.insert(std::move(node));
            }
        }
        Json Previous(const Json& document, const Json& values)
        {
            Json result = Json::object();
            for (auto& [id, unused] : values.items()) result[id] = document.value(id, Json());
            return result;
        }
        void SafeEntity(const Json& value)
        {
            for (auto& [component, unused] : value.items())
                Require(Supported(component) || component == "PrefabInstanceComponent" || component == "EditorEntityStateComponent" || component == "SceneFolderComponent", "unsafe_lifecycle",
                    "Structural editing of this entity requires an adapter for " + component, component);
        }
        bool ReferencesAny(const Json& value, const std::set<std::string>& ids)
        {
            if (value.is_string()) return ids.contains(value.get<std::string>());
            if (value.is_structured()) for (const auto& child : value) if (ReferencesAny(child, ids)) return true;
            return false;
        }
    }

    Json SceneEditingService::ComponentDefaults(const std::string& name)
    {
        if (name == "TagComponent") return Defaults<TagComponent>();
        if (name == "TransformComponent") return Defaults<TransformComponent>();
        if (name == "PointLightComponent") return Defaults<PointLightComponent>();
        if (name == "DirectionalLightComponent") return Defaults<DirectionalLightComponent>();
        if (name == "EnvironmentLightComponent") return Defaults<EnvironmentLightComponent>();
        if (name == "PostProcessingComponent") return Defaults<PostProcessingComponent>();
        if (name == "CameraComponent") return Defaults<CameraComponent>();
        // Default Mesh is an empty CPU object; it does not load/finalize assets.
        if (name == "MeshComponent") return Defaults<MeshComponent>();
        if (name == "SpriteRendererComponent") return Defaults<SpriteRendererComponent>();
        if (name == "PythonScriptComponent") return Defaults<PythonScriptComponent>();
        if (name == "ScriptComponent") return Defaults<ScriptComponent>();
        if (name == "FollowCameraComponent") return CanonicalComponent(name,Json::object());
        if (name == "CapsuleCollider3DComponent") return Defaults<CapsuleCollider3DComponent>();
        if (name == "Rigidbody3DComponent") return Defaults<Rigidbody3DComponent>();
        if (name == "AnimatorComponent") return Defaults<AnimatorComponent>();
        if (name == "SequenceComponent") return Defaults<SequenceComponent>();
        if (name == "SoundComponent") return Defaults<SoundComponent>();
        if (name == "CharacterMovementComponent") return Defaults<CharacterMovementComponent>();
        throw Error("unsupported_component", "No safe authoring adapter: " + name, name);
    }
    void SceneEditingService::CheckThread() const
    { Require(mThread == std::this_thread::get_id(), "wrong_thread", "Use the owning main-thread frame boundary."); }
    void SceneEditingService::Open(const std::filesystem::path& root, const std::string& path)
    {
        const auto resolved = ResolveSceneDocumentPath(root, path);
        const auto parsed = ParseSceneDocument(resolved);
        Json document = Json::object();
        for (const auto& entity : parsed.GetEntities()) document[entity.Uuid] = entity.Value;
        Bind(root, path, std::move(document));
    }
    void SceneEditingService::Bind(const std::filesystem::path& root, const std::string& path, Json document, Writer writer, bool bLoadedFromDisk)
    {
        const auto resolved = ResolveSceneDocumentPath(root, path);
        mRoot = resolved.ProjectRoot; mPath = resolved.FullPath; mProjectPath = resolved.Key.Path;
        mEngineContentRoot.clear();
        if (ConfigManager::GetInstance().IsInitialized() &&
            std::filesystem::weakly_canonical(ConfigManager::GetInstance().GetRootFolder()) == mRoot)
            mEngineContentRoot = ConfigManager::GetInstance().GetEngineContentFolder();
        mSceneId = UUID().str(); mThread = std::this_thread::get_id();
        mDocument = document.is_null() ? Json::object() : std::move(document);
        mWriter = std::move(writer); mRevision = mSavedRevision = mDeltaFloor = 0;
        mHistory.clear(); mEvents.clear(); mRequests.clear(); mPrefabPlans.clear(); mCursor = mHistoryBytes = mEventBytes = 0;
        mBlocked = mPlaying = mNotificationFailed = false; mChangeObserver = {};
        mDiskExists = std::filesystem::exists(mPath);
        mDiskDocument = mDiskExists ? Utils::LoadJsonFile(mPath) : Json();
        mDiskWriteTime = mDiskExists ? std::filesystem::last_write_time(mPath) : std::filesystem::file_time_type{};
        for (auto& [id, value] : mDocument.items())
            if (value.contains("CameraComponent"))
            {
                auto& camera = value["CameraComponent"]["Camera"];
                if (mDiskDocument.is_object() && mDiskDocument.contains(id) &&
                    mDiskDocument.at(id).contains("CameraComponent"))
                {
                    const auto& savedCamera = mDiskDocument.at(id).at("CameraComponent").at("Camera");
                    if (savedCamera.contains("mAspectRatio")) camera["mAspectRatio"] = savedCamera.at("mAspectRatio");
                    else camera.erase("mAspectRatio");
                }
                else camera.erase("mAspectRatio");
            }
        mSavedDocument = !bLoadedFromDisk && mDiskExists && !mDiskDocument.is_null()
            ? mDiskDocument : mDocument;
        bDirtyCacheValid = false;
    }
    bool SceneEditingService::IsDirty() const
    {
        if (!bDirtyCacheValid)
        {
            bCachedDirty = mDocument != mSavedDocument;
            bDirtyCacheValid = true;
        }
        return bCachedDirty;
    }
    Json SceneEditingService::Receipt() const
    {
        Json result = {{"ok", true}, {"sceneId", mSceneId}, {"path", mProjectPath}, {"revision", mRevision},
            {"savedRevision", mSavedRevision}, {"dirty", IsDirty()}, {"blocked", mBlocked},
            {"completion", mWriter ? "authorCommitted" : "documentCommitted"}};
        if (mNotificationFailed) result["warnings"] = {"observer_failed; author state committed, refresh host presentation"};
        return result;
    }
    void SceneEditingService::NotifyChanged() noexcept
    {
        mNotifying = true;
        try { if (mChangeObserver) mChangeObserver(mRevision); }
        catch (...) { mNotificationFailed = true; }
        mNotifying = false;
    }

    Json SceneEditingService::Prepare(const Json& request, Json& after, Json& temporaryIds) const
    {
        const auto& operations = request.at("operations");
        Require(operations.is_array() && !operations.empty() && operations.size() <= 256,
            "operation_limit", "Expected 1..256 operations.");
        after = Json::object(); temporaryIds = Json::object();
        Json prefabRevisions = Json::object();
        auto get = [&](const std::string& id) -> const Json& {
            Require(after.contains(id) ? !after.at(id).is_null() : mDocument.contains(id),
                "entity_missing", "Entity does not exist: " + id, "entity");
            return after.contains(id) ? after.at(id) : mDocument.at(id);
        };
        auto resolve = [&](const Json& ref) -> std::string {
            Require(ref.is_string(), "invalid_reference", "Expected UUID or $temporaryId.");
            std::string id = ref.get<std::string>();
            if (id.starts_with("$"))
            { Require(temporaryIds.contains(id), "invalid_reference", "Unknown temporary reference: " + id); id = temporaryIds.at(id); }
            if (!id.empty()) (void)get(id);
            return id;
        };
        auto ids = [&]() {
            std::set<std::string> result;
            for (auto& [id, unused] : mDocument.items()) result.insert(id);
            for (auto& [id, value] : after.items()) { if (value.is_null()) result.erase(id); else result.insert(id); }
            return result;
        };
        auto world = [&](const std::string& id) {
            glm::mat4 result(1); std::set<std::string> seen; std::string current = id;
            while (!current.empty())
            {
                Require(seen.insert(current).second, "hierarchy_cycle", "Hierarchy contains a cycle.");
                Require(seen.size() <= 256, "hierarchy_depth", "Maximum hierarchy depth is 256.");
                const Json& entity = get(current);
                Require(entity.contains("TransformComponent"), "component_missing", "Parent requires TransformComponent.");
                TransformComponent transform; Serializer::Deserialize(entity.at("TransformComponent"), transform);
                result = transform.GetTransform() * result; current = transform.ParentUuid;
            }
            return result;
        };
        auto setLocal = [&](Json& value, const glm::mat4& matrix) {
            TransformComponent transform;
            Require(Math::DecomposeTransform(matrix, transform.Translation, transform.Rotation, transform.Scale),
                "unrepresentable_transform", "Matrix cannot be decomposed into TRS.");
            const glm::mat4 recomposed = transform.GetTransform();
            for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
                Require(std::isfinite(matrix[c][r]) && std::abs(matrix[c][r] - recomposed[c][r]) <= 1e-4f * std::max(1.f, std::abs(matrix[c][r])),
                    "unrepresentable_transform", "Shear or singular transform cannot be represented by TRS.");
            Json fields; Serializer::Serialize(fields, transform); fields["ParentUuid"] = Parent(value);
            value["TransformComponent"] = std::move(fields);
        };
        auto localFromWorld = [&](Json& value, const glm::mat4& matrix) {
            const glm::mat4 parent = world(Parent(value));
            Require(std::abs(glm::determinant(parent)) > 1e-8f, "singular_parent", "Parent transform is singular.");
            setLocal(value, glm::inverse(parent) * matrix);
        };
        std::map<std::string, size_t> meshSlots;
        auto checkMesh = [&](const Json& mesh) {
            const std::string path = mesh.at("Path");
            if (!meshSlots.contains(path))
            {
                const auto full = ResourcePath(mRoot, path, ".hmesh", mEngineContentRoot);
                const auto contentRoot = full.lexically_relative(mRoot).generic_string().starts_with("../") ? mEngineContentRoot : mRoot;
                auto decoded = MeshLoader::DecodeModelHMESH(full, contentRoot);
                Require(static_cast<bool>(decoded), "resource_invalid", decoded.Message, "MeshComponent.Path");
                meshSlots[path] = std::max(size_t(1), decoded.Model.MaterialPaths.size());
            }
            return meshSlots.at(path);
        };
        for (size_t index = 0; index < operations.size(); ++index)
        {
            std::string currentId;
            try
            {
                const Json& operation = operations[index];
                Keys(operation, {"op", "entity", "entities", "temporaryId", "name", "parent", "components",
                    "component", "patch", "space", "mode", "children", "slot", "path", "pointer", "revision"});
                const std::string op = operation.at("op");
                CheckOperation(operation, OperationSchema(op));
                auto loadPrefab = [&](const std::string& name) {
                    const auto path = ResolveRuntimePrefabPath(mRoot, name);
                    return RuntimePrefabJson(Utils::LoadJsonFile(path.FullPath)).GetDocument();
                };
                if (op == "prefab")
                {
                    const auto path = ResolveRuntimePrefabPath(mRoot, operation.at("path").get<std::string>());
                    auto expanded = ExpandPrefabDocument(loadPrefab(path.Key.Path), loadPrefab, path.Key.Path);
                    prefabRevisions[path.Key.Path] = expanded.Revision;
                    for (auto& [id, components] : expanded.Entities.items())
                        for (auto& [component, fields] : components.items())
                            if (Supported(component))
                            {
                                const Json defaults=ComponentDefaults(component);
                                for(auto& [field, unused] : fields.items()) Require(defaults.contains(field),"unknown_field","Unknown prefab field",component+"."+field);
                                fields=CanonicalComponent(component,fields);
                            }
                    auto instance = InstantiatePrefabDocument(expanded, path.Key.Path, [] { return UUID().str(); });
                    for (auto& [id, value] : instance.Values.items())
                    {
                        Require(!ids().contains(id), "entity_conflict", "Prefab ID collision"); SafeEntity(value);
                        for (auto& [component, fields] : value.items())
                            if (Supported(component))
                            {
                                const Json defaults = ComponentDefaults(component);
                                Require(fields.is_object(), "field_type", "Prefab component must be an object",component);
                                for (auto& [field,item] : fields.items())
                                {
                                    Require(defaults.contains(field), "unknown_field", "Unknown prefab component field",component+"."+field);
                                    if (field != "MaterialOverrides") Shape(item,defaults.at(field),component+"."+field);
                                }
                                fields = CanonicalComponent(component, fields);
                            }
                        if (value.contains("MeshComponent")) (void)checkMesh(value.at("MeshComponent"));
                    }
                    Json& value = instance.Values[instance.Root]; const Json before = value;
                    if (operation.contains("name")) PatchComponent(value["TagComponent"], "TagComponent", {{"Tag", operation.at("name")}});
                    if (operation.contains("parent")) value["TransformComponent"]["ParentUuid"] = resolve(operation.at("parent"));
                    if (operation.contains("components")) for (auto& [component, patch] : operation.at("components").items())
                    { if (!value.contains(component)) value[component] = ComponentDefaults(component); PatchComponent(value[component], component, patch); }
                    TrackPrefabOverrides(before, value);
                    for (auto& [id, item] : instance.Values.items()) after[id] = item;
                    if (operation.contains("temporaryId"))
                    {
                        const std::string name = operation.at("temporaryId");
                        Require(name.starts_with("$") && name.size() > 1 && name.size() <= 65 && !temporaryIds.contains(name), "temporary_id_conflict", "Expected unique $name");
                        temporaryIds[name] = instance.Root;
                    }
                    continue;
                }
                if (op == "create")
                {
                    Json value = {{"TagComponent", ComponentDefaults("TagComponent")},
                        {"TransformComponent", ComponentDefaults("TransformComponent")}};
                    value["TagComponent"]["Tag"] = "Entity";
                    if (operation.contains("name")) PatchComponent(value["TagComponent"], "TagComponent", {{"Tag", operation.at("name")}});
                    if (operation.contains("components"))
                        for (auto& [component, patch] : operation.at("components").items())
                        { value[component] = ComponentDefaults(component); PatchComponent(value[component], component, patch); }
                    if (operation.contains("parent")) value["TransformComponent"]["ParentUuid"] = resolve(operation.at("parent"));
                    PatchComponent(value["TagComponent"],"TagComponent",{{"Tag",value.at("TagComponent").at("Tag")}});
                    if (value.contains("MeshComponent"))
                    {
                        const auto slots = checkMesh(value.at("MeshComponent"));
                        const auto& overrides = value.at("MeshComponent").at("MaterialOverrides");
                        Require(overrides.is_array() && overrides.size() <= slots, "material_slot", "Prefab overrides exceed the decoded material table.","MaterialOverrides");
                        for (const auto& path : overrides) if (!path.get<std::string>().empty()) CheckMaterial(mRoot,path,mEngineContentRoot);
                    }
                    currentId = UUID().str(); after[currentId] = std::move(value);
                    if (operation.contains("temporaryId"))
                    {
                        std::string name = operation.at("temporaryId");
                        Require(name.size() > 1 && name.size() <= 65 && name.starts_with("$") && !temporaryIds.contains(name),
                            "temporary_id_conflict", "temporaryId must be a unique $name, at most 65 bytes.");
                        temporaryIds[name] = currentId;
                    }
                    continue;
                }
                Json targets = operation.contains("entities") ? operation.at("entities") : Json::array({operation.at("entity")});
                Require(targets.is_array() && !targets.empty() && targets.size() <= 1024, "target_limit", "Expected 1..1024 explicit entity references.");
                std::set<std::string> uniqueTargets;
                for (const auto& target : targets)
                {
                    currentId = resolve(target);
                    Require(!currentId.empty() && uniqueTargets.insert(currentId).second, "duplicate_target", "Targets must be unique nonempty UUIDs.");
                    Json value = get(currentId);
                    if (op == "prefab_unpack")
                    {
                        Require(value.contains("PrefabInstanceComponent") && value.at("PrefabInstanceComponent").at("InstanceRootUuid")==currentId,
                            "not_prefab_root","Unpack requires a prefab instance root");
                        for(const auto& id:ids())
                        {
                            Json candidate=get(id);
                            if(candidate.contains("PrefabInstanceComponent") && candidate.at("PrefabInstanceComponent").at("InstanceRootUuid")==currentId)
                            {candidate.erase("PrefabInstanceComponent"); after[id]=std::move(candidate);}
                        }
                        continue;
                    }
                    else if (op == "prefab_sync")
                    {
                        Require(value.contains("PrefabInstanceComponent"), "not_prefab_instance", "Expected a prefab instance root");
                        const std::string path = value.at("PrefabInstanceComponent").at("TemplatePath");
                        auto expanded = ExpandPrefabDocument(loadPrefab(path), loadPrefab, path);
                        prefabRevisions[path] = expanded.Revision;
                        for(auto& [id,components]:expanded.Entities.items())
                            for(auto& [component,fields]:components.items())
                                if(Supported(component))
                                {
                                    const Json defaults=ComponentDefaults(component);
                                    for(auto& [field,unused]:fields.items()) Require(defaults.contains(field),"unknown_field","Unknown prefab field",component+"."+field);
                                    fields=CanonicalComponent(component,fields);
                                }
                        if (operation.contains("revision")) Require(operation.at("revision") == expanded.Revision, "resource_conflict", "Template changed since planning");
                        Json snapshot = mDocument;
                        for (auto& [id, item] : after.items()) { if (item.is_null()) snapshot.erase(id); else snapshot[id] = item; }
                        const Json synced = SynchronizePrefabDocument(snapshot, currentId, expanded, [] { return UUID().str(); });
                        for (auto& [id, item] : synced.items()) { if (!item.is_null()) SafeEntity(item); after[id] = item; }
                        continue;
                    }
                    else if (op == "prefab_override")
                    {
                        const std::string mode = operation.at("mode");
                        Require(mode == "set" || mode == "clear", "override_mode", "mode: set/clear");
                        std::map<std::string,std::string> map;
                        Require(value.contains("PrefabInstanceComponent"), "not_prefab_instance", "Expected prefab instance");
                        const auto root = value.at("PrefabInstanceComponent").at("InstanceRootUuid");
                        for (const auto& id : ids())
                        {
                            const auto& candidate = get(id);
                            if (candidate.contains("PrefabInstanceComponent") && candidate.at("PrefabInstanceComponent").at("InstanceRootUuid") == root)
                                map[candidate.at("PrefabInstanceComponent").at("LocalId").get<std::string>()] = id;
                        }
                        SetPrefabOverride(value, operation.at("pointer"), mode == "set", map);
                    }
                    else if (op == "rename") PatchComponent(value["TagComponent"], "TagComponent", {{"Tag", operation.at("name")}});
                    else if (op == "add" || op == "patch" || op == "remove")
                    {
                        const std::string component = operation.at("component");
                        Require(Supported(component), "unsupported_component", "No safe authoring adapter: " + component);
                        Require(op == "add" ? !value.contains(component) : value.contains(component), "component_presence", "Component presence does not match operation.");
                        if (op == "remove")
                        {
                            Require(component != "TransformComponent" && component != "TagComponent", "required_component", "Name and Transform cannot be removed.");
                            value.erase(component);
                        }
                        else
                        {
                            if (op == "add") value[component] = ComponentDefaults(component);
                            PatchComponent(value[component], component, operation.at("patch"));
                            if (component == "MeshComponent") (void)checkMesh(value[component]);
                        }
                    }
                    else if (op == "transform")
                    {
                        Require(value.contains("TransformComponent"), "component_missing", "Transform required.");
                        const std::string space = operation.value("space", "local"), mode = operation.value("mode", "absolute");
                        Require((space == "local" || space == "world") && (mode == "absolute" || mode == "relative"), "invalid_transform_mode", "space: local/world; mode: absolute/relative.");
                        Json fields = mode == "relative" ? ComponentDefaults("TransformComponent") : value.at("TransformComponent");
                        if (space == "world" && mode == "absolute")
                        {
                            Json worldValue = value; setLocal(worldValue, world(currentId)); fields = worldValue.at("TransformComponent");
                        }
                        PatchComponent(fields, "TransformComponent", operation.at("patch"));
                        TransformComponent transform; Serializer::Deserialize(fields, transform);
                        glm::mat4 result = transform.GetTransform();
                        if (mode == "relative")
                        {
                            TransformComponent old; Serializer::Deserialize(value.at("TransformComponent"), old);
                            result = space == "local" ? old.GetTransform() * result : result * world(currentId);
                        }
                        if (space == "world") localFromWorld(value, result); else setLocal(value, result);
                    }
                    else if (op == "reparent")
                    {
                        const std::string mode = operation.at("mode"), parent = resolve(operation.at("parent"));
                        Require(mode == "keepLocal" || mode == "keepWorld", "invalid_transform_mode", "mode: keepLocal/keepWorld.");
                        const glm::mat4 oldWorld = world(currentId);
                        std::string ancestor = parent; std::set<std::string> seen;
                        while (!ancestor.empty())
                        { Require(ancestor != currentId && seen.insert(ancestor).second, "hierarchy_cycle", "Reparent would create a cycle."); ancestor = Parent(get(ancestor)); }
                        value["TransformComponent"]["ParentUuid"] = parent;
                        if (mode == "keepWorld") localFromWorld(value, oldWorld);
                    }
                    else if (op == "material")
                    {
                        Require(value.contains("MeshComponent"), "component_missing", "MeshComponent required.");
                        Json& mesh = value["MeshComponent"];
                        const int slot = operation.at("slot");
                        Require(slot >= 0 && static_cast<size_t>(slot) < checkMesh(mesh), "material_slot", "Slot is outside the decoded mesh material table.", "slot");
                        const std::string path = operation.at("path");
                        if (!path.empty()) CheckMaterial(mRoot, path, mEngineContentRoot);
                        auto overrides = mesh.value("MaterialOverrides", std::vector<std::string>());
                        if (overrides.size() <= static_cast<size_t>(slot)) overrides.resize(slot + 1);
                        overrides[slot] = path;
                        while (!overrides.empty() && overrides.back().empty()) overrides.pop_back();
                        mesh["MaterialOverrides"] = overrides;
                    }
                    else if (op == "delete" || op == "duplicate")
                    {
                        const std::string policy = operation.at("children");
                        Require(policy == "subtree" || policy == "reject", "children_policy", "children: subtree/reject is required.");
                        std::set<std::string> subtree{currentId}; bool changed = true;
                        while (changed)
                        {
                            changed = false;
                            for (const auto& id : ids()) if (!subtree.contains(id) && subtree.contains(Parent(get(id))))
                            { Require(policy == "subtree", "has_children", "Entity has children; explicitly request subtree."); subtree.insert(id); changed = true; }
                        }
                        // Unknown components may hide references. Fail closed rather than
                        // silently deleting/remapping a reference whose semantics are unknown.
                        for (const auto& id : subtree) SafeEntity(get(id));
                        if (op == "delete")
                            for (const auto& id : ids())
                                if (!subtree.contains(id)) Require(!ReferencesAny(get(id), subtree), "entity_referenced",
                                    "An entity outside the deletion set holds a serialized UUID reference: " + id);
                        if (op == "delete") for (const auto& id : subtree) after[id] = nullptr;
                        else
                        {
                            std::map<std::string, std::string> remap;
                            for (const auto& id : subtree) remap[id] = UUID().str();
                            for (const auto& id : subtree)
                            {
                                Json copy = get(id);
                                RemapPrefabReferences(copy, remap);
                                if (copy.contains("PrefabInstanceComponent"))
                                {
                                    const std::string instanceRoot = copy.at("PrefabInstanceComponent").at("InstanceRootUuid");
                                    Require(remap.contains(instanceRoot), "partial_prefab_duplicate", "Duplicate the whole prefab root, or explicitly unpack before copying a child");
                                    copy["PrefabInstanceComponent"]["InstanceRootUuid"] = remap.at(instanceRoot);
                                }
                                after[remap.at(id)] = std::move(copy);
                            }
                            if (operation.contains("temporaryId"))
                            {
                                const std::string name = operation.at("temporaryId");
                                Require(name.size() > 1 && name.size() <= 65 && name.starts_with("$") && !temporaryIds.contains(name), "temporary_id_conflict", "Expected unique $name, at most 65 bytes.");
                                temporaryIds[name] = remap.at(currentId);
                            }
                        }
                        continue;
                    }
                    else throw Error("unknown_operation", "Unsupported operation: " + op);
                    if (op != "prefab_override") TrackPrefabOverrides(get(currentId), value);
                    after[currentId] = std::move(value);
                }
            }
            catch (Error& error) { error.Index = static_cast<int>(index); error.Entity = currentId; throw; }
            catch (const std::exception& error)
            { Error wrapped("invalid_request", error.what()); wrapped.Index = static_cast<int>(index); wrapped.Entity = currentId; throw wrapped; }
        }
        Require(ids().size() <= 65536, "entity_limit", "Maximum scene entity count is 65536.");
        for (const auto& id : ids()) if (get(id).contains("TransformComponent")) (void)world(id);
        for (auto& [id, value] : after.items())
            if (value.is_object() && value.contains("PythonScriptComponent"))
            {
                const std::string path = value.at("PythonScriptComponent").value("Path", std::string());
                if (!path.empty() && path != "None") (void)ResourcePath(mRoot, path, ".py");
            }
        for (auto it = after.begin(); it != after.end();)
            if (mDocument.value(it.key(), Json()) == it.value()) it = after.erase(it); else ++it;
        Require(after.dump().size() <= MaxHistoryBytes / 2, "transaction_limit", "Affected entity data exceeds 8 MiB.");
        Require(after.size() <= 1024, "transaction_limit", "A transaction can affect at most 1024 entities; use explicit batches.");
        const auto finalIds = ids();
        for (auto it = temporaryIds.begin(); it != temporaryIds.end();)
            if (!finalIds.contains(it.value().get<std::string>())) it = temporaryIds.erase(it); else ++it;
        Json result = Receipt(); result["affectedCount"] = after.size(); result["applicable"] = true;
        result["prefabRevisions"] = prefabRevisions;
        result["affectedEntities"] = Json::array();
        size_t created = 0, deleted = 0;
        for (auto& [id, value] : after.items())
        {
            if (mDocument.contains(id)) result["affectedEntities"].push_back(id); else ++created;
            if (value.is_null()) ++deleted;
        }
        result["createdCount"] = created; result["deletedCount"] = deleted;
        result["completion"] = "planned"; return result;
    }

    void SceneEditingService::Write(const Json& values, const Json& rollback)
    {
        if (!mWriter) return;
        try { mWriter(values); }
        catch (...)
        {
            try { mWriter(rollback); }
            catch (...) { mBlocked = true; throw Error("recovery_failed", "Author state recovery failed; reload required before further writes."); }
            throw Error("commit_failed", "Host commit failed; author state restored.");
        }
    }
    void SceneEditingService::Publish(Change change, bool write)
    {
        Json prepared = change.After;
        change.Revision = mRevision + 1;
        mEvents.push_back(std::move(change));
        try { if (write) Write(mEvents.back().After, mEvents.back().Before); }
        catch (...) { mEvents.pop_back(); throw; }
        PublishPreparedValues(mDocument, prepared);
        bDirtyCacheValid = false;
        ++mRevision; mEventBytes += mEvents.back().Bytes;
        while (mEvents.size() > MaxHistory || mEventBytes > MaxHistoryBytes)
        { mDeltaFloor = mEvents.front().Revision; mEventBytes -= mEvents.front().Bytes; mEvents.pop_front(); }
    }
    Json SceneEditingService::Commit(Json before, Json after, bool write)
    {
        if (after.empty()) { Json result = Receipt(); result["affectedCount"] = 0; return result; }
        Change change{UUID().str(), std::move(before), std::move(after)};
        change.Bytes = change.Before.dump().size() + change.After.dump().size();
        Require(change.Bytes <= MaxHistoryBytes, "transaction_limit", "Transaction history exceeds 16 MiB.");
        // Allocate history/event/entity nodes before touching the host.
        mHistory.reserve(mHistory.size() + 1);
        mHistory.push_back(change);
        try { Publish(change, write); }
        catch (...) { mHistory.pop_back(); throw; }
        // The newly staged entry is last. Remove the obsolete redo range only
        // after successful publication; failed edits leave the branch intact.
        for (size_t index = mCursor; index + 1 < mHistory.size(); ++index) mHistoryBytes -= mHistory[index].Bytes;
        mHistory.erase(mHistory.begin() + static_cast<ptrdiff_t>(mCursor), mHistory.end() - 1);
        mHistoryBytes += change.Bytes;
        while (mHistory.size() > MaxHistory || mHistoryBytes > MaxHistoryBytes)
        { mHistoryBytes -= mHistory.front().Bytes; mHistory.erase(mHistory.begin()); }
        mCursor = mHistory.size(); NotifyChanged();
        Json result = Receipt(); result["transactionId"] = change.Id; result["affectedCount"] = change.After.size(); return result;
    }
    Json SceneEditingService::ApplyPlayScriptFields(const Json& runtimeDocument)
    {
        CheckThread();
        Require(mPlaying && !mBlocked && !mNotifying, "invalid_state", "Apply Play Values requires an active, writable Play session.");
        Json after = Json::object();
        for (const auto& [id, original] : mDocument.items())
        {
            if (!original.contains("ScriptComponent") || !runtimeDocument.contains(id) ||
                !runtimeDocument.at(id).contains("ScriptComponent")) continue;
            const auto& author = original.at("ScriptComponent");
            const auto& runtime = runtimeDocument.at(id).at("ScriptComponent");
            if (author.at("Backend") != runtime.at("Backend") || author.at("Type") != runtime.at("Type")) continue;
            const auto backend = ScriptBackendRegistry::Find(author.at("Backend").get<std::string>());
            if (!backend) continue;
            const auto types = backend->Types();
            const auto typeId = author.at("Type").get<std::string>();
            const auto type = std::find_if(types.begin(), types.end(), [&](const auto& candidate) { return candidate.Id == typeId; });
            if (type == types.end()) continue;
            auto fields = DecodeScriptFields(author.at("Fields").get<std::string>());
            const auto values = DecodeScriptFields(runtime.at("Fields").get<std::string>());
            for (const auto& field : type->Fields)
            {
                const auto value = values.find(field.Id);
                if (value == values.end()) continue;
                Require(value->second.index() == field.Default.index(), "field_type", "Play field type differs from its registration: " + field.Id);
                if (const auto* enumeration = std::get_if<ScriptEnum>(&value->second))
                    Require(enumeration->Type == std::get<ScriptEnum>(field.Default).Type, "field_type", "Play enum identity changed: " + field.Id);
                if (const auto* aggregate = std::get_if<ScriptAggregate>(&value->second))
                    Require(aggregate->Kind == std::get<ScriptAggregate>(field.Default).Kind && aggregate->Type == std::get<ScriptAggregate>(field.Default).Type,
                        "field_type", "Play aggregate identity changed: " + field.Id);
                for (const auto& entityId : ScriptReferenceIds(value->second, false))
                    Require(entityId.empty() || mDocument.contains(entityId), "runtime_reference", "Cannot apply a reference to a runtime-only entity: " + field.Id);
                fields.insert_or_assign(field.Id, value->second);
            }
            Json changed = original;
            changed["ScriptComponent"]["Fields"] = EncodeScriptFields(fields);
            if (changed != original) after[id] = std::move(changed);
        }
        auto before = Previous(mDocument, after);
        return Commit(std::move(before), std::move(after), true);
    }

    void SceneEditingService::Observe(Json document, bool knownEdit)
    {
        CheckThread(); if (mPlaying || mBlocked) return;
        Require(!mNotifying, "reentrant_write", "Observers cannot mutate the author scene.");
        if (document.is_null()) document = Json::object();
        // Viewport resize recalculates this cached projection input. It is not
        // an author edit and must not invalidate history on the following frame.
        for (auto& [id, value] : document.items())
            if (mDocument.contains(id) && value.contains("CameraComponent") && mDocument.at(id).contains("CameraComponent"))
            {
                auto& camera = value["CameraComponent"]["Camera"];
                const auto& previous = mDocument.at(id).at("CameraComponent").at("Camera");
                if (previous.contains("mAspectRatio")) camera["mAspectRatio"] = previous.at("mAspectRatio");
                else camera.erase("mAspectRatio");
            }
        Json after = Json::object();
        for (const auto& [id, value] : document.items())
        {
            const auto previous = mDocument.find(id);
            if (previous == mDocument.end() || *previous != value) after[id] = value;
        }
        for (auto& [id, unused] : mDocument.items()) if (!document.contains(id)) after[id] = nullptr;
        if (after.empty()) return;
        if (knownEdit)
        {
            bool metadataChanged = false;
            for (auto& [id, value] : after.items())
            {
                if (value.is_null() || !mDocument.contains(id)) continue;
                const Json observed = value;
                TrackPrefabOverrides(mDocument.at(id), value);
                metadataChanged |= value != observed;
            }
            // Inspector edits already changed the host. Publish newly derived
            // override metadata too, or the next observation would discard it.
            (void)Commit(Previous(mDocument, after), after, metadataChanged);
        }
        else
        {
            mDocument = std::move(document); ++mRevision; mDeltaFloor = mRevision;
            bDirtyCacheValid = false;
            mHistory.clear(); mEvents.clear(); mCursor = mHistoryBytes = mEventBytes = 0;
            NotifyChanged();
        }
    }

        Json SceneEditingService::Execute(const std::string& method, const Json& request)
    {
        std::string activeRequest;
        const uint64_t initialRevision = mRevision;
        try
        {
            CheckThread(); Require(request.is_object(), "invalid_request", "Request must be an object.");
            Require(request.dump().size() <= 256 * 1024, "request_limit", "Request exceeds 256 KiB.");
            const std::string action = method.starts_with("scene.") ? method.substr(6) : method;
            Require(std::find(Methods.begin(), Methods.end(), action) != Methods.end(), "unknown_method", "Unknown scene method.");
            if (action == "describe")
            {
                Keys(request, {"sceneId", "component", "operation"});
                if (request.contains("sceneId")) Require(request.at("sceneId") == mSceneId, "scene_mismatch", "This request targets another authoring session.");
                Json result = Receipt(); result["schemaVersion"] = "scene-edit/1";
                result["schemaFingerprint"] = SchemaFingerprint();
                result["methods"] = Methods; result["components"] = Components;
                result["operations"] = {"create", "rename", "delete", "duplicate", "add", "remove", "patch", "transform", "reparent", "material", "prefab", "prefab_sync", "prefab_override", "prefab_unpack"};
                result["prefab"] = {{"schemaVersion",2},{"maxEntities",4096},{"authorTransactionMaxEntities",1024},
                    {"referenceFields",{"TransformComponent.ParentUuid","FollowCameraComponent.TargetUUID","EditorEntityStateComponent.FolderUuid"}},
                    {"scriptReferences","Python source UUID literals cannot be remapped; use instance-root/local-ID lookup."},
                    {"syncRevision","Supply prefab_sync.revision from the template content token to reject changed templates."}};
                result["exportPrefab"] = {
                    {"method","scene.export_prefab"},{"requestSchema",ExportPrefabSchema()},
                    {"required",{"sceneId","baseRevision","requestId","entity","path"}},
                    {"path","New project-relative Assets/*.hprefab; parent directory must exist; atomic create, never overwrite."},
                    {"source","Current author document snapshot, not unsynchronized runtime ECS."},
                    {"effects","Publishes one resource; scene revision, dirty state and undo/redo remain unchanged."},
                    {"dependencies","Observed prefab metadata must match current nested dependency revisions; sync first on conflict."},
                    {"idempotency","Same sceneId/requestId/content returns the original receipt without writing again."}};
                result["limits"] = {{"operations",256},{"targets",1024},{"pageSize",128},{"historyTransactions",64},{"historyBytes",MaxHistoryBytes},{"requestIds",1024}};
                if (request.contains("component"))
                {
                    const std::string component = request.at("component"); Json fields = Json::object();
                    const Json defaults = ComponentDefaults(component);
                    for (auto& [field, value] : defaults.items())
                        fields[field] = FieldSchema(component, field, value);
                    result["fields"] = std::move(fields);
                }
                if (request.contains("operation"))
                {
                    result["operationSchema"] = OperationSchema(request.at("operation"));
                    result["semantics"] = {{"order","request order; targets are explicit UUIDs or prior $temporaryId"},
                        {"transform","TRS, Euler radians, engine distance units; local relative = old * delta, world relative = delta * oldWorld"},
                        {"delete","children=subtree or reject; unknown reference-bearing components reject structural edits"},
                        {"material","zero-based decoded .hmesh material slot; project-relative .hmat path, empty clears"},
                        {"idempotency","sceneId lifetime, 1024 requests; no eviction, capacity rejects new writes; new bind changes sceneId"}};
                }
                return result;
            }
            Require(request.at("sceneId") == mSceneId, "scene_mismatch", "This request targets another authoring session.");
            if (action == "query")
            {
                Keys(request, {"sceneId", "revision", "offset", "limit", "entities", "name", "component", "parent", "fields"});
                for (const char* number : {"offset","limit"})
                    if (request.contains(number)) Require(request.at(number).is_number_integer() && request.at(number).get<int64_t>() >= 0, "field_range", "Expected a nonnegative integer.",number);
                const size_t offset = request.value("offset", size_t(0)), limit = request.value("limit", size_t(32));
                Require(limit > 0 && limit <= 128, "output_limit", "limit: 1..128");
                Require(offset == 0 || request.contains("revision"), "revision_required", "Pagination requires the previous page revision.");
                if (request.contains("entities")) Require(request.at("entities").is_array() && request.at("entities").size() <= 1024, "target_limit", "entities must be an array of at most 1024 UUIDs.");
                Require((offset == 0 && !request.contains("revision")) || request.at("revision") == mRevision, "revision_conflict", "Query revision changed; restart pagination.");
                Json fields = request.value("fields", Json::array({"uuid","name"}));
                Require(fields.is_array() && fields.size() <= 8, "field_projection", "fields must be an array of at most 8 names.");
                for (const auto& field : fields) Require(field == "uuid" || field == "name" || field == "parent" || field == "components" || Supported(field.get<std::string>()), "field_projection", "Unsupported projection field.");
                Json rows = Json::array(); size_t matched = 0, bytes = 0; bool more = false;
                for (auto& [id, entity] : mDocument.items())
                {
                    if (request.contains("entities") && std::find(request.at("entities").begin(), request.at("entities").end(), Json(id)) == request.at("entities").end()) continue;
                    const std::string name = entity.at("TagComponent").at("Tag");
                    if (request.contains("name") && name.find(request.at("name").get<std::string>()) == std::string::npos) continue;
                    if (request.contains("component") && !entity.contains(request.at("component").get<std::string>())) continue;
                    if (request.contains("parent") && Parent(entity) != request.at("parent").get<std::string>()) continue;
                    if (matched++ < offset) continue;
                    Json row = Json::object();
                    for (const auto& fieldValue : fields)
                    {
                        const std::string field = fieldValue;
                        if (field == "uuid") row[field] = id;
                        else if (field == "name") row[field] = name;
                        else if (field == "parent") row[field] = Parent(entity);
                        else if (field == "components") { row[field] = Json::array(); for (auto& [c, unused] : entity.items()) row[field].push_back(c); }
                        else if (entity.contains(field)) row[field] = entity.at(field);
                    }
                    if (rows.size() == limit || bytes + row.dump().size() > 48 * 1024) { more = true; break; }
                    bytes += row.dump().size(); rows.push_back(std::move(row));
                }
                Require(!more || !rows.empty(), "output_limit", "Projected entity exceeds output limit; select fewer fields.");
                Json result = Receipt(); result["entities"] = rows;
                result["nextOffset"] = more ? Json(offset + rows.size()) : Json(); return result;
            }
            if (action == "diff")
            {
                Keys(request, {"sceneId","revision","sinceRevision","transactionId","offset","limit"});
                for (const char* number : {"offset","limit","sinceRevision"})
                    if (request.contains(number)) Require(request.at(number).is_number_integer() && request.at(number).get<int64_t>() >= 0, "field_range", "Expected a nonnegative integer.",number);
                Require(!request.contains("revision") || request.at("revision") == mRevision, "revision_conflict", "Restart diff pagination.");
                const uint64_t since = request.value("sinceRevision", mDeltaFloor);
                Require(since >= mDeltaFloor && since <= mRevision, "delta_expired", "Incremental history unavailable; query again.");
                const size_t offset = request.value("offset", size_t(0)), limit = request.value("limit", size_t(16));
                Require(offset == 0 || request.contains("revision"), "revision_required", "Pagination requires revision.");
                Require(limit > 0 && limit <= 128, "output_limit", "limit: 1..128");
                Json rows = Json::array(); size_t count = 0, bytes = 0; bool found = !request.contains("transactionId"), more = false;
                for (const auto& event : mEvents)
                {
                    if (request.contains("transactionId") && request.at("transactionId") != event.Id) continue;
                    found = true; if (event.Revision <= since) continue;
                    for (auto& [id, value] : event.After.items())
                    {
                        if (count++ < offset) continue;
                        Json row = {{"transactionId",event.Id},{"revision",event.Revision},{"entity",id},{"before",event.Before.at(id)},{"after",value}};
                        if (rows.size() == limit || bytes + row.dump().size() > 48 * 1024) { more = true; break; }
                        bytes += row.dump().size(); rows.push_back(std::move(row));
                    }
                    if (more) break;
                }
                Require(found, "delta_expired", "Transaction is no longer retained; query again.");
                Require(!more || !rows.empty(), "output_limit", "Entity diff exceeds output limit; query projected fields.");
                Json result = Receipt(); result["changes"] = rows; result["nextOffset"] = more ? Json(offset + rows.size()) : Json(); return result;
            }
            if (action == "validate")
            {
                Keys(request, {"sceneId","entities"});
                size_t checked = 0; std::set<std::string> unsupported;
                if (request.contains("entities"))
                {
                    Require(request.at("entities").is_array() && request.at("entities").size() <= 1024, "target_limit", "entities: at most 1024 UUIDs.");
                    for (const auto& id : request.at("entities")) Require(id.is_string() && mDocument.contains(id.get<std::string>()), "entity_missing", "Validation target does not exist.");
                }
                for (auto& [id, entity] : mDocument.items())
                {
                    if (request.contains("entities") && std::find(request.at("entities").begin(),request.at("entities").end(),Json(id)) == request.at("entities").end()) continue;
                    ++checked;
                    Require(UUID(id).isValid(), "invalid_uuid", "Invalid scene UUID.");
                    Require(entity.contains("TagComponent") && entity.at("TagComponent").contains("Tag") && entity.at("TagComponent").at("Tag").is_string(), "required_component", "TagComponent.Tag is required.");
                    for (auto& [component, fields] : entity.items())
                    {
                        if (!Supported(component)) { unsupported.insert(component); continue; }
                        const Json defaults = ComponentDefaults(component);
                        Require(fields.is_object(), "field_type", "Component value must be an object.",component);
                        for (auto& [field,value] : fields.items())
                        {
                            Require(defaults.contains(field), "unknown_field", "Unknown component field.",component + "." + field);
                            if (component == "MeshComponent" && field == "MaterialOverrides")
                            {
                                Require(value.is_array(), "field_type", "MaterialOverrides must be an array.",field);
                                for (const auto& path : value) { Require(path.is_string(), "field_type", "Material override must be a string.",field); if (!path.get<std::string>().empty()) CheckMaterial(mRoot,path,mEngineContentRoot); }
                            }
                            else Shape(value,defaults.at(field),component + "." + field);
                        }
                        Json editable = fields;
                        editable.erase("ParentUuid"); editable.erase("MaterialOverrides"); editable.erase("meshResPath");
                        if (component == "CameraComponent" && editable.contains("Camera")) editable["Camera"].erase("mAspectRatio");
                        Json checkedValue = defaults; PatchComponent(checkedValue,component,editable);
                        if (component == "MeshComponent" && fields.value("Path",std::string("None")) != "None")
                        {
                            const auto full = ResourcePath(mRoot,fields.at("Path"),".hmesh",mEngineContentRoot);
                            const auto contentRoot = full.lexically_relative(mRoot).generic_string().starts_with("../") ? mEngineContentRoot : mRoot;
                            auto decoded = MeshLoader::DecodeModelHMESH(full,contentRoot);
                            Require(static_cast<bool>(decoded), "resource_invalid", decoded.Message,component + ".Path");
                            Require(fields.value("MaterialOverrides",Json::array()).size() <= std::max(size_t(1),decoded.Model.MaterialPaths.size()), "material_slot", "Override exceeds mesh slot count.");
                        }
                    }
                    std::set<std::string> seen; std::string parent = id;
                    while (!parent.empty())
                    { Require(mDocument.contains(parent), "dangling_reference", "Missing parent: " + parent); Require(seen.insert(parent).second, "hierarchy_cycle", "Hierarchy cycle."); parent = Parent(mDocument.at(parent)); }
                }
                Json result = Receipt(); result["checkedCount"] = checked;
                if (!unsupported.empty()) result["warnings"] = {{"unvalidatedComponents",std::vector<std::string>(unsupported.begin(),unsupported.end())}};
                return result;
            }
            Keys(request, {"sceneId","baseRevision","requestId","operations","path","entity"});
            Require(action == "save" || action == "export_prefab" || !request.contains("path"), "unknown_field", "Only save/export_prefab accepts a destination path.","path");
            Require(action == "export_prefab" || !request.contains("entity"), "unknown_field", "Only export_prefab accepts a root entity.","entity");
            Require(action == "apply" || action == "plan" || !request.contains("operations"), "unknown_field", "This control method does not accept operations.","operations");
            Require(!mPlaying, "authoring_during_play", "Stop play/preview before editing the author scene.");
            Require(!mNotifying, "reentrant_write", "Observers cannot mutate the author scene.");
            Require(!mBlocked, "recovery_required", "Reload author scene after recovery failure.");
            const std::string requestId = request.value("requestId", std::string());
            const std::string fingerprint = action + ":" + request.dump();
            if (action != "plan")
            {
                Require(!requestId.empty() && requestId.size() <= 128, "request_id_required", "requestId: 1..128 UTF-8 bytes.");
                if (const auto found = mRequests.find(requestId); found != mRequests.end())
                { Require(found->second.first == fingerprint, "idempotency_conflict", "requestId was used with different content."); return found->second.second; }
                Require(mRequests.size() < 1024, "idempotency_capacity", "Request retention is full; save and open a new session. Old sceneId cannot be retried there.");
                // Reserve before any host mutation. Even an unexpected failure
                // cannot turn a retry into a second application.
                mRequests.emplace(requestId, std::make_pair(fingerprint,
                    Json{{"ok",false},{"sceneId",mSceneId},{"error",{{"code","request_incomplete"},{"message","Request did not produce a receipt; query author state."}}}}));
                activeRequest = requestId;
            }
            Require(request.at("baseRevision") == mRevision, "revision_conflict", "baseRevision is stale; query and plan again.");
            Json result;
            if (action == "plan" || action == "apply")
            {
                Json after, temporaryIds; result = Prepare(request, after, temporaryIds);
                if (action == "plan")
                {
                    if (!result.at("prefabRevisions").empty())
                    {
                        Require(!requestId.empty(), "request_id_required", "Prefab plan needs requestId to guard dependency changes");
                        Require(mPrefabPlans.size() < 256 || mPrefabPlans.contains(requestId), "plan_capacity", "Apply existing plans or reopen the authoring session before planning more prefabs");
                        mPrefabPlans[requestId] = result.at("prefabRevisions");
                    }
                    return result;
                }
                if (mPrefabPlans.contains(requestId))
                    Require(mPrefabPlans.at(requestId) == result.at("prefabRevisions"), "resource_conflict", "Prefab dependencies changed after plan; plan again before applying");
                result = Commit(Previous(mDocument, after), after, true); result["temporaryIds"] = temporaryIds;
                mPrefabPlans.erase(requestId);
            }
            else if (action == "undo" || action == "redo")
            {
                Require(action == "undo" ? CanUndo() : CanRedo(), "history_empty", "No retained transaction in that direction.");
                const Change& original = mHistory[action == "undo" ? mCursor - 1 : mCursor];
                Change event{UUID().str(), action == "undo" ? original.After : original.Before,
                    action == "undo" ? original.Before : original.After, 0, original.Bytes};
                Publish(event, true);
                if (action == "undo") --mCursor; else ++mCursor;
                NotifyChanged();
                result = Receipt(); result["transactionId"] = event.Id; result["affectedCount"] = event.After.size();
            }
            else if (action == "export_prefab")
            {
                Require(request.contains("entity") && request.at("entity").is_string() &&
                    request.contains("path") && request.at("path").is_string(),
                    "required_field", "export_prefab requires string entity and path.");
                Require(request.at("baseRevision").is_number_integer(), "field_type", "baseRevision must be an integer.", "baseRevision");
                const std::string rootId = request.at("entity");
                Require(mDocument.contains(rootId), "entity_missing", "Prefab export root is absent from the author document.", "entity");
                const std::string requested = request.at("path");
                Require(!std::filesystem::path(requested).is_absolute() && requested.find(':') == std::string::npos,
                    "export_path", "Prefab export requires a project-relative Assets path.", "path");
                // Reject link components before canonical resolution can hide an existing leaf.
                auto lexicalTarget = mRoot;
                for (const auto& component : std::filesystem::path(requested))
                {
                    lexicalTarget /= component;
                    Require(!std::filesystem::is_symlink(std::filesystem::symlink_status(lexicalTarget)),
                        "export_path", "Prefab export does not traverse symbolic links.", "path");
                }
                const auto target = ResolveRuntimePrefabPath(mRoot, requested);
                const auto assetsRoot = std::filesystem::weakly_canonical(mRoot / "Assets");
                const auto relative = std::filesystem::weakly_canonical(target.FullPath).lexically_relative(assetsRoot);
                Require(target.Key.CachePath.starts_with("assets/") && !relative.empty() && !relative.is_absolute() &&
                    *relative.begin() != "..", "export_path", "Prefab export must remain below project Assets.", "path");
                Require(std::filesystem::is_directory(target.FullPath.parent_path()), "export_path", "Prefab export parent directory must exist.", "path");
                Require(!std::filesystem::exists(std::filesystem::symlink_status(target.FullPath)),
                    "export_exists", "Prefab export never overwrites an existing destination.", "path");
                std::set<std::string> included{rootId};
                bool changed = true;
                while (changed)
                {
                    changed = false;
                    for (const auto& [id, value] : mDocument.items())
                        if (!included.contains(id) && included.contains(Parent(value)))
                        { included.insert(id); changed = true; }
                    Require(included.size() <= 4096, "export_limit", "Prefab subtree exceeds 4096 entities.");
                }
                std::map<std::string, Json> dependencies;
                auto loadPrefab = [&](const std::string& name) {
                    const auto path = ResolveRuntimePrefabPath(mRoot, name);
                    auto found = dependencies.find(path.Key.Path);
                    if (found == dependencies.end())
                        found = dependencies.emplace(path.Key.Path, RuntimePrefabJson(Utils::LoadJsonFile(path.FullPath)).GetDocument()).first;
                    return found->second;
                };
                std::map<std::string, std::string> revisions;
                for (const auto& id : included)
                {
                    const Json& value = mDocument.at(id);
                    if (!value.contains("PrefabInstanceComponent")) continue;
                    const Json& metadata = value.at("PrefabInstanceComponent");
                    const std::string path = metadata.at("TemplatePath");
                    if (!revisions.contains(path))
                        revisions.emplace(path, ExpandPrefabDocument(loadPrefab(path), loadPrefab, path).Revision);
                    Require(metadata.at("TemplateRevision") == revisions.at(path), "resource_conflict",
                        "Prefab dependency changed since the author snapshot; sync before exporting.", "TemplateRevision");
                }
                // This is the same pure subtree capture used by PrefabSerializer::SavePrefab.
                const Json prefab = CapturePrefabDocument(mDocument, rootId, target.Key.Path);
                const auto expanded = ExpandPrefabDocument(prefab, loadPrefab, target.Key.Path);
                Require(prefab.dump().size() <= MaxHistoryBytes, "export_limit", "Prefab output exceeds 16 MiB.");
                for (const auto& [name, document] : dependencies)
                    Require(RuntimePrefabJson(Utils::LoadJsonFile(ResolveRuntimePrefabPath(mRoot, name).FullPath)).GetDocument() == document,
                        "resource_conflict", "Prefab dependency changed during export preparation.", name);
                // Allocate the successful receipt before the only filesystem mutation.
                result = Receipt(); result["completion"] = "exported";
                result["path"] = target.Key.Path; result["rootEntity"] = rootId;
                result["sourceRevision"] = mRevision; result["source"] = "author-document-snapshot";
                result["entityCount"] = expanded.Entities.size(); result["prefabRevision"] = expanded.Revision;
                result["dependencyRevisions"] = revisions;
                Json cachedResult = result;
                try { Utils::SaveJsonToFile(prefab, target.FullPath, Utils::AtomicWriteMode::CreateNew); }
                catch (const std::exception& error) { throw Error("export_publish_failed", error.what(), "path"); }
                mRequests.at(requestId).second.swap(cachedResult);
                return result;
            }
            else if (action == "save")
            {
                auto target = mPath; std::string targetProjectPath = mProjectPath;
                if (request.contains("path"))
                {
                    const auto resolved = ResolveSceneDocumentPath(mRoot,request.at("path").get<std::string>());
                    target = resolved.FullPath; targetProjectPath = resolved.Key.Path;
                }
                const bool retarget = target != mPath;
                const bool exists = std::filesystem::exists(target);
                Require(!retarget || !exists, "external_file_changed", "Save As requires a new path; open existing destinations explicitly.");
                Require(retarget || (exists == mDiskExists && (!exists || (std::filesystem::last_write_time(target) == mDiskWriteTime &&
                    Utils::LoadJsonFile(mPath) == mDiskDocument))), "external_file_changed", "Scene file changed outside this session; refusing overwrite.");
                Json diskDocument = mDocument, savedDocument = mDocument;
                Utils::SaveJsonToFile(mDocument, target, exists ? Utils::AtomicWriteMode::ReplaceExisting : Utils::AtomicWriteMode::CreateNew);
                mPath.swap(target); mProjectPath.swap(targetProjectPath);
                mDiskDocument.swap(diskDocument); mSavedDocument.swap(savedDocument);
                bDirtyCacheValid = false;
                mDiskExists = true; mSavedRevision = mRevision;
                std::error_code metadataError;
                mDiskWriteTime = std::filesystem::last_write_time(mPath,metadataError);
                result = Receipt(); result["completion"] = "saved";
                if (metadataError) result["warnings"] = {"Saved, but file metadata could not be read; reopen before saving again."};
            }
            mRequests.at(requestId).second = result; return result;
        }
        catch (const Error& error)
        {
            Json result = Receipt(); result["ok"] = false;
            result["error"] = {{"code",error.Code},{"operationIndex",error.Index},{"entity",error.Entity},{"field",error.Field},{"message",error.what()}};
            if (!activeRequest.empty()) mRequests.at(activeRequest).second = result;
            return result;
        }
        catch (const std::exception& error)
        {
            if (!activeRequest.empty() && mRevision != initialRevision) mBlocked = true;
            Json result = Receipt(); result["ok"] = false;
            result["error"] = {{"code",mBlocked ? "recovery_required" : "invalid_request"},{"operationIndex",-1},{"entity",""},{"field",""},{"message",error.what()}};
            if (!activeRequest.empty()) mRequests.at(activeRequest).second = result;
            return result;
        }
    }
}
