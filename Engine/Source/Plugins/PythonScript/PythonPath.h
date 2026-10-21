#pragma once

#include <pybind11/pytypes.h>

#include <filesystem>

namespace HEngine
{
	[[nodiscard]] inline pybind11::str ToPythonPath(
		const std::filesystem::path& path)
	{
#if defined(_WIN32)
		const std::wstring widePath = path.wstring();
		PyObject* value = PyUnicode_FromWideChar(
			widePath.data(),
			static_cast<Py_ssize_t>(widePath.size()));
		if (value == nullptr)
			throw pybind11::error_already_set();
		return pybind11::reinterpret_steal<pybind11::str>(value);
#else
		return pybind11::str(path.string());
#endif
	}
}
