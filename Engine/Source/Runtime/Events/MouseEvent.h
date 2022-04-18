#pragma once

#include "Event.h"
#include "Runtime/Input/MouseCodes.h"

namespace HEngine 
{

	class HENGINE_API MouseMovedEvent : public Event
	{
	public:
		MouseMovedEvent(const float x, const float y)
			: mMouseX(x), mMouseY(y) {}

		[[nodiscard]] inline float GetX() const { return mMouseX; }
		[[nodiscard]] inline float GetY() const { return mMouseY; }

		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "MouseMovedEvent: " << mMouseX << ", " << mMouseY;
			return ss.str();
		}

		EVENT_CLASS_TYPE(MouseMoved)
		EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)
	private:
		float mMouseX, mMouseY;
	};

	class HENGINE_API MouseScrolledEvent : public Event
	{
	public:
		MouseScrolledEvent(const float xOffset, const float yOffset)
			: mXOffset(xOffset), mYOffset(yOffset) {}

		[[nodiscard]] inline float GetXOffset() const { return mXOffset; }
		[[nodiscard]] inline float GetYOffset() const { return mYOffset; }

		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "MouseScrolledEvent: " << GetXOffset() << ", " << GetYOffset();
			return ss.str();
		}

		EVENT_CLASS_TYPE(MouseScrolled)
		EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)
	private:
		float mXOffset, mYOffset;
	};

	class HENGINE_API MouseButtonEvent : public Event
	{
	public:
		MouseCode GetMouseButton() const { return mButton; }

		EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)
	protected:
		MouseButtonEvent(const MouseCode button)
			: mButton(button) {}

		MouseCode mButton;
	};

	class HENGINE_API MouseButtonPressedEvent : public MouseButtonEvent
	{
	public:
		MouseButtonPressedEvent(const MouseCode button)
			: MouseButtonEvent(button) {}

		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "MouseButtonPressedEvent: " << mButton;
			return ss.str();
		}

		EVENT_CLASS_TYPE(MouseButtonPressed)
	};

	class HENGINE_API MouseButtonReleasedEvent : public MouseButtonEvent
	{
	public:
		MouseButtonReleasedEvent(const MouseCode button)
			: MouseButtonEvent(button) {}

		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "MouseButtonReleasedEvent: " << mButton;
			return ss.str();
		}

		EVENT_CLASS_TYPE(MouseButtonReleased)
	};

}