#include "Hepch.h"
#include "JsonUtils.h"
#include "CookedJsonFormat.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace HEngine::Utils
{
	namespace
	{
		[[noreturn]] void ThrowJsonLoadCancelled(const std::filesystem::path& jsonFilePath)
		{
			throw std::runtime_error("JSON load was cancelled: " + PathToUtf8(jsonFilePath));
		}

		void CheckJsonLoadCancelled(
			const std::filesystem::path& jsonFilePath,
			std::stop_token stopToken)
		{
			if (stopToken.stop_requested())
				ThrowJsonLoadCancelled(jsonFilePath);
		}

		[[nodiscard]] std::vector<std::uint8_t> ReadJsonFileBounded(
			const std::filesystem::path& jsonFilePath,
			size_t maxFileBytes,
			std::stop_token stopToken)
		{
			CheckJsonLoadCancelled(jsonFilePath, stopToken);
			std::ifstream file(jsonFilePath, std::ios::binary);
			if (!file)
				throw std::runtime_error("Failed to open JSON file: " + PathToUtf8(jsonFilePath));

			constexpr size_t chunkBytes = 64u * 1024u;
			std::array<std::uint8_t, chunkBytes> chunk{};
			std::vector<std::uint8_t> bytes;
			bytes.reserve(std::min(maxFileBytes, chunkBytes));

			while (true)
			{
				CheckJsonLoadCancelled(jsonFilePath, stopToken);
				const size_t remaining = maxFileBytes - bytes.size();
				const size_t requestedBytes =
					remaining >= chunk.size() ? chunk.size() : remaining + 1;

				file.read(
					reinterpret_cast<char*>(chunk.data()),
					static_cast<std::streamsize>(requestedBytes));
				const std::streamsize readCount = file.gcount();
				if (readCount > 0)
				{
					bytes.insert(
						bytes.end(),
						chunk.begin(),
						chunk.begin() + readCount);
					if (bytes.size() > maxFileBytes)
					{
						throw std::runtime_error(
							"JSON file exceeds the configured byte limit: " +
							PathToUtf8(jsonFilePath));
					}
				}

				if (file.bad())
					throw std::runtime_error("Failed to read JSON file: " + PathToUtf8(jsonFilePath));
				if (readCount == 0 || file.eof())
					break;
				if (file.fail())
					throw std::runtime_error("Failed to read JSON file: " + PathToUtf8(jsonFilePath));
			}

			CheckJsonLoadCancelled(jsonFilePath, stopToken);
			return bytes;
		}

		void ValidateJsonTree(
			const Json& root,
			const JsonLoadLimits& limits,
			const std::filesystem::path& jsonFilePath,
			std::stop_token stopToken)
		{
			struct PendingValue
			{
				const Json* Value = nullptr;
				size_t Depth = 0;
			};

			std::vector<PendingValue> pending;
			if (limits.MaxNodes == 0)
			{
				throw std::runtime_error(
					"JSON document exceeds the configured node limit: " +
					PathToUtf8(jsonFilePath));
			}
			pending.push_back({ &root, 0 });
			size_t nodeCount = 1;

			while (!pending.empty())
			{
				CheckJsonLoadCancelled(jsonFilePath, stopToken);
				const PendingValue current = pending.back();
				pending.pop_back();

				if (!current.Value->is_array() && !current.Value->is_object())
					continue;
				if (current.Depth == std::numeric_limits<size_t>::max())
				{
					throw std::runtime_error(
						"JSON document depth cannot be represented: " + PathToUtf8(jsonFilePath));
				}

				const size_t childDepth = current.Depth + 1;
				for (auto iterator = current.Value->begin(); iterator != current.Value->end(); ++iterator)
				{
					CheckJsonLoadCancelled(jsonFilePath, stopToken);
					if (childDepth > limits.MaxDepth)
					{
						throw std::runtime_error(
							"JSON document exceeds the configured depth limit: " +
							PathToUtf8(jsonFilePath));
					}
					if (nodeCount == limits.MaxNodes)
					{
						throw std::runtime_error(
							"JSON document exceeds the configured node limit: " +
							PathToUtf8(jsonFilePath));
					}
					++nodeCount;
					pending.push_back({ &iterator.value(), childDepth });
				}
			}
		}

		[[nodiscard]] Json ParseTextJsonBounded(
			const std::vector<std::uint8_t>& bytes,
			const JsonLoadLimits& limits,
			const std::filesystem::path& jsonFilePath,
			std::stop_token stopToken)
		{
			size_t nodeCount = 0;
			const Json::parser_callback_t callback =
				[&](int depth, Json::parse_event_t event, Json&)
				{
					CheckJsonLoadCancelled(jsonFilePath, stopToken);
					if (event != Json::parse_event_t::object_start &&
						event != Json::parse_event_t::array_start &&
						event != Json::parse_event_t::value)
					{
						return true;
					}

					if (depth < 0 || static_cast<size_t>(depth) > limits.MaxDepth)
					{
						throw std::runtime_error(
							"JSON document exceeds the configured depth limit: " +
							PathToUtf8(jsonFilePath));
					}
					if (nodeCount == limits.MaxNodes)
					{
						throw std::runtime_error(
							"JSON document exceeds the configured node limit: " +
							PathToUtf8(jsonFilePath));
					}
					++nodeCount;
					return true;
				};

			return Json::parse(bytes.begin(), bytes.end(), callback);
		}
	}

	Json LoadJsonFile(const std::filesystem::path& jsonFilePath)
	{
		std::ifstream file(jsonFilePath, std::ios::binary);
		if (!file)
			throw std::runtime_error("Failed to open JSON file: " + PathToUtf8(jsonFilePath));

		std::array<std::uint8_t, CookedJsonMagic.size()> header{};
		file.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
		if (file.gcount() == static_cast<std::streamsize>(header.size()) &&
			std::equal(header.begin(), header.end(), CookedJsonMagic.begin()))
		{
			std::vector<std::uint8_t> payload(
				(std::istreambuf_iterator<char>(file)),
				std::istreambuf_iterator<char>());
			if (payload.empty())
				throw std::runtime_error("Cooked JSON payload is empty: " + PathToUtf8(jsonFilePath));
			return Json::from_msgpack(payload);
		}

		file.clear();
		file.seekg(0, std::ios::beg);
		return Json::parse(file);
	}

	JsonLoadResult LoadJsonFile(
		const std::filesystem::path& jsonFilePath,
		const JsonLoadLimits& limits,
		std::stop_token stopToken)
	{
		std::vector<std::uint8_t> bytes =
			ReadJsonFileBounded(jsonFilePath, limits.MaxFileBytes, stopToken);
		Json value;
		if (bytes.size() >= CookedJsonMagic.size() &&
			std::equal(CookedJsonMagic.begin(), CookedJsonMagic.end(), bytes.begin()))
		{
			if (bytes.size() == CookedJsonMagic.size())
				throw std::runtime_error("Cooked JSON payload is empty: " + PathToUtf8(jsonFilePath));

			CheckJsonLoadCancelled(jsonFilePath, stopToken);
			value = Json::from_msgpack(
				bytes.begin() + static_cast<std::ptrdiff_t>(CookedJsonMagic.size()),
				bytes.end());
			CheckJsonLoadCancelled(jsonFilePath, stopToken);
			ValidateJsonTree(value, limits, jsonFilePath, stopToken);
		}
		else
		{
			value = ParseTextJsonBounded(bytes, limits, jsonFilePath, stopToken);
		}

		CheckJsonLoadCancelled(jsonFilePath, stopToken);
		return { std::move(value), bytes.size() };
	}

}
