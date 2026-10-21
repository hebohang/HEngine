#include "Runtime/Platform/PlatformPaths.h"

#import <AppKit/AppKit.h>
#include <mach-o/dyld.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace HEngine
{
    std::filesystem::path PlatformPaths::GetExecutableDirectory()
    {
        std::uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::vector<char> path(size);
        if (_NSGetExecutablePath(path.data(), &size) != 0)
            throw std::runtime_error("Cannot locate the executable");
        return std::filesystem::canonical(path.data()).parent_path();
    }

    std::filesystem::path PlatformPaths::GetApplicationContentDirectory()
    {
        const auto executableDirectory = GetExecutableDirectory();
        if (executableDirectory.filename() == "MacOS" &&
            executableDirectory.parent_path().filename() == "Contents")
            return executableDirectory.parent_path() / "Resources";
        return executableDirectory;
    }

    std::filesystem::path PlatformPaths::GetLocalAppDataDirectory()
    {
        @autoreleasepool
        {
            NSArray<NSURL*>* locations = [[NSFileManager defaultManager]
                URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask];
            if (locations.count == 0) throw std::runtime_error("Cannot locate Application Support");
            return std::filesystem::path(locations.firstObject.fileSystemRepresentation);
        }
    }

    std::filesystem::path PlatformPaths::GetPowerShellExecutable()
    {
        // Finder launches do not inherit a shell's Homebrew PATH.
        if (const char* environment = std::getenv("PATH"))
        {
            const std::string path(environment);
            std::size_t start = 0;
            while (start <= path.size())
            {
                const auto end = path.find(':', start);
                const auto component = path.substr(start, end == std::string::npos ? end : end - start);
                if (!component.empty())
                {
                    const auto candidate = std::filesystem::path(component) / "pwsh";
                    if (::access(candidate.c_str(), X_OK) == 0) return candidate;
                }
                if (end == std::string::npos) break;
                start = end + 1;
            }
        }
        for (const char* candidate : {"/opt/homebrew/bin/pwsh", "/usr/local/bin/pwsh",
            "/usr/local/microsoft/powershell/7/pwsh"})
            if (::access(candidate, X_OK) == 0) return candidate;
        throw std::runtime_error("PowerShell 7 (pwsh) is required for editor build tools");
    }

    std::filesystem::path PlatformPaths::ToFileIoPath(const std::filesystem::path& path)
    {
        return path.empty() ? path : std::filesystem::absolute(path).lexically_normal();
    }

    void PlatformPaths::ShowFatalDialog(std::string_view title, std::string_view message) noexcept
    {
        std::fprintf(stderr, "%.*s: %.*s\n", int(title.size()), title.data(), int(message.size()), message.data());
        try
        {
            // Cocoa UI is main-thread-only. An error on a worker is still
            // reported to stderr without deadlocking during application teardown.
            if (![NSThread isMainThread]) return;
            @autoreleasepool
            {
                NSAlert* alert = [[NSAlert alloc] init];
                alert.alertStyle = NSAlertStyleCritical;
                alert.messageText = [[NSString alloc] initWithBytes:title.data()
                    length:title.size() encoding:NSUTF8StringEncoding] ?: @"HEngine";
                alert.informativeText = [[NSString alloc] initWithBytes:message.data()
                    length:message.size() encoding:NSUTF8StringEncoding] ?: @"Could not start the application.";
                [alert addButtonWithTitle:@"OK"];
                [alert runModal];
            }
        }
        catch (...) {}
    }
}
