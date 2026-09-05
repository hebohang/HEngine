#include "LLGI.ShaderWebGPU.h"
#include <cctype>
#include <cstring>
#include <string>

namespace LLGI
{
namespace
{
bool ParseAttributeIndex(const std::string& code, const char* attribute, size_t offset, uint32_t& value)
{
	const auto attributePos = code.find(attribute, offset);
	if (attributePos == std::string::npos)
	{
		return false;
	}

	const auto openPos = code.find('(', attributePos);
	if (openPos == std::string::npos)
	{
		return false;
	}

	auto digitPos = openPos + 1;
	while (digitPos < code.size() && std::isspace(static_cast<unsigned char>(code[digitPos])) != 0)
	{
		digitPos++;
	}

	if (digitPos >= code.size() || std::isdigit(static_cast<unsigned char>(code[digitPos])) == 0)
	{
		return false;
	}

	uint32_t parsed = 0;
	while (digitPos < code.size() && std::isdigit(static_cast<unsigned char>(code[digitPos])) != 0)
	{
		parsed = parsed * 10 + static_cast<uint32_t>(code[digitPos] - '0');
		digitPos++;
	}

	value = parsed;
	return true;
}

wgpu::StorageTextureAccess ParseStorageTextureAccess(const std::string& statement)
{
	if (statement.find("read_write") != std::string::npos)
	{
		return wgpu::StorageTextureAccess::ReadWrite;
	}
	if (statement.find("read") != std::string::npos)
	{
		return wgpu::StorageTextureAccess::ReadOnly;
	}
	return wgpu::StorageTextureAccess::WriteOnly;
}

wgpu::TextureFormat ParseStorageTextureFormat(const std::string& statement)
{
	if (statement.find("rgba32float") != std::string::npos)
	{
		return wgpu::TextureFormat::RGBA32Float;
	}
	if (statement.find("rgba32uint") != std::string::npos)
	{
		return wgpu::TextureFormat::RGBA32Uint;
	}
	if (statement.find("rgba32sint") != std::string::npos)
	{
		return wgpu::TextureFormat::RGBA32Sint;
	}
	if (statement.find("rgba16float") != std::string::npos)
	{
		return wgpu::TextureFormat::RGBA16Float;
	}
	if (statement.find("rgba8unorm") != std::string::npos)
	{
		return wgpu::TextureFormat::RGBA8Unorm;
	}
	if (statement.find("rgba8snorm") != std::string::npos)
	{
		return wgpu::TextureFormat::RGBA8Snorm;
	}
	if (statement.find("rgba8uint") != std::string::npos)
	{
		return wgpu::TextureFormat::RGBA8Uint;
	}
	if (statement.find("rgba8sint") != std::string::npos)
	{
		return wgpu::TextureFormat::RGBA8Sint;
	}
	if (statement.find("r32float") != std::string::npos)
	{
		return wgpu::TextureFormat::R32Float;
	}
	if (statement.find("r32uint") != std::string::npos)
	{
		return wgpu::TextureFormat::R32Uint;
	}
	if (statement.find("r32sint") != std::string::npos)
	{
		return wgpu::TextureFormat::R32Sint;
	}
	return wgpu::TextureFormat::Undefined;
}

wgpu::TextureViewDimension ParseTextureViewDimension(const std::string& statement)
{
	if (statement.find("texture_1d") != std::string::npos)
	{
		return wgpu::TextureViewDimension::e1D;
	}
	if (statement.find("texture_2d_array") != std::string::npos || statement.find("texture_storage_2d_array") != std::string::npos)
	{
		return wgpu::TextureViewDimension::e2DArray;
	}
	if (statement.find("texture_3d") != std::string::npos || statement.find("texture_storage_3d") != std::string::npos)
	{
		return wgpu::TextureViewDimension::e3D;
	}
	if (statement.find("texture_cube_array") != std::string::npos)
	{
		return wgpu::TextureViewDimension::CubeArray;
	}
	if (statement.find("texture_cube") != std::string::npos)
	{
		return wgpu::TextureViewDimension::Cube;
	}
	return wgpu::TextureViewDimension::e2D;
}

bool IsSameBinding(const ShaderBindingWebGPU& lhs, const ShaderBindingWebGPU& rhs)
{
	if (lhs.Group != rhs.Group || lhs.Binding != rhs.Binding ||
		lhs.Resource.ResourceType != rhs.Resource.ResourceType ||
		lhs.Resource.Access != rhs.Resource.Access)
	{
		return false;
	}

	if ((lhs.Resource.ResourceType == ShaderResourceType::Texture ||
		 lhs.Resource.ResourceType == ShaderResourceType::StorageTexture) &&
		lhs.TextureViewDimension != rhs.TextureViewDimension)
	{
		return false;
	}

	if (lhs.Resource.ResourceType == ShaderResourceType::Texture)
	{
		return lhs.TextureSampleType == rhs.TextureSampleType;
	}

	if (lhs.Resource.ResourceType == ShaderResourceType::StorageTexture)
	{
		return lhs.StorageTextureFormat == rhs.StorageTextureFormat &&
			   lhs.StorageTextureAccess == rhs.StorageTextureAccess;
	}

	return true;
}

std::vector<ShaderBindingWebGPU> ReflectBindings(const std::string& code)
{
	std::vector<ShaderBindingWebGPU> bindings;

	size_t offset = 0;
	while (true)
	{
		const auto groupPos = code.find("@group", offset);
		if (groupPos == std::string::npos)
		{
			break;
		}

		uint32_t group = 0;
		uint32_t binding = 0;
		if (ParseAttributeIndex(code, "@group", groupPos, group) &&
			ParseAttributeIndex(code, "@binding", groupPos, binding))
		{
			const auto statementEnd = code.find(';', groupPos);
			const auto statementLength = statementEnd == std::string::npos ? std::string::npos : statementEnd - groupPos;
			const auto statement = code.substr(groupPos, statementLength);
			ShaderBindingWebGPU reflected;
			reflected.Group = group;
			reflected.Binding = binding;
			reflected.Resource.Group = static_cast<int32_t>(group);
			reflected.Resource.Slot = static_cast<int32_t>(binding);
			if (statement.find("var<uniform>") != std::string::npos)
			{
				reflected.Resource.ResourceType = ShaderResourceType::UniformBuffer;
				reflected.Resource.Access = ShaderResourceAccess::ReadOnly;
			}
			else if (statement.find("var<storage") != std::string::npos)
			{
				reflected.Resource.ResourceType = ShaderResourceType::StorageBuffer;
				if (statement.find("read_write") != std::string::npos)
				{
					reflected.Resource.Access = ShaderResourceAccess::ReadWrite;
				}
				else
				{
					reflected.Resource.Access = ShaderResourceAccess::ReadOnly;
				}
			}
			else if (statement.find(": texture_storage_") != std::string::npos)
			{
				reflected.Resource.ResourceType = ShaderResourceType::StorageTexture;
				reflected.TextureViewDimension = ParseTextureViewDimension(statement);
				reflected.StorageTextureFormat = ParseStorageTextureFormat(statement);
				reflected.StorageTextureAccess = ParseStorageTextureAccess(statement);
				reflected.Resource.Access =
					reflected.StorageTextureAccess == wgpu::StorageTextureAccess::ReadOnly ? ShaderResourceAccess::ReadOnly : ShaderResourceAccess::ReadWrite;
			}
			else if (statement.find(": texture_") != std::string::npos)
			{
				reflected.Resource.ResourceType = ShaderResourceType::Texture;
				reflected.Resource.Access = ShaderResourceAccess::ReadOnly;
				reflected.TextureViewDimension = ParseTextureViewDimension(statement);
				reflected.TextureSampleType =
					statement.find(": texture_depth_") != std::string::npos ? wgpu::TextureSampleType::Depth : wgpu::TextureSampleType::Float;
			}
			else if (statement.find(": sampler") != std::string::npos)
			{
				reflected.Resource.ResourceType = ShaderResourceType::Sampler;
				reflected.Resource.Access = ShaderResourceAccess::ReadOnly;
			}

			bool exists = false;
			for (const auto& b : bindings)
			{
				if (IsSameBinding(b, reflected))
				{
					exists = true;
					break;
				}
			}

			if (!exists)
			{
				bindings.push_back(reflected);
			}
		}

		offset = groupPos + 6;
	}

	return bindings;
}
} // namespace

ShaderWebGPU::ShaderWebGPU() {}

ShaderWebGPU::~ShaderWebGPU() {}

bool ShaderWebGPU::Initialize(wgpu::Device& device, DataStructure* data, int32_t count)
{
	static const char wgslHeader[] = {'w', 'g', 's', 'l', 'c', 'o', 'd', 'e'};

	if (data == nullptr || count == 0)
	{
		return false;
	}

	wgpu::ShaderModuleDescriptor desc = {};

	if (data[0].Data == nullptr || data[0].Size <= 0)
	{
		return false;
	}

	const auto* bytes = static_cast<const uint8_t*>(data[0].Data);
	const bool hasWGSLHeader = data[0].Size >= static_cast<int32_t>(sizeof(wgslHeader)) &&
							   memcmp(bytes, wgslHeader, sizeof(wgslHeader)) == 0;
	const bool hasSPIRVMagic = data[0].Size >= 4 && bytes[0] == 0x03 && bytes[1] == 0x02 && bytes[2] == 0x23 && bytes[3] == 0x07;

	wgpu::ShaderSourceSPIRV sprivDesc = {};
	wgpu::ShaderSourceWGSL wgslDesc = {};
	std::string wgslCode;

	if (!hasSPIRVMagic)
	{
		const auto codeOffset = hasWGSLHeader ? sizeof(wgslHeader) : 0;
		if (data[0].Size <= static_cast<int32_t>(codeOffset))
		{
			return false;
		}

		wgslCode.assign(reinterpret_cast<const char*>(bytes + codeOffset), static_cast<size_t>(data[0].Size) - codeOffset);
		while (!wgslCode.empty() && wgslCode.back() == '\0')
		{
			wgslCode.pop_back();
		}
		wgslDesc.code = wgpu::StringView(wgslCode.data(), wgslCode.size());
		desc.nextInChain = reinterpret_cast<wgpu::ChainedStruct*>(&wgslDesc);
		bindings_ = ReflectBindings(wgslCode);
		hasBindingReflection_ = true;
	}
	else
	{
		sprivDesc.codeSize = data[0].Size / sizeof(uint32_t);
		sprivDesc.code = reinterpret_cast<const uint32_t*>(data[0].Data);
		desc.nextInChain = reinterpret_cast<wgpu::ChainedStruct*>(&sprivDesc);
	}

	shaderModule_ = device.CreateShaderModule(&desc);

	return shaderModule_ != nullptr;
}

wgpu::ShaderModule& ShaderWebGPU::GetShaderModule() { return shaderModule_; }

const std::vector<ShaderBindingWebGPU>& ShaderWebGPU::GetBindings() const { return bindings_; }

bool ShaderWebGPU::HasBindingReflection() const { return hasBindingReflection_; }

} // namespace LLGI
