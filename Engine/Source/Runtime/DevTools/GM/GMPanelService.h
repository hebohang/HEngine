#pragma once

#include "Runtime/Core/Templates/PublicSingleton.h"
#include "Runtime/Core/UUID.h"
#include "Runtime/DevTools/Commands/DevCommandTypes.h"
#include "Runtime/UI/Rml/RmlUiManager.h"

#include <cstdint>
#include <string>
#include <vector>

namespace HEngine
{
	class Event;
	class KeyPressedEvent;

	class GMPanelService final : public PublicSingleton<GMPanelService>
	{
	public:
		GMPanelService() = default;
		~GMPanelService();

		void Initialize();
		void Shutdown();
		void Update();
		bool OnEvent(Event& event);
		void Toggle();
		void SetVisible(bool bVisible);
		[[nodiscard]] bool IsVisible() const { return mVisible; }

	private:
		bool OnKeyPressed(KeyPressedEvent& event);
		void OnDocumentEvent(const RmlUiNativeEvent& event);
		void RefreshDiscovery();
		void RenderSelectedCommand();
		void RenderHistory();
		void ExecuteSelected(bool bConfirmed);
		[[nodiscard]] Json ReadArguments(const DevCommandDescription& description) const;

		UUID mOwner;
		std::string mDocumentId;
		std::string mSelectedCommand;
		std::vector<std::string> mCommandNames;
		std::uint64_t mObservedCommandRevision = 0;
		std::uint64_t mObservedDomainRevision = 0;
		bool mInitialized = false;
		bool mVisible = false;
		bool mConfirmationPending = false;
	};
}
