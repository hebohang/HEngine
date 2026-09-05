#pragma once

#include "Common/Precompiled.h"

class Class;

using AssignabilityDiagnosticReporter = std::function<void(
    const std::string& severity,
    const std::string& source,
    unsigned line,
    unsigned column,
    const std::string& message)>;

bool ValidateFieldAssignability(
    CXIndex index,
    const fs::path& inputHeader,
    const fs::path& scratchFile,
    const std::vector<std::string>& clangArguments,
    const std::vector<std::shared_ptr<Class>>& classes,
    const AssignabilityDiagnosticReporter& report);
