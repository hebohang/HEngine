#include "Hepch.h"

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include "VulkanRenderBackend.h"
#include "VulkanCapabilities.h"
#include "VulkanNativeContext.h"

#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Platform/GLFW/CommonWindow.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>

#include <GLFW/glfw3.h>

#include <Windows.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
    namespace
    {
        constexpr uint32_t cFramesInFlight = 3;
        constexpr uint32_t cRequiredVulkanVersion = VK_API_VERSION_1_3;
        constexpr uint32_t cRequiredSampledImagesPerStage = 124;
        constexpr uint32_t cRequiredSamplersPerStage = 16;
        constexpr uint32_t cRequiredDescriptorSets = 5;

        [[nodiscard]] std::string VulkanVersionToString(uint32_t version)
        {
            return std::to_string(VK_API_VERSION_MAJOR(version)) + "." +
                std::to_string(VK_API_VERSION_MINOR(version)) + "." +
                std::to_string(VK_API_VERSION_PATCH(version));
        }

        [[nodiscard]] std::string VulkanResultToString(VkResult result)
        {
            const char* nvrhiText = nvrhi::vulkan::resultToString(result);
            return nvrhiText != nullptr
                ? std::string(nvrhiText) + " (" + std::to_string(static_cast<int32_t>(result)) + ")"
                : std::to_string(static_cast<int32_t>(result));
        }

        void CheckVulkan(std::string_view operation, VkResult result)
        {
            if (result != VK_SUCCESS)
            {
                throw std::runtime_error(
                    std::string(operation) + " failed with " + VulkanResultToString(result));
            }
        }

        template<typename Function>
        [[nodiscard]] Function RequireFunction(PFN_vkGetInstanceProcAddr getProcAddr, VkInstance instance, const char* name)
        {
            Function function = reinterpret_cast<Function>(getProcAddr(instance, name));
            if (function == nullptr)
                throw std::runtime_error(std::string("The Vulkan loader does not export ") + name);
            return function;
        }

        class VulkanLoader final
        {
        public:
            VulkanLoader()
            {
                mModule = LoadLibraryExW(
                    L"vulkan-1.dll",
                    nullptr,
                    LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (mModule == nullptr)
                {
                    throw std::runtime_error(
                        "Vulkan was requested, but the system Vulkan loader could not be loaded from "
                        "System32 (vulkan-1.dll, Win32 error " + std::to_string(GetLastError()) +
                        "). Install a current graphics driver with Vulkan support; the default DX12 backend remains available.");
                }

                GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                    GetProcAddress(mModule, "vkGetInstanceProcAddr"));
                if (GetInstanceProcAddr == nullptr)
                    throw std::runtime_error("System32 vulkan-1.dll does not export vkGetInstanceProcAddr");

                EnumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
                    GetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
                EnumerateInstanceExtensionProperties = RequireFunction<PFN_vkEnumerateInstanceExtensionProperties>(
                    GetInstanceProcAddr, VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
                EnumerateInstanceLayerProperties = RequireFunction<PFN_vkEnumerateInstanceLayerProperties>(
                    GetInstanceProcAddr, VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
                CreateInstance = RequireFunction<PFN_vkCreateInstance>(
                    GetInstanceProcAddr, VK_NULL_HANDLE, "vkCreateInstance");
            }

            ~VulkanLoader()
            {
                if (mModule != nullptr)
                    FreeLibrary(mModule);
            }

            VulkanLoader(const VulkanLoader&) = delete;
            VulkanLoader& operator=(const VulkanLoader&) = delete;

            void LoadInstance(VkInstance instance)
            {
                DestroyInstance = LoadInstanceFunction<PFN_vkDestroyInstance>(instance, "vkDestroyInstance");
                EnumeratePhysicalDevices = LoadInstanceFunction<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
                GetPhysicalDeviceProperties2 = LoadInstanceFunction<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2");
                GetPhysicalDeviceFeatures2 = LoadInstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>(instance, "vkGetPhysicalDeviceFeatures2");
                GetPhysicalDeviceMemoryProperties = LoadInstanceFunction<PFN_vkGetPhysicalDeviceMemoryProperties>(instance, "vkGetPhysicalDeviceMemoryProperties");
                GetPhysicalDeviceQueueFamilyProperties = LoadInstanceFunction<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
                EnumerateDeviceExtensionProperties = LoadInstanceFunction<PFN_vkEnumerateDeviceExtensionProperties>(instance, "vkEnumerateDeviceExtensionProperties");
                CreateDevice = LoadInstanceFunction<PFN_vkCreateDevice>(instance, "vkCreateDevice");
                DestroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(GetInstanceProcAddr(instance, "vkDestroySurfaceKHR"));
                GetPhysicalDeviceSurfaceSupport = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(GetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceSupportKHR"));
                GetPhysicalDeviceSurfaceCapabilities = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(GetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
                GetPhysicalDeviceSurfaceFormats = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(GetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
                GetPhysicalDeviceSurfacePresentModes = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(GetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfacePresentModesKHR"));
                CreateDebugUtilsMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(GetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
                DestroyDebugUtilsMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(GetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
                GetDeviceProcAddr = LoadInstanceFunction<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");
            }

            void LoadDevice(VkDevice device)
            {
                DestroyDevice = LoadDeviceFunction<PFN_vkDestroyDevice>(device, "vkDestroyDevice");
                GetDeviceQueue = LoadDeviceFunction<PFN_vkGetDeviceQueue>(device, "vkGetDeviceQueue");
                DeviceWaitIdle = LoadDeviceFunction<PFN_vkDeviceWaitIdle>(device, "vkDeviceWaitIdle");
                CreateSwapchain = LoadDeviceFunction<PFN_vkCreateSwapchainKHR>(device, "vkCreateSwapchainKHR");
                DestroySwapchain = LoadDeviceFunction<PFN_vkDestroySwapchainKHR>(device, "vkDestroySwapchainKHR");
                GetSwapchainImages = LoadDeviceFunction<PFN_vkGetSwapchainImagesKHR>(device, "vkGetSwapchainImagesKHR");
                AcquireNextImage = LoadDeviceFunction<PFN_vkAcquireNextImageKHR>(device, "vkAcquireNextImageKHR");
                QueuePresent = LoadDeviceFunction<PFN_vkQueuePresentKHR>(device, "vkQueuePresentKHR");
                CreateSemaphore = LoadDeviceFunction<PFN_vkCreateSemaphore>(device, "vkCreateSemaphore");
                DestroySemaphore = LoadDeviceFunction<PFN_vkDestroySemaphore>(device, "vkDestroySemaphore");
                CreateCommandPool = LoadDeviceFunction<PFN_vkCreateCommandPool>(device, "vkCreateCommandPool");
                DestroyCommandPool = LoadDeviceFunction<PFN_vkDestroyCommandPool>(device, "vkDestroyCommandPool");
            }

            PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
            PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
            PFN_vkEnumerateInstanceVersion EnumerateInstanceVersion = nullptr;
            PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
            PFN_vkEnumerateInstanceLayerProperties EnumerateInstanceLayerProperties = nullptr;
            PFN_vkCreateInstance CreateInstance = nullptr;
            PFN_vkDestroyInstance DestroyInstance = nullptr;
            PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
            PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;
            PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;
            PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
            PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
            PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
            PFN_vkCreateDevice CreateDevice = nullptr;
            PFN_vkDestroySurfaceKHR DestroySurface = nullptr;
            PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupport = nullptr;
            PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilities = nullptr;
            PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormats = nullptr;
            PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModes = nullptr;
            PFN_vkCreateDebugUtilsMessengerEXT CreateDebugUtilsMessenger = nullptr;
            PFN_vkDestroyDebugUtilsMessengerEXT DestroyDebugUtilsMessenger = nullptr;
            PFN_vkDestroyDevice DestroyDevice = nullptr;
            PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
            PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
            PFN_vkCreateSwapchainKHR CreateSwapchain = nullptr;
            PFN_vkDestroySwapchainKHR DestroySwapchain = nullptr;
            PFN_vkGetSwapchainImagesKHR GetSwapchainImages = nullptr;
            PFN_vkAcquireNextImageKHR AcquireNextImage = nullptr;
            PFN_vkQueuePresentKHR QueuePresent = nullptr;
            PFN_vkCreateSemaphore CreateSemaphore = nullptr;
            PFN_vkDestroySemaphore DestroySemaphore = nullptr;
            PFN_vkCreateCommandPool CreateCommandPool = nullptr;
            PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;

        private:
            template<typename Function>
            [[nodiscard]] Function LoadInstanceFunction(VkInstance instance, const char* name) const
            {
                return RequireFunction<Function>(GetInstanceProcAddr, instance, name);
            }

            template<typename Function>
            [[nodiscard]] Function LoadDeviceFunction(VkDevice device, const char* name) const
            {
                Function function = reinterpret_cast<Function>(GetDeviceProcAddr(device, name));
                if (function == nullptr)
                    throw std::runtime_error(std::string("The Vulkan device does not expose ") + name);
                return function;
            }

            HMODULE mModule = nullptr;
        };

        VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugCallback(
            VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT,
            const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
            void*)
        {
            const char* text = callbackData != nullptr && callbackData->pMessage != nullptr
                ? callbackData->pMessage
                : "<empty Vulkan validation message>";
            if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
            {
                HE_CORE_ERROR("Vulkan validation: {0}", text);
                Log::WriteFallbackError("Vulkan validation", text);
            }
            else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
            {
                HE_CORE_WARN("Vulkan validation: {0}", text);
            }
            else
            {
                HE_CORE_TRACE("Vulkan validation: {0}", text);
            }
            return VK_FALSE;
        }

        [[nodiscard]] bool ContainsName(
            const std::vector<VkExtensionProperties>& properties,
            const char* name)
        {
            return std::any_of(properties.begin(), properties.end(), [name](const auto& property)
            {
                return std::strcmp(property.extensionName, name) == 0;
            });
        }

        [[nodiscard]] bool ContainsName(
            const std::vector<VkLayerProperties>& properties,
            const char* name)
        {
            return std::any_of(properties.begin(), properties.end(), [name](const auto& property)
            {
                return std::strcmp(property.layerName, name) == 0;
            });
        }

        template<typename Property, typename Enumerate>
        [[nodiscard]] std::vector<Property> EnumerateProperties(Enumerate enumerate, std::string_view operation)
        {
            uint32_t count = 0;
            CheckVulkan(operation, enumerate(&count, nullptr));
            std::vector<Property> properties(count);
            if (count != 0)
                CheckVulkan(operation, enumerate(&count, properties.data()));
            properties.resize(count);
            return properties;
        }

        struct VulkanAdapter
        {
            VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
            VkPhysicalDeviceProperties2 Properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
            VkPhysicalDeviceIDProperties Id{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
            VkPhysicalDeviceDriverProperties Driver{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
            VkPhysicalDeviceFeatures2 Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            VkPhysicalDeviceVulkan12Features Features12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
            VkPhysicalDeviceVulkan13Features Features13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
            VkPhysicalDeviceMemoryProperties Memory{};
            uint32_t GraphicsQueueFamily = std::numeric_limits<uint32_t>::max();
            uint64_t DedicatedVideoMemory = 0;
            uint64_t Luid = 0;
            uint32_t PreferenceIndex = 0;
            bool bSupportsSwapchain = false;
        };

        [[nodiscard]] uint64_t ComputeAdapterScore(const VulkanAdapter& adapter)
        {
            uint64_t typeScore = 0;
            switch (adapter.Properties.properties.deviceType)
            {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   typeScore = 4; break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: typeScore = 3; break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    typeScore = 2; break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:            typeScore = 1; break;
            default:                                     typeScore = 0; break;
            }
            return (typeScore << 60u) + std::min<uint64_t>(
                adapter.DedicatedVideoMemory,
                (uint64_t{ 1 } << 60u) - 1u);
        }

        [[nodiscard]] std::string BytesToHex(const uint8_t* bytes, size_t count)
        {
            std::ostringstream stream;
            stream << std::hex << std::setfill('0');
            for (size_t index = 0; index < count; ++index)
                stream << std::setw(2) << static_cast<uint32_t>(bytes[index]);
            return stream.str();
        }

        [[nodiscard]] Json QueryWindowsOsInformation()
        {
            Json result{
                { "name", "Windows" },
                { "architecture", "x64" },
            };
            using RtlGetVersionFunction = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
            const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            const auto rtlGetVersion = ntdll == nullptr ? nullptr :
                reinterpret_cast<RtlGetVersionFunction>(
                    GetProcAddress(ntdll, "RtlGetVersion"));
            if (rtlGetVersion == nullptr)
            {
                result["versionQuery"] = "RtlGetVersion unavailable";
                return result;
            }
            RTL_OSVERSIONINFOW version{};
            version.dwOSVersionInfoSize = sizeof(version);
            const LONG status = rtlGetVersion(&version);
            if (status != 0)
            {
                result["versionQuery"] =
                    "RtlGetVersion failed with NTSTATUS " + std::to_string(status);
                return result;
            }
            result["name"] = version.dwBuildNumber >= 22000
                ? "Windows 11" : "Windows 10";
            result["major"] = version.dwMajorVersion;
            result["minor"] = version.dwMinorVersion;
            result["build"] = version.dwBuildNumber;
            DWORD revision = 0;
            DWORD revisionSize = sizeof(revision);
            if (RegGetValueW(
                    HKEY_LOCAL_MACHINE,
                    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                    L"UBR",
                    RRF_RT_REG_DWORD,
                    nullptr,
                    &revision,
                    &revisionSize) == ERROR_SUCCESS)
            {
                result["revision"] = revision;
            }
            return result;
        }

        [[nodiscard]] std::vector<const char*> CreateInstanceExtensions(
            VulkanLoader& loader,
            bool bNeedsSurface,
            bool bEnableValidation,
            bool& bDebugUtilsEnabled)
        {
            const std::vector<VkExtensionProperties> available =
                EnumerateProperties<VkExtensionProperties>(
                    [&loader](uint32_t* count, VkExtensionProperties* values)
                    {
                        return loader.EnumerateInstanceExtensionProperties(nullptr, count, values);
                    },
                    "vkEnumerateInstanceExtensionProperties");

            std::vector<const char*> result;
            if (bNeedsSurface)
            {
                uint32_t requiredCount = 0;
                const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&requiredCount);
                if (glfwExtensions == nullptr || requiredCount == 0)
                {
                    throw std::runtime_error(
                        "GLFW could not enumerate the Vulkan surface extensions. Verify the system Vulkan loader and graphics driver.");
                }
                for (uint32_t index = 0; index < requiredCount; ++index)
                {
                    if (!ContainsName(available, glfwExtensions[index]))
                    {
                        throw std::runtime_error(
                            std::string("The Vulkan loader is missing the GLFW-required instance extension ") +
                            glfwExtensions[index]);
                    }
                    result.push_back(glfwExtensions[index]);
                }
            }

            bDebugUtilsEnabled = bEnableValidation &&
                ContainsName(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            if (bDebugUtilsEnabled)
                result.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            return result;
        }

        struct VulkanDeviceState
        {
            Scope<VulkanLoader> Loader;
            VkInstance Instance = VK_NULL_HANDLE;
            VkDebugUtilsMessengerEXT DebugMessenger = VK_NULL_HANDLE;
            VkSurfaceKHR Surface = VK_NULL_HANDLE;
            VulkanAdapter Adapter;
            VkDevice Device = VK_NULL_HANDLE;
            VkQueue GraphicsQueue = VK_NULL_HANDLE;
            VkCommandPool InteropCommandPool = VK_NULL_HANDLE;
            std::vector<const char*> InstanceExtensions;
            std::vector<const char*> DeviceExtensions;
            nvrhi::vulkan::DeviceHandle NativeNvrhiDevice;
            nvrhi::DeviceHandle DeviceHandle;
            bool bValidationEnabled = false;
        };

        void DestroyDeviceState(VulkanDeviceState& state) noexcept
        {
            if (state.NativeNvrhiDevice)
                UnregisterVulkanNativeContext(state.NativeNvrhiDevice.Get());
            state.DeviceHandle = nullptr;
            state.NativeNvrhiDevice = nullptr;

            if (state.Device != VK_NULL_HANDLE && state.Loader)
            {
                if (state.InteropCommandPool != VK_NULL_HANDLE && state.Loader->DestroyCommandPool != nullptr)
                    state.Loader->DestroyCommandPool(state.Device, state.InteropCommandPool, nullptr);
                if (state.Loader->DestroyDevice != nullptr)
                    state.Loader->DestroyDevice(state.Device, nullptr);
            }
            state.InteropCommandPool = VK_NULL_HANDLE;
            state.Device = VK_NULL_HANDLE;

            if (state.Surface != VK_NULL_HANDLE && state.Loader && state.Loader->DestroySurface != nullptr)
                state.Loader->DestroySurface(state.Instance, state.Surface, nullptr);
            state.Surface = VK_NULL_HANDLE;
            if (state.DebugMessenger != VK_NULL_HANDLE && state.Loader && state.Loader->DestroyDebugUtilsMessenger != nullptr)
                state.Loader->DestroyDebugUtilsMessenger(state.Instance, state.DebugMessenger, nullptr);
            state.DebugMessenger = VK_NULL_HANDLE;
            if (state.Instance != VK_NULL_HANDLE && state.Loader && state.Loader->DestroyInstance != nullptr)
                state.Loader->DestroyInstance(state.Instance, nullptr);
            state.Instance = VK_NULL_HANDLE;
            state.Loader.reset();
        }

        void CreateVulkanInstance(
            VulkanDeviceState& state,
            bool bNeedsSurface,
            bool bEnableValidation)
        {
            state.Loader = CreateScope<VulkanLoader>();
            uint32_t loaderVersion = VK_API_VERSION_1_0;
            if (state.Loader->EnumerateInstanceVersion != nullptr)
                CheckVulkan("vkEnumerateInstanceVersion", state.Loader->EnumerateInstanceVersion(&loaderVersion));
            if (loaderVersion < cRequiredVulkanVersion)
            {
                throw std::runtime_error(
                    "The installed Vulkan loader exposes " + VulkanVersionToString(loaderVersion) +
                    ", but HEngine's NVRHI backend requires Vulkan 1.3. Update the graphics driver.");
            }

            bool bDebugUtilsEnabled = false;
            state.InstanceExtensions = CreateInstanceExtensions(
                *state.Loader,
                bNeedsSurface,
                bEnableValidation,
                bDebugUtilsEnabled);

            std::vector<const char*> layers;
            if (bEnableValidation)
            {
                const std::vector<VkLayerProperties> availableLayers =
                    EnumerateProperties<VkLayerProperties>(
                        [&state](uint32_t* count, VkLayerProperties* values)
                        {
                            return state.Loader->EnumerateInstanceLayerProperties(count, values);
                        },
                        "vkEnumerateInstanceLayerProperties");
                if (ContainsName(availableLayers, "VK_LAYER_KHRONOS_validation"))
                {
                    layers.push_back("VK_LAYER_KHRONOS_validation");
                    state.bValidationEnabled = true;
                }
                else
                {
                    HE_CORE_WARN(
                        "Vulkan validation was requested but VK_LAYER_KHRONOS_validation is not installed; continuing with NVRHI validation.");
                }
            }

            VkApplicationInfo applicationInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
            applicationInfo.pApplicationName = "HEngine";
            applicationInfo.applicationVersion = VK_MAKE_API_VERSION(0, 2, 0, 0);
            applicationInfo.pEngineName = "HEngine";
            applicationInfo.engineVersion = VK_MAKE_API_VERSION(0, 2, 0, 0);
            applicationInfo.apiVersion = cRequiredVulkanVersion;

            VkDebugUtilsMessengerCreateInfoEXT debugInfo{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            debugInfo.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debugInfo.messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debugInfo.pfnUserCallback = VulkanDebugCallback;

            VkInstanceCreateInfo createInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
            createInfo.pApplicationInfo = &applicationInfo;
            createInfo.enabledExtensionCount = static_cast<uint32_t>(state.InstanceExtensions.size());
            createInfo.ppEnabledExtensionNames = state.InstanceExtensions.data();
            createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
            createInfo.ppEnabledLayerNames = layers.data();
            createInfo.pNext = bDebugUtilsEnabled ? &debugInfo : nullptr;

            CheckVulkan("vkCreateInstance", state.Loader->CreateInstance(&createInfo, nullptr, &state.Instance));
            state.Loader->LoadInstance(state.Instance);
            VULKAN_HPP_DEFAULT_DISPATCHER.init(state.Instance, state.Loader->GetInstanceProcAddr);

            if (bDebugUtilsEnabled && state.Loader->CreateDebugUtilsMessenger != nullptr)
            {
                CheckVulkan(
                    "vkCreateDebugUtilsMessengerEXT",
                    state.Loader->CreateDebugUtilsMessenger(
                        state.Instance,
                        &debugInfo,
                        nullptr,
                        &state.DebugMessenger));
            }
        }

        [[nodiscard]] VulkanAdapter InspectAdapter(
            VulkanDeviceState& state,
            VkPhysicalDevice physicalDevice)
        {
            VulkanAdapter adapter;
            adapter.PhysicalDevice = physicalDevice;
            adapter.Driver.pNext = &adapter.Id;
            adapter.Properties.pNext = &adapter.Driver;
            state.Loader->GetPhysicalDeviceProperties2(physicalDevice, &adapter.Properties);

            adapter.Features12.pNext = &adapter.Features13;
            adapter.Features.pNext = &adapter.Features12;
            state.Loader->GetPhysicalDeviceFeatures2(physicalDevice, &adapter.Features);
            state.Loader->GetPhysicalDeviceMemoryProperties(physicalDevice, &adapter.Memory);

            for (uint32_t heapIndex = 0; heapIndex < adapter.Memory.memoryHeapCount; ++heapIndex)
            {
                if ((adapter.Memory.memoryHeaps[heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
                    adapter.DedicatedVideoMemory += adapter.Memory.memoryHeaps[heapIndex].size;
            }
            if (adapter.Id.deviceLUIDValid == VK_TRUE)
                std::memcpy(&adapter.Luid, adapter.Id.deviceLUID, sizeof(adapter.Luid));

            uint32_t queueCount = 0;
            state.Loader->GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, nullptr);
            std::vector<VkQueueFamilyProperties> queues(queueCount);
            state.Loader->GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, queues.data());
            for (uint32_t queueIndex = 0; queueIndex < queueCount; ++queueIndex)
            {
                constexpr VkQueueFlags requiredQueueFlags =
                    VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
                if ((queues[queueIndex].queueFlags & requiredQueueFlags) !=
                    requiredQueueFlags)
                    continue;
                VkBool32 bPresent = VK_TRUE;
                if (state.Surface != VK_NULL_HANDLE)
                {
                    CheckVulkan(
                        "vkGetPhysicalDeviceSurfaceSupportKHR",
                        state.Loader->GetPhysicalDeviceSurfaceSupport(
                            physicalDevice,
                            queueIndex,
                            state.Surface,
                            &bPresent));
                }
                if (bPresent == VK_TRUE)
                {
                    adapter.GraphicsQueueFamily = queueIndex;
                    break;
                }
            }

            const std::vector<VkExtensionProperties> extensions =
                EnumerateProperties<VkExtensionProperties>(
                    [&state, physicalDevice](uint32_t* count, VkExtensionProperties* values)
                    {
                        return state.Loader->EnumerateDeviceExtensionProperties(
                            physicalDevice, nullptr, count, values);
                    },
                    "vkEnumerateDeviceExtensionProperties");
            adapter.bSupportsSwapchain = ContainsName(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            return adapter;
        }

        [[nodiscard]] bool MeetsVulkanBaseline(const VulkanAdapter& adapter, std::string& reason)
        {
            if (adapter.Properties.properties.apiVersion < cRequiredVulkanVersion)
                reason = "physical device API version is below Vulkan 1.3";
            else if (adapter.GraphicsQueueFamily == std::numeric_limits<uint32_t>::max())
                reason = "no queue family supports graphics, compute, and presentation";
            else if (!adapter.bSupportsSwapchain)
                reason = "VK_KHR_swapchain is unavailable";
            else if (adapter.Features12.timelineSemaphore != VK_TRUE)
                reason = "timelineSemaphore is unavailable";
            else if (adapter.Features12.scalarBlockLayout != VK_TRUE)
                reason = "scalarBlockLayout is unavailable for DX-layout HLSL buffers";
            else if (adapter.Features12.shaderSampledImageArrayNonUniformIndexing != VK_TRUE)
                reason = "shaderSampledImageArrayNonUniformIndexing is unavailable for bounded material tables";
            else if (adapter.Properties.properties.limits.maxPerStageDescriptorSampledImages <
                    cRequiredSampledImagesPerStage ||
                adapter.Properties.properties.limits.maxDescriptorSetSampledImages <
                    cRequiredSampledImagesPerStage)
            {
                reason = "sampled-image descriptor limits are below the bounded material + scene requirement (124)";
            }
            else if (adapter.Properties.properties.limits.maxPerStageDescriptorSamplers <
                    cRequiredSamplersPerStage ||
                adapter.Properties.properties.limits.maxDescriptorSetSamplers <
                    cRequiredSamplersPerStage)
            {
                reason = "sampler descriptor limits are below the bounded material + scene requirement (16)";
            }
            else if (adapter.Properties.properties.limits.maxBoundDescriptorSets <
                cRequiredDescriptorSets)
            {
                reason = "maxBoundDescriptorSets is below HEngine's five-space binding convention";
            }
            else if (adapter.Features13.dynamicRendering != VK_TRUE)
                reason = "dynamicRendering is unavailable";
            else if (adapter.Features13.synchronization2 != VK_TRUE)
                reason = "synchronization2 is unavailable";
            else
                return true;
            return false;
        }

        void SelectVulkanAdapter(
            VulkanDeviceState& state,
            const RenderAdapterSelectionOptions& selection)
        {
            if (selection.bUseWarp)
                throw std::invalid_argument("--warp is DX12-only and cannot be combined with Vulkan");

            uint32_t deviceCount = 0;
            CheckVulkan("vkEnumeratePhysicalDevices", state.Loader->EnumeratePhysicalDevices(
                state.Instance, &deviceCount, nullptr));
            if (deviceCount == 0)
                throw std::runtime_error("The Vulkan loader reported no physical devices");
            std::vector<VkPhysicalDevice> devices(deviceCount);
            CheckVulkan("vkEnumeratePhysicalDevices", state.Loader->EnumeratePhysicalDevices(
                state.Instance, &deviceCount, devices.data()));

            std::vector<VulkanAdapter> candidates;
            std::vector<std::string> rejected;
            for (VkPhysicalDevice device : devices)
            {
                VulkanAdapter adapter = InspectAdapter(state, device);
                std::string reason;
                if (!MeetsVulkanBaseline(adapter, reason))
                {
                    rejected.push_back(std::string(adapter.Properties.properties.deviceName) + ": " + reason);
                    continue;
                }
                candidates.push_back(std::move(adapter));
            }
            std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right)
            {
                return ComputeAdapterScore(left) > ComputeAdapterScore(right);
            });
            for (uint32_t index = 0; index < candidates.size(); ++index)
                candidates[index].PreferenceIndex = index;

            if (candidates.empty())
            {
                std::ostringstream stream;
                stream << "No Vulkan adapter meets the HEngine Vulkan 1.3 baseline";
                for (const std::string& entry : rejected)
                    stream << "; " << entry;
                throw std::runtime_error(stream.str());
            }

            if (selection.AdapterLuid.has_value())
            {
                const auto found = std::find_if(candidates.begin(), candidates.end(), [&selection](const auto& adapter)
                {
                    return adapter.Id.deviceLUIDValid == VK_TRUE &&
                        adapter.Luid == selection.AdapterLuid.value();
                });
                if (found == candidates.end())
                    throw std::runtime_error("No Vulkan adapter matches the requested Windows adapter LUID");
                state.Adapter = *found;
            }
            else if (selection.GpuIndex.has_value())
            {
                if (selection.GpuIndex.value() >= candidates.size())
                {
                    throw std::out_of_range(
                        "Requested Vulkan GPU index " + std::to_string(selection.GpuIndex.value()) +
                        " but only " + std::to_string(candidates.size()) + " compatible adapter(s) are available");
                }
                state.Adapter = candidates[selection.GpuIndex.value()];
            }
            else
            {
                state.Adapter = candidates.front();
            }
        }

        void PopulateVulkanCapabilities(
            VulkanDeviceState& state,
            RenderCapabilities& capabilities)
        {
            const VulkanAdapter& adapter = state.Adapter;
            capabilities.Backend = RenderBackend::Vulkan;
            capabilities.AdapterName = adapter.Properties.properties.deviceName;
            capabilities.AdapterDriverVersion = adapter.Driver.driverName;
            if (adapter.Driver.driverInfo[0] != '\0')
                capabilities.AdapterDriverVersion += std::string(" ") + adapter.Driver.driverInfo;
            capabilities.AdapterUuid = BytesToHex(adapter.Id.deviceUUID, VK_UUID_SIZE);
            capabilities.AdapterIndex = adapter.PreferenceIndex;
            capabilities.AdapterVendorId = adapter.Properties.properties.vendorID;
            capabilities.AdapterDeviceId = adapter.Properties.properties.deviceID;
            capabilities.AdapterLuid = adapter.Luid;
            capabilities.DedicatedVideoMemory = adapter.DedicatedVideoMemory;
            capabilities.bSoftwareAdapter =
                adapter.Properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
            capabilities.VulkanApiVersion = adapter.Properties.properties.apiVersion;
            capabilities.VulkanDriverVersion = adapter.Properties.properties.driverVersion;
            capabilities.bVulkanTimelineSemaphore = adapter.Features12.timelineSemaphore == VK_TRUE;
            capabilities.bVulkanDynamicRendering = adapter.Features13.dynamicRendering == VK_TRUE;
            capabilities.bVulkanSynchronization2 = adapter.Features13.synchronization2 == VK_TRUE;
            capabilities.bVulkanScalarBlockLayout = adapter.Features12.scalarBlockLayout == VK_TRUE;
            capabilities.bVulkanSampledImageArrayNonUniformIndexing =
                adapter.Features12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
            capabilities.VulkanMaxPerStageSampledImages =
                adapter.Properties.properties.limits.maxPerStageDescriptorSampledImages;
            capabilities.VulkanMaxPerStageSamplers =
                adapter.Properties.properties.limits.maxPerStageDescriptorSamplers;
            capabilities.VulkanMaxBoundDescriptorSets =
                adapter.Properties.properties.limits.maxBoundDescriptorSets;
            capabilities.SupportedMsaaSampleMask = 1u << 1u;
            const VkSampleCountFlags samples =
                adapter.Properties.properties.limits.framebufferColorSampleCounts &
                adapter.Properties.properties.limits.framebufferDepthSampleCounts;
            if ((samples & VK_SAMPLE_COUNT_2_BIT) != 0) capabilities.SupportedMsaaSampleMask |= 1u << 2u;
            if ((samples & VK_SAMPLE_COUNT_4_BIT) != 0) capabilities.SupportedMsaaSampleMask |= 1u << 4u;
            if ((samples & VK_SAMPLE_COUNT_8_BIT) != 0) capabilities.SupportedMsaaSampleMask |= 1u << 8u;

            capabilities.BaselineRaster = {
                true,
                true,
                "Vulkan 1.3, timeline semaphores, dynamic rendering, synchronization2, scalar block layout, and bounded-table non-uniform indexing are enabled."
            };
            capabilities.ShaderModel6_6 = {
                false, false, "Shader Model is a DX12-native capability; Vulkan consumes SPIR-V artifacts."
            };
            capabilities.ResourceBindingTier3 = {
                false, false, "D3D12 Resource Binding Tier is not applicable to Vulkan."
            };
            capabilities.HeapDirectlyIndexed = {
                false, false, "Vulkan phase one deliberately uses bounded descriptor tables; descriptor-indexing heap-direct mode is not enabled."
            };
            capabilities.EnhancedBarriers = {
                false, false, "D3D12 Enhanced Barriers are not applicable; Vulkan synchronization2 is enabled."
            };
            capabilities.SamplerFeedback = {
                false, false, "NVRHI does not expose Vulkan sampler feedback for the baseline renderer."
            };

            nvrhi::IDevice* device = state.NativeNvrhiDevice.Get();
            capabilities.bNvrhiRayTracingAccelStruct = device->queryFeatureSupport(nvrhi::Feature::RayTracingAccelStruct);
            capabilities.bNvrhiRayTracingPipeline = device->queryFeatureSupport(nvrhi::Feature::RayTracingPipeline);
            capabilities.bNvrhiRayQuery = device->queryFeatureSupport(nvrhi::Feature::RayQuery);
            capabilities.bNvrhiMeshlets = device->queryFeatureSupport(nvrhi::Feature::Meshlets);
            capabilities.bNvrhiVariableRateShading = device->queryFeatureSupport(nvrhi::Feature::VariableRateShading);
            // Keep phase-one Vulkan on bounded descriptors even if a driver
            // happens to expose mutable descriptor types.
            capabilities.bNvrhiHeapDirectlyIndexed = false;
            capabilities.bNvrhiEnhancedBarriers = false;
            capabilities.bNvrhiShaderExecutionReordering = device->queryFeatureSupport(nvrhi::Feature::ShaderExecutionReordering);
            capabilities.bNvrhiRayTracingOpacityMicromap = device->queryFeatureSupport(nvrhi::Feature::RayTracingOpacityMicromap);
            capabilities.bGraphicsQueue = true;
            // Vulkan graphics queues are required to support compute. This
            // reports renderer compute capability, not the presence of a
            // separate asynchronous-compute queue.
            capabilities.bComputeQueue = true;
            capabilities.bCopyQueue = device->queryFeatureSupport(nvrhi::Feature::CopyQueue);
            capabilities.bValidationLayer = state.bValidationEnabled;
            capabilities.FramesInFlight = cFramesInFlight;

            const auto setFeature = [](RenderFeatureStatus& status, bool supported, const char* enabled, const char* disabled)
            {
                status.bSupported = supported;
                status.bEnabled = supported;
                status.Reason = supported ? enabled : disabled;
            };
            setFeature(capabilities.RayTracingAccelStruct, capabilities.bNvrhiRayTracingAccelStruct,
                "NVRHI reports Vulkan acceleration-structure support.", "Vulkan acceleration structures are not enabled; raster rendering remains available.");
            setFeature(capabilities.RayTracingPipeline, capabilities.bNvrhiRayTracingPipeline,
                "NVRHI reports Vulkan ray-tracing pipeline support.", "Vulkan ray-tracing pipelines are not enabled; raster rendering remains available.");
            setFeature(capabilities.RayQuery, capabilities.bNvrhiRayQuery,
                "NVRHI reports Vulkan RayQuery support.", "Vulkan RayQuery is not enabled.");
            setFeature(capabilities.MeshShader, capabilities.bNvrhiMeshlets,
                "NVRHI reports Vulkan mesh shader support.", "Vulkan mesh shaders are not enabled; vertex/primitive processing is selected.");
            setFeature(capabilities.VariableRateShading, capabilities.bNvrhiVariableRateShading,
                "NVRHI reports Vulkan fragment shading rate support.", "Vulkan variable-rate shading is not enabled; full-rate shading is selected.");
            setFeature(capabilities.ShaderExecutionReordering, capabilities.bNvrhiShaderExecutionReordering,
                "NVRHI reports Vulkan shader execution reordering support.", "Vulkan shader execution reordering is not enabled.");
            setFeature(capabilities.RayTracingOpacityMicromap, capabilities.bNvrhiRayTracingOpacityMicromap,
                "NVRHI reports Vulkan opacity micromap support.", "Vulkan opacity micromaps are not enabled.");
            ResolveRendererCapabilityPolicy(capabilities);
        }

        void CreateVulkanDevice(
            VulkanDeviceState& state,
            const RenderAdapterSelectionOptions& selection,
            bool bEnableValidation,
            nvrhi::IMessageCallback* messageCallback,
            RenderCapabilities& capabilities)
        {
            SelectVulkanAdapter(state, selection);
            const float priority = 1.f;
            VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
            queueInfo.queueFamilyIndex = state.Adapter.GraphicsQueueFamily;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &priority;

            state.DeviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
            VkPhysicalDeviceFeatures2 enabledFeatures = state.Adapter.Features;
            VkPhysicalDeviceVulkan12Features enabled12 = state.Adapter.Features12;
            VkPhysicalDeviceVulkan13Features enabled13 = state.Adapter.Features13;
            enabledFeatures.pNext = &enabled12;
            enabled12.pNext = &enabled13;
            enabled13.pNext = nullptr;

            VkDeviceCreateInfo createInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
            createInfo.pNext = &enabledFeatures;
            createInfo.queueCreateInfoCount = 1;
            createInfo.pQueueCreateInfos = &queueInfo;
            createInfo.enabledExtensionCount = static_cast<uint32_t>(state.DeviceExtensions.size());
            createInfo.ppEnabledExtensionNames = state.DeviceExtensions.data();
            CheckVulkan("vkCreateDevice", state.Loader->CreateDevice(
                state.Adapter.PhysicalDevice, &createInfo, nullptr, &state.Device));
            state.Loader->LoadDevice(state.Device);
            state.Loader->GetDeviceQueue(
                state.Device,
                state.Adapter.GraphicsQueueFamily,
                0,
                &state.GraphicsQueue);
            VULKAN_HPP_DEFAULT_DISPATCHER.init(
                state.Instance,
                state.Loader->GetInstanceProcAddr,
                state.Device);

            VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            poolInfo.queueFamilyIndex = state.Adapter.GraphicsQueueFamily;
            CheckVulkan("vkCreateCommandPool(Vulkan platform interop)", state.Loader->CreateCommandPool(
                state.Device, &poolInfo, nullptr, &state.InteropCommandPool));

            nvrhi::vulkan::DeviceDesc deviceDesc;
            deviceDesc.errorCB = messageCallback;
            deviceDesc.instance = state.Instance;
            deviceDesc.physicalDevice = state.Adapter.PhysicalDevice;
            deviceDesc.device = state.Device;
            deviceDesc.graphicsQueue = state.GraphicsQueue;
            deviceDesc.graphicsQueueIndex = static_cast<int>(state.Adapter.GraphicsQueueFamily);
            deviceDesc.instanceExtensions = state.InstanceExtensions.data();
            deviceDesc.numInstanceExtensions = state.InstanceExtensions.size();
            deviceDesc.deviceExtensions = state.DeviceExtensions.data();
            deviceDesc.numDeviceExtensions = state.DeviceExtensions.size();
            deviceDesc.bufferDeviceAddressSupported = enabled12.bufferDeviceAddress == VK_TRUE;
            deviceDesc.maxTimerQueries = 1024;
            state.NativeNvrhiDevice = nvrhi::vulkan::createDevice(deviceDesc);
            if (!state.NativeNvrhiDevice)
                throw std::runtime_error("nvrhi::vulkan::createDevice failed");
            state.DeviceHandle = bEnableValidation
                ? nvrhi::validation::createValidationLayer(state.NativeNvrhiDevice)
                : state.NativeNvrhiDevice;
            if (!state.DeviceHandle)
                throw std::runtime_error("NVRHI failed to create the Vulkan validation wrapper");

            RegisterVulkanNativeContext({
                state.Instance,
                state.Adapter.PhysicalDevice,
                state.Device,
                state.GraphicsQueue,
                state.Adapter.GraphicsQueueFamily,
                state.InteropCommandPool,
                state.NativeNvrhiDevice.Get(),
                state.DeviceHandle.Get(),
            });
            PopulateVulkanCapabilities(state, capabilities);
        }

        [[nodiscard]] nvrhi::Format ConvertSwapChainFormat(VkFormat format)
        {
            switch (format)
            {
            case VK_FORMAT_R8G8B8A8_UNORM: return nvrhi::Format::RGBA8_UNORM;
            case VK_FORMAT_B8G8R8A8_UNORM: return nvrhi::Format::BGRA8_UNORM;
            default:
                throw std::runtime_error("The selected Vulkan surface format is not supported by HEngine");
            }
        }

        class VulkanSwapChain final : public SwapChain
        {
        public:
            VulkanSwapChain(
                VulkanDeviceState& state,
                GLFWwindow* window,
                bool bInitialVSync)
                : mState(state)
                , mWindow(window)
                , mRequestedVSync(bInitialVSync)
            {
                for (uint32_t index = 0; index < cFramesInFlight; ++index)
                {
                    VkSemaphoreCreateInfo info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
                    CheckVulkan("vkCreateSemaphore(image available)", mState.Loader->CreateSemaphore(
                        mState.Device, &info, nullptr, &mImageAvailable[index]));
                    CheckVulkan("vkCreateSemaphore(render finished)", mState.Loader->CreateSemaphore(
                        mState.Device, &info, nullptr, &mRenderFinished[index]));
                }
                Recreate();
            }

            ~VulkanSwapChain() override
            {
                ReleaseImages();
                if (mSwapchain != VK_NULL_HANDLE)
                    mState.Loader->DestroySwapchain(mState.Device, mSwapchain, nullptr);
                for (uint32_t index = 0; index < cFramesInFlight; ++index)
                {
                    if (mImageAvailable[index] != VK_NULL_HANDLE)
                        mState.Loader->DestroySemaphore(mState.Device, mImageAvailable[index], nullptr);
                    if (mRenderFinished[index] != VK_NULL_HANDLE)
                        mState.Loader->DestroySemaphore(mState.Device, mRenderFinished[index], nullptr);
                }
            }

            void Resize(uint32_t width, uint32_t height) override
            {
                if (width == 0 || height == 0)
                    return;
                mRequestedWidth = width;
                mRequestedHeight = height;
                if (width != mWidth || height != mHeight)
                    Recreate();
            }

            uint32_t AcquireNextImage() override
            {
                return AcquireNextImageForFrame(0);
            }

            [[nodiscard]] uint32_t AcquireNextImageForFrame(uint32_t frameSlot)
            {
                if (frameSlot >= cFramesInFlight)
                    throw std::out_of_range("Vulkan frame slot is outside the semaphore ring");
                if (mRecreatePending)
                {
                    CheckVulkan("vkDeviceWaitIdle before swap-chain recreation", mState.Loader->DeviceWaitIdle(mState.Device));
                    Recreate();
                }

                VkResult result = mState.Loader->AcquireNextImage(
                    mState.Device,
                    mSwapchain,
                    std::numeric_limits<uint64_t>::max(),
                    mImageAvailable[frameSlot],
                    VK_NULL_HANDLE,
                    &mCurrentImage);
                if (result == VK_ERROR_OUT_OF_DATE_KHR)
                {
                    CheckVulkan("vkDeviceWaitIdle after out-of-date acquire", mState.Loader->DeviceWaitIdle(mState.Device));
                    Recreate();
                    result = mState.Loader->AcquireNextImage(
                        mState.Device,
                        mSwapchain,
                        std::numeric_limits<uint64_t>::max(),
                        mImageAvailable[frameSlot],
                        VK_NULL_HANDLE,
                        &mCurrentImage);
                }
                if (result == VK_SUBOPTIMAL_KHR)
                    mRecreatePending = true;
                else
                    CheckVulkan("vkAcquireNextImageKHR", result);
                if (mCurrentImage >= mBackBuffers.size())
                    throw std::runtime_error("Vulkan returned an invalid swap-chain image index");
                mActiveFrameSlot = frameSlot;
                return mCurrentImage;
            }

            void PrepareSubmission(nvrhi::vulkan::IDevice* device, uint32_t frameSlot)
            {
                if (frameSlot != mActiveFrameSlot)
                    throw std::logic_error("Vulkan submission frame slot does not match the acquired image semaphore");
                device->queueWaitForSemaphore(
                    nvrhi::CommandQueue::Graphics,
                    mImageAvailable[frameSlot],
                    0);
                device->queueSignalSemaphore(
                    nvrhi::CommandQueue::Graphics,
                    mRenderFinished[frameSlot],
                    0);
            }

            void Present(bool bVSync) override
            {
                VkSemaphore waitSemaphore = mRenderFinished[mActiveFrameSlot];
                VkPresentInfoKHR info{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
                info.waitSemaphoreCount = 1;
                info.pWaitSemaphores = &waitSemaphore;
                info.swapchainCount = 1;
                info.pSwapchains = &mSwapchain;
                info.pImageIndices = &mCurrentImage;
                const VkResult result = mState.Loader->QueuePresent(mState.GraphicsQueue, &info);
                if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
                    mRecreatePending = true;
                else
                    CheckVulkan("vkQueuePresentKHR", result);
                if (result != VK_ERROR_OUT_OF_DATE_KHR)
                    mImageInitialized.at(mCurrentImage) = true;
                if (bVSync != mRequestedVSync)
                {
                    mRequestedVSync = bVSync;
                    mRecreatePending = true;
                }
            }

            [[nodiscard]] uint32_t GetWidth() const override { return mWidth; }
            [[nodiscard]] uint32_t GetHeight() const override { return mHeight; }
            [[nodiscard]] uint32_t GetBufferCount() const override { return static_cast<uint32_t>(mBackBuffers.size()); }
            [[nodiscard]] nvrhi::Format GetFormat() const override { return mFormat; }
            [[nodiscard]] PresentMode GetPresentMode() const override { return mPresentMode; }
            [[nodiscard]] uint32_t GetCurrentImageIndex() const override { return mCurrentImage; }
            [[nodiscard]] nvrhi::ResourceStates GetCurrentBackBufferInitialState() const override
            {
                return mImageInitialized.at(mCurrentImage)
                    ? nvrhi::ResourceStates::Present
                    : nvrhi::ResourceStates::Common;
            }
            [[nodiscard]] nvrhi::ITexture* GetCurrentBackBuffer() const override { return mBackBuffers.at(mCurrentImage); }
            [[nodiscard]] nvrhi::IFramebuffer* GetCurrentFramebuffer() const override { return mFramebuffers.at(mCurrentImage); }

        private:
            void Recreate()
            {
                VkSurfaceCapabilitiesKHR capabilities{};
                CheckVulkan("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", mState.Loader->GetPhysicalDeviceSurfaceCapabilities(
                    mState.Adapter.PhysicalDevice, mState.Surface, &capabilities));

                const auto formats = EnumerateProperties<VkSurfaceFormatKHR>(
                    [this](uint32_t* count, VkSurfaceFormatKHR* values)
                    {
                        return mState.Loader->GetPhysicalDeviceSurfaceFormats(
                            mState.Adapter.PhysicalDevice, mState.Surface, count, values);
                    },
                    "vkGetPhysicalDeviceSurfaceFormatsKHR");
                if (formats.empty())
                    throw std::runtime_error("The selected Vulkan surface reports no formats");
                VkSurfaceFormatKHR surfaceFormat = formats.front();
                const std::array<VkFormat, 2> preferredFormats = {
                    VK_FORMAT_R8G8B8A8_UNORM,
                    VK_FORMAT_B8G8R8A8_UNORM,
                };
                for (VkFormat preferred : preferredFormats)
                {
                    const auto found = std::find_if(formats.begin(), formats.end(), [preferred](const auto& candidate)
                    {
                        return candidate.format == preferred &&
                            candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
                    });
                    if (found != formats.end())
                    {
                        surfaceFormat = *found;
                        break;
                    }
                }
                mFormat = ConvertSwapChainFormat(surfaceFormat.format);

                const auto presentModes = EnumerateProperties<VkPresentModeKHR>(
                    [this](uint32_t* count, VkPresentModeKHR* values)
                    {
                        return mState.Loader->GetPhysicalDeviceSurfacePresentModes(
                            mState.Adapter.PhysicalDevice, mState.Surface, count, values);
                    },
                    "vkGetPhysicalDeviceSurfacePresentModesKHR");
                VkPresentModeKHR nativePresentMode = VK_PRESENT_MODE_FIFO_KHR;
                mPresentMode = PresentMode::VSync;
                if (!mRequestedVSync)
                {
                    if (std::find(presentModes.begin(), presentModes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != presentModes.end())
                    {
                        nativePresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
                        mPresentMode = PresentMode::Mailbox;
                    }
                    else if (std::find(presentModes.begin(), presentModes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != presentModes.end())
                    {
                        nativePresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
                        mPresentMode = PresentMode::Immediate;
                    }
                    else
                    {
                        HE_CORE_WARN("Vulkan VSync-off requested, but neither MAILBOX nor IMMEDIATE is supported; FIFO selected");
                    }
                }

                VkExtent2D extent = capabilities.currentExtent;
                if (extent.width == std::numeric_limits<uint32_t>::max())
                {
                    int framebufferWidth = 0;
                    int framebufferHeight = 0;
                    glfwGetFramebufferSize(mWindow, &framebufferWidth, &framebufferHeight);
                    const uint32_t requestedWidth = mRequestedWidth != 0
                        ? mRequestedWidth
                        : static_cast<uint32_t>(std::max(framebufferWidth, 1));
                    const uint32_t requestedHeight = mRequestedHeight != 0
                        ? mRequestedHeight
                        : static_cast<uint32_t>(std::max(framebufferHeight, 1));
                    extent.width = std::clamp(
                        requestedWidth,
                        capabilities.minImageExtent.width,
                        capabilities.maxImageExtent.width);
                    extent.height = std::clamp(
                        requestedHeight,
                        capabilities.minImageExtent.height,
                        capabilities.maxImageExtent.height);
                }
                if (extent.width == 0 || extent.height == 0)
                    throw std::runtime_error("Cannot create a Vulkan swap chain for a zero-sized surface");

                uint32_t imageCount = std::max(capabilities.minImageCount + 1u, cFramesInFlight);
                if (capabilities.maxImageCount != 0)
                    imageCount = std::min(imageCount, capabilities.maxImageCount);

                VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
                if ((capabilities.supportedCompositeAlpha & compositeAlpha) == 0)
                {
                    const std::array<VkCompositeAlphaFlagBitsKHR, 3> fallbacks = {
                        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                    };
                    for (VkCompositeAlphaFlagBitsKHR candidate : fallbacks)
                    {
                        if ((capabilities.supportedCompositeAlpha & candidate) != 0)
                        {
                            compositeAlpha = candidate;
                            break;
                        }
                    }
                }

                VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0)
                    usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0)
                    usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

                VkSwapchainCreateInfoKHR createInfo{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
                createInfo.surface = mState.Surface;
                createInfo.minImageCount = imageCount;
                createInfo.imageFormat = surfaceFormat.format;
                createInfo.imageColorSpace = surfaceFormat.colorSpace;
                createInfo.imageExtent = extent;
                createInfo.imageArrayLayers = 1;
                createInfo.imageUsage = usage;
                createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
                createInfo.preTransform = capabilities.currentTransform;
                createInfo.compositeAlpha = compositeAlpha;
                createInfo.presentMode = nativePresentMode;
                createInfo.clipped = VK_TRUE;
                createInfo.oldSwapchain = mSwapchain;

                VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
                CheckVulkan("vkCreateSwapchainKHR", mState.Loader->CreateSwapchain(
                    mState.Device, &createInfo, nullptr, &newSwapchain));
                ReleaseImages();
                if (mSwapchain != VK_NULL_HANDLE)
                    mState.Loader->DestroySwapchain(mState.Device, mSwapchain, nullptr);
                mSwapchain = newSwapchain;
                mWidth = extent.width;
                mHeight = extent.height;

                uint32_t actualImageCount = 0;
                CheckVulkan("vkGetSwapchainImagesKHR", mState.Loader->GetSwapchainImages(
                    mState.Device, mSwapchain, &actualImageCount, nullptr));
                mNativeImages.resize(actualImageCount);
                CheckVulkan("vkGetSwapchainImagesKHR", mState.Loader->GetSwapchainImages(
                    mState.Device, mSwapchain, &actualImageCount, mNativeImages.data()));
                mNativeImages.resize(actualImageCount);
                mBackBuffers.resize(actualImageCount);
                mFramebuffers.resize(actualImageCount);
                mImageInitialized.assign(actualImageCount, false);
                for (uint32_t index = 0; index < actualImageCount; ++index)
                {
                    nvrhi::TextureDesc textureDesc;
                    textureDesc.width = mWidth;
                    textureDesc.height = mHeight;
                    textureDesc.format = mFormat;
                    textureDesc.dimension = nvrhi::TextureDimension::Texture2D;
                    textureDesc.isShaderResource = false;
                    textureDesc.isRenderTarget = true;
                    textureDesc.debugName = "HEngine Vulkan Backbuffer " + std::to_string(index);
                    textureDesc.enableAutomaticStateTracking(nvrhi::ResourceStates::Present);
                    mBackBuffers[index] = mState.DeviceHandle->createHandleForNativeTexture(
                        nvrhi::ObjectTypes::VK_Image,
                        mNativeImages[index],
                        textureDesc);
                    if (!mBackBuffers[index])
                        throw std::runtime_error("NVRHI failed to import a Vulkan swap-chain image");
                    mFramebuffers[index] = mState.DeviceHandle->createFramebuffer(
                        nvrhi::FramebufferDesc().addColorAttachment(mBackBuffers[index]));
                    if (!mFramebuffers[index])
                        throw std::runtime_error("NVRHI failed to create a Vulkan swap-chain framebuffer");
                }
                mCurrentImage = 0;
                mRecreatePending = false;
                HE_CORE_INFO(
                    "Vulkan swap chain: {0}x{1}, images={2}, format={3}, present={4}",
                    mWidth,
                    mHeight,
                    actualImageCount,
                    static_cast<int32_t>(surfaceFormat.format),
                    mPresentMode == PresentMode::VSync ? "FIFO" :
                        mPresentMode == PresentMode::Mailbox ? "MAILBOX" : "IMMEDIATE");
            }

            void ReleaseImages()
            {
                mFramebuffers.clear();
                mBackBuffers.clear();
                mNativeImages.clear();
            }

            VulkanDeviceState& mState;
            GLFWwindow* mWindow = nullptr;
            VkSwapchainKHR mSwapchain = VK_NULL_HANDLE;
            std::array<VkSemaphore, cFramesInFlight> mImageAvailable{};
            std::array<VkSemaphore, cFramesInFlight> mRenderFinished{};
            std::vector<VkImage> mNativeImages;
            std::vector<nvrhi::TextureHandle> mBackBuffers;
            std::vector<nvrhi::FramebufferHandle> mFramebuffers;
            std::vector<bool> mImageInitialized;
            uint32_t mWidth = 0;
            uint32_t mHeight = 0;
            uint32_t mRequestedWidth = 0;
            uint32_t mRequestedHeight = 0;
            uint32_t mCurrentImage = 0;
            uint32_t mActiveFrameSlot = 0;
            nvrhi::Format mFormat = nvrhi::Format::UNKNOWN;
            PresentMode mPresentMode = PresentMode::VSync;
            bool mRequestedVSync = true;
            bool mRecreatePending = false;
        };

        class ProbeMessageCallback final : public nvrhi::IMessageCallback
        {
            void message(nvrhi::MessageSeverity severity, const char* messageText) override
            {
                const char* text = messageText != nullptr ? messageText : "<empty NVRHI Vulkan probe message>";
                if (severity == nvrhi::MessageSeverity::Info)
                    HE_CORE_INFO("NVRHI Vulkan probe: {0}", text);
                else if (severity == nvrhi::MessageSeverity::Warning)
                    HE_CORE_WARN("NVRHI Vulkan probe: {0}", text);
                else
                    HE_CORE_ERROR("NVRHI Vulkan probe: {0}", text);
            }
        };

        void LogVulkanCapabilities(const RenderCapabilities& capabilities)
        {
            HE_CORE_INFO(
                "Vulkan selected adapter [{0}]: {1}; VendorId=0x{2:04X}, DeviceId=0x{3:04X}, LUID={4}, UUID={5}, dedicated VRAM={6} MiB, driver={7}",
                capabilities.AdapterIndex,
                capabilities.AdapterName,
                capabilities.AdapterVendorId,
                capabilities.AdapterDeviceId,
                capabilities.AdapterLuid,
                capabilities.AdapterUuid,
                capabilities.DedicatedVideoMemory / (1024ull * 1024ull),
                capabilities.AdapterDriverVersion);
            HE_CORE_INFO(
                "Vulkan native capabilities: API={0}, timeline={1}, dynamicRendering={2}, synchronization2={3}, scalarBlockLayout={4}, bounded non-uniform indexing={5}",
                VulkanVersionToString(capabilities.VulkanApiVersion),
                capabilities.bVulkanTimelineSemaphore,
                capabilities.bVulkanDynamicRendering,
                capabilities.bVulkanSynchronization2,
                capabilities.bVulkanScalarBlockLayout,
                capabilities.bVulkanSampledImageArrayNonUniformIndexing);
            HE_CORE_INFO(
                "Vulkan renderer profile: {0}; bounded descriptors selected; heap-direct={1}; DXR AS={2}, pipeline={3}, RayQuery={4}, Mesh={5}, VRS={6}",
                ToString(capabilities.Profile),
                capabilities.HeapDirectlyIndexed.Reason,
                capabilities.bNvrhiRayTracingAccelStruct,
                capabilities.bNvrhiRayTracingPipeline,
                capabilities.bNvrhiRayQuery,
                capabilities.bNvrhiMeshlets,
                capabilities.bNvrhiVariableRateShading);
        }

        [[nodiscard]] Json VulkanCapabilitiesToJson(const RenderCapabilities& capabilities)
        {
            const auto feature = [](const RenderFeatureStatus& status)
            {
                return Json{
                    { "Supported", status.bSupported },
                    { "Enabled", status.bEnabled },
                    { "Reason", status.Reason },
                };
            };
            return Json{
                { "OS", QueryWindowsOsInformation() },
                { "Backend", "Vulkan" },
                { "Adapter", {
                    { "Name", capabilities.AdapterName },
                    { "VendorId", capabilities.AdapterVendorId },
                    { "DeviceId", capabilities.AdapterDeviceId },
                    { "Luid", capabilities.AdapterLuid },
                    { "Uuid", capabilities.AdapterUuid },
                    { "DedicatedVideoMemory", capabilities.DedicatedVideoMemory },
                    { "Driver", capabilities.AdapterDriverVersion },
                    { "Software", capabilities.bSoftwareAdapter },
                } },
                { "Native", {
                    { "ApiVersion", VulkanVersionToString(capabilities.VulkanApiVersion) },
                    { "ApiVersionRaw", capabilities.VulkanApiVersion },
                    { "DriverVersionRaw", capabilities.VulkanDriverVersion },
                    { "TimelineSemaphore", capabilities.bVulkanTimelineSemaphore },
                    { "DynamicRendering", capabilities.bVulkanDynamicRendering },
                    { "Synchronization2", capabilities.bVulkanSynchronization2 },
                    { "ScalarBlockLayout", capabilities.bVulkanScalarBlockLayout },
                    { "SampledImageArrayNonUniformIndexing", capabilities.bVulkanSampledImageArrayNonUniformIndexing },
                    { "MaxPerStageSampledImages", capabilities.VulkanMaxPerStageSampledImages },
                    { "MaxPerStageSamplers", capabilities.VulkanMaxPerStageSamplers },
                    { "MaxBoundDescriptorSets", capabilities.VulkanMaxBoundDescriptorSets },
                } },
                { "RendererProfile", ToString(capabilities.Profile) },
                { "SupportedMsaaSampleMask", capabilities.SupportedMsaaSampleMask },
                { "FramesInFlight", capabilities.FramesInFlight },
                { "Queues", {
                    { "Graphics", capabilities.bGraphicsQueue },
                    { "Compute", capabilities.bComputeQueue },
                    { "Copy", capabilities.bCopyQueue },
                } },
                { "NVRHI", {
                    { "RayTracingAccelStruct", capabilities.bNvrhiRayTracingAccelStruct },
                    { "RayTracingPipeline", capabilities.bNvrhiRayTracingPipeline },
                    { "RayQuery", capabilities.bNvrhiRayQuery },
                    { "Meshlets", capabilities.bNvrhiMeshlets },
                    { "VariableRateShading", capabilities.bNvrhiVariableRateShading },
                    { "HeapDirectlyIndexed", capabilities.bNvrhiHeapDirectlyIndexed },
                    { "EnhancedBarriers", capabilities.bNvrhiEnhancedBarriers },
                    { "ShaderExecutionReordering", capabilities.bNvrhiShaderExecutionReordering },
                    { "RayTracingOpacityMicromap", capabilities.bNvrhiRayTracingOpacityMicromap },
                } },
                { "Features", {
                    { "BaselineRaster", feature(capabilities.BaselineRaster) },
                    { "ShaderModel6_6", feature(capabilities.ShaderModel6_6) },
                    { "ResourceBindingTier3", feature(capabilities.ResourceBindingTier3) },
                    { "HeapDirectlyIndexed", feature(capabilities.HeapDirectlyIndexed) },
                    { "EnhancedBarriers", feature(capabilities.EnhancedBarriers) },
                    { "RayTracingAccelStruct", feature(capabilities.RayTracingAccelStruct) },
                    { "RayTracingPipeline", feature(capabilities.RayTracingPipeline) },
                    { "RayQuery", feature(capabilities.RayQuery) },
                    { "MeshShader", feature(capabilities.MeshShader) },
                    { "VariableRateShading", feature(capabilities.VariableRateShading) },
                    { "SamplerFeedback", feature(capabilities.SamplerFeedback) },
                    { "ShaderExecutionReordering", feature(capabilities.ShaderExecutionReordering) },
                    { "RayTracingOpacityMicromap", feature(capabilities.RayTracingOpacityMicromap) },
                    { "HardwareRayTracingRendererPath", feature(capabilities.HardwareRayTracingRendererPath) },
                } },
            };
        }
    }

    struct VulkanRenderBackend::Impl
    {
        VulkanDeviceState State;
        Scope<VulkanSwapChain> SwapChain;
        std::array<FrameContext, cFramesInFlight> Frames;
        std::array<bool, cFramesInFlight> CompletionQueryArmed{};
        FrameContext* ActiveFrame = nullptr;
        uint32_t NextFrameSlot = 0;
        std::string TerminalFrameError;
        bool bInitialized = false;
    };

    VulkanRenderBackend::VulkanRenderBackend()
        : mImpl(CreateScope<Impl>())
    {
    }

    VulkanRenderBackend::~VulkanRenderBackend()
    {
        try
        {
            Shutdown();
        }
        catch (const std::exception& exception)
        {
            HE_CORE_ERROR("Vulkan backend shutdown failed: {0}", exception.what());
        }
    }

    void VulkanRenderBackend::Initialize(
        RenderSurface& surface,
        bool bEnableValidation,
        const RenderAdapterSelectionOptions& selection)
    {
        if (mImpl->bInitialized)
            throw std::logic_error("VulkanRenderBackend is already initialized");
        const auto* commonWindow = dynamic_cast<const CommonWindow*>(&surface);
        if (commonWindow == nullptr || commonWindow->GetGlfwWindow() == nullptr)
            throw std::invalid_argument("VulkanRenderBackend requires a GLFW CommonWindow render surface");

        try
        {
            CreateVulkanInstance(mImpl->State, true, bEnableValidation);
            CheckVulkan(
                "glfwCreateWindowSurface",
                glfwCreateWindowSurface(
                    mImpl->State.Instance,
                    commonWindow->GetGlfwWindow(),
                    nullptr,
                    &mImpl->State.Surface));
            CreateVulkanDevice(mImpl->State, selection, bEnableValidation, this, mCapabilities);
            mImpl->SwapChain = CreateScope<VulkanSwapChain>(
                mImpl->State,
                commonWindow->GetGlfwWindow(),
                commonWindow->IsVSync());
            for (uint32_t frameIndex = 0; frameIndex < cFramesInFlight; ++frameIndex)
            {
                FrameContext& frame = mImpl->Frames[frameIndex];
                frame.FrameIndex = frameIndex;
                frame.GraphicsCommandList = mImpl->State.DeviceHandle->createCommandList(
                    nvrhi::CommandListParameters()
                        .setEnableImmediateExecution(false)
                        .setQueueType(nvrhi::CommandQueue::Graphics));
                frame.CompletionQuery = mImpl->State.DeviceHandle->createEventQuery();
                if (!frame.GraphicsCommandList || !frame.CompletionQuery)
                    throw std::runtime_error("NVRHI failed to create a Vulkan frame command list or completion query");
            }
            mImpl->bInitialized = true;
            LogVulkanCapabilities(mCapabilities);
            HE_CORE_INFO(
                "Initialized Vulkan/NVRHI on {0}, {1} frame slot(s), validation layer: {2}",
                mCapabilities.AdapterName,
                cFramesInFlight,
                mImpl->State.bValidationEnabled ? "enabled" : "unavailable/disabled");
        }
        catch (...)
        {
            mImpl->SwapChain.reset();
            for (FrameContext& frame : mImpl->Frames)
                frame = {};
            DestroyDeviceState(mImpl->State);
            mCapabilities = {};
            throw;
        }
    }

    void VulkanRenderBackend::Shutdown()
    {
        if (!mImpl->bInitialized && mImpl->State.Instance == VK_NULL_HANDLE)
            return;
        std::exception_ptr waitError;
        if (mImpl->ActiveFrame != nullptr)
        {
            try { mImpl->ActiveFrame->GraphicsCommandList->close(); }
            catch (...) { waitError = std::current_exception(); }
            mImpl->ActiveFrame = nullptr;
        }
        if (mImpl->State.DeviceHandle)
        {
            try { WaitForIdle(); }
            catch (...) { if (!waitError) waitError = std::current_exception(); }
        }
        mImpl->SwapChain.reset();
        for (FrameContext& frame : mImpl->Frames)
            frame = {};
        mImpl->CompletionQueryArmed.fill(false);
        DestroyDeviceState(mImpl->State);
        mImpl->TerminalFrameError.clear();
        mImpl->NextFrameSlot = 0;
        mImpl->bInitialized = false;
        mCapabilities = {};
        if (waitError)
            std::rethrow_exception(waitError);
    }

    FrameContext& VulkanRenderBackend::BeginFrame()
    {
        if (!mImpl->bInitialized)
            throw std::logic_error("VulkanRenderBackend is not initialized");
        if (!mImpl->TerminalFrameError.empty())
            throw std::runtime_error("Vulkan backend is terminal: " + mImpl->TerminalFrameError);
        if (mImpl->ActiveFrame != nullptr)
            throw std::logic_error("BeginFrame called before the previous Vulkan frame was submitted");

        const uint32_t frameSlot = mImpl->NextFrameSlot;
        mImpl->NextFrameSlot = (mImpl->NextFrameSlot + 1u) % cFramesInFlight;
        FrameContext& frame = mImpl->Frames[frameSlot];
        if (frame.bInFlight)
        {
            if (!mImpl->CompletionQueryArmed[frameSlot])
                throw std::logic_error("Vulkan frame is in flight without an armed completion query");
            mImpl->State.DeviceHandle->waitEventQuery(frame.CompletionQuery);
            mImpl->State.DeviceHandle->resetEventQuery(frame.CompletionQuery);
            mImpl->CompletionQueryArmed[frameSlot] = false;
            frame.bInFlight = false;
            frame.CompletedSubmissionInstance = frame.SubmissionInstance;
        }
        mImpl->State.DeviceHandle->runGarbageCollection();
        frame.SwapChainImageIndex = mImpl->SwapChain->AcquireNextImageForFrame(frameSlot);
        frame.bSubmissionIssued = false;
        try
        {
            frame.GraphicsCommandList->open();
            mImpl->ActiveFrame = &frame;
        }
        catch (const std::exception& exception)
        {
            mImpl->TerminalFrameError = "BeginFrame command-list setup failed: " + std::string(exception.what());
            throw;
        }
        return frame;
    }

    void VulkanRenderBackend::AbortFrame(FrameContext& frame) noexcept
    {
        if (mImpl->ActiveFrame != &frame || frame.bSubmissionIssued)
            return;
        try
        {
            frame.GraphicsCommandList->close();
            frame.GraphicsCommandList = mImpl->State.DeviceHandle->createCommandList(
                nvrhi::CommandListParameters()
                    .setEnableImmediateExecution(false)
                    .setQueueType(nvrhi::CommandQueue::Graphics));
            if (!frame.GraphicsCommandList)
                throw std::runtime_error("NVRHI failed to replace an aborted Vulkan command list");
        }
        catch (const std::exception& exception)
        {
            mImpl->TerminalFrameError = "AbortFrame failed: " + std::string(exception.what());
            HE_CORE_ERROR("{0}", mImpl->TerminalFrameError);
        }
        mImpl->ActiveFrame = nullptr;
    }

    void VulkanRenderBackend::SubmitAndPresent(FrameContext& frame, bool bVSync)
    {
        if (!mImpl->bInitialized || mImpl->ActiveFrame != &frame)
            throw std::logic_error("SubmitAndPresent did not receive the active Vulkan frame");
        frame.GraphicsCommandList->setTextureState(
            mImpl->SwapChain->GetCurrentBackBuffer(),
            nvrhi::AllSubresources,
            nvrhi::ResourceStates::Present);
        try
        {
            frame.GraphicsCommandList->close();
            mImpl->ActiveFrame = nullptr;
            mImpl->SwapChain->PrepareSubmission(
                mImpl->State.NativeNvrhiDevice.Get(),
                frame.FrameIndex);
            frame.SubmissionInstance = mImpl->State.DeviceHandle->executeCommandList(
                frame.GraphicsCommandList,
                nvrhi::CommandQueue::Graphics);
            frame.bSubmissionIssued = true;
            frame.bInFlight = true;
            mImpl->State.DeviceHandle->setEventQuery(
                frame.CompletionQuery,
                nvrhi::CommandQueue::Graphics);
            mImpl->CompletionQueryArmed[frame.FrameIndex] = true;
            mImpl->SwapChain->Present(bVSync);
            mImpl->State.DeviceHandle->runGarbageCollection();
        }
        catch (const std::exception& exception)
        {
            mImpl->TerminalFrameError = std::string("Vulkan SubmitAndPresent failed ") +
                (frame.bSubmissionIssued ? "after" : "before") +
                " graphics submission: " + exception.what();
            throw std::runtime_error(mImpl->TerminalFrameError);
        }
    }

    void VulkanRenderBackend::Resize(uint32_t width, uint32_t height)
    {
        if (!mImpl->bInitialized)
            throw std::logic_error("VulkanRenderBackend is not initialized");
        if (width == 0 || height == 0)
            return;
        if (mImpl->ActiveFrame != nullptr)
            throw std::logic_error("Cannot resize the Vulkan swap chain while a frame is recording");
        WaitForIdle();
        mImpl->SwapChain->Resize(width, height);
        mImpl->State.DeviceHandle->runGarbageCollection();
    }

    void VulkanRenderBackend::WaitForIdle()
    {
        if (!mImpl->State.DeviceHandle)
            return;
        if (mImpl->ActiveFrame != nullptr)
            throw std::logic_error("Cannot wait for Vulkan idle while a frame is recording");
        if (!mImpl->State.DeviceHandle->waitForIdle())
            throw std::runtime_error("NVRHI failed to wait for the Vulkan device to become idle");
        CheckVulkan("vkDeviceWaitIdle", mImpl->State.Loader->DeviceWaitIdle(mImpl->State.Device));
        for (FrameContext& frame : mImpl->Frames)
        {
            if (!frame.bInFlight)
                continue;
            if (mImpl->CompletionQueryArmed[frame.FrameIndex])
            {
                mImpl->State.DeviceHandle->resetEventQuery(frame.CompletionQuery);
                mImpl->CompletionQueryArmed[frame.FrameIndex] = false;
            }
            frame.bInFlight = false;
            frame.CompletedSubmissionInstance = frame.SubmissionInstance;
        }
        mImpl->State.DeviceHandle->runGarbageCollection();
    }

    nvrhi::IDevice* VulkanRenderBackend::GetDevice() const
    {
        return mImpl->State.DeviceHandle;
    }

    SwapChain& VulkanRenderBackend::GetSwapChain()
    {
        if (!mImpl->SwapChain)
            throw std::logic_error("Vulkan swap chain is not initialized");
        return *mImpl->SwapChain;
    }

    void VulkanRenderBackend::message(nvrhi::MessageSeverity severity, const char* messageText)
    {
        const char* text = messageText != nullptr ? messageText : "<empty NVRHI Vulkan message>";
        if (severity == nvrhi::MessageSeverity::Info)
            HE_CORE_INFO("NVRHI Vulkan: {0}", text);
        else if (severity == nvrhi::MessageSeverity::Warning)
            HE_CORE_WARN("NVRHI Vulkan: {0}", text);
        else
            HE_CORE_ERROR("NVRHI Vulkan: {0}", text);
    }

    void RunVulkanCapabilityProbe(
        const std::filesystem::path& outputPath,
        const RenderAdapterSelectionOptions& selection)
    {
        if (outputPath.empty())
            throw std::invalid_argument("The Vulkan capability probe output path cannot be empty");
        VulkanDeviceState state;
        ProbeMessageCallback callback;
        RenderCapabilities capabilities;
        try
        {
            CreateVulkanInstance(state, false, false);
            CreateVulkanDevice(state, selection, false, &callback, capabilities);
            LogVulkanCapabilities(capabilities);
            HE_CORE_INFO("Vulkan capability probe: waiting for the NVRHI device to become idle");
            if (!state.DeviceHandle->waitForIdle())
                throw std::runtime_error("NVRHI failed to idle the Vulkan probe device");
            HE_CORE_INFO("Vulkan capability probe: device idle; serializing capabilities");

            const std::filesystem::path absoluteOutput = outputPath.is_absolute()
                ? outputPath.lexically_normal()
                : std::filesystem::absolute(outputPath).lexically_normal();
            std::error_code directoryError;
            if (!absoluteOutput.parent_path().empty())
                std::filesystem::create_directories(absoluteOutput.parent_path(), directoryError);
            if (directoryError)
                throw std::runtime_error("Could not create the Vulkan probe output directory: " + directoryError.message());
            Utils::SaveJsonToFile(VulkanCapabilitiesToJson(capabilities), absoluteOutput);
            HE_CORE_INFO("Vulkan capability probe wrote {0}", absoluteOutput.string());
        }
        catch (...)
        {
            DestroyDeviceState(state);
            throw;
        }
        DestroyDeviceState(state);
    }
}
