#pragma once

#include "Runtime/Core/Plugins/Services/RenderCaptureService.h"

#include <string>

namespace HEngine
{
	class RenderDocCaptureService final : public IRenderCaptureService
	{
	public:
		void Initialize();
		void Shutdown() noexcept;
		[[nodiscard]] bool IsAvailable() const noexcept override;
		[[nodiscard]] std::string GetUnavailableReason() const override;
		void Capture() override;

	private:
		std::string mUnavailableReason = "RenderDoc is not initialized.";
	};
}
