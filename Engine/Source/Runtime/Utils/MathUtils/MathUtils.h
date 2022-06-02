#pragma once

#include <glm/glm.hpp>
#include <array>

namespace HEngine
{
	namespace Math
	{
		bool DecomposeTransform(const glm::mat4& transform, glm::vec3& translation, glm::vec3& rotation, glm::vec3& scale);

		// will change every vertex in vertices (minus the average)
		// as the displacement
		glm::mat3 CalculateCovMatrix(std::vector<glm::vec3>& vertices, glm::vec3& outAvgPos);

		// eValues/eVectors [0] [1] [2] for three axes
		void JacobiSolver(glm::mat3 matrix, glm::vec3& eValues, glm::mat3& eVectors);

		void SchmidtOrthogonalization(glm::vec3& v0, glm::vec3& v1, glm::vec3& v2);
	}
}
