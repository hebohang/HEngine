#include "LLGI.PlatformWebGPU.h"
#include "LLGI.GraphicsWebGPU.h"
#include "LLGI.RenderPassWebGPU.h"
#include "LLGI.TextureWebGPU.h"

#include <limits>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace LLGI
{

namespace
{
bool IsSurfaceTextureAcquired(wgpu::SurfaceGetCurrentTextureStatus status)
{
	return status == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
		   status == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal;
}

wgpu::PresentMode SelectPresentMode(const wgpu::SurfaceCapabilities& capabilities, bool waitVSync)
{
	if (waitVSync)
	{
		return wgpu::PresentMode::Fifo;
	}

	for (size_t i = 0; i < capabilities.presentModeCount; i++)
	{
		if (capabilities.presentModes[i] == wgpu::PresentMode::Immediate)
		{
			return wgpu::PresentMode::Immediate;
		}
	}

	for (size_t i = 0; i < capabilities.presentModeCount; i++)
	{
		if (capabilities.presentModes[i] == wgpu::PresentMode::Mailbox)
		{
			return wgpu::PresentMode::Mailbox;
		}
	}

	return wgpu::PresentMode::Fifo;
}

wgpu::CompositeAlphaMode SelectCompositeAlphaMode(const wgpu::SurfaceCapabilities& capabilities, bool isPremultipliedAlphaEnabled)
{
	const auto preferred = isPremultipliedAlphaEnabled ? wgpu::CompositeAlphaMode::Premultiplied : wgpu::CompositeAlphaMode::Opaque;
	for (size_t i = 0; i < capabilities.alphaModeCount; i++)
	{
		if (capabilities.alphaModes[i] == preferred)
		{
			return preferred;
		}
	}

	for (size_t i = 0; i < capabilities.alphaModeCount; i++)
	{
		if (capabilities.alphaModes[i] == wgpu::CompositeAlphaMode::Auto)
		{
			return wgpu::CompositeAlphaMode::Auto;
		}
	}

	return capabilities.alphaModeCount > 0 ? capabilities.alphaModes[0] : wgpu::CompositeAlphaMode::Auto;
}
} // namespace

PlatformWebGPU::PlatformWebGPU(wgpu::Device device) : device_(device) {}

PlatformWebGPU::~PlatformWebGPU() { ResetCurrentScreen(); }

void PlatformWebGPU::ResetCurrentScreen()
{
#if !defined(__EMSCRIPTEN__)
	if (surface_ != nullptr && surfaceTexture_.texture != nullptr && isPresentRequested_ && !hasPresentedCurrentSurface_)
	{
		surface_.Present();
		hasPresentedCurrentSurface_ = true;
	}
#endif

	SafeRelease(currentScreenRenderPass_);
	SafeRelease(currentScreenTexture_);
	surfaceTexture_ = {};
	hasPresentedCurrentSurface_ = false;
	isPresentRequested_ = false;
}

bool PlatformWebGPU::ConfigureSurface(const Vec2I& windowSize)
{
	if (surface_ == nullptr || device_ == nullptr || windowSize.X <= 0 || windowSize.Y <= 0)
	{
		return false;
	}
#if !defined(__EMSCRIPTEN__)
	if (adapter_ == nullptr)
	{
		return false;
	}
#endif

	wgpu::SurfaceCapabilities capabilities{};
	surface_.GetCapabilities(adapter_, &capabilities);
	if (capabilities.formatCount == 0)
	{
		Log(LogType::Error, "WebGPU surface has no supported formats.");
		return false;
	}

	surfaceFormat_ = capabilities.formats[0];
	presentMode_ = SelectPresentMode(capabilities, waitVSync_);
	const auto alphaMode = SelectCompositeAlphaMode(capabilities, isPremultipliedAlphaEnabled_);

	wgpu::SurfaceConfiguration config{};
	config.device = device_;
	config.format = surfaceFormat_;
	config.width = static_cast<uint32_t>(windowSize.X);
	config.height = static_cast<uint32_t>(windowSize.Y);
	config.presentMode = presentMode_;
	config.alphaMode = alphaMode;
	config.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst;
	surface_.Configure(&config);

	windowSize_ = windowSize;
	return true;
}

bool PlatformWebGPU::Initialize(Window* window, bool waitVSync, bool isPremultipliedAlphaEnabled)
{
	waitVSync_ = waitVSync;
	window_ = window;
	isPremultipliedAlphaEnabled_ = isPremultipliedAlphaEnabled;
#if !defined(__EMSCRIPTEN__)
	if (window_ == nullptr)
	{
		return false;
	}
#endif

#if defined(__EMSCRIPTEN__)
	wgpu::InstanceDescriptor instanceDescriptor{};
	static constexpr auto timedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
	instanceDescriptor.requiredFeatureCount = 1;
	instanceDescriptor.requiredFeatures = &timedWaitAny;
	instance_ = wgpu::CreateInstance(&instanceDescriptor);
	if (instance_ == nullptr)
	{
		Log(LogType::Error, "Failed to create browser WebGPU instance.");
		return false;
	}

	auto device = wgpu::Device::Acquire(emscripten_webgpu_get_device());
	if (device == nullptr)
	{
		Log(LogType::Error, "Failed to get preinitialized browser WebGPU device.");
		return false;
	}

	device_ = device;
	if (window_ != nullptr)
	{
		windowSize_ = window_->GetWindowSize();

		wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvasSource{};
		canvasSource.selector = "#canvas";

		wgpu::SurfaceDescriptor surfaceDescriptor{};
		surfaceDescriptor.nextInChain = &canvasSource;
		surface_ = instance_.CreateSurface(&surfaceDescriptor);
		if (surface_ == nullptr)
		{
			Log(LogType::Error, "Failed to create browser WebGPU canvas surface.");
			return false;
		}

		return ConfigureSurface(windowSize_);
	}
	return true;
#elif defined(_WIN32)
	wgpu::InstanceDescriptor instanceDescriptor{};
	static constexpr auto timedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
	instanceDescriptor.requiredFeatureCount = 1;
	instanceDescriptor.requiredFeatures = &timedWaitAny;
	instance_ = wgpu::CreateInstance(&instanceDescriptor);
	if (instance_ == nullptr)
	{
		Log(LogType::Error, "Failed to create WebGPU instance.");
		return false;
	}

	wgpu::SurfaceSourceWindowsHWND hwndSource{};
	hwndSource.hinstance = GetModuleHandleW(nullptr);
	hwndSource.hwnd = window_->GetNativePtr(0);

	wgpu::SurfaceDescriptor surfaceDescriptor{};
	surfaceDescriptor.nextInChain = &hwndSource;
	surface_ = instance_.CreateSurface(&surfaceDescriptor);
	if (surface_ == nullptr)
	{
		Log(LogType::Error, "Failed to create WebGPU surface.");
		return false;
	}

	wgpu::RequestAdapterOptions adapterOptions{};
	adapterOptions.compatibleSurface = surface_;
	adapterOptions.powerPreference = wgpu::PowerPreference::HighPerformance;
	instance_.WaitAny(
		instance_.RequestAdapter(&adapterOptions,
								 wgpu::CallbackMode::WaitAnyOnly,
								 [this](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message) {
									 if (status != wgpu::RequestAdapterStatus::Success)
									 {
										 Log(LogType::Error,
											 std::string("Failed to request WebGPU adapter: ") + std::string(message.data, message.length));
										 return;
									 }
									 adapter_ = adapter;
								 }),
		std::numeric_limits<uint64_t>::max());
	if (adapter_ == nullptr)
	{
		return false;
	}

	wgpu::DeviceDescriptor deviceDescriptor{};
	std::vector<wgpu::FeatureName> requiredFeatures;
	const wgpu::FeatureName optionalFeatures[] = {
		wgpu::FeatureName::Float32Filterable,
		wgpu::FeatureName::TextureFormatsTier2,
		wgpu::FeatureName::TextureCompressionBC,
	};
	for (auto feature : optionalFeatures)
	{
		if (adapter_.HasFeature(feature))
		{
			requiredFeatures.push_back(feature);
		}
	}
	deviceDescriptor.requiredFeatureCount = requiredFeatures.size();
	deviceDescriptor.requiredFeatures = requiredFeatures.data();
	deviceDescriptor.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message) {
		Log(LogType::Error, std::string("WebGPU validation error: ") + std::string(message.data, message.length));
	});

	instance_.WaitAny(
		adapter_.RequestDevice(&deviceDescriptor,
							   wgpu::CallbackMode::WaitAnyOnly,
							   [this](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView message) {
								   if (status != wgpu::RequestDeviceStatus::Success)
								   {
									   Log(LogType::Error,
										   std::string("Failed to request WebGPU device: ") + std::string(message.data, message.length));
									   return;
								   }
								   device_ = device;
							   }),
		std::numeric_limits<uint64_t>::max());
	if (device_ == nullptr)
	{
		return false;
	}

	return ConfigureSurface(window_->GetWindowSize());
#else
	Log(LogType::Error, "WebGPU window platform initialization is implemented for Windows only.");
	return false;
#endif
}

bool PlatformWebGPU::Initialize(wgpu::Device device, bool waitVSync, bool isPremultipliedAlphaEnabled)
{
	waitVSync_ = waitVSync;
	device_ = device;
	isPremultipliedAlphaEnabled_ = isPremultipliedAlphaEnabled;
	return device_ != nullptr;
}

void PlatformWebGPU::SetPremultipliedAlphaEnabled(bool isPremultipliedAlphaEnabled)
{
	if (isPremultipliedAlphaEnabled_ == isPremultipliedAlphaEnabled)
	{
		return;
	}

	isPremultipliedAlphaEnabled_ = isPremultipliedAlphaEnabled;
	if (surface_ != nullptr && windowSize_.X > 0 && windowSize_.Y > 0)
	{
		ResetCurrentScreen();
		ConfigureSurface(windowSize_);
	}
}

int PlatformWebGPU::GetCurrentFrameIndex() const { return 0; }

int PlatformWebGPU::GetMaxFrameCount() const { return 1; }

bool PlatformWebGPU::NewFrame()
{
	if (device_ == nullptr)
	{
		return false;
	}

	if (window_ != nullptr)
	{
		if (!window_->OnNewFrame())
		{
			return false;
		}

		const auto windowSize = window_->GetWindowSize();
		if (windowSize != windowSize_)
		{
			SetWindowSize(windowSize);
		}
	}

	if (surface_ == nullptr)
	{
		return true;
	}

	ResetCurrentScreen();
	return true;
}

bool PlatformWebGPU::AcquireCurrentScreen()
{
	if (surface_ == nullptr)
	{
		return false;
	}

	if (currentScreenRenderPass_ != nullptr)
	{
		return true;
	}

	surface_.GetCurrentTexture(&surfaceTexture_);
	if (!IsSurfaceTextureAcquired(surfaceTexture_.status) || surfaceTexture_.texture == nullptr)
	{
		if (surfaceTexture_.status == wgpu::SurfaceGetCurrentTextureStatus::Outdated ||
			surfaceTexture_.status == wgpu::SurfaceGetCurrentTextureStatus::Lost)
		{
			ConfigureSurface(windowSize_);
		}
		return false;
	}

	TextureParameter textureParameter{};
	textureParameter.Usage = TextureUsageType::RenderTarget;
	textureParameter.Format = ConvertFormat(surfaceFormat_);
	textureParameter.Dimension = 2;
	textureParameter.Size = Vec3I(windowSize_.X, windowSize_.Y, 1);
	textureParameter.MipLevelCount = 1;
	textureParameter.SampleCount = 1;

	currentScreenTexture_ = new TextureWebGPU();
	if (!currentScreenTexture_->InitializeFromSurfaceTexture(device_, surfaceTexture_.texture, textureParameter))
	{
		ResetCurrentScreen();
		return false;
	}

	Texture* textures[] = {currentScreenTexture_};
	currentScreenRenderPass_ = new RenderPassWebGPU();
	if (!currentScreenRenderPass_->Initialize(textures, 1, nullptr, nullptr, nullptr))
	{
		ResetCurrentScreen();
		return false;
	}

	return true;
}

void PlatformWebGPU::Present()
{
	if (surface_ != nullptr && surfaceTexture_.texture != nullptr)
	{
		isPresentRequested_ = true;
	}
}

Graphics* PlatformWebGPU::CreateGraphics()
{
	if (device_ == nullptr)
	{
		return nullptr;
	}
	auto ret = new GraphicsWebGPU(device_, instance_);
	ret->SetWindowSize(windowSize_);
	return ret;
}

void PlatformWebGPU::SetWindowSize(const Vec2I& windowSize)
{
	if (windowSize == windowSize_)
	{
		return;
	}
	ConfigureSurface(windowSize);
}

RenderPass* PlatformWebGPU::GetCurrentScreen(const Color8& clearColor, bool isColorCleared, bool isDepthCleared)
{
	if (currentScreenRenderPass_ == nullptr)
	{
		AcquireCurrentScreen();
	}

	if (currentScreenRenderPass_ == nullptr)
	{
		return nullptr;
	}

	currentScreenRenderPass_->SetClearColor(clearColor);
	currentScreenRenderPass_->SetIsColorCleared(isColorCleared);
	currentScreenRenderPass_->SetIsDepthCleared(isDepthCleared);
	return currentScreenRenderPass_;
}

} // namespace LLGI
