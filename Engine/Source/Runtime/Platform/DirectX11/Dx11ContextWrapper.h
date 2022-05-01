#pragma once

#include "Runtime/Core/Base/PublicSingleton.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Platform/DirectX11/Dx11Context.h"

namespace HEngine
{
    class Dx11ContextWrapper
    {
		template <class T>
		using ComPtr = Microsoft::WRL::ComPtr<T>;
	public:
		static void OnResize() { GetDx11ContextHelp()->OnResize(); }
    public:
		[[nodiscard]] static D3D11_VIEWPORT& GetVp()					{ return GetDx11ContextHelp()->GetVp(); }
		[[nodiscard]] static ComPtr<ID3D11Device>& GetDevice()			{ return GetDx11ContextHelp()->GetDevice(); }
		[[nodiscard]] static ComPtr<IDXGISwapChain>& GetSwapChain()		{ return GetDx11ContextHelp()->GetSwapChain(); }
		[[nodiscard]] static ComPtr<ID3D11DeviceContext>& GetContext()	{ return GetDx11ContextHelp()->GetContext(); }
		[[nodiscard]] static ComPtr<ID3D11RenderTargetView>& GetRTV()	{ return GetDx11ContextHelp()->GetRTV(); }
		[[nodiscard]] static ComPtr<ID3D11DepthStencilView>& GetDSV()	{ return GetDx11ContextHelp()->GetDSV(); }
		[[nodiscard]] static ComPtr<ID3D11Texture2D>& GetDSBuffer()		{ return GetDx11ContextHelp()->GetDSBuffer(); }
	private:
		static Ref<Dx11Context> GetDx11ContextHelp()
		{
			Application& app = Application::GetInstance();
			return std::static_pointer_cast<Dx11Context>(app.GetWindow().GetGraphicsContext());
		}
    };
}