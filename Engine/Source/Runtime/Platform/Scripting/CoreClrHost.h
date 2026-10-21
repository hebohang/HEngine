#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

namespace HEngine
{
    // Private host adapter. Platform details and runtime headers stay here.
    // CoreCLR is process-owned; disposing a host does not unload the runtime.
    class CoreClrHost final
    {
    public:
        CoreClrHost();
        ~CoreClrHost();
        CoreClrHost(const CoreClrHost&) = delete;
        CoreClrHost& operator=(const CoreClrHost&) = delete;
        void Open(const std::filesystem::path& hostFxr,
            const std::filesystem::path& runtimeConfig, const std::filesystem::path& runtimeRoot = {});
        void* LoadEntry(const std::filesystem::path& assembly,
            std::string_view qualifiedType, std::string_view method);
    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
