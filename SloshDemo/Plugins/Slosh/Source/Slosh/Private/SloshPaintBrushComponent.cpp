// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshPaintBrushComponent.h"
#include "SloshPaintComponent.h"
#include "SloshPaintPreset.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

USloshPaintBrushComponent::USloshPaintBrushComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void USloshPaintBrushComponent::StartPainting()
{
	bPainting = true;
}

void USloshPaintBrushComponent::StopPainting()
{
	bPainting = false;
	FinishStroke();
}

float USloshPaintBrushComponent::EffectiveSpacing() const
{
	if (Spacing > 0.0f)
	{
		return Spacing;
	}
	// Points closer than the resampling spacing add nothing; half of it keeps curves smooth.
	return Preset ? FMath::Max(0.5f * Preset->Thickness * Preset->GetUnitCm(), 0.02f) : 0.5f;
}

bool USloshPaintBrushComponent::GetAimRay(FVector& OutStart, FVector& OutEnd) const
{
	if (Aim == ESloshPaintAim::ComponentForward)
	{
		OutStart = GetComponentLocation();
		OutEnd = OutStart + GetForwardVector() * MaxDistance;
		return true;
	}
	APlayerController* Controller = AimController ? AimController.Get() : UGameplayStatics::GetPlayerController(this, 0);
	if (!Controller)
	{
		return false;
	}
	FVector Direction;
	bool bOk = false;
	if (Aim == ESloshPaintAim::MouseCursor)
	{
		bOk = Controller->DeprojectMousePositionToWorld(OutStart, Direction);
	}
	else
	{
		int32 SizeX = 0, SizeY = 0;
		Controller->GetViewportSize(SizeX, SizeY);
		bOk = Controller->DeprojectScreenPositionToWorld(SizeX * 0.5f, SizeY * 0.5f, OutStart, Direction);
	}
	OutEnd = OutStart + Direction * MaxDistance;
	return bOk;
}

FSloshPaintHit USloshPaintBrushComponent::PaintAlongRay(FVector Start, FVector End)
{
	TArray<AActor*> Ignore;
	if (Aim == ESloshPaintAim::ComponentForward && GetOwner())
	{
		Ignore.Add(GetOwner());
	}
	const FSloshPaintHit Hit = USloshPaintLibrary::TracePaintSurface(this, Start, End, TraceChannel, Ignore, bPaintSkinnedWithoutCollision);
	if (!Hit.bHit || !Hit.Component || !Preset)
	{
		FinishStroke();
		return Hit;
	}
	if (CurrentTarget.Get() != Hit.Component || !CurrentPaint.IsValid())
	{
		FinishStroke();
		USloshPaintComponent* Paint = USloshPaintComponent::FindOrAddPaintComponent(Hit.Component);
		if (!Paint)
		{
			return Hit;
		}
		CurrentPaint = Paint;
		CurrentTarget = Hit.Component;
		CurrentStroke = Paint->BeginStroke(Preset, Hit.Location, Hit.Normal, Pressure, Hit.Bone);
		LastPoint = Hit.Location;
		OnStrokeStarted.Broadcast(Paint, CurrentStroke);
		return Hit;
	}
	if (FVector::Distance(Hit.Location, LastPoint) >= EffectiveSpacing())
	{
		CurrentPaint->AddStrokePoint(CurrentStroke, Hit.Location, Hit.Normal, Pressure, Hit.Bone);
		LastPoint = Hit.Location;
	}
	return Hit;
}

void USloshPaintBrushComponent::FinishStroke()
{
	if (USloshPaintComponent* Paint = CurrentPaint.Get(); Paint && CurrentStroke != 0)
	{
		Paint->EndStroke(CurrentStroke);
		OnStrokeFinished.Broadcast(Paint, CurrentStroke);
	}
	CurrentPaint.Reset();
	CurrentTarget.Reset();
	CurrentStroke = 0;
}

void USloshPaintBrushComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!bPainting)
	{
		return;
	}
	FVector Start, End;
	if (GetAimRay(Start, End))
	{
		PaintAlongRay(Start, End);
	}
}
