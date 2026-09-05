#include "Hepch.h"

#include "IniFIle.h"

#include <simpleIni/SimpleIni.h>
#include <simpleIni/ConvertUTF.h>

namespace HEngine
{
    HIni::HIni(const char* iniFilePath)
    {
        //mIni.SetSpaces(true);
        mIni.SetQuotes(true);  // now the value is the same as "value" in ini file
        auto rc = mIni.LoadFile(iniFilePath);
        HE_CORE_ASSERT(rc >= 0 && "ini file load failed!");
    }

    const char* HIni::GetValue(const char* key, const char* section)
    {
        return mIni.GetValue(section, key);
    }

    void HIni::SetValue(const char* key, const char* newVal, const char* section)
    {
        mIni.SetValue(section, key, newVal);
    }
}