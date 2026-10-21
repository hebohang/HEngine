#pragma once

#include "Commandlets/Commandlet.h"
#include "HELightmapExportCommandlet.generated.h"

UCLASS()
class HELIGHTMAPBRIDGE_API UHELightmapExportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UHELightmapExportCommandlet();
    virtual int32 Main(const FString& Params) override;
};
