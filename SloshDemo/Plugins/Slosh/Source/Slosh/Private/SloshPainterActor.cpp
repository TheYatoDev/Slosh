// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshPainterActor.h"
#include "SloshPaintComponent.h"
#include "SloshPaintPreset.h"
#include "Components/ArrowComponent.h"
#include "Components/InputComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"

ASloshPainterActor::ASloshPainterActor()
{
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Brush = CreateDefaultSubobject<USloshPaintBrushComponent>(TEXT("Brush"));
	Brush->SetupAttachment(RootComponent);
	Brush->Aim = ESloshPaintAim::MouseCursor;
#if WITH_EDITORONLY_DATA
	if (UArrowComponent* Arrow = CreateEditorOnlyDefaultSubobject<UArrowComponent>(TEXT("AimArrow")))
	{
		Arrow->SetupAttachment(Brush);   // ComponentForward paints along this
		Arrow->ArrowColor = FColor(255, 160, 40);
		Arrow->ArrowSize = 0.5f;
	}
#endif
}

void ASloshPainterActor::BeginPlay()
{
	Super::BeginPlay();
	Brush->OnStrokeFinished.AddDynamic(this, &ASloshPainterActor::HandleStrokeFinished);
	if (!Presets.IsEmpty() && !Brush->Preset)
	{
		SelectPreset(0);
	}

	if (bMouseInput)
	{
		if (APlayerController* Controller = UGameplayStatics::GetPlayerController(this, 0))
		{
			if (Brush->Aim == ESloshPaintAim::MouseCursor)
			{
				Controller->SetShowMouseCursor(true);
				Brush->AimController = Controller;
			}
			EnableInput(Controller);
			InputComponent->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &ASloshPainterActor::PaintPressed);
			InputComponent->BindKey(EKeys::LeftMouseButton, IE_Released, this, &ASloshPainterActor::PaintReleased);
			InputComponent->BindKey(EKeys::MouseScrollUp, IE_Pressed, this, &ASloshPainterActor::WheelUp);
			InputComponent->BindKey(EKeys::MouseScrollDown, IE_Pressed, this, &ASloshPainterActor::WheelDown);
			InputComponent->BindKey(EKeys::Tab, IE_Pressed, this, &ASloshPainterActor::NextPreset);
			InputComponent->BindKey(EKeys::Z, IE_Pressed, this, &ASloshPainterActor::UndoLastStroke);
			const FKey Digits[9] = { EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five, EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine };
			for (int32 I = 0; I < 9; ++I)
			{
				FInputKeyBinding Binding(FInputChord(Digits[I]), IE_Pressed);
				Binding.KeyDelegate.GetDelegateForManualSet().BindWeakLambda(this, [this, I]() { SelectPreset(I); });
				InputComponent->KeyBindings.Add(Binding);
			}
		}
	}
	if (bAutoPaint)
	{
		Brush->StartPainting();
	}
}

void ASloshPainterActor::SelectPreset(int32 Index)
{
	if (Presets.IsValidIndex(Index) && Presets[Index])
	{
		PresetIndex = Index;
		const bool bWasPainting = Brush->IsPainting();
		Brush->StopPainting();   // a stroke has one preset
		Brush->Preset = Presets[Index];
		if (bWasPainting)
		{
			Brush->StartPainting();
		}
	}
}

void ASloshPainterActor::NextPreset()
{
	if (!Presets.IsEmpty())
	{
		SelectPreset((PresetIndex + 1) % Presets.Num());
	}
}

void ASloshPainterActor::HandleStrokeFinished(USloshPaintComponent* PaintComponent, int32 StrokeId)
{
	History.Emplace(PaintComponent, StrokeId);
}

void ASloshPainterActor::UndoLastStroke()
{
	while (!History.IsEmpty())
	{
		const TPair<TWeakObjectPtr<USloshPaintComponent>, int32> Last = History.Pop();
		if (USloshPaintComponent* Paint = Last.Key.Get(); Paint && Paint->RemoveStroke(Last.Value))
		{
			return;
		}
	}
}
