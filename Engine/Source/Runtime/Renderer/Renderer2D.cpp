#include "hepch.h"
#include "Renderer2D.h"

#include "Runtime/Renderer/VertexArray.h"
#include "Runtime/Renderer/Shader.h"
#include "Runtime/Renderer/RenderCommand.h"

#include "Runtime/Resource/AssetManager/AssetManager.h"

#include <glm/gtc/matrix_transform.hpp>

namespace HEngine
{
    struct QuadVertex
    {
        glm::vec3 Position;
        glm::vec4 Color;
        glm::vec2 TexCoord;
        float TexIndex;
        float TilingFactor;

		// Editor-only
		int EntityID;
    };

	struct CircleVertex
	{
		glm::vec3 WorldPosition;
		glm::vec3 LocalPosition;
		glm::vec4 Color;
		float Thickness;
		float Fade;

		// Editor-only
		int EntityID;
	};

	struct LineVertex
	{
		glm::vec3 Position;
		glm::vec4 Color;

		// Editor-only
		int EntityID;
	};

    struct Renderer2DData
    {
        static const uint32_t MaxQuads = 20000;
        static const uint32_t MaxVertices = MaxQuads * 4;
        static const uint32_t MaxIndices = MaxQuads * 6;
        static const uint32_t MaxTextureSlots = 32; // TODO: RenderCaps

        Ref<VertexArray> QuadVertexArray;
        Ref<VertexBuffer> QuadVertexBuffer;
        Ref<Shader> QuadShader;
        Ref<Texture2D> WhiteTexture;

		//------Line------
		Ref<VertexArray> LineVertexArray;
		Ref<VertexBuffer> LineVertexBuffer;
		Ref<Shader> LineShader;
		//------Line------

		//--------Circle-------------
		Ref<VertexArray> CircleVertexArray;
		Ref<VertexBuffer> CircleVertexBuffer;
		Ref<Shader> CircleShader;
		//--------Circle-------------

        uint32_t QuadIndexCount = 0;
        QuadVertex* QuadVertexBufferBase = nullptr;
        QuadVertex* QuadVertexBufferPtr = nullptr;

		//--------Circle-------------
		uint32_t CircleIndexCount = 0;
		CircleVertex* CircleVertexBufferBase = nullptr;
		CircleVertex* CircleVertexBufferPtr = nullptr;
		//--------Circle-------------

		//------Line------
		uint32_t LineVertexCount = 0;
		LineVertex* LineVertexBufferBase = nullptr;
		LineVertex* LineVertexBufferPtr = nullptr;

		float LineWidth = 2.0f;
		//------Line------

        std::array<Ref<Texture2D>, MaxTextureSlots> TextureSlots;
        uint32_t TextureSlotIndex = 1; // 0 = white texture

        glm::vec4 QuadVertexPositions[4];

        Renderer2D::Statistics Stats;
    };

    static Renderer2DData sData;

    void Renderer2D::Init()
    {
        sData.QuadVertexArray = VertexArray::Create();

        sData.QuadVertexBuffer = VertexBuffer::Create(sData.MaxVertices * sizeof(QuadVertex));
        sData.QuadVertexBuffer->SetLayout({
            { ShaderDataType::Float3,	"a_Position" },
            { ShaderDataType::Float4,	"a_Color" },
            { ShaderDataType::Float2,	"a_TexCoord" },
            { ShaderDataType::Float,	"a_TexIndex" },
            { ShaderDataType::Float,	"a_TilingFactor" },
            { ShaderDataType::Int,		"a_EntityID" },
        });
        sData.QuadVertexArray->AddVertexBuffer(sData.QuadVertexBuffer);

        sData.QuadVertexBufferBase = new QuadVertex[sData.MaxVertices];

        uint32_t* quadIndices = new uint32_t[sData.MaxIndices];

        uint32_t offset = 0;
        for (uint32_t i = 0; i < sData.MaxIndices; i += 6)
        {
            quadIndices[i + 0] = offset + 0;
            quadIndices[i + 1] = offset + 1;
            quadIndices[i + 2] = offset + 2;

            quadIndices[i + 3] = offset + 2;
            quadIndices[i + 4] = offset + 3;
            quadIndices[i + 5] = offset + 0;

            offset += 4;
        }

        Ref<IndexBuffer> quadIB = IndexBuffer::Create(quadIndices, sData.MaxIndices);
        sData.QuadVertexArray->SetIndexBuffer(quadIB);
        delete[] quadIndices;

		//------Circles------
		sData.CircleVertexArray = VertexArray::Create();

		sData.CircleVertexBuffer = VertexBuffer::Create(sData.MaxVertices * sizeof(CircleVertex));
		sData.CircleVertexBuffer->SetLayout({
			{ ShaderDataType::Float3,	"a_WorldPosition"	},
			{ ShaderDataType::Float3,	"a_LocalPosition"	},
			{ ShaderDataType::Float4,	"a_Color"			},
			{ ShaderDataType::Float,	"a_Thickness"		},
			{ ShaderDataType::Float,	"a_Fade"			},
			{ ShaderDataType::Int,		"a_EntityID"		},
			});
		sData.CircleVertexArray->AddVertexBuffer(sData.CircleVertexBuffer);
		sData.CircleVertexArray->SetIndexBuffer(quadIB); // Use quad IB
		sData.CircleVertexBufferBase = new CircleVertex[sData.MaxVertices];
		//------Circles------
		
		//------Lines------
		sData.LineVertexArray = VertexArray::Create();

		sData.LineVertexBuffer = VertexBuffer::Create(sData.MaxVertices * sizeof(LineVertex));
		sData.LineVertexBuffer->SetLayout({
			{ ShaderDataType::Float3,	"a_Position"		},
			{ ShaderDataType::Float4,	"a_Color"			},
			{ ShaderDataType::Int,		"a_EntityID"		},
			});
		sData.LineVertexArray->AddVertexBuffer(sData.LineVertexBuffer);
		sData.LineVertexBufferBase = new LineVertex[sData.MaxVertices];
		//------Lines------

        sData.WhiteTexture = Texture2D::Create(1, 1);
        uint32_t whiteTextureData = 0xffffffff;
        sData.WhiteTexture->SetData(&whiteTextureData, sizeof(uint32_t));

        int32_t samplers[sData.MaxTextureSlots];
        for (uint32_t i = 0; i < sData.MaxTextureSlots; i++)
            samplers[i] = i;

        sData.QuadShader = Shader::Create(AssetManager::GetInstance().GetFullPath("Shaders/Renderer2D_Quad.glsl"));
        sData.CircleShader = Shader::Create(AssetManager::GetInstance().GetFullPath("Shaders/Renderer2D_Circle.glsl"));
        sData.LineShader = Shader::Create(AssetManager::GetInstance().GetFullPath("Shaders/Renderer2D_Line.glsl"));

        sData.QuadShader->Bind();
        sData.QuadShader->SetIntArray("u_Textures", samplers, sData.MaxTextureSlots);

        // Set all texture slots to 0
        sData.TextureSlots[0] = sData.WhiteTexture;

        sData.QuadVertexPositions[0] = { -0.5f, -0.5f, 0.0f, 1.0f };
        sData.QuadVertexPositions[1] = {  0.5f, -0.5f, 0.0f, 1.0f };
        sData.QuadVertexPositions[2] = {  0.5f,  0.5f, 0.0f, 1.0f };
        sData.QuadVertexPositions[3] = { -0.5f,  0.5f, 0.0f, 1.0f };
    }

    void Renderer2D::Shutdown()
    {
		delete[] sData.QuadVertexBufferBase;
    }

    void Renderer2D::BeginScene(const Camera& camera, const glm::mat4& transform)
    {
        glm::mat4 viewProj = camera.GetProjection() * glm::inverse(transform);

        sData.QuadShader->Bind();
        sData.QuadShader->SetMat4("u_ViewProjection", viewProj);

		sData.CircleShader->Bind();
		sData.CircleShader->SetMat4("u_ViewProjection", viewProj);

		sData.LineShader->Bind();
		sData.LineShader->SetMat4("u_ViewProjection", viewProj);

		StartBatch();
    }

	void Renderer2D::BeginScene(const EditorCamera& camera)
	{
		glm::mat4 viewProj = camera.GetViewProjection();

		sData.QuadShader->Bind();
		sData.QuadShader->SetMat4("u_ViewProjection", viewProj);

		sData.CircleShader->Bind();
		sData.CircleShader->SetMat4("u_ViewProjection", viewProj);

		sData.LineShader->Bind();
		sData.LineShader->SetMat4("u_ViewProjection", viewProj);

		StartBatch();
	}

    void Renderer2D::EndScene()
    {
        Flush();
    }

	void Renderer2D::StartBatch()
	{
		sData.QuadIndexCount = 0;
		sData.QuadVertexBufferPtr = sData.QuadVertexBufferBase;

		sData.CircleIndexCount = 0;
		sData.CircleVertexBufferPtr = sData.CircleVertexBufferBase;

		sData.LineVertexCount = 0;
		sData.LineVertexBufferPtr = sData.LineVertexBufferBase;

		sData.TextureSlotIndex = 1;
	}

    void Renderer2D::Flush()
    {
		if (sData.QuadIndexCount)
		{
			uint32_t dataSize = (uint8_t*)sData.QuadVertexBufferPtr - (uint8_t*)sData.QuadVertexBufferBase;
			sData.QuadVertexBuffer->SetData(sData.QuadVertexBufferBase, dataSize);

			// Bind textures
			for (uint32_t i = 0; i < sData.TextureSlotIndex; i++)
				sData.TextureSlots[i]->Bind(i);

			sData.QuadShader->Bind();
			RenderCommand::DrawIndexed(sData.QuadVertexArray, sData.QuadIndexCount);
			sData.Stats.DrawCalls++;
		}

		if (sData.CircleIndexCount)
		{
			uint32_t dataSize = (uint32_t)((uint8_t*)sData.CircleVertexBufferPtr - (uint8_t*)sData.CircleVertexBufferBase);
			sData.CircleVertexBuffer->SetData(sData.CircleVertexBufferBase, dataSize);

			sData.CircleShader->Bind();
			RenderCommand::DrawIndexed(sData.CircleVertexArray, sData.CircleIndexCount);
			sData.Stats.DrawCalls++;
		}

		if (sData.LineVertexCount)
		{
			uint32_t dataSize = (uint32_t)((uint8_t*)sData.LineVertexBufferPtr - (uint8_t*)sData.LineVertexBufferBase);
			sData.LineVertexBuffer->SetData(sData.LineVertexBufferBase, dataSize);

			sData.LineShader->Bind();
			RenderCommand::SetLineWidth(sData.LineWidth);
			RenderCommand::DrawLines(sData.LineVertexArray, sData.LineVertexCount);
			sData.Stats.DrawCalls++;
		}
    }

	void Renderer2D::NextBatch()
	{
		Flush();
		StartBatch();
	}

    void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color)
    {
        DrawQuad({ position.x, position.y, 0.0f }, size, color);
    }

    void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color)
    {

        glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
            * glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

        DrawQuad(transform, color);
    }

    void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size, const Ref<Texture2D>& texture, float tilingFactor, const glm::vec4& tintColor)
    {
        DrawQuad({ position.x, position.y, 0.0f }, size, texture, tilingFactor, tintColor);
    }

    void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size, const Ref<Texture2D>& texture, float tilingFactor, const glm::vec4& tintColor)
    {
        glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
            * glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

        DrawQuad(transform, texture, tilingFactor, tintColor);
    }

    void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size, const Ref<SubTexture2D>& subtexture, float tilingFactor, const glm::vec4& tintColor)
    {
        DrawQuad({ position.x, position.y, 0.0f }, size, subtexture, tilingFactor, tintColor);
    }

    void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size, const Ref<SubTexture2D>& subtexture, float tilingFactor, const glm::vec4& tintColor)
    {
        constexpr size_t quadVertexCount = 4;
        const glm::vec2* textureCoords = subtexture->GetTexCoords();
        const Ref<Texture2D> texture = subtexture->GetTexture();

        if (sData.QuadIndexCount >= Renderer2DData::MaxIndices)
			NextBatch();

        float textureIndex = 0.0f;
        for (uint32_t i = 1; i < sData.TextureSlotIndex; i++)
        {
            if (*sData.TextureSlots[i].get() == *texture.get())
            {
                textureIndex = (float)i;
                break;
            }
        }

        if (textureIndex == 0.0f)
        {
            if (sData.TextureSlotIndex >= Renderer2DData::MaxTextureSlots)
				NextBatch();

            textureIndex = (float)sData.TextureSlotIndex;
            sData.TextureSlots[sData.TextureSlotIndex] = texture;
            sData.TextureSlotIndex++;
        }

        glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
            * glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

        for (size_t i = 0; i < quadVertexCount; i++)
        {
            sData.QuadVertexBufferPtr->Position = transform * sData.QuadVertexPositions[i];
            sData.QuadVertexBufferPtr->Color = tintColor;
            sData.QuadVertexBufferPtr->TexCoord = textureCoords[i];
            sData.QuadVertexBufferPtr->TexIndex = textureIndex;
            sData.QuadVertexBufferPtr->TilingFactor = tilingFactor;
            sData.QuadVertexBufferPtr++;
        }

        sData.QuadIndexCount += 6;

        sData.Stats.QuadCount++;
    }

    void Renderer2D::DrawQuad(const glm::mat4& transform, const glm::vec4& color, int entityID)
    {
        constexpr size_t quadVertexCount = 4;
        const float textureIndex = 0.0f; // White Texture
        constexpr glm::vec2 textureCoords[] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
        const float tilingFactor = 1.0f;

        if (sData.QuadIndexCount >= Renderer2DData::MaxIndices)
			NextBatch();

        for (size_t i = 0; i < quadVertexCount; i++)
        {
            sData.QuadVertexBufferPtr->Position = transform * sData.QuadVertexPositions[i];
            sData.QuadVertexBufferPtr->Color = color;
            sData.QuadVertexBufferPtr->TexCoord = textureCoords[i];
            sData.QuadVertexBufferPtr->TexIndex = textureIndex;
            sData.QuadVertexBufferPtr->TilingFactor = tilingFactor;
			sData.QuadVertexBufferPtr->EntityID = entityID;
            sData.QuadVertexBufferPtr++;
        }

        sData.QuadIndexCount += 6;

        sData.Stats.QuadCount++;
    }

    void Renderer2D::DrawQuad(const glm::mat4& transform, const Ref<Texture2D>& texture, float tilingFactor, const glm::vec4& tintColor, int entityID)
    {
        constexpr size_t quadVertexCount = 4;
        constexpr glm::vec2 textureCoords[] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };

        if (sData.QuadIndexCount >= Renderer2DData::MaxIndices)
			NextBatch();

        float textureIndex = 0.0f;
        for (uint32_t i = 1; i < sData.TextureSlotIndex; i++)
        {
            if (*sData.TextureSlots[i].get() == *texture.get())
            {
                textureIndex = (float)i;
                break;
            }
        }

        if (textureIndex == 0.0f)
        {
            if (sData.TextureSlotIndex >= Renderer2DData::MaxTextureSlots)
				NextBatch();

            textureIndex = (float)sData.TextureSlotIndex;
            sData.TextureSlots[sData.TextureSlotIndex] = texture;
            sData.TextureSlotIndex++;
        }

        for (size_t i = 0; i < quadVertexCount; i++)
        {
            sData.QuadVertexBufferPtr->Position = transform * sData.QuadVertexPositions[i];
            sData.QuadVertexBufferPtr->Color = tintColor;
            sData.QuadVertexBufferPtr->TexCoord = textureCoords[i];
            sData.QuadVertexBufferPtr->TexIndex = textureIndex;
            sData.QuadVertexBufferPtr->TilingFactor = tilingFactor;
			sData.QuadVertexBufferPtr->EntityID = entityID;
			sData.QuadVertexBufferPtr++;
        }

        sData.QuadIndexCount += 6;

        sData.Stats.QuadCount++;
    }

    void Renderer2D::DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, const glm::vec4& color)
    {
        DrawRotatedQuad({ position.x, position.y, 0.0f }, size, rotation, color);
    }

    void Renderer2D::DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, const glm::vec4& color)
    {
		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
			* glm::rotate(glm::mat4(1.0f), glm::radians(rotation), { 0.0f, 0.0f, 1.0f })
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		DrawQuad(transform, color);
    }

    void Renderer2D::DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, const Ref<Texture2D>& texture, float tilingFactor, const glm::vec4& tintColor)
    {
        DrawRotatedQuad({ position.x, position.y, 0.0f }, size, rotation, texture, tilingFactor, tintColor);
    }

    void Renderer2D::DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, const Ref<Texture2D>& texture, float tilingFactor, const glm::vec4& tintColor)
    {
		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
			* glm::rotate(glm::mat4(1.0f), glm::radians(rotation), { 0.0f, 0.0f, 1.0f })
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		DrawQuad(transform, texture, tilingFactor, tintColor);
    }

    void Renderer2D::DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, const Ref<SubTexture2D>& subtexture, float tilingFactor, const glm::vec4& tintColor)
    {
        DrawRotatedQuad({ position.x, position.y, 0.0f }, size, rotation, subtexture, tilingFactor, tintColor);
    }

    void Renderer2D::DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, const Ref<SubTexture2D>& subtexture, float tilingFactor, const glm::vec4& tintColor)
    {
        constexpr size_t quadVertexCount = 4;
        const glm::vec2* textureCoords = subtexture->GetTexCoords();
        const Ref<Texture2D> texture = subtexture->GetTexture();

        if (sData.QuadIndexCount >= Renderer2DData::MaxIndices)
			NextBatch();

        float textureIndex = 0.0f;
        for (uint32_t i = 1; i < sData.TextureSlotIndex; i++)
        {
            if (*sData.TextureSlots[i].get() == *texture.get())
            {
                textureIndex = (float)i;
                break;
            }
        }

        if (textureIndex == 0.0f)
        {
            if (sData.TextureSlotIndex >= Renderer2DData::MaxTextureSlots)
				NextBatch();

            textureIndex = (float)sData.TextureSlotIndex;
            sData.TextureSlots[sData.TextureSlotIndex] = texture;
            sData.TextureSlotIndex++;
        }

        glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
            * glm::rotate(glm::mat4(1.0f), rotation, { 0.0f, 0.0f, 1.0f })
            * glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

        for (size_t i = 0; i < quadVertexCount; i++)
        {
            sData.QuadVertexBufferPtr->Position = transform * sData.QuadVertexPositions[i];
            sData.QuadVertexBufferPtr->Color = tintColor;
            sData.QuadVertexBufferPtr->TexCoord = textureCoords[i];
            sData.QuadVertexBufferPtr->TexIndex = textureIndex;
            sData.QuadVertexBufferPtr->TilingFactor = tilingFactor;
            sData.QuadVertexBufferPtr++;
        }

        sData.QuadIndexCount += 6;

        sData.Stats.QuadCount++;
    }

	void Renderer2D::DrawCircle(const glm::mat4& transform, const glm::vec4& color, float thickness, float fade, int entityID)
	{
		// TODO: implement for circles
		// if (sData.QuadIndexCount >= Renderer2DData::MaxIndices)
		// 	 NextBatch();

		for (size_t i = 0; i < 4; i++)
		{
			sData.CircleVertexBufferPtr->WorldPosition = transform * sData.QuadVertexPositions[i];
			sData.CircleVertexBufferPtr->LocalPosition = sData.QuadVertexPositions[i] * 2.0f;
			sData.CircleVertexBufferPtr->Color = color;
			sData.CircleVertexBufferPtr->Thickness = thickness;
			sData.CircleVertexBufferPtr->Fade = fade;
			sData.CircleVertexBufferPtr->EntityID = entityID;
			sData.CircleVertexBufferPtr++;
		}

		sData.CircleIndexCount += 6;

		sData.Stats.QuadCount++;
	}

	void Renderer2D::DrawLine(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, int entityID)
	{
		sData.LineVertexBufferPtr->Position = p0;
		sData.LineVertexBufferPtr->Color = color;
		sData.LineVertexBufferPtr->EntityID = entityID;
		sData.LineVertexBufferPtr++;

		sData.LineVertexBufferPtr->Position = p1;
		sData.LineVertexBufferPtr->Color = color;
		sData.LineVertexBufferPtr->EntityID = entityID;
		sData.LineVertexBufferPtr++;

		sData.LineVertexCount += 2;
	}

	void Renderer2D::DrawSprite(const glm::mat4& transform, SpriteRendererComponent& src, int entityID)
	{
		if (src.Texture)
			DrawQuad(transform, src.Texture, src.TilingFactor, src.Color, entityID);
		else 
			DrawQuad(transform, src.Color, entityID);
	}

	void Renderer2D::DrawRect(const glm::mat4& transform, const glm::vec4& color, int entityID)
	{
		glm::vec3 lineVertices[4];

		for (size_t i = 0; i < 4; ++i)
		{
			lineVertices[i] = transform * sData.QuadVertexPositions[i];
		}

		DrawLine(lineVertices[0], lineVertices[1], color);
		DrawLine(lineVertices[1], lineVertices[2], color);
		DrawLine(lineVertices[2], lineVertices[3], color);
		DrawLine(lineVertices[3], lineVertices[0], color);
	}

	void Renderer2D::DrawRect(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color, int entityID)
	{
		glm::vec3 p0 = glm::vec3(position.x - size.x + 0.5f, position.y - size.y + 0.5f, position.z);
		glm::vec3 p1 = glm::vec3(position.x + size.x + 0.5f, position.y - size.y + 0.5f, position.z);
		glm::vec3 p2 = glm::vec3(position.x + size.x + 0.5f, position.y + size.y + 0.5f, position.z);
		glm::vec3 p3 = glm::vec3(position.x - size.x + 0.5f, position.y + size.y + 0.5f, position.z);
		DrawLine(p0, p1, color);
		DrawLine(p1, p2, color);
		DrawLine(p2, p3, color);
		DrawLine(p3, p0, color);
	}

	float Renderer2D::GetLineWidth()
	{
		return sData.LineWidth;
	}

	void Renderer2D::SetLineWidth(float width)
	{
		sData.LineWidth = width;
	}

    void Renderer2D::ResetStats()
    {
        memset(&sData.Stats, 0, sizeof(Statistics));
    }

    Renderer2D::Statistics Renderer2D::GetStats()
    {
        return sData.Stats;
    }
}
