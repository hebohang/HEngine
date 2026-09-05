#pragma once

#define HE_TEST_REFLECT(...) __attribute__((annotate(#__VA_ARGS__)))

class HE_TEST_REFLECT(Fields, DisplayName : "First", DisplayName : "Second")
    MetadataDuplicate
{
public:
    int Value = 0;
};
