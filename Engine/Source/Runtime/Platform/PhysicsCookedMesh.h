#pragma once
#include "PhysicsCollisionFormat.h"
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <span>
#include <cstdint>
#include <filesystem>

namespace HEngine
{
    // Both versions require an exact runtime ABI. V2 portable geometry is tool-only input.
    inline constexpr uint64_t c_CookedMeshMagic = PhysicsCollisionFormat::MagicV1;
    JPH::RefConst<JPH::Shape> RestoreCookedTriangleMesh(std::span<const uint8_t> bytes);
    JPH::RefConst<JPH::Shape> LoadCookedTriangleMesh(const std::filesystem::path& projectRoot, const std::string& projectPath);
}
