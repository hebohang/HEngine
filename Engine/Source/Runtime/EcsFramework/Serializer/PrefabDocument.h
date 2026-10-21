#pragma once

#include "Runtime/Core/Meta/Json.h"

#include <functional>
#include <map>
#include <string>

namespace HEngine
{
    // Pure authoring transforms. All references use explicitly declared slots;
    // arbitrary strings (including Python source) are never rewritten.
    struct PrefabDocument
    {
        std::string Root;
        Json Entities = Json::object();
        std::string Revision;
        Json Source = Json::object();
        // Old component-object prefabs instantiate as ordinary entities.
        // A v2 parent still owns metadata for its nested legacy children.
        bool bLegacySingleEntity = false;
    };

    struct PrefabInstantiation
    {
        std::string Root;
        Json Values = Json::object();
    };

    using PrefabLoader = std::function<Json(const std::string&)>;
    using PrefabIdFactory = std::function<std::string()>;
    Json NormalizePrefabDocument(const Json& document);
    PrefabDocument ExpandPrefabDocument(const Json& document, const PrefabLoader& loader,
        const std::string& path = {});
    void RemapPrefabReferences(Json& components, const std::map<std::string, std::string>& ids,
        bool rejectExternal = false);
    PrefabInstantiation InstantiatePrefabDocument(const PrefabDocument& prefab,
        const std::string& path, const PrefabIdFactory& ids);
    Json SynchronizePrefabDocument(const Json& scene, const std::string& root,
        const PrefabDocument& prefab, const PrefabIdFactory& ids);
    Json CapturePrefabDocument(const Json& scene, const std::string& root, const std::string& path);
    // Record explicit component/field overrides after ordinary scene operations.
    void TrackPrefabOverrides(const Json& before, Json& after);
    void SetPrefabOverride(Json& value, const std::string& pointer, bool enabled,
        const std::map<std::string, std::string>& localToScene = {});
}
