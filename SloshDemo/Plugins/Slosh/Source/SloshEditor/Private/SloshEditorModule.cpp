// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "Modules/ModuleManager.h"
#include "SloshPaintEdMode.h"

class FSloshEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FSloshPaintEdModeCommands::Register();
	}

	virtual void ShutdownModule() override
	{
		FSloshPaintEdModeCommands::Unregister();
	}
};

IMPLEMENT_MODULE(FSloshEditorModule, SloshEditor)
