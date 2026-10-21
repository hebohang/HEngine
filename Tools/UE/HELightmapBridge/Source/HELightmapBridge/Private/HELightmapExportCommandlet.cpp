#include "HELightmapExportCommandlet.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "LightMap.h"
#include "Misc/FileHelper.h"
#include "Misc/Crc.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "SceneManagement.h"
#include "StaticMeshComponentLODInfo.h"
#include "TextureResource.h"

DEFINE_LOG_CATEGORY_STATIC(LogHELightmapBridge, Log, All);

UHELightmapExportCommandlet::UHELightmapExportCommandlet()
{
    IsClient = false;
    IsServer = false;
    LogToConsole = true;
}

int32 UHELightmapExportCommandlet::Main(const FString& Params)
{
    FString LevelPath;
    FString OutputPath;
    FString TextureDir;
    int32 Limit = 0;
    int32 MaxTextures = 0;
    int32 RawBasis = -1;
    if (!FParse::Value(*Params, TEXT("Level="), LevelPath) ||
        !FParse::Value(*Params, TEXT("Output="), OutputPath))
    {
        UE_LOG(LogHELightmapBridge, Error, TEXT("Usage: -run=HELightmapExport -Level=/Game/... -Output=C:/.../bindings.json [-Limit=100]"));
        return 2;
    }
    FParse::Value(*Params, TEXT("Limit="), Limit);
    FParse::Value(*Params, TEXT("TextureDir="), TextureDir);
    FParse::Value(*Params, TEXT("MaxTextures="), MaxTextures);
    FParse::Value(*Params, TEXT("RawBasis="), RawBasis);
    const bool bMetadataOnly = FParse::Param(*Params, TEXT("MetadataOnly"));
    const bool bAllMips = FParse::Param(*Params, TEXT("AllMips"));
    if (RawBasis < -1 || RawBasis > 1)
    {
        UE_LOG(LogHELightmapBridge, Error, TEXT("RawBasis must be -1 (both), 0 (HQ), or 1 (LQ)"));
        return 2;
    }
    if (!FPackageName::IsValidLongPackageName(LevelPath) || FPaths::IsRelative(OutputPath))
    {
        UE_LOG(LogHELightmapBridge, Error, TEXT("Level must be a UE package path and Output must be absolute"));
        return 2;
    }
    OutputPath = FPaths::ConvertRelativePathToFull(OutputPath);
    FPaths::NormalizeFilename(OutputPath);
    const FString ContentPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
    if (FPaths::IsUnderDirectory(OutputPath, ContentPath) || FPaths::IsSamePath(OutputPath, ContentPath))
    {
        UE_LOG(LogHELightmapBridge, Error, TEXT("Output must be outside the UE Content directory"));
        return 2;
    }
    if (!TextureDir.IsEmpty())
    {
        if (FPaths::IsRelative(TextureDir))
        {
            UE_LOG(LogHELightmapBridge, Error, TEXT("TextureDir must be absolute"));
            return 2;
        }
        TextureDir = FPaths::ConvertRelativePathToFull(TextureDir);
        FPaths::NormalizeDirectoryName(TextureDir);
        if (FPaths::IsUnderDirectory(TextureDir, ContentPath) ||
            FPaths::IsSamePath(TextureDir, ContentPath))
        {
            UE_LOG(LogHELightmapBridge, Error, TEXT("TextureDir must be outside UE Content"));
            return 2;
        }
    }

    const FString LevelFilename = FPackageName::LongPackageNameToFilename(
        LevelPath, FPackageName::GetMapPackageExtension());
    UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(LevelFilename);
    if (!World)
    {
        UE_LOG(LogHELightmapBridge, Error, TEXT("Failed to load %s"), *LevelPath);
        return 3;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("schema"), TEXT("he.ue.lightmap-bindings.v1"));
    Root->SetStringField(TEXT("level"), LevelPath);
    TArray<TSharedPtr<FJsonValue>> Bindings;
    int32 MeshComponentCount = 0;
    int32 LightmappedComponentCount = 0;
    int32 RawTextureCount = 0;
    TSet<FString> ExportedTextures;
    for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
    {
        TArray<UStaticMeshComponent*> Components;
        ActorIt->GetComponents(Components);
        for (UStaticMeshComponent* Component : Components)
        {
            if (!Component || !Component->GetStaticMesh())
            {
                continue;
            }
            ++MeshComponentCount;
            for (int32 LodIndex = 0; LodIndex < Component->LODData.Num(); ++LodIndex)
            {
                const FStaticMeshComponentLODInfo& LodInfo = Component->LODData[LodIndex];
                const FMeshMapBuildData* BuildData = Component->GetMeshMapBuildData(LodInfo, false);
                const FLightMap2D* LightMap = BuildData && BuildData->LightMap.GetReference()
                    ? BuildData->LightMap->GetLightMap2D() : nullptr;
                if (!LightMap)
                {
                    continue;
                }
                ++LightmappedComponentCount;
                TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
                Binding->SetStringField(TEXT("actor"), ActorIt->GetPathName());
                Binding->SetStringField(TEXT("component"), Component->GetPathName());
                Binding->SetStringField(TEXT("mesh"), Component->GetStaticMesh()->GetPathName());
                Binding->SetStringField(TEXT("mapBuildDataId"), LodInfo.MapBuildDataId.ToString());
                Binding->SetNumberField(TEXT("lod"), LodIndex);
                Binding->SetNumberField(TEXT("lightmapUvChannel"), Component->GetStaticMesh()->GetLightMapCoordinateIndex());
                TArray<TSharedPtr<FJsonValue>> Scale;
                TArray<TSharedPtr<FJsonValue>> Bias;
                const FVector2D CoordinateScale = LightMap->GetCoordinateScale();
                const FVector2D CoordinateBias = LightMap->GetCoordinateBias();
                Scale.Add(MakeShared<FJsonValueNumber>(CoordinateScale.X));
                Scale.Add(MakeShared<FJsonValueNumber>(CoordinateScale.Y));
                Bias.Add(MakeShared<FJsonValueNumber>(CoordinateBias.X));
                Bias.Add(MakeShared<FJsonValueNumber>(CoordinateBias.Y));
                Binding->SetArrayField(TEXT("coordinateScale"), Scale);
                Binding->SetArrayField(TEXT("coordinateBias"), Bias);
                const FLightMapInteraction Interaction = LightMap->GetInteraction(ERHIFeatureLevel::SM5);
                if (Interaction.GetType() != LMIT_Texture && !bMetadataOnly)
                {
                    UE_LOG(LogHELightmapBridge, Error,
                        TEXT("Lightmap coefficient data unavailable for %s; rerun with -AllowCommandletRendering or explicitly use -MetadataOnly"),
                        *Component->GetPathName());
                    return 6;
                }
                if (Interaction.GetType() == LMIT_Texture)
                {
                    TArray<TSharedPtr<FJsonValue>> CoefficientScales;
                    TArray<TSharedPtr<FJsonValue>> CoefficientAdds;
                    const FVector4f* ScaleVectors = Interaction.GetScaleArray();
                    const FVector4f* AddVectors = Interaction.GetAddArray();
                    for (uint32 CoefficientIndex = 0;
                         CoefficientIndex < Interaction.GetNumLightmapCoefficients(); ++CoefficientIndex)
                    {
                        const FVector4f& S = ScaleVectors[CoefficientIndex];
                        const FVector4f& A = AddVectors[CoefficientIndex];
                        TArray<TSharedPtr<FJsonValue>> SComponents;
                        TArray<TSharedPtr<FJsonValue>> AComponents;
                        for (int32 Channel = 0; Channel < 4; ++Channel)
                        {
                            SComponents.Add(MakeShared<FJsonValueNumber>(S[Channel]));
                            AComponents.Add(MakeShared<FJsonValueNumber>(A[Channel]));
                        }
                        CoefficientScales.Add(MakeShared<FJsonValueArray>(SComponents));
                        CoefficientAdds.Add(MakeShared<FJsonValueArray>(AComponents));
                    }
                    Binding->SetArrayField(TEXT("coefficientScales"), CoefficientScales);
                    Binding->SetArrayField(TEXT("coefficientAdds"), CoefficientAdds);
                }

                TArray<TSharedPtr<FJsonValue>> Textures;
                for (uint32 BasisIndex = 0; BasisIndex < 2; ++BasisIndex)
                {
                    const UTexture2D* Texture = LightMap->GetTexture(BasisIndex);
                    if (!Texture)
                    {
                        continue;
                    }
                    TSharedRef<FJsonObject> TextureInfo = MakeShared<FJsonObject>();
                    TextureInfo->SetNumberField(TEXT("basis"), BasisIndex);
                    TextureInfo->SetStringField(TEXT("path"), Texture->GetPathName());
                    TextureInfo->SetNumberField(TEXT("sourceWidth"), Texture->Source.GetSizeX());
                    TextureInfo->SetNumberField(TEXT("sourceHeight"), Texture->Source.GetSizeY());
                    TextureInfo->SetNumberField(TEXT("sourceFormat"), static_cast<int32>(Texture->Source.GetFormat()));
                    TextureInfo->SetNumberField(TEXT("sourceMips"), Texture->Source.GetNumMips());
                    TextureInfo->SetBoolField(TEXT("srgb"), Texture->SRGB);
                    if (const FTexturePlatformData* PlatformData = Texture->GetPlatformData())
                    {
                        TextureInfo->SetNumberField(TEXT("platformFormat"), PlatformData->PixelFormat);
                        TextureInfo->SetNumberField(TEXT("platformMips"), PlatformData->Mips.Num());
                        TextureInfo->SetNumberField(TEXT("platformMip0Bytes"),
                            PlatformData->Mips.Num() ? PlatformData->Mips[0].BulkData.GetBulkDataSize() : 0);
                    }
                    if (!TextureDir.IsEmpty() && (RawBasis < 0 || RawBasis == static_cast<int32>(BasisIndex)) &&
                        Texture->Source.GetFormat() == TSF_BGRA8 &&
                        (ExportedTextures.Contains(Texture->GetPathName()) ||
                         MaxTextures <= 0 || RawTextureCount < MaxTextures))
                    {
                        const FString TexturePath = Texture->GetPathName();
                        const FString RawStem = FString::Printf(TEXT("%08x_%s"),
                            FCrc::StrCrc32(*TexturePath), *Texture->GetName());
                        TArray<TSharedPtr<FJsonValue>> RawMips;
                        const int32 MipCount = bAllMips ? Texture->Source.GetNumMips() : 1;
                        for (int32 MipIndex = 0; MipIndex < MipCount; ++MipIndex)
                        {
                            const int64 MipWidth = FMath::Max<int64>(1, Texture->Source.GetSizeX() >> MipIndex);
                            const int64 MipHeight = FMath::Max<int64>(1, Texture->Source.GetSizeY() >> MipIndex);
                            const FString RawName = RawStem +
                                (MipIndex ? FString::Printf(TEXT("_mip%d"), MipIndex) : TEXT("")) +
                                TEXT(".bgra8");
                            const FString RawPath = FPaths::Combine(TextureDir, RawName);
                            if (!ExportedTextures.Contains(TexturePath))
                            {
                                TArray64<uint8> Mip;
                                const int64 ExpectedBytes = MipWidth * MipHeight * 4;
                                // UE's copy-out API is non-const because it may populate a decode cache;
                                // it does not edit source pixels or mark the asset dirty.
                                if (!const_cast<FTextureSource&>(Texture->Source).GetMipData(
                                        Mip, 0, 0, MipIndex) ||
                                    Mip.Num() != ExpectedBytes ||
                                    !IFileManager::Get().MakeDirectory(*TextureDir, true) ||
                                    !FFileHelper::SaveArrayToFile(Mip, *RawPath))
                                {
                                    UE_LOG(LogHELightmapBridge, Error,
                                        TEXT("Failed to export BGRA8 mip %d: %s"), MipIndex, *TexturePath);
                                    return 5;
                                }
                            }
                            TSharedRef<FJsonObject> RawMip = MakeShared<FJsonObject>();
                            RawMip->SetNumberField(TEXT("mip"), MipIndex);
                            RawMip->SetNumberField(TEXT("width"), MipWidth);
                            RawMip->SetNumberField(TEXT("height"), MipHeight);
                            RawMip->SetStringField(TEXT("file"), RawPath);
                            RawMips.Add(MakeShared<FJsonValueObject>(RawMip));
                            if (MipIndex == 0)
                            {
                                TextureInfo->SetStringField(TEXT("rawFile"), RawPath);
                            }
                        }
                        if (!ExportedTextures.Contains(TexturePath))
                        {
                            ExportedTextures.Add(TexturePath);
                            ++RawTextureCount;
                        }
                        TextureInfo->SetArrayField(TEXT("rawMips"), RawMips);
                    }
                    Textures.Add(MakeShared<FJsonValueObject>(TextureInfo));
                }
                Binding->SetArrayField(TEXT("textures"), Textures);
                Bindings.Add(MakeShared<FJsonValueObject>(Binding));
                if (Limit > 0 && Bindings.Num() >= Limit)
                {
                    break;
                }
            }
            if (Limit > 0 && Bindings.Num() >= Limit)
            {
                break;
            }
        }
        if (Limit > 0 && Bindings.Num() >= Limit)
        {
            break;
        }
    }
    Root->SetNumberField(TEXT("meshComponentsVisited"), MeshComponentCount);
    Root->SetNumberField(TEXT("lightmappedLods"), LightmappedComponentCount);
    Root->SetNumberField(TEXT("rawTextureCount"), RawTextureCount);
    Root->SetArrayField(TEXT("bindings"), Bindings);

    FString Json;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
    if (!FJsonSerializer::Serialize(Root, Writer) ||
        !IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true) ||
        !FFileHelper::SaveStringToFile(Json, *OutputPath))
    {
        UE_LOG(LogHELightmapBridge, Error, TEXT("Failed to write %s"), *OutputPath);
        return 4;
    }
    UE_LOG(LogHELightmapBridge, Display, TEXT("Exported %d lightmap bindings to %s"), Bindings.Num(), *OutputPath);
    return 0;
}
