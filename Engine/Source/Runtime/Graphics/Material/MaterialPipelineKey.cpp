#include "Hepch.h"

#include "MaterialPipelineKey.h"

#include <string_view>

namespace HEngine
{
    namespace
    {
        class StablePipelineKeyBuilder final
        {
        public:
            void AddUnsigned(uint64_t value) noexcept
            {
                // An explicit byte order keeps the key stable across processes
                // and independent of object padding or host representation.
                for (uint32_t byteIndex = 0; byteIndex < sizeof(value); ++byteIndex)
                {
                    AddByte(static_cast<uint8_t>(value & 0xffu));
                    value >>= 8u;
                }
            }

            void AddString(std::string_view value) noexcept
            {
                AddUnsigned(static_cast<uint64_t>(value.size()));
                for (const unsigned char character : value)
                    AddByte(character);
            }

            [[nodiscard]] uint64_t Finish() const noexcept
            {
                return mValue == 0 ? 1 : mValue;
            }

        private:
            void AddByte(uint8_t value) noexcept
            {
                mValue ^= value;
                mValue *= 1099511628211ull;
            }

            uint64_t mValue = 14695981039346656037ull;
        };
    }

    GraphicsPipelineKey BuildMaterialPipelineKey(const MaterialPipelineDesc& desc) noexcept
    {
        StablePipelineKeyBuilder key;
        key.AddString("HEngine.MaterialPipelineKey.v2");
        key.AddString(desc.Program.RecursiveContentHash);
        key.AddString(desc.Program.ArtifactIdentityHash);
        key.AddUnsigned(desc.Program.Generation);
        key.AddUnsigned(desc.Program.MaterialAbiVersion);
        key.AddString(desc.Program.BindingSignature);
        key.AddUnsigned(static_cast<uint64_t>(desc.Pass));
        key.AddUnsigned(static_cast<uint64_t>(desc.VertexPath));
        key.AddUnsigned(static_cast<uint64_t>(desc.DescriptorTier));
        key.AddUnsigned(desc.InputLayout.Value);
        key.AddUnsigned(static_cast<uint64_t>(desc.BlendMode));
        key.AddUnsigned(static_cast<uint64_t>(desc.DepthWrite));
        key.AddUnsigned(static_cast<uint64_t>(desc.CullMode));
        key.AddUnsigned(static_cast<uint64_t>(desc.Sidedness));
        key.AddUnsigned(static_cast<uint64_t>(desc.AlphaMode));
        key.AddUnsigned(static_cast<uint64_t>(desc.Framebuffer.ColorFormat));
        key.AddUnsigned(static_cast<uint64_t>(desc.Framebuffer.DepthFormat));
        key.AddUnsigned(desc.Framebuffer.SampleCount);
        key.AddUnsigned(desc.Framebuffer.SampleQuality);
        return GraphicsPipelineKey{ .Value = key.Finish() };
    }
}
