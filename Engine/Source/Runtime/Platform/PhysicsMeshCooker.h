#pragma once
#include <array>
#include <span>
#include <string>
#include <cstdint>

namespace HEngine
{
    // Tool-only implementation, compiled by Importers and the target-configuration HCooker.
    // Consecutive triples are triangles, in engine local coordinates/metres.
    std::string CookTriangleCollision(std::span<const std::array<float, 3>> vertices, bool includePortableSource = true);
    // V2 always recooks portable geometry. Legacy V1 is accepted only with an exact ABI.
    std::string CookTriangleCollisionForTarget(std::span<const uint8_t> source);
}
