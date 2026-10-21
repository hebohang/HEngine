// Included inside SceneRenderer::Impl. Authored index LODs share one resident
// vertex/index allocation and the selection is reused by every raster pass.
struct MeshLodHistory
{
    const SubMesh* Mesh = nullptr;
    uint32_t Level = 0;
};
std::unordered_map<uint64_t, MeshLodHistory> MeshLodHistoryBySection;

uint32_t SelectMeshLod(const MeshEntry& mesh, const RenderObject& object,
    const RenderMeshSection& section, const RenderCamera& camera, uint32_t sectionIndex)
{
    // Generic authored errors do not bound deformation, skinning or alpha
    // coverage. Those materials retain their full geometry unless a future
    // resource version supplies the corresponding conservative error bounds.
    if (!bLodEnabled || mesh.Lods.size() < 2 || mesh.bSkinned ||
        (section.GetMaterialInstance().bCustomSurface &&
            section.GetMaterialInstance().ExpectedMaterialAbiVersion != MaterialShaderAbiVersion) || section.AlphaMode != RenderMaterialAlphaMode::Opaque ||
        !mesh.Resource->HasBounds() || LastHeight == 0)
        return 0;

    const glm::vec3 localCenter = (mesh.Resource->GetBoundsMin() + mesh.Resource->GetBoundsMax()) * 0.5f;
    const glm::vec3 localExtents = (mesh.Resource->GetBoundsMax() - mesh.Resource->GetBoundsMin()) * 0.5f;
    const glm::mat3 linear(object.WorldTransform);
    // sqrt(||M||1 * ||M||infinity) bounds the largest singular value,
    // including parent-induced shear; max column length alone is unsafe.
    float normOne = 0.0f, normInfinity = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        float column = 0.0f, row = 0.0f;
        for (int j = 0; j < 3; ++j)
        {
            column += std::abs(linear[i][j]);
            row += std::abs(linear[j][i]);
        }
        normOne = std::max(normOne, column);
        normInfinity = std::max(normInfinity, row);
    }
    const float scale = std::sqrt(normOne * normInfinity);
    const glm::vec3 center = glm::vec3(camera.View * object.WorldTransform * glm::vec4(localCenter, 1.0f));
    const float radius = glm::length(localExtents) * scale;
    const bool bPerspective = std::abs(camera.Projection[2][3]) > 0.5f;
    const float closestDepth = -center.z - radius;
    if (!std::isfinite(scale) || !std::isfinite(closestDepth) ||
        (bPerspective && closestDepth <= camera.NearPlane))
        return 0;
    float pixelScale = 0.5f * scale * std::max(
        float(LastOutputWidth) * std::abs(camera.Projection[0][0]),
        float(LastOutputHeight) * std::abs(camera.Projection[1][1]));
    // The public error budget is measured on the display. Internal resolution
    // scaling must not silently double the visible LOD error on a 4K screen.
    if (bPerspective)
    {
        // The off-axis factor also bounds depth displacement in projection.
        const float offAxis = (std::max(std::abs(center.x), std::abs(center.y)) + radius) / closestDepth;
        pixelScale *= (1.0f + offAxis) / closestDepth;
    }
    if (!std::isfinite(pixelScale)) return 0;
    const uint64_t key = (uint64_t(object.ObjectGpuIndex) << 32u) | sectionIndex;
    auto& history = MeshLodHistoryBySection[key];
    uint32_t previous = history.Mesh == mesh.Resource.get() && !camera.bCameraCut ? history.Level : 0;
    uint32_t selected = 0;
    for (uint32_t level = 1; level < mesh.Lods.size(); ++level)
    {
        // Enter a coarser level below 80% of the public error limit. Retain it
        // only within the limit, so hysteresis never exceeds stated quality.
        const float limit = LodMaxPixelError * (level <= previous ? 1.0f : 0.8f);
        if (mesh.Lods[level].ObjectSpaceError * pixelScale > limit) break;
        selected = level;
    }
    history = { mesh.Resource.get(), selected };
    return selected;
}

static nvrhi::DrawArguments MeshDrawArguments(const MeshEntry& mesh, uint32_t lodIndex)
{
    const auto& range = mesh.Lods.at(lodIndex);
    return nvrhi::DrawArguments().setVertexCount(range.IndexCount).setStartIndexLocation(range.FirstIndex);
}
