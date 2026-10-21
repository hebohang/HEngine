#include "Editor/Panels/SequenceInspector.h"
#include "Editor/ImGuiWrapper/ImGuiWrapper.h"
#include "Runtime/EcsFramework/Component/Animation/SequenceComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Utils/PlatformUtils.h"
#include "_generated/serializer/engine/Runtime/EcsFramework/Component/Animation/SequenceComponent.gen.h"
#include <imgui/imgui.h>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

namespace HEngine
{
    namespace
    {
        bool Text(const char* label, std::string& value)
        {
            std::array<char, 1024> buffer{};
            std::memcpy(buffer.data(), value.data(), std::min(value.size(), buffer.size() - 1));
            if (!ImGui::InputText(label, buffer.data(), buffer.size())) return false;
            value = buffer.data();
            return true;
        }
    }
    bool DrawSequenceInspector(Entity entity, SequenceComponent& sequence)
    {
        bool changed = false;
        ImGui::TextWrapped("Preview in Play mode. Stop restores camera and input. Seeking is silent and does not replay events.");
        if (ImGui::Button(sequence.Playing ? "Stop##Sequence" : "Play##Sequence"))
        { sequence.Playing = !sequence.Playing; sequence.Paused = false; }
        ImGuiWrapper::SameLine(0.0f);
        ImGui::Checkbox("Pause##Sequence", &sequence.Paused);
        float time = sequence.Time;
        if (ImGui::SliderFloat("Time##Sequence", &time, 0.0f, std::max(0.01f, sequence.Duration), "%.3f s"))
        { sequence.SeekTime = time; sequence.Paused = true; sequence.Playing = true; }
        if (!sequence.Error.empty()) ImGui::TextWrapped("%s", sequence.Error.c_str());
        ImGui::BeginDisabled(sequence.Playing);
        changed |= ImGui::DragFloat("Duration", &sequence.Duration, 0.05f, 0.01f, 3600.0f);
        changed |= ImGui::DragFloat("Blend in", &sequence.BlendIn, 0.01f, 0.0f, sequence.Duration);
        changed |= ImGui::DragFloat("Blend out", &sequence.BlendOut, 0.01f, 0.0f, sequence.Duration);
        changed |= Text("Reference binding (optional)", sequence.ReferenceBinding);
        changed |= ImGui::DragFloat3("Authored reference position", glm::value_ptr(sequence.ReferencePosition), .01f);
        changed |= ImGui::DragFloat3("Authored reference scale", glm::value_ptr(sequence.ReferenceScale), .01f);
        glm::vec3 referenceDegrees = glm::degrees(sequence.ReferenceRotation);
        if (ImGui::DragFloat3("Authored reference rotation", glm::value_ptr(referenceDegrees), .2f))
        { sequence.ReferenceRotation = glm::radians(referenceDegrees); changed = true; }
        changed |= ImGui::Checkbox("Auto Play", &sequence.AutoPlay);
        ImGuiWrapper::SameLine(0.0f);
        changed |= ImGui::Checkbox("Loop", &sequence.Loop);
        if (ImGui::Button("Export .hsequence"))
        {
            auto path = FileDialogs::SaveFile("Sequence (*.hsequence)\0*.hsequence\0\0");
            if (!path.empty()) try
            {
                Json json;
                Serializer::Serialize(json, sequence);
                std::ofstream file(path);
                file << json.dump(2);
                if (!file) throw std::runtime_error("Could not save sequence.");
                sequence.Error.clear();
            }
            catch (const std::exception& error) { sequence.Error = error.what(); }
        }
        ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button("Import .hsequence"))
        {
            auto path = FileDialogs::OpenFile("Sequence (*.hsequence)\0*.hsequence\0\0");
            if (!path.empty()) try
            {
                std::ifstream file(path);
                Json json;
                file >> json;
                SequenceComponent imported;
                Serializer::Deserialize(json, imported);
                sequence = std::move(imported);
                changed = true;
            }
            catch (const std::exception& error) { sequence.Error = error.what(); }
        }
        if (ImGui::Button("Add track"))
        {
            SequenceTrack track;
            SequenceKey key;
            key.Value = 45.0f;
            track.Keys.push_back(key);
            sequence.Tracks.push_back(track);
            changed = true;
        }
        for (size_t index = 0; index < sequence.Tracks.size(); ++index)
        {
            ImGui::PushID(static_cast<int>(index));
            auto& track = sequence.Tracks[index];
            bool remove = false;
            if (ImGui::TreeNode("Track", "%zu: %s / %s", index, track.Kind.c_str(), track.Target.c_str()))
            {
                const char* kinds[] = {"camera", "transform", "float", "int", "bool", "expression", "lookat", "limb", "trigger", "audio", "event"};
                if (ImGui::BeginCombo("Kind", track.Kind.c_str()))
                {
                    for (const auto* kind : kinds)
                        if (ImGui::Selectable(kind, track.Kind == kind)) { track.Kind = kind; changed = true; }
                    ImGui::EndCombo();
                }
                changed |= Text("Binding name", track.Target);
                std::string binding = sequence.Bindings.contains(track.Target) ? sequence.Bindings.at(track.Target) : "";
                if (Text("Entity UUID", binding)) { sequence.Bindings[track.Target] = binding; changed = true; }
                changed |= Text("Parameter / audio path / event", track.Name);
                changed |= ImGui::Checkbox("Smooth interpolation", &track.Smooth);
                if (ImGui::Button("Add key"))
                {
                    SequenceKey key = track.Keys.empty() ? SequenceKey{} : track.Keys.back();
                    key.Time = track.Keys.empty() ? 0.0f : key.Time + 1.0f;
                    sequence.Duration = std::max(sequence.Duration, key.Time);
                    track.Keys.push_back(key);
                    changed = true;
                }
                ImGuiWrapper::SameLine(0.0f);
                if (ImGui::Button("Sort keys"))
                {
                    std::stable_sort(track.Keys.begin(), track.Keys.end(), [](const auto& a, const auto& b) { return a.Time < b.Time; });
                    changed = true;
                }
                for (size_t k = 0; k < track.Keys.size(); ++k)
                {
                    ImGui::PushID(static_cast<int>(k));
                    auto& key = track.Keys[k];
                    changed |= ImGui::DragFloat("Time", &key.Time, 0.01f, 0.0f, sequence.Duration);
                    changed |= ImGui::DragFloat(track.Kind == "camera" ? "FOV degrees" : "Value / volume", &key.Value, 0.01f);
                    if (track.Kind == "lookat")
                        changed |= ImGui::DragFloat3("World target", glm::value_ptr(key.Position), 0.01f);
                    if (track.Kind == "transform")
                    {
                        changed |= ImGui::DragFloat3("Position (world)", glm::value_ptr(key.Position), .01f);
                        glm::vec3 degrees = glm::degrees(key.Rotation);
                        if (ImGui::DragFloat3("Rotation degrees", glm::value_ptr(degrees), .2f))
                        { key.Rotation = glm::radians(degrees); changed = true; }
                    }
                    if (track.Kind == "camera")
                    {
                        changed |= ImGui::DragFloat3("Position (local)", glm::value_ptr(key.Position), 0.01f);
                        glm::vec3 degrees = glm::degrees(key.Rotation);
                        if (ImGui::DragFloat3("Rotation degrees", glm::value_ptr(degrees), 0.2f))
                        { key.Rotation = glm::radians(degrees); changed = true; }
                        if (ImGui::Button("Capture bound camera")) try
                        {
                            auto target = entity.GetLevel()->TryGetEntityByUUID(UUID(binding.empty() ? track.Target : binding));
                            if (target && target.HasComponent<TransformComponent>() && target.HasComponent<CameraComponent>())
                            {
                                auto& transform = target.GetComponent<TransformComponent>();
                                key.Position = transform.Translation;
                                key.Rotation = transform.Rotation;
                                key.Value = glm::degrees(target.GetComponent<CameraComponent>().Camera.GetPerspectiveVerticalFOV());
                                changed = true;
                            }
                            else sequence.Error = "Bind an entity with Camera and Transform first.";
                        }
                        catch (const std::exception& error) { sequence.Error = error.what(); }
                    }
                    const bool removeKey = ImGui::Button("Remove key");
                    ImGui::Separator();
                    ImGui::PopID();
                    if (removeKey) { track.Keys.erase(track.Keys.begin() + k); changed = true; break; }
                }
                remove = ImGui::Button("Remove track");
                ImGui::TreePop();
            }
            ImGui::PopID();
            if (remove) { sequence.Tracks.erase(sequence.Tracks.begin() + index); changed = true; break; }
        }
        ImGui::EndDisabled();
        return changed;
    }
}
