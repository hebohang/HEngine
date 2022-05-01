// refer to https://github.com/hebohang/SEngine

#include "hepch.h"

#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Platform/DirectX11/Dx11Context.h"
#include "Runtime/Platform/DirectX11/DXTrace.h"

#include <GLFW/glfw3.h>

#define GLFW_EXPOSE_NATIVE_WIN32 1
#include <GLFW/glfw3native.h>

namespace HEngine
{
    Dx11Context::Dx11Context(GLFWwindow* windowHandle)
        : mWindowHandle(windowHandle)
    {
        HE_CORE_ASSERT(windowHandle, "Window handle is null!");
    }

    void Dx11Context::Init()
    {
		HWND hWnd = glfwGetWin32Window(mWindowHandle);

		// 注记：
		// 本来想用 D3D11CreateDeviceAndSwapChain 这个API一步到位
		// 事后发现不用 MSAA 结果是真的难看
		// 而 MSAA 需要用 CheckMultisampleQualityLevels API 判断
		// 但是这个API需要device，且结果用于填充交换链的描述符
		// 因此只能先创建device->判断MSAA情况->创建交换链，顺序不可调换

		HRESULT hr = S_OK;

		// 创建D3D设备 和 D3D设备上下文
		UINT createDeviceFlags = 0;
#if defined(DEBUG) || defined(_DEBUG)  
		createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
		// 特性等级数组
		D3D_FEATURE_LEVEL featureLevels[] =
		{
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0,
		};
		UINT numFeatureLevels = ARRAYSIZE(featureLevels);
		D3D_FEATURE_LEVEL featureLevel;
		hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevels, numFeatureLevels,
			D3D11_SDK_VERSION, pDevice.GetAddressOf(), &featureLevel, pContext.GetAddressOf());
		if (hr == E_INVALIDARG)
		{
			// Direct3D 11.0 的API不承认D3D_FEATURE_LEVEL_11_1，所以我们需要尝试特性等级11.0以及以下的版本
			hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, &featureLevels[1], numFeatureLevels - 1,
				D3D11_SDK_VERSION, pDevice.GetAddressOf(), &featureLevel, pContext.GetAddressOf());
		}
		if (FAILED(hr))
		{
			MessageBox(0, "D3D11CreateDevice Failed.", 0, 0);
		}
		// 检测是否支持特性等级11.0或11.1
		if (featureLevel != D3D_FEATURE_LEVEL_11_0 && featureLevel != D3D_FEATURE_LEVEL_11_1)
		{
			MessageBox(0, "Direct3D Feature Level 11 unsupported.", 0, 0);
		}

		// 创建好了Device了，现在可以去检测 MSAA支持的质量等级
		pDevice->CheckMultisampleQualityLevels(
			DXGI_FORMAT_R8G8B8A8_UNORM, 4, &m4xMsaaQuality);
		assert(m4xMsaaQuality > 0);

		DXGI_SWAP_CHAIN_DESC sd = {};
		sd.BufferDesc.Width = 0;
		sd.BufferDesc.Height = 0;
		sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		sd.BufferDesc.RefreshRate.Numerator = 0;
		sd.BufferDesc.RefreshRate.Denominator = 0;
		sd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
		sd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
		// 是否开启4倍多重采样？
		if (bEnable4xMsaa)
		{
			sd.SampleDesc.Count = 4;
			sd.SampleDesc.Quality = bEnable4xMsaa - 1;
		}
		else
		{
			sd.SampleDesc.Count = 1;
			sd.SampleDesc.Quality = 0;
		}
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount = 1;
		sd.OutputWindow = hWnd;
		sd.Windowed = TRUE;
		sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		sd.Flags = 0;

		UINT swapCreateFlags = 0u;
#ifndef NDEBUG
		swapCreateFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

		// 根据官方文档，ID3D11Device** 的参数为NULL将不返回Device，但是我尝试过后续会出问题
		// 想避开繁琐的 IDXGIFactory 方法创建交换链，于是再创建一次Device
		HR(D3D11CreateDeviceAndSwapChain(
			nullptr,
			D3D_DRIVER_TYPE_HARDWARE,
			nullptr,
			swapCreateFlags,
			nullptr,
			0,
			D3D11_SDK_VERSION,
			&sd,
			&pSwap,
			&pDevice,
			nullptr,
			&pContext
		));

        glfwMakeContextCurrent(mWindowHandle);
    }

    void Dx11Context::SwapBuffers()
    {
		HR(pSwap->Present(1, 0)); // Present with vsync
		//HR(pSwap->Present(0, 0)); // Present without vsync
    }

	void Dx11Context::OnResize()
	{
		// 释放渲染管线输出用到的相关资源
		pTarget.Reset();
		pDSV.Reset();
		pDepthStencilBuffer.Reset();

		// 重设交换链并且重新创建渲染目标视图
		ComPtr<ID3D11Texture2D> backBuffer;
		Application& app = Application::GetInstance();
		HR(pSwap->ResizeBuffers(1, app.GetWindow().GetWidth(), app.GetWindow().GetHeight(), DXGI_FORMAT_B8G8R8A8_UNORM, 0));	// 注意此处DXGI_FORMAT_B8G8R8A8_UNORM
		HR(pSwap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backBuffer.GetAddressOf())));
		HR(pDevice->CreateRenderTargetView(backBuffer.Get(), nullptr, pTarget.GetAddressOf()));

		backBuffer.Reset();


		D3D11_TEXTURE2D_DESC depthStencilDesc;

		depthStencilDesc.Width = app.GetWindow().GetWidth();
		depthStencilDesc.Height = app.GetWindow().GetHeight();
		depthStencilDesc.MipLevels = 1;
		depthStencilDesc.ArraySize = 1;
		depthStencilDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;

		// 要使用 4X MSAA?
		if (bEnable4xMsaa)
		{
			depthStencilDesc.SampleDesc.Count = 4;
			depthStencilDesc.SampleDesc.Quality = m4xMsaaQuality - 1;
		}
		else
		{
			depthStencilDesc.SampleDesc.Count = 1;
			depthStencilDesc.SampleDesc.Quality = 0;
		}


		depthStencilDesc.Usage = D3D11_USAGE_DEFAULT;
		depthStencilDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		depthStencilDesc.CPUAccessFlags = 0;
		depthStencilDesc.MiscFlags = 0;

		// 创建深度缓冲区以及深度模板视图
		HR(pDevice->CreateTexture2D(&depthStencilDesc, nullptr, pDepthStencilBuffer.GetAddressOf()));
		HR(pDevice->CreateDepthStencilView(pDepthStencilBuffer.Get(), nullptr, pDSV.GetAddressOf()));

		// 将渲染目标视图和深度/模板缓冲区结合到管线
		pContext->OMSetRenderTargets(1, pTarget.GetAddressOf(), pDSV.Get());

		// 设置视口变换
		ScreenViewport.TopLeftX = 0;
		ScreenViewport.TopLeftY = 0;
		ScreenViewport.Width = static_cast<float>(app.GetWindow().GetWidth());
		ScreenViewport.Height = static_cast<float>(app.GetWindow().GetHeight());
		ScreenViewport.MinDepth = 0.0f;
		ScreenViewport.MaxDepth = 1.0f;

		pContext->RSSetViewports(1, &ScreenViewport);
	}
}