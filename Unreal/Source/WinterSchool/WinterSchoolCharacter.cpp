// Copyright Epic Games, Inc. All Rights Reserved.

#include "WinterSchoolCharacter.h"
#include "Engine/LocalPlayer.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/Controller.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "WinterSchool.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarCameraStats(
	TEXT("ws.Debug.CameraStats"),
	!UE_BUILD_SHIPPING,
	TEXT("1 = log a camera smoothness summary every 2 s while mouse looking (default on outside Shipping): frames with and\n")
	TEXT("without mouse input, the spread of the per-frame look input, the longest frame and how often the camera boom's\n")
	TEXT("length jumped (collision probe hits)"));

AWinterSchoolCharacter::AWinterSchoolCharacter()
{
	// Set size for collision capsule
	GetCapsuleComponent()->InitCapsuleSize(42.f, 96.0f);
		
	// Don't rotate when the controller rotates. Let that just affect the camera.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	// Configure character movement
	GetCharacterMovement()->bOrientRotationToMovement = true;
	GetCharacterMovement()->RotationRate = FRotator(0.0f, 500.0f, 0.0f);

	// Note: For faster iteration times these variables, and many more, can be tweaked in the Character Blueprint
	// instead of recompiling to adjust them
	GetCharacterMovement()->JumpZVelocity = 500.f;
	GetCharacterMovement()->AirControl = 0.35f;
	GetCharacterMovement()->MaxWalkSpeed = 500.f;
	GetCharacterMovement()->MinAnalogWalkSpeed = 20.f;
	GetCharacterMovement()->BrakingDecelerationWalking = 2000.f;
	GetCharacterMovement()->BrakingDecelerationFalling = 1500.0f;

	// Create a camera boom (pulls in towards the player if there is a collision)
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 400.0f;
	CameraBoom->bUsePawnControlRotation = true;

	// Create a follow camera
	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	PrimaryActorTick.bCanEverTick = true;

	// Note: The skeletal mesh and anim blueprint references on the Mesh component (inherited from Character) 
	// are set in the derived blueprint asset named ThirdPersonCharacter (to avoid direct content references in C++)
}

void AWinterSchoolCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	// Set up action bindings
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent)) {
		
		// Jumping
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);

		// Moving
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AWinterSchoolCharacter::Move);
		EnhancedInputComponent->BindAction(MouseLookAction, ETriggerEvent::Triggered, this, &AWinterSchoolCharacter::MouseLook);

		// Looking
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AWinterSchoolCharacter::Look);
	}
	else
	{
		UE_LOG(LogWinterSchool, Error, TEXT("'%s' Failed to find an Enhanced Input component! This template is built to use the Enhanced Input system. If you intend to use the legacy system, then you will need to update this C++ file."), *GetNameSafe(this));
	}
}

void AWinterSchoolCharacter::Move(const FInputActionValue& Value)
{
	// input is a Vector2D
	FVector2D MovementVector = Value.Get<FVector2D>();

	// route the input
	DoMove(MovementVector.X, MovementVector.Y);
}

void AWinterSchoolCharacter::Look(const FInputActionValue& Value)
{
	// input is a Vector2D
	FVector2D LookAxisVector = Value.Get<FVector2D>();

	// route the input
	DoLook(LookAxisVector.X, LookAxisVector.Y);
}

void AWinterSchoolCharacter::MouseLook(const FInputActionValue& Value)
{
	const FVector2D LookAxisVector = Value.Get<FVector2D>() * MouseLookSensitivity;
	FrameMouseLook += LookAxisVector;
	DoLook(LookAxisVector.X, LookAxisVector.Y);
}

void AWinterSchoolCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!CVarCameraStats.GetValueOnGameThread() || !IsLocallyControlled())
	{
		FrameMouseLook = FVector2D::ZeroVector;
		return;
	}
	FCameraStats& S = CameraStats;
	const double Look = FrameMouseLook.Size();
	const bool bInput = Look > 0.0;
	FrameMouseLook = FVector2D::ZeroVector;
	S.Frames++;
	S.Time += DeltaSeconds;
	S.MaxDeltaTime = FMath::Max(S.MaxDeltaTime, double(DeltaSeconds));
	if (bInput)
	{
		S.InputFrames++;
		S.SumLook += Look;
		S.SumLookSq += Look * Look;
		S.MaxLook = FMath::Max(S.MaxLook, Look);
	}
	else if (S.bPrevInput)
	{
		S.GapFrames++;  // no input in a frame right after one with input: uneven delivery (or the end of a move)
	}
	S.bPrevInput = bInput;
	// The camera boom's actual length: its socket's distance from the boom's origin
	if (const USpringArmComponent* Boom = GetCameraBoom())
	{
		const float Length = (Boom->GetSocketLocation(USpringArmComponent::SocketName) - Boom->GetComponentLocation()).Size();
		S.MinBoom = FMath::Min(S.MinBoom, Length);
		S.MaxBoom = FMath::Max(S.MaxBoom, Length);
		S.BoomJumps += (S.LastBoom >= 0.0f && FMath::Abs(Length - S.LastBoom) > 20.0f) ? 1 : 0;
		S.LastBoom = Length;
	}
	if (S.Time >= 2.0)
	{
		if (S.InputFrames > 0)
		{
			const double Mean = S.SumLook / S.InputFrames;
			const double Std = FMath::Sqrt(FMath::Max(S.SumLookSq / S.InputFrames - Mean * Mean, 0.0));
			UE_LOG(LogWinterSchool, Display,
				TEXT("CameraStats: %d frames (%.1f fps, longest %.1f ms), mouse look in %d (%d gaps), per-frame look %.2f +- %.2f (max %.2f), boom %.0f-%.0f cm (%d jumps > 20 cm)"),
				S.Frames, S.Frames / S.Time, S.MaxDeltaTime * 1000.0, S.InputFrames, S.GapFrames, Mean, Std, S.MaxLook,
				S.MinBoom, S.MaxBoom, S.BoomJumps);
		}
		S = FCameraStats();
	}
}

void AWinterSchoolCharacter::DoMove(float Right, float Forward)
{
	if (GetController() != nullptr)
	{
		// find out which way is forward
		const FRotator Rotation = GetController()->GetControlRotation();
		const FRotator YawRotation(0, Rotation.Yaw, 0);

		// get forward vector
		const FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);

		// get right vector 
		const FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

		// add movement 
		AddMovementInput(ForwardDirection, Forward);
		AddMovementInput(RightDirection, Right);
	}
}

void AWinterSchoolCharacter::DoLook(float Yaw, float Pitch)
{
	if (GetController() != nullptr)
	{
		// add yaw and pitch input to controller
		AddControllerYawInput(Yaw);
		AddControllerPitchInput(Pitch);
	}
}

void AWinterSchoolCharacter::DoJumpStart()
{
	// signal the character to jump
	Jump();
}

void AWinterSchoolCharacter::DoJumpEnd()
{
	// signal the character to stop jumping
	StopJumping();
}
