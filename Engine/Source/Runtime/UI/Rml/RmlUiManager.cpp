#include "Hepch.h"
#include "RmlBuildFeatures.h"

#include "Runtime/UI/Rml/RmlUiManager.h"

#include "Runtime/Core/Input/Input.h"
#include "Runtime/Core/Input/KeyCodes.h"
#include "Runtime/Core/Input/MouseCodes.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/Core/PresentationClock.h"
#include "Runtime/Core/Window/Window.h"
#include "Runtime/EcsFramework/System/UI/UIViewportContext.h"
#include "Runtime/EcsFramework/System/Script/PythonScriptSystem.h"
#include "Runtime/Events/Event.h"
#include "Runtime/Events/KeyEvent.h"
#include "Runtime/Events/MouseEvent.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/ResType/UIRes.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeUiResource.h"
#include "Runtime/Scripting/ScriptEngine.h"
#include "Runtime/UI/Rml/RmlRenderBackend.h"
#include "Runtime/UI/Rml/RmlUiEventPayload.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#if HE_ENABLE_GM
#include "Runtime/DevTools/GM/GMPanelService.h"
#endif

#include <RmlUi/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/URL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace HEngine
{
    namespace
    {
        bool TraceUiInput()
        {
            static const bool enabled = [] { const char* value = std::getenv("HE_TRACE_UI_INPUT"); return value && std::string_view(value) == "1"; }();
            return enabled;
        }

        constexpr const char* kRmlContextName = "HEngineRuntimeUI";
        constexpr float kReferenceUiWidth = 1280.0f;
        constexpr float kReferenceUiHeight = 720.0f;

		void ReportRmlCleanupFailure(
			std::string_view operation,
			const std::exception_ptr& failure) noexcept
		{
			try
			{
				if (failure)
					std::rethrow_exception(failure);
			}
			catch (const std::exception& exception)
			{
				Log::WriteFallbackError(operation, exception.what());
			}
			catch (...)
			{
				Log::WriteFallbackError(operation, "unknown exception");
			}
		}

        static float ComputeRmlDensityScale(float viewportWidth, float viewportHeight, float userScale)
        {
            // Runtime documents target 1280x720, so preserve their screen-space
            // proportions independently of the desktop's DPI setting.
            const float viewportFitScale = std::min(
                std::max(viewportWidth, 1.0f) / kReferenceUiWidth,
                std::max(viewportHeight, 1.0f) / kReferenceUiHeight);
            return std::clamp(viewportFitScale * userScale, 0.5f, 3.0f);
        }

        class ScopedCurrentScriptOwner
        {
        public:
            explicit ScopedCurrentScriptOwner(const UUID& owner)
            {
                PythonScriptSystem::SetCurrentScriptOwner(owner);
            }

            ~ScopedCurrentScriptOwner()
            {
                PythonScriptSystem::ClearCurrentScriptOwner();
            }
        };

        static std::string NormalizeProjectPath(const std::string& path)
        {
            if (path.empty())
                return path;

            const std::filesystem::path filesystemPath = Utils::Utf8ToPath(path);
            if (filesystemPath.is_absolute())
                return Utils::NormalizeAssetPath(ConfigManager::GetRelativePath(filesystemPath));

            return Utils::NormalizeAssetPath(path);
        }

        static bool IsProjectRootRelativePath(const std::string& path)
        {
            return path == "Assets" || path.rfind("Assets/", 0) == 0 ||
                path == "Scripts" || path.rfind("Scripts/", 0) == 0 ||
                path == "Shaders" || path.rfind("Shaders/", 0) == 0 ||
                path == "Config" || path.rfind("Config/", 0) == 0;
        }

        static bool IsPathWithinRoot(
            const std::filesystem::path& candidate,
            const std::filesystem::path& root)
        {
            std::error_code candidateError;
            std::error_code rootError;
            std::filesystem::path candidateCanonical =
                std::filesystem::weakly_canonical(candidate, candidateError);
            std::filesystem::path rootCanonical =
                std::filesystem::weakly_canonical(root, rootError);
            if (candidateError || rootError)
                return false;

#if defined(_WIN32)
            std::string candidateText = Rml::StringUtilities::ToLower(Utils::PathToUtf8(candidateCanonical));
            std::string rootText = Rml::StringUtilities::ToLower(Utils::PathToUtf8(rootCanonical));
#else
            std::string candidateText = Utils::PathToUtf8(candidateCanonical);
            std::string rootText = Utils::PathToUtf8(rootCanonical);
#endif
            if (!rootText.ends_with('/'))
                rootText.push_back('/');
            return candidateText == rootText.substr(0, rootText.size() - 1) ||
                candidateText.starts_with(rootText);
        }

        static bool IsFallbackFontPath(const std::string& path)
        {
            const std::string lower = Rml::StringUtilities::ToLower(path);
            return lower.find("notosanssc") != std::string::npos || lower.find("noto_sans_sc") != std::string::npos;
        }

        static int GetRmlKeyModifiers()
        {
            int modifiers = 0;
            if (Input::IsKeyPressed(Key::LeftControl) || Input::IsKeyPressed(Key::RightControl))
                modifiers |= Rml::Input::KM_CTRL;
            if (Input::IsKeyPressed(Key::LeftShift) || Input::IsKeyPressed(Key::RightShift))
                modifiers |= Rml::Input::KM_SHIFT;
            if (Input::IsKeyPressed(Key::LeftAlt) || Input::IsKeyPressed(Key::RightAlt))
                modifiers |= Rml::Input::KM_ALT;
            return modifiers;
        }

        static Rml::Input::KeyIdentifier ToRmlKeyIdentifier(KeyCode key)
        {
            switch (key)
            {
            case Key::Space: return Rml::Input::KI_SPACE;
            case Key::D0: return Rml::Input::KI_0;
            case Key::D1: return Rml::Input::KI_1;
            case Key::D2: return Rml::Input::KI_2;
            case Key::D3: return Rml::Input::KI_3;
            case Key::D4: return Rml::Input::KI_4;
            case Key::D5: return Rml::Input::KI_5;
            case Key::D6: return Rml::Input::KI_6;
            case Key::D7: return Rml::Input::KI_7;
            case Key::D8: return Rml::Input::KI_8;
            case Key::D9: return Rml::Input::KI_9;
            case Key::A: return Rml::Input::KI_A;
            case Key::B: return Rml::Input::KI_B;
            case Key::C: return Rml::Input::KI_C;
            case Key::D: return Rml::Input::KI_D;
            case Key::E: return Rml::Input::KI_E;
            case Key::F: return Rml::Input::KI_F;
            case Key::G: return Rml::Input::KI_G;
            case Key::H: return Rml::Input::KI_H;
            case Key::I: return Rml::Input::KI_I;
            case Key::J: return Rml::Input::KI_J;
            case Key::K: return Rml::Input::KI_K;
            case Key::L: return Rml::Input::KI_L;
            case Key::M: return Rml::Input::KI_M;
            case Key::N: return Rml::Input::KI_N;
            case Key::O: return Rml::Input::KI_O;
            case Key::P: return Rml::Input::KI_P;
            case Key::Q: return Rml::Input::KI_Q;
            case Key::R: return Rml::Input::KI_R;
            case Key::S: return Rml::Input::KI_S;
            case Key::T: return Rml::Input::KI_T;
            case Key::U: return Rml::Input::KI_U;
            case Key::V: return Rml::Input::KI_V;
            case Key::W: return Rml::Input::KI_W;
            case Key::X: return Rml::Input::KI_X;
            case Key::Y: return Rml::Input::KI_Y;
            case Key::Z: return Rml::Input::KI_Z;
            case Key::Semicolon: return Rml::Input::KI_OEM_1;
            case Key::Equal: return Rml::Input::KI_OEM_PLUS;
            case Key::Comma: return Rml::Input::KI_OEM_COMMA;
            case Key::Minus: return Rml::Input::KI_OEM_MINUS;
            case Key::Period: return Rml::Input::KI_OEM_PERIOD;
            case Key::Slash: return Rml::Input::KI_OEM_2;
            case Key::GraveAccent: return Rml::Input::KI_OEM_3;
            case Key::LeftBracket: return Rml::Input::KI_OEM_4;
            case Key::Backslash: return Rml::Input::KI_OEM_5;
            case Key::RightBracket: return Rml::Input::KI_OEM_6;
            case Key::Apostrophe: return Rml::Input::KI_OEM_7;
            case Key::Backspace: return Rml::Input::KI_BACK;
            case Key::Tab: return Rml::Input::KI_TAB;
            case Key::Enter: return Rml::Input::KI_RETURN;
            case Key::Pause: return Rml::Input::KI_PAUSE;
            case Key::CapsLock: return Rml::Input::KI_CAPITAL;
            case Key::Escape: return Rml::Input::KI_ESCAPE;
            case Key::PageUp: return Rml::Input::KI_PRIOR;
            case Key::PageDown: return Rml::Input::KI_NEXT;
            case Key::End: return Rml::Input::KI_END;
            case Key::Home: return Rml::Input::KI_HOME;
            case Key::Left: return Rml::Input::KI_LEFT;
            case Key::Up: return Rml::Input::KI_UP;
            case Key::Right: return Rml::Input::KI_RIGHT;
            case Key::Down: return Rml::Input::KI_DOWN;
            case Key::Insert: return Rml::Input::KI_INSERT;
            case Key::Delete: return Rml::Input::KI_DELETE;
            case Key::F1: return Rml::Input::KI_F1;
            case Key::F2: return Rml::Input::KI_F2;
            case Key::F3: return Rml::Input::KI_F3;
            case Key::F4: return Rml::Input::KI_F4;
            case Key::F5: return Rml::Input::KI_F5;
            case Key::F6: return Rml::Input::KI_F6;
            case Key::F7: return Rml::Input::KI_F7;
            case Key::F8: return Rml::Input::KI_F8;
            case Key::F9: return Rml::Input::KI_F9;
            case Key::F10: return Rml::Input::KI_F10;
            case Key::F11: return Rml::Input::KI_F11;
            case Key::F12: return Rml::Input::KI_F12;
            case Key::NumLock: return Rml::Input::KI_NUMLOCK;
            case Key::ScrollLock: return Rml::Input::KI_SCROLL;
            case Key::KP0: return Rml::Input::KI_NUMPAD0;
            case Key::KP1: return Rml::Input::KI_NUMPAD1;
            case Key::KP2: return Rml::Input::KI_NUMPAD2;
            case Key::KP3: return Rml::Input::KI_NUMPAD3;
            case Key::KP4: return Rml::Input::KI_NUMPAD4;
            case Key::KP5: return Rml::Input::KI_NUMPAD5;
            case Key::KP6: return Rml::Input::KI_NUMPAD6;
            case Key::KP7: return Rml::Input::KI_NUMPAD7;
            case Key::KP8: return Rml::Input::KI_NUMPAD8;
            case Key::KP9: return Rml::Input::KI_NUMPAD9;
            case Key::KPEnter: return Rml::Input::KI_NUMPADENTER;
            case Key::KPMultiply: return Rml::Input::KI_MULTIPLY;
            case Key::KPAdd: return Rml::Input::KI_ADD;
            case Key::KPSubtract: return Rml::Input::KI_SUBTRACT;
            case Key::KPDecimal: return Rml::Input::KI_DECIMAL;
            case Key::KPDivide: return Rml::Input::KI_DIVIDE;
            case Key::LeftShift: return Rml::Input::KI_LSHIFT;
            case Key::RightShift: return Rml::Input::KI_RSHIFT;
            case Key::LeftControl: return Rml::Input::KI_LCONTROL;
            case Key::RightControl: return Rml::Input::KI_RCONTROL;
            case Key::LeftAlt: return Rml::Input::KI_LMENU;
            case Key::RightAlt: return Rml::Input::KI_RMENU;
            default:
                break;
            }

            return Rml::Input::KI_UNKNOWN;
        }

        class RmlFileInterface final : public Rml::FileInterface
        {
        public:
            Rml::FileHandle Open(const Rml::String& path) override
            {
                const std::filesystem::path fullPath = Utils::Utf8ToPath(path.c_str());
                if (!fullPath.is_absolute() ||
                    (!IsPathWithinRoot(fullPath, ConfigManager::GetInstance().GetRootFolder()) &&
                     !IsPathWithinRoot(fullPath, ConfigManager::GetInstance().GetEngineContentFolder())))
                {
                    HE_CORE_ERROR("RmlUi rejected an out-of-root resource path: {0}", path.c_str());
                    return 0;
                }
                std::error_code statusError;
                if (!std::filesystem::is_regular_file(fullPath, statusError) || statusError)
                    return 0;
                const std::uintmax_t fileSize = std::filesystem::file_size(fullPath, statusError);
                const std::string extension = Rml::StringUtilities::ToLower(
                    Utils::PathToUtf8(fullPath.extension()));
                const std::uintmax_t sizeLimit =
                    (extension == ".svg" || extension == ".json")
                    ? 1024u * 1024u
                    : 64u * 1024u * 1024u;
                if (statusError || fileSize > sizeLimit)
                {
                    HE_CORE_ERROR(
                        "RmlUi rejected resource '{0}' because it exceeds the {1} byte budget.",
                        path.c_str(),
                        sizeLimit);
                    return 0;
                }
                FILE* file = nullptr;
#if defined(_WIN32)
                _wfopen_s(&file, fullPath.c_str(), L"rb");
#else
                const std::string utf8Path = Utils::PathToUtf8(fullPath);
                file = std::fopen(utf8Path.c_str(), "rb");
#endif
                return reinterpret_cast<Rml::FileHandle>(file);
            }

            void Close(Rml::FileHandle file) override
            {
                if (FILE* fp = reinterpret_cast<FILE*>(file))
                    fclose(fp);
            }

            size_t Read(void* buffer, size_t size, Rml::FileHandle file) override
            {
                if (FILE* fp = reinterpret_cast<FILE*>(file))
                    return fread(buffer, 1, size, fp);
                return 0;
            }

            bool Seek(Rml::FileHandle file, long offset, int origin) override
            {
                if (FILE* fp = reinterpret_cast<FILE*>(file))
                    return fseek(fp, offset, origin) == 0;
                return false;
            }

            size_t Tell(Rml::FileHandle file) override
            {
                if (FILE* fp = reinterpret_cast<FILE*>(file))
                    return static_cast<size_t>(ftell(fp));
                return 0;
            }
        };

        class RmlSystemInterface final : public Rml::SystemInterface
        {
        public:
            double GetElapsedTime() override
            {
                return PresentationClock::GetInstance().GetElapsedSeconds();
            }

            void JoinPath(Rml::String& translated_path, const Rml::String& document_path, const Rml::String& path) override
            {
                if (path.empty())
                {
                    translated_path.clear();
                    return;
                }

                const std::string normalizedPath = Utils::NormalizeAssetPath(path.c_str());
                if (IsProjectRootRelativePath(normalizedPath))
                {
                    translated_path = Utils::PathToUtf8(
                        ConfigManager::GetFullPath(normalizedPath).lexically_normal());
                    return;
                }

                if (path.size() > 0 && path[0] == '/')
                {
                    translated_path = path.substr(1);
                    return;
                }

                const size_t drivePos = path.find(':');
                const size_t slashPos = std::min(path.find('/'), path.find('\\'));
                if (drivePos != Rml::String::npos && drivePos < slashPos)
                {
                    translated_path = path;
                    return;
                }

                using Rml::StringUtilities::Replace;

                translated_path = Replace(document_path, '\\', '/');
                const size_t fileStart = translated_path.rfind('/');
                if (fileStart != Rml::String::npos)
                    translated_path.resize(fileStart + 1);
                else
                    translated_path.clear();

                Rml::URL url(Replace(translated_path, ':', '|') + Replace(path, '\\', '/'));
                translated_path = Replace(url.GetPathedFileName(), '|', ':');
            }

            bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
            {
                switch (type)
                {
                case Rml::Log::Type::LT_ERROR:
                    HE_CORE_ERROR("[RmlUi] {0}", message.c_str());
                    break;
                case Rml::Log::Type::LT_WARNING:
                    HE_CORE_WARN("[RmlUi] {0}", message.c_str());
                    break;
                default:
                    HE_CORE_INFO("[RmlUi] {0}", message.c_str());
                    break;
                }
                return true;
            }
        };

        class RmlEventListener final : public Rml::EventListener
        {
        public:
            struct EventData
            {
                std::string Type;
                std::string LegacySourceId;
                std::string CurrentElementId;
                std::string TargetElementId;
                std::string Property;
                std::string AnimationName;
                float ElapsedTime = 0.0f;
                bool bHasElapsedTime = false;
                float MouseX = 0.0f;
                float MouseY = 0.0f;
                float WheelDelta = 0.0f;
                int MouseButton = 0;
                bool bHasMouseX = false;
                bool bHasMouseY = false;
                bool bHasWheelDelta = false;
                bool bHasMouseButton = false;
            };

            using EventCallback = std::function<void(const EventData&)>;

            explicit RmlEventListener(EventCallback callback)
                : mCallback(std::move(callback))
            {
            }

            void ProcessEvent(Rml::Event& event) override
            {
                if (!mCallback)
                    return;

                const auto getExactElementId = [](Rml::Element* element)
                {
                    return element && !element->GetId().empty()
                        ? std::string(element->GetId().c_str())
                        : std::string{};
                };
                const auto findLegacyEventSourceId = [](Rml::Element* element)
                {
                    std::string closestId;
                    while (element)
                    {
                        const Rml::String& id = element->GetId();
                        if (!id.empty())
                        {
                            if (closestId.empty())
                                closestId = id.c_str();

                            const Rml::String& tagName = element->GetTagName();
                            if (tagName == "button" || tagName == "input" || tagName == "select" || tagName == "textarea")
                                return std::string(id.c_str());
                        }
                        element = element->GetParentNode();
                    }
                    return closestId;
                };

                Rml::Element* current = event.GetCurrentElement();
                Rml::Element* target = event.GetTargetElement();
                EventData data;
                data.Type = event.GetType().c_str();
                data.CurrentElementId = getExactElementId(current);
                data.TargetElementId = getExactElementId(target);

                const bool bAnimationEvent = data.Type == "animationend" || data.Type == "transitionend";
                data.LegacySourceId = bAnimationEvent
                    ? data.TargetElementId
                    : findLegacyEventSourceId(target);
                if (data.LegacySourceId.empty())
                    data.LegacySourceId = findLegacyEventSourceId(current);

                data.Property = event.GetParameter<Rml::String>("property", "").c_str();
                data.AnimationName = event.GetParameter<Rml::String>("animation_name", "").c_str();
                const Rml::Dictionary& parameters = event.GetParameters();
                if (parameters.find("elapsed_time") != parameters.end())
                {
                    data.ElapsedTime = event.GetParameter<float>("elapsed_time", 0.0f);
                    data.bHasElapsedTime = true;
                }
                if (parameters.find("mouse_x") != parameters.end())
                {
                    data.MouseX = event.GetParameter<float>("mouse_x", 0.0f);
                    data.bHasMouseX = true;
                }
                if (parameters.find("mouse_y") != parameters.end())
                {
                    data.MouseY = event.GetParameter<float>("mouse_y", 0.0f);
                    data.bHasMouseY = true;
                }
                if (parameters.find("wheel_delta") != parameters.end())
                {
                    data.WheelDelta = event.GetParameter<float>("wheel_delta", 0.0f);
                    data.bHasWheelDelta = true;
                }
                if (parameters.find("button") != parameters.end())
                {
                    data.MouseButton = event.GetParameter<int>("button", 0);
                    data.bHasMouseButton = true;
                }
                if (TraceUiInput() && (data.Type == "mousedown" || data.Type == "mouseup" || data.Type == "click"))
                    HE_CORE_INFO("[UIInput] DOM event={} target={} current={} legacy={} mouse={},{} button={}",
                        data.Type, data.TargetElementId, data.CurrentElementId, data.LegacySourceId,
                        data.MouseX, data.MouseY, data.MouseButton);
                mCallback(data);
            }

        private:
            EventCallback mCallback;
        };
    }

    struct RmlUiManager::DocumentState
    {
        std::string DocumentId;
        std::string UiAssetPath;
        std::string RmlPath;
        std::string OwnerScriptId;
        Rml::ElementDocument* Document = nullptr;
        bool bVisible = false;
        bool bMouseCursorVisible = false;
        std::string MouseCursorAssetPath;
        uint32_t MouseCursorHotspotX = 0;
        uint32_t MouseCursorHotspotY = 0;
        bool bRegisteredRootEvents = false;
        Scope<RmlEventListener> EventListener;
        std::function<void(const RmlUiNativeEvent&)> NativeEventHandler;
        RuntimeResourceHandle<RuntimeUiDefinition> UiDefinitionHandle;
    };

    struct RmlUiManager::Impl
    {
        Scope<RmlFileInterface> FileInterface;
        Scope<RmlSystemInterface> SystemInterface;
        Scope<INvrhiRmlRenderBackend> Renderer;
        Rml::Context* Context = nullptr;
    };

    RmlUiManager::~RmlUiManager() = default;

	bool RmlUiManager::ShouldDeferDocumentMutation() const noexcept
	{
		return mDispatchDepth != 0 || mDrainingDeferredMutations;
	}

	void RmlUiManager::QueueDeferredMutation(DeferredMutationType type, std::string value)
	{
		mDeferredMutations.push_back({ type, std::move(value) });
	}

	void RmlUiManager::BeginDocumentDispatch() noexcept
	{
		++mDispatchDepth;
	}

	void RmlUiManager::EndDocumentDispatch()
	{
		if (mDispatchDepth == 0)
			throw std::logic_error("RmlUi document dispatch depth underflow.");

		--mDispatchDepth;
		if (mDispatchDepth == 0)
			DrainDeferredMutations();
	}

	void RmlUiManager::DrainDeferredMutations()
	{
		if (mDispatchDepth != 0 || mDrainingDeferredMutations || mDeferredMutations.empty())
			return;

		mDrainingDeferredMutations = true;
		struct DrainStateReset
		{
			bool& State;

			~DrainStateReset()
			{
				State = false;
			}
		} drainStateReset{ mDrainingDeferredMutations };

		std::exception_ptr firstFailure;
		while (!mDeferredMutations.empty())
		{
			DeferredMutation mutation = std::move(mDeferredMutations.front());
			mDeferredMutations.pop_front();
			try
			{
				switch (mutation.Type)
				{
					case DeferredMutationType::Reload:
						(void)ReloadDocumentNow(mutation.Value);
						break;
					case DeferredMutationType::Unload:
						(void)UnloadDocumentNow(mutation.Value);
						break;
					case DeferredMutationType::ClearOwner:
						ClearOwnerDocumentsNow(UUID(mutation.Value));
						break;
				}
			}
			catch (...)
			{
				const std::exception_ptr failure = std::current_exception();
				if (!firstFailure)
					firstFailure = failure;
				else
					ReportRmlCleanupFailure("Deferred RmlUi document mutation failed.", failure);
			}
		}

		if (firstFailure)
			std::rethrow_exception(firstFailure);
	}

    void RmlUiManager::Init()
    {
        if (mInitialized)
            return;

        HE_CORE_ERROR(
            "RmlUi requires explicit NVRHI initialization. "
            "Call InitNvrhi after the DX12 RenderDevice is initialized.");
    }

    void RmlUiManager::InitNvrhi(const NvrhiRmlRenderBackendDesc& desc)
    {
        InitNvrhiInternal(desc, nullptr);
    }

    void RmlUiManager::InitNvrhi(
        const NvrhiRmlRenderBackendDesc& desc,
        Window& cursorWindow)
    {
        InitNvrhiInternal(desc, &cursorWindow);
    }

    void RmlUiManager::InitNvrhiInternal(
        const NvrhiRmlRenderBackendDesc& desc,
        Window* cursorWindow)
    {
        if (mInitialized || mRmlInitialized || mImpl)
            throw std::logic_error("RmlUi manager is already initialized");

        try
        {
            mImpl = CreateRef<Impl>();
            mCursorWindow = cursorWindow;
            mImpl->FileInterface = CreateScope<RmlFileInterface>();
            mImpl->SystemInterface = CreateScope<RmlSystemInterface>();
            mImpl->Renderer = CreateNvrhiRmlRenderBackend(desc);

            Rml::SetFileInterface(mImpl->FileInterface.get());
            Rml::SetSystemInterface(mImpl->SystemInterface.get());
            Rml::SetRenderInterface(mImpl->Renderer->GetRenderInterface());

            if (!Rml::Initialise())
                throw std::runtime_error("Failed to initialize RmlUi");
            mRmlInitialized = true;
            RegisterUnsupportedUiElements();

            mImpl->Context = Rml::CreateContext(kRmlContextName, Rml::Vector2i(1280, 720));
            if (!mImpl->Context)
                throw std::runtime_error("Failed to create the RmlUi runtime context");

            LoadDefaultFonts();

            mInitialized = true;
        }
        catch (...)
        {
            const std::exception_ptr initializationFailure = std::current_exception();
            try
            {
                Shutdown();
            }
            catch (const std::exception& cleanupFailure)
            {
                Log::WriteFallbackError("RmlUi initialization rollback failed.", cleanupFailure.what());
            }
            catch (...)
            {
                Log::WriteFallbackError("RmlUi initialization rollback failed.", "unknown exception");
            }
            std::rethrow_exception(initializationFailure);
        }
    }

    void RmlUiManager::LoadDefaultFonts()
    {
        EnsureFontLoaded("Assets/Fonts/opensans/OpenSans-Regular.ttf");
        EnsureFontLoaded("Assets/Fonts/opensans/OpenSans-Bold.ttf");
        EnsureFontLoaded("Assets/Fonts/Noto_Sans_SC/NotoSansSC-Regular.otf", true);
        EnsureFontLoaded("Assets/Fonts/Noto_Sans_SC/NotoSansSC-Bold.otf", true);
    }

    bool RmlUiManager::EnsureFontLoaded(const std::string& relativeFontPath, bool bFallbackFace)
    {
        if (relativeFontPath.empty())
            return false;

        const std::string normalized = NormalizeProjectPath(relativeFontPath);
        const std::string fontKey = normalized + (bFallbackFace ? "#fallback" : "#normal");
        if (mLoadedFontPaths.contains(fontKey))
            return true;

        const std::filesystem::path fullPath = ConfigManager::GetFullPath(normalized);
        if (!std::filesystem::exists(fullPath))
            return false;

        const std::string utf8Path = Utils::PathToUtf8(fullPath);
        const bool bLoaded = Rml::LoadFontFace(utf8Path, bFallbackFace);
        HE_CORE_INFO("RmlUi font load '{0}' fallback={1}: {2}", utf8Path, bFallbackFace ? "true" : "false", bLoaded ? "ok" : "failed");
        if (bLoaded)
            mLoadedFontPaths.insert(fontKey);
        return bLoaded;
    }

    void RmlUiManager::LoadFontsForUiDefinition(const UIRes& uiRes)
    {
        for (const UINodeRes& node : uiRes.Nodes)
        {
            if (node.Style.FontPath.empty())
                continue;

            const bool bFallbackFace = IsFallbackFontPath(node.Style.FontPath);
            EnsureFontLoaded(node.Style.FontPath, bFallbackFace);
        }
    }

    void RmlUiManager::Shutdown()
    {
		std::exception_ptr shutdownFailure;
		if (mDispatchDepth != 0 || mDrainingDeferredMutations)
			throw std::logic_error("RmlUi cannot shut down during document event dispatch.");
		if (mGpuFrameActive)
			EndGpuFrame();
		if (!mInitialized && !mRmlInitialized && !mImpl && mDocuments.empty() &&
			mDeferredMutations.empty())
            return;
		mDeferredMutations.clear();

        if (mImpl && mImpl->Context)
        {
            // Documents retain raw pointers to their root event listener. Use
            // the normal unload path so those listeners are detached before
            // their owning DocumentState objects are destroyed.
			std::vector<std::string> documentIds;
			documentIds.reserve(mDocuments.size());
			for (const auto& [documentId, state] : mDocuments)
			{
				(void)state;
				documentIds.push_back(documentId);
			}

			mDrainingDeferredMutations = true;
			for (const std::string& documentId : documentIds)
			{
				try
				{
					(void)UnloadDocumentNow(documentId);
				}
				catch (...)
				{
					const std::exception_ptr failure = std::current_exception();
					if (!shutdownFailure)
						shutdownFailure = failure;
					else
						ReportRmlCleanupFailure("RmlUi document shutdown failed.", failure);
				}
			}

			if (!mDocuments.empty())
			{
				mDrainingDeferredMutations = false;
				mDeferredMutations.clear();
				if (!shutdownFailure)
				{
					shutdownFailure = std::make_exception_ptr(std::runtime_error(
						"RmlUi shutdown left live documents without reporting a failure."));
				}
			}
			mDrainingDeferredMutations = false;
			mDeferredMutations.clear();
        }
        else
        {
            mDocuments.clear();
        }

        mDocumentOrder.clear();
        mOwnerToDocuments.clear();
        mActiveDocumentId.clear();
        mOverlayDocumentId.clear();

        if (mImpl && mImpl->Context)
        {
            Rml::RemoveContext(kRmlContextName);
            mImpl->Context = nullptr;
        }

        if (mRmlInitialized)
            Rml::Shutdown();
        Rml::SetRenderInterface(nullptr);
        Rml::SetSystemInterface(nullptr);
        Rml::SetFileInterface(nullptr);
		if (mImpl && mImpl->Renderer)
		{
			try
			{
				mImpl->Renderer->Shutdown();
			}
			catch (...)
			{
				if (!shutdownFailure)
					shutdownFailure = std::current_exception();
				else
					ReportRmlCleanupFailure(
						"RmlUi renderer shutdown failed.",
						std::current_exception());
			}
		}
        mImpl.reset();
        mLoadedFontPaths.clear();
		mDeferredMutations.clear();
		mDispatchDepth = 0;
		mDrainingDeferredMutations = false;
		mRmlInitialized = false;
        mInitialized = false;
		mGpuFrameActive = false;
        mCustomCursorApplied = false;
        mCursorWindow = nullptr;
		mRuntimeUiTransition = {};
		if (shutdownFailure)
			std::rethrow_exception(shutdownFailure);
    }

    void RmlUiManager::SetViewportRect(float x, float y, float w, float h, float /*uiScale*/)
    {
        mViewportX = x;
        mViewportY = y;
        mViewportW = w;
        mViewportH = h;
        mUIScale = ComputeRmlDensityScale(w, h, mUserScale);
    }

    void RmlUiManager::BeginFrame(
        float viewportWidth,
        float viewportHeight,
        float /*uiScale*/,
        float renderWidth,
        float renderHeight)
    {
		HE_PROFILE;
        mFrameRenderingSuppressed = false;

        if (!mInitialized || !mImpl || !mImpl->Context)
            return;

        mViewportW = viewportWidth;
        mViewportH = viewportHeight;
        const float targetRenderWidth = renderWidth > 0.0f ? renderWidth : viewportWidth;
        const float targetRenderHeight = renderHeight > 0.0f ? renderHeight : viewportHeight;
        mUIScale = ComputeRmlDensityScale(viewportWidth, viewportHeight, mUserScale);

        mImpl->Context->SetDimensions(Rml::Vector2i(
            static_cast<int>(std::round(std::max(viewportWidth, 1.0f))),
            static_cast<int>(std::round(std::max(viewportHeight, 1.0f)))));
        mImpl->Context->SetDensityIndependentPixelRatio(mUIScale);
        mImpl->Renderer->SetViewportSize(
            targetRenderWidth,
            targetRenderHeight,
            viewportWidth,
            viewportHeight);
    }

    void RmlUiManager::Update()
    {
		HE_PROFILE;

        if (!mInitialized || !mImpl || !mImpl->Context)
            return;

#if HE_ENABLE_GM
		GMPanelService::GetInstance().Update();
#endif

		BeginDocumentDispatch();
		std::exception_ptr dispatchFailure;
		try
		{
			RefreshMouseCursor();
			mImpl->Context->Update();
		}
		catch (...)
		{
			dispatchFailure = std::current_exception();
		}

		if (dispatchFailure)
		{
			try
			{
				EndDocumentDispatch();
			}
			catch (...)
			{
				ReportRmlCleanupFailure(
					"Deferred RmlUi mutation cleanup failed after Update.",
					std::current_exception());
			}
			std::rethrow_exception(dispatchFailure);
		}
		EndDocumentDispatch();
    }

    void RmlUiManager::BeginGpuFrame(
        nvrhi::ICommandList* commandList,
        nvrhi::IFramebuffer* framebuffer,
        uint32_t frameSlot,
        uint64_t completedSubmission,
        nvrhi::ITexture* backdropTexture)
    {
        if (!mInitialized || !mImpl || !mImpl->Renderer)
            throw std::logic_error("RmlUi BeginGpuFrame requires InitNvrhi");
        if (mGpuFrameActive)
            throw std::logic_error("RmlUi BeginGpuFrame called before EndGpuFrame");

        mImpl->Renderer->BeginFrame(
            commandList,
            framebuffer,
            frameSlot,
            completedSubmission,
            backdropTexture);
        mGpuFrameActive = true;
    }

    void RmlUiManager::Render()
    {
        if (mFrameRenderingSuppressed) return;
		HE_PROFILE_SECTION("RmlUi::Render");

        if (!mInitialized || !mImpl || !mImpl->Context)
            return;
		if (!mGpuFrameActive)
			throw std::logic_error("RmlUi Render requires an active GPU frame");

		BeginDocumentDispatch();
		try
		{
			if (!mImpl->Context->Render())
				throw std::runtime_error("RmlUi context render failed");
			EndDocumentDispatch();
		}
		catch (...)
		{
			const std::exception_ptr dispatchFailure = std::current_exception();
			if (mDispatchDepth != 0)
			{
				try
				{
					EndDocumentDispatch();
				}
				catch (...)
				{
					ReportRmlCleanupFailure(
						"Deferred RmlUi mutation cleanup failed after Render.",
						std::current_exception());
				}
			}
			if (mGpuFrameActive)
			{
				try
				{
					EndGpuFrame();
				}
				catch (...)
				{
					ReportRmlCleanupFailure(
						"RmlUi GPU frame cleanup failed after Render.",
						std::current_exception());
				}
			}
			std::rethrow_exception(dispatchFailure);
		}
    }

    void RmlUiManager::EndGpuFrame()
    {
        if (!mGpuFrameActive || !mImpl || !mImpl->Renderer)
            throw std::logic_error("RmlUi EndGpuFrame called without BeginGpuFrame");

        mGpuFrameActive = false;
        mImpl->Renderer->EndFrame();
    }

    void RmlUiManager::NotifyGpuFrameSubmitted(
        uint32_t frameSlot,
        uint64_t submission)
    {
        if (!mInitialized || !mImpl || !mImpl->Renderer)
            return;
        mImpl->Renderer->NotifyFrameSubmitted(frameSlot, submission);
    }

    void RmlUiManager::AbortGpuFrame(uint32_t frameSlot) noexcept
    {
        if (!mInitialized || !mImpl || !mImpl->Renderer)
            return;
        mImpl->Renderer->AbortFrame(frameSlot);
    }

    const NvrhiRmlRenderBackendStats& RmlUiManager::GetRenderStats() const
    {
        if (!mInitialized || !mImpl || !mImpl->Renderer)
            throw std::logic_error("RmlUi render statistics require InitNvrhi");
        return mImpl->Renderer->GetStats();
    }

    RmlRenderCapabilities RmlUiManager::GetRenderCapabilities() const noexcept
    {
        if (!mInitialized || !mImpl || !mImpl->Renderer)
            return {};
        return mImpl->Renderer->GetCapabilities();
    }

    void RmlUiManager::SetRuntimeUiTransition(
        RuntimeUiTransitionParameters transition) noexcept
    {
        mRuntimeUiTransition = transition;
    }

    void RmlUiManager::ClearRuntimeUiTransition() noexcept
    {
        mRuntimeUiTransition = {};
    }

    RuntimeUiTransitionParameters RmlUiManager::GetRuntimeUiTransition() const noexcept
    {
        // A system overlay must not inherit a gameplay menu's partial wipe or
        // fade. Preserve the requested transition for when the overlay closes.
        return mOverlayDocumentId.empty() ? mRuntimeUiTransition : RuntimeUiTransitionParameters{};
    }

    bool RmlUiManager::OnEvent(Event& e)
    {
		HE_PROFILE;

        if (!mInitialized || !mImpl || !mImpl->Context)
            return false;

		BeginDocumentDispatch();
		std::exception_ptr dispatchFailure;
		try
		{
			const int modifiers = GetRmlKeyModifiers();
			EventDispatcher dispatcher(e);

        static glm::vec2 lastMouseMoveEvent{-1.0f, -1.0f};

        const auto processMousePosition = [&](float mouseX, float mouseY, bool& bWithinViewport)
        {
            const float localX = mouseX - mViewportX;
            const float localY = mouseY - mViewportY;
            bWithinViewport = localX >= 0.0f && localY >= 0.0f && localX <= mViewportW && localY <= mViewportH;
            if (!bWithinViewport)
            {
                mImpl->Context->ProcessMouseLeave();
                return false;
            }

            return mImpl->Context->ProcessMouseMove(
                static_cast<int>(std::round(localX)),
                static_cast<int>(std::round(localY)),
                modifiers);
        };

        const auto traceMouse = [&](const char* phase, const glm::vec2& raw, bool within)
        {
            if (!TraceUiInput()) return;
            const auto dimensions = mImpl->Context->GetDimensions();
            auto* hovered = mImpl->Context->GetHoverElement();
            HE_CORE_INFO("[UIInput] Rml {} raw={},{} rect={},{},{},{} context={}x{} density={} within={} hoverTag={} hoverId={} overlay={}",
                phase, raw.x, raw.y, mViewportX, mViewportY, mViewportW, mViewportH,
                dimensions.x, dimensions.y, mUIScale, within,
                hovered ? hovered->GetTagName().c_str() : "<none>",
                hovered ? hovered->GetId().c_str() : "<none>", mOverlayDocumentId);
            HE_CORE_INFO("[UIInput] last MouseMoved event={},{}", lastMouseMoveEvent.x, lastMouseMoveEvent.y);
            if (const char* elementId = std::getenv("HE_TRACE_UI_ELEMENT"); elementId && *elementId)
            {
                for (const auto& [id, state] : mDocuments)
                {
                    if (!state || !state->Document) continue;
                    if (auto* element = state->Document->GetElementById(elementId))
                    {
                        const auto offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
                        const auto size = element->GetBox().GetSize(Rml::BoxArea::Border);
                        HE_CORE_INFO("[UIInput] element={} document={} shown={} visible={} tag={} bounds={},{},{},{}",
                            elementId, id, state->bVisible, element->IsVisible(true),
                            element->GetTagName().c_str(), offset.x, offset.y, size.x, size.y);
                    }
                }
            }
        };

        dispatcher.Dispatch<MouseMovedEvent>([&](MouseMovedEvent& mouseEvent)
        {
            lastMouseMoveEvent = {mouseEvent.GetX(), mouseEvent.GetY()};
            bool bWithinViewport = false;
            const bool bMouseMoveUnconsumed = processMousePosition(mouseEvent.GetX(), mouseEvent.GetY(), bWithinViewport);
            if (!bWithinViewport)
                return false;

            return !bMouseMoveUnconsumed;
        });

        dispatcher.Dispatch<MouseButtonPressedEvent>([&](MouseButtonPressedEvent& mouseEvent)
        {
            const glm::vec2 mousePosition = Input::GetMousePosition();
            bool bWithinViewport = false;
            processMousePosition(mousePosition.x, mousePosition.y, bWithinViewport);
            traceMouse("down", mousePosition, bWithinViewport);
            if (!bWithinViewport)
                return false;

            return !mImpl->Context->ProcessMouseButtonDown(static_cast<int>(mouseEvent.GetMouseButton()), modifiers);
        });

        dispatcher.Dispatch<MouseButtonReleasedEvent>([&](MouseButtonReleasedEvent& mouseEvent)
        {
            const glm::vec2 mousePosition = Input::GetMousePosition();
            bool bWithinViewport = false;
            processMousePosition(mousePosition.x, mousePosition.y, bWithinViewport);
            traceMouse("up", mousePosition, bWithinViewport);
            if (!bWithinViewport)
                return false;

            return !mImpl->Context->ProcessMouseButtonUp(static_cast<int>(mouseEvent.GetMouseButton()), modifiers);
        });

        dispatcher.Dispatch<MouseScrolledEvent>([&](MouseScrolledEvent& mouseEvent)
        {
            return !mImpl->Context->ProcessMouseWheel({ mouseEvent.GetXOffset(), -mouseEvent.GetYOffset() }, modifiers);
        });

        dispatcher.Dispatch<KeyPressedEvent>([&](KeyPressedEvent& keyEvent)
        {
            const Rml::Input::KeyIdentifier keyId = ToRmlKeyIdentifier(keyEvent.GetKeyCode());
            return !mImpl->Context->ProcessKeyDown(keyId, modifiers);
        });

        dispatcher.Dispatch<KeyReleasedEvent>([&](KeyReleasedEvent& keyEvent)
        {
            const Rml::Input::KeyIdentifier keyId = ToRmlKeyIdentifier(keyEvent.GetKeyCode());
            return !mImpl->Context->ProcessKeyUp(keyId, modifiers);
        });

			dispatcher.Dispatch<KeyTypedEvent>([&](KeyTypedEvent& keyEvent)
			{
				return !mImpl->Context->ProcessTextInput(static_cast<Rml::Character>(static_cast<char32_t>(static_cast<uint16_t>(keyEvent.GetKeyCode()))));
			});
		}
		catch (...)
		{
			dispatchFailure = std::current_exception();
		}

		if (dispatchFailure)
		{
			try
			{
				EndDocumentDispatch();
			}
			catch (...)
			{
				ReportRmlCleanupFailure(
					"Deferred RmlUi mutation cleanup failed after input dispatch.",
					std::current_exception());
			}
			std::rethrow_exception(dispatchFailure);
		}
		EndDocumentDispatch();
		return e.Handled;
    }

    std::string RmlUiManager::BuildDocumentId(const std::string& canonicalCachePath, const UUID& ownerScript) const
    {
        return canonicalCachePath + "#" + ownerScript.str();
    }

    RmlUiManager::DocumentState* RmlUiManager::FindDocument(const std::string& documentId)
    {
        auto it = mDocuments.find(documentId);
        return it != mDocuments.end() ? it->second.get() : nullptr;
    }

    const RmlUiManager::DocumentState* RmlUiManager::FindDocument(const std::string& documentId) const
    {
        auto it = mDocuments.find(documentId);
        return it != mDocuments.end() ? it->second.get() : nullptr;
    }

    Ref<RmlUiManager::DocumentState> RmlUiManager::CreateDocumentState(
        const std::string& uiAssetPath,
        const UUID& ownerScript,
        std::string& outDocumentId,
        bool bAllowExistingDocument)
    {
        outDocumentId.clear();

        const std::filesystem::path& projectRoot = ConfigManager::GetInstance().GetRootFolder();
        std::filesystem::path contentRoot = projectRoot;
        std::filesystem::path resolvedInputPath;
        std::string normalizedInputPath;
        Utils::PathValidationResult pathResult = Utils::TryResolveProjectRelativePath(
            projectRoot,
            uiAssetPath,
            resolvedInputPath,
            normalizedInputPath);
        if (!pathResult)
        {
            HE_CORE_ERROR("RmlUi rejected document path '{0}': {1}", uiAssetPath, pathResult.Message);
            return {};
        }

        std::error_code inputError;
        if (!std::filesystem::is_regular_file(resolvedInputPath, inputError) || inputError)
        {
            const std::filesystem::path& engineContentRoot =
                ConfigManager::GetInstance().GetEngineContentFolder();
            std::filesystem::path engineInputPath;
            std::string engineInputProjectPath;
            pathResult = Utils::TryResolveProjectRelativePath(
                engineContentRoot,
                uiAssetPath,
                engineInputPath,
                engineInputProjectPath);
            inputError.clear();
            if (pathResult &&
                std::filesystem::is_regular_file(engineInputPath, inputError) && !inputError)
            {
                contentRoot = engineContentRoot;
                resolvedInputPath = std::move(engineInputPath);
                normalizedInputPath = std::move(engineInputProjectPath);
            }
        }

        const std::string inputCachePath = Utils::MakeProjectPathCacheKey(normalizedInputPath);
        const std::string extension = Utils::PathToUtf8(
            Utils::Utf8ToPath(inputCachePath).extension());

        std::filesystem::path fullRmlPath;
        std::string canonicalUiPath;
        std::string canonicalRmlPath;
        std::string canonicalCachePath;
        RuntimeResourceHandle<RuntimeUiDefinition> definitionHandle;

        try
        {
            if (extension == ".hui")
            {
                RuntimeResourceRegistry& registry = RuntimeResourceRegistry::GetInstance();
                if (!registry.IsInitialized())
                {
                    HE_CORE_ERROR("RmlUi cannot load .hui before the runtime resource registry is initialized: {0}", normalizedInputPath);
                    return {};
                }

                const RuntimeUiPath runtimePath = ResolveRuntimeUiPath(contentRoot, normalizedInputPath);
                canonicalUiPath = runtimePath.HuiProjectPath;
                canonicalRmlPath = runtimePath.RmlProjectPath;
                canonicalCachePath = runtimePath.Key.CachePath;
                fullRmlPath = runtimePath.RmlFullPath;
                outDocumentId = BuildDocumentId(canonicalCachePath, ownerScript);
                if (!bAllowExistingDocument && FindDocument(outDocumentId))
                    return {};

                definitionHandle = AcquireRuntimeUiDefinition(registry, runtimePath);
                if (!definitionHandle)
                {
                    HE_CORE_ERROR("RmlUi failed to acquire UI definition: {0}", canonicalUiPath);
                    return {};
                }
            }
            else if (extension == ".rml")
            {
                canonicalUiPath = normalizedInputPath;
                canonicalRmlPath = normalizedInputPath;
                canonicalCachePath = inputCachePath;
                fullRmlPath = resolvedInputPath;
                outDocumentId = BuildDocumentId(canonicalCachePath, ownerScript);
                if (!bAllowExistingDocument && FindDocument(outDocumentId))
                    return {};
            }
            else
            {
                HE_CORE_ERROR("RmlUi document path must use the .hui or .rml extension: {0}", normalizedInputPath);
                return {};
            }
        }
        catch (const std::exception& exception)
        {
            HE_CORE_ERROR("RmlUi failed to resolve document '{0}': {1}", normalizedInputPath, exception.what());
            return {};
        }

        // RmlUi caches RCSS globally; clear it so PIE picks up newly exported HUD styles.
        Rml::Factory::ClearStyleSheetCache();
        if (definitionHandle)
            LoadFontsForUiDefinition(definitionHandle->GetDefinition());

        std::error_code fileError;
        if (!std::filesystem::is_regular_file(fullRmlPath, fileError) || fileError)
        {
            HE_CORE_ERROR("RmlUi document not found: {0}", Utils::PathToUtf8(fullRmlPath));
            return {};
        }

        const std::string utf8RmlPath = Utils::PathToUtf8(fullRmlPath);
        HE_CORE_INFO("RmlUi loading document: {0}", utf8RmlPath);

        Rml::ElementDocument* document = mImpl->Context->LoadDocument(utf8RmlPath);
        if (!document)
        {
            HE_CORE_ERROR("Failed to load RmlUi document: {0}", utf8RmlPath);
            return {};
        }

		Ref<DocumentState> state;
		try
		{
			document->Hide();

			state = CreateRef<DocumentState>();
        state->DocumentId = outDocumentId;
        state->UiAssetPath = canonicalUiPath;
        state->RmlPath = canonicalRmlPath;
        state->OwnerScriptId = ownerScript.str();
        state->Document = document;
        state->EventListener = CreateScope<RmlEventListener>([this, ownerId = ownerScript.str(), docId = outDocumentId](const RmlEventListener::EventData& event)
        {
            auto it = mDocuments.find(docId);
            if (it == mDocuments.end())
                return;

            if (event.LegacySourceId.empty() && event.TargetElementId.empty())
                return;

            if (TraceUiInput() && event.Type == "click")
                HE_CORE_INFO("[UIInput] dispatch document={} nativeHandler={} owner={} source={} target={}",
                    docId, it->second && static_cast<bool>(it->second->NativeEventHandler),
                    ownerId, event.LegacySourceId, event.TargetElementId);
            if (it->second && it->second->NativeEventHandler)
            {
                it->second->NativeEventHandler(RmlUiNativeEvent{
                    .Type = event.Type,
                    .SourceElementId = event.LegacySourceId,
                    .TargetElementId = event.TargetElementId
                });
            }

            if (ownerId.empty())
                return;
            const UUID scriptUuid(ownerId);
            if (!ScriptEngine::IsClassRegistered(scriptUuid))
                return;
            ScopedCurrentScriptOwner currentOwner(scriptUuid);
                RmlUiEventPayloadData payloadData;
                payloadData.DocumentId = docId;
                payloadData.EventType = event.Type;
                payloadData.TargetElementId = event.TargetElementId;
                payloadData.CurrentElementId = event.CurrentElementId;
                payloadData.Property = event.Property;
                if (!event.AnimationName.empty())
                    payloadData.AnimationName = event.AnimationName;
                if (event.bHasElapsedTime)
                    payloadData.ElapsedTime = event.ElapsedTime;
                if (event.bHasMouseX)
                    payloadData.MouseX = event.MouseX;
                if (event.bHasMouseY)
                    payloadData.MouseY = event.MouseY;
                if (event.bHasWheelDelta)
                    payloadData.WheelDelta = event.WheelDelta;
                if (event.bHasMouseButton)
                    payloadData.MouseButton = event.MouseButton;
            try { ScriptEngine::DispatchUI(scriptUuid, event.LegacySourceId, payloadData); }
            catch (const std::exception& error)
            {
                HE_CORE_ERROR("[RmlUi] OnUIEventEx failed: {0}", error.what());
            }
        });

			static const std::array<const char*, 11> events = { "mousemove", "mousedown", "mouseup", "mousescroll", "click", "change", "submit", "focus", "blur", "animationend", "transitionend" };
			state->bRegisteredRootEvents = true;
			for (const char* eventName : events)
				document->AddEventListener(eventName, state->EventListener.get(), false);
			state->UiDefinitionHandle = std::move(definitionHandle);

			return state;
		}
		catch (...)
		{
			const std::exception_ptr creationFailure = std::current_exception();
			try
			{
				if (state)
					DestroyDocumentDom(*state);
				else if (document && mImpl && mImpl->Context)
					mImpl->Context->UnloadDocument(document);
			}
			catch (...)
			{
				ReportRmlCleanupFailure(
					"RmlUi unpublished document rollback failed.",
					std::current_exception());
			}
			std::rethrow_exception(creationFailure);
		}
    }

    bool RmlUiManager::LoadDocument(const std::string& uiAssetPath, const UUID& ownerScript, std::string* outDocumentId)
    {
        HE_PROFILE_SECTION("RmlUiManager::LoadDocument");
        if (!mInitialized || !mImpl || !mImpl->Context)
            return false;

        std::string documentId;
        Ref<DocumentState> state = CreateDocumentState(uiAssetPath, ownerScript, documentId, false);
        if (!state)
        {
            if (!documentId.empty() && FindDocument(documentId))
            {
                if (outDocumentId)
                    *outDocumentId = documentId;
                return true;
            }
            return false;
        }

        mOwnerToDocuments[ownerScript].push_back(documentId);
        mDocumentOrder.push_back(documentId);
        mDocuments.emplace(documentId, std::move(state));
        if (outDocumentId)
            *outDocumentId = documentId;
        HE_CORE_INFO("RmlUi loaded document id: {0}", documentId);
        return true;
    }

    bool RmlUiManager::ShowDocument(const std::string& documentId, bool bFocus)
    {
        if (DocumentState* state = FindDocument(documentId))
        {
			BeginDocumentDispatch();
			std::exception_ptr dispatchFailure;
			try
			{
				state->bVisible = true;
				state->Document->Show(Rml::ModalFlag::None, bFocus ? Rml::FocusFlag::Auto : Rml::FocusFlag::None);
			}
			catch (...)
			{
				dispatchFailure = std::current_exception();
			}
			if (dispatchFailure)
			{
				try
				{
					EndDocumentDispatch();
				}
				catch (...)
				{
					ReportRmlCleanupFailure(
						"Deferred RmlUi mutation cleanup failed after document Show.",
						std::current_exception());
				}
				std::rethrow_exception(dispatchFailure);
			}
			EndDocumentDispatch();
			RefreshMouseCursor();
            HE_CORE_INFO("RmlUi showing document: {0}", documentId);
            return true;
        }
        return false;
    }

    bool RmlUiManager::ShowOverlayDocument(const std::string& documentId)
    {
        if (!FindDocument(documentId))
            return false;
        if (mOverlayDocumentId == documentId)
            return true;
        if (!mOverlayDocumentId.empty())
            HideDocument(mOverlayDocumentId);
        if (!ShowDocument(documentId, false))
            return false;
        auto* state = FindDocument(documentId);
        if (!state)
            return false;
        mOverlayDocumentId = documentId;
        state->Document->SetProperty("z-index", "2147483646");
        HE_CORE_INFO("RmlUi overlay shown: {0}; active gameplay document preserved: {1}", documentId, mActiveDocumentId);
        return true;
    }

    bool RmlUiManager::HideDocument(const std::string& documentId)
    {
        if (DocumentState* state = FindDocument(documentId))
        {
			BeginDocumentDispatch();
			std::exception_ptr dispatchFailure;
			try
			{
				state->bVisible = false;
				state->Document->Hide();
				if (mActiveDocumentId == documentId)
					mActiveDocumentId.clear();
                if (mOverlayDocumentId == documentId)
                {
                    HE_CORE_INFO("RmlUi overlay hidden: {0}; active gameplay document preserved: {1}", documentId, mActiveDocumentId);
                    mOverlayDocumentId.clear();
                }
			}
			catch (...)
			{
				dispatchFailure = std::current_exception();
			}
			if (dispatchFailure)
			{
				try
				{
					EndDocumentDispatch();
				}
				catch (...)
				{
					ReportRmlCleanupFailure(
						"Deferred RmlUi mutation cleanup failed after document Hide.",
						std::current_exception());
				}
				std::rethrow_exception(dispatchFailure);
			}
			EndDocumentDispatch();
			RefreshMouseCursor();
            return true;
        }
        return false;
    }

    void RmlUiManager::DestroyDocumentDom(DocumentState& state)
    {
		bool bListenerDetachFailed = false;
        if (state.Document && state.EventListener && state.bRegisteredRootEvents)
        {
			static const std::array<const char*, 11> events = { "mousemove", "mousedown", "mouseup", "mousescroll", "click", "change", "submit", "focus", "blur", "animationend", "transitionend" };
            for (const char* eventName : events)
			{
				try
				{
					state.Document->RemoveEventListener(eventName, state.EventListener.get(), false);
				}
				catch (...)
				{
					bListenerDetachFailed = true;
					ReportRmlCleanupFailure(
						"RmlUi document listener detach failed.",
						std::current_exception());
				}
			}
        }
		if (!bListenerDetachFailed)
			state.bRegisteredRootEvents = false;

        if (state.Document && mImpl && mImpl->Context)
		{
			// Keep both the DOM and its listener alive if Context rejects the unload;
			// the caller can retry without leaving a dangling listener pointer.
			mImpl->Context->UnloadDocument(state.Document);
		}
        state.Document = nullptr;
		state.bRegisteredRootEvents = false;
		state.EventListener.reset();
    }

    bool RmlUiManager::ReloadDocument(const std::string& documentId)
	{
		if (!FindDocument(documentId))
			return false;
		if (ShouldDeferDocumentMutation())
		{
			QueueDeferredMutation(DeferredMutationType::Reload, documentId);
			return true;
		}
		return ReloadDocumentNow(documentId);
	}

	bool RmlUiManager::ReloadDocumentNow(const std::string& documentId)
    {
		const auto initialDocumentIt = mDocuments.find(documentId);
		if (initialDocumentIt == mDocuments.end() || !initialDocumentIt->second)
            return false;

		Ref<DocumentState> oldState = initialDocumentIt->second;
        const std::string uiAssetPath = oldState->UiAssetPath;
        const UUID owner(oldState->OwnerScriptId);
        const bool bWasVisible = oldState->bVisible;
        const bool bWasActive = mActiveDocumentId == documentId;
        const bool bWantedMouseCursor = oldState->bMouseCursorVisible;
        const std::string cursorAssetPath = oldState->MouseCursorAssetPath;
        const uint32_t cursorHotspotX = oldState->MouseCursorHotspotX;
        const uint32_t cursorHotspotY = oldState->MouseCursorHotspotY;
        auto nativeEventHandler = oldState->NativeEventHandler;

        if (oldState->UiDefinitionHandle)
        {
            RuntimeResourceRegistry& registry = RuntimeResourceRegistry::GetInstance();
            if (!registry.IsInitialized())
            {
                HE_CORE_ERROR("RmlUi cannot reload .hui after the runtime resource registry has shut down: {0}", uiAssetPath);
                return false;
            }
            (void)registry.Invalidate(oldState->UiDefinitionHandle.GetKey());
        }

        std::string newDocumentId;
        Ref<DocumentState> replacement = CreateDocumentState(uiAssetPath, owner, newDocumentId, true);
        if (!replacement)
            return false;
        if (newDocumentId != documentId)
        {
            HE_CORE_ERROR("RmlUi reload changed the canonical document id from '{0}' to '{1}'", documentId, newDocumentId);
            DestroyDocumentDom(*replacement);
            return false;
        }

        replacement->bVisible = bWasVisible || bWasActive;
        replacement->bMouseCursorVisible = bWantedMouseCursor;
        replacement->MouseCursorAssetPath = cursorAssetPath;
        replacement->MouseCursorHotspotX = cursorHotspotX;
        replacement->MouseCursorHotspotY = cursorHotspotY;
        replacement->NativeEventHandler = std::move(nativeEventHandler);

		// Loading the replacement may execute Rml callbacks that publish another
		// document and rehash mDocuments. Reacquire the entry before committing.
		auto documentIt = mDocuments.find(documentId);
		if (documentIt == mDocuments.end() || documentIt->second != oldState)
		{
			DestroyDocumentDom(*replacement);
			return false;
		}

        // Keep the old DOM and resource generation alive until the replacement
        // definition and DOM have both loaded successfully.
		try
		{
			DestroyDocumentDom(*oldState);
		}
		catch (...)
		{
			const std::exception_ptr oldDomFailure = std::current_exception();
			try
			{
				DestroyDocumentDom(*replacement);
			}
			catch (...)
			{
				ReportRmlCleanupFailure(
					"RmlUi replacement rollback failed after old DOM unload failure.",
					std::current_exception());
			}
			std::rethrow_exception(oldDomFailure);
		}
		documentIt->second = replacement;
		oldState.reset();

		BeginDocumentDispatch();
		std::exception_ptr dispatchFailure;
		try
		{
			DocumentState* reloadedState = replacement.get();
			if (bWasActive)
				reloadedState->Document->Show(Rml::ModalFlag::None, Rml::FocusFlag::Auto);
			else if (bWasVisible)
				reloadedState->Document->Show();
		}
		catch (...)
		{
			dispatchFailure = std::current_exception();
		}
		if (dispatchFailure)
		{
			try
			{
				EndDocumentDispatch();
			}
			catch (...)
			{
				ReportRmlCleanupFailure(
					"Deferred RmlUi mutation cleanup failed after document reload visibility restore.",
					std::current_exception());
			}
			std::rethrow_exception(dispatchFailure);
		}
		EndDocumentDispatch();

		try
		{
			RefreshMouseCursor();
		}
		catch (...)
		{
			ReportRmlCleanupFailure(
				"RmlUi cursor refresh failed after document reload.",
				std::current_exception());
		}
        HE_CORE_INFO("RmlUi reloaded document id: {0}", documentId);
        return true;
    }

    bool RmlUiManager::UnloadDocument(const std::string& documentId)
	{
		if (!FindDocument(documentId))
			return false;
		if (ShouldDeferDocumentMutation())
		{
			QueueDeferredMutation(DeferredMutationType::Unload, documentId);
			return true;
		}
		return UnloadDocumentNow(documentId);
	}

	bool RmlUiManager::UnloadDocumentNow(const std::string& documentId)
    {
        auto it = mDocuments.find(documentId);
        if (it == mDocuments.end())
            return false;

        Ref<DocumentState> state = it->second;
        DestroyDocumentDom(*state);

        mDocumentOrder.erase(std::remove(mDocumentOrder.begin(), mDocumentOrder.end(), documentId), mDocumentOrder.end());
        if (mActiveDocumentId == documentId)
            mActiveDocumentId.clear();
        if (mOverlayDocumentId == documentId)
            mOverlayDocumentId.clear();

        for (auto ownerIt = mOwnerToDocuments.begin(); ownerIt != mOwnerToDocuments.end();)
        {
            auto& docs = ownerIt->second;
            docs.erase(std::remove(docs.begin(), docs.end(), documentId), docs.end());
            if (docs.empty())
                ownerIt = mOwnerToDocuments.erase(ownerIt);
            else
                ++ownerIt;
        }

        mDocuments.erase(it);
        state.reset();
		try
		{
			RefreshMouseCursor();
		}
		catch (...)
		{
			ReportRmlCleanupFailure(
				"RmlUi cursor refresh failed after document unload.",
				std::current_exception());
		}
        return true;
    }

    bool RmlUiManager::SetActiveDocument(const std::string& documentId)
    {
        if (!FindDocument(documentId))
            return false;

		const std::vector<std::string> documentOrder = mDocumentOrder;
		BeginDocumentDispatch();
		std::exception_ptr dispatchFailure;
		try
		{
			for (const auto& id : documentOrder)
			{
				if (DocumentState* state = FindDocument(id))
				{
					if (id == documentId)
					{
						state->bVisible = true;
						state->Document->Show(Rml::ModalFlag::None, Rml::FocusFlag::Auto);
						HE_CORE_INFO("RmlUi active document: {0}", documentId);
					}
					else if (id != mOverlayDocumentId)
					{
						state->bVisible = false;
						state->Document->Hide();
					}
				}
			}
			mActiveDocumentId = documentId;
		}
		catch (...)
		{
			dispatchFailure = std::current_exception();
		}
		if (dispatchFailure)
		{
			try
			{
				EndDocumentDispatch();
			}
			catch (...)
			{
				ReportRmlCleanupFailure(
					"Deferred RmlUi mutation cleanup failed after active document change.",
					std::current_exception());
			}
			std::rethrow_exception(dispatchFailure);
		}
		EndDocumentDispatch();
		RefreshMouseCursor();
        return true;
    }

    void RmlUiManager::ClearOwnerDocuments(const UUID& ownerScript)
	{
		if (mOwnerToDocuments.find(ownerScript) == mOwnerToDocuments.end())
			return;
		if (ShouldDeferDocumentMutation())
		{
			QueueDeferredMutation(DeferredMutationType::ClearOwner, ownerScript.str());
			return;
		}
		ClearOwnerDocumentsNow(ownerScript);
	}

	void RmlUiManager::ClearOwnerDocumentsNow(const UUID& ownerScript)
    {
        auto ownerIt = mOwnerToDocuments.find(ownerScript);
        if (ownerIt == mOwnerToDocuments.end())
            return;

        const auto docs = ownerIt->second;
		std::exception_ptr firstFailure;
        for (const std::string& documentId : docs)
		{
			try
			{
				(void)UnloadDocumentNow(documentId);
			}
			catch (...)
			{
				const std::exception_ptr failure = std::current_exception();
				if (!firstFailure)
					firstFailure = failure;
				else
					ReportRmlCleanupFailure("RmlUi owner document cleanup failed.", failure);
			}
		}

		ownerIt = mOwnerToDocuments.find(ownerScript);
		if (ownerIt != mOwnerToDocuments.end())
		{
			auto& remainingDocuments = ownerIt->second;
			remainingDocuments.erase(
				std::remove_if(
					remainingDocuments.begin(),
					remainingDocuments.end(),
					[this](const std::string& documentId)
					{
						return FindDocument(documentId) == nullptr;
					}),
				remainingDocuments.end());
			if (remainingDocuments.empty())
				mOwnerToDocuments.erase(ownerIt);
		}

        if (mImpl && mImpl->Context)
		{
			try
			{
				Update();
			}
			catch (...)
			{
				const std::exception_ptr failure = std::current_exception();
				if (!firstFailure)
					firstFailure = failure;
				else
					ReportRmlCleanupFailure("RmlUi owner post-cleanup update failed.", failure);
			}
		}

		if (firstFailure)
			std::rethrow_exception(firstFailure);
    }

    bool RmlUiManager::SetElementText(const std::string& documentId, const std::string& elementId, const std::string& text)
    {
        DocumentState* state = FindDocument(documentId);
        if (!state || !state->Document)
            return false;

        if (Rml::Element* element = state->Document->GetElementById(elementId))
        {
            element->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
            return true;
        }
        return false;
    }

    bool RmlUiManager::SetElementMarkup(
        const std::string& documentId,
        const std::string& elementId,
        const std::string& markup)
    {
        DocumentState* state = FindDocument(documentId);
        if (!state || !state->Document)
            return false;

        if (Rml::Element* element = state->Document->GetElementById(elementId))
        {
            element->SetInnerRML(markup);
            return true;
        }
        return false;
    }

    bool RmlUiManager::SetElementAttribute(const std::string& documentId, const std::string& elementId, const std::string& name, const std::string& value)
    {
        DocumentState* state = FindDocument(documentId);
        if (!state || !state->Document)
            return false;

        if (Rml::Element* element = state->Document->GetElementById(elementId))
        {
            element->SetAttribute(name, value);
            return true;
        }
        return false;
    }

    bool RmlUiManager::GetElementValue(
        const std::string& documentId,
        const std::string& elementId,
        std::string& outValue) const
    {
        const DocumentState* state = FindDocument(documentId);
        if (!state || !state->Document)
            return false;

        Rml::Element* element = state->Document->GetElementById(elementId);
        auto* control = dynamic_cast<Rml::ElementFormControl*>(element);
        if (!control)
            return false;
        outValue = control->GetValue().c_str();
        return true;
    }

    bool RmlUiManager::SetDocumentEventHandler(
        const std::string& documentId,
        std::function<void(const RmlUiNativeEvent&)> handler)
    {
        DocumentState* state = FindDocument(documentId);
        if (!state)
            return false;
        state->NativeEventHandler = std::move(handler);
        return true;
    }

    bool RmlUiManager::SetElementClass(const std::string& documentId, const std::string& elementId, const std::string& className, bool bEnabled)
    {
        DocumentState* state = FindDocument(documentId);
        if (!state || !state->Document)
            return false;

        if (Rml::Element* element = state->Document->GetElementById(elementId))
        {
            element->SetClass(className, bEnabled);
            return true;
        }
        return false;
    }

    bool RmlUiManager::ScrollElementIntoView(const std::string& documentId, const std::string& elementId)
    {
        DocumentState* state = FindDocument(documentId);
        if (!state || !state->Document || !state->bVisible) return false;
        // Scripted menus can insert/show a list and scroll it in the same callback.
        // Resolve its current box before scrolling; the old layout may be empty.
        state->Document->UpdateDocument();
        Rml::Element* element = state->Document->GetElementById(elementId);
        if (!element || !element->IsVisible()) return false;
        element->ScrollIntoView(Rml::ScrollAlignment::Nearest);
        return true;
    }

    bool RmlUiManager::FocusElement(
        const std::string& documentId,
        const std::string& elementId)
    {
        DocumentState* state = FindDocument(documentId);
        if (!state || !state->Document || !state->bVisible)
            return false;

        Rml::Element* element = state->Document->GetElementById(elementId);
        if (!element || !element->IsVisible() || !element->Focus(true))
            return false;
        element->ScrollIntoView(Rml::ScrollAlignment::Nearest);
        return true;
    }

    bool RmlUiManager::ClickElement(
        const std::string& documentId,
        const std::string& elementId)
    {
        DocumentState* state = FindDocument(documentId);
        if (!state || !state->Document || !state->bVisible)
            return false;

        Rml::Element* element = state->Document->GetElementById(elementId);
        if (!element || !element->IsVisible())
            return false;
        element->Click();
        return true;
    }

    bool RmlUiManager::GetElementClass(
        const std::string& documentId,
        const std::string& elementId,
        const std::string& className,
        bool& outEnabled) const
    {
        const DocumentState* state = FindDocument(documentId);
        if (!state || !state->Document)
            return false;
        if (Rml::Element* element = state->Document->GetElementById(elementId))
        {
            outEnabled = element->IsClassSet(className);
            return true;
        }
        return false;
    }

    bool RmlUiManager::SetElementStyle(const std::string& documentId, const std::string& elementId, const std::string& name, const std::string& value)
    {
        DocumentState* state = FindDocument(documentId);
        if (!state || !state->Document)
            return false;

        if (Rml::Element* element = state->Document->GetElementById(elementId))
            return element->SetProperty(name, value);
        return false;
    }

    bool RmlUiManager::GetActiveElementRect(const std::string& elementId, std::array<float, 4>& rect) const
    {
        const auto* state = FindDocument(mActiveDocumentId);
        if (!state || !state->Document) return false;
        auto* element = state->Document->GetElementById(elementId);
        if (!element || !element->IsVisible()) return false;
        const auto position = element->GetAbsoluteOffset(Rml::BoxArea::Border);
        const auto size = element->GetBox().GetSize(Rml::BoxArea::Border);
        rect = {position.x, position.y, size.x, size.y};
        return size.x > 0 && size.y > 0;
    }

    bool RmlUiManager::GetElementStyle(
        const std::string& documentId,
        const std::string& elementId,
        const std::string& name,
        std::string& outValue) const
    {
        const DocumentState* state = FindDocument(documentId);
        if (!state || !state->Document)
            return false;
        if (Rml::Element* element = state->Document->GetElementById(elementId))
        {
            const Rml::Property* property = element->GetProperty(name);
            if (!property)
                return false;
            outValue = property->ToString().c_str();
            return true;
        }
        return false;
    }

    bool RmlUiManager::SetMouseCursorVisible(const std::string& documentId, bool bVisible)
    {
        if (DocumentState* state = FindDocument(documentId))
        {
            state->bMouseCursorVisible = bVisible;
            RefreshMouseCursor();
            return true;
        }
        return false;
    }

    bool RmlUiManager::SetCustomMouseCursor(const std::string& documentId, const std::string& assetPath, uint32_t hotspotX, uint32_t hotspotY)
    {
        DocumentState* state = FindDocument(documentId);
        if (!state)
            return false;

        const std::string normalizedPath = NormalizeProjectPath(assetPath);
        if (normalizedPath.empty() || !std::filesystem::exists(ConfigManager::GetFullPath(normalizedPath)))
        {
            HE_CORE_WARN("RmlUi custom mouse cursor not found: {0}", assetPath);
            return false;
        }

        state->MouseCursorAssetPath = normalizedPath;
        state->MouseCursorHotspotX = hotspotX;
        state->MouseCursorHotspotY = hotspotY;
        RefreshMouseCursor();
        return true;
    }

    void RmlUiManager::SetUserScale(float scale)
    {
        mUserScale = std::clamp(scale, 0.8f, 1.3f);
    }

    void RmlUiManager::SetMouseCursorEnabled(bool bEnabled)
    {
        if (mMouseCursorEnabled == bEnabled) return;
        mMouseCursorEnabled = bEnabled;
        RefreshMouseCursor();
    }

    void RmlUiManager::RefreshMouseCursor()
    {
        const DocumentState* cursorState = nullptr;
        for (auto it = mDocumentOrder.rbegin(); it != mDocumentOrder.rend(); ++it)
        {
            const DocumentState* state = FindDocument(*it);
            if (state && state->bVisible && state->bMouseCursorVisible)
            {
                cursorState = state;
                break;
            }
        }

        if (!mMouseCursorEnabled || !cursorState || cursorState->MouseCursorAssetPath.empty())
        {
            if (mCustomCursorApplied && mCursorWindow)
            {
                mCursorWindow->ResetMouseCursor();
                mCustomCursorApplied = false;
            }
            return;
        }

        if (!mCursorWindow)
        {
            HE_CORE_WARN("RmlUi custom cursor request ignored because no Window was injected.");
            return;
        }

        Window& window = *mCursorWindow;
        const std::filesystem::path fullPath = ConfigManager::GetFullPath(cursorState->MouseCursorAssetPath);
        if (!window.SetMouseCursorImage(
                fullPath,
                cursorState->MouseCursorHotspotX,
                cursorState->MouseCursorHotspotY))
        {
            window.ResetMouseCursor();
            mCustomCursorApplied = false;
            return;
        }
        mCustomCursorApplied = true;
    }

    bool RmlUiManager::IsMouseCursorVisibleRequested() const
    {
        for (const auto& documentEntry : mDocuments)
        {
            const Ref<DocumentState>& state = documentEntry.second;
            if (state && state->bVisible && state->bMouseCursorVisible)
                return true;
        }
        return false;
    }
}
