#pragma once

#include <pybind11/pybind11.h>

namespace HEngine
{
	void GMBinding(pybind11::module_& engineModule);
	void InitializeProjectGMCommands();
	void ReloadProjectGMCommands();
	void ShutdownProjectGMCommands();
}
