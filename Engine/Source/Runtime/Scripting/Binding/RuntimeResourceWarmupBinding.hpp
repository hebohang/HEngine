#pragma once

#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeResourceWarmupManager.h"

#include <pybind11/embed.h>

#include <stdexcept>
#include <string>

namespace py = pybind11;

namespace HEngine
{
    static RuntimeResourceWarmupManager& RequireRuntimeResourceWarmupManager()
    {
        RuntimeResourceWarmupManager& manager = RuntimeResourceWarmupManager::GetInstance();
        if (!manager.IsInitialized())
            throw std::runtime_error("Runtime resource warm-up manager is not initialized.");
        return manager;
    }

    static void RuntimeResourceWarmupBinding(py::module_& HEngineModule)
    {
        py::module_ warmup = HEngineModule.def_submodule("RuntimeResourceWarmup");

        py::enum_<RuntimeResourceWarmupGroupState>(warmup, "State")
            .value("Idle", RuntimeResourceWarmupGroupState::Idle)
            .value("Queued", RuntimeResourceWarmupGroupState::Queued)
            .value("Loading", RuntimeResourceWarmupGroupState::Loading)
            .value("Ready", RuntimeResourceWarmupGroupState::Ready)
            .value("Failed", RuntimeResourceWarmupGroupState::Failed)
            .value("Released", RuntimeResourceWarmupGroupState::Released)
            .export_values();

        py::class_<RuntimeResourceWarmupGroupProgress>(warmup, "GroupProgress")
            .def_readonly("State", &RuntimeResourceWarmupGroupProgress::State)
            .def_readonly("Total", &RuntimeResourceWarmupGroupProgress::Total)
            .def_readonly("Undispatched", &RuntimeResourceWarmupGroupProgress::Undispatched)
            .def_readonly("Dispatched", &RuntimeResourceWarmupGroupProgress::Dispatched)
            .def_readonly("Ready", &RuntimeResourceWarmupGroupProgress::Ready)
            .def_readonly("Failed", &RuntimeResourceWarmupGroupProgress::Failed)
            .def_readonly("Loading", &RuntimeResourceWarmupGroupProgress::Loading)
            .def_readonly("Skipped", &RuntimeResourceWarmupGroupProgress::Skipped)
            .def_readonly("Ratio", &RuntimeResourceWarmupGroupProgress::Ratio);

        py::class_<RuntimeResourceWarmupTriggerProgress>(warmup, "TriggerProgress")
            .def_readonly("TotalGroups", &RuntimeResourceWarmupTriggerProgress::TotalGroups)
            .def_readonly("IdleGroups", &RuntimeResourceWarmupTriggerProgress::IdleGroups)
            .def_readonly("QueuedGroups", &RuntimeResourceWarmupTriggerProgress::QueuedGroups)
            .def_readonly("LoadingGroups", &RuntimeResourceWarmupTriggerProgress::LoadingGroups)
            .def_readonly("ReadyGroups", &RuntimeResourceWarmupTriggerProgress::ReadyGroups)
            .def_readonly("FailedGroups", &RuntimeResourceWarmupTriggerProgress::FailedGroups)
            .def_readonly("ReleasedGroups", &RuntimeResourceWarmupTriggerProgress::ReleasedGroups)
            .def_readonly("Total", &RuntimeResourceWarmupTriggerProgress::Total)
            .def_readonly("Undispatched", &RuntimeResourceWarmupTriggerProgress::Undispatched)
            .def_readonly("Dispatched", &RuntimeResourceWarmupTriggerProgress::Dispatched)
            .def_readonly("Ready", &RuntimeResourceWarmupTriggerProgress::Ready)
            .def_readonly("Failed", &RuntimeResourceWarmupTriggerProgress::Failed)
            .def_readonly("Loading", &RuntimeResourceWarmupTriggerProgress::Loading)
            .def_readonly("Skipped", &RuntimeResourceWarmupTriggerProgress::Skipped)
            .def_readonly("Ratio", &RuntimeResourceWarmupTriggerProgress::Ratio);

        warmup.def("QueueTrigger", [](const std::string& trigger)
        {
            return RequireRuntimeResourceWarmupManager().QueueTrigger(trigger);
        }, py::arg("name"));

        warmup.def("GetGroupProgress", [](const std::string& groupName) -> py::object
        {
            const auto progress = RequireRuntimeResourceWarmupManager().GetGroupProgress(groupName);
            return progress
                ? py::cast(*progress, py::return_value_policy::copy)
                : py::none();
        }, py::arg("name"));

        warmup.def("GetTriggerProgress", [](const std::string& trigger) -> py::object
        {
            const auto progress = RequireRuntimeResourceWarmupManager().GetTriggerProgress(trigger);
            return progress
                ? py::cast(*progress, py::return_value_policy::copy)
                : py::none();
        }, py::arg("name"));

        warmup.def("RethrowFailure", [](const std::string& groupName)
        {
            RequireRuntimeResourceWarmupManager().RethrowFailure(groupName);
        }, py::arg("name"));

        warmup.def("ReleaseGroup", [](const std::string& groupName)
        {
            return RequireRuntimeResourceWarmupManager().ReleaseGroup(groupName);
        }, py::arg("name"));
    }
}
