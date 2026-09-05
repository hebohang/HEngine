#include "LLGI.ShaderMetal.h"
#include "LLGI.GraphicsMetal.h"
#include "LLGI.Metal_Impl.h"
#import <dispatch/dispatch.h>
#import <MetalKit/MetalKit.h>

#include <cstring>

#define SUPPRESS_COMPILE_WARNINGS

namespace LLGI
{
namespace
{
constexpr char MetalCodeHeader[] = "mtlcode";
constexpr size_t MetalCodeHeaderSize = sizeof(MetalCodeHeader) - 1;

bool IsMetalCode(const DataStructure& data)
{
	return data.Data != nullptr && data.Size >= static_cast<int32_t>(MetalCodeHeaderSize) &&
		   std::memcmp(data.Data, MetalCodeHeader, MetalCodeHeaderSize) == 0;
}
} // namespace

ShaderMetal::ShaderMetal() {}

ShaderMetal::~ShaderMetal()
{
	if (library_ != nullptr)
	{
		[library_ release];
		library_ = nullptr;
	}
	SafeRelease(graphics_);
}

bool ShaderMetal::Initialize(GraphicsMetal* graphics, DataStructure* data, int32_t count)
{
	@autoreleasepool
	{
		if (graphics == nullptr || data == nullptr || count <= 0 || data[0].Data == nullptr || data[0].Size <= 0)
		{
			return false;
		}

		SafeAddRef(graphics);
		SafeRelease(graphics_);
		graphics_ = graphics;
		auto g = static_cast<GraphicsMetal*>(graphics);

		auto device = g->GetDevice();

		if (IsMetalCode(data[0]))
		{
			const auto* code = static_cast<const char*>(data[0].Data) + MetalCodeHeaderSize;
			auto codeSize = static_cast<size_t>(data[0].Size) - MetalCodeHeaderSize;
			if (codeSize > 0 && code[codeSize - 1] == '\0')
			{
				codeSize--;
			}

			NSString* codeStr = [[[NSString alloc] initWithBytes:code length:codeSize encoding:NSUTF8StringEncoding] autorelease];
			if (codeStr == nil)
			{
				return false;
			}

			NSError* libraryError = nil;
			id<MTLLibrary> lib = [device newLibraryWithSource:codeStr options:NULL error:&libraryError];
			if (libraryError
#ifdef SUPPRESS_COMPILE_WARNINGS
				&& [libraryError.localizedDescription rangeOfString:@"succeeded"].location == NSNotFound
#endif
			)
			{
				Log(LogType::Error, libraryError.localizedDescription.UTF8String);
				return false;
			}
			if (lib == nil)
			{
				Log(LogType::Error, "Failed to create Metal shader library from source.");
				return false;
			}

			this->library_ = lib;
		}
		else
		{
			dispatch_data_t libraryData = dispatch_data_create(data[0].Data, data[0].Size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
			if (libraryData == nullptr)
			{
				return false;
			}

			NSError* libraryError = nil;
			id<MTLLibrary> lib = [device newLibraryWithData:libraryData error:&libraryError];
			dispatch_release(libraryData);

			if (libraryError
#ifdef SUPPRESS_COMPILE_WARNINGS
				&& [libraryError.localizedDescription rangeOfString:@"succeeded"].location == NSNotFound
#endif
			)
			{
				Log(LogType::Error, libraryError.localizedDescription.UTF8String);
				return false;
			}
			if (lib == nil)
			{
				Log(LogType::Error, "Failed to create Metal shader library from binary.");
				return false;
			}

			this->library_ = lib;
		}

		return true;
	}
}

}
