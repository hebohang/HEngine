#include "Hepch.h"
#include "Math.h"
#include <glm/gtx/matrix_decompose.hpp>
#include "MathUtils.h"

namespace HEngine::Math
{
	bool DecomposeTransform(const glm::mat4& transform, glm::vec3& translation, glm::vec3& rotation, glm::vec3& scale)
	{
		glm::vec3 skew(0.0f);
		glm::vec4 perspective(0.0f);
		glm::quat orientation(1.0f, 0.0f, 0.0f, 0.0f);

		if (!glm::decompose(transform, scale, orientation, translation, skew, perspective))
			return false;

		rotation = glm::eulerAngles(glm::normalize(orientation));
		return true;
	}

	glm::mat3 CalculateCovMatrix(std::vector<glm::vec3>& vertices, glm::vec3& outAvgPos)
	{
		glm::mat3 reMat{ 0 };

		glm::vec3 avgVec{ 0 };
		for (const auto& vertex : vertices)
		{
			avgVec += vertex;
		}
		avgVec /= vertices.size();
		outAvgPos = avgVec;
		for (auto& vertex : vertices)
		{
			vertex -= avgVec; // X - E[X], E is the Expectation
		}

		for (uint32_t row = 0; row < 3; row++)
		{
			for (uint32_t colum = 0; colum < 3; colum++)
			{
				for (const auto& vertex : vertices)
				{
					reMat[row][colum] += vertex[row] * vertex[colum];
				}
			}
		}
		reMat /= vertices.size();

		return reMat;
	}

	// offer to https://github.com/Tuufless/PCA-OBB/blob/master/OBBGenerator/Main.cpp
	void JacobiSolver(glm::mat3 matrix, glm::vec3& eValues, glm::mat3& eVectors)
	{
		const float eps1 = 0.000001f;
		const float eps2 = 0.000001f;
		const float eps3 = 0.000001f;
		const float INV_SQRT_TWO = 0.707106781f;

		float p, q, spq;
		float cosa, sina;					// holds cos(alpha) and sin(alpha)
		float temp;							// used for temporary storage
		float s1 = 0.0f;					// sums of squares of diagonal
		float s2;							// elements

		bool flag = true;					// determines whether to iterate again.
		int iteration = 0;					// iteration counter

		glm::vec3 mik;						// used for temporary storage of m[i][k]

		glm::mat3 t = glm::mat3(1.0f);	// stores the product of the rotation matrices.
											// Its columns ultimately hold the eigenvectors

		do
		{
			iteration++;

			for (int i = 0; i < 2; ++i)
			{
				for (int j = i + 1; j < 3; ++j)
				{
					if ((fabs(matrix[j][i]) < eps1))
					{
						matrix[j][i] = 0.0f;
					}
					else
					{
						q = fabs(matrix[i][i] - matrix[j][j]);

						if (q > eps2)
						{
							p = 2.0f * matrix[j][i] * q / (matrix[i][i] - matrix[j][j]);
							spq = sqrt(p * p + q * q);
							cosa = sqrt((1.0f + q / spq) / 2.0f);
							sina = p / (2.0f * cosa * spq);
						}
						else
						{
							sina = cosa = INV_SQRT_TWO;
						}

						for (int k = 0; k < 3; ++k)
						{
							temp = t[k][i];
							t[k][i] = temp * cosa + t[k][j] * sina;
							t[k][j] = temp * sina - t[k][j] * cosa;
						}

						for (int k = i; k < 3; ++k)
						{
							if (k > j)
							{
								temp = matrix[k][i];
								matrix[k][i] = cosa * temp + sina * matrix[k][j];
								matrix[k][j] = sina * temp - cosa * matrix[k][j];
							}
							else
							{
								mik[k] = matrix[k][i];
								matrix[k][i] = cosa * mik[k] + sina * matrix[j][k];

								if (k == j)
								{
									matrix[k][j] = sina * mik[k] - cosa * matrix[k][j];
								}
							}
						}

						mik[j] = sina * mik[i] - cosa * mik[j];

						for (int k = 0; k <= j; ++k)
						{
							if (k <= i)
							{
								temp = matrix[i][k];
								matrix[i][k] = cosa * temp + sina * matrix[j][k];
								matrix[j][k] = sina * temp - cosa * matrix[j][k];
							}
							else
							{
								matrix[j][k] = sina * mik[k] - cosa * matrix[j][k];
							}
						}
					}
				}
			}

			s2 = 0.0f;

			for (int i = 0; i < 3; ++i)
			{
				eValues[i] = matrix[i][i];
				s2 += eValues[i] * eValues[i];
			}

			if (fabs(s2) < static_cast<float>(1.e-5) || fabs(1 - s1 / s2) < eps3)
			{
				flag = false;
			}
			else
			{
				s1 = s2;
			}
		} while (flag);

		eVectors[0] = t[0];
		eVectors[1] = t[1];
		eVectors[2] = t[2];

		// preserve righthanded-ness:
		if (glm::dot(glm::cross(eVectors[0], eVectors[1]), eVectors[2]) < 0.0f)
		{
			eVectors[2] = -eVectors[2];
		}
	}

	void SchmidtOrthogonalization(glm::vec3& v0, glm::vec3& v1, glm::vec3& v2)
	{
		v0 = glm::normalize(v0);
		v1 -= glm::dot(v0, v1) * v0;
		v1 = glm::normalize(v1);
		v2 = glm::cross(v0, v1);
	}
}
