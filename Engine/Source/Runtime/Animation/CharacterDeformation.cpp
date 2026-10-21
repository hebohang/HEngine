#include "Hepch.h"

#include "Runtime/Animation/CharacterDeformation.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"
#include "Runtime/Graphics/Mesh/Mesh.h"
#include "Runtime/Profile/RenderFrameTiming.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <map>

namespace HEngine
{
    namespace
    {
        glm::mat4 SkinMatrix(const SkinnedVertex& vertex, const std::vector<glm::mat4>& palette,
            uint8_t influenceCount)
        {
            if (influenceCount == 1 && vertex.mBoneIDs[0] >= 0 &&
                static_cast<size_t>(vertex.mBoneIDs[0]) < palette.size() && vertex.mWeights[0] == 1.0f)
                return palette[vertex.mBoneIDs[0]];
            glm::mat4 result(0);
            float sum = 0;
            for (int i = 0; i < influenceCount; ++i)
            {
                const int bone = vertex.mBoneIDs[i];
                if (bone >= 0 && static_cast<size_t>(bone) < palette.size() && vertex.mWeights[i] > 0)
                { result += palette[bone] * vertex.mWeights[i]; sum += vertex.mWeights[i]; }
            }
            return sum == 1.0f ? result : sum > 1e-6f ? result / sum : glm::mat4(1);
        }

        bool PrepareClothTopology(ClothRenderState& solver, const SubMesh& section)
        {
            const auto& bind = section.GetBaseSkinnedVertices();
            const auto& indices = section.GetIndices();
            if (solver.BindVertexData == bind.data() && solver.IndexData == indices.data() &&
                solver.IndexCount == indices.size() && solver.RenderToParticle.size() == bind.size() &&
                solver.InfluenceCounts.size() == bind.size()) return true;
            solver = ClothRenderState{};
            solver.BindVertexData = bind.data();
            solver.IndexData = indices.data();
            solver.IndexCount = indices.size();
            solver.RenderToParticle.reserve(bind.size());
            solver.InfluenceCounts.reserve(bind.size());
            std::map<std::array<int64_t, 3>, uint32_t> particles;
            for (const auto& vertex : bind)
            {
                uint8_t influenceCount = 0;
                for (int i = 0; i < MAX_BONE_INFLUENCE; ++i)
                    if (vertex.mBoneIDs[i] >= 0 && vertex.mWeights[i] > 0)
                        influenceCount = static_cast<uint8_t>(i + 1);
                solver.InfluenceCounts.push_back(influenceCount);
                std::array<int64_t, 3> key;
                for (int axis = 0; axis < 3; ++axis)
                {
                    const double value = static_cast<double>(vertex.Pos[axis]) * 100000.0;
                    if (!std::isfinite(value) || std::abs(value) > 1e15)
                    { solver = ClothRenderState{}; return false; }
                    key[axis] = static_cast<int64_t>(std::llround(value));
                }
                const auto [found, inserted] = particles.emplace(key, static_cast<uint32_t>(particles.size()));
                solver.RenderToParticle.push_back(found->second);
            }
            solver.ParticleCount = static_cast<uint32_t>(particles.size());
            solver.Contributors.assign(solver.ParticleCount, 0);
            for (const auto particle : solver.RenderToParticle) ++solver.Contributors[particle];
            solver.SimulationTriangles.reserve(indices.size());
            for (size_t i = 0; i + 2 < indices.size(); i += 3)
            {
                if (indices[i] >= bind.size() || indices[i + 1] >= bind.size() || indices[i + 2] >= bind.size())
                { solver = ClothRenderState{}; return false; }
                const auto a = solver.RenderToParticle[indices[i]], b = solver.RenderToParticle[indices[i + 1]],
                    c = solver.RenderToParticle[indices[i + 2]];
                if (a == b || b == c || c == a) continue;
                solver.SimulationTriangles.insert(solver.SimulationTriangles.end(), {a, b, c});
            }
            return true;
        }

        bool ApplyCloth(AnimatorComponent& animator, Mesh& mesh, SubMesh& section,
            size_t sectionIndex, const ClothPanel& panel, const glm::mat4& world, float dt,
            const std::vector<ClothWorldCapsule>& colliders)
        {
            auto& vertices = section.mSkinnedVertices;
            if (vertices.empty()) return false;
            const auto& palette = mesh.bUseExternalBoneMatrices ? mesh.mExternalBoneMatrices : mesh.mAnimator.GetFinalBoneMatrices();
            auto& solver = animator.mClothSimulation[static_cast<int>(sectionIndex)];
            if (!PrepareClothTopology(solver, section)) return true;
            solver.Rest.assign(solver.ParticleCount, glm::vec3(0));
            solver.Radii.assign(solver.ParticleCount, 3.0f);
            solver.InverseSkins.resize(vertices.size());
            solver.NormalToLocal.resize(vertices.size());
            auto& rest = solver.Rest;
            auto& radii = solver.Radii;
            auto& inverseSkins = solver.InverseSkins;
            const float maxDistance = std::isfinite(panel.MaxDistance) ? std::clamp(panel.MaxDistance, 0.0f, 3.0f) : 0.0f;
            {
                ScopedAnimationCpuTiming skinTiming(sAnimationCpuTimings.ClothSkinMilliseconds);
                for (size_t i = 0; i < vertices.size(); ++i)
                {
                    const auto& vertex = vertices[i];
                    const glm::mat4 skin = world * SkinMatrix(vertex, palette, solver.InfluenceCounts[i]);
                    const float determinant = glm::determinant(skin);
                    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-8f) { solver.Reset(); return true; }
                    const uint32_t particle = solver.RenderToParticle[i];
                    rest[particle] += glm::vec3(skin * glm::vec4(vertex.Pos, 1));
                    inverseSkins[i] = glm::inverse(skin);
                    // Pulling a world normal back uses transpose(skin), not another
                    // inversion of the already inverted matrix (also valid for scale).
                    solver.NormalToLocal[i] = glm::transpose(glm::mat3(skin));
                    const float sourcePaint = vertex.ClothWeight < 0.0f ? vertex.Color.r : vertex.ClothWeight;
                    const float paint = std::isfinite(sourcePaint) ? std::clamp(sourcePaint, 0.0f, 1.0f) : 0.0f;
                    // A pinned copy at a UV seam pins the shared physical vertex.
                    radii[particle] = std::min(radii[particle], paint * maxDistance);
                }
                for (size_t i = 0; i < rest.size(); ++i) rest[i] /= static_cast<float>(solver.Contributors[i]);
            }
            {
                ScopedAnimationCpuTiming solveTiming(sAnimationCpuTimings.ClothSolveMilliseconds);
                solver.Step(dt, rest, solver.SimulationTriangles, radii, colliders,
                    animator.SecondaryWind, panel, glm::vec3(world[3]));
            }
            const auto& positions = solver.Positions();
            if (positions.size() != rest.size()) return true;
            ScopedAnimationCpuTiming renderTiming(sAnimationCpuTimings.ClothRenderMilliseconds);
            solver.RenderPositions.resize(vertices.size());
            auto& renderPositions = solver.RenderPositions;
            for (size_t i = 0; i < vertices.size(); ++i)
            {
                renderPositions[i] = positions[solver.RenderToParticle[i]];
                vertices[i].Pos = glm::vec3(inverseSkins[i] * glm::vec4(renderPositions[i], 1));
            }

            // Reconstruct frame in simulated world-space then pull back through
            // each skin matrix. This keeps lighting attached to the bent cloth.
            solver.Normals.assign(vertices.size(), glm::vec3(0));
            solver.Tangents.assign(vertices.size(), glm::vec3(0));
            auto& normals = solver.Normals;
            auto& tangents = solver.Tangents;
            const auto& indices = section.GetIndices();
            for (size_t i = 0; i + 2 < indices.size(); i += 3)
            {
                const auto a = indices[i], b = indices[i + 1], c = indices[i + 2];
                if (a >= renderPositions.size() || b >= renderPositions.size() || c >= renderPositions.size()) continue;
                const glm::vec3 e1 = renderPositions[b] - renderPositions[a], e2 = renderPositions[c] - renderPositions[a];
                const glm::vec3 normal = glm::cross(e1, e2);
                const glm::vec2 uv1 = vertices[b].TexCoord - vertices[a].TexCoord, uv2 = vertices[c].TexCoord - vertices[a].TexCoord;
                const float determinant = uv1.x * uv2.y - uv1.y * uv2.x;
                const glm::vec3 tangent = std::abs(determinant) > 1e-8f ? (e1 * uv2.y - e2 * uv1.y) / determinant : e1;
                for (auto index : {a, b, c}) { normals[index] += normal; tangents[index] += tangent; }
            }
            for (size_t i = 0; i < vertices.size(); ++i)
            {
                auto& vertex = vertices[i];
                const float sign = glm::dot(glm::cross(vertex.Normal, vertex.Tangent), vertex.Bitangent) < 0 ? -1.0f : 1.0f;
                if (glm::length(normals[i]) > 1e-7f)
                    vertex.Normal = glm::normalize(solver.NormalToLocal[i] * glm::normalize(normals[i]));
                glm::vec3 tangent = glm::mat3(inverseSkins[i]) * tangents[i];
                tangent -= vertex.Normal * glm::dot(tangent, vertex.Normal);
                if (glm::length(tangent) > 1e-7f) vertex.Tangent = glm::normalize(tangent);
                vertex.Bitangent = sign * glm::cross(vertex.Normal, vertex.Tangent);
            }
            return solver.WasReset();
        }
    }

    CharacterDeformationTick ResolveCharacterDeformationTick(const AnimatorComponent& animator,
        float frameDeltaSeconds, bool inRange)
    {
        return {animator.bLoaded && animator.bPlay && inRange,
            animator.bPaused ? animator.mStepSeconds : frameDeltaSeconds};
    }

    uint64_t PreviewCharacterDeformation(AnimatorComponent& animator, Mesh& mesh)
    {
        animator.mClothSimulation.clear();
        animator.mDeformationWasActive = false;
        const bool needsStream = std::any_of(mesh.mSubMeshes.begin(), mesh.mSubMeshes.end(),
            [](const auto& section) {
                return section && (section->HasEffectiveMorphTargets() || section->GetDeformationRevision() > 0);
            });
        if (!needsStream) return 0;
        mesh.EnsureUniqueDeformation();
        uint64_t updatedVertices = 0;
        for (auto& section : mesh.mSubMeshes)
        {
            if (!section || (!section->HasEffectiveMorphTargets() && section->GetDeformationRevision() == 0)) continue;
            const auto previousRevision = section->GetDeformationRevision();
            // Cloth's EndDeformation invalidates the pure-morph cache. This call
            // restores base position/normal/tangent streams before applying morphs,
            // even for a formerly simulated section with no morph targets.
            section->UpdatePureMorphWeights(animator.FacialWeights, true);
            if (section->GetDeformationRevision() != previousRevision)
                updatedVertices += section->IsSkinned() ? section->mSkinnedVertices.size() : section->mStaticVertices.size();
        }
        return updatedVertices;
    }

    uint64_t UpdateCharacterDeformation(AnimatorComponent& animator, Mesh& mesh,
        const glm::mat4& world, float deltaSeconds, bool active)
    {
        const bool hasMorph = std::any_of(mesh.mSubMeshes.begin(), mesh.mSubMeshes.end(),
            [](const auto& section) { return section && section->HasEffectiveMorphTargets(); });
        if (!hasMorph && animator.ClothPanels.empty() && !animator.mDeformationWasActive) return 0;
        uint64_t updatedVertices = 0;
        const auto vertexCount = [](const SubMesh& section) {
            return section.IsSkinned() ? section.mSkinnedVertices.size() : section.mStaticVertices.size();
        };
        mesh.EnsureUniqueDeformation();
        if (!active)
        {
            if (animator.mDeformationWasActive)
                for (auto& section : mesh.mSubMeshes)
                    if (section && section->GetDeformationRevision() > 0)
                    {
                        section->EndDeformation(true);
                        updatedVertices += vertexCount(*section);
                    }
            animator.mDeformationWasActive = false;
            animator.mClothSimulation.clear();
            return updatedVertices;
        }
        const bool reset = !animator.mDeformationWasActive || deltaSeconds > .25f ||
            glm::distance(glm::vec3(world[3]), animator.mDeformationOrigin) > 2;
        animator.mDeformationOrigin = glm::vec3(world[3]);
        animator.mDeformationWasActive = true;
        const auto& weights = animator.mFacialPlayer.Update(deltaSeconds, animator.FacialPerformancePath,
            animator.FacialPerformanceData, animator.FacialWeights, animator.FacialBlinkLeft,
            animator.FacialBlinkRight, animator.FacialBlinkInterval);
        std::vector<ClothWorldCapsule> colliders;
        const auto& palette = mesh.bUseExternalBoneMatrices ? mesh.mExternalBoneMatrices : mesh.mAnimator.GetFinalBoneMatrices();
        for (const auto& capsule : animator.ClothColliders)
        {
            glm::mat4 bone;
            if (!CharacterPose::BoneMatrix(mesh.mAnimation, palette, capsule.Bone, bone)) continue;
            const glm::mat4 transform = world * bone;
            const float scale = std::max({glm::length(glm::vec3(transform[0])), glm::length(glm::vec3(transform[1])), glm::length(glm::vec3(transform[2]))});
            colliders.push_back({glm::vec3(transform * glm::vec4(capsule.Start, 1)), glm::vec3(transform * glm::vec4(capsule.End, 1)), capsule.Radius * scale});
        }
        for (size_t i = 0; i < mesh.mSubMeshes.size(); ++i)
        {
            auto& section = mesh.mSubMeshes[i];
            if (!section) continue;
            const auto panel = std::find_if(animator.ClothPanels.begin(), animator.ClothPanels.end(),
                [&](const auto& item) { return item.MaterialIndex >= 0 && static_cast<uint32_t>(item.MaterialIndex) == section->mMaterialIndex; });
            const bool cloth = panel != animator.ClothPanels.end() && section->IsSkinned();
            if (!section->HasEffectiveMorphTargets() && !cloth && section->GetDeformationRevision() == 0) continue;
            if (!cloth)
            {
                const auto previousRevision = section->GetDeformationRevision();
                section->UpdatePureMorphWeights(weights, reset);
                if (section->GetDeformationRevision() != previousRevision) updatedVertices += vertexCount(*section);
                continue;
            }
            section->BeginDeformation();
            section->ApplyMorphWeights(weights);
            bool resetCloth = false;
            if (cloth) resetCloth = ApplyCloth(animator, mesh, *section, i, *panel, world, deltaSeconds, colliders);
            section->EndDeformation(reset || resetCloth);
            updatedVertices += vertexCount(*section);
        }
        return updatedVertices;
    }
}
