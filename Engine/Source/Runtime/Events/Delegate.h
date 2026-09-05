#pragma once
#include "Hepch.h"
#include "Runtime/Core/Base/Base.h"

namespace HEngine 
{
#define DECLARE_MULTICAST_DELEGATE(name, ...) \
	using name = MultiCastDelegate<__VA_ARGS__>; \

	class DelegateHandle
	{
	public:
		DelegateHandle() : id(INVALID_ID) {}
		explicit DelegateHandle(int) : id(GenerateID()) {}
		~DelegateHandle() noexcept = default;
		DelegateHandle(const DelegateHandle&) = default;
		DelegateHandle(DelegateHandle&& that) noexcept : id(that.id)
		{
			that.Reset();
		}
		DelegateHandle& operator=(const DelegateHandle&) = default;
		DelegateHandle& operator=(DelegateHandle&& that) noexcept
		{
			id = that.id;
			that.Reset();
			return *this;
		}

		operator bool() const
		{
			return IsValid();
		}

		bool operator==(const DelegateHandle& that) const
		{
			return id == that.id;
		}

		bool operator<(const DelegateHandle& that) const
		{
			return id < that.id;
		}

		bool IsValid() const
		{
			return id != INVALID_ID;
		}

		void Reset()
		{
			id = INVALID_ID;
		}

	private:
		size_t id;

	private:

		inline static constexpr size_t INVALID_ID = size_t(-1);
		static size_t GenerateID()
		{
			static size_t currentID = 0;
			return currentID++;
		}
	};

	template<typename... Args>
	class MultiCastDelegate
	{
		using DelegateType = std::function<void(Args...)>;
		using HandleDelegatePair = std::pair<DelegateHandle, DelegateType>;

	public:
		MultiCastDelegate() = default;
		MultiCastDelegate(const MultiCastDelegate&) = default;
		MultiCastDelegate(MultiCastDelegate&&) noexcept = default;
		~MultiCastDelegate() = default;
		MultiCastDelegate& operator=(const MultiCastDelegate&) = default;
		MultiCastDelegate& operator=(MultiCastDelegate&&) noexcept = default;

		template<typename F> requires std::is_constructible_v<DelegateType, F>
		[[maybe_unused]] DelegateHandle Add(F&& callable)
		{
			delegateArray.emplace_back(DelegateHandle(0), std::forward<F>(callable));
			return delegateArray.back().first;
		}

		template<typename T>
		[[maybe_unused]] DelegateHandle AddMember(void(T::* mem_pfn)(Args...), T& instance)
		{
			delegateArray.emplace_back(DelegateHandle(0), [&instance, mem_pfn](Args&&... args) mutable -> void {return (instance.*mem_pfn)(std::forward<Args>(args)...); });
			return delegateArray.back().first;
		}

		template<typename F> requires std::is_constructible_v<DelegateType, F>
		[[maybe_unused]] DelegateHandle operator+=(F&& callable) noexcept
		{
			return Add(std::forward<F>(callable));
		}

		[[maybe_unused]] bool operator-=(DelegateHandle& handle)
		{
			return Remove(handle);
		}

		[[maybe_unused]] bool Remove(DelegateHandle& handle)
		{
			if (handle.IsValid())
			{
				for (size_t i = 0; i < delegateArray.size(); ++i)
				{
					if (delegateArray[i].first == handle)
					{
						std::swap(delegateArray[i], delegateArray.back());
						delegateArray.pop_back();
						handle.Reset();
						return true;
					}
				}
			}
			return false;
		}

		void RemoveAll()
		{
			delegateArray.clear();
		}

		void Broadcast(Args... args)
		{
			for (size_t i = 0; i < delegateArray.size(); ++i)
			{
				if (delegateArray[i].first.IsValid()) delegateArray[i].second(std::forward<Args>(args)...);
			}
		}

		bool IsHandleBound(const DelegateHandle& handle) const
		{
			if (handle.IsValid())
			{
				for (size_t i = 0; i < delegateArray.size(); ++i)
				{
					if (delegateArray[i].Handle == handle) return true;
				}
			}
			return false;
		}

	private:
		std::vector<HandleDelegatePair> delegateArray;
	};
}

