#include "ScriptFieldsJson.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace HEngine
{
    namespace
    {
        using Json = nlohmann::json;
        bool Aggregate(const std::string& tag) { return tag == "struct" || tag == "list" || tag == "array"; }
        void Validate(const Json& field, unsigned depth, unsigned& count)
        {
            if (depth > 4 || ++count > 4096) throw std::invalid_argument("Script aggregate exceeds depth 4 or 4096 values");
            const auto tag = field.at("type").get<std::string>();
            const auto& value = field.at("value");
            if (!Aggregate(tag))
            {
                (void)DecodeScriptFields(Json{{"schema", 1}, {"fields", {{"member", field}}}}.dump());
                return;
            }
            const auto identity = value.at("type").get<std::string>();
            if (identity.empty() || identity.size() > 256) throw std::invalid_argument("Invalid aggregate identity");
            const auto& children = value.at(tag == "struct" ? "fields" : "items");
            if ((tag == "struct" ? !children.is_object() : !children.is_array()) || children.size() > 256)
                throw std::invalid_argument("Script aggregate requires at most 256 typed members/items");
            for (auto it = children.begin(); it != children.end(); ++it)
            {
                if (tag == "struct" && (it.key().empty() || it.key().size() > 256)) throw std::invalid_argument("Invalid structure field ID");
                Validate(*it, depth + 1, count);
            }
            if (tag != "struct") Validate(value.at("prototype"), depth + 1, count);
        }
        Json Merge(const Json& previous, Json current)
        {
            if (previous.at("type") != current.at("type")) return previous;
            const auto tag = current.at("type").get<std::string>();
            if (!Aggregate(tag)) return current;
            if (previous.at("value").at("type") != current.at("value").at("type")) return previous;
            const char* key = tag == "struct" ? "fields" : "items";
            const auto& old = previous.at("value").at(key);
            auto& values = current["value"][key];
            if (tag == "struct")
            {
                for (auto it = old.begin(); it != old.end(); ++it)
                    if (!values.contains(it.key())) values[it.key()] = it.value();
                    else values[it.key()] = Merge(it.value(), values[it.key()]);
            }
            else for (std::size_t index = 0; index < std::min(old.size(), values.size()); ++index) values[index] = Merge(old[index], values[index]);
            return current;
        }
    }
    ScriptAggregate ParseScriptAggregate(std::string_view text)
    {
        if (text.size() > 1024 * 1024) throw std::invalid_argument("Script aggregate exceeds 1 MiB");
        const auto value = Json::parse(text.begin(), text.end(), [](int depth, Json::parse_event_t, Json&) {
            if (depth > 32) throw std::invalid_argument("Aggregate JSON exceeds depth limit"); return true;
        });
        const auto tag = value.at("type").get<std::string>();
        if (!Aggregate(tag)) throw std::invalid_argument("Expected aggregate field");
        unsigned count = 0; Validate(value, 0, count);
        return {tag, value.at("value").at("type").get<std::string>(), value.dump()};
    }
    ScriptValue PreserveUnknownScriptMembers(const ScriptValue& previous, ScriptValue current)
    {
        if (const auto* old = std::get_if<ScriptAggregate>(&previous))
            if (auto* value = std::get_if<ScriptAggregate>(&current); value && old->Kind == value->Kind && old->Type == value->Type)
                *value = ParseScriptAggregate(Merge(Json::parse(old->Json), Json::parse(value->Json)).dump());
        return current;
    }
    std::vector<std::string> ScriptReferenceIds(const ScriptValue& value, bool resources)
    {
        std::vector<std::string> result;
        if (const auto* reference = std::get_if<ScriptResourceReference>(&value); resources && reference) result.push_back(reference->Id);
        if (const auto* reference = std::get_if<ScriptEntityReference>(&value); !resources && reference) result.push_back(reference->Id);
        if (const auto* aggregate = std::get_if<ScriptAggregate>(&value))
        {
            const auto visit = [&](const auto& self, const Json& field) -> void {
                const auto tag = field.at("type").get<std::string>();
                if (tag == (resources ? "resource" : "entity")) result.push_back(field.at("value").get<std::string>());
                else if (Aggregate(tag)) for (const auto& child : field.at("value").at(tag == "struct" ? "fields" : "items")) self(self, child);
            };
            visit(visit, Json::parse(aggregate->Json));
        }
        return result;
    }
}
