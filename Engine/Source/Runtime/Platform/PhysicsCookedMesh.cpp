#include "Hepch.h"
#include "PhysicsCookedMesh.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include <Jolt/Core/StreamWrapper.h>
#include <sstream>
#include <fstream>
#include <stdexcept>

namespace HEngine
{
    JPH::RefConst<JPH::Shape> LoadCookedTriangleMesh(const std::filesystem::path& projectRoot, const std::string& projectPath)
    {
        std::filesystem::path path; std::string normalized;
        if (!Utils::TryResolveProjectRelativePath(projectRoot, projectPath, path, normalized) || path.extension() != ".hcollision")
            throw std::runtime_error("Invalid cooked collision project path.");
        const auto size = std::filesystem::file_size(path);
        if (size < 16 || size > 128u * 1024u * 1024u) throw std::runtime_error("Cooked collision size limit exceeded.");
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        std::ifstream input(path, std::ios::binary);
        input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        if (!input) throw std::runtime_error("Cannot read cooked collision.");
        return RestoreCookedTriangleMesh(bytes);
    }
    JPH::RefConst<JPH::Shape> RestoreCookedTriangleMesh(std::span<const uint8_t> bytes)
    {
        using JPH::uint64; // JPH_VERSION_ID uses this library typedef internally.
        const auto document = PhysicsCollisionFormat::Parse(bytes);
        if (document.Abi != JPH_VERSION_ID)
            throw std::runtime_error("Cooked collision Jolt ABI mismatch; recook the package or reimport the source.");
        std::istringstream input(std::string(reinterpret_cast<const char*>(document.Payload.data()), document.Payload.size()), std::ios::binary);
        JPH::StreamInWrapper stream(input);
        JPH::Shape::IDToShapeMap shapes;
        JPH::Shape::IDToMaterialMap materials;
        auto result = JPH::Shape::sRestoreWithChildren(stream, shapes, materials);
        if (stream.IsFailed() || result.HasError() || result.Get()->GetSubType() != JPH::EShapeSubType::Mesh)
            throw std::runtime_error("Invalid cooked triangle collision payload.");
        return result.Get();
    }
}
