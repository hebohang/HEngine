#include "Hepch.h"

#include "Runtime/DevTools/GM/GMPanelService.h"
#include "Runtime/DevTools/GM/GMPanelModel.h"
#include "Runtime/DevTools/GM/GMService.h"
#include "Runtime/Core/Input/KeyCodes.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/Events/Event.h"
#include "Runtime/Events/KeyEvent.h"

#include <charconv>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace HEngine
{
	GMPanelService::~GMPanelService()
	{
		Shutdown();
	}

	void GMPanelService::Initialize()
	{
		if (mInitialized)
			return;

		auto& rmlUi = RmlUiManager::GetInstance();
		if (!rmlUi.IsInitialized() ||
			!rmlUi.LoadDocument("Assets/DevOnly/GM/GMPanel.hui", mOwner, &mDocumentId))
		{
			HE_CORE_ERROR("GM panel could not load its Engine-owned RmlUi document.");
			return;
		}
		if (!rmlUi.SetDocumentEventHandler(
			mDocumentId,
			[this](const RmlUiNativeEvent& event) { OnDocumentEvent(event); }))
		{
			(void)rmlUi.UnloadDocument(mDocumentId);
			mDocumentId.clear();
			HE_CORE_ERROR("GM panel could not attach its native event handler.");
			return;
		}

		mInitialized = true;
		(void)GMService::GetInstance().UnregisterOwner("engine.gm.panel");
		DevCommandDescription visibilityCommand;
		visibilityCommand.Name = "engine.gm.panel.set_visible";
		visibilityCommand.DisplayName = "设置 GM 面板显示状态";
		visibilityCommand.Summary = "显示或隐藏引擎内置 GM 面板。";
		visibilityCommand.Category = "引擎";
		visibilityCommand.Scope = "developer-ui";
		visibilityCommand.Owner = "engine.gm.panel";
		DevCommandArgument visibleArgument;
		visibleArgument.Name = "visible";
		visibleArgument.DisplayName = "显示";
		visibleArgument.Type = DevCommandArgumentType::Boolean;
		visibleArgument.bRequired = true;
		visibleArgument.DefaultValue = true;
		visibilityCommand.Arguments.push_back(std::move(visibleArgument));
		if (!GMService::GetInstance().GetCommandRegistry().Register(
			std::move(visibilityCommand),
			[this](DevCommandExecutionContext& context)
			{
				SetVisible(context.Arguments.at("visible").get<bool>());
				return DevCommandResult::Success(Json{ { "visible", IsVisible() } });
			}))
		{
			(void)rmlUi.UnloadDocument(mDocumentId);
			mDocumentId.clear();
			mInitialized = false;
			HE_CORE_ERROR("GM panel could not register its C++ visibility command.");
			return;
		}
		RefreshDiscovery();
		RenderHistory();
		(void)rmlUi.HideDocument(mDocumentId);
		HE_CORE_INFO("GM panel initialized. Press F10 to toggle it.");
	}

	void GMPanelService::Shutdown()
	{
		if (!mInitialized)
			return;
		auto& rmlUi = RmlUiManager::GetInstance();
		if (rmlUi.IsInitialized())
		{
			(void)rmlUi.SetDocumentEventHandler(mDocumentId, {});
			(void)rmlUi.UnloadDocument(mDocumentId);
		}
		mDocumentId.clear();
		(void)GMService::GetInstance().UnregisterOwner("engine.gm.panel");
		mCommandNames.clear();
		mSelectedCommand.clear();
		mInitialized = false;
		mVisible = false;
		mConfirmationPending = false;
	}

	void GMPanelService::Update()
	{
		if (!mInitialized)
			return;
		const auto& service = GMService::GetInstance();
		if (mObservedCommandRevision != service.GetCommandRegistry().GetRevision() ||
			mObservedDomainRevision != service.GetStateDomainRegistry().GetRevision())
		{
			RefreshDiscovery();
		}
	}

	bool GMPanelService::OnEvent(Event& event)
	{
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<KeyPressedEvent>([this](KeyPressedEvent& keyEvent)
		{
			return OnKeyPressed(keyEvent);
		});
		return event.Handled;
	}

	void GMPanelService::Toggle()
	{
		SetVisible(!mVisible);
	}

	void GMPanelService::SetVisible(bool bVisible)
	{
		if (!mInitialized || mVisible == bVisible)
			return;
		auto& rmlUi = RmlUiManager::GetInstance();
		const bool bUpdated = bVisible
			? rmlUi.ShowDocument(mDocumentId)
			: rmlUi.HideDocument(mDocumentId);
		if (!bUpdated)
			return;
		mVisible = bVisible;
		(void)rmlUi.SetMouseCursorVisible(mDocumentId, bVisible);
		if (bVisible)
		{
			RefreshDiscovery();
			RenderHistory();
		}
	}

	bool GMPanelService::OnKeyPressed(KeyPressedEvent& event)
	{
		if (event.GetKeyCode() != Key::F10 || event.GetRepeatCount() != 0)
			return false;
		Toggle();
		return true;
	}

	void GMPanelService::OnDocumentEvent(const RmlUiNativeEvent& event)
	{
		if (event.Type != "click")
			return;
		const std::string& source = event.SourceElementId;
		if (source == "gm_close")
		{
			SetVisible(false);
			return;
		}
		if (source == "gm_refresh")
		{
			RefreshDiscovery();
			RenderHistory();
			return;
		}
		if (source == "gm_execute")
		{
			ExecuteSelected(false);
			return;
		}
		if (source == "gm_confirm")
		{
			ExecuteSelected(true);
			return;
		}
		constexpr std::string_view prefix = "gm_cmd_";
		if (!source.starts_with(prefix))
			return;
		std::size_t index = 0;
		const std::string_view suffix(source.data() + prefix.size(), source.size() - prefix.size());
		const auto [end, error] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), index);
		if (error != std::errc{} || end != suffix.data() + suffix.size() || index >= mCommandNames.size())
			return;
		mSelectedCommand = mCommandNames[index];
		mConfirmationPending = false;
		RenderSelectedCommand();
	}

	void GMPanelService::RefreshDiscovery()
	{
		if (!mInitialized)
			return;
		GMService& service = GMService::GetInstance();
		const Json discovery = service.Discover();
		mObservedCommandRevision = service.GetCommandRegistry().GetRevision();
		mObservedDomainRevision = service.GetStateDomainRegistry().GetRevision();
		(void)RmlUiManager::GetInstance().SetElementMarkup(
			mDocumentId,
			"gm_command_list",
			GMPanelModel::BuildCommandList(discovery, mCommandNames));
		(void)RmlUiManager::GetInstance().SetElementText(
			mDocumentId,
			"gm_taint_state",
			service.GetExecutor().IsCurrentSessionTainted() ? "已污染" : "干净");

		if (!mSelectedCommand.empty() && !service.GetCommandRegistry().Find(mSelectedCommand))
			mSelectedCommand.clear();
		RenderSelectedCommand();
	}

	void GMPanelService::RenderSelectedCommand()
	{
		if (!mInitialized)
			return;
		const DevCommandDescription* description =
			GMService::GetInstance().GetCommandRegistry().Find(mSelectedCommand);
		if (!description)
		{
			(void)RmlUiManager::GetInstance().SetElementMarkup(
				mDocumentId,
				"gm_command_form",
				"<p class=\"gm-empty\">请选择命令查看参数定义。</p>");
			return;
		}
		(void)RmlUiManager::GetInstance().SetElementMarkup(
			mDocumentId,
			"gm_command_form",
			GMPanelModel::BuildCommandForm(*description));
	}

	void GMPanelService::RenderHistory()
	{
		if (!mInitialized)
			return;
		(void)RmlUiManager::GetInstance().SetElementMarkup(
			mDocumentId,
			"gm_history",
			GMPanelModel::BuildHistory(GMService::GetInstance().GetHistoryJson(25)));
	}

	void GMPanelService::ExecuteSelected(bool bConfirmed)
	{
		const DevCommandDescription* description =
			GMService::GetInstance().GetCommandRegistry().Find(mSelectedCommand);
		if (!description)
			return;

		DevCommandResult result;
		try
		{
			result = GMService::GetInstance().Execute(
				mSelectedCommand,
				ReadArguments(*description),
				bConfirmed,
				"panel");
		}
		catch (const std::exception& error)
		{
			result = DevCommandResult::Failure("invalid_arguments", error.what());
		}

		mConfirmationPending = result.Code == "confirmation_required";
		auto& rmlUi = RmlUiManager::GetInstance();
		(void)rmlUi.SetElementText(
			mDocumentId,
			"gm_status",
			result.Message + "（" + result.Code + "）");
		(void)rmlUi.SetElementClass(
			mDocumentId,
			"gm_confirm",
			"hidden",
			!mConfirmationPending);
		(void)rmlUi.SetElementText(
			mDocumentId,
			"gm_taint_state",
			GMService::GetInstance().GetExecutor().IsCurrentSessionTainted() ? "已污染" : "干净");
		if (result.Code != "confirmation_required")
			RenderHistory();
	}

	Json GMPanelService::ReadArguments(const DevCommandDescription& description) const
	{
		Json arguments = Json::object();
		for (std::size_t index = 0; index < description.Arguments.size(); ++index)
		{
			const DevCommandArgument& schema = description.Arguments[index];
			std::string value;
			if (!RmlUiManager::GetInstance().GetElementValue(
				mDocumentId,
				"gm_arg_" + std::to_string(index),
				value))
			{
				throw std::runtime_error("GM 表单控件不可用：" + schema.Name);
			}
			if (value.empty() && !schema.bRequired && schema.DefaultValue.is_null())
				continue;

			switch (schema.Type)
			{
				case DevCommandArgumentType::Boolean:
					arguments[schema.Name] = value == "true";
					break;
				case DevCommandArgumentType::Integer:
				{
					std::int64_t parsed = 0;
					const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
					if (error != std::errc{} || end != value.data() + value.size())
						throw std::runtime_error("GM 整数参数无效：" + schema.Name);
					arguments[schema.Name] = parsed;
					break;
				}
				case DevCommandArgumentType::Number:
				{
					std::size_t consumed = 0;
					const double parsed = std::stod(value, &consumed);
					if (consumed != value.size())
						throw std::runtime_error("GM 数值参数无效：" + schema.Name);
					arguments[schema.Name] = parsed;
					break;
				}
				case DevCommandArgumentType::String:
				case DevCommandArgumentType::Enumeration:
					arguments[schema.Name] = value;
					break;
			}
		}
		return arguments;
	}
}
