#pragma once

#include <string>

namespace HEngine
{
    class HDXResourceBase
    {
    public:
        HDXResourceBase() = default;
#if HE_DEBUG
    public:
        HDXResourceBase(std::string name)
            : mResName(name)
        {
        }
    public:
        void SetName(const std::string& name) 
        { 
            mResName = name; 
        }

        const std::string& GetName() const
        {
            return mResName;
        }
    protected:
        std::string mResName = "";
#endif
    };
}
