#include "Editor/RawRenderTest/RawLayer.h"

namespace HEngine
{
    void RawLayer::OnUpdate(Timestep ts)
    {
        RenderCommand::SetClearColor({ 1.0f, 0.0f, 0.0f, 1.0f });
        RenderCommand::Clear();
    }
}