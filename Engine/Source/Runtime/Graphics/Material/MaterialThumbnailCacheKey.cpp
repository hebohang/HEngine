#include "Hepch.h"

#include "Runtime/Graphics/Material/MaterialThumbnailCacheKey.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace HEngine
{
	namespace
	{
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
					const std::size_t copied = std::min(bytes.size(), mBuffer.size() - mBufferedBytes);
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
				if (mTotalBytes > std::numeric_limits<std::uint64_t>::max() / 8)
					throw std::overflow_error("SHA-256 input is too large");
				const std::uint64_t bitLength = mTotalBytes * 8;

				mBuffer[mBufferedBytes++] = 0x80;
				if (mBufferedBytes > 56)
				{
					std::fill(mBuffer.begin() + mBufferedBytes, mBuffer.end(), 0);
					Transform(mBuffer.data());
					mBufferedBytes = 0;
				}
				std::fill(mBuffer.begin() + mBufferedBytes, mBuffer.begin() + 56, 0);
				for (std::size_t index = 0; index < 8; ++index)
				{
					mBuffer[56 + index] = static_cast<std::uint8_t>(
						(bitLength >> ((7 - index) * 8)) & 0xffu);
				}
				Transform(mBuffer.data());

				std::array<std::uint8_t, 32> digest{};
				for (std::size_t word = 0; word < mState.size(); ++word)
				{
					for (std::size_t byte = 0; byte < 4; ++byte)
					{
						digest[word * 4 + byte] = static_cast<std::uint8_t>(
							(mState[word] >> ((3 - byte) * 8)) & 0xffu);
					}
				}
				return digest;
			}

		private:
			static constexpr std::array<std::uint32_t, 64> RoundConstants = {
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
				0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
			};

			static std::uint32_t RotateRight(std::uint32_t value, std::uint32_t count) noexcept
			{
				return (value >> count) | (value << (32 - count));
			}

			void Transform(const std::uint8_t* block) noexcept
			{
				std::array<std::uint32_t, 64> words{};
				for (std::size_t index = 0; index < 16; ++index)
				{
					words[index] =
						(static_cast<std::uint32_t>(block[index * 4]) << 24) |
						(static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
						(static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
						static_cast<std::uint32_t>(block[index * 4 + 3]);
				}
				for (std::size_t index = 16; index < words.size(); ++index)
				{
					const std::uint32_t sigma0 = RotateRight(words[index - 15], 7) ^
						RotateRight(words[index - 15], 18) ^ (words[index - 15] >> 3);
					const std::uint32_t sigma1 = RotateRight(words[index - 2], 17) ^
						RotateRight(words[index - 2], 19) ^ (words[index - 2] >> 10);
					words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
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
					const std::uint32_t sum1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
					const std::uint32_t choose = (e & f) ^ (~e & g);
					const std::uint32_t temporary1 = h + sum1 + choose + RoundConstants[index] + words[index];
					const std::uint32_t sum0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
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
				0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
			};
			std::array<std::uint8_t, 64> mBuffer{};
			std::uint64_t mTotalBytes = 0;
			std::size_t mBufferedBytes = 0;
		};

		void HashUnsigned64(Sha256& hash, std::uint64_t value)
		{
			std::array<std::uint8_t, 8> bytes{};
			for (std::size_t index = 0; index < bytes.size(); ++index)
				bytes[index] = static_cast<std::uint8_t>((value >> ((7 - index) * 8)) & 0xffu);
			hash.Update(bytes);
		}

		void HashString(Sha256& hash, std::string_view value)
		{
			HashUnsigned64(hash, static_cast<std::uint64_t>(value.size()));
			hash.Update(std::span<const std::uint8_t>(
				reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
		}

		void HashField(Sha256& hash, std::string_view name, std::string_view value)
		{
			HashString(hash, name);
			HashString(hash, value);
		}

		[[nodiscard]] std::string ToHex(std::span<const std::uint8_t> bytes)
		{
			std::ostringstream stream;
			stream << std::hex << std::setfill('0');
			for (const std::uint8_t byte : bytes)
				stream << std::setw(2) << static_cast<unsigned int>(byte);
			return stream.str();
		}
	}

	std::string BuildMaterialThumbnailCacheKey(const MaterialThumbnailCacheKeyDesc& desc)
	{
		Sha256 hash;
		HashField(hash, "format", "HEngineMaterialThumbnailCacheKey-v2");
		HashField(hash, "material", desc.MaterialDocumentContent);
		HashField(hash, "shader", desc.ShaderIdentity);
		HashField(hash, "shader.artifacts", desc.ShaderArtifactIdentity);

		std::vector<MaterialThumbnailFileIdentity> textures = desc.TextureDependencies;
		std::sort(textures.begin(), textures.end(), [](const auto& lhs, const auto& rhs)
		{
			if (lhs.AssetPath != rhs.AssetPath)
				return lhs.AssetPath < rhs.AssetPath;
			return lhs.ContentHash < rhs.ContentHash;
		});
		HashUnsigned64(hash, static_cast<std::uint64_t>(textures.size()));
		for (const MaterialThumbnailFileIdentity& texture : textures)
		{
			HashField(hash, "texture.path", texture.AssetPath);
			HashField(hash, "texture.content", texture.ContentHash);
		}

		HashField(hash, "preview.model", desc.PreviewModelIdentity);
		HashField(hash, "preview.environment", desc.EnvironmentIdentity);
		HashField(hash, "preview.rig", desc.RigVersion);
		HashUnsigned64(hash, desc.MaterialAbiVersion);
		HashUnsigned64(hash, desc.RendererVersion);
		const std::array<std::uint8_t, 32> digest = hash.Finalize();
		return ToHex(digest);
	}

	std::vector<std::string> SelectMaterialThumbnailCacheEvictions(
		std::vector<MaterialThumbnailCacheEntry> entries,
		std::size_t maximumEntryCount,
		std::uint64_t maximumByteSize)
	{
		std::uint64_t totalBytes = 0;
		for (const MaterialThumbnailCacheEntry& entry : entries)
		{
			if (entry.ByteSize > std::numeric_limits<std::uint64_t>::max() - totalBytes)
				totalBytes = std::numeric_limits<std::uint64_t>::max();
			else
				totalBytes += entry.ByteSize;
		}

		std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs)
		{
			if (lhs.bProtected != rhs.bProtected)
				return !lhs.bProtected;
			if (lhs.LastAccessOrder != rhs.LastAccessOrder)
				return lhs.LastAccessOrder < rhs.LastAccessOrder;
			return lhs.Key < rhs.Key;
		});

		std::vector<std::string> evictions;
		std::size_t remainingCount = entries.size();
		for (const MaterialThumbnailCacheEntry& entry : entries)
		{
			if (remainingCount <= maximumEntryCount && totalBytes <= maximumByteSize)
				break;
			if (entry.bProtected)
				continue;
			evictions.push_back(entry.Key);
			--remainingCount;
			totalBytes -= std::min(totalBytes, entry.ByteSize);
		}
		return evictions;
	}
}
