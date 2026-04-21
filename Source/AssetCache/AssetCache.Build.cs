using UnrealBuildTool;

public class AssetCache : ModuleRules
{
	public AssetCache(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"SDFutureExtensions",
			"StructUtils",
			"DeveloperSettings",
		});
	}
}
