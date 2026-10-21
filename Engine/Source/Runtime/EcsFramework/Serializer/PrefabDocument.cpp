#include "Hepch.h"

#include "Runtime/EcsFramework/Serializer/PrefabDocument.h"
#include "Runtime/Scripting/ScriptFieldsJson.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace HEngine
{
    namespace
    {
        constexpr const char* Metadata = "PrefabInstanceComponent";
        constexpr size_t MaxEntities = 4096, MaxDepth = 32;
        const std::vector<std::pair<std::string, std::string>> References = {
            {"TransformComponent", "ParentUuid"}, {"FollowCameraComponent", "TargetUUID"},
            {"EditorEntityStateComponent", "FolderUuid"}};

        void Check(bool condition, const std::string& message)
        {
            if (!condition) throw std::invalid_argument("prefab: " + message);
        }
        std::string Parent(const Json& value)
        {
            return value.contains("TransformComponent") ? value.at("TransformComponent").value("ParentUuid", std::string()) : std::string();
        }
        std::string Escape(std::string key)
        {
            std::string result;
            for (char ch : key) result += ch == '~' ? "~0" : ch == '/' ? "~1" : std::string(1, ch);
            return result;
        }
        std::string Revision(const Json& value)
        {
            // A deterministic content token, not a cryptographic provenance hash.
            uint64_t hash = 14695981039346656037ull;
            for (unsigned char ch : value.dump()) { hash ^= ch; hash *= 1099511628211ull; }
            std::ostringstream result; result << std::hex << std::setw(16) << std::setfill('0') << hash;
            return result.str();
        }
        std::string PathKey(const std::string& path)
        {
            auto key = std::filesystem::path(path).lexically_normal().generic_string();
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return key;
        }
        Json MergePatch(const Json& before, const Json& after)
        {
            if (!before.is_object() || !after.is_object()) return after;
            Json patch = Json::object();
            for (auto& [key, value] : before.items()) if (!after.contains(key)) patch[key] = nullptr;
            for (auto& [key, value] : after.items())
                if (!before.contains(key)) patch[key] = value;
                else if (before.at(key) != value) patch[key] = MergePatch(before.at(key), value);
            return patch;
        }
        void ComposePatch(Json& destination, const Json& patch)
        {
            if (!destination.is_object()) destination = Json::object();
            for (auto& [key, value] : patch.items())
                if (value.is_object()) ComposePatch(destination[key], value);
                else destination[key] = value; // Preserve null as a deletion instruction.
        }
        void ValidateEntities(const Json& entities, const std::string& root)
        {
            Check(entities.is_object() && !entities.empty() && entities.size() <= MaxEntities, "expected 1..4096 entities");
            Check(entities.contains(root), "root local ID is absent");
            for (auto& [id, value] : entities.items())
            {
                Check(!id.empty() && id.size() <= 512 && value.is_object(), "invalid local ID or component object");
                Check(!value.contains(Metadata) && !value.contains("IDComponent"), "template must not contain instance metadata or scene ID component");
                std::set<std::string> visited;
                std::string current = id;
                while (!current.empty())
                {
                    Check(entities.contains(current), "external/missing parent: " + current);
                    Check(visited.insert(current).second, "hierarchy cycle at " + current);
                    Check(visited.size() <= 256, "hierarchy depth exceeds 256");
                    current = Parent(entities.at(current));
                }
                Check(visited.contains(root), "entity is outside the root subtree: " + id);
                for (const auto& [component, field] : References)
                    if (value.contains(component) && value.at(component).contains(field))
                    {
                        if (component == "FollowCameraComponent" && !value.at(component).value("bHasTarget", false)) continue;
                        const Json& reference = value.at(component).at(field);
                        Check(reference.is_string(), "reference must be a string: " + component + "." + field);
                        const auto target = reference.get<std::string>();
                        Check(target.empty() || target == "00000000-0000-0000-0000-000000000000" || entities.contains(target),
                            "unresolved entity reference: " + component + "." + field + " -> " + target);
                    }
            }
        }
        Json Expand(const Json& source, const PrefabLoader& loader, std::set<std::string>& stack, size_t depth)
        {
            Check(depth <= MaxDepth, "nested prefab depth exceeds 32");
            Json doc = NormalizePrefabDocument(source);
            Json entities = doc.at("entities");
            for (Json& nested : doc["nested"])
            {
                Check(nested.is_object(), "nested entry must be an object");
                const std::string anchor = nested.at("id"), path = nested.at("path");
                Check(!anchor.empty() && !path.empty() && static_cast<bool>(loader), "nested entry needs id, path and loader");
                Check(stack.insert(PathKey(path)).second, "nested dependency cycle: " + path);
                const Json child = Expand(loader(path), loader, stack, depth + 1);
                stack.erase(PathKey(path));
                const std::string childRoot = child.at("root");
                nested["root"] = childRoot;
                std::map<std::string, std::string> map;
                for (auto& [id, unused] : child.at("entities").items()) map[id] = id == childRoot ? anchor : anchor + "/" + id;
                Json values = child.at("entities");
                const Json overrides = nested.value("overrides", Json::object());
                Check(overrides.is_object(), "nested overrides must map local IDs to merge patches");
                for (auto& [id, patch] : overrides.items())
                {
                    Check(values.contains(id) && patch.is_object(), "nested override target missing: " + id);
                    values[id].merge_patch(patch);
                }
                for (auto& [id, value] : values.items())
                {
                    RemapPrefabReferences(value, map, true);
                    if (id == childRoot) value["TransformComponent"]["ParentUuid"] = nested.value("parent", std::string());
                    Check(!entities.contains(map.at(id)), "duplicate nested local ID: " + map.at(id));
                    entities[map.at(id)] = value;
                }
                Check(entities.size() <= MaxEntities, "expanded entity count exceeds 4096");
            }
            ValidateEntities(entities, doc.at("root"));
            return {{"schemaVersion", 2}, {"root", doc.at("root")}, {"entities", entities}, {"nested", Json::array()}, {"source", doc}};
        }
        Json StripMetadata(Json value) { value.erase(Metadata); return value; }
        bool Covered(const Json& paths, const std::string& path)
        {
            for (const Json& entry : paths)
            {
                const auto item = entry.get<std::string>();
                if (path == item || path.starts_with(item + "/")) return true;
            }
            return false;
        }
        Json Merge(const Json& base, const Json& current, const Json& next, const Json& paths, const std::string& path)
        {
            if (Covered(paths, path)) return current;
            if (base.is_object() && current.is_object() && next.is_object())
            {
                Json merged = Json::object();
                std::set<std::string> keys;
                for (auto& [key, unused] : base.items()) keys.insert(key);
                for (auto& [key, unused] : current.items()) keys.insert(key);
                for (auto& [key, unused] : next.items()) keys.insert(key);
                for (const auto& key : keys)
                {
                    const std::string child = path + "/" + Escape(key);
                    const bool b = base.contains(key), c = current.contains(key), n = next.contains(key);
                    if (b && !n)
                    {
                        bool hasOverride = Covered(paths, child);
                        for (const Json& p : paths) hasOverride = hasOverride || p.get<std::string>().starts_with(child + "/");
                        Check(!hasOverride && (!c || current.at(key) == base.at(key)), "deleted overridden field: " + child);
                        continue;
                    }
                    if (!b)
                    {
                        Check(!(c && n && current.at(key) != next.at(key)), "concurrent added field: " + child);
                        if (c || n) merged[key] = c ? current.at(key) : next.at(key);
                    }
                    else if (!c) { /* local component/field deletion is an override */ }
                    else if (n) merged[key] = Merge(base.at(key), current.at(key), next.at(key), paths, child);
                }
                return merged;
            }
            return current == base ? next : current;
        }
        void ChangedPaths(const Json& before, const Json& after, const std::string& path, std::set<std::string>& paths)
        {
            if (before == after) return;
            if (before.is_object() && after.is_object())
            {
                std::set<std::string> keys;
                for (auto& [key, unused] : before.items()) keys.insert(key);
                for (auto& [key, unused] : after.items()) keys.insert(key);
                for (const auto& key : keys)
                {
                    const auto child = path + "/" + Escape(key);
                    if (!before.contains(key) || !after.contains(key)) paths.insert(child);
                    else ChangedPaths(before.at(key), after.at(key), child, paths);
                }
            }
            else paths.insert(path);
        }
    }

    Json NormalizePrefabDocument(const Json& source)
    {
        Check(source.is_object(), "document must be an object");
        if (!source.contains("schemaVersion"))
        {
            Json value = source;
            value.erase("IDComponent"); value.erase(Metadata);
            if (value.contains("TransformComponent")) value["TransformComponent"]["ParentUuid"] = "";
            return {{"schemaVersion", 2}, {"root", "root"}, {"entities", {{"root", value}}}, {"nested", Json::array()}};
        }
        Check(source.at("schemaVersion") == 2, "unsupported schemaVersion");
        Check(source.contains("root") && source.at("root").is_string() && source.contains("entities") && source.at("entities").is_object(), "v2 needs root and entities");
        Json doc = source;
        if (!doc.contains("nested")) doc["nested"] = Json::array();
        Check(doc.at("nested").is_array(), "nested must be an array");
        return doc;
    }

    PrefabDocument ExpandPrefabDocument(const Json& source, const PrefabLoader& loader, const std::string& path)
    {
        std::set<std::string> stack;
        if (!path.empty()) stack.insert(PathKey(path));
        const Json expanded = Expand(source, loader, stack, 0);
        Json definition=expanded.at("source"); definition["expandedLocalIds"]=Json::array();
        for(auto& [id,unused]:expanded.at("entities").items()) definition["expandedLocalIds"].push_back(id);
        return {expanded.at("root"), expanded.at("entities"), Revision(expanded), definition,
            !source.contains("schemaVersion")};
    }

    void RemapPrefabReferences(Json& value, const std::map<std::string, std::string>& ids, bool rejectExternal)
    {
        if (value.contains("ScriptComponent"))
        {
            auto& component = value["ScriptComponent"];
            component["Fields"] = RemapScriptEntityReferences(component.value("Fields", std::string("{\"schema\":1,\"fields\":{}}")), ids, rejectExternal);
        }
        for (const auto& [component, field] : References)
        {
            if (!value.contains(component) || !value.at(component).contains(field)) continue;
            if (component == "FollowCameraComponent" && !value.at(component).value("bHasTarget", false)) continue;
            Json& reference = value[component][field];
            Check(reference.is_string(), "entity reference must be a string");
            const std::string key = reference;
            if (const auto it = ids.find(key); it != ids.end()) reference = it->second;
            else Check(!rejectExternal || key.empty() || key == "00000000-0000-0000-0000-000000000000", "external entity reference: " + component + "." + field + "=" + key);
        }
    }

    PrefabInstantiation InstantiatePrefabDocument(const PrefabDocument& prefab, const std::string& path, const PrefabIdFactory& ids)
    {
        ValidateEntities(prefab.Entities, prefab.Root);
        std::map<std::string, std::string> map;
        std::set<std::string> unique;
        for (auto& [id, unused] : prefab.Entities.items())
        {
            const auto next = ids();
            Check(!next.empty() && unique.insert(next).second, "duplicate generated instance ID"); map[id] = next;
        }
        PrefabInstantiation result; result.Root = map.at(prefab.Root);
        for (auto& [id, value] : prefab.Entities.items())
        {
            Json instance = value;
            RemapPrefabReferences(instance, map, true);
            if (!instance.contains("TagComponent")) instance["TagComponent"] = {{"Tag", "Entity"}};
            if (!prefab.bLegacySingleEntity)
                instance[Metadata] = {{"TemplatePath", path}, {"LocalId", id}, {"InstanceRootUuid", result.Root},
                    {"BaselineJson", value.dump()}, {"OverridePathsJson", "[]"}, {"TemplateRevision", prefab.Revision},
                    {"TemplateDocumentJson", id == prefab.Root ? prefab.Source.dump() : std::string()}};
            result.Values[map.at(id)] = std::move(instance);
        }
        return result;
    }

    Json SynchronizePrefabDocument(const Json& scene, const std::string& root, const PrefabDocument& prefab, const PrefabIdFactory& ids)
    {
        Check(scene.contains(root) && scene.at(root).contains(Metadata), "target is not a prefab instance");
        const Json rootMetadata = scene.at(root).at(Metadata);
        Check(rootMetadata.at("InstanceRootUuid") == root, "sync target must be instance root");
        const std::string path = rootMetadata.at("TemplatePath");
        std::map<std::string, std::string> localToScene, sceneToLocal;
        for (auto& [id, value] : scene.items())
            if (value.contains(Metadata) && value.at(Metadata).at("InstanceRootUuid") == root)
            {
                const std::string local = value.at(Metadata).at("LocalId");
                Check(!localToScene.contains(local), "duplicate instance local ID: " + local);
                localToScene[local] = id; sceneToLocal[id] = local;
            }
        Check(localToScene.contains(prefab.Root) && localToScene.at(prefab.Root) == root, "template root identity changed");
        for (auto& [local, unused] : prefab.Entities.items())
            if (!localToScene.contains(local))
            {
                const std::string id = ids(); Check(!scene.contains(id) && !sceneToLocal.contains(id), "generated ID collision");
                localToScene[local] = id; sceneToLocal[id] = local;
            }
        Json changes = Json::object(); std::set<std::string> deleted;
        for (const auto& [local, id] : localToScene)
        {
            if (!prefab.Entities.contains(local))
            {
                const Json& old = scene.at(id);
                Json current = StripMetadata(old); RemapPrefabReferences(current, sceneToLocal);
                const Json baseline = Json::parse(old.at(Metadata).at("BaselineJson").get<std::string>());
                const Json overrides = Json::parse(old.at(Metadata).at("OverridePathsJson").get<std::string>());
                Check(current == baseline && overrides.empty(), "deleted overridden entity: " + local);
                changes[id] = nullptr; deleted.insert(id); continue;
            }
            const Json& next = prefab.Entities.at(local);
            Json current = next, overrides = Json::array();
            if (scene.contains(id))
            {
                const Json& old = scene.at(id);
                current = StripMetadata(old); RemapPrefabReferences(current, sceneToLocal);
                const Json baseline = Json::parse(old.at(Metadata).at("BaselineJson").get<std::string>());
                overrides = Json::parse(old.at(Metadata).at("OverridePathsJson").get<std::string>());
                current = Merge(baseline, current, next, overrides, "");
            }
            RemapPrefabReferences(current, localToScene);
            if (!current.contains("TagComponent")) current["TagComponent"] = {{"Tag", "Entity"}};
            current[Metadata] = {{"TemplatePath", path}, {"LocalId", local}, {"InstanceRootUuid", root},
                {"BaselineJson", next.dump()}, {"OverridePathsJson", overrides.dump()}, {"TemplateRevision", prefab.Revision},
                {"TemplateDocumentJson", local == prefab.Root ? prefab.Source.dump() : std::string()}};
            changes[id] = std::move(current);
        }
        for (auto& [id, value] : scene.items())
        {
            if (deleted.contains(id)) continue;
            const Json& resulting = changes.contains(id) ? changes.at(id) : value;
            for (const auto& [component, field] : References)
                if (resulting.contains(component) && resulting.at(component).contains(field))
                    Check(!deleted.contains(resulting.at(component).at(field).get<std::string>()), "deleted entity still referenced by " + id + "." + component + "." + field);
        }
        return changes;
    }

    Json CapturePrefabDocument(const Json& scene, const std::string& root, const std::string& path)
    {
        Check(scene.contains(root), "capture root missing");
        std::set<std::string> included{root};
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (auto& [id, value] : scene.items()) if (!included.contains(id) && included.contains(Parent(value)))
            { included.insert(id); changed = true; }
            Check(included.size() <= MaxEntities, "capture exceeds 4096 entities");
        }
        std::map<std::string, std::string> map;
        std::set<std::string> locals;
        for (const auto& id : included)
        {
            const Json& value = scene.at(id);
            const std::string local = value.contains(Metadata) && value.at(Metadata).at("TemplatePath") == path
                ? value.at(Metadata).at("LocalId").get<std::string>() : id;
            Check(locals.insert(local).second, "local identity collision while capturing"); map[id] = local;
        }
        std::map<std::string, std::vector<std::string>> nestedGroups;
        for (const auto& id : included)
        {
            const Json& value = scene.at(id);
            if (!value.contains(Metadata)) continue;
            const Json& metadata = value.at(Metadata);
            const std::string instanceRoot = metadata.at("InstanceRootUuid");
            if (instanceRoot == root || !included.contains(instanceRoot) || metadata.at("TemplatePath") == path) continue;
            nestedGroups[instanceRoot].push_back(id);
        }
        for (const auto& [nestedRoot, members] : nestedGroups)
        {
            const std::string anchor = map.at(nestedRoot);
            for (const auto& id : members)
                map[id] = id == nestedRoot ? anchor : anchor + "/" + scene.at(id).at(Metadata).at("LocalId").get<std::string>();
        }
        Json entities = Json::object();
        for (const auto& id : included)
        {
            Json value = StripMetadata(scene.at(id)); value.erase("IDComponent");
            if (id == root && value.contains("TransformComponent")) value["TransformComponent"]["ParentUuid"] = "";
            RemapPrefabReferences(value, map, true); entities[map.at(id)] = std::move(value);
        }
        ValidateEntities(entities, map.at(root));
        Json nested = Json::array();
        for (const auto& [nestedRoot, members] : nestedGroups)
        {
            const Json& rootMetadata = scene.at(nestedRoot).at(Metadata);
            Json patches = Json::object();
            std::map<std::string, std::string> unqualify;
            for (const auto& id : members) unqualify[map.at(id)] = scene.at(id).at(Metadata).at("LocalId");
            const Json definition=Json::parse(rootMetadata.at("TemplateDocumentJson").get<std::string>());
            for(const Json& expected:definition.value("expandedLocalIds",Json::array()))
                Check(std::any_of(unqualify.begin(),unqualify.end(),[&](const auto& item){return item.second==expected.get<std::string>();}),
                    "nested child deleted; edit its template or unpack before saving");
            for (const auto& id : members)
            {
                const Json& metadata = scene.at(id).at(Metadata);
                Json current = entities.at(map.at(id));
                if (id == nestedRoot) current["TransformComponent"]["ParentUuid"] = "";
                RemapPrefabReferences(current, unqualify, true);
                const Json baseline = Json::parse(metadata.at("BaselineJson").get<std::string>());
                const Json patch = MergePatch(baseline, current);
                if (!patch.empty()) patches[metadata.at("LocalId").get<std::string>()] = patch;
            }
            nested.push_back({{"id",map.at(nestedRoot)}, {"root",rootMetadata.at("LocalId")},
                {"path",rootMetadata.at("TemplatePath")}, {"parent",Parent(entities.at(map.at(nestedRoot)))}, {"overrides",patches}});
            for (const auto& id : members) entities.erase(map.at(id));
        }
        // Keep authored nested dependency edges when resaving an instance of
        // this template. Expanded local IDs use anchor/child namespaces.
        const Json& rootValue = scene.at(root);
        if (rootValue.contains(Metadata) && rootValue.at(Metadata).at("TemplatePath") == path &&
            !rootValue.at(Metadata).value("TemplateDocumentJson", std::string()).empty())
        {
            const Json source = Json::parse(rootValue.at(Metadata).at("TemplateDocumentJson").get<std::string>());
            for (Json entry : source.value("nested", Json::array()))
            {
                const std::string anchor = entry.at("id");
                Check(entities.contains(anchor), "nested root deleted; explicitly unpack before deleting a nested dependency");
                for(const Json& expected:source.value("expandedLocalIds",Json::array()))
                {
                    const std::string local=expected;
                    if(local==anchor || local.starts_with(anchor+"/")) Check(entities.contains(local),"nested child deleted; edit its template before saving");
                }
                Json patches = entry.value("overrides", Json::object());
                // Source child root can be any local ID; persist it in the
                // expanded definition during instantiation for this operation.
                const std::string childRoot = entry.value("root", std::string());
                Check(!childRoot.empty(), "nested source lacks root identity; reload template before saving");
                std::vector<std::string> erase;
                for (auto& [local, current] : entities.items())
                {
                    if (local != anchor && !local.starts_with(anchor + "/")) continue;
                    const std::string childId = local == anchor ? childRoot : local.substr(anchor.size() + 1);
                    auto sourceId = std::find_if(map.begin(), map.end(), [&](const auto& item) { return item.second == local; });
                    Check(sourceId != map.end() && scene.at(sourceId->first).contains(Metadata), "new nested child requires editing its own template");
                    Json baseline = Json::parse(scene.at(sourceId->first).at(Metadata).at("BaselineJson").get<std::string>());
                    Json changed = current;
                    if (local == anchor)
                    {
                        entry["parent"] = Parent(changed);
                        baseline["TransformComponent"]["ParentUuid"] = "";
                        changed["TransformComponent"]["ParentUuid"] = "";
                    }
                    std::map<std::string, std::string> unqualify;
                    for (auto& [candidate, unused] : entities.items())
                        if (candidate == anchor || candidate.starts_with(anchor + "/")) unqualify[candidate] = candidate == anchor ? childRoot : candidate.substr(anchor.size() + 1);
                    RemapPrefabReferences(baseline, unqualify); RemapPrefabReferences(changed, unqualify);
                    Json patch = MergePatch(baseline, changed);
                    if (!patch.empty())
                    {
                        if (!patches.contains(childId)) patches[childId] = Json::object();
                        ComposePatch(patches[childId], patch);
                    }
                    erase.push_back(local);
                }
                for (const auto& local : erase) entities.erase(local);
                entry["overrides"] = patches; nested.push_back(std::move(entry));
            }
        }
        return {{"schemaVersion", 2}, {"root", map.at(root)}, {"entities", entities}, {"nested", nested}};
    }

    void TrackPrefabOverrides(const Json& before, Json& after)
    {
        if (!before.is_object() || !after.is_object() || !after.contains(Metadata)) return;
        std::set<std::string> paths;
        for (const Json& path : Json::parse(after.at(Metadata).at("OverridePathsJson").get<std::string>())) paths.insert(path.get<std::string>());
        ChangedPaths(StripMetadata(before), StripMetadata(after), "", paths);
        after[Metadata]["OverridePathsJson"] = Json(paths).dump();
    }

    void SetPrefabOverride(Json& value, const std::string& pointer, bool enabled,
        const std::map<std::string, std::string>& localToScene)
    {
        Check(value.contains(Metadata) && pointer.starts_with("/") && !pointer.starts_with("/PrefabInstanceComponent"), "invalid override pointer");
        const Json::json_pointer parsed(pointer);
        Check(!enabled || value.contains(parsed), "override field does not exist: " + pointer);
        Json paths = Json::parse(value.at(Metadata).at("OverridePathsJson").get<std::string>());
        std::set<std::string> unique;
        for (const Json& path : paths) unique.insert(path.get<std::string>());
        if (enabled) unique.insert(pointer);
        else
        {
            for (auto it = unique.begin(); it != unique.end();)
                if (*it == pointer || it->starts_with(pointer + "/")) it = unique.erase(it); else ++it;
            Json baseline = Json::parse(value.at(Metadata).at("BaselineJson").get<std::string>());
            RemapPrefabReferences(baseline, localToScene);
            if (baseline.contains(parsed)) value[parsed] = baseline.at(parsed);
            else if (value.contains(parsed)) value = value.patch(Json::array({{{"op","remove"},{"path",pointer}}}));
        }
        value[Metadata]["OverridePathsJson"] = Json(unique).dump();
    }
}
