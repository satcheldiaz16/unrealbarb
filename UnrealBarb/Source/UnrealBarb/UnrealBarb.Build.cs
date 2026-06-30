using UnrealBuildTool;
using System.IO;
public class UnrealBarb : ModuleRules
{
    public UnrealBarb(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Json",
            "JsonUtilities",
            "Projects"
        });

        // Stage the Rhubarb binary + resources into packaged builds.
        string RhubarbDir = Path.Combine(PluginDirectory, "ThirdParty", "Rhubarb");
        if (Directory.Exists(RhubarbDir))
        {
            foreach (string FilePath in Directory.GetFiles(RhubarbDir, "*", SearchOption.AllDirectories))
            {
                RuntimeDependencies.Add(FilePath);
            }
        }
    }
}