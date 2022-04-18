#pragma once

#include <type_traits>

namespace HEngine
{

    template<typename T>
    class PublicSingleton
    {
    protected:
        PublicSingleton() = default;

    public:
        [[nodiscard]] static T& GetInstance() noexcept(std::is_nothrow_constructible<T>::value)
        {
            static T instance;
            return instance;
        }
        virtual ~PublicSingleton() noexcept = default;
        PublicSingleton(const PublicSingleton&) = delete;
        PublicSingleton& operator=(const PublicSingleton&) = delete;
    };
}