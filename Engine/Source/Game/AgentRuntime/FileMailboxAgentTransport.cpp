#include "Game/AgentRuntime/FileMailboxAgentTransport.h"
#include "Game/AgentRuntime/AgentCommandRegistry.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <system_error>
#include <thread>
#include <vector>

namespace HEngine
{
	namespace
	{
		constexpr std::string_view RequestSuffix = ".request.json";
		constexpr std::string_view RequestTemporaryMarker = ".request.json.tmp.";
		constexpr std::size_t MaxDirectoryEntriesScannedPerPoll = 64;
		constexpr std::chrono::seconds RequestTemporaryGracePeriod{ 30 };
		constexpr std::size_t RequestReadAttempts = 4;
		constexpr std::size_t MaxRequestReadPollAttempts = 4;

		void WaitForRequestReadRetry(std::size_t attempt)
		{
			if (attempt + 1 >= RequestReadAttempts)
				return;
			const auto retryDelay = std::chrono::milliseconds{
				1LL << static_cast<int>(attempt * 2)
			};
			std::this_thread::sleep_for(retryDelay);
		}

		bool GetRequestFileSizeWithRetry(
			const std::filesystem::path& path,
			std::uintmax_t& fileSize)
		{
			for (std::size_t attempt = 0; attempt < RequestReadAttempts; ++attempt)
			{
				std::error_code error;
				fileSize = std::filesystem::file_size(path, error);
				if (!error)
					return true;
				WaitForRequestReadRetry(attempt);
			}
			return false;
		}

		bool ReadRequestContentsWithRetry(
			const std::filesystem::path& path,
			std::uintmax_t fileSize,
			std::string& contents)
		{
			contents.assign(static_cast<std::size_t>(fileSize), '\0');
			for (std::size_t attempt = 0; attempt < RequestReadAttempts; ++attempt)
			{
				std::ifstream stream(path, std::ios::binary);
				if (stream.is_open())
				{
					if (!contents.empty())
						stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
					if (!stream.bad() &&
						static_cast<std::uintmax_t>(stream.gcount()) == fileSize)
						return true;
				}
				WaitForRequestReadRetry(attempt);
			}
			return false;
		}
	}

	// Coupled to vendored nlohmann 3.12.0 detail serializer; review on library upgrades.
	static_assert(NLOHMANN_JSON_VERSION_MAJOR == 3 && NLOHMANN_JSON_VERSION_MINOR == 12 &&
		NLOHMANN_JSON_VERSION_PATCH == 0, "Review bounded response serializer for JSON upgrade");

	struct FileMailboxAgentTransport::ResponseWriter
	{
		struct LimitExceeded {};
		struct Buffer final : nlohmann::detail::output_adapter_protocol<char>
		{
			explicit Buffer(std::size_t capacity) : mBytes(capacity) {}
			void write_character(char value) override { write_characters(&value, 1); }
			void write_characters(const char* values, std::size_t count) override
			{
				if (count > mLimit - mSize)
					throw LimitExceeded{};
				if (count != 0)
					std::memcpy(mBytes.data() + mSize, values, count);
				mSize += count;
			}
			std::vector<char> mBytes;
			std::size_t mSize = 0;
			std::size_t mLimit = 0;
		};

		explicit ResponseWriter(std::size_t limit)
			: mError(AgentCommandRegistry::MakeError(nullptr, "response_too_large",
				"Response exceeds the session byte limit.").dump()),
			  mOutput(CreateRef<Buffer>(std::max(limit, mError.size()))),
			  mSerializer(mOutput, ' ', Json::error_handler_t::replace)
		{}

		std::string_view Encode(const Json& response, std::size_t limit)
		{
			mOutput->mSize = 0;
			mOutput->mLimit = limit;
			try
			{
				mSerializer.dump(response, false, false, 0);
			}
			catch (const LimitExceeded&)
			{
				// Preserve the existing tiny-budget exception: the fixed error may exceed limit.
				mOutput->mSize = 0;
				mOutput->mLimit = mOutput->mBytes.size();
				mOutput->write_characters(mError.data(), mError.size());
			}
			return {mOutput->mBytes.data(), mOutput->mSize};
		}

		std::string mError;
		Ref<Buffer> mOutput;
		nlohmann::detail::serializer<Json> mSerializer;
	};

	FileMailboxAgentTransport::FileMailboxAgentTransport() = default;
	FileMailboxAgentTransport::~FileMailboxAgentTransport() = default;

	bool FileMailboxAgentTransport::Start(const AgentTransportConfig& config, const Json& sessionDescriptor)
	{
		if (mStarted || config.SessionDirectory.empty() || !config.SessionDirectory.is_absolute())
			return false;
		if (config.MaxRequestBytes == 0 || config.MaxRequestBytes > AgentProtocol::MaxRequestBytes ||
			config.MaxResponseBytes == 0 || config.MaxResponseBytes > AgentProtocol::MaxResponseBytes)
		{
			return false;
		}

		mConfig = config;
		mConfig.SessionDirectory = config.SessionDirectory.lexically_normal();
		std::error_code error;
		if (std::filesystem::exists(mConfig.SessionDirectory, error))
		{
			if (error || !std::filesystem::is_directory(mConfig.SessionDirectory, error) ||
				error || !std::filesystem::is_empty(mConfig.SessionDirectory, error) || error)
			{
				return false;
			}
		}
		else
		{
			std::filesystem::create_directories(mConfig.SessionDirectory, error);
			if (error)
				return false;
		}

		const std::filesystem::file_status sessionStatus =
			std::filesystem::symlink_status(mConfig.SessionDirectory, error);
		if (error || std::filesystem::is_symlink(sessionStatus))
			return false;
		mConfig.SessionDirectory = std::filesystem::weakly_canonical(mConfig.SessionDirectory, error);
		if (error || !mConfig.SessionDirectory.is_absolute())
			return false;

		mRequestsDirectory = mConfig.SessionDirectory / "requests";
		mResponsesDirectory = mConfig.SessionDirectory / "responses";
		mProcessingDirectory = mConfig.SessionDirectory / "processing";
		mProcessedDirectory = mConfig.SessionDirectory / "processed";
		mRejectedDirectory = mConfig.SessionDirectory / "rejected";

		std::filesystem::create_directories(mRequestsDirectory, error);
		if (error)
			return false;
		std::filesystem::create_directories(mResponsesDirectory, error);
		if (error)
			return false;
		std::filesystem::create_directories(mProcessingDirectory, error);
		if (error)
			return false;
		std::filesystem::create_directories(mProcessedDirectory, error);
		if (error)
			return false;
		std::filesystem::create_directories(mRejectedDirectory, error);
		if (error)
			return false;

		auto responseWriter = CreateScope<ResponseWriter>(mConfig.MaxResponseBytes);
		const std::string descriptor = sessionDescriptor.dump(
			-1, ' ', false, Json::error_handler_t::replace);
		if (descriptor.size() > mConfig.MaxResponseBytes ||
			!WriteAtomically(mConfig.SessionDirectory / "session.json", descriptor, false))
		{
			return false;
		}

		mResponseWriter = std::move(responseWriter);
		mRequestReadFailures.clear();
		mStarted = true;
		return true;
	}

	void FileMailboxAgentTransport::Stop(const Json& finalStatus)
	{
		if (!mStarted)
			return;
		(void)PublishStatus(finalStatus);
		mStarted = false;
		mResponseWriter.reset();
	}

	std::vector<AgentTransportMessage> FileMailboxAgentTransport::Poll(std::size_t maxMessages)
	{
		std::vector<AgentTransportMessage> messages;
		if (!mStarted || maxMessages == 0)
			return messages;
		maxMessages = std::min(maxMessages, AgentProtocol::MaxRequestsPerTick);

		std::vector<std::filesystem::path> candidates;
		std::vector<std::filesystem::path> rejected;
		std::error_code error;
		std::size_t scanned = 0;
		for (std::filesystem::directory_iterator iterator(mRequestsDirectory, error), end;
			!error && iterator != end && scanned < MaxDirectoryEntriesScannedPerPoll;
			iterator.increment(error), ++scanned)
		{
			const std::string filename = iterator->path().filename().string();
			const std::filesystem::file_status entryStatus = iterator->symlink_status(error);
			if (!error && !std::filesystem::is_symlink(entryStatus) &&
				std::filesystem::is_regular_file(entryStatus) &&
				HasRequestSuffix(filename) && IsSafeHandle(GetHandle(filename)))
			{
				candidates.push_back(iterator->path());
				if (candidates.size() >= maxMessages)
					break;
				continue;
			}
			if (!error && !std::filesystem::is_symlink(entryStatus) &&
				std::filesystem::is_regular_file(entryStatus) &&
				filename.find(RequestTemporaryMarker) != std::string::npos)
			{
				const auto writeTime = std::filesystem::last_write_time(iterator->path(), error);
				const auto now = std::filesystem::file_time_type::clock::now();
				if (!error && (writeTime > now || now - writeTime <= RequestTemporaryGracePeriod))
					continue;
			}

			error.clear();
			rejected.push_back(iterator->path());
		}
		for (const std::filesystem::path& rejectedEntry : rejected)
		{
			const std::filesystem::path rejectedPath = mRejectedDirectory /
				(rejectedEntry.filename().string() + ".rejected." + std::to_string(++mTempCounter));
			std::filesystem::rename(rejectedEntry, rejectedPath, error);
			error.clear();
		}
		std::sort(candidates.begin(), candidates.end());

		for (const std::filesystem::path& requestPath : candidates)
		{
			if (messages.size() >= maxMessages)
				break;

			const std::string handle = GetHandle(requestPath.filename().string());
			const std::filesystem::path processingPath = mProcessingDirectory / (handle + ".processing.json");
			std::filesystem::rename(requestPath, processingPath, error);
			if (error)
			{
				error.clear();
				continue;
			}

			AgentTransportMessage message;
			message.Handle = handle;
			auto tryRequeueRequest = [&]()
			{
				std::size_t& readFailures = mRequestReadFailures[handle];
				++readFailures;
				if (readFailures >= MaxRequestReadPollAttempts)
				{
					mRequestReadFailures.erase(handle);
					return false;
				}

				error.clear();
				std::filesystem::rename(processingPath, requestPath, error);
				if (!error)
					return true;
				error.clear();
				mRequestReadFailures.erase(handle);
				return false;
			};

			std::uintmax_t fileSize = 0;
			if (!GetRequestFileSizeWithRetry(processingPath, fileSize))
			{
				if (tryRequeueRequest())
					continue;
				message.ErrorCode = "request_read_failed";
				message.ErrorMessage = "Request could not be read.";
				messages.push_back(std::move(message));
				continue;
			}
			if (fileSize > mConfig.MaxRequestBytes)
			{
				mRequestReadFailures.erase(handle);
				message.ErrorCode = "request_too_large";
				message.ErrorMessage = "Request exceeds the session byte limit.";
				messages.push_back(std::move(message));
				continue;
			}

			std::string contents;
			if (!ReadRequestContentsWithRetry(processingPath, fileSize, contents))
			{
				if (tryRequeueRequest())
					continue;
				message.ErrorCode = "request_read_failed";
				message.ErrorMessage = "Request could not be read.";
				messages.push_back(std::move(message));
				continue;
			}
			mRequestReadFailures.erase(handle);

			message.Payload = Json::parse(contents, nullptr, false);
			if (message.Payload.is_discarded())
			{
				message.Payload = Json();
				message.ErrorCode = "invalid_json";
				message.ErrorMessage = "Request is not valid JSON.";
			}
			messages.push_back(std::move(message));
		}
		return messages;
	}

	bool FileMailboxAgentTransport::Respond(const AgentTransportMessage& message, const Json& response)
	{
		if (!mStarted || !IsSafeHandle(message.Handle))
			return false;

		const std::string_view contents = mResponseWriter->Encode(response, mConfig.MaxResponseBytes);

		const std::filesystem::path responsePath = mResponsesDirectory / (message.Handle + ".response.json");
		if (!WriteAtomically(responsePath, contents, false))
			return false;
		ArchiveRequest(message.Handle);
		return true;
	}

	bool FileMailboxAgentTransport::PublishStatus(const Json& status)
	{
		if (!mStarted)
			return false;
		const std::string contents = status.dump(-1, ' ', false, Json::error_handler_t::replace);
		if (contents.size() > mConfig.MaxResponseBytes)
			return false;
		return WriteAtomically(mConfig.SessionDirectory / "status.json", contents, true);
	}

	bool FileMailboxAgentTransport::WriteAtomically(
		const std::filesystem::path& target,
		std::string_view contents,
		bool bReplaceExisting)
	{
		const std::filesystem::path temporary = target.parent_path() /
			(target.filename().string() + ".tmp." + std::to_string(++mTempCounter));
		std::error_code error;
		std::filesystem::remove(temporary, error);

		std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
		stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
		stream.flush();
		if (!stream.good())
		{
			stream.close();
			std::filesystem::remove(temporary, error);
			return false;
		}
		stream.close();

		if (bReplaceExisting)
			std::filesystem::remove(target, error);
		else if (std::filesystem::exists(target, error))
		{
			std::filesystem::remove(temporary, error);
			return false;
		}

		std::filesystem::rename(temporary, target, error);
		if (error)
		{
			std::filesystem::remove(temporary, error);
			return false;
		}
		return true;
	}

	bool FileMailboxAgentTransport::IsSafeHandle(std::string_view handle)
	{
		if (handle.empty() || handle.size() > AgentProtocol::MaxRequestIdLength)
			return false;
		return std::all_of(handle.begin(), handle.end(), [](unsigned char character)
		{
			return std::isalnum(character) != 0 || character == '_' || character == '-';
		});
	}

	bool FileMailboxAgentTransport::HasRequestSuffix(std::string_view filename)
	{
		return filename.size() > RequestSuffix.size() && filename.ends_with(RequestSuffix);
	}

	std::string FileMailboxAgentTransport::GetHandle(std::string_view filename)
	{
		if (!HasRequestSuffix(filename))
			return {};
		return std::string(filename.substr(0, filename.size() - RequestSuffix.size()));
	}

	void FileMailboxAgentTransport::ArchiveRequest(std::string_view handle)
	{
		std::error_code error;
		const std::filesystem::path processingPath = mProcessingDirectory /
			(std::string(handle) + ".processing.json");
		const std::filesystem::path processedPath = mProcessedDirectory /
			(std::string(handle) + ".processed.json");
		std::filesystem::rename(processingPath, processedPath, error);
	}
}
