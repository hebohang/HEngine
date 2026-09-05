#pragma once

#include "Runtime/Core/Log/Log.h"

#include <pybind11/embed.h>

namespace HEngine
{
    void PyLogTrace(const std::string & log)        { HE_TRACE(log); }
    void PyLogInfo(const std::string & log)         { HE_INFO(log); }
    void PyLogWarn(const std::string & log)         { HE_WARN(log); }
    void PyLogError(const std::string & log)        { HE_ERROR(log); }    
    void PyCoreLogTrace(const std::string & log)    { HE_CORE_TRACE(log); }
    void PyCoreLogInfo(const std::string & log)     { HE_CORE_INFO(log); }
    void PyCoreLogWarn(const std::string & log)     { HE_CORE_WARN(log); }
    void PyCoreLogError(const std::string & log)    { HE_CORE_ERROR(log); }

#ifdef WITH_EDITOR
    void PyEditorLog(const char* log) { HE_CORE_INFO("{}", log); }
#endif

    static void LogBinding(py::module_& HEngineModule)
    {
        HEngineModule.def("HE_TRACE",       &PyLogTrace);
        HEngineModule.def("HE_INFO",        &PyLogInfo);
        HEngineModule.def("HE_WARN",        &PyLogWarn);
        HEngineModule.def("HE_ERROR",       &PyLogError);        
        HEngineModule.def("HE_CORE_TRACE",  &PyCoreLogTrace);
        HEngineModule.def("HE_CORE_INFO",   &PyCoreLogInfo);
        HEngineModule.def("HE_CORE_WARN",   &PyCoreLogWarn);
        HEngineModule.def("HE_CORE_ERROR",  &PyCoreLogError);

#ifdef WITH_EDITOR
        HEngineModule.def("RAW_LOG",  &PyEditorLog);
#endif
    }
}
