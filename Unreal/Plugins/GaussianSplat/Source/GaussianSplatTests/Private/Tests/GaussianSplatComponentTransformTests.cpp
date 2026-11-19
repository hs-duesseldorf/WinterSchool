// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITORONLY_DATA

#include "GaussianSplatTestUtils.h"

#include "GaussianSplat.h"
#include "GaussianSplatBuilder.h"
#include "GaussianSplatComponent.h"
#include "GaussianSplatTransform.h"

#include "Components/ActorTestSpawner.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"

#include "Math/Box.h"
#include "Math/Matrix.h"
#include "Math/Rotator.h"
#include "Math/Transform.h"
#include "Math/Vector.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

// Exercise the production matrix helper shared by proxy creation and per-frame updates.
// GPU consumption still requires a rendered functional test.
TEST_CLASS_WITH_FLAGS(FGaussianSplatComponentTransformTests, "GaussianSplat.Component.Transform", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
{
	FActorTestSpawner Spawner;

	UGaussianSplatComponent* SpawnGaussianSplatComponent(AActor& Actor)
	{
		USceneComponent* Root = NewObject<USceneComponent>(&Actor, USceneComponent::StaticClass(), "Root", RF_Transient);
		Actor.SetRootComponent(Root);
		Root->RegisterComponent();

		UClass* GaussianSplatComponentClass = GaussianSplatTests::FindGaussianSplatComponentClass();
		if (GaussianSplatComponentClass == nullptr)
		{
			return nullptr;
		}

		USceneComponent* RawComponent = NewObject<USceneComponent>(&Actor, GaussianSplatComponentClass, NAME_None, RF_Transient);
		RawComponent->SetupAttachment(Root);
		RawComponent->RegisterComponent();

		return static_cast<UGaussianSplatComponent*>(RawComponent);
	}

	TEST_METHOD(GetComponentTransform_NonIdentityScaleAndRotation_TransformsAssetBoundsCorrectly)
	{
		FGaussianSplatBuilder Builder = GaussianSplatTests::MakeSyntheticBuilder(/*NumSplats=*/8, /*SHDegree=*/1);
		ASSERT_THAT(IsTrue(Builder.IsValid()));

		TStrongObjectPtr<UGaussianSplat> Asset(NewObject<UGaussianSplat>(GetTransientPackage(), NAME_None, RF_Transient));
		Asset->SetData(Builder);

		AActor& Actor = Spawner.SpawnActor<AActor>();
		UGaussianSplatComponent* Component = SpawnGaussianSplatComponent(Actor);
		ASSERT_THAT(IsNotNull(Component));
		Component->Asset = Asset.Get();

		const FTransform ActorTransform(
			FRotator(30.0, 45.0, 60.0),
			FVector(100.0, -50.0, 25.0),
			FVector(2.0, 3.0, 4.0));
		Actor.SetActorTransform(ActorTransform);

		const FMatrix44f ActualLocalToWorld = GaussianSplat::GetComponentLocalToWorld(*Component);
		const FBox3f ActualWorldBounds = Asset->GetBounds().TransformBy(ActualLocalToWorld);

		// Independently-computed ground truth: transform the local bounds in double precision via the
		// same FTransform applied to the actor, rather than reusing the component's cached matrix.
		const FBox LocalBoundsDouble(FVector(Asset->GetBounds().Min), FVector(Asset->GetBounds().Max));
		const FBox ExpectedWorldBoundsDouble = LocalBoundsDouble.TransformBy(ActorTransform);

		// Generous epsilon: the render path itself narrows to float (FMatrix44f) before transforming
		// bounds, so some precision loss relative to the double-precision ground truth is expected.
		constexpr float Epsilon = 0.1f;
		ASSERT_THAT(IsNear(float(ExpectedWorldBoundsDouble.Min.X), ActualWorldBounds.Min.X, Epsilon));
		ASSERT_THAT(IsNear(float(ExpectedWorldBoundsDouble.Min.Y), ActualWorldBounds.Min.Y, Epsilon));
		ASSERT_THAT(IsNear(float(ExpectedWorldBoundsDouble.Min.Z), ActualWorldBounds.Min.Z, Epsilon));
		ASSERT_THAT(IsNear(float(ExpectedWorldBoundsDouble.Max.X), ActualWorldBounds.Max.X, Epsilon));
		ASSERT_THAT(IsNear(float(ExpectedWorldBoundsDouble.Max.Y), ActualWorldBounds.Max.Y, Epsilon));
		ASSERT_THAT(IsNear(float(ExpectedWorldBoundsDouble.Max.Z), ActualWorldBounds.Max.Z, Epsilon));
	}

	TEST_METHOD(GetComponentTransform_IdentityTransform_MatchesAssetLocalBounds)
	{
		FGaussianSplatBuilder Builder = GaussianSplatTests::MakeSyntheticBuilder(/*NumSplats=*/8, /*SHDegree=*/1);
		TStrongObjectPtr<UGaussianSplat> Asset(NewObject<UGaussianSplat>(GetTransientPackage(), NAME_None, RF_Transient));
		Asset->SetData(Builder);

		AActor& Actor = Spawner.SpawnActor<AActor>();
		UGaussianSplatComponent* Component = SpawnGaussianSplatComponent(Actor);
		ASSERT_THAT(IsNotNull(Component));
		Component->Asset = Asset.Get();

		const FMatrix44f IdentityLocalToWorld = GaussianSplat::GetComponentLocalToWorld(*Component);
		ASSERT_THAT(IsTrue(IdentityLocalToWorld == FMatrix44f::Identity));
		const FBox3f WorldBounds = Asset->GetBounds().TransformBy(IdentityLocalToWorld);

		// TransformBy reconstructs min/max through center and extent, which can round even for identity.
		constexpr float Epsilon = 0.00001f;
		ASSERT_THAT(IsNear(Asset->GetBounds().Min.X, WorldBounds.Min.X, Epsilon));
		ASSERT_THAT(IsNear(Asset->GetBounds().Min.Y, WorldBounds.Min.Y, Epsilon));
		ASSERT_THAT(IsNear(Asset->GetBounds().Min.Z, WorldBounds.Min.Z, Epsilon));
		ASSERT_THAT(IsNear(Asset->GetBounds().Max.X, WorldBounds.Max.X, Epsilon));
		ASSERT_THAT(IsNear(Asset->GetBounds().Max.Y, WorldBounds.Max.Y, Epsilon));
		ASSERT_THAT(IsNear(Asset->GetBounds().Max.Z, WorldBounds.Max.Z, Epsilon));
	}

	TEST_METHOD(GetComponentLocalToWorld_RelativeOffsetAndParentUpdate_TracksWorldTransform)
	{
		AActor& Actor = Spawner.SpawnActor<AActor>();
		UGaussianSplatComponent* Component = SpawnGaussianSplatComponent(Actor);
		ASSERT_THAT(IsNotNull(Component));

		const FVector RelativeOffset(10.0, -20.0, 5.0);
		Component->SetRelativeLocation(RelativeOffset);

		const FTransform ParentTransforms[] =
		{
			FTransform(FRotator(30.0, 45.0, 60.0), FVector(100.0, -50.0, 25.0), FVector(2.0, 3.0, 4.0)),
			FTransform(FRotator(-15.0, 90.0, 10.0), FVector(-75.0, 30.0, 80.0), FVector(4.0, 2.0, 3.0))
		};
		const FVector LocalPoints[] =
		{
			FVector::ZeroVector,
			FVector(1.0, 0.0, 0.0),
			FVector(0.0, 1.0, 0.0),
			FVector(0.0, 0.0, 1.0)
		};
		for (const FTransform& ParentTransform : ParentTransforms)
		{
			Actor.SetActorTransform(ParentTransform);
			const FMatrix44f LocalToWorld = GaussianSplat::GetComponentLocalToWorld(*Component);
			for (const FVector& LocalPoint : LocalPoints)
			{
				const FVector Expected = ParentTransform.TransformPosition(LocalPoint + RelativeOffset);
				const FVector3f Actual(LocalToWorld.TransformPosition(FVector3f(LocalPoint)));
				ASSERT_THAT(IsNear(float(Expected.X), Actual.X, 0.001f));
				ASSERT_THAT(IsNear(float(Expected.Y), Actual.Y, 0.001f));
				ASSERT_THAT(IsNear(float(Expected.Z), Actual.Z, 0.001f));
			}
		}
	}
};

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITORONLY_DATA
