#include "CoreClrHost.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <dlfcn.h>
#include <mach-o/dyld.h>
#endif

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace HEngine
{
    namespace
    {
#if defined(_WIN32)
        using HostChar = wchar_t;
        using HostString = std::wstring;
#define HE_HOST_CALL __cdecl
#define HE_MANAGED_CALL __stdcall
        HostString HostText(std::string_view text)
        {
            if (text.empty()) return {};
            const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (size <= 0) throw std::runtime_error("Invalid UTF-8 in a managed entry name");
            HostString result(static_cast<std::size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                text.data(), static_cast<int>(text.size()), result.data(), size);
            return result;
        }
        std::filesystem::path ExecutablePath()
        {
            std::wstring path(32768, L'\0');
            const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (!length || length >= path.size()) throw std::runtime_error("Cannot resolve the native scripting host executable");
            path.resize(length);
            return path;
        }
#else
        using HostChar = char;
        using HostString = std::string;
#define HE_HOST_CALL
#define HE_MANAGED_CALL
        HostString HostText(std::string_view text) { return std::string(text); }
        std::filesystem::path ExecutablePath()
        {
            std::uint32_t size = 0;
            _NSGetExecutablePath(nullptr, &size);
            std::vector<char> path(size);
            if (_NSGetExecutablePath(path.data(), &size) != 0)
                throw std::runtime_error("Cannot resolve the native scripting host executable");
            return std::filesystem::canonical(path.data());
        }
#endif
    }

    // Public hostfxr / coreclr_delegates ABI: Windows uses UTF-16 wchar_t,
    // whereas Unix hosts use UTF-8 char and the platform C calling convention.
    struct CoreClrHost::Impl
    {
        using Initialize = int(HE_HOST_CALL*)(const HostChar*, const void*, void**);
        using GetDelegate = int(HE_HOST_CALL*)(void*, int, void**);
        using Close = int(HE_HOST_CALL*)(void*);
        using LoadAssembly = int(HE_MANAGED_CALL*)(const HostChar*, const HostChar*, const HostChar*,
            const HostChar*, void*, void**);
#if defined(_WIN32)
        HMODULE Module = nullptr;
        ~Impl() { if (Module) FreeLibrary(Module); }
        void OpenModule(const std::filesystem::path& path)
        {
            Module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!Module) throw std::runtime_error("Cannot load hostfxr: " + std::to_string(GetLastError()));
        }
        auto Symbol(const char* name) { return GetProcAddress(Module, name); }
#else
        void* Module = nullptr;
        ~Impl() { if (Module) dlclose(Module); }
        void OpenModule(const std::filesystem::path& path)
        {
            Module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!Module)
            {
                const char* error = dlerror();
                throw std::runtime_error("Cannot load hostfxr: " + std::string(error ? error : "unknown loader error"));
            }
        }
        void* Symbol(const char* name) { return dlsym(Module, name); }
#endif
        LoadAssembly Load = nullptr;
    };

    CoreClrHost::CoreClrHost() : mImpl(std::make_unique<Impl>()) {}
    CoreClrHost::~CoreClrHost() = default;

    void CoreClrHost::Open(const std::filesystem::path& hostFxr, const std::filesystem::path& runtimeConfig,
        const std::filesystem::path& runtimeRoot)
    {
        if (mImpl->Module) throw std::logic_error("CoreCLR host already opened");
        if (!hostFxr.is_absolute() || !runtimeConfig.is_absolute() || (!runtimeRoot.empty() && !runtimeRoot.is_absolute()))
            throw std::invalid_argument("CoreCLR host paths must be resolved absolute paths");
        auto nativeFxr = hostFxr; nativeFxr.make_preferred();
        auto nativeConfig = runtimeConfig; nativeConfig.make_preferred();
        auto nativeRoot = runtimeRoot; nativeRoot.make_preferred();
        auto candidate = std::make_unique<Impl>();
        candidate->OpenModule(nativeFxr);
        const auto initialize = reinterpret_cast<Impl::Initialize>(candidate->Symbol("hostfxr_initialize_for_runtime_config"));
        const auto getDelegate = reinterpret_cast<Impl::GetDelegate>(candidate->Symbol("hostfxr_get_runtime_delegate"));
        const auto close = reinterpret_cast<Impl::Close>(candidate->Symbol("hostfxr_close"));
        if (!initialize || !getDelegate || !close) throw std::runtime_error("Incompatible hostfxr hosting ABI");
        struct Parameters { std::size_t Size; const HostChar* HostPath; const HostChar* DotnetRoot; };
        const auto hostPath = ExecutablePath();
        const Parameters parameters{sizeof(Parameters), hostPath.c_str(), nativeRoot.c_str()};
        void* context = nullptr;
        const auto status = initialize(nativeConfig.c_str(), runtimeRoot.empty() ? nullptr : &parameters, &context);
        if (status < 0 || !context) throw std::runtime_error("CoreCLR initialization failed: " + std::to_string(status));
        void* entry = nullptr;
        // hdt_load_assembly_and_get_function_pointer = 5.
        const auto delegateStatus = getDelegate(context, 5, &entry);
        close(context);
        if (delegateStatus < 0 || !entry) throw std::runtime_error("CoreCLR delegate lookup failed: " + std::to_string(delegateStatus));
        candidate->Load = reinterpret_cast<Impl::LoadAssembly>(entry);
        mImpl = std::move(candidate);
    }

    void* CoreClrHost::LoadEntry(const std::filesystem::path& assembly,
        std::string_view qualifiedType, std::string_view method)
    {
        if (!mImpl->Load || !assembly.is_absolute()) throw std::logic_error("CoreCLR host is not ready or assembly path is unresolved");
        void* entry = nullptr;
        auto nativeAssembly = assembly; nativeAssembly.make_preferred();
        const auto nativeType = HostText(qualifiedType);
        const auto nativeMethod = HostText(method);
        const auto unmanagedOnly = reinterpret_cast<const HostChar*>(static_cast<std::intptr_t>(-1));
        const auto status = mImpl->Load(nativeAssembly.c_str(), nativeType.c_str(), nativeMethod.c_str(), unmanagedOnly, nullptr, &entry);
        if (status < 0 || !entry) throw std::runtime_error("Managed entry lookup failed: " + std::to_string(status));
        return entry;
    }
}
