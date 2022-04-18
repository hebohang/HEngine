#pragma once

#include <string>
#include <chrono>
#include <algorithm>
#include <fstream>

#include <thread>

namespace HEngine 
{
	struct ProfileResult
	{
		std::string Name;
		long long Start, End;
		uint32_t ThreadID;
	};

	struct InstrumentationSession
	{
		std::string Name;
	};

	class Instrumentor
	{
	private:
		InstrumentationSession* mCurrentSession;
		std::ofstream mOutputStream;
		int mProfileCount;
	public:
		Instrumentor()
			: mCurrentSession(nullptr), mProfileCount(0)
		{
		}

		void BeginSession(const std::string& name, const std::string& filepath = "results.json")
		{
			mOutputStream.open(filepath);
			WriteHeader();
			mCurrentSession = new InstrumentationSession{ name };
		}

		void EndSession()
		{
			WriteFooter();
			mOutputStream.close();
			delete mCurrentSession;
			mCurrentSession = nullptr;
			mProfileCount = 0;
		}

		void WriteProfile(const ProfileResult& result)
		{
			if (mProfileCount++ > 0)
				mOutputStream << ",";

			std::string name = result.Name;
			std::replace(name.begin(), name.end(), '"', '\'');

			mOutputStream << "{";
			mOutputStream << "\"cat\":\"function\",";
			mOutputStream << "\"dur\":" << (result.End - result.Start) << ',';
			mOutputStream << "\"name\":\"" << name << "\",";
			mOutputStream << "\"ph\":\"X\",";
			mOutputStream << "\"pid\":0,";
			mOutputStream << "\"tid\":" << result.ThreadID << ",";
			mOutputStream << "\"ts\":" << result.Start;
			mOutputStream << "}";

			mOutputStream.flush();
		}

		void WriteHeader()
		{
			mOutputStream << "{\"otherData\": {},\"traceEvents\":[";
			mOutputStream.flush();
		}

		void WriteFooter()
		{
			mOutputStream << "]}";
			mOutputStream.flush();
		}

		[[nodiscard]] static Instrumentor& Get()
		{
			static Instrumentor instance;
			return instance;
		}
	};

	class InstrumentationTimer
	{
	public:
		InstrumentationTimer(const char* name)
			: mName(name), mStopped(false)
		{
			mStartTimepoint = std::chrono::high_resolution_clock::now();
		}

		~InstrumentationTimer()
		{
			if (!mStopped)
				Stop();
		}

		void Stop()
		{
			auto endTimepoint = std::chrono::high_resolution_clock::now();

			long long start = std::chrono::time_point_cast<std::chrono::microseconds>(mStartTimepoint).time_since_epoch().count();
			long long end = std::chrono::time_point_cast<std::chrono::microseconds>(endTimepoint).time_since_epoch().count();

			uint32_t threadID = std::hash<std::thread::id>{}(std::this_thread::get_id());
			Instrumentor::Get().WriteProfile({ mName, start, end, threadID });

			mStopped = true;
		}
	private:
		const char* mName;
		std::chrono::time_point<std::chrono::high_resolution_clock> mStartTimepoint;
		bool mStopped;
	};
}

#define HE_PROFILE 0
#if HE_PROFILE
	#define HE_PROFILE_BEGIN_SESSION(name, filepath) ::HEngine::Instrumentor::Get().BeginSession(name, filepath)
	#define HE_PROFILE_END_SESSION() ::HEngine::Instrumentor::Get().EndSession()
	#define HE_PROFILE_SCOPE(name) ::HEngine::InstrumentationTimer timer##__LINE__(name);
	#define HE_PROFILE_FUNCTION() HE_PROFILE_SCOPE(__FUNCSIG__)
#else
	#define HE_PROFILE_BEGIN_SESSION(name, filepath)
	#define HE_PROFILE_END_SESSION()
	#define HE_PROFILE_SCOPE(name)
	#define HE_PROFILE_FUNCTION()
#endif