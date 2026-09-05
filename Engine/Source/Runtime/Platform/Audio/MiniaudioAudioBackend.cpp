#include "Hepch.h"

#include "Runtime/Audio/AudioBackend.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <miniaudio.h>

#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace HEngine
{
    namespace
    {
        constexpr size_t kAudioBusCount = static_cast<size_t>(AudioBus::Count);
        constexpr uint32_t kListenerCount = 4;
        constexpr float kMinimumPitch = 0.01f;
        constexpr float kMinimumDistance = 0.001f;
        constexpr float kTwoPi = 6.28318530718f;

        [[nodiscard]] bool IsValidBus(AudioBus bus)
        {
            return static_cast<size_t>(bus) < kAudioBusCount;
        }

        [[nodiscard]] bool IsValidAttenuation(AudioAttenuationModel attenuation)
        {
            return static_cast<uint8_t>(attenuation) <
                static_cast<uint8_t>(AudioAttenuationModel::Count);
        }

        [[nodiscard]] float FiniteOr(float value, float fallback)
        {
            return std::isfinite(value) ? value : fallback;
        }

        [[nodiscard]] float NonNegative(float value, float fallback = 0.0f)
        {
            return std::max(0.0f, FiniteOr(value, fallback));
        }

        [[nodiscard]] glm::vec3 FiniteVector(
            const glm::vec3& value,
            const glm::vec3& fallback = glm::vec3(0.0f))
        {
            if (!std::isfinite(value.x) ||
                !std::isfinite(value.y) ||
                !std::isfinite(value.z))
            {
                return fallback;
            }
            return value;
        }

        [[nodiscard]] glm::vec3 UnitVector(
            const glm::vec3& value,
            const glm::vec3& fallback)
        {
            const glm::vec3 finiteValue = FiniteVector(value, fallback);
            const float lengthSquared = glm::dot(finiteValue, finiteValue);
            if (!std::isfinite(lengthSquared) || lengthSquared <= 0.000001f)
                return fallback;
            return finiteValue / std::sqrt(lengthSquared);
        }

        [[nodiscard]] ma_attenuation_model ToMiniaudioAttenuation(
            AudioAttenuationModel attenuation)
        {
            switch (attenuation)
            {
            case AudioAttenuationModel::None:
                return ma_attenuation_model_none;
            case AudioAttenuationModel::Linear:
                return ma_attenuation_model_linear;
            case AudioAttenuationModel::Exponential:
                return ma_attenuation_model_exponential;
            case AudioAttenuationModel::Inverse:
            default:
                return ma_attenuation_model_inverse;
            }
        }

        class MiniaudioAudioBackend final : public AudioBackend
        {
        public:
            ~MiniaudioAudioBackend() override
            {
                Shutdown();
            }

            bool Initialize() override
            {
                std::scoped_lock lock(mMutex);
                if (mInitialized)
                    return true;

                mLastError.clear();
                ma_engine_config engineConfig = ma_engine_config_init();
                engineConfig.listenerCount = kListenerCount;
                engineConfig.defaultVolumeSmoothTimeInPCMFrames = 256;

                ma_result result = ma_engine_init(&engineConfig, &mEngine);
                if (result != MA_SUCCESS)
                {
                    const std::string physicalDeviceError = ResultMessage(
                        "Failed to initialize the default audio device",
                        result);
                    HE_CORE_WARN("[Audio] {0}; trying miniaudio null backend", physicalDeviceError);

                    ma_backend nullBackend = ma_backend_null;
                    ma_context_config contextConfig = ma_context_config_init();
                    result = ma_context_init(
                        &nullBackend,
                        1,
                        &contextConfig,
                        &mNullContext);
                    if (result != MA_SUCCESS)
                    {
                        SetError(ResultMessage(
                            physicalDeviceError + "; failed to initialize null context",
                            result));
                        return false;
                    }
                    mNullContextInitialized = true;

                    engineConfig = ma_engine_config_init();
                    engineConfig.pContext = &mNullContext;
                    engineConfig.listenerCount = kListenerCount;
                    engineConfig.defaultVolumeSmoothTimeInPCMFrames = 256;
                    result = ma_engine_init(&engineConfig, &mEngine);
                    if (result != MA_SUCCESS)
                    {
                        SetError(ResultMessage(
                            physicalDeviceError + "; failed to initialize null audio engine",
                            result));
                        ma_context_uninit(&mNullContext);
                        mNullContextInitialized = false;
                        return false;
                    }
                    mEngineInitialized = true;
                    mLastError = physicalDeviceError + "; using miniaudio null backend";
                }

                mEngineInitialized = true;
                for (uint32_t listenerIndex = 0; listenerIndex < kListenerCount; ++listenerIndex)
                {
                    ma_engine_listener_set_enabled(
                        &mEngine,
                        listenerIndex,
                        listenerIndex == 0 ? MA_TRUE : MA_FALSE);
                }
                mDeviceAvailable = DetectPhysicalDevice();
                if (!mDeviceAvailable && mLastError.empty())
                    mLastError = "Audio output device unavailable; using miniaudio null backend";

                for (size_t index = 0; index < kAudioBusCount; ++index)
                {
                    result = ma_sound_group_init(
                        &mEngine,
                        0,
                        nullptr,
                        &mBusGroups[index]);
                    if (result != MA_SUCCESS)
                    {
                        SetError(ResultMessage("Failed to initialize audio bus", result));
                        ShutdownUnlocked();
                        return false;
                    }
                    mBusGroupInitialized[index] = true;
                    ma_sound_group_set_volume(&mBusGroups[index], mBusVolumes[index]);
                }

                ma_engine_set_volume(&mEngine, mMasterVolume);
                if (mGameplayPaused)
                    ma_sound_group_stop(&mBusGroups[BusIndex(AudioBus::Gameplay)]);

                mInitialized = true;
                return true;
            }

            void Shutdown() override
            {
                std::scoped_lock lock(mMutex);
                ShutdownUnlocked();
            }

            void Update() override
            {
                std::scoped_lock lock(mMutex);
                if (!mInitialized)
                    return;

                for (auto iterator = mVoices.begin(); iterator != mVoices.end();)
                {
                    Voice& voice = *iterator->second;
                    const bool bFinished =
                        voice.bStopRequested ||
                        (!voice.bLooping && !voice.bPaused && ma_sound_at_end(&voice.Sound));
                    if (bFinished)
                        iterator = mVoices.erase(iterator);
                    else
                        ++iterator;
                }
            }

            [[nodiscard]] bool IsInitialized() const override
            {
                std::scoped_lock lock(mMutex);
                return mInitialized;
            }

            [[nodiscard]] bool IsDeviceAvailable() const override
            {
                std::scoped_lock lock(mMutex);
                return mInitialized && mDeviceAvailable;
            }

            [[nodiscard]] std::string GetLastError() const override
            {
                std::scoped_lock lock(mMutex);
                return mLastError;
            }

            AudioHandle Play(
                const std::filesystem::path& path,
                const AudioPlayParams& params) override
            {
                std::scoped_lock lock(mMutex);
                if (!CanPlay(params) || path.empty())
                {
                    if (path.empty())
                        SetError("Cannot play an empty audio path");
                    return {};
                }

                auto voice = std::make_unique<Voice>();
                voice->Bus = params.Bus;
                voice->bLooping = params.bLooping;
                voice->DebugName = Utils::PathToUtf8(path);

                ma_uint32 flags = params.bStream
                    ? MA_SOUND_FLAG_STREAM
                    : MA_SOUND_FLAG_DECODE;
                if (!params.bSpatialized)
                    flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;

#if defined(HE_PLATFORM_WINDOWS) || defined(_WIN32)
                const ma_result result = ma_sound_init_from_file_w(
                    &mEngine,
                    path.c_str(),
                    flags,
                    BusGroup(params.Bus),
                    nullptr,
                    &voice->Sound);
#else
                const std::string pathText = Utils::PathToUtf8(path);
                const ma_result result = ma_sound_init_from_file(
                    &mEngine,
                    pathText.c_str(),
                    flags,
                    BusGroup(params.Bus),
                    nullptr,
                    &voice->Sound);
#endif
                if (result != MA_SUCCESS)
                {
                    SetError(ResultMessage("Failed to load audio file " + voice->DebugName, result));
                    return {};
                }
                voice->bSoundInitialized = true;
                return StartVoice(std::move(voice), params);
            }

            AudioHandle PlayMemory(
                std::shared_ptr<std::vector<uint8_t>> data,
                const std::string& debugName,
                const AudioPlayParams& params) override
            {
                std::scoped_lock lock(mMutex);
                if (!CanPlay(params) || !data || data->empty())
                {
                    if (!data || data->empty())
                        SetError("Cannot play an empty audio memory buffer: " + debugName);
                    return {};
                }

                auto voice = std::make_unique<Voice>();
                voice->Bus = params.Bus;
                voice->bLooping = params.bLooping;
                voice->Memory = std::move(data);
                voice->DebugName = debugName;

                ma_result result = ma_decoder_init_memory(
                    voice->Memory->data(),
                    voice->Memory->size(),
                    nullptr,
                    &voice->Decoder);
                if (result != MA_SUCCESS)
                {
                    SetError(ResultMessage("Failed to decode audio memory " + debugName, result));
                    return {};
                }
                voice->bDecoderInitialized = true;

                ma_uint32 flags = params.bSpatialized
                    ? 0
                    : MA_SOUND_FLAG_NO_SPATIALIZATION;
                result = ma_sound_init_from_data_source(
                    &mEngine,
                    reinterpret_cast<ma_data_source*>(&voice->Decoder),
                    flags,
                    BusGroup(params.Bus),
                    &voice->Sound);
                if (result != MA_SUCCESS)
                {
                    SetError(ResultMessage("Failed to create memory sound " + debugName, result));
                    return {};
                }
                voice->bSoundInitialized = true;
                return StartVoice(std::move(voice), params);
            }

            bool Stop(AudioHandle handle) override
            {
                std::scoped_lock lock(mMutex);
                Voice* voice = FindVoice(handle);
                if (!voice)
                    return false;
                ma_sound_stop(&voice->Sound);
                voice->bPaused = false;
                voice->bStopRequested = true;
                return true;
            }

            bool Pause(AudioHandle handle, bool bPaused) override
            {
                std::scoped_lock lock(mMutex);
                Voice* voice = FindVoice(handle);
                if (!voice || voice->bStopRequested)
                    return false;

                const ma_result result = bPaused
                    ? ma_sound_stop(&voice->Sound)
                    : ma_sound_start(&voice->Sound);
                if (result != MA_SUCCESS)
                {
                    SetError(ResultMessage("Failed to change sound pause state", result));
                    return false;
                }
                voice->bPaused = bPaused;
                return true;
            }

            [[nodiscard]] bool IsPlaying(AudioHandle handle) const override
            {
                std::scoped_lock lock(mMutex);
                const Voice* voice = FindVoice(handle);
                return voice &&
                    !voice->bStopRequested &&
                    !voice->bPaused &&
                    !(voice->Bus == AudioBus::Gameplay && mGameplayPaused) &&
                    ma_sound_is_playing(&voice->Sound);
            }

            [[nodiscard]] bool IsPaused(AudioHandle handle) const override
            {
                std::scoped_lock lock(mMutex);
                const Voice* voice = FindVoice(handle);
                return voice &&
                    !voice->bStopRequested &&
                    (voice->bPaused ||
                        (voice->Bus == AudioBus::Gameplay && mGameplayPaused));
            }

            [[nodiscard]] AudioPlaybackState GetState(AudioHandle handle) const override
            {
                std::scoped_lock lock(mMutex);
                const Voice* voice = FindVoice(handle);
                if (!voice)
                    return AudioPlaybackState::Invalid;
                if (voice->bStopRequested || ma_sound_at_end(&voice->Sound))
                    return AudioPlaybackState::Stopped;
                if (voice->bPaused ||
                    (voice->Bus == AudioBus::Gameplay && mGameplayPaused))
                {
                    return AudioPlaybackState::Paused;
                }
                return ma_sound_is_playing(&voice->Sound)
                    ? AudioPlaybackState::Playing
                    : AudioPlaybackState::Stopped;
            }

            bool SetVolume(AudioHandle handle, float volume) override
            {
                std::scoped_lock lock(mMutex);
                Voice* voice = FindVoice(handle);
                if (!voice)
                    return false;
                ma_sound_set_volume(&voice->Sound, NonNegative(volume, 1.0f));
                return true;
            }

            bool SetPitch(AudioHandle handle, float pitch) override
            {
                std::scoped_lock lock(mMutex);
                Voice* voice = FindVoice(handle);
                if (!voice)
                    return false;
                ma_sound_set_pitch(
                    &voice->Sound,
                    std::max(kMinimumPitch, FiniteOr(pitch, 1.0f)));
                return true;
            }

            bool SetPan(AudioHandle handle, float pan) override
            {
                std::scoped_lock lock(mMutex);
                Voice* voice = FindVoice(handle);
                if (!voice)
                    return false;
                ma_sound_set_pan(
                    &voice->Sound,
                    std::clamp(FiniteOr(pan, 0.0f), -1.0f, 1.0f));
                return true;
            }

            bool SetPosition(AudioHandle handle, const glm::vec3& position) override
            {
                std::scoped_lock lock(mMutex);
                Voice* voice = FindVoice(handle);
                if (!voice)
                    return false;
                const glm::vec3 safePosition = FiniteVector(position);
                ma_sound_set_position(
                    &voice->Sound,
                    safePosition.x,
                    safePosition.y,
                    safePosition.z);
                return true;
            }

            bool SetDirection(AudioHandle handle, const glm::vec3& direction) override
            {
                std::scoped_lock lock(mMutex);
                Voice* voice = FindVoice(handle);
                if (!voice)
                    return false;
                const glm::vec3 safeDirection = UnitVector(
                    direction,
                    glm::vec3(0.0f, 0.0f, -1.0f));
                ma_sound_set_direction(
                    &voice->Sound,
                    safeDirection.x,
                    safeDirection.y,
                    safeDirection.z);
                return true;
            }

            bool SetVelocity(AudioHandle handle, const glm::vec3& velocity) override
            {
                std::scoped_lock lock(mMutex);
                Voice* voice = FindVoice(handle);
                if (!voice)
                    return false;
                const glm::vec3 safeVelocity = FiniteVector(velocity);
                ma_sound_set_velocity(
                    &voice->Sound,
                    safeVelocity.x,
                    safeVelocity.y,
                    safeVelocity.z);
                return true;
            }

            bool SetCone(
                AudioHandle handle,
                float innerAngleRadians,
                float outerAngleRadians,
                float outerGain) override
            {
                std::scoped_lock lock(mMutex);
                Voice* voice = FindVoice(handle);
                if (!voice)
                    return false;
                const float inner = std::clamp(
                    FiniteOr(innerAngleRadians, kTwoPi),
                    0.0f,
                    kTwoPi);
                const float outer = std::clamp(
                    FiniteOr(outerAngleRadians, kTwoPi),
                    inner,
                    kTwoPi);
                const float gain = std::clamp(
                    FiniteOr(outerGain, 1.0f),
                    0.0f,
                    1.0f);
                ma_sound_set_cone(&voice->Sound, inner, outer, gain);
                return true;
            }

            bool Seek(AudioHandle handle, double seconds) override
            {
                std::scoped_lock lock(mMutex);
                Voice* voice = FindVoice(handle);
                if (!voice || !std::isfinite(seconds))
                    return false;
                const ma_result result = ma_sound_seek_to_second(
                    &voice->Sound,
                    static_cast<float>(std::max(0.0, seconds)));
                if (result != MA_SUCCESS)
                {
                    SetError(ResultMessage("Failed to seek sound " + voice->DebugName, result));
                    return false;
                }
                return true;
            }

            [[nodiscard]] double GetPlaybackPosition(AudioHandle handle) const override
            {
                std::scoped_lock lock(mMutex);
                const Voice* voice = FindVoice(handle);
                if (!voice)
                    return 0.0;
                float cursor = 0.0f;
                return ma_sound_get_cursor_in_seconds(&voice->Sound, &cursor) == MA_SUCCESS
                    ? static_cast<double>(cursor)
                    : 0.0;
            }

            [[nodiscard]] double GetLength(AudioHandle handle) const override
            {
                std::scoped_lock lock(mMutex);
                const Voice* voice = FindVoice(handle);
                if (!voice)
                    return 0.0;
                float length = 0.0f;
                return ma_sound_get_length_in_seconds(&voice->Sound, &length) == MA_SUCCESS
                    ? static_cast<double>(length)
                    : 0.0;
            }

            void SetMasterVolume(float volume) override
            {
                std::scoped_lock lock(mMutex);
                mMasterVolume = NonNegative(volume, 1.0f);
                if (mEngineInitialized)
                    ma_engine_set_volume(&mEngine, mMasterVolume);
            }

            [[nodiscard]] float GetMasterVolume() const override
            {
                std::scoped_lock lock(mMutex);
                return mMasterVolume;
            }

            void SetBusVolume(AudioBus bus, float volume) override
            {
                std::scoped_lock lock(mMutex);
                if (!IsValidBus(bus))
                    return;
                const size_t index = BusIndex(bus);
                mBusVolumes[index] = NonNegative(volume, 1.0f);
                if (mBusGroupInitialized[index])
                    ma_sound_group_set_volume(&mBusGroups[index], mBusVolumes[index]);
            }

            [[nodiscard]] float GetBusVolume(AudioBus bus) const override
            {
                std::scoped_lock lock(mMutex);
                return IsValidBus(bus) ? mBusVolumes[BusIndex(bus)] : 0.0f;
            }

            void StopBus(AudioBus bus) override
            {
                std::scoped_lock lock(mMutex);
                if (!IsValidBus(bus))
                    return;
                for (auto& [_, voice] : mVoices)
                {
                    if (voice->Bus != bus || voice->bStopRequested)
                        continue;
                    ma_sound_stop(&voice->Sound);
                    voice->bPaused = false;
                    voice->bStopRequested = true;
                }
            }

            void SetGameplayPaused(bool bPaused) override
            {
                std::scoped_lock lock(mMutex);
                if (mGameplayPaused == bPaused)
                    return;

                const size_t index = BusIndex(AudioBus::Gameplay);
                if (mBusGroupInitialized[index])
                {
                    const ma_result result = bPaused
                        ? ma_sound_group_stop(&mBusGroups[index])
                        : ma_sound_group_start(&mBusGroups[index]);
                    if (result != MA_SUCCESS)
                    {
                        SetError(ResultMessage("Failed to change gameplay audio pause state", result));
                        return;
                    }
                }
                mGameplayPaused = bPaused;
            }

            [[nodiscard]] bool IsGameplayPaused() const override
            {
                std::scoped_lock lock(mMutex);
                return mGameplayPaused;
            }

            bool SetListener(uint32_t index, const AudioListener& listener) override
            {
                std::scoped_lock lock(mMutex);
                if (!mInitialized || index >= kListenerCount)
                    return false;

                const glm::vec3 position = FiniteVector(listener.Position);
                const glm::vec3 direction = UnitVector(
                    listener.Forward,
                    glm::vec3(0.0f, 0.0f, -1.0f));
                const glm::vec3 up = UnitVector(
                    listener.Up,
                    glm::vec3(0.0f, 1.0f, 0.0f));
                const glm::vec3 velocity = FiniteVector(listener.Velocity);
                ma_engine_listener_set_position(
                    &mEngine,
                    index,
                    position.x,
                    position.y,
                    position.z);
                ma_engine_listener_set_direction(
                    &mEngine,
                    index,
                    direction.x,
                    direction.y,
                    direction.z);
                ma_engine_listener_set_world_up(
                    &mEngine,
                    index,
                    up.x,
                    up.y,
                    up.z);
                ma_engine_listener_set_velocity(
                    &mEngine,
                    index,
                    velocity.x,
                    velocity.y,
                    velocity.z);
                ma_engine_listener_set_enabled(
                    &mEngine,
                    index,
                    listener.bEnabled ? MA_TRUE : MA_FALSE);
                return true;
            }

        private:
            struct Voice
            {
                ~Voice()
                {
                    if (bSoundInitialized)
                        ma_sound_uninit(&Sound);
                    if (bDecoderInitialized)
                        ma_decoder_uninit(&Decoder);
                }

                ma_sound Sound{};
                ma_decoder Decoder{};
                std::shared_ptr<std::vector<uint8_t>> Memory;
                std::string DebugName;
                AudioBus Bus = AudioBus::Gameplay;
                bool bSoundInitialized = false;
                bool bDecoderInitialized = false;
                bool bLooping = false;
                bool bPaused = false;
                bool bStopRequested = false;
            };

            [[nodiscard]] static size_t BusIndex(AudioBus bus)
            {
                return static_cast<size_t>(bus);
            }

            [[nodiscard]] ma_sound_group* BusGroup(AudioBus bus)
            {
                return IsValidBus(bus) ? &mBusGroups[BusIndex(bus)] : nullptr;
            }

            [[nodiscard]] bool CanPlay(const AudioPlayParams& params)
            {
                if (!mInitialized)
                {
                    SetError("Audio engine is not initialized");
                    return false;
                }
                if (!IsValidBus(params.Bus))
                {
                    SetError("Invalid audio bus");
                    return false;
                }
                if (!IsValidAttenuation(params.Attenuation))
                {
                    SetError("Invalid audio attenuation model");
                    return false;
                }
                return true;
            }

            [[nodiscard]] AudioHandle StartVoice(
                std::unique_ptr<Voice> voice,
                const AudioPlayParams& params)
            {
                ApplyParams(*voice, params);
                if (params.StartTimeSeconds > 0.0)
                {
                    const ma_result seekResult = ma_sound_seek_to_second(
                        &voice->Sound,
                        static_cast<float>(params.StartTimeSeconds));
                    if (seekResult != MA_SUCCESS)
                    {
                        SetError(ResultMessage(
                            "Failed to set sound start position " + voice->DebugName,
                            seekResult));
                        return {};
                    }
                }

                voice->bPaused = params.bStartPaused;
                if (!params.bStartPaused)
                {
                    const ma_result startResult = ma_sound_start(&voice->Sound);
                    if (startResult != MA_SUCCESS)
                    {
                        SetError(ResultMessage("Failed to start sound " + voice->DebugName, startResult));
                        return {};
                    }
                }

                const AudioHandle handle = NextHandle();
                mVoices.emplace(handle.Value, std::move(voice));
                return handle;
            }

            void ApplyParams(Voice& voice, const AudioPlayParams& params)
            {
                const glm::vec3 position = FiniteVector(params.Position);
                const glm::vec3 direction = UnitVector(
                    params.Direction,
                    glm::vec3(0.0f, 0.0f, -1.0f));
                const glm::vec3 velocity = FiniteVector(params.Velocity);
                const float minimumDistance = std::max(
                    kMinimumDistance,
                    FiniteOr(params.MinDistance, 1.0f));
                const float maximumDistance = std::max(
                    minimumDistance,
                    FiniteOr(params.MaxDistance, 100.0f));
                const float innerCone = std::clamp(
                    FiniteOr(params.ConeInnerAngleRadians, kTwoPi),
                    0.0f,
                    kTwoPi);
                const float outerCone = std::clamp(
                    FiniteOr(params.ConeOuterAngleRadians, kTwoPi),
                    innerCone,
                    kTwoPi);

                ma_sound_set_volume(&voice.Sound, NonNegative(params.Volume, 1.0f));
                ma_sound_set_pitch(
                    &voice.Sound,
                    std::max(kMinimumPitch, FiniteOr(params.Pitch, 1.0f)));
                ma_sound_set_pan(
                    &voice.Sound,
                    std::clamp(FiniteOr(params.Pan, 0.0f), -1.0f, 1.0f));
                ma_sound_set_looping(&voice.Sound, params.bLooping ? MA_TRUE : MA_FALSE);
                ma_sound_set_spatialization_enabled(
                    &voice.Sound,
                    params.bSpatialized ? MA_TRUE : MA_FALSE);
                ma_sound_set_position(
                    &voice.Sound,
                    position.x,
                    position.y,
                    position.z);
                ma_sound_set_direction(
                    &voice.Sound,
                    direction.x,
                    direction.y,
                    direction.z);
                ma_sound_set_velocity(
                    &voice.Sound,
                    velocity.x,
                    velocity.y,
                    velocity.z);
                ma_sound_set_attenuation_model(
                    &voice.Sound,
                    ToMiniaudioAttenuation(params.Attenuation));
                ma_sound_set_min_distance(&voice.Sound, minimumDistance);
                ma_sound_set_max_distance(&voice.Sound, maximumDistance);
                ma_sound_set_rolloff(&voice.Sound, NonNegative(params.Rolloff, 1.0f));
                ma_sound_set_doppler_factor(
                    &voice.Sound,
                    NonNegative(params.DopplerFactor, 1.0f));
                ma_sound_set_cone(
                    &voice.Sound,
                    innerCone,
                    outerCone,
                    std::clamp(FiniteOr(params.ConeOuterGain, 1.0f), 0.0f, 1.0f));
            }

            [[nodiscard]] AudioHandle NextHandle()
            {
                for (;;)
                {
                    if (mNextHandle == 0)
                        mNextHandle = 1;
                    const uint64_t value = mNextHandle++;
                    if (!mVoices.contains(value))
                        return AudioHandle{ value };
                }
            }

            [[nodiscard]] Voice* FindVoice(AudioHandle handle)
            {
                if (!handle.IsValid())
                    return nullptr;
                const auto iterator = mVoices.find(handle.Value);
                return iterator != mVoices.end() ? iterator->second.get() : nullptr;
            }

            [[nodiscard]] const Voice* FindVoice(AudioHandle handle) const
            {
                if (!handle.IsValid())
                    return nullptr;
                const auto iterator = mVoices.find(handle.Value);
                return iterator != mVoices.end() ? iterator->second.get() : nullptr;
            }

            [[nodiscard]] bool DetectPhysicalDevice() const
            {
                const ma_device* device = ma_engine_get_device(
                    const_cast<ma_engine*>(&mEngine));
                return device &&
                    device->pContext &&
                    device->pContext->backend != ma_backend_null;
            }

            [[nodiscard]] static std::string ResultMessage(
                const std::string& prefix,
                ma_result result)
            {
                return prefix + ": " + ma_result_description(result);
            }

            void SetError(std::string message)
            {
                mLastError = std::move(message);
                HE_CORE_WARN("[Audio] {0}", mLastError);
            }

            void ShutdownUnlocked()
            {
                mVoices.clear();
                for (size_t index = kAudioBusCount; index > 0; --index)
                {
                    const size_t groupIndex = index - 1;
                    if (!mBusGroupInitialized[groupIndex])
                        continue;
                    ma_sound_group_uninit(&mBusGroups[groupIndex]);
                    mBusGroupInitialized[groupIndex] = false;
                }
                if (mEngineInitialized)
                {
                    ma_engine_uninit(&mEngine);
                    mEngineInitialized = false;
                }
                if (mNullContextInitialized)
                {
                    ma_context_uninit(&mNullContext);
                    mNullContextInitialized = false;
                }
                mInitialized = false;
                mDeviceAvailable = false;
            }

            mutable std::mutex mMutex;
            ma_engine mEngine{};
            ma_context mNullContext{};
            std::array<ma_sound_group, kAudioBusCount> mBusGroups{};
            std::array<bool, kAudioBusCount> mBusGroupInitialized{};
            std::array<float, kAudioBusCount> mBusVolumes{ 1.0f, 1.0f, 1.0f };
            std::unordered_map<uint64_t, std::unique_ptr<Voice>> mVoices;
            std::string mLastError;
            uint64_t mNextHandle = 1;
            float mMasterVolume = 1.0f;
            bool mInitialized = false;
            bool mEngineInitialized = false;
            bool mNullContextInitialized = false;
            bool mDeviceAvailable = false;
            bool mGameplayPaused = false;
        };
    }

    Scope<AudioBackend> CreatePlatformAudioBackend()
    {
        return CreateScope<MiniaudioAudioBackend>();
    }
}
