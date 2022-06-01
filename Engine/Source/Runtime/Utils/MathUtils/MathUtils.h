#pragma once

#include <glm/glm.hpp>
#include <array>

namespace HEngine
{
	namespace Math
	{
		bool DecomposeTransform(const glm::mat4& transform, glm::vec3& translation, glm::vec3& rotation, glm::vec3& scale);

		glm::mat4 CalculateCovMatrix(const std::vector<glm::vec3>& vertices);
	}
}
