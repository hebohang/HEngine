
#pragma once

#include "../LLGI.CommandList.h"
#include "LLGI.BaseDX12.h"
#include "LLGI.DescriptorHeapDX12.h"
#include "LLGI.GraphicsDX12.h"
#include "LLGI.PipelineStateDX12.h"
#include "LLGI.RenderPassDX12.h"
#include <unordered_map>

namespace LLGI
{

struct PlatformContextDX12
{
	ID3D12GraphicsCommandList* commandList = nullptr;
};

class BufferDX12;

class CommandListDX12 : public CommandList
{
private:
	static const int MaximumRenderTargetChange = 32;

	std::shared_ptr<DX12::DescriptorHeapAllocator> samplerDescriptorHeap_;
	std::shared_ptr<DX12::DescriptorHeapAllocator> cbDescriptorHeap_;
	std::shared_ptr<DX12::DescriptorHeapAllocator> computeDescriptorHeap_;
	std::shared_ptr<DX12::DescriptorHeapAllocator> rtDescriptorHeap_;
	std::shared_ptr<DX12::DescriptorHeapAllocator> dtDescriptorHeap_;

	std::shared_ptr<ID3D12GraphicsCommandList> commandList_;
	std::shared_ptr<ID3D12CommandAllocator> commandAllocator_;
	ID3D12Fence* fence_ = nullptr;
	HANDLE fenceEvent_ = nullptr;
	UINT64 fenceValue_ = 1;

	std::shared_ptr<GraphicsDX12> graphics_;
	std::shared_ptr<RenderPassDX12> renderPass_;
	TextureDX12* fallbackSampledTexture_ = nullptr;

	ID3D12GraphicsCommandList* currentCommandList_ = nullptr;
	ID3D12RootSignature* mipmapRootSignature_ = nullptr;
	std::unordered_map<DXGI_FORMAT, ID3D12PipelineState*> mipmapPipelineStates_;

	D3D12_SHADER_RESOURCE_VIEW_DESC GetSRVDescFromTexture(const TextureDX12* texture);
	D3D12_SAMPLER_DESC GeSamplerDescFromBindingTexture(const BindingTexture& texture);
	D3D12_SHADER_RESOURCE_VIEW_DESC GetSRVDescFromStorageBuffer(const BindingStorageBuffer& buffer, bool isRawBuffer);
	D3D12_UNORDERED_ACCESS_VIEW_DESC GetUAVDescFromStorageBuffer(const BindingStorageBuffer& buffer, bool isRawBuffer);
	bool ShouldBindStorageBufferAsSRV(const PipelineStateDX12* pipeline, const BindingStorageBuffer& buffer, int32_t unit) const;
	bool ShouldBindStorageBufferAsUAV(const PipelineStateDX12* pipeline, const BindingStorageBuffer& buffer, int32_t unit) const;
	bool ValidateStorageBufferView(const BindingStorageBuffer& buffer, bool isRawBuffer, int32_t unit) const;
	bool CreateStorageBufferSRV(const BindingStorageBuffer& buffer, bool isRawBuffer, int32_t unit, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle);
	bool CreateFallbackSampledTexture();
	bool CreateSampledTextureSRV(TextureDX12* texture, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle);
	bool CreateStorageBufferUAV(const BindingStorageBuffer& buffer, bool isRawBuffer, int32_t unit, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle);
	bool CreateMipmapRootSignature();
	ID3D12PipelineState* GetMipmapPipelineState(DXGI_FORMAT format);

	void BeginInternal();

public:
	CommandListDX12();
	~CommandListDX12() override;
	bool Initialize(GraphicsDX12* graphics, int32_t drawingCount);

	void Begin() override;
	void End() override;
	bool BeginWithPlatform(void* platformContextPtr) override;
	void EndWithPlatform() override;

	void BeginRenderPass(RenderPass* renderPass) override;
	void EndRenderPass() override;
	void Draw(int32_t primitiveCount, int32_t instanceCount) override;
	void CopyTexture(Texture* src, Texture* dst) override;
	void CopyTexture(
		Texture* src, Texture* dst, const Vec3I& srcPos, const Vec3I& dstPos, const Vec3I& size, int srcLayer, int dstLayer) override;
	void GenerateMipMap(Texture* src) override;

	void CopyBuffer(Buffer* src, Buffer* dst) override;

	bool ResetQuery(Query* query) override;
	bool BeginQuery(Query* query, uint32_t queryIndex) override;
	bool EndQuery(Query* query, uint32_t queryIndex) override;
	bool RecordTimestamp(Query* query, uint32_t queryIndex) override;

	void BeginComputePass() override;
	void EndComputePass() override;
	void Dispatch(int32_t groupX, int32_t groupY, int32_t groupZ, int32_t threadX, int32_t threadY, int32_t threadZ) override;

	void Clear(const Color8& color);

	void ClearDepth();

	ID3D12GraphicsCommandList* GetCommandList() const;

	ID3D12Fence* GetFence() const;
	UINT64 GetAndIncFenceValue();

	void WaitUntilCompleted() override;
};

} // namespace LLGI
