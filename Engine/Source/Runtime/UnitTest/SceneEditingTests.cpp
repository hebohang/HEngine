#include "Hepch.h"
#include "Runtime/UnitTest/SceneEditingTests.h"
#include "Runtime/EcsFramework/Serializer/SceneEditingService.h"
#include "Runtime/EcsFramework/Serializer/PrefabDocument.h"
#include "Runtime/EcsFramework/Serializer/SceneEditingLevel.h"
#include "Runtime/EcsFramework/Serializer/SceneSerializer.h"
#include "Runtime/Core/UUID.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include <stdexcept>

namespace HEngine::Tests
{
    void SceneEditingContainers()
    {
        auto require = [](bool test, const char* message) { if (!test) throw std::runtime_error(message); };
        const auto root = std::filesystem::temp_directory_path() / ("HEngine_SceneEditing_" + UUID().str());
        std::filesystem::create_directories(root);
        struct Cleanup { std::filesystem::path Path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(Path, ec); } } cleanup{root};
        {
            SceneEditingService containers; containers.Bind(root, "containers.he", Json::object());
            auto edit = [&](const Json& operations) { return containers.Execute("apply", {
                {"sceneId",containers.GetSceneId()},{"baseRevision",containers.GetRevision()},
                {"requestId",UUID().str()},{"operations",operations}}); };
            const Json animator = {{"FacialWeights",{{"Smile",0.375}}},
                {"ClothPanels",Json::array({{{"MaterialIndex",2},{"Damping",3.125}}})}};
            const Json sequence = {{"Bindings",{{"Hero","actor"}}}, {"Tracks",Json::array({
                {{"Kind","expression"},{"Target","Hero"},{"Name","Smile"},
                    {"Keys",Json::array({{{"Time",0.0},{"Value",0.25},{"Position",{0.0,0.0,0.0}}}})}}})}};
            const auto created = edit(Json::array({{{"op","create"},{"temporaryId","$actor"},
                {"components",{{"AnimatorComponent",animator},{"SequenceComponent",sequence}}}}}));
            require(created.at("ok"), "reflected vector, map and nested sequence authoring");
            const std::string actor = created.at("temporaryIds").at("$actor");
            const auto valid = containers.GetDocument();
            for (const Json& bad : {Json{{"ClothPanels",Json::array({{{"Damping","wrong"}}})}},
                                   Json{{"ClothPanels",Json::array({{{"Typo",1}}})}},
                                   Json{{"ClothPanels",Json::array({{{"Iterations",1.5}}})}},
                                   Json{{"FacialWeights",{{"Smile","wrong"}}}}})
            {
                const auto rejected = edit(Json::array({{{"op","patch"},{"entity",actor},
                    {"component","AnimatorComponent"},{"patch",bad}}}));
                require(!rejected.at("ok").get<bool>() && containers.GetDocument() == valid,
                    "invalid reflected container element rejected atomically");
            }
            require(!edit(Json::array({{{"op","patch"},{"entity",actor},{"component","TransformComponent"},
                {"patch",{{"Translation",{1,2}}}}}})).at("ok").get<bool>(), "fixed glm vector length remains strict");
            require(containers.Execute("save",{{"sceneId",containers.GetSceneId()},
                {"baseRevision",containers.GetRevision()},{"requestId",UUID().str()}}).at("ok"), "container save");
            SceneEditingService reopenedContainers; reopenedContainers.Open(root,"containers.he");
            require(reopenedContainers.GetDocument() == valid, "reflected containers save reopen");
        }
        {
            const auto meshFixture = std::filesystem::current_path() /
                "Project/Assets/Models/StarbloomMenu";
            const auto meshDestination = root / "Assets/Models/StarbloomMenu";
            std::filesystem::create_directories(meshDestination);
            for (const char* name : {"StarbloomMenuBackdrop.hmesh",
                    "StarbloomMenuBackdrop.bin", "StarbloomMenuBackdrop.hmat"})
            {
                std::filesystem::copy_file(meshFixture / name,
                    meshDestination / name);
            }
            SceneEditingService lightmaps;
            lightmaps.Bind(root, "lightmaps.he", Json::object());
            const Json lightmap = {
                {"Path", "Assets/Models/StarbloomMenu/StarbloomMenuBackdrop.hmesh"},
                {"StaticLightmapPath", "Assets/Lighting/TestAtlas.dds"},
                {"StaticLightmapUvScale", {0.25, 0.5}},
                {"StaticLightmapUvBias", {0.125, 0.25}},
                {"StaticLightmapScale0", {1.0, 2.0, 3.0, 4.0}},
                {"StaticLightmapAdd0", {0.1, 0.2, 0.3, 0.4}},
                {"StaticLightmapScale1", {5.0, 6.0, 7.0, 8.0}},
                {"StaticLightmapAdd1", {0.5, 0.6, 0.7, 0.8}},
                {"StaticLightmapBlend", 0.2},
            };
            const Json created = lightmaps.Execute("apply", {
                {"sceneId", lightmaps.GetSceneId()},
                {"baseRevision", lightmaps.GetRevision()},
                {"requestId", UUID().str()},
                {"operations", Json::array({{{"op", "create"},
                    {"temporaryId", "$lightmapped"},
                    {"components", {{"MeshComponent", lightmap}}}}})},
            });
            if (!created.at("ok"))
                throw std::runtime_error("reflected static-lightmap authoring: " + created.dump());
            const Json expected = lightmaps.GetDocument();
            require(lightmaps.Execute("save", {
                {"sceneId", lightmaps.GetSceneId()},
                {"baseRevision", lightmaps.GetRevision()},
                {"requestId", UUID().str()},
            }).at("ok"), "static-lightmap save");
            SceneEditingService reopened;
            reopened.Open(root, "lightmaps.he");
            require(reopened.GetDocument() == expected,
                "static-lightmap fields changed after save and reopen");
        }
        {
            SceneEditingService fog; fog.Bind(root, "fog.he", Json::object());
            const auto apply = [&](const Json& operations) { return fog.Execute("apply", {
                {"sceneId",fog.GetSceneId()},{"baseRevision",fog.GetRevision()},
                {"requestId",UUID().str()},{"operations",operations}}); };
            const auto created = apply(Json::array({{{"op","create"},{"temporaryId","$environment"},
                {"name","Environment"},{"components",{{"EnvironmentLightComponent",{
                    {"bFogEnabled",true},{"FogColor",{0.24,0.31,0.42}},
                    {"FogDensity",0.009},{"FogStartDistance",22.0},
                    {"FogBaseHeight",-3.0},{"FogHeightFalloff",0.003},
                    {"SecondaryFogColor",{0.06,0.13,0.34}},
                    {"SecondaryFogDensity",0.008},
                    {"SecondaryFogStartDistance",0.0},
                    {"SecondaryFogBaseHeight",-2.0},
                    {"SecondaryFogHeightFalloff",0.003},
                    {"FogDirectionalColor",{0.25,0.25,0.125}},
                    {"FogDirectionalIntensity",1.5},
                    {"FogDirectionalExponent",4.0},
                    {"SkyColorTint",{1.14,1.02,0.99}}}},
                    {"PostProcessingComponent",{{"bOverrideTemperature",true},
                        {"Temperature",-0.35},{"bOverrideContrast",true},
                        {"Contrast",1.15},{"bOverrideVignetteIntensity",true},
                        {"VignetteIntensity",0.6},{"bOverrideColorGain",true},
                        {"ColorGain",{1.05,1.0,1.0}},{"bOverrideColorOffset",true},
                        {"ColorOffset",{0.0,0.002,0.005}}}}}}}}));
            if (!created.at("ok").get<bool>())
                throw std::runtime_error("environment fog and color temperature authoring: " + created.dump());
            const std::string id = created.at("temporaryIds").at("$environment");
            const Json authored = fog.GetDocument();
            const auto invalid = apply(Json::array({{{"op","patch"},{"entity",id},
                {"component","EnvironmentLightComponent"},{"patch",{{"FogDensity",-1.0}}}}}));
            require(!invalid.at("ok").get<bool>() && fog.GetDocument() == authored,
                "negative fog density rejected atomically");
            const auto invalidSecondary = apply(Json::array({{{"op","patch"},{"entity",id},
                {"component","EnvironmentLightComponent"},{"patch",{{"SecondaryFogDensity",-1.0}}}}}));
            require(!invalidSecondary.at("ok").get<bool>() && fog.GetDocument() == authored,
                "negative secondary fog density rejected atomically");
            const auto invalidTint = apply(Json::array({{{"op","patch"},{"entity",id},
                {"component","EnvironmentLightComponent"},{"patch",{{"SkyColorTint",{4.1,1.0,1.0}}}}}}));
            require(!invalidTint.at("ok").get<bool>() && fog.GetDocument() == authored,
                "out-of-range sky tint rejected atomically");
            const auto invalidVignette = apply(Json::array({{{"op","patch"},{"entity",id},
                {"component","PostProcessingComponent"},{"patch",{{"VignetteIntensity",1.1}}}}}));
            require(!invalidVignette.at("ok").get<bool>() && fog.GetDocument() == authored,
                "out-of-range vignette rejected atomically");
            const auto invalidGain = apply(Json::array({{{"op","patch"},{"entity",id},
                {"component","PostProcessingComponent"},{"patch",{{"ColorGain",{2.1,1.0,1.0}}}}}}));
            require(!invalidGain.at("ok").get<bool>() && fog.GetDocument() == authored,
                "out-of-range color gain rejected atomically");
            const auto invalidOffset = apply(Json::array({{{"op","patch"},{"entity",id},
                {"component","PostProcessingComponent"},{"patch",{{"ColorOffset",{0.0,0.0,-0.3}}}}}}));
            require(!invalidOffset.at("ok").get<bool>() && fog.GetDocument() == authored,
                "out-of-range color offset rejected atomically");
            require(fog.Execute("save",{{"sceneId",fog.GetSceneId()},
                {"baseRevision",fog.GetRevision()},{"requestId",UUID().str()}}).at("ok"),
                "fog save");
            SceneEditingService reopenedFog; reopenedFog.Open(root,"fog.he");
            require(reopenedFog.GetDocument() == authored, "fog and color temperature save reopen");
        }
    }

    void SceneEditingContract()
    {
        SceneEditingContainers();
        auto require = [](bool test, const char* message) { if (!test) throw std::runtime_error(message); };
        const auto root = std::filesystem::temp_directory_path() / ("HEngine_SceneEditing_" + UUID().str());
        std::filesystem::create_directories(root);
        struct Cleanup { std::filesystem::path Path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(Path, ec); } } cleanup{root};
        SceneEditingService service; service.Bind(root, "test.he", Json::object());
        auto request = [&](Json operations) { return Json{{"sceneId", service.GetSceneId()}, {"baseRevision", service.GetRevision()}, {"requestId", UUID().str()}, {"operations", operations}}; };
        auto control = [&](const char* method) { return service.Execute(method, {{"sceneId",service.GetSceneId()},{"baseRevision",service.GetRevision()},{"requestId",UUID().str()}}); };
        Json create = request(Json::array({
            {{"op","create"},{"temporaryId","$parent"},{"name","Parent"}},
            {{"op","create"},{"temporaryId","$child"},{"name","Child"},{"parent","$parent"}},
            {{"op","transform"},{"entities",{"$parent","$child"}},{"patch",{{"Translation",{1,2,3}}}}}
        }));
        const auto plan = service.Execute("plan", create); require(plan.at("ok"), "create plan");
        require(service.GetRevision() == 0 && service.GetDocument().empty(), "plan must be pure");
        auto applied = service.Execute("apply", create); require(applied.at("ok"), "create apply");
        auto diffPage = service.Execute("diff",{{"sceneId",service.GetSceneId()},{"transactionId",applied.at("transactionId")},{"limit",1}});
        require(diffPage.at("changes").size() == 1 && diffPage.at("nextOffset") == 1, "transaction diff pagination");
        auto diffLast = service.Execute("diff",{{"sceneId",service.GetSceneId()},{"transactionId",applied.at("transactionId")},
            {"revision",service.GetRevision()},{"offset",1},{"limit",1}});
        require(diffLast.at("changes").size() == 1 && diffLast.at("nextOffset").is_null(), "transaction diff second page");
        const std::string parent = applied.at("temporaryIds").at("$parent"), child = applied.at("temporaryIds").at("$child");
        require(service.GetDocument().at(child).at("TransformComponent").at("ParentUuid") == parent, "temporary parent");
        require(service.Execute("apply", create) == applied && service.GetDocument().size() == 2, "idempotent retry");
        auto conflict = create; conflict["operations"][0]["name"] = "Different";
        require(service.Execute("apply", conflict).at("error").at("code") == "idempotency_conflict", "id reuse conflict");
        auto stale = create; stale["requestId"] = UUID().str();
        require(service.Execute("apply", stale).at("error").at("code") == "revision_conflict", "stale rejection");
        auto before = service.GetDocument();
        auto invalid = request(Json::array({{{"op","rename"},{"entity",parent},{"name","Changed"}},
            {{"op","patch"},{"entity",child},{"component","TransformComponent"},{"patch",{{"Bogus",5}}}}}));
        require(!service.Execute("apply", invalid).at("ok").get<bool>() && before == service.GetDocument(), "atomic invalid batch");
        auto cycle = request(Json::array({{{"op","reparent"},{"entity",parent},{"parent",child},{"mode","keepLocal"}}}));
        require(service.Execute("apply", cycle).at("error").at("code") == "hierarchy_cycle", "cycle rejection");
        auto detach = request(Json::array({{{"op","reparent"},{"entity",child},{"parent",""},{"mode","keepWorld"}}}));
        require(service.Execute("apply", detach).at("ok"), "keepWorld reparent");
        require(service.GetDocument().at(child).at("TransformComponent").at("Translation") == Json::array({2.0,4.0,6.0}), "keepWorld translation");
        require(control("undo").at("ok") && service.GetDocument() == before, "undo reparent");
        auto remove = request(Json::array({{{"op","delete"},{"entity",parent},{"children","subtree"}}}));
        require(service.Execute("apply", remove).at("ok") && service.GetDocument().empty(), "delete subtree");
        require(control("undo").at("ok") && service.GetDocument() == before, "restore UUID hierarchy and components");
        require(control("save").at("ok") && !service.IsDirty(), "save clean");
        require(control("undo").at("ok") && service.IsDirty(), "save undo dirty");
        require(control("redo").at("ok") && !service.IsDirty(), "redo saved clean");
        SceneEditingService reopened; reopened.Open(root,"test.he");
        require(reopened.GetDocument() == service.GetDocument(), "save reload roundtrip");

        // Export uses the same real subtree serializer as the Editor save command,
        // while publishing a new resource leaves the author document/history alone.
        std::filesystem::create_directories(root / "Assets");
        int exportWrites = 0;
        SceneEditingService exporting;
        exporting.Bind(root, "export-author.he", before, [&](const Json&) { ++exportWrites; });
        Json editedExport = before;
        editedExport[parent]["TagComponent"]["Tag"] = "Exported parent";
        exporting.Observe(editedExport, true);
        const auto exportRevision = exporting.GetRevision();
        const bool exportDirty = exporting.IsDirty(), exportUndo = exporting.CanUndo(), exportRedo = exporting.CanRedo();
        const auto exportRequest = [&](const std::string& path) { return Json{
            {"sceneId",exporting.GetSceneId()},{"baseRevision",exporting.GetRevision()},
            {"requestId",UUID().str()},{"entity",parent},{"path",path}}; };
        const Json exportArgs = exportRequest("Assets/actor.hprefab");
        const Json exportReceipt = exporting.Execute("scene.export_prefab", exportArgs);
        require(exportReceipt.at("ok") && exportReceipt.at("completion") == "exported" &&
            exportReceipt.at("entityCount") == 2, "real subtree exported");
        const Json exported = Utils::LoadJsonFile(root / "Assets/actor.hprefab");
        const auto loader = [&](const std::string& path) { return Utils::LoadJsonFile(root / path); };
        const auto expandedExport = ExpandPrefabDocument(exported, loader, "Assets/actor.hprefab");
        require(expandedExport.Entities.size() == 2 &&
            expandedExport.Entities.at(child).at("TransformComponent").at("ParentUuid") == parent &&
            expandedExport.Entities.at(parent).at("TransformComponent").at("ParentUuid") == "", "export preserves subtree references");
        const auto exportTime = std::filesystem::last_write_time(root / "Assets/actor.hprefab");
        require(exporting.Execute("export_prefab", exportArgs) == exportReceipt &&
            std::filesystem::last_write_time(root / "Assets/actor.hprefab") == exportTime, "export retry does not rewrite");
        auto exportConflict = exportArgs; exportConflict["path"] = "Assets/conflict.hprefab";
        require(exporting.Execute("export_prefab", exportConflict).at("error").at("code") == "idempotency_conflict", "export request reuse conflict");
        require(exporting.Execute("export_prefab", exportRequest("Assets/actor.hprefab")).at("error").at("code") == "export_exists" &&
            Utils::LoadJsonFile(root / "Assets/actor.hprefab") == exported, "export never overwrites");
        for (const std::string path : {"../escape.hprefab", "outside.hprefab", "Assets/wrong.he", "Assets/missing/actor.hprefab"})
            require(!exporting.Execute("export_prefab", exportRequest(path)).at("ok").get<bool>(), "export path rejected");
        auto staleExport = exportRequest("Assets/stale.hprefab"); staleExport["baseRevision"] = exportRevision - 1;
        require(exporting.Execute("export_prefab", staleExport).at("error").at("code") == "revision_conflict", "export stale revision rejected");
        auto missingExport = exportRequest("Assets/missing.hprefab"); missingExport["entity"] = UUID().str();
        require(exporting.Execute("export_prefab", missingExport).at("error").at("code") == "entity_missing", "export missing root rejected");
        exporting.SetPlaying(true);
        require(exporting.Execute("export_prefab", exportRequest("Assets/playing.hprefab")).at("error").at("code") == "authoring_during_play", "export cannot bypass play guard");
        exporting.SetPlaying(false);
        require(exporting.GetDocument() == editedExport && exporting.GetRevision() == exportRevision && exportWrites == 0 &&
            exporting.IsDirty() == exportDirty && exporting.CanUndo() == exportUndo && exporting.CanRedo() == exportRedo &&
            !std::filesystem::exists(root / "export-author.he"), "export leaves author state and scene file untouched");
        const auto instance = InstantiatePrefabDocument(expandedExport, "Assets/actor.hprefab", [] { return UUID().str(); });
        const std::string wrapper = UUID().str();
        Json nested = instance.Values; nested[wrapper] = before.at(parent);
        nested[wrapper]["TransformComponent"]["ParentUuid"] = "";
        nested[instance.Root]["TransformComponent"]["ParentUuid"] = wrapper;
        SceneEditingService nestedExport; nestedExport.Bind(root, "nested-export.he", nested);
        const auto nestedRequest = [&](const std::string& path) { return Json{
            {"sceneId",nestedExport.GetSceneId()},{"baseRevision",nestedExport.GetRevision()},
            {"requestId",UUID().str()},{"entity",wrapper},{"path",path}}; };
        require(nestedExport.Execute("export_prefab", nestedRequest("Assets/nested.hprefab")).at("ok"), "matching nested dependency exports");
        Json changedTemplate = exported;
        changedTemplate["entities"][parent]["TagComponent"]["Tag"] = "External template edit";
        Utils::SaveJsonToFile(changedTemplate, root / "Assets/actor.hprefab");
        require(nestedExport.Execute("export_prefab", nestedRequest("Assets/rejected-nested.hprefab")).at("error").at("code") == "resource_conflict" &&
            !std::filesystem::exists(root / "Assets/rejected-nested.hprefab") && nestedExport.GetDocument() == nested,
            "stale nested dependency fails before publication");

        Json externalReference = before;
        const std::string outsideId = UUID().str();
        externalReference[outsideId] = before.at(parent);
        externalReference[child]["FollowCameraComponent"] = SceneEditingService::ComponentDefaults("FollowCameraComponent");
        externalReference[child]["FollowCameraComponent"]["bHasTarget"] = true;
        externalReference[child]["FollowCameraComponent"]["TargetUUID"] = outsideId;
        require(externalReference[outsideId]["TransformComponent"]["ParentUuid"] == "",
            "external reference fixture target is outside the exported subtree");
        SceneEditingService invalidExport; invalidExport.Bind(root, "external-ref.he", externalReference);
        const Json invalidExportArgs = {{"sceneId",invalidExport.GetSceneId()},{"baseRevision",invalidExport.GetRevision()},
            {"requestId",UUID().str()},{"entity",parent},{"path","Assets/external-ref.hprefab"}};
        const Json invalidExportReceipt = invalidExport.Execute("export_prefab", invalidExportArgs);
        require(!invalidExportReceipt.at("ok").get<bool>() &&
            invalidExportReceipt.at("error").at("message").get<std::string>().find("external entity reference: FollowCameraComponent.TargetUUID") != std::string::npos,
            "active external subtree reference is rejected by capture");
        require(!std::filesystem::exists(root / "Assets/external-ref.hprefab") &&
            invalidExport.GetDocument() == externalReference && invalidExport.GetRevision() == 0,
            "external subtree reference failure leaves file and author state untouched");
        require(exporting.Execute("describe", Json::object()).at("exportPrefab").at("requestSchema").at("required").size() == 5,
            "export machine contract is discoverable");

        // A legacy file may omit reflected defaults. Loading and resizing it
        // establishes a clean authoring baseline without rewriting that file.
        Json legacy = before;
        legacy[parent]["CameraComponent"] = SceneEditingService::ComponentDefaults("CameraComponent");
        legacy[parent]["CameraComponent"]["Camera"].erase("mAspectRatio");
        legacy[parent]["TransformComponent"].erase("ParentUuid");
        const auto legacyPath = root / "legacy.he";
        Utils::SaveJsonToFile(legacy, legacyPath);
        auto loadedLevel = CreateRef<Level>();
        ApplySceneEditingValues(loadedLevel, legacy);
        loadedLevel->OnViewportResize(1600, 900);
        const Json loadedValues = SceneSerializer(loadedLevel).SerializeToJson();
        require(loadedValues != legacy, "fixture exercises reflected defaults and viewport normalization");
        SceneEditingService loaded;
        loaded.Bind(root, "legacy.he", loadedValues, {}, true);
        require(!loaded.IsDirty() && loaded.GetRevision() == 0 && !loaded.CanUndo(), "freshly loaded scene is clean");
        require(!loaded.GetDocument().at(parent).at("CameraComponent").at("Camera").contains("mAspectRatio"),
            "viewport aspect absent on disk must remain absent in the author document");
        loadedLevel->OnViewportResize(1200, 900);
        loaded.Observe(SceneSerializer(loadedLevel).SerializeToJson(), false);
        require(!loaded.IsDirty() && loaded.GetRevision() == 0, "resize is not an author edit");
        require(!loaded.GetDocument().at(parent).at("CameraComponent").at("Camera").contains("mAspectRatio"),
            "viewport resize must not materialize camera aspect in the author document");
        require(Utils::LoadJsonFile(legacyPath) == legacy, "opening does not rewrite the source scene");
        Json loadedEdit = loaded.GetDocument();
        loadedEdit[parent]["TagComponent"]["Tag"] = "Edited after loading";
        loaded.Observe(loadedEdit, true);
        require(loaded.IsDirty() && loaded.CanUndo(), "real edits still mark the loaded scene dirty");
        auto loadedControl = [&](const char* method) { return loaded.Execute(method,
            {{"sceneId",loaded.GetSceneId()},{"baseRevision",loaded.GetRevision()},{"requestId",UUID().str()}}); };
        require(loadedControl("undo").at("ok") && !loaded.IsDirty(), "undo restores the loaded clean baseline");
        require(loadedControl("redo").at("ok") && loaded.IsDirty(), "redo restores a real edit");
        require(loadedControl("save").at("ok") && !loaded.IsDirty(), "normalized baseline retains the original disk conflict token");
        require(!Utils::LoadJsonFile(legacyPath).at(parent).at("CameraComponent").at("Camera").contains("mAspectRatio"),
            "saving another edit must not persist viewport aspect");
        Json externalEdit = Utils::LoadJsonFile(legacyPath);
        externalEdit[parent]["TagComponent"]["Tag"] = "External edit";
        Utils::SaveJsonToFile(externalEdit, legacyPath);
        require(loadedControl("save").at("error").at("code") == "external_file_changed", "loaded baseline still rejects external overwrite");
        auto page = service.Execute("query", {{"sceneId",service.GetSceneId()},{"limit",1},{"fields",{"uuid"}}});
        require(page.at("entities").size() == 1 && page.at("entities")[0].size() == 1 && page.at("nextOffset") == 1, "projection pagination");
        Json manual = service.GetDocument(); manual[parent]["TagComponent"]["Tag"] = "Manual";
        const auto rev = service.GetRevision(); service.Observe(manual, true);
        require(service.GetRevision() == rev + 1 && control("undo").at("ok"), "manual shared history");
        page = service.Execute("query", {{"sceneId",service.GetSceneId()},{"revision",rev},{"offset",1}});
        require(page.at("error").at("code") == "revision_conflict", "paged revision check");
        service.SetPlaying(true); service.Observe(manual, true);
        require(service.GetDocument() == before && control("undo").at("error").at("code") == "authoring_during_play", "simulation isolation");
        service.SetPlaying(false);
        service.Observe(manual, false);
        require(!service.CanUndo() && service.Execute("diff", {{"sceneId",service.GetSceneId()},{"sinceRevision",0}}).at("error").at("code") == "delta_expired", "external edit invalidates history");
        Utils::SaveJsonToFile(Json::object(),root / "test.he");
        require(control("save").at("error").at("code") == "external_file_changed", "external disk protection");

        SceneEditingService host;
        Json hostValues = before;
        int writes = 0;
        host.Bind(root, "host.he", before, [&](const Json& values) {
            for (auto& [id, value] : values.items()) if (value.is_null()) hostValues.erase(id); else hostValues[id] = value;
            if (++writes == 1) throw std::runtime_error("injected commit failure");
        });
        Json failing = {{"sceneId",host.GetSceneId()},{"baseRevision",0},{"requestId","fail-once"},
            {"operations",Json::array({{{"op","rename"},{"entity",parent},{"name","Failed"}}})}};
        require(host.Execute("apply", failing).at("error").at("code") == "commit_failed" &&
            hostValues == before && host.GetDocument() == before && host.GetRevision() == 0 && !host.CanUndo(), "commit failure must restore host and history");
        require(host.Execute("apply", failing).at("error").at("code") == "commit_failed" && writes == 2, "failed request retry is not executed twice");
        host.Bind(root,"blocked.he",before,[](const Json&) { throw std::runtime_error("injected recovery failure"); });
        failing["sceneId"] = host.GetSceneId();
        require(host.Execute("apply",failing).at("error").at("code") == "recovery_failed", "recovery failure explicit");
        failing["requestId"] = "blocked-write";
        require(host.Execute("apply",failing).at("error").at("code") == "recovery_required", "failed recovery blocks writes");

        SceneEditingService retained; retained.Bind(root,"retained.he",before);
        for (int index = 0; index < 70; ++index)
        {
            auto result = retained.Execute("apply",{{"sceneId",retained.GetSceneId()},{"baseRevision",retained.GetRevision()},
                {"requestId",std::to_string(index)},{"operations",Json::array({{{"op","rename"},{"entity",parent},{"name",std::to_string(index)}}})}});
            require(result.at("ok"), "history capacity application");
        }
        require(retained.Execute("diff",{{"sceneId",retained.GetSceneId()},{"sinceRevision",0}}).at("error").at("code") == "delta_expired", "bounded incremental retention");
        auto branch = retained.Execute("undo",{{"sceneId",retained.GetSceneId()},{"baseRevision",retained.GetRevision()},{"requestId","branch-undo"}});
        require(branch.at("ok") && retained.CanRedo(), "redo branch exists");
        branch = retained.Execute("apply",{{"sceneId",retained.GetSceneId()},{"baseRevision",retained.GetRevision()},
            {"requestId","new-branch"},{"operations",Json::array({{{"op","rename"},{"entity",parent},{"name","New branch"}}})}});
        require(branch.at("ok") && !retained.CanRedo(), "new edits discard redo branch");

        auto level = CreateRef<Level>(); ApplySceneEditingValues(level, before);
        SceneEditingService ecs; ecs.Bind(root,"ecs.he",SceneSerializer(level).SerializeToJson(),
            [&](const Json& values) { ApplySceneEditingValues(level, values); });
        std::vector<uint64_t> notified;
        ecs.SetChangeObserver([&](uint64_t revision) { notified.push_back(revision); });
        auto removed = ecs.Execute("apply",{{"sceneId",ecs.GetSceneId()},{"baseRevision",ecs.GetRevision()},{"requestId","ecs-delete"},
            {"operations",Json::array({{{"op","delete"},{"entity",parent},{"children","subtree"}}})}});
        require(removed.at("ok") && SceneSerializer(level).SerializeToJson().empty(), "ECS lifecycle delete");
        require(ecs.Execute("undo",{{"sceneId",ecs.GetSceneId()},{"baseRevision",ecs.GetRevision()},{"requestId","ecs-undo"}}).at("ok"), "ECS lifecycle undo");
        require(SceneSerializer(level).SerializeToJson() == before, "ECS serialized roundtrip");
        Json manualEcs = before; manualEcs[parent]["TagComponent"]["Tag"] = "Manual notification";
        ApplySceneEditingValues(level,manualEcs); ecs.Observe(SceneSerializer(level).SerializeToJson(),true);
        require(notified == std::vector<uint64_t>({1,2,3}), "manual and command edits publish one monotonic notification each");
        require(ecs.Execute("save",{{"sceneId",ecs.GetSceneId()},{"baseRevision",ecs.GetRevision()},
            {"requestId","save-as"},{"path","moved.he"}}).at("ok") && ecs.CanUndo() && !ecs.IsDirty(), "Save As retains history and marks saved state");
        require(ecs.Execute("save",{{"sceneId",ecs.GetSceneId()},{"baseRevision",ecs.GetRevision()},
            {"requestId","save-as-existing"},{"path","test.he"}}).at("error").at("code") == "external_file_changed", "Save As refuses unknown existing content");
        require(ecs.Execute("describe",Json::object()).at("path") == "moved.he", "failed Save As keeps document binding");

        SceneEditingService copies; copies.Bind(root,"copies.he",before);
        auto duplicate = copies.Execute("apply",{{"sceneId",copies.GetSceneId()},{"baseRevision",0},{"requestId","duplicate-tree"},
            {"operations",Json::array({{{"op","duplicate"},{"entity",parent},{"children","subtree"},{"temporaryId","$copy"}}})}});
        require(duplicate.at("ok") && copies.GetDocument().size() == 4, "duplicate subtree count");
        const std::string copyParent = duplicate.at("temporaryIds").at("$copy"); bool remappedChild = false;
        for (auto& [id,entity] : copies.GetDocument().items())
            if (id != child && entity.at("TransformComponent").at("ParentUuid") == copyParent) remappedChild = true;
        require(remappedChild && copies.GetDocument().at(child).at("TransformComponent").at("ParentUuid") == parent, "only copied internal parent references remap");
        Json prefab = before.at(child); prefab["TransformComponent"]["ParentUuid"] = "";
        Utils::SaveJsonToFile(prefab,root / "sample.hprefab");
        auto instantiated = copies.Execute("apply",{{"sceneId",copies.GetSceneId()},{"baseRevision",copies.GetRevision()},{"requestId","prefab"},
            {"operations",Json::array({{{"op","prefab"},{"path","sample.hprefab"},{"temporaryId","$instance"}}})}});
        require(instantiated.at("ok") && copies.GetDocument().at(instantiated.at("temporaryIds").at("$instance").get<std::string>()) == prefab, "existing single-entity prefab format roundtrip");

        SceneEditingService transforms; transforms.Bind(root,"transforms.he",before);
        auto editTransform = [&](Json operations) { return transforms.Execute("apply",{{"sceneId",transforms.GetSceneId()},
            {"baseRevision",transforms.GetRevision()},{"requestId",UUID().str()},{"operations",operations}}); };
        require(editTransform(Json::array({{{"op","reparent"},{"entity",child},{"parent",""},{"mode","keepLocal"}}})).at("ok"), "keepLocal detach");
        require(transforms.GetDocument().at(child).at("TransformComponent").at("Translation") == before.at(child).at("TransformComponent").at("Translation"), "keepLocal retains local TRS");
        auto singular = transforms.GetDocument(); singular[parent]["TransformComponent"]["Scale"] = {0,1,1}; transforms.Observe(singular,false);
        require(editTransform(Json::array({{{"op","reparent"},{"entity",child},{"parent",parent},{"mode","keepWorld"}}})).at("error").at("code") == "singular_parent", "singular parent rejection");
        auto copied = editTransform(Json::array({{{"op","duplicate"},{"entity",child},{"children","subtree"},{"temporaryId","$copy"}}}));
        require(copied.at("ok") && copied.at("temporaryIds").at("$copy") != child, "duplicate has new identity");
        const auto describe = transforms.Execute("describe",{{"sceneId",transforms.GetSceneId()},{"component","CameraComponent"},{"operation","transform"}});
        require(describe.contains("schemaFingerprint") && describe.at("operationSchema").contains("properties") && describe.at("fields").at("Camera").at("properties").at("mAspectRatio").at("readOnly"), "discover cached typed schema");
        auto queryInvalid = transforms.Execute("query",{{"sceneId",transforms.GetSceneId()},{"fields",{"notAField"}}});
        require(queryInvalid.at("error").at("code") == "field_projection", "unknown projection rejected");

        std::filesystem::path repository(__FILE__);
        for (int index = 0; index < 5; ++index) repository = repository.parent_path();
        SceneEditingService resources; resources.Bind(repository / "Examples/RenderingTests", "Saved/SceneEditingResourceTest.he", Json::object());
        auto editResource = [&](Json operations) { return resources.Execute("apply",{{"sceneId",resources.GetSceneId()},
            {"baseRevision",resources.GetRevision()},{"requestId",UUID().str()},{"operations",operations}}); };
        auto resourceError = editResource(Json::array({{{"op","create"},{"components",{{"MeshComponent",{{"Path","../escape.hmesh"}}}}}}}));
        require(resourceError.at("error").at("code") == "resource_path", "escaping resource path");
        resourceError = editResource(Json::array({{{"op","create"},{"components",{{"MeshComponent",{{"Path","Assets/Material/Emissive.hmat"}}}}}}}));
        require(resourceError.at("error").at("code") == "resource_type", "wrong mesh resource type");
        auto mesh = editResource(Json::array({{{"op","create"},{"temporaryId","$mesh"},{"components",{{"MeshComponent",{{"Path","Assets/Models/Cube/Cube.hmesh"}}}}}}}));
        require(mesh.at("ok"), "CPU-only mesh validation"); const std::string meshId = mesh.at("temporaryIds").at("$mesh");
        resourceError = editResource(Json::array({{{"op","material"},{"entity",meshId},{"slot",9999},{"path",""}}}));
        require(resourceError.at("error").at("code") == "material_slot", "material slot range");
        resourceError = editResource(Json::array({{{"op","material"},{"entity",meshId},{"slot",0},{"path","Assets/Models/Cube/Cube.hmesh"}}}));
        require(resourceError.at("error").at("code") == "resource_type", "material resource type");
        require(editResource(Json::array({{{"op","material"},{"entity",meshId},{"slot",0},{"path","Assets/Material/Emissive.hmat"}}})).at("ok"), "material override");
        require(editResource(Json::array({{{"op","material"},{"entity",meshId},{"slot",0},{"path",""}}})).at("ok") &&
            resources.GetDocument().at(meshId).at("MeshComponent").at("MaterialOverrides").empty(), "clear material restores authored slot");

        SceneEditingService quota; quota.Bind(root,"quota.he",before);
        Json noChange = {{"sceneId",quota.GetSceneId()},{"baseRevision",0},{"requestId","0"},
            {"operations",Json::array({{{"op","rename"},{"entity",parent},{"name","Parent"}}})}};
        Json firstReceipt;
        for (int index = 0; index < 1024; ++index)
        {
            noChange["requestId"] = std::to_string(index);
            auto receipt = quota.Execute("apply",noChange); require(receipt.at("ok"), "retained idempotency quota");
            if (index == 0) firstReceipt = receipt;
        }
        noChange["requestId"] = "beyond-capacity";
        require(quota.Execute("apply",noChange).at("error").at("code") == "idempotency_capacity", "no silent request eviction");
        noChange["requestId"] = "0";
        require(quota.Execute("apply",noChange) == firstReceipt, "old request still retries safely at capacity");
    }
}
