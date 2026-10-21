#include "Runtime/Platform/Scripting/CoreClrHost.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
    try
    {
        if (argc != 4 && argc != 5) throw std::invalid_argument("Usage: ManagedProbeHost hostfxr.dll runtimeconfig.json assembly.dll [runtime-root]");
        HEngine::CoreClrHost host;
        host.Open(argv[1], argv[2], argc == 5 ? std::filesystem::path(argv[4]) : std::filesystem::path{});
        using Run = int(*)(char*, int);
        const auto run = reinterpret_cast<Run>(host.LoadEntry(argv[3], "PcEntry, HEngine.ManagedProbe", "Run"));
        std::array<char, 4096> buffer{};
        for (int i = 0; i < 10; ++i)
        {
            const auto length = run(buffer.data(), static_cast<int>(buffer.size()));
            if (length <= 0 || length > static_cast<int>(buffer.size())) throw std::runtime_error("Managed probe failed");
            const std::string result(buffer.data(), length);
            if (result.find("CoreCLR=10.0.10; rooted=True") == std::string::npos ||
                result.find("controlled-coreclr-exception") == std::string::npos)
                throw std::runtime_error("Unexpected managed probe result: " + result);
            if (i == 0) std::cout << result << '\n';
        }
        std::cout << "PASS: 10 native-to-CoreCLR calls with GC and caught managed exceptions\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
