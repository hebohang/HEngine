#include "CookCache.h"

#include "Runtime/Platform/PlatformFileSystem.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

namespace HEngine::Cooker
{
	namespace
	{
		using Json = nlohmann::json;

		constexpr std::string_view CookCacheKeyDomain = "HEngine.HCooker.Payload";
		constexpr std::uint64_t MaxMetadataBytes = 64ull * 1024ull;

		class CorruptCacheEntry final : public std::runtime_error
		{
		public:
			using std::runtime_error::runtime_error;
		};

		class CacheIoError final : public std::runtime_error
		{
		public:
			using std::runtime_error::runtime_error;
		};

		class Sha256
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

		void UpdateUnsigned64(Sha256& hash, std::uint64_t value)
		{
			std::array<std::uint8_t, 8> bytes{};
			for (std::size_t index = 0; index < bytes.size(); ++index)
				bytes[index] = static_cast<std::uint8_t>((value >> ((7 - index) * 8)) & 0xffu);
			hash.Update(bytes);
		}

		void UpdateLengthPrefixed(Sha256& hash, std::span<const std::uint8_t> bytes)
		{
			UpdateUnsigned64(hash, static_cast<std::uint64_t>(bytes.size()));
			hash.Update(bytes);
		}

		void UpdateLengthPrefixed(Sha256& hash, std::string_view value)
		{
			UpdateLengthPrefixed(
				hash,
				std::span<const std::uint8_t>(
					reinterpret_cast<const std::uint8_t*>(value.data()),
					value.size()));
		}

		std::string ToHex(std::span<const std::uint8_t> bytes)
		{
			std::ostringstream stream;
			stream << std::hex << std::setfill('0');
			for (const std::uint8_t byte : bytes)
				stream << std::setw(2) << static_cast<unsigned int>(byte);
			return stream.str();
		}

		std::string HashPayload(std::span<const std::uint8_t> payload)
		{
			Sha256 hash;
			hash.Update(payload);
			const std::array<std::uint8_t, 32> digest = hash.Finalize();
			return ToHex(digest);
		}

		bool IsCacheKey(std::string_view key) noexcept
		{
			return key.size() == 64 && std::all_of(key.begin(), key.end(), [](char character)
			{
				return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
			});
		}

		std::filesystem::path EntryPath(const std::filesystem::path& root, std::string_view key)
		{
			return root / "v1" / "sha256" / std::string(key.substr(0, 2)) / std::string(key);
		}

		std::uint64_t UniqueNonce()
		{
			static std::atomic<std::uint64_t> counter = 0;
			static std::random_device entropy;
			const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
			return static_cast<std::uint64_t>(now) ^
				(static_cast<std::uint64_t>(entropy()) << 32) ^
				static_cast<std::uint64_t>(entropy()) ^
				counter.fetch_add(1, std::memory_order_relaxed);
		}

		std::filesystem::path UniqueSiblingPath(
			const std::filesystem::path& entryPath,
			std::string_view suffix)
		{
			for (std::size_t attempt = 0; attempt < 64; ++attempt)
			{
				std::ostringstream name;
				name << '.' << entryPath.filename().string() << suffix << '-'
					<< std::hex << UniqueNonce();
				const std::filesystem::path candidate = entryPath.parent_path() / name.str();
				std::error_code error;
				if (!std::filesystem::exists(candidate, error) && !error)
					return candidate;
			}
			throw std::runtime_error("Cannot reserve a unique cook-cache path");
		}

		std::vector<std::uint8_t> ReadBinaryFile(
			const std::filesystem::path& path,
			std::uint64_t expectedBytes,
			std::uint64_t maximumBytes)
		{
			if (expectedBytes > maximumBytes ||
				expectedBytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
				expectedBytes > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
			{
				throw CorruptCacheEntry("Cook-cache payload exceeds the configured size limit");
			}

			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				throw CacheIoError("Cannot open cook-cache file: " + path.generic_string());
			std::vector<std::uint8_t> bytes(static_cast<std::size_t>(expectedBytes));
			if (!bytes.empty())
				stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			if (!stream || stream.peek() != std::ifstream::traits_type::eof())
				throw CorruptCacheEntry("Cook-cache file size changed while reading: " + path.generic_string());
			return bytes;
		}

		void ValidateMetadataNesting(std::string_view text)
		{
			std::size_t depth = 0;
			bool bInString = false;
			bool bEscaped = false;
			for (const char character : text)
			{
				if (bInString)
				{
					if (bEscaped)
						bEscaped = false;
					else if (character == '\\')
						bEscaped = true;
					else if (character == '"')
						bInString = false;
					continue;
				}
				if (character == '"')
					bInString = true;
				else if (character == '{' || character == '[')
				{
					if (++depth > 8)
						throw CorruptCacheEntry("Cook-cache metadata nesting is invalid");
				}
				else if (character == '}' || character == ']')
				{
					if (depth == 0)
						throw CorruptCacheEntry("Cook-cache metadata nesting is invalid");
					--depth;
				}
			}
			if (bInString || depth != 0)
				throw CorruptCacheEntry("Cook-cache metadata nesting is invalid");
		}

		std::string ReadTextFile(const std::filesystem::path& path)
		{
			std::error_code error;
			const std::uint64_t size = std::filesystem::file_size(path, error);
			if (error)
			{
				if (error == std::errc::no_such_file_or_directory)
					throw CorruptCacheEntry("Cook-cache metadata file is missing");
				throw CacheIoError("Cannot inspect cook-cache metadata: " + error.message());
			}
			if (size > MaxMetadataBytes)
				throw CorruptCacheEntry("Cook-cache metadata has an invalid size");
			const std::vector<std::uint8_t> bytes = ReadBinaryFile(path, size, MaxMetadataBytes);
			return std::string(bytes.begin(), bytes.end());
		}

		CookCacheArtifact LoadEntry(
			const std::filesystem::path& entryPath,
			std::string_view key,
			std::uint64_t maximumPayloadBytes)
		{
			std::error_code typeError;
			const bool bDirectory = std::filesystem::is_directory(entryPath, typeError);
			if (typeError)
				throw CacheIoError("Cannot inspect cook-cache entry: " + typeError.message());
			if (!bDirectory)
				throw CorruptCacheEntry("Cook-cache entry is not a directory");

			const std::string metadataText = ReadTextFile(entryPath / "meta.json");
			ValidateMetadataNesting(metadataText);
			Json metadata;
			try
			{
				metadata = Json::parse(metadataText);
			}
			catch (const std::exception& exception)
			{
				throw CorruptCacheEntry(std::string("Cook-cache metadata JSON is invalid: ") + exception.what());
			}
			if (!metadata.is_object() || metadata.size() != 5 ||
				!metadata.contains("SchemaVersion") || !metadata.contains("Algorithm") ||
				!metadata.contains("Key") || !metadata.contains("PayloadSize") ||
				!metadata.contains("PayloadHash"))
			{
				throw CorruptCacheEntry("Cook-cache metadata schema is invalid");
			}
			if (!metadata["SchemaVersion"].is_number_unsigned() ||
				metadata["SchemaVersion"].get<std::uint64_t>() != CookCacheSchemaVersion ||
				!metadata["Algorithm"].is_string() ||
				metadata["Algorithm"].get<std::string>() != std::string(CookCacheHashAlgorithm) ||
				!metadata["Key"].is_string() || metadata["Key"].get<std::string>() != key ||
				!metadata["PayloadSize"].is_number_unsigned() ||
				!metadata["PayloadHash"].is_string() ||
				!IsCacheKey(metadata["PayloadHash"].get<std::string>()))
			{
				throw CorruptCacheEntry("Cook-cache metadata values are invalid");
			}
			if (metadataText != metadata.dump(2) + '\n')
				throw CorruptCacheEntry("Cook-cache metadata is not in canonical form");

			const std::uint64_t payloadSize = metadata["PayloadSize"].get<std::uint64_t>();
			std::error_code error;
			const std::uint64_t actualSize = std::filesystem::file_size(entryPath / "payload.bin", error);
			if (error)
			{
				if (error == std::errc::no_such_file_or_directory)
					throw CorruptCacheEntry("Cook-cache payload file is missing");
				throw CacheIoError("Cannot inspect cook-cache payload: " + error.message());
			}
			if (actualSize != payloadSize)
				throw CorruptCacheEntry("Cook-cache payload size is invalid");
			CookCacheArtifact artifact;
			artifact.Payload = ReadBinaryFile(entryPath / "payload.bin", payloadSize, maximumPayloadBytes);
			if (HashPayload(artifact.Payload) != metadata["PayloadHash"].get<std::string>())
				throw CorruptCacheEntry("Cook-cache payload hash is invalid");
			return artifact;
		}

		void RemoveBestEffort(const std::filesystem::path& path) noexcept
		{
			try
			{
				std::error_code error;
				std::filesystem::remove_all(path, error);
			}
			catch (...)
			{
			}
		}

		bool QuarantineEntry(const std::filesystem::path& entryPath) noexcept
		{
			try
			{
				const std::filesystem::path quarantinePath = UniqueSiblingPath(entryPath, ".corrupt");
				std::error_code error;
				std::filesystem::rename(entryPath, quarantinePath, error);
				if (error)
					return false;
				RemoveBestEffort(quarantinePath);
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		bool PublishDirectoryWithRetry(
			const std::filesystem::path& temporaryPath,
			const std::filesystem::path& entryPath,
			std::error_code& outError) noexcept
		{
			constexpr std::size_t AttemptCount = 7;
			for (std::size_t attempt = 0; attempt < AttemptCount; ++attempt)
			{
				outError.clear();
				std::filesystem::rename(temporaryPath, entryPath, outError);
				if (!outError)
					return true;
				if (outError != std::errc::permission_denied &&
					outError != std::errc::device_or_resource_busy &&
					outError != std::errc::resource_unavailable_try_again)
				{
					return false;
				}
				if (attempt + 1 < AttemptCount)
				{
					std::this_thread::sleep_for(
						std::chrono::milliseconds(10u << attempt));
				}
			}
			return false;
		}

		void Warn(std::string_view message) noexcept
		{
			std::fprintf(stderr, "HCooker warning: %.*s\n", static_cast<int>(message.size()), message.data());
		}
	}

	CookCache::CookCache(std::filesystem::path root, std::uint64_t maxPayloadBytes)
		: mRoot(std::move(root)), mMaxPayloadBytes(maxPayloadBytes)
	{
		if (mRoot.empty())
			throw std::invalid_argument("Cook-cache root cannot be empty");
		if (mMaxPayloadBytes == 0)
			throw std::invalid_argument("Cook-cache payload limit cannot be zero");
		if (mMaxPayloadBytes > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
			throw std::invalid_argument("Cook-cache payload limit exceeds the stream size limit");
	}

	std::string CookCache::BuildKey(const CookCacheKeyInput& input) const
	{
		if (input.LogicalPath.empty())
			throw std::invalid_argument("Cook-cache logical path cannot be empty");

		Sha256 hash;
		UpdateLengthPrefixed(hash, CookCacheKeyDomain);
		UpdateUnsigned64(hash, CookCacheSchemaVersion);
		UpdateUnsigned64(hash, CookCacheImplementationVersion);
		UpdateLengthPrefixed(hash, input.LogicalPath);
		UpdateLengthPrefixed(hash, input.SourceBytes);
		UpdateUnsigned64(hash, static_cast<std::uint64_t>(input.PayloadOptions.size()));
		for (const CookCacheOption& option : input.PayloadOptions)
		{
			if (option.Name.empty())
				throw std::invalid_argument("Cook-cache option names cannot be empty");
			UpdateLengthPrefixed(hash, option.Name);
			UpdateLengthPrefixed(hash, option.Value);
		}
		const std::array<std::uint8_t, 32> digest = hash.Finalize();
		return ToHex(digest);
	}

	std::optional<CookCacheArtifact> CookCache::TryLoad(
		std::string_view key,
		std::string_view logicalPath)
	{
		if (!IsCacheKey(key))
			throw std::invalid_argument("Cook-cache key is invalid");
		++mStats.Cacheable;
		const std::filesystem::path entryPath = EntryPath(mRoot, key);
		std::error_code error;
		const bool bExists = std::filesystem::exists(entryPath, error);
		if (!bExists && !error)
		{
			++mStats.Misses;
			mEvents.push_back({ std::string(logicalPath), std::string(key), "Miss" });
			return std::nullopt;
		}

		try
		{
			if (error)
				throw CacheIoError("Cannot inspect cook-cache entry: " + error.message());
			const bool bDirectory = std::filesystem::is_directory(entryPath, error);
			if (error)
				throw CacheIoError("Cannot inspect cook-cache entry: " + error.message());
			if (!bDirectory)
				throw CorruptCacheEntry("Cook-cache entry is not a directory");
			CookCacheArtifact artifact = LoadEntry(entryPath, key, mMaxPayloadBytes);
			++mStats.Hits;
			mEvents.push_back({ std::string(logicalPath), std::string(key), "Hit" });
			return artifact;
		}
		catch (const CorruptCacheEntry& exception)
		{
			++mStats.Misses;
			++mStats.Corruptions;
			mEvents.push_back({ std::string(logicalPath), std::string(key), "Corrupt" });
			Warn("ignoring corrupt cache entry '" + entryPath.generic_string() + "': " + exception.what());
			return std::nullopt;
		}
		catch (const std::exception& exception)
		{
			++mStats.Misses;
			mEvents.push_back({ std::string(logicalPath), std::string(key), "Unavailable" });
			Warn("ignoring unavailable cache entry '" + entryPath.generic_string() + "': " + exception.what());
			return std::nullopt;
		}
	}

	void CookCache::Store(std::string_view key, std::span<const std::uint8_t> payload) noexcept
	{
		std::filesystem::path temporaryPath;
		try
		{
			if (!IsCacheKey(key) || payload.size() > mMaxPayloadBytes ||
				payload.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
				throw std::invalid_argument("Cook-cache store input is invalid");

			const std::filesystem::path entryPath = EntryPath(mRoot, key);
			std::error_code error;
			std::filesystem::create_directories(entryPath.parent_path(), error);
			if (error)
				throw std::runtime_error("Cannot create cook-cache shard: " + error.message());

			temporaryPath = UniqueSiblingPath(entryPath, ".tmp");
			if (!std::filesystem::create_directory(temporaryPath, error) || error)
				throw std::runtime_error("Cannot create cook-cache temporary directory: " + error.message());

			const std::string payloadHash = HashPayload(payload);
			{
				std::ofstream stream(temporaryPath / "payload.bin", std::ios::binary | std::ios::trunc);
				if (!stream)
					throw std::runtime_error("Cannot write cook-cache payload");
				if (!payload.empty())
					stream.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
				stream.flush();
				if (!stream)
					throw std::runtime_error("Cannot finish cook-cache payload");
				stream.close();
				if (stream.fail())
					throw std::runtime_error("Cannot close cook-cache payload");
			}

			const Json metadata = {
				{ "SchemaVersion", CookCacheSchemaVersion },
				{ "Algorithm", std::string(CookCacheHashAlgorithm) },
				{ "Key", std::string(key) },
				{ "PayloadSize", payload.size() },
				{ "PayloadHash", payloadHash }
			};
			{
				std::ofstream stream(temporaryPath / "meta.json", std::ios::binary | std::ios::trunc);
				if (!stream)
					throw std::runtime_error("Cannot write cook-cache metadata");
				stream << metadata.dump(2) << '\n';
				stream.flush();
				if (!stream)
					throw std::runtime_error("Cannot finish cook-cache metadata");
				stream.close();
				if (stream.fail())
					throw std::runtime_error("Cannot close cook-cache metadata");
			}

			PlatformFileLock entryLock(
				entryPath.parent_path() / ("." + entryPath.filename().string() + ".lock"));
			if (!entryLock.IsOwned())
			{
				try
				{
					(void)LoadEntry(entryPath, key, mMaxPayloadBytes);
					RemoveBestEffort(temporaryPath);
					return;
				}
				catch (...)
				{
					throw CacheIoError("Cook-cache entry is being published by another process");
				}
			}

			error.clear();
			const bool bEntryExists = std::filesystem::exists(entryPath, error);
			if (error)
				throw CacheIoError("Cannot inspect existing cook-cache entry: " + error.message());
			if (bEntryExists)
			{
				try
				{
					(void)LoadEntry(entryPath, key, mMaxPayloadBytes);
					RemoveBestEffort(temporaryPath);
					return;
				}
				catch (const CorruptCacheEntry&)
				{
					const auto event = std::find_if(mEvents.rbegin(), mEvents.rend(), [key](const CookCacheEvent& value)
					{
						return value.Key == key;
					});
					if (event == mEvents.rend() || event->Outcome != "Corrupt")
						++mStats.Corruptions;
					if (event != mEvents.rend())
						event->Outcome = "Corrupt";
					if (!QuarantineEntry(entryPath))
						throw CacheIoError("Cannot quarantine corrupt cook-cache entry");
				}
			}

			if (!PublishDirectoryWithRetry(temporaryPath, entryPath, error))
				throw CacheIoError("Cannot publish cook-cache entry: " + error.message());
			++mStats.Stores;
		}
		catch (const std::exception& exception)
		{
			if (!temporaryPath.empty())
				RemoveBestEffort(temporaryPath);
			std::fprintf(stderr, "HCooker warning: cannot store cook-cache entry: %s\n", exception.what());
		}
		catch (...)
		{
			if (!temporaryPath.empty())
				RemoveBestEffort(temporaryPath);
			Warn("cannot store cook-cache entry: unknown error");
		}
	}
}
