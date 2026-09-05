#pragma once

#ifdef JPH_DEBUG_RENDERER
#include <Jolt/Jolt.h>
#include <Jolt/Renderer/DebugRendererSimple.h>

#include <glm/glm.hpp>

#include <vector>
#endif

namespace HEngine
{
	#ifdef JPH_DEBUG_RENDERER
	class JoltDebugRendererAdapter final : public JPH::DebugRendererSimple
	{
	public:
		JoltDebugRendererAdapter() = default;
		~JoltDebugRendererAdapter() override = default;

		void FlushToRenderer2D();
		void Clear();
		void Reserve(size_t lineCapacity);

		void DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo, JPH::ColorArg inColor) override;
		void DrawText3D(JPH::RVec3Arg inPosition, const JPH::string_view& inString, JPH::ColorArg inColor, float inHeight) override;

	private:
		struct DebugLine
		{
			glm::vec3 From;
			glm::vec3 To;
			glm::vec4 Color;
		};

		std::vector<DebugLine> mLines;
	};
	#endif
}
