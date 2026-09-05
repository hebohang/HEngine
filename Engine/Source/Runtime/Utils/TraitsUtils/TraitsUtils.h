#pragma once
#include <type_traits>

namespace HEngine
{
	namespace Utils
	{
		template<typename T>
		requires(!std::is_lvalue_reference_v<T>)
		T* GetRValuePtr(T&& v) 
		{
			return &v;
		}

		template<typename T>
		struct ArrayMeta;

		template<typename T, size_t N>
		struct ArrayMeta<T[N]> 
		{
			static constexpr size_t ArraySize = N;
			static constexpr size_t ByteSize = N * sizeof(T);
		};

		template<typename T>
		requires(std::is_bounded_array_v<T>) 
		constexpr size_t ArrayCount(T const& t) 
		{
			return ArrayMeta<T>::ArraySize;
		}

		template<typename T>
		requires(std::is_bounded_array_v<T>) 
		constexpr size_t ArrayByteSize(T const& t) 
		{
			return ArrayMeta<T>::ByteSize;
		}
	}
}
