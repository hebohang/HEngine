#pragma once

#include "Runtime/Graphics/VFX/EffectTransformBatch.h"
#include "Runtime/Graphics/VFX/EffekseerRuntime.h"

#include <pybind11/embed.h>
#include <pybind11/stl.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace HEngine
{
    static void VFXBinding(py::module_& HEngineModule)
    {
        py::module_ vfx = HEngineModule.def_submodule("VFX");
        vfx.def("Describe", []()
        {
            return py::module_::import("json").attr("loads")(EffekseerRuntime::Describe().dump());
        });
        vfx.def("ReloadEffect", &EffekseerRuntime::ReloadEffect, py::arg("relativePath"));
        vfx.def("SetParameter", &EffekseerRuntime::SetEffectParameter,
            py::arg("handle"), py::arg("name"), py::arg("value"));
        vfx.def("GetEffectPosition", [](int handle)
        {
            const auto position = EffekseerRuntime::GetEffectPosition(handle);
            return py::make_tuple(position.x, position.y, position.z);
        }, py::arg("handle"));

        vfx.def("LoadEffect", [](const std::string& relativePath)
        {
            return EffekseerRuntime::LoadEffect(relativePath);
        }, py::arg("relativePath"));

        vfx.def("PlayEffect", [](const std::string& relativePath, float x, float y, float z)
        {
            auto handle = EffekseerRuntime::PlayEffect(relativePath, glm::vec3(x, y, z));
            return static_cast<int>(handle);
        }, py::arg("relativePath"), py::arg("x"), py::arg("y"), py::arg("z"));

        vfx.def("PlayEffectAdvanced",
            [](const std::string& relativePath,
               float x, float y, float z,
               float fx, float fy, float fz,
               float scale,
               float speed,
               int category)
        {
            if (category < static_cast<int>(VFXBudgetCategory::GameplayCritical) ||
                category >= static_cast<int>(VFXBudgetCategory::Count))
            {
                throw py::value_error("category must be 0 (GameplayCritical), 1 (Combat), 2 (Ambient), or 3 (UI).");
            }

            VFXPlayParams params;
            params.Position = glm::vec3(x, y, z);
            params.Forward = glm::vec3(fx, fy, fz);
            params.Scale = scale;
            params.Speed = speed;
            params.Category = static_cast<VFXBudgetCategory>(category);
            return static_cast<int>(EffekseerRuntime::PlayEffect(relativePath, params));
        },
        py::arg("relativePath"),
        py::arg("x"), py::arg("y"), py::arg("z"),
        py::arg("fx"), py::arg("fy"), py::arg("fz"),
        py::arg("scale") = 1.0f,
        py::arg("speed") = 1.0f,
        py::arg("category") = static_cast<int>(VFXBudgetCategory::Combat));

        vfx.def("SetEffectTransform",
            [](int handle,
               float x, float y, float z,
               float fx, float fy, float fz)
        {
            return EffekseerRuntime::SetEffectTransform(
                static_cast<VFXHandle>(handle),
                glm::vec3(x, y, z),
                glm::vec3(fx, fy, fz));
        },
        py::arg("handle"),
        py::arg("x"), py::arg("y"), py::arg("z"),
        py::arg("fx"), py::arg("fy"), py::arg("fz"));

        vfx.def("SetEffectTransforms",
            [](const py::iterable& transforms)
        {
            static thread_local bool sBatchActive = false;
            if (sBatchActive)
                throw std::runtime_error("Recursive effect transform batches are not supported.");

            struct BatchCallScope
            {
                explicit BatchCallScope(bool& bActive)
                    : bBatchActive(bActive)
                {
                    bBatchActive = true;
                }

                ~BatchCallScope()
                {
                    bBatchActive = false;
                }

                bool& bBatchActive;
            };

            BatchCallScope batchCallScope(sBatchActive);
            static thread_local std::vector<EffectTransformCommand> sCommands;
            static thread_local std::vector<uint8_t> sResults;

            sCommands.clear();
            for (const py::handle transformHandle : transforms)
            {
                if (!py::isinstance<py::sequence>(transformHandle))
                    throw py::type_error("Each effect transform must be a seven-item sequence.");

                const py::sequence transform = py::reinterpret_borrow<py::sequence>(transformHandle);
                if (py::len(transform) != 7)
                    throw py::value_error("Each effect transform must contain exactly seven values.");

                EffectTransformCommand command = {
                    .Handle = transform[0].cast<int32_t>(),
                    .PositionX = transform[1].cast<float>(),
                    .PositionY = transform[2].cast<float>(),
                    .PositionZ = transform[3].cast<float>(),
                    .ForwardX = transform[4].cast<float>(),
                    .ForwardY = transform[5].cast<float>(),
                    .ForwardZ = transform[6].cast<float>()
                };
                if (!std::isfinite(command.PositionX) ||
                    !std::isfinite(command.PositionY) ||
                    !std::isfinite(command.PositionZ) ||
                    !std::isfinite(command.ForwardX) ||
                    !std::isfinite(command.ForwardY) ||
                    !std::isfinite(command.ForwardZ))
                {
                    throw py::value_error("Effect transform values must be finite.");
                }

                sCommands.emplace_back(command);
            }

            py::tuple results(sCommands.size());
            for (std::size_t i = 0; i < sCommands.size(); ++i)
                results[i] = py::bool_(false);

            EffekseerRuntime::SetEffectTransforms(sCommands, sResults);

            for (std::size_t i = 0; i < sResults.size(); ++i)
                results[i] = py::bool_(sResults[i] != 0);
            return results;
        }, py::arg("transforms"));

        vfx.def("IsEffectPlaying", [](int handle)
        {
            return EffekseerRuntime::IsEffectPlaying(
                static_cast<VFXHandle>(handle));
        }, py::arg("handle"));

        vfx.def("StopEffect", [](int handle)
        {
            EffekseerRuntime::StopEffect(static_cast<VFXHandle>(handle));
        }, py::arg("handle"));

        vfx.def("StopAll", []()
        {
            EffekseerRuntime::StopAllEffects();
        });

        const auto getStatsSnapshot = []()
        {
            const VFXStats stats = EffekseerRuntime::GetStats();
            py::dict result;
            const auto addMetric = [&result](const char* name, const VFXRollingMetric& metric)
            {
                py::dict value;
                value["current"] = metric.Current;
                value["peak"] = metric.Peak;
                value["p50"] = metric.P50;
                value["p95"] = metric.P95;
                value["p99"] = metric.P99;
                value["sample_count"] = metric.SampleCount;
                value["status"] = static_cast<int>(metric.Status);
                result[name] = std::move(value);
            };

            addMetric("cpu_update_ms", stats.CpuUpdateMs);
            addMetric("cpu_draw_submit_ms", stats.CpuDrawSubmitMs);
            addMetric("effect_load_ms", stats.EffectLoadMs);
            addMetric("first_play_ms", stats.FirstPlayMs);
            addMetric("batch_transform_ms", stats.BatchTransformMs);
            addMetric("gpu_simulation_ms", stats.GpuSimulationMs);
            addMetric("gpu_render_ms", stats.GpuRenderMs);
            addMetric("gpu_background_copy_ms", stats.GpuBackgroundCopyMs);
            addMetric("gpu_transparent_render_ms", stats.GpuTransparentRenderMs);
            addMetric("active_effects", stats.ActiveEffects);
            addMetric("active_cpu_instances", stats.ActiveCpuInstances);
            addMetric("active_gpu_particles", stats.ActiveGpuParticles);
            addMetric("effects_created_this_frame", stats.EffectsCreatedThisFrame);
            addMetric("effects_destroyed_this_frame", stats.EffectsDestroyedThisFrame);
            addMetric("cpu_instances_created_this_frame", stats.CpuInstancesCreatedThisFrame);
            addMetric("cpu_instances_destroyed_this_frame", stats.CpuInstancesDestroyedThisFrame);
            addMetric("cached_effects", stats.CachedEffects);
            addMetric("cached_packages", stats.CachedPackages);
            addMetric("draw_calls", stats.DrawCalls);
            addMetric("vertex_count", stats.VertexCount);
            addMetric("particle_buffer_used", stats.ParticleBufferUsed);
            addMetric("particle_buffer_capacity", stats.ParticleBufferCapacity);
            addMetric("budget_exceeded_count", stats.BudgetExceededCount);
            addMetric("budget_rejected_count", stats.BudgetRejectedCount);
            result["highest_cost_effect_path"] = stats.HighestCostEffectPath;
            result["highest_cost_effect_estimated_instances"] = stats.HighestCostEffectEstimatedInstances;
            result["highest_cost_effect_status"] = static_cast<int>(stats.HighestCostEffectStatus);
            result["quality_tier"] = static_cast<int>(stats.QualityTier);
            result["gpu_particles_capable"] = stats.bGpuParticlesCapable;
            result["gpu_particles_enabled"] = stats.bGpuParticlesEnabled;
            result["update_serial"] = stats.UpdateSerial;
            result["gpu_emitter_slot_failures"] = stats.GpuEmitterSlotFailures;
            result["gpu_particle_block_failures"] = stats.GpuParticleBlockFailures;
            result["gpu_trail_block_failures"] = stats.GpuTrailBlockFailures;
            result["gpu_allocated_emitters"] = stats.GpuAllocatedEmitters;
            result["gpu_reserved_particles"] = stats.GpuReservedParticles;
            result["gpu_reserved_trails"] = stats.GpuReservedTrails;
            result["gpu_allocation_scope"] = "current GPU system CPU allocator; not live GPU particles or fence-retired memory";
            result["last_gpu_timing_submission"] = stats.LastGpuTimingSubmission;
            result["last_gpu_simulation_submission"] = stats.LastGpuSimulationSubmission;
            result["last_gpu_render_submission"] = stats.LastGpuRenderSubmission;
            result["update_serial_scope"] = "last completed VFX manager update; not world frame identity";
            return result;
        };
        vfx.def("GetStats", getStatsSnapshot);
        vfx.def("GetStatsSnapshot", getStatsSnapshot);

        vfx.def("IsGpuParticleCapable", &EffekseerRuntime::IsGpuParticleCapable);
        vfx.def("PlaySeeded", [](const std::string& relativePath, float x, float y, float z,
                                  int32_t seed, float scale, float speed)
        {
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
                !std::isfinite(scale) || scale <= 0 || scale > 1000 ||
                !std::isfinite(speed) || speed < 0 || speed > 16)
                throw py::value_error("position must be finite; scale in (0,1000], speed in [0,16]");
            VFXPlayParams params;
            params.Position = {x, y, z};
            params.Scale = scale;
            params.Speed = speed;
            params.bOverrideRandomSeed = true;
            params.RandomSeed = seed;
            return EffekseerRuntime::PlayEffect(relativePath, params);
        }, py::arg("relativePath"), py::arg("x"), py::arg("y"), py::arg("z"),
            py::arg("seed"), py::arg("scale") = 1.0f, py::arg("speed") = 1.0f);
        vfx.def("SetPaused", &EffekseerRuntime::SetEffectPaused,
            py::arg("handle"), py::arg("paused"));
        vfx.def("SetSpeed", &EffekseerRuntime::SetEffectSpeed,
            py::arg("handle"), py::arg("speed"));
        vfx.def("SetColor", [](int handle, float r, float g, float b, float a)
        {
            return EffekseerRuntime::SetEffectColor(handle, {r, g, b, a});
        }, py::arg("handle"), py::arg("r"), py::arg("g"), py::arg("b"), py::arg("a") = 1.0f);
        vfx.def("SetDynamicInput", &EffekseerRuntime::SetEffectDynamicInput,
            py::arg("handle"), py::arg("index"), py::arg("value"));
        vfx.def("IsGpuParticlesEnabled", &EffekseerRuntime::IsGpuParticlesEnabled);
        vfx.def("SetGpuParticlesEnabled", &EffekseerRuntime::SetGpuParticlesEnabled,
                py::arg("enabled"));

        vfx.def("SetQualityTier", [](int qualityTier)
        {
            if (qualityTier < static_cast<int>(VFXQualityTier::Low) ||
                qualityTier >= static_cast<int>(VFXQualityTier::Count))
            {
                throw py::value_error("qualityTier must be 0 (Low), 1 (Medium), or 2 (High).");
            }
            EffekseerRuntime::SetQualityTier(static_cast<VFXQualityTier>(qualityTier));
        }, py::arg("qualityTier"));

        vfx.def("ValidateEffectPackages", [](const std::vector<std::string>& relativePaths)
        {
            return EffekseerRuntime::ValidateEffectPackages(relativePaths);
        }, py::arg("relativePaths"));
        vfx.def("ValidateAllEffectPackages", &EffekseerRuntime::ValidateAllEffectPackages);
    }
}
