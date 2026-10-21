#include "Runtime/Platform/PlatformProcess.h"
#include "Runtime/Platform/PlatformFileSystem.h"
#include "Runtime/Profile/CpuExecutionTimes.h"

#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stop_token>
#include <thread>
#include <vector>

int main()
{
    using namespace HEngine;
    auto invoke = [](std::vector<std::string> arguments, std::stop_token token = {}, unsigned timeout = 3)
    {
        return PlatformProcess::RunCaptured("/bin/sh", arguments, token, timeout);
    };
    auto output = invoke({"-c", "printf '中文 text'; printf ' error' >&2; exit 7"});
    assert(output.ExitCode == 7 && !output.Cancelled && output.Output == "中文 text error");
    output = invoke({"-c", "printf '%s' \"$1\"", "sh", "quoted ' spaced \" 中文"});
    assert(output.ExitCode == 0 && output.Output == "quoted ' spaced \" 中文");
    output = PlatformProcess::RunCaptured("/does/not/exist", {}, {}, 3);
    assert(output.ExitCode == -1 && !output.Output.empty());
    std::stop_source cancelled;
    cancelled.request_stop();
    output = invoke({"-c", "exit 0"}, cancelled.get_token());
    assert(output.Cancelled && output.ExitCode == -1);
    auto started = std::chrono::steady_clock::now();
    output = invoke({"-c", "trap '' TERM; sleep 30 & wait"}, {}, 1);
    assert(output.Cancelled && std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
    output = invoke({"-c", "yes x | head -c 1500000; printf FINAL"});
    assert(output.Output.size() == 1024 * 1024 && output.Output.ends_with("FINAL"));
    output = invoke({"-c", "yes x"}, {}, 1);
    assert(output.Cancelled && output.Output.size() <= 1024 * 1024);

    const auto root = PlatformFileSystem::MakeTemporarySiblingPath(
        std::filesystem::temp_directory_path() / "he-mac-platform-contracts");
    std::filesystem::create_directories(root);
    const auto target = root / "target";
    auto temporary = PlatformFileSystem::MakeTemporarySiblingPath(target);
    std::ofstream(temporary) << "first";
    std::error_code error;
    assert(PlatformFileSystem::PublishFileAtomically(temporary, target, true, error));
    temporary = PlatformFileSystem::MakeTemporarySiblingPath(target);
    std::ofstream(temporary) << "second";
    assert(!PlatformFileSystem::PublishFileAtomically(temporary, target, false, error));
    std::string text;
    assert(PlatformFileSystem::ReadSharedTextTail(target, 3, text) && text == "rst");
    assert(PlatformFileSystem::PublishFileAtomically(temporary, target, true, error));
    assert(PlatformFileSystem::ReadSharedTextTail(target, 100, text) && text == "second");
    PlatformStableFileReadLease lease;
    assert(!PlatformFileSystem::AcquireStableFileReadLease({}, lease, error));
    assert(error == std::errc::operation_not_supported && !lease.IsValid());
    const auto memory = PlatformProcess::ReadCurrentMemory();
    assert(memory.bProcessAvailable && memory.WorkingSetBytes > 0);
    const auto cpu = QueryCpuExecutionTimes();
    assert(cpu.bValid && cpu.ThreadId != 0);
    std::filesystem::remove_all(root);
    std::cout << "macOS process/filesystem contracts PASS\n";
}
