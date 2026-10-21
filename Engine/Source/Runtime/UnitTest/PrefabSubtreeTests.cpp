#include "Hepch.h"

#include "Runtime/UnitTest/PrefabSubtreeTests.h"
#include "Runtime/EcsFramework/Serializer/PrefabDocument.h"
#include "Runtime/EcsFramework/Serializer/SceneEditingService.h"
#include "Runtime/EcsFramework/Component/Basic/PrefabInstanceComponent.h"
#include "Runtime/Core/Meta/Serializer/Serializer.h"
#include "Runtime/Core/UUID.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "all_serializer.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace HEngine::Tests
{
    void PrefabSubtreeContract()
    {
        auto require = [](bool condition, const char* message) { if (!condition) throw std::runtime_error(message); };
        auto rejects = [&](const auto& action, const char* message) {
            bool rejected = false; try { action(); } catch (const std::exception&) { rejected = true; } require(rejected, message);
        };
        auto node = [](const char* name, const char* parent) {
            Json value;
            value["TagComponent"] = {{"Tag", name}};
            value["TransformComponent"] = {{"ParentUuid", parent}, {"Translation", {0,0,0}},
                {"Rotation", {0,0,0}}, {"Scale", {1,1,1}}};
            return value;
        };
        Json source = {{"schemaVersion",2},{"root","character"},{"entities",{
            {"character",node("Character","")},{"socket",node("WeaponSocket","character")},
            {"camera",node("Camera","character")}}},{"nested",Json::array()}};
        source["entities"]["camera"]["FollowCameraComponent"] = {{"bHasTarget",true},{"TargetUUID","socket"}};
        source["entities"]["socket"]["PythonScriptComponent"] = {{"Path","Scripts/PresentationDemo.py"}};
        auto noLoader = [](const std::string&) -> Json { throw std::runtime_error("unexpected dependency"); };
        const auto expanded = ExpandPrefabDocument(source,noLoader,"Assets/Prefabs/Character.hprefab");
        int serial = 0;
        auto ids = [&] { return "instance-" + std::to_string(++serial); };
        auto first = InstantiatePrefabDocument(expanded,"Assets/Prefabs/Character.hprefab",ids);
        auto second = InstantiatePrefabDocument(expanded,"Assets/Prefabs/Character.hprefab",ids);
        require(first.Values.size()==3 && second.Values.size()==3 && first.Root!=second.Root,"two independent subtrees");
        auto find = [](const Json& values,const char* local) {
            for(auto& [id,value]:values.items()) if(value.at("PrefabInstanceComponent").at("LocalId")==local) return id;
            throw std::runtime_error("local ID absent");
        };
        const auto socket = find(first.Values,"socket"), camera = find(first.Values,"camera");
        require(first.Values.at(camera).at("FollowCameraComponent").at("TargetUUID")==socket,"internal camera target remap");
        require(first.Values.at(socket).at("TransformComponent").at("ParentUuid")==first.Root,"parent remap");
        require(first.Values.at(socket).at("PythonScriptComponent").at("Path")=="Scripts/PresentationDemo.py","script path preserved");
        Json scene = first.Values; scene.update(second.Values);
        const Json before = scene.at(socket);
        scene[socket]["TagComponent"]["Tag"]="Local socket";
        TrackPrefabOverrides(before,scene[socket]);
        Json next = source;
        next["entities"]["character"]["TagComponent"]["Tag"]="Shared revised";
        next["entities"]["socket"]["TagComponent"]["Tag"]="Shared socket";
        next["entities"]["new"] = node("New child","character");
        const auto revised = ExpandPrefabDocument(next,noLoader);
        auto changes = SynchronizePrefabDocument(scene,first.Root,revised,ids);
        require(changes.at(socket).at("TagComponent").at("Tag")=="Local socket","local override survives template update");
        require(changes.at(first.Root).at("TagComponent").at("Tag")=="Shared revised","unmodified value propagates");
        require(changes.size()==4 && !changes.contains(second.Root),"new node and instance isolation");
        scene.update(changes);
        SetPrefabOverride(scene[socket],"/TagComponent/Tag",false);
        require(scene.at(socket).at("TagComponent").at("Tag")=="Shared socket","clear override restores current template baseline");
        SetPrefabOverride(scene[socket],"/TagComponent/Tag",true);
        Json deleted = next; deleted["entities"]["socket"].erase("TagComponent");
        rejects([&]{(void)SynchronizePrefabDocument(scene,first.Root,ExpandPrefabDocument(deleted,noLoader),ids);},"deleting explicitly overridden field must conflict");
        Json removed = next; removed["entities"].erase("socket"); removed["entities"]["camera"]["FollowCameraComponent"]["TargetUUID"]="character";
        rejects([&]{(void)SynchronizePrefabDocument(scene,first.Root,ExpandPrefabDocument(removed,noLoader),ids);},"deleting overridden entity must conflict");
        Json external = source; external["entities"]["camera"]["FollowCameraComponent"]["TargetUUID"]="outside";
        rejects([&]{(void)ExpandPrefabDocument(external,noLoader);},"unknown external ref rejected");
        Json cycle = source; cycle["entities"]["character"]["TransformComponent"]["ParentUuid"]="socket";
        rejects([&]{(void)ExpandPrefabDocument(cycle,noLoader);},"hierarchy cycle rejected");
        const auto legacy = ExpandPrefabDocument(node("Legacy","old-parent"),noLoader);
        require(legacy.Entities.size()==1 && legacy.Entities.at("root").at("TransformComponent").at("ParentUuid")=="","legacy root detached");
        const auto legacyFirst = InstantiatePrefabDocument(legacy,"legacy.hprefab",ids);
        const auto legacySecond = InstantiatePrefabDocument(legacy,"legacy.hprefab",ids);
        require(legacyFirst.Values.at(legacyFirst.Root)==node("Legacy","") &&
            legacySecond.Values.at(legacySecond.Root)==node("Legacy","") && legacyFirst.Root!=legacySecond.Root,
            "legacy instances retain the exact component object without v2 metadata");

        Json leaf = {{"schemaVersion",2},{"root","leaf"},{"entities",{{"leaf",node("Leaf","")},{"tip",node("Tip","leaf")}}}};
        Json parent = {{"schemaVersion",2},{"root","outer"},{"entities",{{"outer",node("Outer","")}}},
            {"nested",Json::array({{{"id","mount"},{"path","leaf.hprefab"},{"parent","outer"}}})}};
        auto loader = [&](const std::string& path) -> Json { return path=="leaf.hprefab"?leaf:parent; };
        const auto nested = ExpandPrefabDocument(parent,loader,"parent.hprefab");
        require(nested.Entities.size()==3 && nested.Entities.contains("mount/tip"),"nested qualified identities");
        const auto nestedInstance = InstantiatePrefabDocument(nested,"parent.hprefab",ids);
        const auto captured = CapturePrefabDocument(nestedInstance.Values,nestedInstance.Root,"parent.hprefab");
        require(captured.at("nested").size()==1 && captured.at("entities").size()==1,"resave preserves nested dependency");
        require(ExpandPrefabDocument(captured,loader).Entities==nested.Entities,"nested save expand roundtrip");
        const auto legacyLoader = [&](const std::string&) -> Json { return node("LegacyNested","old-parent"); };
        const auto nestedLegacy = InstantiatePrefabDocument(ExpandPrefabDocument(parent,legacyLoader,"parent.hprefab"),"parent.hprefab",ids);
        const auto mountedLegacy = find(nestedLegacy.Values,"mount");
        require(nestedLegacy.Values.at(mountedLegacy).at("PrefabInstanceComponent").at("InstanceRootUuid")==nestedLegacy.Root &&
            nestedLegacy.Values.at(mountedLegacy).at("TransformComponent").at("ParentUuid")==nestedLegacy.Root,
            "v2 parent retains managed identity and remapped parent for a nested legacy prefab");
        leaf["nested"] = Json::array({{{"id","cycle"},{"path","parent.hprefab"},{"parent","leaf"}}});
        rejects([&]{(void)ExpandPrefabDocument(parent,loader,"parent.hprefab");},"resource dependency cycle rejected");
        PrefabInstanceComponent metadata;
        Serializer::Deserialize(first.Values.at(first.Root).at("PrefabInstanceComponent"),metadata);
        Json roundtrip; Serializer::Serialize(roundtrip,metadata);
        require(roundtrip==first.Values.at(first.Root).at("PrefabInstanceComponent"),"reflection instance metadata roundtrip");

        const auto project = std::filesystem::temp_directory_path()/("HEngine_Prefab_"+UUID().str());
        std::filesystem::create_directories(project/"Assets/Prefabs");
        struct Cleanup { std::filesystem::path Path; ~Cleanup(){std::error_code error; std::filesystem::remove_all(Path,error);} } cleanup{project};
        const auto file = project/"Assets/Prefabs/Test.hprefab";
        Json simple = source; simple["entities"]["socket"].erase("PythonScriptComponent"); simple["entities"].erase("camera");
        const Json sprite = {{"Color",{0.25,0.5,0.75,1.0}},{"Texture",""},{"TilingFactor",2.0}};
        simple["entities"]["socket"]["SpriteRendererComponent"] = sprite;
        Utils::SaveJsonToFile(simple,file);
        SceneEditingService service; service.Bind(project,"Assets/Scenes/Test.he",Json::object());
        auto request = [&](Json operations){return Json{{"sceneId",service.GetSceneId()},{"baseRevision",service.GetRevision()},{"requestId",UUID().str()},{"operations",operations}};};
        const Json instantiate = request(Json::array({{{"op","prefab"},{"path","Assets/Prefabs/Test.hprefab"},{"temporaryId","$root"}}}));
        const auto plan=service.Execute("plan",instantiate); require(plan.at("ok") && service.GetDocument().empty(),"prefab plan remains pure");
        const auto applied=service.Execute("apply",instantiate); require(applied.at("ok") && service.GetDocument().size()==2,"prefab scene apply");
        const auto spriteId=find(service.GetDocument(),"socket");
        require(service.GetDocument().at(spriteId).at("SpriteRendererComponent")==sprite,
            "prefab SpriteRenderer author fields survive structural apply");
        require(service.Execute("apply",instantiate)==applied,"prefab request idempotency");
        auto undo=service.Execute("undo",{{"sceneId",service.GetSceneId()},{"baseRevision",service.GetRevision()},{"requestId",UUID().str()}});
        require(undo.at("ok") && service.GetDocument().empty(),"subtree undo atomic");
        const auto redo=service.Execute("redo",{{"sceneId",service.GetSceneId()},{"baseRevision",service.GetRevision()},{"requestId",UUID().str()}});
        require(redo.at("ok") && service.GetDocument().size()==2,"subtree redo atomic");
        std::filesystem::create_directories(project/"Assets/Scenes");
        const auto save=service.Execute("save",{{"sceneId",service.GetSceneId()},{"baseRevision",service.GetRevision()},{"requestId",UUID().str()}});
        require(save.at("ok"),"prefab scene save");
        SceneEditingService reopened; reopened.Open(project,"Assets/Scenes/Test.he");
        require(reopened.GetDocument()==service.GetDocument(),"scene reopen keeps stable IDs and override metadata");
        require(reopened.GetDocument().at(spriteId).at("SpriteRendererComponent")==sprite,
            "SpriteRenderer fields survive subtree undo redo save reopen");
        const Json unchanged=service.GetDocument();
        Json observedHost = unchanged;
        int observationWrites = 0;
        SceneEditingService inspector;
        inspector.Bind(project, "Assets/Scenes/Test.he", unchanged, [&](const Json& values) {
            ++observationWrites;
            for (const auto& [id, value] : values.items()) observedHost[id] = value;
        });
        observedHost[spriteId]["SpriteRendererComponent"]["TilingFactor"] = 3.0;
        inspector.Observe(observedHost, true);
        const Json inspected = inspector.GetDocument();
        require(observationWrites == 1 && observedHost == inspected,
            "direct inspector edit publishes derived override metadata back to host");
        const auto overridePaths = Json::parse(inspected.at(spriteId).at("PrefabInstanceComponent").at("OverridePathsJson").get<std::string>());
        require(std::find(overridePaths.begin(), overridePaths.end(), "/SpriteRendererComponent/TilingFactor") != overridePaths.end(),
            "direct inspector edit records exact overridden field");
        const auto observedRevision = inspector.GetRevision();
        inspector.Observe(observedHost, false);
        require(inspector.GetRevision() == observedRevision && inspector.CanUndo(), "host observation retains inspector history");
        auto inspectorControl = [&](const char* method) { return inspector.Execute(method, {{"sceneId", inspector.GetSceneId()},
            {"baseRevision", inspector.GetRevision()}, {"requestId", UUID().str()}}); };
        require(inspectorControl("undo").at("ok") && observedHost == unchanged, "inspector undo restores value and metadata");
        require(inspectorControl("redo").at("ok") && observedHost == inspected, "inspector redo restores value and metadata");
        const auto reverted = inspector.Execute("apply", {{"sceneId", inspector.GetSceneId()}, {"baseRevision", inspector.GetRevision()},
            {"requestId", UUID().str()}, {"operations", Json::array({{{"op", "prefab_override"}, {"entity", spriteId},
                {"pointer", "/SpriteRendererComponent/TilingFactor"}, {"mode", "clear"}}})}});
        require(reverted.at("ok") && observedHost == unchanged, "revert direct inspector override restores template baseline");
        const auto pending=request(Json::array({{{"op","prefab"},{"path","Assets/Prefabs/Test.hprefab"}}}));
        require(service.Execute("plan",pending).at("ok"),"dependency conflict setup plan");
        simple["entities"]["character"]["TagComponent"]["Tag"]="Changed externally";
        Utils::SaveJsonToFile(simple,file);
        const auto conflict=service.Execute("apply",pending);
        require(!conflict.at("ok").get<bool>() && conflict.at("error").at("code")=="resource_conflict" && service.GetDocument()==unchanged,"template mutation after plan rejected atomically");
        std::filesystem::create_directories(project/"Scripts");
        { std::ofstream script(project/"Scripts/Presentation.py"); script << "# authored presentation script\n"; }
        const std::string rootId=applied.at("temporaryIds").at("$root");
        const auto scriptEdit=service.Execute("apply",request(Json::array({{{"op","add"},{"entity",rootId},
            {"component","PythonScriptComponent"},{"patch",{{"Path","Scripts/Presentation.py"}}}}})));
        require(scriptEdit.at("ok") && service.GetDocument().at(rootId).at("PythonScriptComponent").at("Path")=="Scripts/Presentation.py","PythonScript author transaction");
        require(service.Execute("undo",{{"sceneId",service.GetSceneId()},{"baseRevision",service.GetRevision()},{"requestId",UUID().str()}}).at("ok") && service.GetDocument()==unchanged,"PythonScript undo restores scene");
        const auto spritePatch=service.Execute("apply",request(Json::array({{{"op","patch"},{"entity",spriteId},
            {"component","SpriteRendererComponent"},{"patch",{{"Color",{1.0,0.0,0.0,0.5}}}}}})));
        require(spritePatch.at("ok") && service.GetDocument().at(spriteId).at("SpriteRendererComponent").at("TilingFactor")==2.0,
            "SpriteRenderer patch preserves unedited author fields");
        require(service.Execute("undo",{{"sceneId",service.GetSceneId()},{"baseRevision",service.GetRevision()},{"requestId",UUID().str()}}).at("ok") && service.GetDocument()==unchanged,
            "SpriteRenderer patch undo restores exact prefab document");
        const auto badSprite=service.Execute("apply",request(Json::array({{{"op","patch"},{"entity",spriteId},
            {"component","SpriteRendererComponent"},{"patch",{{"Color",{1.0,0.0}}}}}})));
        require(!badSprite.at("ok").get<bool>() && badSprite.at("error").at("code")=="field_type" && service.GetDocument()==unchanged,
            "SpriteRenderer rejects malformed reflected vector atomically");
        const auto deletedSprite=service.Execute("apply",request(Json::array({{{"op","delete"},{"entity",spriteId},{"children","reject"}}})));
        require(deletedSprite.at("ok") && !service.GetDocument().contains(spriteId),"SpriteRenderer permits safe structural deletion");
        require(service.Execute("undo",{{"sceneId",service.GetSceneId()},{"baseRevision",service.GetRevision()},{"requestId",UUID().str()}}).at("ok") && service.GetDocument()==unchanged,
            "SpriteRenderer structural delete undo restores exact prefab document");
        SceneEditingService failure;
        int writes=0;
        failure.Bind(project,"Assets/Scenes/Test.he",Json::object(),[&](const Json&){if(++writes==1) throw std::runtime_error("injected writer failure");});
        const auto failed=failure.Execute("apply",{{"sceneId",failure.GetSceneId()},{"baseRevision",0},{"requestId",UUID().str()},
            {"operations",Json::array({{{"op","prefab"},{"path","Assets/Prefabs/Test.hprefab"}}})}});
        require(!failed.at("ok").get<bool>() && failure.GetDocument().empty() && writes==2,"failed writer rolls back entire prefab");
    }
}
