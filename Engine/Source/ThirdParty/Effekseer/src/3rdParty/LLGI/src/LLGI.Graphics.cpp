#include "LLGI.Graphics.h"
#include "LLGI.Buffer.h"
#include "LLGI.Texture.h"

namespace LLGI
{

//! TODO should be moved
static std::function<void(LogType, const std::string&)> g_logger;

void SetLogger(const std::function<void(LogType, const std::string&)>& logger) { g_logger = logger; }

static const char* GetLogTypeName(LogType logType)
{
	switch (logType)
	{
	case LogType::Info:
		return "Info";
	case LogType::Warning:
		return "Warning";
	case LogType::Error:
		return "Error";
	case LogType::Debug:
		return "Debug";
	default:
		return "Unknown";
	}
}

static std::string DescribeTextureForLog(const Texture* texture)
{
	if (texture == nullptr)
	{
		return "null";
	}

	return std::string("{type=") + to_string(texture->GetType()) + ", format=" + to_string(texture->GetFormat()) +
		   ", size=" + to_string(texture->GetSizeAs2D()) + ", samples=" + std::to_string(texture->GetSamplingCount()) +
		   ", mips=" + std::to_string(texture->GetMipmapCount()) + "}";
}

void Log(LogType logType, const std::string& message)
{
	if (g_logger != nullptr)
	{
		g_logger(logType, message);
		return;
	}

	fprintf(stderr, "[LLGI][%s] %s\n", GetLogTypeName(logType), message.c_str());
}

std::string DescribeTextureParameter(const TextureParameter& parameter)
{
	return std::string("{format=") + to_string(parameter.Format) + ", size=" + to_string(parameter.Size) +
		   ", dimension=" + std::to_string(parameter.Dimension) + ", mips=" + std::to_string(parameter.MipLevelCount) +
		   ", samples=" + std::to_string(parameter.SampleCount) + ", usage=" + to_string(parameter.Usage) +
		   ", generateMips=" + (parameter.IsMipmapGenerationEnabled ? "true" : "false") + "}";
}

TextureFormatType GetDepthTextureFormat(DepthTextureMode mode)
{
	return mode == DepthTextureMode::DepthStencil ? TextureFormatType::D24S8 : TextureFormatType::D32;
}

TextureParameter ToTextureParameter(const TextureInitializationParameter& parameter)
{
	TextureParameter ret;
	ret.Dimension = 2;
	ret.Format = parameter.Format;
	ret.MipLevelCount = parameter.MipMapCount;
	ret.IsMipmapGenerationEnabled = parameter.MipMapCount > 1;
	ret.SampleCount = 1;
	ret.Size = {parameter.Size.X, parameter.Size.Y, 1};
	ret.Usage = TextureUsageType::NoneFlag;
	return ret;
}

TextureParameter ToTextureParameter(const RenderTextureInitializationParameter& parameter)
{
	TextureParameter ret;
	ret.Dimension = 2;
	ret.Format = parameter.Format;
	ret.MipLevelCount = 1;
	ret.SampleCount = parameter.SamplingCount;
	ret.Size = {parameter.Size.X, parameter.Size.Y, 1};
	ret.Usage = TextureUsageType::RenderTarget;
	return ret;
}

TextureParameter ToTextureParameter(const DepthTextureInitializationParameter& parameter)
{
	TextureParameter ret;
	ret.Dimension = 2;
	ret.Format = GetDepthTextureFormat(parameter.Mode);
	ret.MipLevelCount = 1;
	ret.SampleCount = parameter.SamplingCount;
	ret.Size = {parameter.Size.X, parameter.Size.Y, 1};
	ret.Usage = TextureUsageType::NoneFlag;
	return ret;
}

bool ValidateTextureParameter(const TextureParameter& parameter, const char* caller, int32_t minimumDimension)
{
	const auto prefix = std::string(caller != nullptr ? caller : "ValidateTextureParameter") + " failed: ";
	if (parameter.Dimension < minimumDimension || parameter.Dimension > 3)
	{
		Log(LogType::Error,
			prefix + "invalid dimension. minimum=" + std::to_string(minimumDimension) +
				", parameter=" + DescribeTextureParameter(parameter));
		return false;
	}
	if (parameter.Size.X <= 0 || parameter.Size.Y <= 0 || parameter.Size.Z <= 0)
	{
		Log(LogType::Error, prefix + "invalid size. parameter=" + DescribeTextureParameter(parameter));
		return false;
	}
	if (parameter.MipLevelCount <= 0)
	{
		Log(LogType::Error, prefix + "invalid mip level count. parameter=" + DescribeTextureParameter(parameter));
		return false;
	}
	if (parameter.SampleCount <= 0)
	{
		Log(LogType::Error, prefix + "invalid sample count. parameter=" + DescribeTextureParameter(parameter));
		return false;
	}
	if (parameter.Format == TextureFormatType::Unknown)
	{
		Log(LogType::Error, prefix + "unknown texture format. parameter=" + DescribeTextureParameter(parameter));
		return false;
	}
	if (parameter.Dimension == 3 && BitwiseContains(parameter.Usage, TextureUsageType::Array))
	{
		Log(LogType::Error, prefix + "3D texture arrays are not supported. parameter=" + DescribeTextureParameter(parameter));
		return false;
	}
	if (parameter.IsMipmapGenerationEnabled && BitwiseContains(parameter.Usage, TextureUsageType::Array))
	{
		Log(LogType::Error,
			prefix + "mipmap generation for texture arrays is currently not supported by LLGI. parameter=" +
				DescribeTextureParameter(parameter));
		return false;
	}
	if (parameter.SampleCount > 1)
	{
		if (parameter.Dimension != 2 || parameter.MipLevelCount != 1)
		{
			Log(LogType::Error,
				prefix + "MSAA textures must be 2D and single-mip. parameter=" + DescribeTextureParameter(parameter));
			return false;
		}
		if (!(BitwiseContains(parameter.Usage, TextureUsageType::RenderTarget) || IsDepthFormat(parameter.Format)))
		{
			Log(LogType::Error,
				prefix + "MSAA textures must be render or depth textures. parameter=" + DescribeTextureParameter(parameter));
			return false;
		}
	}
	if (IsBlockCompressedFormat(parameter.Format))
	{
		if (BitwiseContains(parameter.Usage, TextureUsageType::RenderTarget) || BitwiseContains(parameter.Usage, TextureUsageType::Storage) ||
			parameter.SampleCount > 1)
		{
			Log(LogType::Error,
				prefix + "block-compressed textures cannot be render, storage, or multisampled textures. parameter=" +
					DescribeTextureParameter(parameter));
			return false;
		}
	}
	if (IsDepthFormat(parameter.Format))
	{
		if (parameter.Dimension != 2 || BitwiseContains(parameter.Usage, TextureUsageType::Array) || parameter.MipLevelCount != 1)
		{
			Log(LogType::Error,
				prefix + "depth textures must be 2D, non-array, and single-mip. parameter=" + DescribeTextureParameter(parameter));
			return false;
		}
	}

	return true;
}

bool ValidateExternalTextureID(uint64_t id, const char* caller)
{
	if (id == 0)
	{
		Log(LogType::Error, std::string(caller != nullptr ? caller : "CreateTexture(external)") + " failed: id is null.");
		return false;
	}

	return true;
}

SingleFrameMemoryPool::SingleFrameMemoryPool(int32_t swapBufferCount) : swapBufferCount_(swapBufferCount)
{

	for (int i = 0; i < swapBufferCount_; i++)
	{
		offsets_.push_back(0);
		buffers_.push_back(std::vector<Buffer*>());
	}
}

SingleFrameMemoryPool::~SingleFrameMemoryPool()
{
	for (auto& buffer : buffers_)
	{
		for (auto c : buffer)
		{
			c->Release();
		}
	}
}

void SingleFrameMemoryPool::NewFrame()
{
	currentSwapBuffer_++;
	currentSwapBuffer_ %= swapBufferCount_;
	offsets_[currentSwapBuffer_] = 0;
}

Buffer* SingleFrameMemoryPool::CreateConstantBuffer(int32_t size)
{
	assert(currentSwapBuffer_ >= 0);

	if (static_cast<int32_t>(buffers_[currentSwapBuffer_].size()) <= offsets_[currentSwapBuffer_])
	{
		auto cb = CreateBufferInternal(size);
		if (cb == nullptr)
		{
			return nullptr;
		}

		buffers_[currentSwapBuffer_].push_back(cb);
		SafeAddRef(cb);
		offsets_[currentSwapBuffer_]++;
		return cb;
	}
	else
	{
		auto cb = buffers_[currentSwapBuffer_][offsets_[currentSwapBuffer_]];
		auto newCb = ReinitializeBuffer(cb, size);
		if (newCb == nullptr)
		{
			return nullptr;
		}

		SafeAddRef(newCb);
		offsets_[currentSwapBuffer_]++;
		return newCb;
	}
}

bool RenderPass::assignRenderTextures(Texture** textures, int32_t count)
{
	if (textures == nullptr || count <= 0)
	{
		Log(LogType::Error, "RenderPass : Invalid Count.");
		return false;
	}

	for (int32_t i = 0; i < count; i++)
	{
		if (textures[i] == nullptr)
		{
			Log(LogType::Error, std::string("RenderPass : Invalid RenderTexture. index=") + std::to_string(i) + ", texture=null");
			return false;
		}

		if (!(textures[i]->GetType() == TextureType::Render || textures[i]->GetType() == TextureType::Screen))
		{
			Log(LogType::Error,
				std::string("RenderPass : Invalid RenderTexture. index=") + std::to_string(i) +
					", texture=" + DescribeTextureForLog(textures[i]));
			return false;
		}
	}

	for (int32_t i = 0; i < count; i++)
	{
		SafeAddRef(textures[i]);
	}

	for (int32_t i = 0; i < static_cast<int32_t>(renderTextures_.size()); i++)
	{
		renderTextures_.at(i)->Release();
	}

	renderTextures_.resize(count);

	for (int32_t i = 0; i < count; i++)
	{
		renderTextures_.at(i) = textures[i];
	}

	return true;
}

bool RenderPass::assignDepthTexture(Texture* depthTexture)
{
	if (depthTexture != nullptr && depthTexture->GetType() != TextureType::Depth)
	{
		Log(LogType::Error, std::string("RenderPass : Invalid DepthTexture. texture=") + DescribeTextureForLog(depthTexture));
		return false;
	}

	SafeAddRef(depthTexture);
	SafeRelease(depthTexture_);
	depthTexture_ = depthTexture;

	return true;
}

bool RenderPass::assignResolvedRenderTexture(Texture* texture)
{
	if (texture != nullptr && texture->GetType() != TextureType::Render)
	{
		Log(LogType::Error, std::string("RenderPass : Invalid ResolvedTexture. texture=") + DescribeTextureForLog(texture));
		return false;
	}

	SafeAddRef(texture);
	SafeRelease(resolvedRenderTexture_);
	resolvedRenderTexture_ = texture;

	return true;
}

bool RenderPass::assignResolvedDepthTexture(Texture* texture)
{
	if (texture != nullptr && texture->GetType() != TextureType::Depth)
	{
		Log(LogType::Error, std::string("RenderPass : Invalid ResolvedDepthTexture. texture=") + DescribeTextureForLog(texture));
		return false;
	}

	SafeAddRef(texture);
	SafeRelease(resolvedDepthTexture_);
	resolvedDepthTexture_ = texture;

	return true;
}

bool RenderPass::getSize(Vec2I& size,
						 const Texture** textures,
						 int32_t textureCount,
						 Texture* depthTexture,
						 Texture* resolvedRenderTexture,
						 Texture* resolvedDepthTexture) const
{
	if (textures == nullptr || textureCount <= 0)
	{
		Log(LogType::Error, "RenderPass : Invalid Count.");
		return false;
	}

	for (int i = 0; i < textureCount; i++)
	{
		if (textures[i] == nullptr)
		{
			Log(LogType::Error, "RenderPass : Invalid RenderTexture.");
			return false;
		}
	}

	size = textures[0]->GetSizeAs2D();

	for (int i = 0; i < textureCount; i++)
	{
		auto temp = textures[i]->GetSizeAs2D();
		if (size.X != temp.X)
		{
			Log(LogType::Error,
				std::string("RenderPass : RenderTexture width mismatch. index=") + std::to_string(i) +
					", expected=" + std::to_string(size.X) + ", actual=" + std::to_string(temp.X) +
					", texture=" + DescribeTextureForLog(textures[i]));
			goto FAIL;
		}
		if (size.Y != temp.Y)
		{
			Log(LogType::Error,
				std::string("RenderPass : RenderTexture height mismatch. index=") + std::to_string(i) +
					", expected=" + std::to_string(size.Y) + ", actual=" + std::to_string(temp.Y) +
					", texture=" + DescribeTextureForLog(textures[i]));
			goto FAIL;
		}
	}

	if (depthTexture != nullptr)
	{
		if (size.X != depthTexture->GetSizeAs2D().X)
		{
			Log(LogType::Error,
				std::string("RenderPass : DepthTexture width mismatch. expected=") + std::to_string(size.X) +
					", actual=" + std::to_string(depthTexture->GetSizeAs2D().X) +
					", texture=" + DescribeTextureForLog(depthTexture));
			goto FAIL;
		}
		if (size.Y != depthTexture->GetSizeAs2D().Y)
		{
			Log(LogType::Error,
				std::string("RenderPass : DepthTexture height mismatch. expected=") + std::to_string(size.Y) +
					", actual=" + std::to_string(depthTexture->GetSizeAs2D().Y) +
					", texture=" + DescribeTextureForLog(depthTexture));
			goto FAIL;
		}
	}

	if (resolvedRenderTexture != nullptr)
	{
		if (size.X != resolvedRenderTexture->GetSizeAs2D().X)
		{
			Log(LogType::Error,
				std::string("RenderPass : ResolvedRenderTexture width mismatch. expected=") + std::to_string(size.X) +
					", actual=" + std::to_string(resolvedRenderTexture->GetSizeAs2D().X) +
					", texture=" + DescribeTextureForLog(resolvedRenderTexture));
			goto FAIL;
		}

		if (size.Y != resolvedRenderTexture->GetSizeAs2D().Y)
		{
			Log(LogType::Error,
				std::string("RenderPass : ResolvedRenderTexture height mismatch. expected=") + std::to_string(size.Y) +
					", actual=" + std::to_string(resolvedRenderTexture->GetSizeAs2D().Y) +
					", texture=" + DescribeTextureForLog(resolvedRenderTexture));
			goto FAIL;
		}
	}

	if (resolvedDepthTexture != nullptr)
	{
		if (size.X != resolvedDepthTexture->GetSizeAs2D().X)
		{
			Log(LogType::Error,
				std::string("RenderPass : ResolvedDepthTexture width mismatch. expected=") + std::to_string(size.X) +
					", actual=" + std::to_string(resolvedDepthTexture->GetSizeAs2D().X) +
					", texture=" + DescribeTextureForLog(resolvedDepthTexture));
			goto FAIL;
		}
		if (size.Y != resolvedDepthTexture->GetSizeAs2D().Y)
		{
			Log(LogType::Error,
				std::string("RenderPass : ResolvedDepthTexture height mismatch. expected=") + std::to_string(size.Y) +
					", actual=" + std::to_string(resolvedDepthTexture->GetSizeAs2D().Y) +
					", texture=" + DescribeTextureForLog(resolvedDepthTexture));
			goto FAIL;
		}
	}

	return true;

FAIL:;
	Log(LogType::Error, "RenderPass : Invalid Size.");
	return false;
}

bool RenderPass::sanitize()
{

	if (resolvedRenderTexture_ != nullptr)
	{
		if (renderTextures_.size() != 1)
		{
			Log(LogType::Error,
				std::string("RenderPass : Resolved render target requires exactly one render target. count=") +
					std::to_string(renderTextures_.size()));
			return false;
		}

		if (renderTextures_.at(0)->GetFormat() != resolvedRenderTexture_->GetFormat())
		{
			Log(LogType::Error,
				std::string("RenderPass : Formats are not same between Render and Resolved. render=") +
					DescribeTextureForLog(renderTextures_.at(0)) + ", resolved=" + DescribeTextureForLog(resolvedRenderTexture_));
			return false;
		}

		if (renderTextures_.at(0)->GetSamplingCount() <= 1 || resolvedRenderTexture_->GetSamplingCount() != 1)
		{
			Log(LogType::Error,
				std::string("RenderPass : Invalid SamplingCount between Render and Resolved. render=") +
					DescribeTextureForLog(renderTextures_.at(0)) + ", resolved=" + DescribeTextureForLog(resolvedRenderTexture_));
			return false;
		}
	}

	if (resolvedDepthTexture_ != nullptr)
	{
		if (depthTexture_ == nullptr)
		{
			Log(LogType::Error,
				std::string("RenderPass : Require a depth texture for resolved depth. resolved=") +
					DescribeTextureForLog(resolvedDepthTexture_));
			return false;
		}

		if (depthTexture_->GetFormat() != resolvedDepthTexture_->GetFormat())
		{
			Log(LogType::Error,
				std::string("RenderPass : Formats are not same between Depth and ResolvedDepth. depth=") +
					DescribeTextureForLog(depthTexture_) + ", resolved=" + DescribeTextureForLog(resolvedDepthTexture_));
			return false;
		}

		if (depthTexture_->GetSamplingCount() <= 1 || resolvedDepthTexture_->GetSamplingCount() != 1)
		{
			Log(LogType::Error,
				std::string("RenderPass : Invalid SamplingCount between Depth and ResolvedDepth. depth=") +
					DescribeTextureForLog(depthTexture_) + ", resolved=" + DescribeTextureForLog(resolvedDepthTexture_));
			return false;
		}
	}

	if (depthTexture_ != nullptr && renderTextures_.size() > 0 && renderTextures_.at(0)->GetSamplingCount() != depthTexture_->GetSamplingCount())
	{
		Log(LogType::Error,
			std::string("RenderPass : SamplingCount are not same. render=") + DescribeTextureForLog(renderTextures_.at(0)) +
				", depth=" + DescribeTextureForLog(depthTexture_));
		return false;
	}

	return true;
}

RenderPass::~RenderPass()
{
	SafeRelease(depthTexture_);
	SafeRelease(resolvedRenderTexture_);
	SafeRelease(resolvedDepthTexture_);

	for (size_t i = 0; i < renderTextures_.size(); i++)
	{
		renderTextures_.at(i)->Release();
	}
}

void RenderPass::SetIsColorCleared(bool isColorCleared) { isColorCleared_ = isColorCleared; }

void RenderPass::SetIsDepthCleared(bool isDepthCleared) { isDepthCleared_ = isDepthCleared; }

void RenderPass::SetClearColor(const Color8& color) { color_ = color; }

bool RenderPass::GetIsSwapchainScreen() const
{
	return GetRenderTextureCount() > 0 && GetRenderTexture(0)->GetType() == TextureType::Screen;
}

RenderPassPipelineStateKey RenderPass::GetKey() const
{
	RenderPassPipelineStateKey key;

	key.IsPresent = GetIsSwapchainScreen();
	key.IsColorCleared = GetIsColorCleared();
	key.IsDepthCleared = GetIsDepthCleared();
	const auto renderTextureCount = GetRenderTextureCount();
	key.RenderTargetFormats.resize(renderTextureCount);
	key.SamplingCount = renderTextureCount > 0 ? renderTextures_.at(0)->GetSamplingCount() : 1;
	key.HasResolvedRenderTarget = GetResolvedRenderTexture() != nullptr;
	key.HasResolvedDepthTarget = GetResolvedDepthTexture() != nullptr;

	for (size_t i = 0; i < key.RenderTargetFormats.size(); i++)
	{
		key.RenderTargetFormats.at(i) = GetRenderTexture(static_cast<int32_t>(i))->GetFormat();
	}

	if (GetHasDepthTexture())
	{
		key.DepthFormat = GetDepthTexture()->GetFormat();
	}
	else
	{
		key.DepthFormat = TextureFormatType::Unknown;
	}

	return key;
}

Graphics::~Graphics()
{
	if (disposed_ != nullptr)
	{
		disposed_();
	}
}

void Graphics::SetWindowSize(const Vec2I& windowSize) { windowSize_ = windowSize; }

void Graphics::Execute(CommandList* commandList) {}

// RenderPass* Graphics::GetCurrentScreen(const Color8& clearColor, bool isColorCleared, bool isDepthCleared) { return nullptr; }

Buffer* Graphics::CreateBuffer(BufferUsageType usage, int32_t size) { return nullptr; }

Shader* Graphics::CreateShader(DataStructure* data, int32_t count) { return nullptr; }

PipelineState* Graphics::CreatePiplineState() { return nullptr; }

SingleFrameMemoryPool* Graphics::CreateSingleFrameMemoryPool(int32_t constantBufferPoolSize, int32_t drawingCount) { return nullptr; }

CommandList* Graphics::CreateCommandList(SingleFrameMemoryPool* memoryPool) { return nullptr; }

Texture* Graphics::CreateTexture(uint64_t id) { return nullptr; }

RenderPassPipelineState* Graphics::CreateRenderPassPipelineState(RenderPass* renderPass)
{
	if (renderPass == nullptr)
	{
		Log(LogType::Error, "RenderPass is null.");
		return nullptr;
	}

	return CreateRenderPassPipelineState(renderPass->GetKey());
}

RenderPassPipelineState* Graphics::CreateRenderPassPipelineState(const RenderPassPipelineStateKey& key)
{
	auto ret = new RenderPassPipelineState();
	ret->Key = key;
	return ret;
}

std::vector<uint8_t> Graphics::CaptureRenderTarget(Texture* renderTarget)
{
	Log(LogType::Error, "GetColorBuffer is not implemented.");
	assert(0);
	return std::vector<uint8_t>();
}

void Graphics::SetDisposed(const std::function<void()>& disposed) { disposed_ = disposed; }

} // namespace LLGI
