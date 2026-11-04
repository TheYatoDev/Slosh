// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshPaintTool.h"
#include "SloshPaintComponent.h"
#include "SloshPaintPreset.h"
#include "BaseBehaviors/ClickDragBehavior.h"
#include "BaseBehaviors/MouseHoverBehavior.h"
#include "InteractiveToolManager.h"
#include "ToolContextInterfaces.h"
#include "SceneManagement.h"
#include "Engine/World.h"

#define LOCTEXT_NAMESPACE "SloshPaintTool"

UInteractiveTool* USloshPaintToolBuilder::BuildTool(const FToolBuilderState& SceneState) const
{
	USloshPaintTool* Tool = NewObject<USloshPaintTool>(SceneState.ToolManager);
	Tool->SetWorld(SceneState.World);
	return Tool;
}

void USloshPaintTool::Setup()
{
	UInteractiveTool::Setup();

	UClickDragInputBehavior* ClickDrag = NewObject<UClickDragInputBehavior>(this);
	ClickDrag->Initialize(this);
	AddInputBehavior(ClickDrag);

	UMouseHoverBehavior* HoverBehavior = NewObject<UMouseHoverBehavior>(this);
	HoverBehavior->Initialize(this);
	AddInputBehavior(HoverBehavior);

	Properties = NewObject<USloshPaintToolProperties>(this);
	Properties->RestoreProperties(this);
	AddToolPropertySource(Properties);

	GetToolManager()->DisplayMessage(
		LOCTEXT("Startup", "Drag on any surface to paint liquid. Pick a preset under Brush. Ctrl+Z undoes the last stroke."),
		EToolMessageLevel::UserNotification);
}

void USloshPaintTool::Shutdown(EToolShutdownType ShutdownType)
{
	FinishStroke();
	Properties->SaveProperties(this);
}

float USloshPaintTool::BrushRadius() const
{
	const USloshPaintPreset* Preset = Properties ? Properties->Preset.Get() : nullptr;
	return Preset ? Preset->Thickness * Preset->GetUnitCm() * Properties->Pressure : 1.0f;
}

float USloshPaintTool::BrushSpacing() const
{
	if (Properties && Properties->Spacing > 0.0f)
	{
		return Properties->Spacing;
	}
	return FMath::Max(0.5f * BrushRadius(), 0.02f);
}

FSloshPaintHit USloshPaintTool::Trace(const FInputDeviceRay& Ray) const
{
	UWorld* TraceWorld = World.Get();
	if (!TraceWorld)
	{
		return FSloshPaintHit();
	}
	const FVector Start = Ray.WorldRay.Origin;
	const FVector End = Start + Ray.WorldRay.Direction * 1.0e6;
	return USloshPaintLibrary::TracePaintSurface(TraceWorld, Start, End, ECC_Visibility, TArray<AActor*>(),
		Properties ? Properties->bPaintSkinnedWithoutCollision : true);
}

FInputRayHit USloshPaintTool::CanBeginClickDragSequence(const FInputDeviceRay& PressPos)
{
	const FSloshPaintHit Hit = Trace(PressPos);
	return Hit.bHit ? FInputRayHit(Hit.Distance, Hit.Normal) : FInputRayHit();
}

void USloshPaintTool::OnClickPress(const FInputDeviceRay& PressPos)
{
	if (!Properties || !Properties->Preset)
	{
		GetToolManager()->DisplayMessage(LOCTEXT("NoPreset", "Choose a liquid preset first (Brush > Preset)."), EToolMessageLevel::UserWarning);
		return;
	}
	GetToolManager()->BeginUndoTransaction(LOCTEXT("PaintStroke", "Slosh Paint Stroke"));
	bInTransaction = true;
	PaintAt(Trace(PressPos));
}

void USloshPaintTool::OnClickDrag(const FInputDeviceRay& DragPos)
{
	if (bInTransaction)
	{
		const FSloshPaintHit Hit = Trace(DragPos);
		Hover = Hit;
		bHoverValid = Hit.bHit;
		PaintAt(Hit);
	}
}

void USloshPaintTool::OnClickRelease(const FInputDeviceRay& ReleasePos)
{
	FinishStroke();
}

void USloshPaintTool::OnTerminateDragSequence()
{
	FinishStroke();
}

void USloshPaintTool::PaintAt(const FSloshPaintHit& Hit)
{
	if (!Hit.bHit || !Hit.Component)
	{
		return;
	}
	if (ActiveTarget.Get() != Hit.Component || !ActivePaint.IsValid())
	{
		// New surface: end the current stroke and start one on the new target (same undo transaction).
		if (USloshPaintComponent* Previous = ActivePaint.Get())
		{
			Previous->EndStroke(ActiveStroke);
		}
		if (AActor* Owner = Hit.Component->GetOwner())
		{
			Owner->Modify();
		}
		USloshPaintComponent* Paint = USloshPaintComponent::FindOrAddPaintComponent(Hit.Component);
		if (!Paint)
		{
			return;
		}
		Paint->Modify();
		ActivePaint = Paint;
		ActiveTarget = Hit.Component;
		ActiveStroke = Paint->BeginStroke(Properties->Preset, Hit.Location, Hit.Normal, Properties->Pressure, Hit.Bone);
		LastPoint = Hit.Location;
		return;
	}
	if (FVector::Distance(Hit.Location, LastPoint) >= BrushSpacing())
	{
		ActivePaint->AddStrokePoint(ActiveStroke, Hit.Location, Hit.Normal, Properties->Pressure, Hit.Bone);
		LastPoint = Hit.Location;
	}
}

void USloshPaintTool::FinishStroke()
{
	if (USloshPaintComponent* Paint = ActivePaint.Get())
	{
		Paint->EndStroke(ActiveStroke);
	}
	ActivePaint.Reset();
	ActiveTarget.Reset();
	ActiveStroke = 0;
	if (bInTransaction)
	{
		GetToolManager()->EndUndoTransaction();
		bInTransaction = false;
	}
}

FInputRayHit USloshPaintTool::BeginHoverSequenceHitTest(const FInputDeviceRay& PressPos)
{
	const FSloshPaintHit Hit = Trace(PressPos);
	return Hit.bHit ? FInputRayHit(Hit.Distance) : FInputRayHit();
}

bool USloshPaintTool::OnUpdateHover(const FInputDeviceRay& DevicePos)
{
	Hover = Trace(DevicePos);
	bHoverValid = Hover.bHit;
	return true;
}

void USloshPaintTool::Render(IToolsContextRenderAPI* RenderAPI)
{
	if (!bHoverValid || !RenderAPI)
	{
		return;
	}
	FPrimitiveDrawInterface* PDI = RenderAPI->GetPrimitiveDrawInterface();
	const FVector N = Hover.Normal.GetSafeNormal();
	FVector X, Y;
	N.FindBestAxisVectors(X, Y);
	const FLinearColor Color = Properties && Properties->Preset ? FLinearColor(0.2f, 0.8f, 1.0f) : FLinearColor(1.0f, 0.3f, 0.2f);
	const float Radius = BrushRadius();
	DrawCircle(PDI, Hover.Location + N * 0.05, X, Y, Color, Radius, 24, SDPG_Foreground, 0.5f);
	// Fixed-size reticle so the brush stays visible when the bead is millimetre-sized.
	DrawCircle(PDI, Hover.Location + N * 0.05, X, Y, Color.CopyWithNewOpacity(0.5f), FMath::Max(Radius * 4.0f, 2.0f), 32, SDPG_Foreground, 0.25f);
	PDI->DrawLine(Hover.Location, Hover.Location + N * FMath::Max(Radius * 6.0f, 3.0f), Color, SDPG_Foreground, 0.5f);
}

#undef LOCTEXT_NAMESPACE
