
#pragma once

#include "LLGI.Base.h"

namespace LLGI
{

enum class ShaderResourceType
{
	Unknown,
	UniformBuffer,
	Texture,
	Sampler,
	StorageBuffer,
	StorageTexture,
};

enum class ShaderResourceAccess
{
	ReadOnly,
	ReadWrite,
};

enum class StorageBufferViewType
{
	Structured,
	Raw,
};

struct ShaderResourceBinding
{
	ShaderResourceType ResourceType = ShaderResourceType::Unknown;
	ShaderResourceAccess Access = ShaderResourceAccess::ReadWrite;
	StorageBufferViewType StorageBufferView = StorageBufferViewType::Structured;
	ShaderStageType Stage = ShaderStageType::Max;
	int32_t Group = -1;
	int32_t Slot = 0;
	int32_t ElementStride = 0;
};

inline bool IsReadOnly(ShaderResourceAccess access)
{
	return access == ShaderResourceAccess::ReadOnly;
}

class Shader : public ReferenceObject
{
private:
public:
	Shader() = default;
	~Shader() override = default;
};

} // namespace LLGI
