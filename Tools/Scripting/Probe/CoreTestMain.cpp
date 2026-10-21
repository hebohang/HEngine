#include "Runtime/UnitTest/ScriptCoreTests.h"

#include <iostream>
#include <stdexcept>

int main()
{
    try
    {
        HEngine::Tests::ScriptCoreContract();
        std::cout << "PASS: isolated ScriptCore contracts\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
