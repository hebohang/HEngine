#pragma once

#include "Runtime/Resource/ResType/Skeleton.h"

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>

#include <cmath>
#include <stdexcept>
#include <vector>

namespace HEngine
{
    inline std::vector<glm::mat4> BuildMeshBindPalette(const SkeletonRes& skeleton)
    {
        const size_t count = skeleton.bones.size();
        if (!count || skeleton.inverseBindMatrices.size() != count)
            throw std::invalid_argument("Mesh bind skeleton requires bones and matching inverse binds");
        std::vector<const SkeletonBoneRes*> bones(count, nullptr);
        std::vector<glm::mat4> inverseBinds(count), globals(count), result(count);
        const auto finite = [](const glm::mat4& matrix) {
            for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
                if (!std::isfinite(matrix[c][r])) return false;
            return true;
        };
        for (size_t index = 0; index < count; ++index)
        {
            const auto& bone = skeleton.bones[index];
            if (bone.index < 0 || static_cast<size_t>(bone.index) >= count || bones[bone.index] ||
                bone.parentIndex < -1 || bone.parentIndex >= static_cast<int>(count) ||
                skeleton.inverseBindMatrices[index].size() != 16)
                throw std::invalid_argument("Invalid mesh bind skeleton indices or inverse bind layout");
            bones[bone.index] = &bone;
            inverseBinds[bone.index] = glm::make_mat4(skeleton.inverseBindMatrices[index].data());
            if (!finite(inverseBinds[bone.index]) || glm::determinant(inverseBinds[bone.index]) == 0.0f)
                throw std::invalid_argument("Non-finite or singular inverse bind");
        }
        std::vector<unsigned char> state(count, 0);
        for (size_t start = 0; start < count; ++start)
        {
            if (state[start] == 2) continue;
            std::vector<size_t> chain;
            int current = static_cast<int>(start);
            while (current >= 0 && state[current] != 2)
            {
                if (state[current] == 1) throw std::invalid_argument("Cycle in mesh bind skeleton");
                state[current] = 1;
                chain.push_back(static_cast<size_t>(current));
                current = bones[current]->parentIndex;
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            {
                const auto& bone = *bones[*it];
                const float rotationLength = glm::length(bone.localBindR);
                if (!std::isfinite(rotationLength) || rotationLength < 1e-8f)
                    throw std::invalid_argument("Invalid mesh bind rotation");
                const glm::mat4 local = glm::translate(glm::mat4(1), bone.localBindT) *
                    glm::toMat4(glm::normalize(bone.localBindR)) * glm::scale(glm::mat4(1), bone.localBindS);
                globals[*it] = bone.parentIndex >= 0 ? globals[bone.parentIndex] * local : local;
                result[*it] = globals[*it] * inverseBinds[*it];
                if (!finite(result[*it]) || glm::determinant(globals[*it]) == 0.0f)
                    throw std::invalid_argument("Non-finite or singular mesh bind palette");
                state[*it] = 2;
            }
        }
        return result;
    }
}
