#include "Runtime/Core/Log/Log.h"
#include "Runtime/Core/Log/LogSession.h"
#include "Runtime/UnitTest/RenderSettingsTests.h"

#include <exception>
#include <iostream>

int main()
{
    HEngine::LogSession logSession;
    try
    {
        HEngine::Log::Init();
        HEngine::Tests::RenderSettingsContract();
        std::cout << "[Test] PASS name=RenderSettings\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[Test] FAIL name=RenderSettings: " << error.what() << '\n';
        return 1;
    }
}
