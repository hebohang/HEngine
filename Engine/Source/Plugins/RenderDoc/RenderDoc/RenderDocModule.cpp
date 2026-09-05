#include "renderdoc_app.h"
#include "RenderDocModule.h"
#include "Runtime/Core/Base/Base.h"
#include "Runtime/Utils/Misc/RegexUtils.h"

namespace HEngine
{
	RENDERDOC_API_1_1_2* rdoc_api = NULL;

	// refer to https://renderdoc.org/docs/in_application_api.html
	void RenderDocCaptureService::Initialize()
	{
		HE_CORE_INFO("RenderDoc Module Init!");
		mUnavailableReason = "RenderDoc is not installed or its API is unavailable.";

#ifdef HE_PLATFORM_WINDOWS
		std::string RenderDocPath;
		if (Utils::QueryRegKey(HKEY_LOCAL_MACHINE, "SOFTWARE\\Classes\\RenderDoc.RDCCapture.1\\DefaultIcon\\", "", RenderDocPath))
		{
			std::string renderDocDllPath = RenderDocPath.substr(0, RenderDocPath.find_last_of('\\'));
			renderDocDllPath += "\\renderdoc.dll";

			if (!LoadLibraryA(renderDocDllPath.c_str()))
				return;

			// At init, on windows
			if (HMODULE mod = GetModuleHandleA("renderdoc.dll"))
			{
				pRENDERDOC_GetAPI RENDERDOC_GetAPI =
					(pRENDERDOC_GetAPI)GetProcAddress(mod, "RENDERDOC_GetAPI");
				if (!RENDERDOC_GetAPI)
					return;
				int ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_1_2, (void**)&rdoc_api);
				if (ret != 1 || !rdoc_api)
				{
					rdoc_api = nullptr;
					return;
				}

				// 消除默认屏幕左上角的ui
				rdoc_api->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);

				// 关闭f12的功能，避免冲突
				RENDERDOC_InputButton f12 = RENDERDOC_InputButton::eRENDERDOC_Key_F12;
				rdoc_api->SetFocusToggleKeys(&f12, 0);

				// https://github.com/baldurk/renderdoc/issues/90
				// renderdoc 启动后，资源卸载可能会导致检查报错
				rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_AllowVSync, 1);
				mUnavailableReason.clear();
			}
		}
#endif
	}

	void RenderDocCaptureService::Shutdown() noexcept
	{
		rdoc_api = nullptr;
		mUnavailableReason = "RenderDoc plugin has stopped.";
	}

	bool RenderDocCaptureService::IsAvailable() const noexcept
	{
		return rdoc_api != nullptr;
	}

	std::string RenderDocCaptureService::GetUnavailableReason() const
	{
		return mUnavailableReason;
	}

	void RenderDocCaptureService::Capture()
	{
		if (!rdoc_api)
		{
			HE_CORE_WARN("RenderDoc capture unavailable: {}", mUnavailableReason);
			return;
		}

		rdoc_api->TriggerCapture();
		if (!rdoc_api->IsRemoteAccessConnected())
		{
			rdoc_api->LaunchReplayUI(true, NULL);
		}
	}
}
