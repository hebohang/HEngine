#pragma once

namespace HEngine
{
	class IModule
	{
	public:
		virtual ~IModule() {}

		virtual void StartModule() {}

		virtual void StopModule() {}
	};
}