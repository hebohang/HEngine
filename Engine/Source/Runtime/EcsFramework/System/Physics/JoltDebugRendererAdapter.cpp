#include "Hepch.h"

#include "Runtime/EcsFramework/System/Physics/JoltDebugRendererAdapter.h"

#ifdef JPH_DEBUG_RENDERER
	#include "Runtime/Graphics/Renderer2D.h"

namespace HEngine
{
	void JoltDebugRendererAdapter::FlushToRenderer2D()
	{
		for (const DebugLine& line : mLines)
			Renderer2D::DrawLine(line.From, line.To, line.Color);
		mLines.clear();
	}

	void JoltDebugRendererAdapter::Clear()
	{
		mLines.clear();
	}

	void JoltDebugRendererAdapter::Reserve(size_t lineCapacity)
	{
		if (mLines.capacity() < lineCapacity)
			mLines.reserve(lineCapacity);
	}

	void JoltDebugRendererAdapter::DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo, JPH::ColorArg inColor)
	{
		const glm::vec4 color(
			inColor.r / 255.0f,
			inColor.g / 255.0f,
			inColor.b / 255.0f,
			inColor.a / 255.0f);

		const glm::vec3 from(
			static_cast<float>(inFrom.GetX()),
			static_cast<float>(inFrom.GetY()),
			static_cast<float>(inFrom.GetZ()));
		const glm::vec3 to(
			static_cast<float>(inTo.GetX()),
			static_cast<float>(inTo.GetY()),
			static_cast<float>(inTo.GetZ()));

		mLines.push_back({ from, to, color });
	}

	void JoltDebugRendererAdapter::DrawText3D(JPH::RVec3Arg inPosition, const JPH::string_view& inString, JPH::ColorArg inColor, float inHeight)
	{
		(void)inPosition;
		(void)inString;
		(void)inColor;
		(void)inHeight;
	}
}
#endif
