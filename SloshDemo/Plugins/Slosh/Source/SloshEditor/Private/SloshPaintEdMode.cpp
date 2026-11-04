// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshPaintEdMode.h"
#include "SloshPaintTool.h"
#include "InteractiveToolManager.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "SloshPaintEdMode"

const FEditorModeID USloshPaintEdMode::ModeId = TEXT("EM_SloshPaint");

FSloshPaintEdModeCommands::FSloshPaintEdModeCommands()
	: TCommands<FSloshPaintEdModeCommands>("SloshPaintEdMode", LOCTEXT("CommandsName", "Slosh Paint Mode"), NAME_None, FAppStyle::GetAppStyleSetName())
{
}

void FSloshPaintEdModeCommands::RegisterCommands()
{
	TArray<TSharedPtr<FUICommandInfo>>& ToolCommands = Commands.FindOrAdd(NAME_Default);
	UI_COMMAND(PaintTool, "Paint", "Paint liquid strokes on surfaces (drag). Ctrl+Z undoes a stroke.", EUserInterfaceActionType::ToggleButton, FInputChord());
	ToolCommands.Add(PaintTool);
}

TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> FSloshPaintEdModeCommands::GetCommands()
{
	return FSloshPaintEdModeCommands::Get().Commands;
}

USloshPaintEdMode::USloshPaintEdMode()
{
	Info = FEditorModeInfo(ModeId, LOCTEXT("ModeName", "Slosh Paint"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.MeshPaintMode", "LevelEditor.MeshPaintMode.Small"), true);
}

void USloshPaintEdMode::Enter()
{
	UEdMode::Enter();
	RegisterTool(FSloshPaintEdModeCommands::Get().PaintTool, TEXT("SloshPaintTool"), NewObject<USloshPaintToolBuilder>(this));
	GetToolManager()->SelectActiveToolType(EToolSide::Left, TEXT("SloshPaintTool"));
	GetToolManager()->ActivateTool(EToolSide::Left);
}

void USloshPaintEdMode::CreateToolkit()
{
	Toolkit = MakeShareable(new FSloshPaintEdModeToolkit);
}

TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> USloshPaintEdMode::GetModeCommands() const
{
	return FSloshPaintEdModeCommands::GetCommands();
}

#undef LOCTEXT_NAMESPACE
