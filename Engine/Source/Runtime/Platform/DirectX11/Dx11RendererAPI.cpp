#include "hepch.h"

#include "Runtime/Platform/DirectX11/Dx11RendererAPI.h"
#include "Runtime/Platform/DirectX11/Dx11ContextWrapper.h"
#include "Runtime/Platform/DirectX11/RenderStates.h"

#include <d3d11.h>
#include <glm/gtc/type_ptr.hpp>

namespace HEngine
{
    void Dx11RendererAPI::Init()
    {
        RenderStates::InitAll(Dx11ContextWrapper::GetDevice().Get());
        Dx11ContextWrapper::GetContext()->OMSetBlendState(RenderStates::BSTransparent.Get(), nullptr, 0xFFFFFFFF);
        Dx11ContextWrapper::GetContext()->RSSetState(RenderStates::RSNoCull.Get());
        Dx11ContextWrapper::GetContext()->OMSetDepthStencilState(RenderStates::DSSNoDepthWrite.Get(), 0);
    }

    void Dx11RendererAPI::SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
    {
        D3D11_VIEWPORT viewPort = {};
        viewPort.TopLeftX = x;
        viewPort.TopLeftY = y + height;
        viewPort.Width = width;
        viewPort.Height = height;
        Dx11ContextWrapper::GetContext()->RSSetViewports(1, &viewPort);

        Dx11ContextWrapper::OnResize();
    }

    void Dx11RendererAPI::SetClearColor(const glm::vec4& color)
    {
        Dx11ContextWrapper::GetContext()->ClearRenderTargetView(Dx11ContextWrapper::GetRTV().Get(), glm::value_ptr(color));
        
    }

    void Dx11RendererAPI::Clear()
    {
        
    }

    void Dx11RendererAPI::DrawIndexed(const Ref<VertexArray>& vertexArray, uint32_t indexCount)
    {
        Dx11ContextWrapper::GetContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        //Dx11ContextWrapper::GetContext()->DrawIndexed((UINT)indexCount, (UINT)startIndexLocation, (UINT)baseVertexLocation);
    }

    void Dx11RendererAPI::DrawLines(const Ref<VertexArray>& vertexArray, uint32_t vertexCount)
    {
        
    }

    void Dx11RendererAPI::SetLineWidth(float width)
    {
        
    }
}
