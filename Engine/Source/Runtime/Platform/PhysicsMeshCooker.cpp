#include "PhysicsMeshCooker.h"
#include "PhysicsCookedMesh.h"

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/StreamWrapper.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>

#include <sstream>
#include <stdexcept>
#include <bit>
#include <cmath>
#include <vector>

namespace HEngine
{
    namespace
    {
        struct CookingContext
        {
            bool bOwner = false;
            CookingContext()
            {
                if (!JPH::Factory::sInstance)
                {
                    JPH::RegisterDefaultAllocator();
                    JPH::Factory::sInstance = new JPH::Factory();
                    JPH::RegisterTypes(); bOwner = true;
                }
            }
            ~CookingContext()
            {
                if (bOwner) { JPH::UnregisterTypes(); delete JPH::Factory::sInstance; JPH::Factory::sInstance = nullptr; }
            }
        };
        void ValidatePayload(std::span<const uint8_t> payload)
        {
            std::istringstream input(std::string(reinterpret_cast<const char*>(payload.data()), payload.size()), std::ios::binary);
            JPH::StreamInWrapper stream(input);
            JPH::Shape::IDToShapeMap shapes;
            JPH::Shape::IDToMaterialMap materials;
            auto restored = JPH::Shape::sRestoreWithChildren(stream, shapes, materials);
            if (stream.IsFailed() || restored.HasError() || restored.Get()->GetSubType() != JPH::EShapeSubType::Mesh)
                throw std::runtime_error("collision_cook: Invalid Jolt triangle payload.");
        }
    }
    std::string CookTriangleCollision(std::span<const std::array<float, 3>> vertices, bool includePortableSource)
    {
        using JPH::uint64;
        CookingContext context;
        if (vertices.empty() || vertices.size() % 3 || vertices.size() > (PhysicsCollisionFormat::MaxBytes - 32) / 12)
            throw std::runtime_error("collision_cook: Expected nonempty triangle triples.");
        for (const auto& vertex : vertices)
            for (float value : vertex)
                if (!std::isfinite(value)) throw std::runtime_error("collision_cook: Non-finite triangle vertex.");
        JPH::TriangleList triangles;
        triangles.reserve(vertices.size() / 3);
        for (size_t i = 0; i < vertices.size(); i += 3)
            triangles.emplace_back(JPH::Float3(vertices[i][0], vertices[i][1], vertices[i][2]),
                JPH::Float3(vertices[i+1][0], vertices[i+1][1], vertices[i+1][2]),
                JPH::Float3(vertices[i+2][0], vertices[i+2][1], vertices[i+2][2]));
        JPH::MeshShapeSettings settings(triangles);
        auto shape = settings.Create();
        if (shape.HasError()) throw std::runtime_error("collision_cook: Jolt rejected triangle geometry.");
        std::ostringstream output(std::ios::binary);
        JPH::StreamOutWrapper stream(output);
        JPH::Shape::ShapeToIDMap shapes; JPH::Shape::MaterialToIDMap materials;
        shape.Get()->SaveWithChildren(stream, shapes, materials);
        if (stream.IsFailed()) throw std::runtime_error("collision_cook: Cannot serialize Jolt acceleration structure.");
        const auto payload = output.str();
        ValidatePayload(std::span(reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));
        std::string cooked;
        PhysicsCollisionFormat::AppendLittle(cooked, includePortableSource ? PhysicsCollisionFormat::MagicV2 : PhysicsCollisionFormat::MagicV1, 8);
        PhysicsCollisionFormat::AppendLittle(cooked, uint64_t(JPH_VERSION_ID), 8);
        if (includePortableSource)
        {
            PhysicsCollisionFormat::AppendLittle(cooked, vertices.size(), 8);
            PhysicsCollisionFormat::AppendLittle(cooked, payload.size(), 8);
            for (const auto& vertex : vertices)
                for (float value : vertex) PhysicsCollisionFormat::AppendLittle(cooked, std::bit_cast<uint32_t>(value), 4);
        }
        cooked.append(payload);
        if (cooked.size() > PhysicsCollisionFormat::MaxBytes)
            throw std::runtime_error("collision_cook: Cooked collision exceeds size limit.");
        return cooked;
    }

    std::string CookTriangleCollisionForTarget(std::span<const uint8_t> source)
    {
        using JPH::uint64;
        const auto document = PhysicsCollisionFormat::Parse(source);
        if (!document.bPortable)
        {
            if (document.Abi != JPH_VERSION_ID)
                throw std::runtime_error("collision_cook: Legacy v1 Jolt ABI mismatch; reimport source geometry with the v2 importer before packaging.");
            CookingContext context;
            ValidatePayload(document.Payload);
            return std::string(reinterpret_cast<const char*>(source.data()), source.size());
        }
        std::vector<std::array<float, 3>> vertices(static_cast<size_t>(document.VertexCount));
        for (size_t i = 0; i < vertices.size(); ++i)
            for (size_t j = 0; j < 3; ++j)
                vertices[i][j] = std::bit_cast<float>(static_cast<uint32_t>(
                    PhysicsCollisionFormat::ReadLittle(document.Triangles, i * 12 + j * 4, 4)));
        // Do not reinterpret foreign cooked bytes, even when their header ABI happens to match.
        return CookTriangleCollision(vertices, false);
    }
}
