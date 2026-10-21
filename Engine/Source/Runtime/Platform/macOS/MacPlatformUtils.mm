#include "Runtime/Utils/PlatformUtils.h"
#include "Runtime/Core/Log/Log.h"

#import <AppKit/AppKit.h>
#include <crt_externs.h>
#include <spawn.h>
#include <sys/wait.h>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace HEngine
{
    namespace
    {
        NSString* NativeString(std::string_view text)
        {
            return [[NSString alloc] initWithBytes:text.data() length:text.size()
                encoding:NSUTF8StringEncoding] ?: @"";
        }

        NSURL* NativeUrl(const std::filesystem::path& path)
        {
            return [NSURL fileURLWithPath:NativeString(std::filesystem::absolute(path).string())];
        }

        void ConfigurePanel(NSSavePanel* panel, const char* filter, const std::string& initialDirectory)
        {
            panel.canCreateDirectories = YES;
            if (!initialDirectory.empty()) panel.directoryURL = NativeUrl(initialDirectory);
            // Engine dialog filters use Win32's description\0pattern\0 pairs.
            // The initial pattern is sufficient for the editor's save/open types.
            if (filter && *filter)
            {
                const char* pattern = filter + std::strlen(filter) + 1;
                NSMutableArray<NSString*>* extensions = [NSMutableArray array];
                const std::string patterns(pattern);
                std::size_t start = 0;
                bool allFiles = false;
                while (start < patterns.size())
                {
                    const auto end = patterns.find(';', start);
                    const auto value = patterns.substr(start, end == std::string::npos ? end : end - start);
                    if (value == "*" || value == "*.*") { allFiles = true; break; }
                    if (value.starts_with("*.") && value.size() > 2)
                        [extensions addObject:NativeString(value.substr(2))];
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
                if (!allFiles && extensions.count)
                {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                    panel.allowedFileTypes = extensions;
#pragma clang diagnostic pop
                    panel.allowsOtherFileTypes = NO;
                }
            }
        }

        std::mutex& StateMutex()
        {
            static auto* mutex = new std::mutex;
            return *mutex;
        }

        void CopyState(const std::filesystem::path& source, const std::filesystem::path& destination)
        {
            if (!std::filesystem::is_regular_file(source))
                throw std::runtime_error("External application state is not a regular file: " + source.string());
            std::filesystem::create_directories(destination.parent_path());
            std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
        }

        bool LaunchExternal(const std::filesystem::path& applicationPath, const std::filesystem::path& filePath,
            std::vector<ExternalApplicationStateFile> stateFiles)
        {
            std::string executable = applicationPath.string();
            std::vector<std::string> arguments;
            if (applicationPath.extension() == ".app")
            {
                executable = "/usr/bin/open";
                arguments = {"-W", "-n", "-a", applicationPath.string(), filePath.string()};
            }
            else arguments = {filePath.string()};
            std::vector<char*> nativeArguments{executable.data()};
            for (auto& argument : arguments) nativeArguments.push_back(argument.data());
            nativeArguments.push_back(nullptr);
            pid_t pid;
            const int error = ::posix_spawnp(&pid, executable.c_str(), nullptr, nullptr,
                nativeArguments.data(), *_NSGetEnviron());
            if (error)
            {
                HE_CORE_ERROR("Could not launch external application '{}': {}", applicationPath.string(), std::strerror(error));
                return false;
            }
            std::thread([pid, stateFiles = std::move(stateFiles)]
            {
                int status = 0;
                pid_t result;
                do { result = ::waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
                if (result != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return;
                try
                {
                    std::scoped_lock lock(StateMutex());
                    for (const auto& state : stateFiles) CopyState(state.RuntimePath, state.CachePath);
                }
                catch (...) {}
            }).detach();
            return true;
        }
    }

    std::filesystem::path FileDialogs::OpenFile(const char* filter, std::string initialDir)
    {
        if (![NSThread isMainThread]) throw std::logic_error("File dialogs require the macOS main thread");
        @autoreleasepool
        {
            NSOpenPanel* panel = [NSOpenPanel openPanel];
            panel.canChooseFiles = YES;
            panel.canChooseDirectories = NO;
            panel.allowsMultipleSelection = NO;
            ConfigurePanel(panel, filter, initialDir);
            return [panel runModal] == NSModalResponseOK ?
                std::filesystem::path(panel.URL.fileSystemRepresentation) : std::filesystem::path{};
        }
    }

    std::filesystem::path FileDialogs::SaveFile(const char* filter, std::string initialDir)
    {
        if (![NSThread isMainThread]) throw std::logic_error("File dialogs require the macOS main thread");
        @autoreleasepool
        {
            NSSavePanel* panel = [NSSavePanel savePanel];
            ConfigurePanel(panel, filter, initialDir);
            return [panel runModal] == NSModalResponseOK ?
                std::filesystem::path(panel.URL.fileSystemRepresentation) : std::filesystem::path{};
        }
    }

    std::filesystem::path FileDialogs::SelectFolder(std::string initialDir)
    {
        if (![NSThread isMainThread]) throw std::logic_error("File dialogs require the macOS main thread");
        @autoreleasepool
        {
            NSOpenPanel* panel = [NSOpenPanel openPanel];
            panel.canChooseFiles = NO;
            panel.canChooseDirectories = YES;
            panel.allowsMultipleSelection = NO;
            ConfigurePanel(panel, nullptr, initialDir);
            return [panel runModal] == NSModalResponseOK ?
                std::filesystem::path(panel.URL.fileSystemRepresentation) : std::filesystem::path{};
        }
    }

    void FileDialogs::OpenInExplorer(std::string filePath)
    {
        @autoreleasepool
        {
            [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[NativeUrl(filePath)]];
        }
    }

    bool FileDialogs::OpenWebUrl(std::string_view url)
    {
        if (!url.starts_with("https://") && !url.starts_with("http://")) return false;
        @autoreleasepool
        {
            NSURL* nativeUrl = [NSURL URLWithString:NativeString(url)];
            return nativeUrl && [[NSWorkspace sharedWorkspace] openURL:nativeUrl];
        }
    }

    bool FileDialogs::OpenFileWithApplication(const std::filesystem::path& applicationPath,
        const std::filesystem::path& filePath)
    {
        return LaunchExternal(applicationPath, filePath, {});
    }

    bool FileDialogs::OpenFileWithApplicationAndStateCache(const std::filesystem::path& applicationPath,
        const std::filesystem::path& filePath, std::span<const ExternalApplicationStateFile> stateFiles)
    {
        std::vector<ExternalApplicationStateFile> ownedState;
        try
        {
            std::scoped_lock lock(StateMutex());
            for (const auto& state : stateFiles)
            {
                if (!std::filesystem::exists(state.CachePath)) CopyState(state.DefaultPath, state.CachePath);
                CopyState(state.CachePath, state.RuntimePath);
            }
            ownedState.assign(stateFiles.begin(), stateFiles.end());
        }
        catch (const std::exception& error)
        {
            HE_CORE_WARN("External application state cache unavailable: {}", error.what());
        }
        return LaunchExternal(applicationPath, filePath, std::move(ownedState));
    }
}
