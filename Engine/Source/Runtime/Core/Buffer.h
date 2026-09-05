#pragma once

#include "Base/Base.h"

#include <span>
#include <cstddef>

namespace HEngine
{
	struct Buffer
	{
		std::byte* Data = nullptr;
		uint32_t Size = 0;

		Buffer() = default;
		Buffer(const Buffer&) = delete;
		Buffer& operator=(const Buffer&) = delete;
		Buffer(Buffer&& buffer) noexcept
			: Data(buffer.Data), Size(buffer.Size)
		{
			buffer.Reset();
		}
		Buffer& operator=(Buffer&& buffer) noexcept
		{
			Data = buffer.Data;
			Size = buffer.Size;
			buffer.Reset();
			return *this;
		}
		Buffer(void* _Data, uint32_t _Size)
			: Data(reinterpret_cast<std::byte*>(_Data)), Size(_Size)
		{
		}
		~Buffer() = default;

		operator std::span<const std::byte>() const 
		{ 
			return std::as_bytes(std::span{ reinterpret_cast<const std::byte*>(Data), Size });
		}

		void Release()
		{
			delete[] Data;
			Data = nullptr;
			Size = 0;
		}

		// must ensure the data has been deleted
		void Reset()
		{
			Data = nullptr;
			Size = 0;
		}

		std::byte& operator[](uint32_t index) { return Data[index]; }
		std::byte operator[](uint32_t index) const { return Data[index]; }

		template<typename T>
		T& Get(uint32_t index = 0)
		{
			HE_CORE_ASSERT(sizeof(T) * (index + 1) <= sizeof(std::byte) * Size);
			return ((T*)Data)[index];
		}

		void Write(void* data, uint32_t size, uint32_t offset = 0)
		{
			HE_CORE_ASSERT(offset + size <= Size);
			std::memcpy(Data + offset, data, size);
		}

		void Move(void* data, uint32_t size)
		{
			Data = reinterpret_cast<std::byte*>(data);
			Size = size;
			data = nullptr;
		}

		template<typename T>
		T* As()
		{
			return (T*)Data;
		}

		void* GetRawBuffer()
		{
			return (void*)Data;
		}
	};
}
