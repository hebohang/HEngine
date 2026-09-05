#include "Hepch.h"

#include "Runtime/Audio/AudioEngine.h"

#include "Runtime/Audio/AudioBackend.h"

namespace HEngine
{
    struct AudioEngine::Impl
    {
        Scope<AudioBackend> Backend;
    };

    AudioEngine& AudioEngine::GetInstance()
    {
        static AudioEngine instance;
        return instance;
    }

    AudioEngine::AudioEngine()
        : AudioEngine(nullptr)
    {
    }

    AudioEngine::AudioEngine(Scope<AudioBackend> backend)
        : mImpl(CreateScope<Impl>())
    {
        mImpl->Backend = std::move(backend);
    }

    AudioEngine::~AudioEngine()
    {
        Shutdown();
    }

    bool AudioEngine::Initialize()
    {
        if (!mImpl->Backend)
            mImpl->Backend = CreatePlatformAudioBackend();
        return mImpl->Backend && mImpl->Backend->Initialize();
    }

    void AudioEngine::Shutdown()
    {
        if (mImpl->Backend)
            mImpl->Backend->Shutdown();
    }

    void AudioEngine::Update()
    {
        if (mImpl->Backend)
            mImpl->Backend->Update();
    }

    bool AudioEngine::IsInitialized() const
    {
        return mImpl->Backend && mImpl->Backend->IsInitialized();
    }

    bool AudioEngine::IsDeviceAvailable() const
    {
        return mImpl->Backend && mImpl->Backend->IsDeviceAvailable();
    }

    std::string AudioEngine::GetLastError() const
    {
        return mImpl->Backend ? mImpl->Backend->GetLastError() : "Audio backend is not created";
    }

    AudioHandle AudioEngine::Play(
        const std::filesystem::path& path,
        const AudioPlayParams& params)
    {
        return mImpl->Backend ? mImpl->Backend->Play(path, params) : AudioHandle{};
    }

    AudioHandle AudioEngine::PlayMemory(
        std::shared_ptr<std::vector<uint8_t>> data,
        const std::string& debugName,
        const AudioPlayParams& params)
    {
        return mImpl->Backend
            ? mImpl->Backend->PlayMemory(std::move(data), debugName, params)
            : AudioHandle{};
    }

    bool AudioEngine::Stop(AudioHandle handle)
    {
        return mImpl->Backend && mImpl->Backend->Stop(handle);
    }

    bool AudioEngine::Pause(AudioHandle handle, bool bPaused)
    {
        return mImpl->Backend && mImpl->Backend->Pause(handle, bPaused);
    }

    bool AudioEngine::IsPlaying(AudioHandle handle) const
    {
        return mImpl->Backend && mImpl->Backend->IsPlaying(handle);
    }

    bool AudioEngine::IsPaused(AudioHandle handle) const
    {
        return mImpl->Backend && mImpl->Backend->IsPaused(handle);
    }

    AudioPlaybackState AudioEngine::GetState(AudioHandle handle) const
    {
        return mImpl->Backend
            ? mImpl->Backend->GetState(handle)
            : AudioPlaybackState::Invalid;
    }

    bool AudioEngine::SetVolume(AudioHandle handle, float volume)
    {
        return mImpl->Backend && mImpl->Backend->SetVolume(handle, volume);
    }

    bool AudioEngine::SetPitch(AudioHandle handle, float pitch)
    {
        return mImpl->Backend && mImpl->Backend->SetPitch(handle, pitch);
    }

    bool AudioEngine::SetPan(AudioHandle handle, float pan)
    {
        return mImpl->Backend && mImpl->Backend->SetPan(handle, pan);
    }

    bool AudioEngine::SetPosition(AudioHandle handle, const glm::vec3& position)
    {
        return mImpl->Backend && mImpl->Backend->SetPosition(handle, position);
    }

    bool AudioEngine::SetDirection(AudioHandle handle, const glm::vec3& direction)
    {
        return mImpl->Backend && mImpl->Backend->SetDirection(handle, direction);
    }

    bool AudioEngine::SetVelocity(AudioHandle handle, const glm::vec3& velocity)
    {
        return mImpl->Backend && mImpl->Backend->SetVelocity(handle, velocity);
    }

    bool AudioEngine::SetCone(
        AudioHandle handle,
        float innerAngleRadians,
        float outerAngleRadians,
        float outerGain)
    {
        return mImpl->Backend && mImpl->Backend->SetCone(
            handle,
            innerAngleRadians,
            outerAngleRadians,
            outerGain);
    }

    bool AudioEngine::Seek(AudioHandle handle, double seconds)
    {
        return mImpl->Backend && mImpl->Backend->Seek(handle, seconds);
    }

    double AudioEngine::GetPlaybackPosition(AudioHandle handle) const
    {
        return mImpl->Backend ? mImpl->Backend->GetPlaybackPosition(handle) : 0.0;
    }

    double AudioEngine::GetLength(AudioHandle handle) const
    {
        return mImpl->Backend ? mImpl->Backend->GetLength(handle) : 0.0;
    }

    void AudioEngine::SetMasterVolume(float volume)
    {
        if (mImpl->Backend)
            mImpl->Backend->SetMasterVolume(volume);
    }

    float AudioEngine::GetMasterVolume() const
    {
        return mImpl->Backend ? mImpl->Backend->GetMasterVolume() : 1.0f;
    }

    void AudioEngine::SetBusVolume(AudioBus bus, float volume)
    {
        if (mImpl->Backend)
            mImpl->Backend->SetBusVolume(bus, volume);
    }

    float AudioEngine::GetBusVolume(AudioBus bus) const
    {
        return mImpl->Backend ? mImpl->Backend->GetBusVolume(bus) : 1.0f;
    }

    void AudioEngine::StopBus(AudioBus bus)
    {
        if (mImpl->Backend)
            mImpl->Backend->StopBus(bus);
    }

    void AudioEngine::SetGameplayPaused(bool bPaused)
    {
        if (mImpl->Backend)
            mImpl->Backend->SetGameplayPaused(bPaused);
    }

    bool AudioEngine::IsGameplayPaused() const
    {
        return mImpl->Backend && mImpl->Backend->IsGameplayPaused();
    }

    bool AudioEngine::SetListener(uint32_t index, const AudioListener& listener)
    {
        return mImpl->Backend && mImpl->Backend->SetListener(index, listener);
    }
}
