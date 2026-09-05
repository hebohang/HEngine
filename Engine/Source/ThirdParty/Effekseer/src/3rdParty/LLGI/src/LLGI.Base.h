
#pragma once

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <vector>

namespace LLGI
{

static const int RenderTargetMax = 8;
static const int VertexLayoutMax = 16;
static const int TextureSlotMax = 8;

enum class DeviceType
{
	Default,
	DirectX12,
	Metal,
	Vulkan,
	WebGPU,
};

enum class ErrorCode
{
	OK,
	Failed,
};

enum class ShaderStageType
{
	Vertex,
	Pixel,
	Compute,
	Max,
};

enum class CullingMode
{
	Clockwise,
	CounterClockwise,
	DoubleSide,
};

enum class BlendEquationType
{
	Add,
	Sub,
	ReverseSub,
	Min,
	Max,
};

enum class BlendFuncType
{
	Zero,
	One,
	SrcColor,
	OneMinusSrcColor,
	SrcAlpha,
	OneMinusSrcAlpha,
	DstAlpha,
	OneMinusDstAlpha,
	DstColor,
	OneMinusDstColor,
};

enum class VertexLayoutFormat
{
	R32G32B32_FLOAT,
	R32G32B32A32_FLOAT,
	R8G8B8A8_UNORM,
	R8G8B8A8_UINT,
	R32G32_FLOAT,
	R32_FLOAT,
	R16G16_UNORM,
};

enum class TopologyType
{
	Triangle,
	Line,
	Point,
};

enum class TextureWrapMode
{
	Clamp,
	Repeat,
	Mirror,
};

enum class TextureMinMagFilter
{
	Nearest,
	Linear,
};

enum class DepthFuncType
{
	Never,
	Less,
	Equal,
	LessEqual,
	Greater,
	NotEqual,
	GreaterEqual,
	Always,
};

enum class CompareFuncType
{
	Never,
	Less,
	Equal,
	LessEqual,
	Greater,
	NotEqual,
	GreaterEqual,
	Always,
};

enum class StencilOperatorType
{
	Keep,
	Zero,
	Replace,
	IncClamp,
	DecClamp,
	Invert,
	IncRepeat,
	DecRepeat,
};

enum class ConstantBufferType
{
	LongTime,  //! this constant buffer is not almost changed
	ShortTime, //! this constant buffer is disposed or rewrite by a frame. If shorttime, this constant buffer must be disposed by a frame.
};

struct Vec2I
{
	int32_t X;
	int32_t Y;

	Vec2I() : X(0), Y(0) {}

	Vec2I(int32_t x, int32_t y) : X(x), Y(y) {}

	bool operator==(const Vec2I& o) const { return X == o.X && Y == o.Y; }

	bool operator!=(const Vec2I& o) const { return !(*this == o); }
};

struct Vec2F
{
	float X;
	float Y;

	Vec2F() : X(0), Y(0) {}

	Vec2F(float x, float y) : X(x), Y(y) {}
};

struct Vec3I
{
	int32_t X;
	int32_t Y;
	int32_t Z;

	Vec3I() : X(0), Y(0), Z(0) {}

	Vec3I(int32_t x, int32_t y, int32_t z) : X(x), Y(y), Z(z) {}

	int32_t& operator[](int i) { return reinterpret_cast<int32_t*>(this)[i]; }

	const int32_t& operator[](int i) const { return reinterpret_cast<const int32_t*>(this)[i]; }

	bool operator==(const Vec3I& o) const { return X == o.X && Y == o.Y && Z == o.Z; }

	bool operator!=(const Vec3I& o) const { return !(*this == o); }
};

struct Vec3F
{
	float X;
	float Y;
	float Z;

	Vec3F() : X(0), Y(0), Z(0) {}

	Vec3F(float x, float y, float z) : X(x), Y(y), Z(z) {}

	static Vec3F Normalize(const Vec3F& in)
	{
		float inv = 1.0f / std::sqrt(in.X * in.X + in.Y * in.Y + in.Z * in.Z);
		return Vec3F(in.X * inv, in.Y * inv, in.Z * inv);
	}

	static Vec3F Cross(const Vec3F& in1, const Vec3F& in2)
	{
		float x = in1.Y * in2.Z - in1.Z * in2.Y;
		float y = in1.Z * in2.X - in1.X * in2.Z;
		float z = in1.X * in2.Y - in1.Y * in2.X;
		return Vec3F(x, y, z);
	}

	static Vec3F Sub(const Vec3F& in1, const Vec3F& in2) { return Vec3F(in1.X - in2.X, in1.Y - in2.Y, in1.Z - in2.Z); }

	static float Dot(const Vec3F& in1, const Vec3F& in2) { return in1.X * in2.X + in1.Y * in2.Y + in1.Z * in2.Z; }
};

inline Vec3F operator*(const Vec3F& v1, float v2) { return Vec3F(v1.X * v2, v1.Y * v2, v1.Z * v2); }

struct Color8
{
	uint8_t R;
	uint8_t G;
	uint8_t B;
	uint8_t A;

	Color8() : R(255), G(255), B(255), A(255) {}

	Color8(uint8_t r, uint8_t g, uint8_t b, uint8_t a) : R(r), G(g), B(b), A(a) {}
};

struct ColorF
{
	float R;
	float G;
	float B;
	float A;

	ColorF() : R(1.0f), G(1.0f), B(1.0f), A(1.0f) {}

	ColorF(float r, float g, float b, float a) : R(r), G(g), B(b), A(a) {}
};

enum class TextureFormatType
{
	R8G8B8A8_UNORM,
	B8G8R8A8_UNORM,
	R8_UNORM,
	R16_FLOAT,
	R32_FLOAT,
	R16G16_FLOAT,
	R32G32_FLOAT,
	R16G16B16A16_FLOAT,
	R32G32B32A32_FLOAT,
	BC1,
	BC2,
	BC3,
	BC7,
	R8G8B8A8_UNORM_SRGB,
	B8G8R8A8_UNORM_SRGB,
	BC1_SRGB,
	BC2_SRGB,
	BC3_SRGB,
	BC7_SRGB,
	D32,
	D24S8,
	D32S8,
	RG11B10_UFLOAT,
	Unknown,
};

enum class TextureUsageType : uint32_t
{
	NoneFlag = 0,
	RenderTarget = 1 << 0,
	Array = 1 << 1,
	Storage = 1 << 2,
	External = 1 << 3,
};

inline TextureUsageType operator|(TextureUsageType lhs, TextureUsageType rhs)
{
	return static_cast<TextureUsageType>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

inline TextureUsageType operator&(TextureUsageType lhs, TextureUsageType rhs)
{
	return static_cast<TextureUsageType>(static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
}

enum class BufferUsageType : uint32_t
{
	Index = 1 << 0,
	Vertex = 1 << 1,
	Constant = 1 << 2,
	StorageRead = 1 << 3,
	StorageWrite = 1 << 4,
	MapRead = 1 << 5,
	MapWrite = 1 << 6,
	CopySrc = 1 << 7,
	CopyDst = 1 << 8,
};

inline BufferUsageType operator|(BufferUsageType lhs, BufferUsageType rhs)
{
	return static_cast<BufferUsageType>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

inline BufferUsageType operator&(BufferUsageType lhs, BufferUsageType rhs)
{
	return static_cast<BufferUsageType>(static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
}

inline bool IsDepthFormat(TextureFormatType format)
{
	if (format == TextureFormatType::D32)
		return true;

	if (format == TextureFormatType::D24S8)
		return true;

	if (format == TextureFormatType::D32S8)
		return true;

	return false;
}

inline bool HasStencil(TextureFormatType format)
{
	if (format == TextureFormatType::D24S8)
		return true;

	if (format == TextureFormatType::D32S8)
		return true;

	return false;
}

enum class TextureType
{
	Screen,
	Color,
	Depth,
	Render,
	Cube,
	Unknown,
};

struct DataStructure
{
	const void* Data;
	int32_t Size;
};

enum class QueryType
{
	Timestamp,
	Occulusion,
};

template <class T> void SafeAddRef(T& t)
{
	if (t != NULL)
	{
		t->AddRef();
	}
}

template <class T> void SafeRelease(T& t)
{
	if (t != NULL)
	{
		t->Release();
		t = NULL;
	}
}

template <class T> void SafeAssign(T& t, T value)
{
	SafeAddRef(value);
	SafeRelease(t);
	t = value;
}

template <class T> void SafeDelete(T& t)
{
	if (t != NULL)
	{
		delete t;
		t = NULL;
	}
}

template <typename T> bool BitwiseContains(T value, T elm) { return (value & elm) == elm; }

class ReferenceObject
{
private:
	mutable std::atomic<int32_t> reference;

public:
	ReferenceObject() : reference(1) {}

	virtual ~ReferenceObject() {}

	int AddRef()
	{
		std::atomic_fetch_add_explicit(&reference, 1, std::memory_order_consume);
		return reference;
	}

	int GetRef() { return reference; }

	int Release()
	{
		assert(reference > 0);

		bool destroy = std::atomic_fetch_sub_explicit(&reference, 1, std::memory_order_consume) == 1;
		if (destroy)
		{
			delete this;
			return 0;
		}

		return reference;
	}
};

template <typename T> struct ReferenceDeleter
{
	void operator()(T* p)
	{
		auto p_ = (p);
		SafeRelease(p_);
	}
};

template <typename T> static std::shared_ptr<T> CreateSharedPtr(T* p, bool addRef = false)
{
	if (addRef)
	{
		SafeAddRef(p);
	}

	return std::shared_ptr<T>(p, ReferenceDeleter<T>());
}

template <typename T> inline std::unique_ptr<T, ReferenceDeleter<T>> CreateUniqueReference(T* ptr, bool addRef = false)
{
	if (ptr == nullptr)
		return std::unique_ptr<T, ReferenceDeleter<T>>(nullptr);

	if (addRef)
	{
		ptr->AddRef();
	}

	return std::unique_ptr<T, ReferenceDeleter<T>>(ptr);
}

template <typename T> using unique_ref = std::unique_ptr<T, ReferenceDeleter<T>>;

class Buffer;
class Shader;
class PipelineState;
class Texture;
class Platform;
class Graphics;
class CommandList;
class Compiler;
class RenderPass;
class RenderPassPipelineState;
class Query;

enum class LogType
{
	Info,
	Warning,
	Error,
	Debug,
};

void SetLogger(const std::function<void(LogType, const std::string&)>& logger);

void Log(LogType logType, const std::string& message);

inline size_t GetAlignedSize(size_t size, size_t alignment) { return (size + (alignment - 1)) & ~(alignment - 1); }

inline std::string to_string(const Vec2I& value)
{
	return std::string("(") + std::to_string(value.X) + ", " + std::to_string(value.Y) + ")";
}

inline std::string to_string(const Vec3I& value)
{
	return std::string("(") + std::to_string(value.X) + ", " + std::to_string(value.Y) + ", " + std::to_string(value.Z) + ")";
}

inline std::string to_string(TopologyType topology)
{
	switch (topology)
	{
	case TopologyType::Triangle:
		return "Triangle";
	case TopologyType::Line:
		return "Line";
	case TopologyType::Point:
		return "Point";
	default:
		return "Unregistered";
	}
}

inline int32_t GetIndexCountPerPrimitive(TopologyType topology)
{
	switch (topology)
	{
	case TopologyType::Triangle:
		return 3;
	case TopologyType::Line:
		return 2;
	case TopologyType::Point:
		return 1;
	default:
		return 0;
	}
}

inline std::string to_string(TextureFormatType format)
{
	switch (format)
	{
	case TextureFormatType::R8G8B8A8_UNORM:
		return "R8G8B8A8_UNORM";
	case TextureFormatType::B8G8R8A8_UNORM:
		return "B8G8R8A8_UNORM";
	case TextureFormatType::R8_UNORM:
		return "R8_UNORM";
	case TextureFormatType::R16_FLOAT:
		return "R16_FLOAT";
	case TextureFormatType::R32_FLOAT:
		return "R32_FLOAT";
	case TextureFormatType::R16G16_FLOAT:
		return "R16G16_FLOAT";
	case TextureFormatType::R32G32_FLOAT:
		return "R32G32_FLOAT";
	case TextureFormatType::R16G16B16A16_FLOAT:
		return "R16G16B16A16_FLOAT";
	case TextureFormatType::R32G32B32A32_FLOAT:
		return "R32G32B32A32_FLOAT";
	case TextureFormatType::BC1:
		return "BC1";
	case TextureFormatType::BC2:
		return "BC2";
	case TextureFormatType::BC3:
		return "BC3";
	case TextureFormatType::BC7:
		return "BC7";
	case TextureFormatType::R8G8B8A8_UNORM_SRGB:
		return "R8G8B8A8_UNORM_SRGB";
	case TextureFormatType::B8G8R8A8_UNORM_SRGB:
		return "B8G8R8A8_UNORM_SRGB";
	case TextureFormatType::BC1_SRGB:
		return "BC1_SRGB";
	case TextureFormatType::BC2_SRGB:
		return "BC2_SRGB";
	case TextureFormatType::BC3_SRGB:
		return "BC3_SRGB";
	case TextureFormatType::BC7_SRGB:
		return "BC7_SRGB";
	case TextureFormatType::D32:
		return "D32";
	case TextureFormatType::D32S8:
		return "D32S8";
	case TextureFormatType::D24S8:
		return "D24S8";
	case TextureFormatType::RG11B10_UFLOAT:
		return "RG11B10_UFLOAT";
	default:
		return "Unregistered";
	}
}

inline std::string to_string(TextureUsageType usage)
{
	if (usage == TextureUsageType::NoneFlag)
	{
		return "None";
	}

	std::string ret;
	auto append = [&ret](const char* value)
	{
		if (!ret.empty())
		{
			ret += "|";
		}
		ret += value;
	};

	auto contains = [usage](TextureUsageType value) -> bool
	{
		return (static_cast<uint32_t>(usage) & static_cast<uint32_t>(value)) == static_cast<uint32_t>(value);
	};

	if (contains(TextureUsageType::RenderTarget))
	{
		append("RenderTarget");
	}
	if (contains(TextureUsageType::Array))
	{
		append("Array");
	}
	if (contains(TextureUsageType::Storage))
	{
		append("Storage");
	}
	if (contains(TextureUsageType::External))
	{
		append("External");
	}

	const auto knownFlags = static_cast<uint32_t>(TextureUsageType::RenderTarget) | static_cast<uint32_t>(TextureUsageType::Array) |
							static_cast<uint32_t>(TextureUsageType::Storage) | static_cast<uint32_t>(TextureUsageType::External);
	const auto unknownFlags = static_cast<uint32_t>(usage) & ~knownFlags;
	if (unknownFlags != 0)
	{
		const auto unknown = std::string("Unknown(") + std::to_string(unknownFlags) + ")";
		append(unknown.c_str());
	}

	return ret.empty() ? std::string("None") : ret;
}

inline std::string to_string(TextureType type)
{
	switch (type)
	{
	case TextureType::Screen:
		return "Screen";
	case TextureType::Color:
		return "Color";
	case TextureType::Depth:
		return "Depth";
	case TextureType::Render:
		return "Render";
	case TextureType::Cube:
		return "Cube";
	case TextureType::Unknown:
		return "Unknown";
	default:
		return "Unregistered";
	}
}

inline bool IsBlockCompressedFormat(TextureFormatType format)
{
	switch (format)
	{
	case TextureFormatType::BC1:
		return true;
	case TextureFormatType::BC2:
		return true;
	case TextureFormatType::BC3:
		return true;
	case TextureFormatType::BC7:
		return true;
	case TextureFormatType::BC1_SRGB:
		return true;
	case TextureFormatType::BC2_SRGB:
		return true;
	case TextureFormatType::BC3_SRGB:
		return true;
	case TextureFormatType::BC7_SRGB:
		return true;
	default:
		return false;
	}
}

inline int32_t GetTextureRowPitch(TextureFormatType format, Vec3I size)
{
	if (size.X <= 0 || size.Y <= 0 || size.Z <= 0)
		return 0;

	const auto blockCountX = (size.X + 3) / 4;

	switch (format)
	{
	case TextureFormatType::R8G8B8A8_UNORM:
		return size.X * 4;
	case TextureFormatType::B8G8R8A8_UNORM:
		return size.X * 4;
	case TextureFormatType::RG11B10_UFLOAT:
		return size.X * 4;
	case TextureFormatType::R16G16_FLOAT:
		return size.X * 4;
	case TextureFormatType::R8G8B8A8_UNORM_SRGB:
		return size.X * 4;
	case TextureFormatType::B8G8R8A8_UNORM_SRGB:
		return size.X * 4;
	case TextureFormatType::D32:
		return size.X * 4;
	case TextureFormatType::D24S8:
		return size.X * 4;
	case TextureFormatType::R8_UNORM:
		return size.X;
	case TextureFormatType::R16_FLOAT:
		return size.X * 2;
	case TextureFormatType::R32_FLOAT:
		return size.X * 4;
	case TextureFormatType::R32G32_FLOAT:
		return size.X * 8;
	case TextureFormatType::R16G16B16A16_FLOAT:
		return size.X * 8;
	case TextureFormatType::R32G32B32A32_FLOAT:
		return size.X * 16;
	case TextureFormatType::D32S8:
		return size.X * 5;
	case TextureFormatType::BC1:
		return blockCountX * 8;
	case TextureFormatType::BC1_SRGB:
		return blockCountX * 8;
	case TextureFormatType::BC2:
		return blockCountX * 16;
	case TextureFormatType::BC3:
		return blockCountX * 16;
	case TextureFormatType::BC7:
		return blockCountX * 16;
	case TextureFormatType::BC2_SRGB:
		return blockCountX * 16;
	case TextureFormatType::BC3_SRGB:
		return blockCountX * 16;
	case TextureFormatType::BC7_SRGB:
		return blockCountX * 16;
	default:
		auto str = to_string(format);
		Log(LogType::Error, str + " : GetTextureRowPitch is not supported");
		return 0;
	}
}

inline int32_t GetTextureRowCount(TextureFormatType format, Vec3I size)
{
	if (size.Y <= 0 || size.Z <= 0)
		return 0;

	if (IsBlockCompressedFormat(format))
	{
		return ((size.Y + 3) / 4) * size.Z;
	}

	return size.Y * size.Z;
}

inline int32_t GetTextureMemorySize(TextureFormatType format, Vec3I size)
{
	return GetTextureRowPitch(format, size) * GetTextureRowCount(format, size);
}

inline Vec3I GetTextureMipSize(Vec3I size, int32_t mipLevel, bool preserveDepth = false)
{
	auto getMipSize = [](int32_t value, int32_t level) -> int32_t
	{
		return (std::max)(value >> level, 1);
	};

	return Vec3I{
		getMipSize(size.X, mipLevel),
		getMipSize(size.Y, mipLevel),
		preserveDepth ? size.Z : getMipSize(size.Z, mipLevel)};
}

inline int32_t GetTextureMemorySize(TextureFormatType format, Vec3I size, int32_t mipLevelCount, bool preserveDepth = false)
{
	int32_t totalSize = 0;
	for (int32_t mipLevel = 0; mipLevel < mipLevelCount; mipLevel++)
	{
		totalSize += GetTextureMemorySize(format, GetTextureMipSize(size, mipLevel, preserveDepth));
	}
	return totalSize;
}

inline uint32_t GetMaximumMipLevels(const Vec2I& size)
{
	// (std::max) HACK for MSVC
	const auto largeSize = static_cast<float>((std::max)(size.X, size.Y));
	return static_cast<uint32_t>(floorf(log2f(largeSize))) + 1;
}

/**
	@brief	window abstraction class
*/
class Window
{
public:
	Window() = default;

	virtual ~Window() = default;

	/**
		@brief	called when platform calls newframe, if it need to exit, return false
	*/
	virtual bool OnNewFrame() = 0;

	/**
		@brief return native window ptr
	*/
	virtual void* GetNativePtr(int32_t index) = 0;

	/**
		@brief	return current window size
	*/
	virtual Vec2I GetWindowSize() const = 0;

	/**
		@brief  return current frame buffer  size
	*/
	virtual Vec2I GetFrameBufferSize() const { return GetWindowSize(); }
};

} // namespace LLGI

namespace std
{

template <> struct hash<LLGI::TextureFormatType>
{
	size_t operator()(const LLGI::TextureFormatType& _Keyval) const noexcept
	{
		return std::hash<uint32_t>()(static_cast<uint32_t>(_Keyval));
	}
};

} // namespace std
