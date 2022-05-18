#include "hepch.h"

#include "Runtime/Renderer/Material.h"

namespace HEngine
{
    void Material::Initialize()
    {
		unsigned char data[4];

		for (size_t i = 0; i < 3; i++)
		{
			data[i] = (unsigned char)(metallic * 255.0f);
		}
		data[3] = (unsigned char)255.0f;
		metallicRGBA->SetData(data, sizeof(unsigned char) * 4);

		for (size_t i = 0; i < 3; i++)
		{
			data[i] = (unsigned char)(roughness * 255.0f);
		}
		data[3] = (unsigned char)255.0f;
		roughnessRGBA->SetData(data, sizeof(unsigned char) * 4);
    }
}