#pragma once

#include "../LLGI.Platform.h"
#include "LLGI.BaseWebGPU.h"

namespace LLGI
{

class RenderPassWebGPU;
class TextureWebGPU;

class PlatformWebGPU : public Platform
{
private:
	Window* window_ = nullptr;
	Vec2I windowSize_;
	wgpu::Instance instance_;
	wgpu::Adapter adapter_;
	wgpu::Device device_;
	wgpu::Surface surface_;
	wgpu::TextureFormat surfaceFormat_ = wgpu::TextureFormat::Undefined;
	wgpu::PresentMode presentMode_ = wgpu::PresentMode::Fifo;
	wgpu::SurfaceTexture surfaceTexture_;
	TextureWebGPU* currentScreenTexture_ = nullptr;
	RenderPassWebGPU* currentScreenRenderPass_ = nullptr;
	bool hasPresentedCurrentSurface_ = false;
	bool isPresentRequested_ = false;
	bool isPremultipliedAlphaEnabled_ = false;

	bool ConfigureSurface(const Vec2I& windowSize);
	bool AcquireCurrentScreen();
	void ResetCurrentScreen();

public:
	PlatformWebGPU() = default;
	explicit PlatformWebGPU(wgpu::Device device);
	~PlatformWebGPU() override;

	bool Initialize(Window* window, bool waitVSync, bool isPremultipliedAlphaEnabled = false);
	bool Initialize(wgpu::Device device, bool waitVSync, bool isPremultipliedAlphaEnabled = false);
	void SetPremultipliedAlphaEnabled(bool isPremultipliedAlphaEnabled);

	int GetCurrentFrameIndex() const override;
	int GetMaxFrameCount() const override;

	bool NewFrame() override;
	void Present() override;
	Graphics* CreateGraphics() override;
	DeviceType GetDeviceType() const override { return DeviceType::WebGPU; }
	void SetWindowSize(const Vec2I& windowSize) override;
	RenderPass* GetCurrentScreen(const Color8& clearColor, bool isColorCleared, bool isDepthCleared) override;
};

} // namespace LLGI
