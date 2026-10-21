#include "ScriptFieldsJson.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace HEngine
{
    namespace
    {
        using FieldJson = nlohmann::json;
        constexpr std::array<const char*, 10> c_Tags = {
            "bool", "int64", "float64", "string", "vec2", "vec3", "vec4", "entity", "resource", "enum"};

        std::int64_t ReadInt(const FieldJson& value)
        {
            if (!value.is_number_integer() ||
                (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
                throw std::invalid_argument("Script field requires signed int64");
            return value.get<std::int64_t>();
        }

        double ReadNumber(const FieldJson& value)
        {
            if (!value.is_number()) throw std::invalid_argument("Script field requires a number");
            const auto number = value.get<double>();
            if (!std::isfinite(number)) throw std::invalid_argument("Non-finite script field");
            return number;
        }

        template<std::size_t N>
        std::array<float, N> ReadVector(const FieldJson& value)
        {
            if (!value.is_array() || value.size() != N) throw std::invalid_argument("Script vector dimension mismatch");
            std::array<float, N> result{};
            for (std::size_t i = 0; i < N; ++i)
            {
                const auto number = ReadNumber(value[i]);
                if (std::abs(number) > std::numeric_limits<float>::max()) throw std::invalid_argument("Script vector component overflow");
                result[i] = static_cast<float>(number);
            }
            return result;
        }
    }

    std::string EncodeScriptFields(const ScriptFields& fields)
    {
        if (fields.size() > 1024) throw std::invalid_argument("Too many script fields");
        FieldJson entries = FieldJson::object();
        bool aggregate = false;
        for (const auto& [id, value] : fields)
        {
            if (id.empty() || id.size() > 256) throw std::invalid_argument("Invalid script field ID");
            if (const auto* composite = std::get_if<ScriptAggregate>(&value))
            {
                const auto validated = ParseScriptAggregate(composite->Json);
                if (validated.Kind != composite->Kind || validated.Type != composite->Type) throw std::invalid_argument("Aggregate identity mismatch");
                entries[id] = FieldJson::parse(validated.Json); aggregate = true; continue;
            }
            FieldJson payload;
            std::visit([&](const auto& entry)
            {
                using T = std::decay_t<decltype(entry)>;
                if constexpr (std::is_same_v<T, ScriptEntityReference> || std::is_same_v<T, ScriptResourceReference>)
                    payload = entry.Id;
                else if constexpr (std::is_same_v<T, ScriptEnum>)
                    payload = {{"type", entry.Type}, {"value", entry.Value}};
                else if constexpr (std::is_same_v<T, ScriptAggregate>) throw std::logic_error("Aggregate requires schema 2");
                else if constexpr (std::is_same_v<T, double>)
                {
                    if (!std::isfinite(entry)) throw std::invalid_argument("Non-finite script field");
                    payload = entry;
                }
                else
                {
                    if constexpr (std::is_same_v<T, std::array<float, 2>> || std::is_same_v<T, std::array<float, 3>> || std::is_same_v<T, std::array<float, 4>>)
                        for (const auto component : entry)
                            if (!std::isfinite(component)) throw std::invalid_argument("Non-finite script vector");
                    payload = entry;
                }
            }, value);
            entries[id] = {{"type", c_Tags[value.index()]}, {"value", std::move(payload)}};
        }
        auto result = FieldJson({{"schema", aggregate ? 2 : 1}, {"fields", std::move(entries)}}).dump();
        if (result.size() > 1024 * 1024) throw std::invalid_argument("Script fields exceed 1 MiB");
        return result;
    }

    std::string RemapScriptEntityReferences(std::string_view json, const std::map<std::string, std::string>& ids, bool rejectExternal)
    {
        auto document = nlohmann::json::parse(EncodeScriptFields(DecodeScriptFields(json)));
        const auto remap = [&](const auto& self, nlohmann::json& field) -> void {
            const auto kind = field.at("type").get<std::string>();
            if (kind == "entity")
            {
                const auto id = field.at("value").get<std::string>();
                if (const auto found = ids.find(id); found != ids.end()) field["value"] = found->second;
                else if (rejectExternal && !id.empty()) throw std::invalid_argument("External script entity reference: " + id);
            }
            else if (kind == "struct") for (auto& child : field["value"]["fields"]) self(self, child);
            else if (kind == "list" || kind == "array")
            {
                for (auto& child : field["value"]["items"]) self(self, child);
                self(self, field["value"]["prototype"]);
            }
        };
        for (auto& field : document["fields"]) remap(remap, field);
        return document.dump();
    }

    ScriptFields DecodeScriptFields(std::string_view json)
    {
        if (json.size() > 1024 * 1024) throw std::invalid_argument("Script fields exceed 1 MiB");
        const auto root = FieldJson::parse(json.begin(), json.end(), [](int depth, FieldJson::parse_event_t, FieldJson&)
        {
            if (depth > 32) throw std::invalid_argument("Script fields nesting exceeds schema limits");
            return true;
        });
        if (!root.is_object() || (root.at("schema") != 1 && root.at("schema") != 2) || !root.at("fields").is_object())
            throw std::invalid_argument("Unsupported script field schema");
        const auto& entries = root.at("fields");
        if (entries.size() > 1024) throw std::invalid_argument("Too many script fields");
        ScriptFields result;
        for (auto it = entries.begin(); it != entries.end(); ++it)
        {
            if (it.key().empty() || it.key().size() > 256) throw std::invalid_argument("Invalid script field ID");
            const auto tag = it->at("type").get<std::string>();
            const auto& value = it->at("value");
            ScriptValue field;
            if (tag == "bool") field = value.get<bool>();
            else if (tag == "int64") field = ReadInt(value);
            else if (tag == "float64") field = ReadNumber(value);
            else if (tag == "string") field = value.get<std::string>();
            else if (tag == "vec2") field = ReadVector<2>(value);
            else if (tag == "vec3") field = ReadVector<3>(value);
            else if (tag == "vec4") field = ReadVector<4>(value);
            else if (tag == "entity") field = ScriptEntityReference{value.get<std::string>()};
            else if (tag == "resource") field = ScriptResourceReference{value.get<std::string>()};
            else if (tag == "enum") field = ScriptEnum{value.at("type").get<std::string>(), ReadInt(value.at("value"))};
            else if (tag == "struct" || tag == "list" || tag == "array")
            {
                if (root.at("schema") != 2) throw std::invalid_argument("Aggregate fields require schema 2");
                field = ParseScriptAggregate(it->dump());
            }
            else throw std::invalid_argument("Unsupported script field type: " + tag);
            result.emplace(it.key(), std::move(field));
        }
        return result;
    }
}
