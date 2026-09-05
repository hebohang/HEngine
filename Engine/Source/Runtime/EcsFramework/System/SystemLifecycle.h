#pragma once

#include <cstddef>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace HEngine::SystemLifecycle
{
	namespace Detail
	{
		template<typename FailureCallback>
		void ReportFailure(
			FailureCallback& reportFailure,
			std::string_view context,
			const std::exception_ptr& failure) noexcept
		{
			try
			{
				reportFailure(context, failure);
			}
			catch (...)
			{
			}
		}
	}

	template<
		typename StartCallback,
		typename StopCallback,
		typename CleanupCallback,
		typename FailureCallback>
	void Start(
		size_t systemCount,
		size_t& activeSystemCount,
		StartCallback&& startSystem,
		StopCallback&& stopSystem,
		CleanupCallback&& cleanup,
		FailureCallback&& reportFailure)
	{
		if (activeSystemCount != 0)
			throw std::logic_error("Runtime systems are already active.");

		try
		{
			for (size_t systemIndex = 0; systemIndex < systemCount; ++systemIndex)
			{
				++activeSystemCount;
				startSystem(systemIndex);
			}
		}
		catch (...)
		{
			const std::exception_ptr startupFailure = std::current_exception();
			while (activeSystemCount > 0)
			{
				const size_t systemIndex = --activeSystemCount;
				try
				{
					stopSystem(systemIndex);
				}
				catch (...)
				{
					Detail::ReportFailure(
						reportFailure,
						"Runtime system rollback failed.",
						std::current_exception());
				}
			}

			try
			{
				cleanup();
			}
			catch (...)
			{
				Detail::ReportFailure(
					reportFailure,
					"Failed to clean up after runtime system startup rollback.",
					std::current_exception());
			}

			std::rethrow_exception(startupFailure);
		}
	}

	template<
		typename StopCallback,
		typename CleanupCallback,
		typename FailureCallback>
	void Stop(
		size_t& activeSystemCount,
		StopCallback&& stopSystem,
		CleanupCallback&& cleanup,
		FailureCallback&& reportFailure)
	{
		std::exception_ptr firstFailure;
		const auto saveOrReportFailure = [
			&firstFailure,
			&reportFailure](
				std::string_view context,
				const std::exception_ptr& failure) noexcept
		{
			if (!firstFailure)
			{
				firstFailure = failure;
				return;
			}

			Detail::ReportFailure(reportFailure, context, failure);
		};

		const size_t systemCount = std::exchange(activeSystemCount, 0);
		for (size_t remainingSystemCount = systemCount;
			remainingSystemCount > 0;
			--remainingSystemCount)
		{
			try
			{
				stopSystem(remainingSystemCount - 1);
			}
			catch (...)
			{
				saveOrReportFailure(
					"Additional runtime system stop failure.",
					std::current_exception());
			}
		}

		try
		{
			cleanup();
		}
		catch (...)
		{
			saveOrReportFailure(
				"Failed to clean up after runtime system stop.",
				std::current_exception());
		}

		if (firstFailure)
			std::rethrow_exception(firstFailure);
	}
}
