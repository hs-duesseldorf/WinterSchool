// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatComponent.h"
#include "Modules/ModuleManager.h"

#include "GaussianSplatRenderer.h"

#include "Engine/World.h"
#include "Misc/App.h"
#include "SceneInterface.h"

#define LOCTEXT_NAMESPACE "GaussianSplatsActor"

UGaussianSplatComponent::UGaussianSplatComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, PointScaleFactor(1.0f)
	, OpacityScale(1.0f)
	, MaxSHDegree(3)
	, bInvertToneCurve(false)
	, InverseToneCurveStrength(1.0f)
	, bInvertExpandGamut(true)
	, LightingStrength(1.0f)
	, ShadowStrength(0.75f)
{
	CastShadow = true; // like a mesh (UPrimitiveComponent defaults to false)
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	SetCanEverAffectNavigation(false);
	bAffectDistanceFieldLighting = false;
	bAffectDynamicIndirectLighting = false;
}

FBoxSphereBounds UGaussianSplatComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	const FBox3f Bounds = IsValid(Asset) ? Asset->GetBounds() : FBox3f(EForceInit::ForceInit);
	return Bounds.IsValid ? FBoxSphereBounds(FBox(Bounds)).TransformBy(LocalToWorld) : FBoxSphereBounds(LocalToWorld.GetLocation(), FVector::ZeroVector, 0.0);
}

bool UGaussianSplatComponent::HasValidSplatData() const
{
	return IsValid(Asset) && (Asset->GetNumSplats() > 0);
}

void UGaussianSplatComponent::OnRegister()
{
	Super::OnRegister();

	if (IsTemplate() || !FApp::CanEverRender())
	{
		return;
	}

	const UWorld* World = GetWorld();
	if (!World || !World->Scene)
	{
		return;
	}

	const EShaderPlatform Platform = World->Scene->GetShaderPlatform();
	ActiveRenderer = FGaussianSplatRendererRegistry::Get().SelectRenderer(Platform);
	if (ActiveRenderer)
	{
		ActiveRenderer->OnComponentRegistered(this);
	}
}

void UGaussianSplatComponent::OnUnregister()
{
	Super::OnUnregister();

	if (ActiveRenderer)
	{
		ActiveRenderer->OnComponentUnregistered(this);
		ActiveRenderer = nullptr;
	}
}

#undef LOCTEXT_NAMESPACE
