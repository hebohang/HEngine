using UnrealBuildTool;

public class HELightmapBridge : ModuleRules
{
    public HELightmapBridge(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core", "CoreUObject", "Engine", "Json", "UnrealEd"
        });
    }
}
