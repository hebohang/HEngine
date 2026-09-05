#include "Hepch.h"

#include "Image.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <stb_image.h>

#include <fstream>
#include <limits>
#include <vector>

namespace HEngine
{
    Image::Image(const std::filesystem::path& path, bool flip)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        std::streamoff fileSize = -1;
        if (stream)
            fileSize = static_cast<std::streamoff>(stream.tellg());
        if (fileSize <= 0 || fileSize > std::numeric_limits<int>::max())
        {
            HE_CORE_ERROR("Load Image Failed: {}", Utils::PathToUtf8(path));
            return;
        }

        std::vector<stbi_uc> fileBytes(static_cast<std::size_t>(fileSize));
        stream.seekg(0, std::ios::beg);
        if (!stream.read(
                reinterpret_cast<char*>(fileBytes.data()),
                static_cast<std::streamsize>(fileSize)))
        {
            HE_CORE_ERROR("Load Image Failed: {}", Utils::PathToUtf8(path));
            return;
        }

        int w, h, c;
        stbi_set_flip_vertically_on_load(static_cast<int>(flip));
        void* data;
        const int desiredChannels = STBI_rgb_alpha;
        const int byteCount = static_cast<int>(fileBytes.size());
        if (stbi_is_hdr_from_memory(fileBytes.data(), byteCount))
        {
            data = stbi_loadf_from_memory(fileBytes.data(), byteCount, &w, &h, &c, desiredChannels);
            c = desiredChannels;
            format = EResourceFormat::R32G32B32A32_FLOAT;
        }
		else
		{
			data = stbi_load_from_memory(fileBytes.data(), byteCount, &w, &h, &c, desiredChannels);
			c = desiredChannels;
			format = EResourceFormat::R8G8B8A8_UNORM;
		}

		if (!data)
		{
			HE_CORE_ERROR("Load Image Failed: {}", Utils::PathToUtf8(path));
			width = 0;
			height = 0;
			format = EResourceFormat::UNKNOWN;
			return;
		}

        width = w;
        height = h;
        const uint32_t elementSize = format == EResourceFormat::R32G32B32A32_FLOAT ? sizeof(float) : sizeof(stbi_uc);
        buffer.Move(data, width * height * c * elementSize);
    }

    Image::~Image()
    {
        stbi_image_free(buffer.GetRawBuffer());
        buffer.Reset();
    }
} 
