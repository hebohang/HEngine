#pragma once

namespace std
{
    template<typename T>
    class Alloc
    {};

    template<typename T, typename Allocator = Alloc<T>>
    class vector
    {
    public:
        using value_type = T;
        T& operator[](unsigned long);
        const T& operator[](unsigned long) const;
    };

    class string
    {};
}

#define HE_TEST_REFLECT(...) __attribute__((annotate(#__VA_ARGS__)))
#define HE_TEST_PROPERTY(...) __attribute__((annotate(#__VA_ARGS__)))
