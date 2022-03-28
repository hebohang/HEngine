#pragma once

#include "HEngine.h"

#include "ParticleSystem.h"

class Sandbox2D : public HEngine::Layer
{
public:
    Sandbox2D();
    virtual ~Sandbox2D() = default;

    virtual void OnAttach() override;
    virtual void OnDetach() override;

    void OnUpdate(HEngine::Timestep ts) override;
    virtual void OnImGuiRender() override;
    void OnEvent(HEngine::Event& e) override;
private:
    HEngine::OrthographicCameraController m_CameraController;

    // Temp
    HEngine::Ref<HEngine::VertexArray> m_SquareVA;
    HEngine::Ref<HEngine::Shader> m_FlatColorShader;

    HEngine::Ref<HEngine::Texture2D> m_CheckerboardTexture;
    HEngine::Ref<HEngine::Texture2D> m_SpriteSheet;
    HEngine::Ref<HEngine::SubTexture2D> m_TextureStairs, m_TextureBarrel, m_TextureTree;

    glm::vec4 m_SquareColor = { 0.2f, 0.3f, 0.8f, 1.0f };

    ParticleProps m_Particle;
    ParticleSystem m_ParticleSystem;
};