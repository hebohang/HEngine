#pragma once

#include <cstdint>

namespace Effekseer
{
namespace Backend
{
class GraphicsDevice;
}
}

namespace EffekseerRendererLLGI
{
namespace Backend
{
// Owner-thread snapshot of buffers held by registered storage/uniform objects.
// Logical GetSize bytes only: excludes native padding, textures, vertex/index,
// temporary command-only uploads and frame-memory pools. No lifecycle changes.
struct BufferPoolStats
{
	bool Available = false;
	std::uint64_t StorageBufferCount = 0;
	std::uint64_t StorageLogicalBytes = 0;
	std::uint64_t UniformBufferCount = 0;
	std::uint64_t UniformLogicalBytes = 0;
	std::uint64_t UniformSnapshotCount = 0;
};

// Null or non-LLGI devices return an unavailable, zero-initialized snapshot.
BufferPoolStats GetBufferPoolStats(const Effekseer::Backend::GraphicsDevice* graphicsDevice);
}
}
