#include "Runtime/Platform/Interop/OidnDenoiser.h"
#include "Runtime/Platform/PlatformPaths.h"

#if defined(_WIN32)
#include <Windows.h>
#else
#include <dlfcn.h>
#endif
#include <OpenImageDenoise/oidn.h>

#include <stdexcept>
#include <string>

namespace HEngine
{
    struct OidnDenoiser::Impl
    {
#if defined(_WIN32)
        HMODULE Module = nullptr, CpuModule = nullptr;
#else
        void* Module = nullptr;
        void* CpuModule = nullptr;
#endif
        OIDNDevice Device = nullptr;
        OIDNFilter Filter = nullptr;
#define HE_OIDN_FUNCTION(name) decltype(&name) name##Fn = nullptr
        HE_OIDN_FUNCTION(oidnNewDevice);
        HE_OIDN_FUNCTION(oidnReleaseDevice);
        HE_OIDN_FUNCTION(oidnSetDeviceInt);
        HE_OIDN_FUNCTION(oidnSetDeviceBool);
        HE_OIDN_FUNCTION(oidnGetDeviceInt);
        HE_OIDN_FUNCTION(oidnGetDeviceError);
        HE_OIDN_FUNCTION(oidnCommitDevice);
        HE_OIDN_FUNCTION(oidnNewFilter);
        HE_OIDN_FUNCTION(oidnReleaseFilter);
        HE_OIDN_FUNCTION(oidnSetFilterBool);
        HE_OIDN_FUNCTION(oidnSetSharedFilterImage);
        HE_OIDN_FUNCTION(oidnCommitFilter);
        HE_OIDN_FUNCTION(oidnExecuteFilter);
#undef HE_OIDN_FUNCTION

        ~Impl()
        {
            if (Filter && oidnReleaseFilterFn) oidnReleaseFilterFn(Filter);
            if (Device && oidnReleaseDeviceFn) oidnReleaseDeviceFn(Device);
#if defined(_WIN32)
            if (Module) FreeLibrary(Module);
            if (CpuModule) FreeLibrary(CpuModule);
#else
            if (Module) dlclose(Module);
            if (CpuModule) dlclose(CpuModule);
#endif
        }
        void Check()
        {
            const char* message = nullptr;
            if (oidnGetDeviceErrorFn(Device, &message) != OIDN_ERROR_NONE)
                throw std::runtime_error(message ? message : "OIDN CPU execution failed");
        }
        void Initialize()
        {
#if defined(_WIN32)
            const auto root = PlatformPaths::GetExecutableDirectory() / "Plugins/OfflineReferenceRenderer";
            constexpr DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
            CpuModule = LoadLibraryExW((root / "OpenImageDenoise_device_cpu.dll").c_str(), nullptr, flags);
            if (!CpuModule) throw std::runtime_error("Cannot load offline plugin OIDN CPU runtime (Windows error " + std::to_string(GetLastError()) + ")");
            Module = LoadLibraryExW((root / "OpenImageDenoise.dll").c_str(), nullptr, flags);
            if (!Module) throw std::runtime_error("Cannot load offline plugin OIDN runtime (Windows error " + std::to_string(GetLastError()) + ")");
#define HE_OIDN_SYMBOL(name) GetProcAddress(Module, name)
#else
            const auto executable = PlatformPaths::GetExecutableDirectory();
            const auto root = executable.filename() == "MacOS" ?
                executable.parent_path() / "Frameworks" : executable / "Plugins/OfflineReferenceRenderer";
            const auto cpuPath = root / "libOpenImageDenoise_device_cpu.2.5.1.dylib";
            CpuModule = dlopen(cpuPath.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!CpuModule)
            {
                const char* error = dlerror();
                throw std::runtime_error("Cannot load offline OIDN CPU runtime: " + std::string(error ? error : "unknown loader error"));
            }
            Module = dlopen((root / "libOpenImageDenoise.2.5.1.dylib").c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!Module)
            {
                const char* error = dlerror();
                throw std::runtime_error("Cannot load offline OIDN runtime: " + std::string(error ? error : "unknown loader error"));
            }
#define HE_OIDN_SYMBOL(name) dlsym(Module, name)
#endif
#define HE_OIDN_LOAD(name) \
            name##Fn = reinterpret_cast<decltype(&name)>(HE_OIDN_SYMBOL(#name)); \
            if (!name##Fn) throw std::runtime_error("OIDN runtime export missing: " #name)
            HE_OIDN_LOAD(oidnNewDevice);
            HE_OIDN_LOAD(oidnReleaseDevice);
            HE_OIDN_LOAD(oidnSetDeviceInt);
            HE_OIDN_LOAD(oidnSetDeviceBool);
            HE_OIDN_LOAD(oidnGetDeviceInt);
            HE_OIDN_LOAD(oidnGetDeviceError);
            HE_OIDN_LOAD(oidnCommitDevice);
            HE_OIDN_LOAD(oidnNewFilter);
            HE_OIDN_LOAD(oidnReleaseFilter);
            HE_OIDN_LOAD(oidnSetFilterBool);
            HE_OIDN_LOAD(oidnSetSharedFilterImage);
            HE_OIDN_LOAD(oidnCommitFilter);
            HE_OIDN_LOAD(oidnExecuteFilter);
#undef HE_OIDN_LOAD
#undef HE_OIDN_SYMBOL
            Device = oidnNewDeviceFn(OIDN_DEVICE_TYPE_CPU);
            Check();
            if (!Device) throw std::runtime_error("OIDN CPU device unavailable");
            if (oidnGetDeviceIntFn(Device, "version") != 20501)
                throw std::runtime_error("Offline plugin requires OIDN 2.5.1");
            oidnSetDeviceIntFn(Device, "numThreads", 4);
            oidnSetDeviceBoolFn(Device, "setAffinity", false);
            oidnCommitDeviceFn(Device);
            Check();
            Filter = oidnNewFilterFn(Device, "RT");
            Check();
            if (!Filter) throw std::runtime_error("OIDN RT filter unavailable");
            oidnSetFilterBoolFn(Filter, "hdr", true);
            oidnSetFilterBoolFn(Filter, "cleanAux", false);
        }
    };

    OidnDenoiser::OidnDenoiser() : mImpl(std::make_unique<Impl>()) { mImpl->Initialize(); }
    OidnDenoiser::~OidnDenoiser() = default;
    void OidnDenoiser::Run(uint32_t width, uint32_t height, std::span<float> color,
        std::span<float> albedo, std::span<float> normal, std::span<float> output)
    {
        const auto size = size_t(width) * height * 4;
        if (!width || !height || width > 4096 || height > 4096 ||
            color.size() != size || albedo.size() != size || normal.size() != size || output.size() != size)
            throw std::invalid_argument("OIDN snapshot dimensions do not match its inputs");
        auto bind = [&](const char* name, std::span<float> data) {
            mImpl->oidnSetSharedFilterImageFn(mImpl->Filter, name, data.data(), OIDN_FORMAT_FLOAT3,
                width, height, 0, sizeof(float) * 4, 0);
        };
        bind("color", color); bind("albedo", albedo); bind("normal", normal); bind("output", output);
        mImpl->oidnCommitFilterFn(mImpl->Filter);
        mImpl->Check();
        mImpl->oidnExecuteFilterFn(mImpl->Filter);
        mImpl->Check();
    }
}
