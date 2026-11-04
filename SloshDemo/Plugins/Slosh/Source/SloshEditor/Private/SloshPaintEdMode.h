// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "Tools/UEdMode.h"
#include "Toolkits/BaseToolkit.h"
#include "Framework/Commands/Commands.h"
#include "SloshPaintEdMode.generated.h"

/** Editor commands of the Slosh Paint mode. */
class FSloshPaintEdModeCommands : public TCommands<FSloshPaintEdModeCommands>
{
public:
	FSloshPaintEdModeCommands();
	virtual void RegisterCommands() override;
	static TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> GetCommands();

	TSharedPtr<FUICommandInfo> PaintTool;

protected:
	TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> Commands;
};

/** "Slosh Paint" editor mode: paint Fluid Painter-style liquid strokes onto any surface in the level. */
UCLASS()
class USloshPaintEdMode : public UEdMode
{
	GENERATED_BODY()

public:
	static const FEditorModeID ModeId;

	USloshPaintEdMode();

	virtual void Enter() override;
	virtual void CreateToolkit() override;
	virtual TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> GetModeCommands() const override;
};

/** Mode panel: tool palette plus the active tool's settings. */
class FSloshPaintEdModeToolkit : public FModeToolkit
{
public:
	virtual void GetToolPaletteNames(TArray<FName>& PaletteNames) const override { PaletteNames.Add(NAME_Default); }
	virtual FName GetToolkitFName() const override { return FName("SloshPaintEdMode"); }
	virtual FText GetBaseToolkitName() const override { return NSLOCTEXT("SloshPaint", "ToolkitName", "Slosh Paint"); }
};
