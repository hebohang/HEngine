#include "Runtime/Platform/PlatformProcess.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <algorithm>
#include <fstream>

int main(int argc, char** argv)
{
    using namespace std::chrono_literals;
    if (argc > 1)
    {
        const std::string mode = argv[1];
        if (mode == "echo") { for (int i = 2; i < argc; ++i) std::cout << argv[i] << '\n'; return 0; }
        if (mode == "fail") { std::cerr << "compiler.cs(3,8): error TEST001: expected failure\n"; return 7; }
        if (mode == "flood") { for (int i = 0; i < 600; ++i) std::cout << std::string(4096, 'x'); std::cout << "END"; return 0; }
        if (mode == "hang") { std::this_thread::sleep_for(30s); return 0; }
        if (mode == "stream") { for (;;) std::cout << std::string(4096, 'x'); }
        if (mode == "heartbeat" && argc > 2)
        {
            std::ofstream heartbeat(argv[2], std::ios::binary);
            for (;;) { heartbeat << 'x' << std::flush; std::this_thread::sleep_for(10ms); }
        }
        if (mode == "branch" && argc > 2)
            return HEngine::PlatformProcess::RunCaptured(std::filesystem::absolute(argv[0]),
                std::vector<std::string>{"heartbeat", argv[2]}, {}).ExitCode;
        if (mode == "run" && argc > 2)
        {
            std::vector<std::string> arguments;
            for (int i = 3; i < argc; ++i) arguments.emplace_back(argv[i]);
            auto result = HEngine::PlatformProcess::RunCaptured(std::filesystem::u8path(argv[2]), arguments, {});
            std::cout << result.Output;
            return result.ExitCode;
        }
    }
    try
    {
        using HEngine::PlatformProcess;
        const auto exe = std::filesystem::absolute(argv[0]);
        auto require = [](bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); };
        const std::vector<std::string> arguments{"echo", "with spaces", "embedded\"quote", "trailing \\", ""};
        auto echo = PlatformProcess::RunCaptured(exe, arguments, {});
        std::erase(echo.Output, '\r');
        std::string expected;
        for (std::size_t i = 1; i < arguments.size(); ++i) expected += arguments[i] + "\n";
        require(echo.ExitCode == 0 && echo.Output == expected, "Argument quoting or final pipe drain failed");
        auto fail = PlatformProcess::RunCaptured(exe, std::vector<std::string>{"fail"}, {});
        require(fail.ExitCode == 7 && fail.Output.find("TEST001") != std::string::npos, "Compiler diagnostics/exit code lost");
        auto flood = PlatformProcess::RunCaptured(exe, std::vector<std::string>{"flood"}, {});
        require(flood.ExitCode == 0 && flood.Output.size() <= 1024 * 1024 && flood.Output.ends_with("END"), "Bounded output deadlock or tail lost");
        std::stop_source cancel;
        std::jthread trigger([&] { std::this_thread::sleep_for(100ms); cancel.request_stop(); });
        const auto start = std::chrono::steady_clock::now();
        auto stopped = PlatformProcess::RunCaptured(exe, std::vector<std::string>{"hang"}, cancel.get_token());
        require(stopped.Cancelled && stopped.ExitCode != 0 && std::chrono::steady_clock::now() - start < 5s, "Cancellation did not reap promptly");
        auto timed = PlatformProcess::RunCaptured(exe, std::vector<std::string>{"hang"}, {}, 1);
        require(timed.Cancelled && timed.ExitCode != 0, "Timeout did not terminate compiler");
        auto streaming = PlatformProcess::RunCaptured(exe, std::vector<std::string>{"stream"}, {}, 1);
        require(streaming.Cancelled && streaming.Output.size() <= 1024 * 1024, "Continuous output blocked timeout or exceeded output limit");
        const auto heartbeat = exe.parent_path()/"descendant-heartbeat.txt";
        std::filesystem::remove(heartbeat);
        auto branch = PlatformProcess::RunCaptured(exe, std::vector<std::string>{"branch", heartbeat.string()}, {}, 1);
        require(branch.Cancelled && std::filesystem::exists(heartbeat) && std::filesystem::file_size(heartbeat) > 0, "Descendant never actually started");
        const auto stoppedSize = std::filesystem::file_size(heartbeat);
        std::this_thread::sleep_for(200ms);
        require(std::filesystem::file_size(heartbeat) == stoppedSize, "Descendant survived process-tree termination");
        std::stop_source alreadyStopped;
        alreadyStopped.request_stop();
        auto beforeLaunch = PlatformProcess::RunCaptured(exe, std::vector<std::string>{"hang"}, alreadyStopped.get_token());
        require(beforeLaunch.Cancelled && beforeLaunch.ExitCode == -1, "Pre-cancelled task launched a process");
        auto missing = PlatformProcess::RunCaptured(exe.parent_path()/"missing-compiler.exe", {}, {});
        require(missing.ExitCode == -1 && !missing.Output.empty(), "Missing compiler diagnostic lost");
        std::cout << "unit-test PlatformProcess capture PASS: quoting, exit, stderr, output limit, cancellation, timeout, continuous output, descendants, pre-cancellation, missing executable\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
