// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

class FSloshCoreModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Shared by SloshCore and SloshRender; both modules load at PostConfigInit.
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Slosh"));
		check(Plugin.IsValid());
		const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/Slosh"), ShaderDir);
	}
};

IMPLEMENT_MODULE(FSloshCoreModule, SloshCore)
