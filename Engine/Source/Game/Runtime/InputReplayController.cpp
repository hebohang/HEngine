#include "Game/Runtime/InputReplayController.h"

#if defined(HE_DEV_AGENT_RUNTIME)

#include "Runtime/Core/Log/Log.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>

namespace HEngine
{
    void InputReplayController::Start(const std::filesystem::path& path)
    {
        if (mActive)
            throw std::logic_error("Input replay is already active.");
        if (!std::filesystem::is_regular_file(path) ||
            std::filesystem::file_size(path) > 1024u * 1024u)
        {
            throw std::invalid_argument("Input replay must be an existing JSON file up to 1 MiB.");
        }
        std::ifstream file(path);
        if (!file)
            throw std::runtime_error("Could not read input replay file.");
        const Json document = Json::parse(file);
        if (!document.is_object() || document.size() != 2 ||
            !document.contains("version") || !document["version"].is_number_integer() ||
            document["version"] != 1 || !document.contains("segments") ||
            !document["segments"].is_array() || document["segments"].empty() ||
            document["segments"].size() > 1024)
        {
            throw std::invalid_argument("Input replay requires version 1 and 1..1024 segments.");
        }

        const Json allowed = mInput.ListKeys().at("keys");
        std::vector<Segment> segments;
        segments.reserve(document["segments"].size());
        std::uint64_t totalFrames = 0;
        for (const Json& item : document["segments"])
        {
            if (!item.is_object() || item.size() != 2 ||
                !item.contains("frames") || !item["frames"].is_number_integer() ||
                !item.contains("held") || !item["held"].is_array() ||
                item["held"].size() > 32)
            {
                throw std::invalid_argument("Input replay segment requires frames and held keys.");
            }
            const std::int64_t frames = item["frames"].get<std::int64_t>();
            if (frames <= 0 || frames > 100'000 || totalFrames + frames > 100'000)
                throw std::invalid_argument("Input replay exceeds 100000 frames.");
            Json unique = Json::array();
            for (const Json& key : item["held"])
            {
                if (!key.is_string() ||
                    std::find(allowed.begin(), allowed.end(), key) == allowed.end() ||
                    std::find(unique.begin(), unique.end(), key) != unique.end())
                {
                    throw std::invalid_argument("Input replay contains an unknown or duplicate key.");
                }
                unique.push_back(key);
            }
            segments.push_back({ static_cast<std::uint64_t>(frames), std::move(unique) });
            totalFrames += static_cast<std::uint64_t>(frames);
        }

        mSegments = std::move(segments);
        mSegmentIndex = 0;
        mSegmentFrame = 0;
        mPlayedFrames = 0;
        mCompleted = false;
        mInput.Start();
        mActive = true;
        HE_CORE_INFO("[InputReplay] Started file={0} segments={1} frames={2}",
            path.string(), mSegments.size(), totalFrames);
    }

    void InputReplayController::Advance()
    {
        if (!mActive || mCompleted)
            return;
        if (mSegmentIndex == mSegments.size())
        {
            (void)mInput.Clear(Json::object());
            mCompleted = true;
            HE_CORE_INFO("[InputReplay] Completed frames={0}", mPlayedFrames);
            return;
        }
        const Segment& segment = mSegments[mSegmentIndex];
        if (mSegmentFrame == 0)
        {
            (void)mInput.SetHeldKeys(Json{ {"held", segment.Held} });
            HE_CORE_INFO("[InputReplay] Segment {0} held={1} frames={2}",
                mSegmentIndex, segment.Held.dump(), segment.Frames);
        }
        ++mSegmentFrame;
        ++mPlayedFrames;
        if (mSegmentFrame == segment.Frames)
        {
            ++mSegmentIndex;
            mSegmentFrame = 0;
        }
    }

    void InputReplayController::Stop()
    {
        if (mActive)
            mInput.Stop();
        mSegments.clear();
        mSegmentIndex = 0;
        mSegmentFrame = 0;
        mPlayedFrames = 0;
        mActive = false;
        mCompleted = false;
    }
}

#endif
