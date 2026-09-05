#include "Runtime/Graphics/Material/MaterialShaderManifest.h"

#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace HEngine
{
    namespace
    {
        using Json = nlohmann::json;

        class Sha256 final
        {
        public:
            void Update(std::span<const std::uint8_t> bytes)
            {
                if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - mTotalBytes)
                    throw std::overflow_error("SHA-256 input is too large");
                mTotalBytes += static_cast<std::uint64_t>(bytes.size());

                std::size_t offset = 0;
                if (mBufferedBytes != 0)
                {
                    const std::size_t copied = std::min(
                        bytes.size(), mBuffer.size() - mBufferedBytes);
                    std::copy_n(bytes.data(), copied, mBuffer.data() + mBufferedBytes);
                    mBufferedBytes += copied;
                    offset += copied;
                    if (mBufferedBytes == mBuffer.size())
                    {
                        Transform(mBuffer.data());
                        mBufferedBytes = 0;
                    }
                }

                while (bytes.size() - offset >= mBuffer.size())
                {
                    Transform(bytes.data() + offset);
                    offset += mBuffer.size();
                }
                if (offset != bytes.size())
                {
                    mBufferedBytes = bytes.size() - offset;
                    std::copy_n(bytes.data() + offset, mBufferedBytes, mBuffer.data());
                }
            }

            [[nodiscard]] std::array<std::uint8_t, 32> Finalize()
            {
                if (mTotalBytes > std::numeric_limits<std::uint64_t>::max() / 8u)
                    throw std::overflow_error("SHA-256 input is too large");
                const std::uint64_t bitLength = mTotalBytes * 8u;

                mBuffer[mBufferedBytes++] = 0x80u;
                if (mBufferedBytes > 56u)
                {
                    std::fill(mBuffer.begin() + mBufferedBytes, mBuffer.end(), 0u);
                    Transform(mBuffer.data());
                    mBufferedBytes = 0;
                }
                std::fill(mBuffer.begin() + mBufferedBytes, mBuffer.begin() + 56u, 0u);
                for (std::size_t index = 0; index < 8u; ++index)
                {
                    mBuffer[56u + index] = static_cast<std::uint8_t>(
                        (bitLength >> ((7u - index) * 8u)) & 0xffu);
                }
                Transform(mBuffer.data());

                std::array<std::uint8_t, 32> digest{};
                for (std::size_t word = 0; word < mState.size(); ++word)
                {
                    for (std::size_t byte = 0; byte < 4u; ++byte)
                    {
                        digest[word * 4u + byte] = static_cast<std::uint8_t>(
                            (mState[word] >> ((3u - byte) * 8u)) & 0xffu);
                    }
                }
                return digest;
            }

        private:
            static constexpr std::array<std::uint32_t, 64> c_RoundConstants = {
                0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
                0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
                0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
                0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
                0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
                0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
                0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
                0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
                0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
                0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
                0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
                0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
                0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
                0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
                0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
                0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
            };

            [[nodiscard]] static std::uint32_t RotateRight(
                std::uint32_t value,
                std::uint32_t count) noexcept
            {
                return (value >> count) | (value << (32u - count));
            }

            void Transform(const std::uint8_t* block) noexcept
            {
                std::array<std::uint32_t, 64> words{};
                for (std::size_t index = 0; index < 16u; ++index)
                {
                    words[index] =
                        (static_cast<std::uint32_t>(block[index * 4u]) << 24u) |
                        (static_cast<std::uint32_t>(block[index * 4u + 1u]) << 16u) |
                        (static_cast<std::uint32_t>(block[index * 4u + 2u]) << 8u) |
                        static_cast<std::uint32_t>(block[index * 4u + 3u]);
                }
                for (std::size_t index = 16u; index < words.size(); ++index)
                {
                    const std::uint32_t sigma0 = RotateRight(words[index - 15u], 7u) ^
                        RotateRight(words[index - 15u], 18u) ^ (words[index - 15u] >> 3u);
                    const std::uint32_t sigma1 = RotateRight(words[index - 2u], 17u) ^
                        RotateRight(words[index - 2u], 19u) ^ (words[index - 2u] >> 10u);
                    words[index] = words[index - 16u] + sigma0 + words[index - 7u] + sigma1;
                }

                std::uint32_t a = mState[0];
                std::uint32_t b = mState[1];
                std::uint32_t c = mState[2];
                std::uint32_t d = mState[3];
                std::uint32_t e = mState[4];
                std::uint32_t f = mState[5];
                std::uint32_t g = mState[6];
                std::uint32_t h = mState[7];
                for (std::size_t index = 0; index < words.size(); ++index)
                {
                    const std::uint32_t sum1 = RotateRight(e, 6u) ^
                        RotateRight(e, 11u) ^ RotateRight(e, 25u);
                    const std::uint32_t choose = (e & f) ^ (~e & g);
                    const std::uint32_t temporary1 = h + sum1 + choose +
                        c_RoundConstants[index] + words[index];
                    const std::uint32_t sum0 = RotateRight(a, 2u) ^
                        RotateRight(a, 13u) ^ RotateRight(a, 22u);
                    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
                    const std::uint32_t temporary2 = sum0 + majority;
                    h = g;
                    g = f;
                    f = e;
                    e = d + temporary1;
                    d = c;
                    c = b;
                    b = a;
                    a = temporary1 + temporary2;
                }
                mState[0] += a;
                mState[1] += b;
                mState[2] += c;
                mState[3] += d;
                mState[4] += e;
                mState[5] += f;
                mState[6] += g;
                mState[7] += h;
            }

            std::array<std::uint32_t, 8> mState = {
                0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
            };
            std::array<std::uint8_t, 64> mBuffer{};
            std::uint64_t mTotalBytes = 0;
            std::size_t mBufferedBytes = 0;
        };

        [[nodiscard]] std::string ToHex(std::span<const std::uint8_t> bytes)
        {
            constexpr char c_HexDigits[] = "0123456789abcdef";
            std::string result;
            result.reserve(bytes.size() * 2u);
            for (const std::uint8_t byte : bytes)
            {
                result.push_back(c_HexDigits[byte >> 4u]);
                result.push_back(c_HexDigits[byte & 0x0fu]);
            }
            return result;
        }

        void HashUnsigned64(Sha256& hash, std::uint64_t value)
        {
            std::array<std::uint8_t, 8> bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index)
            {
                bytes[index] = static_cast<std::uint8_t>(
                    (value >> ((7u - index) * 8u)) & 0xffu);
            }
            hash.Update(bytes);
        }

        void HashString(Sha256& hash, std::string_view value)
        {
            HashUnsigned64(hash, static_cast<std::uint64_t>(value.size()));
            hash.Update(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
        }

        MaterialShaderDiagnostic MakeError(
            std::string code,
            std::string message,
            const std::string& source)
        {
            return {
                .Severity = MaterialShaderDiagnosticSeverity::Error,
                .Code = std::move(code),
                .Message = std::move(message),
                .Source = source,
            };
        }

        bool IsHexHash(std::string_view value)
        {
            return value.size() == 64 &&
                std::all_of(value.begin(), value.end(), [](const unsigned char character)
                {
                    return std::isxdigit(character) != 0;
                });
        }

        bool IsSafeRelativePath(std::string_view value)
        {
            if (value.empty() || value.find('\0') != std::string_view::npos ||
                value.find('\\') != std::string_view::npos)
            {
                return false;
            }
            const std::filesystem::path path(value);
            if (path.is_absolute() || path.has_root_name())
                return false;
            for (const std::filesystem::path& component : path)
            {
                if (component == "..")
                    return false;
                const std::string componentText = component.string();
                if (componentText.empty() || componentText.back() == '.' ||
                    componentText.back() == ' ' ||
                    componentText.find_first_of("<>:\"|?*") != std::string::npos ||
                    Utils::IsWindowsReservedPathComponent(componentText) ||
                    std::any_of(
                        componentText.begin(), componentText.end(), [](char character)
                        {
                            return static_cast<unsigned char>(character) < 32u;
                        }))
                {
                    return false;
                }
            }
            return path.lexically_normal().generic_string() == value;
        }

        bool IsIdentifier(std::string_view value)
        {
            if (value.empty() ||
                (!std::isalpha(static_cast<unsigned char>(value.front())) && value.front() != '_'))
            {
                return false;
            }
            return std::all_of(value.begin() + 1, value.end(), [](const unsigned char character)
            {
                return std::isalnum(character) != 0 || character == '_';
            });
        }

        std::string ToLowerCopy(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
            return value;
        }

        bool IsFloatAbiRepresentable(double value) noexcept
        {
            return std::isfinite(value) &&
                value >= -static_cast<double>(std::numeric_limits<float>::max()) &&
                value <= static_cast<double>(std::numeric_limits<float>::max()) &&
                std::isfinite(static_cast<float>(value));
        }

        bool IsInt32AbiRepresentable(double value) noexcept
        {
            return std::isfinite(value) && std::floor(value) == value &&
                value >= static_cast<double>(std::numeric_limits<std::int32_t>::min()) &&
                value <= static_cast<double>(std::numeric_limits<std::int32_t>::max());
        }

        bool IsBuiltinTextureFallback(std::string_view value)
        {
            const std::string lower = ToLowerCopy(std::string(value));
            return lower == "white" || lower == "black" || lower == "normal";
        }

        const char* ArtifactBackendName(RenderBackend backend) noexcept
        {
            return backend == RenderBackend::Vulkan ? "Vulkan" : "DX12";
        }

        bool IsSupportedTextureExtension(std::string extension)
        {
            extension = ToLowerCopy(std::move(extension));
            return extension == ".png" || extension == ".jpg" ||
                extension == ".jpeg" || extension == ".bmp" ||
                extension == ".tga" || extension == ".hdr" ||
                extension == ".dds";
        }

        template<typename T>
        bool ReadRequired(
            const Json& object,
            const char* field,
            T& value,
            std::vector<MaterialShaderDiagnostic>& diagnostics,
            const std::string& source)
        {
            const auto iterator = object.find(field);
            if (iterator == object.end())
            {
                diagnostics.push_back(MakeError(
                    "Manifest.MissingField",
                    std::string("Missing required field '") + field + "'.",
                    source));
                return false;
            }
            try
            {
                value = iterator->get<T>();
                return true;
            }
            catch (const Json::exception& exception)
            {
                diagnostics.push_back(MakeError(
                    "Manifest.InvalidField",
                    std::string("Invalid field '") + field + "': " + exception.what(),
                    source));
                return false;
            }
        }

        template<typename Enum>
        bool TryParseEnum(std::string_view value, Enum& output);

#define HE_PARSE_ENUM(EnumType, ...) \
        template<> bool TryParseEnum<EnumType>(std::string_view value, EnumType& output) \
        { \
            const std::pair<std::string_view, EnumType> values[] = { __VA_ARGS__ }; \
            for (const auto& [name, candidate] : values) \
            { \
                if (value == name) \
                { \
                    output = candidate; \
                    return true; \
                } \
            } \
            return false; \
        }

        HE_PARSE_ENUM(MaterialShaderDomain,
            { "Surface", MaterialShaderDomain::Surface });
        HE_PARSE_ENUM(MaterialShaderShadingModel,
            { "Lit", MaterialShaderShadingModel::Lit },
            { "Unlit", MaterialShaderShadingModel::Unlit });
        HE_PARSE_ENUM(MaterialShaderBlendMode,
            { "Opaque", MaterialShaderBlendMode::Opaque },
            { "Mask", MaterialShaderBlendMode::Mask },
            { "Transparent", MaterialShaderBlendMode::Transparent });
        HE_PARSE_ENUM(MaterialShaderCullMode,
            { "Back", MaterialShaderCullMode::Back },
            { "None", MaterialShaderCullMode::None });
        HE_PARSE_ENUM(MaterialShaderDescriptorMode,
            { "Baseline", MaterialShaderDescriptorMode::Baseline },
            { "HeapDirect", MaterialShaderDescriptorMode::HeapDirect });
        HE_PARSE_ENUM(MaterialShaderVertexPath,
            { "Static", MaterialShaderVertexPath::Static },
            { "Skinned", MaterialShaderVertexPath::Skinned },
            { "GpuDriven", MaterialShaderVertexPath::GpuDriven });
        HE_PARSE_ENUM(MaterialShaderPass,
            { "Forward", MaterialShaderPass::Forward },
            { "Depth", MaterialShaderPass::Depth },
            { "Shadow", MaterialShaderPass::Shadow },
            { "Outline", MaterialShaderPass::Outline });
        HE_PARSE_ENUM(MaterialShaderPropertyType,
            { "Float", MaterialShaderPropertyType::Float },
            { "Float2", MaterialShaderPropertyType::Float2 },
            { "Float3", MaterialShaderPropertyType::Float3 },
            { "Color", MaterialShaderPropertyType::Color },
            { "Bool", MaterialShaderPropertyType::Bool },
            { "Int", MaterialShaderPropertyType::Int },
            { "Texture2D", MaterialShaderPropertyType::Texture2D });
        HE_PARSE_ENUM(MaterialShaderTextureSemantic,
            { "None", MaterialShaderTextureSemantic::None },
            { "BaseColor", MaterialShaderTextureSemantic::BaseColor },
            { "Normal", MaterialShaderTextureSemantic::Normal },
            { "MetallicRoughness", MaterialShaderTextureSemantic::MetallicRoughness },
            { "Occlusion", MaterialShaderTextureSemantic::Occlusion },
            { "Emissive", MaterialShaderTextureSemantic::Emissive },
            { "Data", MaterialShaderTextureSemantic::Data });
        HE_PARSE_ENUM(MaterialShaderColorSpace,
            { "Linear", MaterialShaderColorSpace::Linear },
            { "SRGB", MaterialShaderColorSpace::Srgb });
        HE_PARSE_ENUM(MaterialShaderDiagnosticSeverity,
            { "Info", MaterialShaderDiagnosticSeverity::Info },
            { "Warning", MaterialShaderDiagnosticSeverity::Warning },
            { "Error", MaterialShaderDiagnosticSeverity::Error });
        HE_PARSE_ENUM(RenderBackend,
            { "DX12", RenderBackend::DX12 },
            { "Vulkan", RenderBackend::Vulkan });
        HE_PARSE_ENUM(ShaderBinaryFormat,
            { "DXIL", ShaderBinaryFormat::Dxil },
            { "SPIR-V", ShaderBinaryFormat::SpirV });

#undef HE_PARSE_ENUM

        template<typename Enum>
        bool ReadEnum(
            const Json& object,
            const char* field,
            Enum& value,
            std::vector<MaterialShaderDiagnostic>& diagnostics,
            const std::string& source)
        {
            std::string text;
            if (!ReadRequired(object, field, text, diagnostics, source))
                return false;
            if (TryParseEnum(text, value))
                return true;
            diagnostics.push_back(MakeError(
                "Manifest.InvalidEnum",
                std::string("Unsupported '") + field + "' value '" + text + "'.",
                source));
            return false;
        }

        Json DiagnosticToJson(const MaterialShaderDiagnostic& diagnostic)
        {
            return {
                { "Severity", ToString(diagnostic.Severity) },
                { "Code", diagnostic.Code },
                { "Message", diagnostic.Message },
                { "Source", diagnostic.Source },
                { "Line", diagnostic.Line },
                { "Column", diagnostic.Column },
            };
        }

        Json DefaultToJson(const MaterialShaderProperty& property)
        {
            switch (property.Type)
            {
                case MaterialShaderPropertyType::Bool:
                    return property.Default.Boolean;
                case MaterialShaderPropertyType::Int:
                    return property.Default.Integer;
                case MaterialShaderPropertyType::Texture2D:
                    return property.Default.Texture;
                case MaterialShaderPropertyType::Float:
                    return property.Default.Numeric.empty() ? Json(0.0) : Json(property.Default.Numeric.front());
                default:
                    return property.Default.Numeric;
            }
        }

        void ParseDefault(
            const Json& value,
            MaterialShaderProperty& property,
            std::vector<MaterialShaderDiagnostic>& diagnostics,
            const std::string& source)
        {
            try
            {
                switch (property.Type)
                {
                    case MaterialShaderPropertyType::Bool:
                        property.Default.Boolean = value.get<bool>();
                        break;
                    case MaterialShaderPropertyType::Int:
                        property.Default.Integer = value.get<std::int64_t>();
                        break;
                    case MaterialShaderPropertyType::Texture2D:
                        property.Default.Texture = value.get<std::string>();
                        break;
                    case MaterialShaderPropertyType::Float:
                        property.Default.Numeric = { value.get<double>() };
                        break;
                    default:
                        property.Default.Numeric = value.get<std::vector<double>>();
                        break;
                }
            }
            catch (const Json::exception& exception)
            {
                diagnostics.push_back(MakeError(
                    "Manifest.InvalidDefault",
                    "Invalid property default: " + std::string(exception.what()),
                    source));
            }
        }
    }

    bool MaterialShaderManifestLoadResult::Succeeded() const noexcept
    {
        return FindFirstMaterialShaderError(Diagnostics) == nullptr &&
            FindFirstMaterialShaderError(Manifest.Diagnostics) == nullptr;
    }

    const char* ToString(MaterialShaderDomain value) noexcept
    {
        return value == MaterialShaderDomain::Surface ? "Surface" : "Invalid";
    }

    const char* ToString(MaterialShaderShadingModel value) noexcept
    {
        switch (value)
        {
            case MaterialShaderShadingModel::Lit: return "Lit";
            case MaterialShaderShadingModel::Unlit: return "Unlit";
        }
        return "Invalid";
    }

    const char* ToString(MaterialShaderBlendMode value) noexcept
    {
        switch (value)
        {
            case MaterialShaderBlendMode::Opaque: return "Opaque";
            case MaterialShaderBlendMode::Mask: return "Mask";
            case MaterialShaderBlendMode::Transparent: return "Transparent";
        }
        return "Invalid";
    }

    const char* ToString(MaterialShaderCullMode value) noexcept
    {
        switch (value)
        {
            case MaterialShaderCullMode::Back: return "Back";
            case MaterialShaderCullMode::None: return "None";
        }
        return "Invalid";
    }

    const char* ToString(MaterialShaderDescriptorMode value) noexcept
    {
        switch (value)
        {
            case MaterialShaderDescriptorMode::Baseline: return "Baseline";
            case MaterialShaderDescriptorMode::HeapDirect: return "HeapDirect";
        }
        return "Invalid";
    }

    const char* ToString(MaterialShaderVertexPath value) noexcept
    {
        switch (value)
        {
            case MaterialShaderVertexPath::Static: return "Static";
            case MaterialShaderVertexPath::Skinned: return "Skinned";
            case MaterialShaderVertexPath::GpuDriven: return "GpuDriven";
        }
        return "Invalid";
    }

    const char* ToString(MaterialShaderPass value) noexcept
    {
        switch (value)
        {
            case MaterialShaderPass::Forward: return "Forward";
            case MaterialShaderPass::Depth: return "Depth";
            case MaterialShaderPass::Shadow: return "Shadow";
            case MaterialShaderPass::Outline: return "Outline";
        }
        return "Invalid";
    }

    const char* ToString(MaterialShaderPropertyType value) noexcept
    {
        switch (value)
        {
            case MaterialShaderPropertyType::Float: return "Float";
            case MaterialShaderPropertyType::Float2: return "Float2";
            case MaterialShaderPropertyType::Float3: return "Float3";
            case MaterialShaderPropertyType::Color: return "Color";
            case MaterialShaderPropertyType::Bool: return "Bool";
            case MaterialShaderPropertyType::Int: return "Int";
            case MaterialShaderPropertyType::Texture2D: return "Texture2D";
        }
        return "Invalid";
    }

    const char* ToString(MaterialShaderTextureSemantic value) noexcept
    {
        switch (value)
        {
            case MaterialShaderTextureSemantic::None: return "None";
            case MaterialShaderTextureSemantic::BaseColor: return "BaseColor";
            case MaterialShaderTextureSemantic::Normal: return "Normal";
            case MaterialShaderTextureSemantic::MetallicRoughness: return "MetallicRoughness";
            case MaterialShaderTextureSemantic::Occlusion: return "Occlusion";
            case MaterialShaderTextureSemantic::Emissive: return "Emissive";
            case MaterialShaderTextureSemantic::Data: return "Data";
        }
        return "Invalid";
    }

    const char* ToString(MaterialShaderColorSpace value) noexcept
    {
        switch (value)
        {
            case MaterialShaderColorSpace::Linear: return "Linear";
            case MaterialShaderColorSpace::Srgb: return "SRGB";
        }
        return "Invalid";
    }

    const char* ToString(MaterialShaderDiagnosticSeverity value) noexcept
    {
        switch (value)
        {
            case MaterialShaderDiagnosticSeverity::Info: return "Info";
            case MaterialShaderDiagnosticSeverity::Warning: return "Warning";
            case MaterialShaderDiagnosticSeverity::Error: return "Error";
        }
        return "Invalid";
    }

    const MaterialShaderDiagnostic* FindFirstMaterialShaderError(
        std::span<const MaterialShaderDiagnostic> diagnostics) noexcept
    {
        const auto error = std::find_if(
            diagnostics.begin(), diagnostics.end(), [](const MaterialShaderDiagnostic& diagnostic)
            {
                return diagnostic.Severity == MaterialShaderDiagnosticSeverity::Error;
            });
        return error == diagnostics.end() ? nullptr : &*error;
    }

    std::string FormatMaterialShaderDiagnostics(
        std::span<const MaterialShaderDiagnostic> diagnostics)
    {
        std::ostringstream formatted;
        for (std::size_t index = 0; index < diagnostics.size(); ++index)
        {
            const MaterialShaderDiagnostic& diagnostic = diagnostics[index];
            if (index != 0)
                formatted << '\n';
            if (!diagnostic.Source.empty())
            {
                formatted << diagnostic.Source;
                if (diagnostic.Line != 0)
                {
                    formatted << ':' << diagnostic.Line;
                    if (diagnostic.Column != 0)
                        formatted << ':' << diagnostic.Column;
                }
                formatted << ": ";
            }
            formatted << ToString(diagnostic.Severity);
            if (!diagnostic.Code.empty())
                formatted << " [" << diagnostic.Code << ']';
            if (!diagnostic.Message.empty())
                formatted << ": " << diagnostic.Message;
        }
        return formatted.str();
    }

    std::uint32_t MaterialShaderPropertyByteSize(MaterialShaderPropertyType type) noexcept
    {
        switch (type)
        {
            case MaterialShaderPropertyType::Float:
            case MaterialShaderPropertyType::Bool:
            case MaterialShaderPropertyType::Int:
            case MaterialShaderPropertyType::Texture2D:
                return 4;
            case MaterialShaderPropertyType::Float2: return 8;
            case MaterialShaderPropertyType::Float3: return 12;
            case MaterialShaderPropertyType::Color: return 16;
        }
        return 0;
    }

    bool IsValidMaterialShaderTextureDefault(
        std::string_view value,
        std::string* error)
    {
        if (error)
            error->clear();
        if (IsBuiltinTextureFallback(value))
            return true;
        if (!IsSafeRelativePath(value))
        {
            if (error)
                *error = "Texture default must be a normalized content-relative path.";
            return false;
        }
        if (!value.starts_with("Assets/") ||
            !IsSupportedTextureExtension(std::filesystem::path(value).extension().string()))
        {
            if (error)
            {
                *error = "Texture default must use an Assets-relative supported texture path "
                    "(.png, .jpg, .jpeg, .bmp, .tga, .hdr, or .dds).";
            }
            return false;
        }
        return true;
    }

    MaterialShaderArtifactKey BuildMaterialShaderArtifactKey(
        const MaterialShaderArtifact& artifact)
    {
        return {
            .Pass = artifact.Pass,
            .VertexPath = artifact.VertexPath,
            .DescriptorMode = artifact.DescriptorMode,
            .Stage = artifact.Stage == "vs"
                ? MaterialShaderArtifactStage::Vertex
                : artifact.Stage == "ps"
                    ? MaterialShaderArtifactStage::Pixel
                    : MaterialShaderArtifactStage::Invalid,
        };
    }

    const MaterialShaderArtifactSet* FindMaterialShaderArtifactSet(
        const MaterialShaderManifest& manifest,
        RenderBackend backend,
        ShaderBinaryFormat binaryFormat) noexcept
    {
        const auto found = std::find_if(
            manifest.ArtifactSets.begin(),
            manifest.ArtifactSets.end(),
            [backend, binaryFormat](const MaterialShaderArtifactSet& set)
            {
                return set.Backend == backend && set.BinaryFormat == binaryFormat;
            });
        return found == manifest.ArtifactSets.end() ? nullptr : &*found;
    }

    std::span<const MaterialShaderArtifact> GetMaterialShaderArtifacts(
        const MaterialShaderManifest& manifest,
        RenderBackend backend,
        ShaderBinaryFormat binaryFormat) noexcept
    {
        if (backend == RenderBackend::DX12 &&
            binaryFormat == ShaderBinaryFormat::Dxil &&
            !manifest.Artifacts.empty())
        {
            return manifest.Artifacts;
        }
        if (const MaterialShaderArtifactSet* set = FindMaterialShaderArtifactSet(
                manifest, backend, binaryFormat))
        {
            return set->Artifacts;
        }
        if (manifest.ArtifactSets.empty() && backend == RenderBackend::DX12 &&
            binaryFormat == ShaderBinaryFormat::Dxil)
        {
            return manifest.Artifacts;
        }
        return {};
    }

    std::string ComputeMaterialShaderSha256(std::span<const std::byte> bytes)
    {
        Sha256 hash;
        hash.Update(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
        return ToHex(hash.Finalize());
    }

    bool ComputeMaterialShaderFileSha256(
        const std::filesystem::path& path,
        std::string& hash,
        std::string& error)
    {
        hash.clear();
        error.clear();
        std::ifstream stream(path, std::ios::in | std::ios::binary);
        if (!stream)
        {
            error = "Failed to open file for SHA-256: " + path.generic_string();
            return false;
        }

        try
        {
            Sha256 sha256;
            std::array<char, 64u * 1024u> buffer{};
            while (stream)
            {
                stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize count = stream.gcount();
                if (count > 0)
                {
                    sha256.Update(std::span<const std::uint8_t>(
                        reinterpret_cast<const std::uint8_t*>(buffer.data()),
                        static_cast<std::size_t>(count)));
                }
            }
            if (!stream.eof())
            {
                error = "Failed while reading file for SHA-256: " + path.generic_string();
                return false;
            }
            hash = ToHex(sha256.Finalize());
            return true;
        }
        catch (const std::exception& exception)
        {
            error = "Failed to compute SHA-256 for '" + path.generic_string() + "': " +
                exception.what();
            return false;
        }
    }

    std::string BuildMaterialShaderBindingSignatureHash(
        const MaterialShaderManifest& manifest)
    {
        std::ostringstream signature;
        signature.imbue(std::locale::classic());
        signature << "abi=" << manifest.MaterialAbiVersion
                  << ";alignment=" << manifest.ParameterBlock.Alignment
                  << ";size=" << manifest.ParameterBlock.ByteSize << '\n';
        for (const MaterialShaderProperty& property : manifest.Properties)
        {
            signature << property.StableId << '|'
                      << ToString(property.Type) << '|'
                      << property.ArrayCount << '|'
                      << property.RowOffset << '|'
                      << property.ByteOffset << '|'
                      << property.ByteSize << '|'
                      << property.Binding.RegisterType << '|'
                      << property.Binding.RegisterIndex << '|'
                      << property.Binding.RegisterSpace << '|'
                      << property.Binding.DescriptorIndex << '\n';
        }

        Sha256 hash;
        HashString(hash, "HEngine.MaterialShader.BindingSignature.v1");
        HashString(hash, signature.str());
        return ToHex(hash.Finalize());
    }

    nlohmann::json MaterialShaderManifestToJson(const MaterialShaderManifest& manifest)
    {
        Json properties = Json::array();
        for (const MaterialShaderProperty& property : manifest.Properties)
        {
            Json range = nullptr;
            if (property.Range.bEnabled)
            {
                range = {
                    { "Min", property.Range.Minimum },
                    { "Max", property.Range.Maximum },
                };
            }
            properties.push_back({
                { "StableId", property.StableId },
                { "Name", property.Name },
                { "DisplayName", property.DisplayName },
                { "Group", property.Group },
                { "Source", property.Source },
                { "Line", property.Line },
                { "Column", property.Column },
                { "Type", ToString(property.Type) },
                { "ArrayCount", property.ArrayCount },
                { "RowOffset", property.RowOffset },
                { "ByteOffset", property.ByteOffset },
                { "ByteSize", property.ByteSize },
                { "Default", DefaultToJson(property) },
                { "Range", std::move(range) },
                { "TextureSemantic", ToString(property.TextureSemantic) },
                { "ColorSpace", ToString(property.ColorSpace) },
                { "Fallback", property.Fallback },
                { "Binding", {
                    { "RegisterType", property.Binding.RegisterType },
                    { "RegisterIndex", property.Binding.RegisterIndex },
                    { "RegisterSpace", property.Binding.RegisterSpace },
                    { "DescriptorIndex", property.Binding.DescriptorIndex },
                } },
            });
        }

        const auto artifactsToJson = [](std::span<const MaterialShaderArtifact> artifacts)
        {
            Json result = Json::array();
            for (const MaterialShaderArtifact& artifact : artifacts)
            {
                result.push_back({
                    { "Pass", ToString(artifact.Pass) },
                    { "Stage", artifact.Stage },
                    { "EntryPoint", artifact.EntryPoint },
                    { "TargetProfile", artifact.TargetProfile },
                    { "Permutation", artifact.Permutation },
                    { "DescriptorMode", ToString(artifact.DescriptorMode) },
                    { "VertexPath", ToString(artifact.VertexPath) },
                    { "RelativePath", artifact.RelativePath },
                    { "ContentHash", artifact.ContentHash },
                });
            }
            return result;
        };

        Json artifactSets = Json::array();
        if (manifest.ArtifactSets.empty() && !manifest.Artifacts.empty())
        {
            artifactSets.push_back({
                { "Backend", "DX12" },
                { "BinaryFormat", "DXIL" },
                { "Artifacts", artifactsToJson(manifest.Artifacts) },
            });
        }
        else
        {
            for (const MaterialShaderArtifactSet& set : manifest.ArtifactSets)
            {
                const std::span<const MaterialShaderArtifact> artifacts =
                    set.Backend == RenderBackend::DX12 &&
                        set.BinaryFormat == ShaderBinaryFormat::Dxil &&
                        !manifest.Artifacts.empty()
                    ? std::span<const MaterialShaderArtifact>(manifest.Artifacts)
                    : std::span<const MaterialShaderArtifact>(set.Artifacts);
                artifactSets.push_back({
                    { "Backend", ArtifactBackendName(set.Backend) },
                    { "BinaryFormat", std::string(ToString(set.BinaryFormat)) },
                    { "Artifacts", artifactsToJson(artifacts) },
                });
            }
        }

        return {
            { "FormatVersion", manifest.FormatVersion },
            { "MaterialAbiVersion", manifest.MaterialAbiVersion },
            { "SourcePath", manifest.SourcePath },
            { "RecursiveSourceHash", manifest.RecursiveSourceHash },
            { "Domain", ToString(manifest.Domain) },
            { "ShadingModel", ToString(manifest.ShadingModel) },
            { "BlendMode", ToString(manifest.BlendMode) },
            { "CullMode", ToString(manifest.CullMode) },
            { "DepthWrite", manifest.bDepthWrite },
            { "CastsShadow", manifest.bCastsShadow },
            { "DescriptorMode", ToString(manifest.DescriptorMode) },
            { "Properties", std::move(properties) },
            { "ParameterBlock", {
                { "ByteSize", manifest.ParameterBlock.ByteSize },
                { "Alignment", manifest.ParameterBlock.Alignment },
            } },
            { "BindingSignatureHash", manifest.BindingSignatureHash },
            { "ArtifactSets", std::move(artifactSets) },
            { "Diagnostics", MaterialShaderDiagnosticsToJson(manifest.Diagnostics) },
        };
    }

    nlohmann::json MaterialShaderDiagnosticsToJson(
        const std::vector<MaterialShaderDiagnostic>& diagnostics)
    {
        Json result = Json::array();
        for (const MaterialShaderDiagnostic& diagnostic : diagnostics)
            result.push_back(DiagnosticToJson(diagnostic));
        return result;
    }

    MaterialShaderManifestLoadResult ParseMaterialShaderManifest(
        const nlohmann::json& json,
        std::string sourceName)
    {
        MaterialShaderManifestLoadResult result;
        if (!json.is_object())
        {
            result.Diagnostics.push_back(MakeError(
                "Manifest.RootType", "Material shader manifest root must be an object.", sourceName));
            return result;
        }

        MaterialShaderManifest& manifest = result.Manifest;
        if (!ReadRequired(
                json, "FormatVersion", manifest.FormatVersion, result.Diagnostics, sourceName))
        {
            return result;
        }
        if (manifest.FormatVersion != MaterialShaderManifestFormatVersion)
        {
            result.Diagnostics.push_back(MakeError(
                "Manifest.FormatVersion",
                "Unsupported material shader manifest FormatVersion " +
                    std::to_string(manifest.FormatVersion) + "; expected " +
                    std::to_string(MaterialShaderManifestFormatVersion) + ".",
                sourceName));
            return result;
        }
        ReadRequired(json, "MaterialAbiVersion", manifest.MaterialAbiVersion, result.Diagnostics, sourceName);
        ReadRequired(json, "SourcePath", manifest.SourcePath, result.Diagnostics, sourceName);
        ReadRequired(json, "RecursiveSourceHash", manifest.RecursiveSourceHash, result.Diagnostics, sourceName);
        ReadEnum(json, "Domain", manifest.Domain, result.Diagnostics, sourceName);
        ReadEnum(json, "ShadingModel", manifest.ShadingModel, result.Diagnostics, sourceName);
        ReadEnum(json, "BlendMode", manifest.BlendMode, result.Diagnostics, sourceName);
        ReadEnum(json, "CullMode", manifest.CullMode, result.Diagnostics, sourceName);
        ReadRequired(json, "DepthWrite", manifest.bDepthWrite, result.Diagnostics, sourceName);
        ReadRequired(json, "CastsShadow", manifest.bCastsShadow, result.Diagnostics, sourceName);
        ReadEnum(json, "DescriptorMode", manifest.DescriptorMode, result.Diagnostics, sourceName);
        ReadRequired(json, "BindingSignatureHash", manifest.BindingSignatureHash, result.Diagnostics, sourceName);

        const auto parameterBlock = json.find("ParameterBlock");
        if (parameterBlock == json.end() || !parameterBlock->is_object())
        {
            result.Diagnostics.push_back(MakeError(
                "Manifest.ParameterBlock", "ParameterBlock must be an object.", sourceName));
        }
        else
        {
            ReadRequired(*parameterBlock, "ByteSize", manifest.ParameterBlock.ByteSize, result.Diagnostics, sourceName);
            ReadRequired(*parameterBlock, "Alignment", manifest.ParameterBlock.Alignment, result.Diagnostics, sourceName);
        }

        const auto properties = json.find("Properties");
        if (properties == json.end() || !properties->is_array())
        {
            result.Diagnostics.push_back(MakeError(
                "Manifest.Properties", "Properties must be an array.", sourceName));
        }
        else
        {
            for (const Json& propertyJson : *properties)
            {
                if (!propertyJson.is_object())
                {
                    result.Diagnostics.push_back(MakeError(
                        "Manifest.PropertyType", "Each property must be an object.", sourceName));
                    continue;
                }

                MaterialShaderProperty property;
                ReadRequired(propertyJson, "StableId", property.StableId, result.Diagnostics, sourceName);
                ReadRequired(propertyJson, "Name", property.Name, result.Diagnostics, sourceName);
                ReadRequired(propertyJson, "DisplayName", property.DisplayName, result.Diagnostics, sourceName);
                ReadRequired(propertyJson, "Group", property.Group, result.Diagnostics, sourceName);
                ReadRequired(propertyJson, "Source", property.Source, result.Diagnostics, sourceName);
                ReadRequired(propertyJson, "Line", property.Line, result.Diagnostics, sourceName);
                ReadRequired(propertyJson, "Column", property.Column, result.Diagnostics, sourceName);
                ReadEnum(propertyJson, "Type", property.Type, result.Diagnostics, sourceName);
                ReadRequired(propertyJson, "ArrayCount", property.ArrayCount, result.Diagnostics, sourceName);
                ReadRequired(propertyJson, "RowOffset", property.RowOffset, result.Diagnostics, sourceName);
                ReadRequired(propertyJson, "ByteOffset", property.ByteOffset, result.Diagnostics, sourceName);
                ReadRequired(propertyJson, "ByteSize", property.ByteSize, result.Diagnostics, sourceName);
                ReadEnum(propertyJson, "TextureSemantic", property.TextureSemantic, result.Diagnostics, sourceName);
                ReadEnum(propertyJson, "ColorSpace", property.ColorSpace, result.Diagnostics, sourceName);
                ReadRequired(propertyJson, "Fallback", property.Fallback, result.Diagnostics, sourceName);

                if (const auto defaultValue = propertyJson.find("Default"); defaultValue != propertyJson.end())
                    ParseDefault(*defaultValue, property, result.Diagnostics, sourceName);
                else
                    result.Diagnostics.push_back(MakeError(
                        "Manifest.MissingDefault", "Property Default is required.", sourceName));

                if (const auto range = propertyJson.find("Range"); range != propertyJson.end() && !range->is_null())
                {
                    if (!range->is_object())
                    {
                        result.Diagnostics.push_back(MakeError(
                            "Manifest.RangeType", "Property Range must be null or an object.", sourceName));
                    }
                    else
                    {
                        property.Range.bEnabled = true;
                        ReadRequired(*range, "Min", property.Range.Minimum, result.Diagnostics, sourceName);
                        ReadRequired(*range, "Max", property.Range.Maximum, result.Diagnostics, sourceName);
                    }
                }

                const auto binding = propertyJson.find("Binding");
                if (binding == propertyJson.end() || !binding->is_object())
                {
                    result.Diagnostics.push_back(MakeError(
                        "Manifest.Binding", "Property Binding must be an object.", sourceName));
                }
                else
                {
                    ReadRequired(*binding, "RegisterType", property.Binding.RegisterType, result.Diagnostics, sourceName);
                    ReadRequired(*binding, "RegisterIndex", property.Binding.RegisterIndex, result.Diagnostics, sourceName);
                    ReadRequired(*binding, "RegisterSpace", property.Binding.RegisterSpace, result.Diagnostics, sourceName);
                    ReadRequired(*binding, "DescriptorIndex", property.Binding.DescriptorIndex, result.Diagnostics, sourceName);
                }
                manifest.Properties.push_back(std::move(property));
            }
        }

        const auto parseArtifacts = [&](
            const Json& artifactsJson,
            std::vector<MaterialShaderArtifact>& output)
        {
            if (!artifactsJson.is_array())
            {
                result.Diagnostics.push_back(MakeError(
                    "Manifest.Artifacts", "Artifacts must be an array.", sourceName));
                return;
            }
            for (const Json& artifactJson : artifactsJson)
            {
                if (!artifactJson.is_object())
                {
                    result.Diagnostics.push_back(MakeError(
                        "Manifest.ArtifactType", "Each artifact must be an object.", sourceName));
                    continue;
                }
                MaterialShaderArtifact artifact;
                ReadEnum(artifactJson, "Pass", artifact.Pass, result.Diagnostics, sourceName);
                ReadRequired(artifactJson, "Stage", artifact.Stage, result.Diagnostics, sourceName);
                ReadRequired(artifactJson, "EntryPoint", artifact.EntryPoint, result.Diagnostics, sourceName);
                ReadRequired(artifactJson, "TargetProfile", artifact.TargetProfile, result.Diagnostics, sourceName);
                ReadRequired(artifactJson, "Permutation", artifact.Permutation, result.Diagnostics, sourceName);
                ReadEnum(artifactJson, "DescriptorMode", artifact.DescriptorMode, result.Diagnostics, sourceName);
                ReadEnum(artifactJson, "VertexPath", artifact.VertexPath, result.Diagnostics, sourceName);
                ReadRequired(artifactJson, "RelativePath", artifact.RelativePath, result.Diagnostics, sourceName);
                ReadRequired(artifactJson, "ContentHash", artifact.ContentHash, result.Diagnostics, sourceName);
                output.push_back(std::move(artifact));
            }
        };

        const auto artifactSets = json.find("ArtifactSets");
        if (artifactSets == json.end() || !artifactSets->is_array())
        {
            result.Diagnostics.push_back(MakeError(
                "Manifest.ArtifactSets", "ArtifactSets must be an array.", sourceName));
        }
        else
        {
            for (const Json& setJson : *artifactSets)
            {
                if (!setJson.is_object())
                {
                    result.Diagnostics.push_back(MakeError(
                        "Manifest.ArtifactSetType",
                        "Each ArtifactSets entry must be an object.", sourceName));
                    continue;
                }
                MaterialShaderArtifactSet set;
                ReadEnum(setJson, "Backend", set.Backend, result.Diagnostics, sourceName);
                ReadEnum(setJson, "BinaryFormat", set.BinaryFormat, result.Diagnostics, sourceName);
                const auto artifacts = setJson.find("Artifacts");
                if (artifacts == setJson.end())
                {
                    result.Diagnostics.push_back(MakeError(
                        "Manifest.MissingField",
                        "Missing required field 'Artifacts'.", sourceName));
                }
                else
                {
                    parseArtifacts(*artifacts, set.Artifacts);
                }
                manifest.ArtifactSets.push_back(std::move(set));
            }
        }

        if (const MaterialShaderArtifactSet* dxil = FindMaterialShaderArtifactSet(
                manifest, RenderBackend::DX12, ShaderBinaryFormat::Dxil))
        {
            manifest.Artifacts = dxil->Artifacts;
        }

        if (const auto diagnostics = json.find("Diagnostics"); diagnostics != json.end())
        {
            if (!diagnostics->is_array())
            {
                result.Diagnostics.push_back(MakeError(
                    "Manifest.Diagnostics", "Diagnostics must be an array.", sourceName));
            }
            else
            {
                for (std::size_t index = 0; index < diagnostics->size(); ++index)
                {
                    const Json& diagnosticJson = (*diagnostics)[index];
                    if (!diagnosticJson.is_object())
                    {
                        result.Diagnostics.push_back(MakeError(
                            "Manifest.DiagnosticType",
                            "Diagnostics[" + std::to_string(index) + "] must be an object.",
                            sourceName));
                        continue;
                    }
                    MaterialShaderDiagnostic diagnostic;
                    const bool bSeverityValid = ReadEnum(
                        diagnosticJson, "Severity", diagnostic.Severity, result.Diagnostics, sourceName);
                    const bool bCodeValid = ReadRequired(
                        diagnosticJson, "Code", diagnostic.Code, result.Diagnostics, sourceName);
                    const bool bMessageValid = ReadRequired(
                        diagnosticJson, "Message", diagnostic.Message, result.Diagnostics, sourceName);
                    const bool bSourceValid = ReadRequired(
                        diagnosticJson, "Source", diagnostic.Source, result.Diagnostics, sourceName);
                    const bool bLineValid = ReadRequired(
                        diagnosticJson, "Line", diagnostic.Line, result.Diagnostics, sourceName);
                    const bool bColumnValid = ReadRequired(
                        diagnosticJson, "Column", diagnostic.Column, result.Diagnostics, sourceName);
                    if (bSeverityValid && bCodeValid && bMessageValid && bSourceValid &&
                        bLineValid && bColumnValid)
                    {
                        manifest.Diagnostics.push_back(std::move(diagnostic));
                    }
                }
            }
        }
        result.Diagnostics.insert(
            result.Diagnostics.end(),
            manifest.Diagnostics.begin(),
            manifest.Diagnostics.end());

        try
        {
            std::vector<MaterialShaderDiagnostic> validation =
                ValidateMaterialShaderManifest(manifest, sourceName);
            result.Diagnostics.insert(
                result.Diagnostics.end(),
                std::make_move_iterator(validation.begin()),
                std::make_move_iterator(validation.end()));
        }
        catch (const std::exception& exception)
        {
            result.Diagnostics.push_back(MakeError(
                "Manifest.Validation",
                "Material shader manifest validation failed: " + std::string(exception.what()),
                sourceName));
        }
        return result;
    }

    MaterialShaderManifestLoadResult LoadMaterialShaderManifest(const std::filesystem::path& path)
    {
        MaterialShaderManifestLoadResult result;
        std::ifstream stream(path, std::ios::in | std::ios::binary);
        if (!stream)
        {
            result.Diagnostics.push_back(MakeError(
                "Manifest.Open", "Failed to open material shader manifest.", path.generic_string()));
            return result;
        }

        try
        {
            const Json json = Json::parse(stream, nullptr, true, true);
            return ParseMaterialShaderManifest(json, path.generic_string());
        }
        catch (const std::exception& exception)
        {
            result.Diagnostics.push_back(MakeError(
                "Manifest.Json",
                "Invalid material shader manifest JSON: " + std::string(exception.what()),
                path.generic_string()));
            return result;
        }
    }

    std::vector<MaterialShaderDiagnostic> ValidateMaterialShaderManifest(
        const MaterialShaderManifest& manifest,
        std::string sourceName)
    {
        std::vector<MaterialShaderDiagnostic> diagnostics;
        const auto add = [&](std::string code, std::string message)
        {
            diagnostics.push_back(MakeError(std::move(code), std::move(message), sourceName));
        };

        if (manifest.FormatVersion != MaterialShaderManifestFormatVersion)
            add("Manifest.FormatVersion", "Unsupported material shader manifest FormatVersion.");
        if (manifest.MaterialAbiVersion != MaterialShaderAbiVersion)
            add("Manifest.AbiVersion", "Unsupported MaterialAbiVersion.");
        if (std::string_view(ToString(manifest.Domain)) == "Invalid" ||
            std::string_view(ToString(manifest.ShadingModel)) == "Invalid" ||
            std::string_view(ToString(manifest.BlendMode)) == "Invalid" ||
            std::string_view(ToString(manifest.CullMode)) == "Invalid" ||
            std::string_view(ToString(manifest.DescriptorMode)) == "Invalid")
        {
            add("Manifest.MaterialEnum", "Material shader manifest contains an invalid material enum value.");
        }
        if ((manifest.BlendMode == MaterialShaderBlendMode::Opaque ||
             manifest.BlendMode == MaterialShaderBlendMode::Mask) &&
            !manifest.bDepthWrite)
        {
            add("Manifest.RenderState",
                "Material shader ABI v1 requires DepthWrite=true for Opaque and Mask materials.");
        }
        if (manifest.BlendMode == MaterialShaderBlendMode::Transparent && manifest.bDepthWrite)
        {
            add("Manifest.RenderState",
                "Material shader ABI v1 requires DepthWrite=false for Transparent materials.");
        }
        if (manifest.BlendMode == MaterialShaderBlendMode::Transparent && manifest.bCastsShadow)
        {
            add("Manifest.RenderState",
                "Material shader ABI v1 does not support Transparent materials casting shadows.");
        }
        if (!IsSafeRelativePath(manifest.SourcePath) ||
            ToLowerCopy(std::filesystem::path(manifest.SourcePath).extension().string()) != ".hlsl")
        {
            add("Manifest.SourcePath", "SourcePath must be a normalized relative .hlsl path.");
        }
        if (!IsHexHash(manifest.RecursiveSourceHash))
            add("Manifest.RecursiveHash", "RecursiveSourceHash must be a 64-character hexadecimal SHA-256 hash.");
        if (!IsHexHash(manifest.BindingSignatureHash))
            add("Manifest.BindingHash", "BindingSignatureHash must be a 64-character hexadecimal SHA-256 hash.");
        else if (ToLowerCopy(manifest.BindingSignatureHash) !=
            BuildMaterialShaderBindingSignatureHash(manifest))
        {
            add("Manifest.BindingSignature",
                "BindingSignatureHash does not match the deterministic material ABI layout signature.");
        }
        if (manifest.ParameterBlock.Alignment != MaterialShaderParameterAlignment)
            add("Manifest.ParameterAlignment", "ParameterBlock alignment must be 16 bytes.");
        if (manifest.ParameterBlock.ByteSize % MaterialShaderParameterAlignment != 0)
            add("Manifest.ParameterSize", "ParameterBlock ByteSize must be 16-byte aligned.");
        if (manifest.Properties.size() > MaterialParameterMaximumPropertyCount)
        {
            add("Manifest.PropertyLimit", "Material shader ABI v1 supports at most 64 properties.");
        }
        if (manifest.ParameterBlock.ByteSize > MaterialParameterMaximumByteSize)
        {
            add("Manifest.ParameterBlockLimit",
                "Material shader ABI v1 parameter blocks cannot exceed 1024 bytes.");
        }

        std::unordered_set<std::string> names;
        std::unordered_set<std::string> stableIds;
        std::unordered_set<std::uint32_t> rows;
        std::size_t textureCount = 0;
        for (const MaterialShaderProperty& property : manifest.Properties)
        {
            if (std::string_view(ToString(property.Type)) == "Invalid" ||
                std::string_view(ToString(property.TextureSemantic)) == "Invalid" ||
                std::string_view(ToString(property.ColorSpace)) == "Invalid")
            {
                add("Manifest.PropertyEnum", "Material shader property contains an invalid enum value.");
            }
            if (!IsIdentifier(property.Name) || !names.insert(property.Name).second)
                add("Manifest.PropertyName", "Property names must be non-empty and unique.");
            if (property.Source.empty() || property.Line == 0 || property.Column == 0)
                add("Manifest.PropertySource", "Property source path, line, and column are required.");
            if (property.StableId.empty() || !stableIds.insert(property.StableId).second)
                add("Manifest.PropertyStableId", "Property StableId values must be non-empty and unique.");
            if (property.ArrayCount != 1)
                add("Manifest.PropertyArray", "Material shader ABI v1 only supports ArrayCount=1.");
            const std::uint64_t expectedByteOffset =
                static_cast<std::uint64_t>(property.RowOffset) *
                MaterialShaderParameterAlignment;
            if (!rows.insert(property.RowOffset).second ||
                expectedByteOffset != property.ByteOffset)
            {
                add("Manifest.PropertyOffset", "Each property must occupy one unique 16-byte row.");
            }
            const std::uint64_t propertyEnd =
                static_cast<std::uint64_t>(property.ByteOffset) + property.ByteSize;
            if (property.ByteSize != MaterialShaderPropertyByteSize(property.Type) ||
                propertyEnd > manifest.ParameterBlock.ByteSize)
            {
                add("Manifest.PropertySize", "Property ByteSize or ByteOffset is inconsistent with its type.");
            }

            const std::size_t expectedNumericCount =
                property.Type == MaterialShaderPropertyType::Float ? 1u :
                property.Type == MaterialShaderPropertyType::Float2 ? 2u :
                property.Type == MaterialShaderPropertyType::Float3 ? 3u :
                property.Type == MaterialShaderPropertyType::Color ? 4u : 0u;
            if (expectedNumericCount != 0 && property.Default.Numeric.size() != expectedNumericCount)
                add("Manifest.PropertyDefault", "Numeric property default has the wrong component count.");
            if (std::any_of(property.Default.Numeric.begin(), property.Default.Numeric.end(), [](double value)
                {
                    return !IsFloatAbiRepresentable(value);
                }))
            {
                add("Manifest.PropertyDefault",
                    "Numeric property defaults must be finite and representable by the float material ABI.");
            }
            if (property.Type == MaterialShaderPropertyType::Int &&
                (property.Default.Integer < std::numeric_limits<std::int32_t>::min() ||
                 property.Default.Integer > std::numeric_limits<std::int32_t>::max()))
            {
                add("Manifest.PropertyDefault",
                    "Int property defaults must be representable by the signed 32-bit material ABI.");
            }

            if (property.Range.bEnabled)
            {
                if (property.Type != MaterialShaderPropertyType::Float &&
                    property.Type != MaterialShaderPropertyType::Int)
                {
                    add("Manifest.PropertyRange", "Only Float and Int properties may declare Range.");
                }
                if (!std::isfinite(property.Range.Minimum) || !std::isfinite(property.Range.Maximum) ||
                    property.Range.Minimum > property.Range.Maximum)
                {
                    add("Manifest.PropertyRange", "Property Range must be finite and Min <= Max.");
                }
                if (property.Type == MaterialShaderPropertyType::Int &&
                    (!IsInt32AbiRepresentable(property.Range.Minimum) ||
                     !IsInt32AbiRepresentable(property.Range.Maximum)))
                {
                    add("Manifest.PropertyRange",
                        "Int property Range values must be signed 32-bit integers.");
                }
                if (property.Type == MaterialShaderPropertyType::Float &&
                    (!IsFloatAbiRepresentable(property.Range.Minimum) ||
                     !IsFloatAbiRepresentable(property.Range.Maximum)))
                {
                    add("Manifest.PropertyRange",
                        "Float property Range values must be representable by the float material ABI.");
                }
                if (property.Type == MaterialShaderPropertyType::Float &&
                    property.Default.Numeric.size() == 1u &&
                    IsFloatAbiRepresentable(property.Default.Numeric.front()) &&
                    IsFloatAbiRepresentable(property.Range.Minimum) &&
                    IsFloatAbiRepresentable(property.Range.Maximum))
                {
                    const float defaultValue = static_cast<float>(property.Default.Numeric.front());
                    const float minimum = static_cast<float>(property.Range.Minimum);
                    const float maximum = static_cast<float>(property.Range.Maximum);
                    if (defaultValue < minimum || defaultValue > maximum)
                    {
                        add("Manifest.DefaultRange",
                            "Float property Default must be inside its declared Range.");
                    }
                }
                if (property.Type == MaterialShaderPropertyType::Int &&
                    (property.Default.Integer < property.Range.Minimum ||
                     property.Default.Integer > property.Range.Maximum))
                {
                    add("Manifest.DefaultRange",
                        "Int property Default must be inside its declared Range.");
                }
            }

            if (property.Type == MaterialShaderPropertyType::Texture2D)
            {
                ++textureCount;
                if (property.Default.Texture.empty() || property.Fallback.empty())
                    add("Manifest.TextureDefault", "Texture2D properties require non-empty Default and Fallback tokens.");
                else
                {
                    std::string defaultError;
                    if (!IsValidMaterialShaderTextureDefault(
                            property.Default.Texture, &defaultError))
                    {
                        add("Manifest.TextureDefault", std::move(defaultError));
                    }
                }
                if (!IsBuiltinTextureFallback(property.Fallback))
                {
                    add("Manifest.TextureFallback",
                        "Texture2D Fallback must be one of the built-in white, black, or normal tokens.");
                }
                if (property.Binding.RegisterType != "ParameterRow" ||
                    property.Binding.RegisterIndex != -1 ||
                    property.Binding.RegisterSpace != -1 ||
                    property.Binding.DescriptorIndex != static_cast<std::int32_t>(property.RowOffset))
                {
                    add("Manifest.TextureBinding", "Texture2D properties must resolve their descriptor from their ParameterRow.");
                }
            }
            else if (property.TextureSemantic != MaterialShaderTextureSemantic::None ||
                property.ColorSpace != MaterialShaderColorSpace::Linear ||
                !property.Fallback.empty())
            {
                add("Manifest.NumericMetadata", "Non-texture properties cannot declare texture metadata.");
            }
            else if (!property.Binding.RegisterType.empty() ||
                property.Binding.RegisterIndex != -1 ||
                property.Binding.RegisterSpace != -1 ||
                property.Binding.DescriptorIndex != -1)
            {
                add("Manifest.NumericBinding", "Non-texture properties cannot declare descriptor bindings.");
            }
        }
        if (textureCount > MaterialParameterMaximumTextureCount)
        {
            add("Manifest.TextureLimit", "Material shader ABI v1 supports at most 16 textures.");
        }
        if (manifest.ParameterBlock.ByteSize != manifest.Properties.size() * MaterialShaderParameterAlignment)
            add("Manifest.ParameterSize", "ParameterBlock ByteSize must equal property count times 16 bytes.");

        std::vector<MaterialShaderArtifactSet> artifactSets = manifest.ArtifactSets;
        if (artifactSets.empty() && !manifest.Artifacts.empty())
        {
            artifactSets.push_back({
                .Backend = RenderBackend::DX12,
                .BinaryFormat = ShaderBinaryFormat::Dxil,
                .Artifacts = manifest.Artifacts,
            });
        }
        else if (!manifest.Artifacts.empty())
        {
            const auto dxil = std::find_if(
                artifactSets.begin(), artifactSets.end(), [](const MaterialShaderArtifactSet& set)
                {
                    return set.Backend == RenderBackend::DX12 &&
                        set.BinaryFormat == ShaderBinaryFormat::Dxil;
                });
            if (dxil != artifactSets.end())
                dxil->Artifacts = manifest.Artifacts;
        }
        if (artifactSets.empty())
            add("Manifest.ArtifactSets", "A material shader manifest must contain at least one artifact set.");

        std::vector<ShaderArtifactTarget> targets;
        for (const MaterialShaderArtifactSet& set : artifactSets)
        {
            const ShaderArtifactTarget target{ set.Backend, set.BinaryFormat };
            if (!IsShaderArtifactTargetCompatible(set.Backend, set.BinaryFormat))
            {
                add("Manifest.ArtifactTarget",
                    "Material shader ArtifactSet backend and binary format are incompatible.");
            }
            if (std::find(targets.begin(), targets.end(), target) != targets.end())
                add("Manifest.ArtifactSetDuplicate", "Material shader ArtifactSet targets must be unique.");
            else
                targets.push_back(target);
            if (set.Artifacts.empty())
                add("Manifest.Artifacts", "Each material shader ArtifactSet must contain artifacts.");

            std::vector<MaterialShaderArtifactKey> artifactKeys;
            artifactKeys.reserve(set.Artifacts.size());
            for (const MaterialShaderArtifact& artifact : set.Artifacts)
            {
                if (std::string_view(ToString(artifact.Pass)) == "Invalid" ||
                    std::string_view(ToString(artifact.DescriptorMode)) == "Invalid" ||
                    std::string_view(ToString(artifact.VertexPath)) == "Invalid")
                {
                    add("Manifest.ArtifactEnum", "Material shader artifact contains an invalid enum value.");
                }
                if (set.Backend == RenderBackend::Vulkan &&
                    artifact.DescriptorMode == MaterialShaderDescriptorMode::HeapDirect)
                {
                    add("Manifest.VulkanHeapDirect",
                        "Vulkan material artifacts must use Baseline bounded descriptor tables.");
                }
                if (artifact.Stage != "vs" && artifact.Stage != "ps")
                    add("Manifest.ArtifactStage", "Material shader artifacts only support vs and ps.");
                if (artifact.EntryPoint.empty())
                    add("Manifest.ArtifactEntry", "Material shader artifacts require EntryPoint.");
                else if ((artifact.Stage == "vs" && artifact.EntryPoint != "HEMaterialVS") ||
                    (artifact.Stage == "ps" && artifact.EntryPoint != "HEMaterialPS"))
                {
                    add("Manifest.ArtifactEntry",
                        "Material shader artifact EntryPoint does not match its engine-owned stage entry point.");
                }
                const std::string_view expectedProfile = artifact.Stage == "vs"
                    ? std::string_view("vs_6_0")
                    : artifact.DescriptorMode == MaterialShaderDescriptorMode::HeapDirect
                        ? std::string_view("ps_6_6")
                        : std::string_view("ps_6_0");
                if (artifact.TargetProfile != expectedProfile)
                    add("Manifest.ArtifactProfile", "Artifact TargetProfile does not match its descriptor mode.");
                if (artifact.Permutation.empty())
                    add("Manifest.ArtifactPermutation", "Material shader artifact Permutation must not be empty.");
                if (!IsSafeRelativePath(artifact.RelativePath))
                    add("Manifest.ArtifactPath", "Artifact RelativePath must be normalized and relative.");
                else
                {
                    const std::string expectedDirectory(
                        GetShaderArtifactDirectoryName(set.BinaryFormat));
                    if (!artifact.RelativePath.starts_with(expectedDirectory + "/"))
                    {
                        add("Manifest.ArtifactPath",
                            "Artifact RelativePath must stay inside its ArtifactSet format directory.");
                    }
                }
                if (!IsHexHash(artifact.ContentHash))
                    add("Manifest.ArtifactHash", "Artifact ContentHash must be a SHA-256 hash.");
                MaterialShaderArtifactKey key = BuildMaterialShaderArtifactKey(artifact);
                if (std::find(artifactKeys.begin(), artifactKeys.end(), key) != artifactKeys.end())
                    add("Manifest.ArtifactDuplicate", "Artifact typed keys must be unique within a target.");
                else
                    artifactKeys.push_back(std::move(key));
            }

            const std::array descriptorModes = set.Backend == RenderBackend::Vulkan
                ? std::array{ MaterialShaderDescriptorMode::Baseline,
                    MaterialShaderDescriptorMode::Baseline }
                : std::array{ MaterialShaderDescriptorMode::Baseline,
                    MaterialShaderDescriptorMode::HeapDirect };
            const std::size_t descriptorModeCount =
                set.Backend == RenderBackend::Vulkan ? 1u : 2u;
            const auto requirePassMatrix = [&](MaterialShaderPass pass)
            {
                for (const MaterialShaderVertexPath vertexPath : {
                        MaterialShaderVertexPath::Static,
                        MaterialShaderVertexPath::Skinned })
                {
                    for (std::size_t modeIndex = 0; modeIndex < descriptorModeCount; ++modeIndex)
                    {
                        for (const MaterialShaderArtifactStage stage : {
                                MaterialShaderArtifactStage::Vertex,
                                MaterialShaderArtifactStage::Pixel })
                        {
                            const MaterialShaderArtifactKey required{
                                .Pass = pass,
                                .VertexPath = vertexPath,
                                .DescriptorMode = descriptorModes[modeIndex],
                                .Stage = stage,
                            };
                            if (std::find(artifactKeys.begin(), artifactKeys.end(), required) == artifactKeys.end())
                                add("Manifest.ArtifactMatrix", "Missing required material shader target artifact.");
                        }
                    }
                }
            };
            requirePassMatrix(MaterialShaderPass::Forward);
            if (manifest.BlendMode == MaterialShaderBlendMode::Mask)
            {
                requirePassMatrix(MaterialShaderPass::Depth);
                requirePassMatrix(MaterialShaderPass::Shadow);
                requirePassMatrix(MaterialShaderPass::Outline);
            }
            const std::size_t passCount = manifest.BlendMode == MaterialShaderBlendMode::Mask ? 4u : 1u;
            const std::size_t expectedArtifactCount = passCount * 2u * descriptorModeCount * 2u;
            if (set.Artifacts.size() != expectedArtifactCount)
                add("Manifest.ArtifactMatrix", "Artifact count does not match the target pass matrix.");
        }
        return diagnostics;
    }
}
