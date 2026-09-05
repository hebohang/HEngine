#include <cstdint>
#include <cstdio>
#include <cstring>
#include <d3dcompiler.h>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

namespace
{

const char* shader = R"(
Texture2D<float4> SrcTexture : register(t0);
SamplerState SrcSampler : register(s0);

struct VSOutput
{
	float4 Position : SV_Position;
	float2 UV : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
	float2 positions[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
	float2 uvs[3] = { float2(0.0, 1.0), float2(0.0, -1.0), float2(2.0, 1.0) };
	VSOutput output;
	output.Position = float4(positions[vertexId], 0.0, 1.0);
	output.UV = uvs[vertexId];
	return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
	return SrcTexture.Sample(SrcSampler, input.UV);
}
)";

void SafeRelease(IUnknown* object)
{
	if (object != nullptr)
	{
		object->Release();
	}
}

bool Compile(std::vector<uint8_t>& binary, const char* entryPoint, const char* target)
{
	ID3DBlob* shaderBlob = nullptr;
	ID3DBlob* errorBlob = nullptr;
	const auto hr = D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, entryPoint, target, 0, 0, &shaderBlob, &errorBlob);
	if (FAILED(hr))
	{
		if (errorBlob != nullptr)
		{
			std::fprintf(stderr, "%s\n", static_cast<const char*>(errorBlob->GetBufferPointer()));
		}
		SafeRelease(shaderBlob);
		SafeRelease(errorBlob);
		return false;
	}

	const auto* data = static_cast<const uint8_t*>(shaderBlob->GetBufferPointer());
	binary.assign(data, data + shaderBlob->GetBufferSize());
	SafeRelease(shaderBlob);
	SafeRelease(errorBlob);
	return true;
}

void WriteArray(std::ofstream& ofs, const char* name, const std::vector<uint8_t>& binary)
{
	ofs << "static const uint8_t " << name << "[] = {\n";
	for (size_t i = 0; i < binary.size(); i++)
	{
		if (i % 12 == 0)
		{
			ofs << "\t";
		}

		ofs << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(binary[i]) << std::dec;
		if (i + 1 < binary.size())
		{
			ofs << ", ";
		}

		if (i % 12 == 11 || i + 1 == binary.size())
		{
			ofs << "\n";
		}
	}
	ofs << "};\n";
	ofs << "static const size_t " << name << "Size = sizeof(" << name << ");\n\n";
}

} // namespace

int main(int argc, char* argv[])
{
	if (argc != 2)
	{
		std::fprintf(stderr, "Usage: %s <output-header>\n", argv[0]);
		return 1;
	}

	std::vector<uint8_t> vertexShader;
	std::vector<uint8_t> pixelShader;
	if (!Compile(vertexShader, "VSMain", "vs_5_0") || !Compile(pixelShader, "PSMain", "ps_5_0"))
	{
		return 1;
	}

	std::ofstream ofs(argv[1], std::ios::binary);
	if (!ofs)
	{
		std::fprintf(stderr, "Failed to open %s\n", argv[1]);
		return 1;
	}

	ofs << "#pragma once\n\n";
	ofs << "#include <cstddef>\n";
	ofs << "#include <cstdint>\n\n";
	ofs << "namespace LLGI\n";
	ofs << "{\n";
	ofs << "namespace DX12MipmapShader\n";
	ofs << "{\n\n";
	WriteArray(ofs, "VertexShader", vertexShader);
	WriteArray(ofs, "PixelShader", pixelShader);
	ofs << "} // namespace DX12MipmapShader\n";
	ofs << "} // namespace LLGI\n";

	return 0;
}
