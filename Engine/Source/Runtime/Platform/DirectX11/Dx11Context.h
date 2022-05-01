#pragma once

#include "Runtime/Renderer/GraphicsContext.h"

#include <wrl/client.h>
#include <d3d11.h>

struct GLFWwindow;

namespace HEngine
{
    class Dx11Context : public GraphicsContext
    {
        template <class T>
        using ComPtr = Microsoft::WRL::ComPtr<T>;
    public:
        Dx11Context(GLFWwindow* windowHandle);

        virtual void Init() override;
        virtual void SwapBuffers() override;
    private:
        GLFWwindow* mWindowHandle;

        D3D11_VIEWPORT ScreenViewport;
        ComPtr<ID3D11Device> pDevice;
        ComPtr<IDXGISwapChain> pSwap;
        ComPtr<ID3D11DeviceContext> pContext;
        ComPtr<ID3D11RenderTargetView> pTarget;
        ComPtr<ID3D11DepthStencilView> pDSV;
        ComPtr<ID3D11Texture2D> pDepthStencilBuffer;				// 深度模板缓冲区

        bool bEnable4xMsaa = true;	          // 是否开启4倍多重采样
        UINT m4xMsaaQuality;          // MSAA支持的质量等级
    };
}