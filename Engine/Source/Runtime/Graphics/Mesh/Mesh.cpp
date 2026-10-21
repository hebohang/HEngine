#include "Hepch.h"

#include "Mesh.h"
#include "MeshLoader.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Resource/ResType/Mesh.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "_generated/serializer/Mesh.gen.h"

#include <limits>
#include <stdexcept>

namespace HEngine
{
    Mesh::Mesh(const std::string& path)
    {
        mMaterial.push_back(CreateRef<Material>());
        MeshLoader::LoadModel(this, path);
    }

    void Mesh::EnsureUniqueDeformation()
    {
        if (mDeformationOwner == this) return;
        for (auto& section : mSubMeshes)
            if (section) section = CreateRef<SubMesh>(*section);
        mDeformationOwner = this;
#if WITH_EDITOR
        InvalidateEditorPickData();
#endif
    }

    void Mesh::UpdateMorphWeights(const std::map<std::string, float>& weights, bool resetHistory)
    {
        EnsureUniqueDeformation();
        for (auto& section : mSubMeshes)
        {
            if (!section || !section->HasEffectiveMorphTargets()) continue;
            section->BeginDeformation();
            section->ApplyMorphWeights(weights);
            section->EndDeformation(resetHistory);
        }
    }

    bool Mesh::IsVisible(const glm::mat4& transform, const glm::mat4& viewProjection) const
    {
        for (const Ref<SubMesh>& subMesh : mSubMeshes)
        {
            if (subMesh && subMesh->IsVisible(transform, viewProjection))
                return true;
        }
        return mSubMeshes.empty();
    }

#if WITH_EDITOR
    Ref<const MeshEditorPickData> Mesh::GetEditorPickData() const
    {
        if (!mEditorPickDataCache)
            throw std::logic_error("The mesh editor pick-data cache is unavailable.");

        std::scoped_lock lock(mEditorPickDataCache->Mutex);
        if (mEditorPickDataCache->Data)
            return mEditorPickDataCache->Data;

        Ref<MeshEditorPickData> pickData = CreateRef<MeshEditorPickData>();
        pickData->Vertices.reserve(mSubMeshes.size() * 64);
        pickData->TriangleIndices.reserve(mSubMeshes.size() * 192);
        pickData->HitRanges.reserve(mSubMeshes.size());

        for (std::uint32_t subMeshIndex = 0; subMeshIndex < mSubMeshes.size(); ++subMeshIndex)
        {
            const Ref<SubMesh>& subMesh = mSubMeshes[subMeshIndex];
            if (!subMesh || subMesh->mIndices.size() < 3)
                continue;

            if (pickData->Vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
                pickData->TriangleIndices.size() / 3 > std::numeric_limits<std::uint32_t>::max())
            {
                throw std::length_error("The mesh editor pick geometry exceeds 32-bit Embree index limits.");
            }

            const std::uint32_t vertexBase = static_cast<std::uint32_t>(pickData->Vertices.size());
            const std::uint32_t firstPrimitive =
                static_cast<std::uint32_t>(pickData->TriangleIndices.size() / 3);
            std::size_t subMeshVertexCount = 0;

            if (!subMesh->mStaticVertices.empty())
            {
                subMeshVertexCount = subMesh->mStaticVertices.size();
                if (subMeshVertexCount > std::numeric_limits<std::uint32_t>::max() - vertexBase)
                    throw std::length_error("The mesh editor pick vertex buffer exceeds 32-bit index limits.");
                for (const StaticVertex& vertex : subMesh->mStaticVertices)
                    pickData->Vertices.push_back(vertex.Pos);
            }
            else if (!subMesh->mSkinnedVertices.empty())
            {
                subMeshVertexCount = subMesh->mSkinnedVertices.size();
                if (subMeshVertexCount > std::numeric_limits<std::uint32_t>::max() - vertexBase)
                    throw std::length_error("The mesh editor pick vertex buffer exceeds 32-bit index limits.");
                for (const SkinnedVertex& vertex : subMesh->mSkinnedVertices)
                    pickData->Vertices.push_back(vertex.Pos);
            }
            else
            {
                continue;
            }

            for (std::size_t offset = 0; offset + 2 < subMesh->mIndices.size(); offset += 3)
            {
                const std::uint32_t index0 = subMesh->mIndices[offset];
                const std::uint32_t index1 = subMesh->mIndices[offset + 1];
                const std::uint32_t index2 = subMesh->mIndices[offset + 2];
                if (index0 >= subMeshVertexCount || index1 >= subMeshVertexCount || index2 >= subMeshVertexCount)
                    continue;

                pickData->TriangleIndices.push_back(index0 + vertexBase);
                pickData->TriangleIndices.push_back(index1 + vertexBase);
                pickData->TriangleIndices.push_back(index2 + vertexBase);
            }

            const std::size_t totalPrimitiveCount = pickData->TriangleIndices.size() / 3;
            if (totalPrimitiveCount > std::numeric_limits<std::uint32_t>::max())
                throw std::length_error("The mesh editor pick triangle buffer exceeds 32-bit primitive limits.");
            const std::uint32_t primitiveCount =
                static_cast<std::uint32_t>(totalPrimitiveCount) - firstPrimitive;
            if (primitiveCount == 0)
                continue;

            pickData->HitRanges.push_back(MeshHitRange{
                subMeshIndex,
                subMesh->mMaterialIndex,
                firstPrimitive,
                primitiveCount });
        }

        mEditorPickDataCache->Data = pickData;
        return pickData;
    }

    void Mesh::InvalidateEditorPickData()
    {
        if (!mEditorPickDataCache)
            mEditorPickDataCache = CreateRef<MeshEditorPickDataCache>();

        std::scoped_lock lock(mEditorPickDataCache->Mutex);
        mEditorPickDataCache->Data.reset();
    }

    bool Mesh::ResolveEmbreeHitInfoFromRanges(
        const std::vector<EmbreeSubMeshHitInfo>& hitInfos,
        uint32_t primID,
        uint32_t& outSubMeshIndex,
        uint32_t& outMaterialIndex)
    {
        return ResolveMeshHitRange(hitInfos, primID, outSubMeshIndex, outMaterialIndex);
    }

    bool Mesh::GetEmbreeHitInfo(
        uint32_t primID,
        uint32_t& outSubMeshIndex,
        uint32_t& outMaterialIndex) const
    {
        const Ref<const MeshEditorPickData> pickData = GetEditorPickData();
        return pickData && ResolveEmbreeHitInfoFromRanges(
            pickData->HitRanges,
            primID,
            outSubMeshIndex,
            outMaterialIndex);
    }
#endif
}
