// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITORONLY_DATA

#include "GaussianSplatTestUtils.h"

#include "GaussianSplat.h"
#include "GaussianSplatBuilder.h"

#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

// UGaussianSplat::BuildDerivedDataKey() hashes the raw bytes of the editor-side splat arrays
// (FGaussianSplatFormatFloat::FBaseData / SHData) via FBlake3. FBaseData carries two Padding
// fields purely to keep the struct GPU-alignment friendly; a previous bug let uninitialized
// padding bytes leak into that hash, making the derived-data key nondeterministic for otherwise
// identical source data (two imports of the same file could disagree on cache key, defeating DDC
// reuse). These tests guard against that regressing.
TEST_CLASS_WITH_FLAGS(FGaussianSplatDerivedDataKeyTests, "GaussianSplat.Editor.DerivedDataKey", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
{
	TEST_METHOD(BuildDerivedDataKey_SameAssetRebuiltTwice_ReturnsIdenticalKey)
	{
		FGaussianSplatBuilder Builder = GaussianSplatTests::MakeSyntheticBuilder(/*NumSplats=*/8, /*SHDegree=*/1);
		ASSERT_THAT(IsTrue(Builder.IsValid()));

		TStrongObjectPtr<UGaussianSplat> Asset(NewObject<UGaussianSplat>(GetTransientPackage(), NAME_None, RF_Transient));
		Asset->SetData(Builder);

		const FString FirstKey = Asset->BuildDerivedDataKeyForAutomationTest();

		// SetData consumes the source arrays, so rebuild from independently generated data.
		FGaussianSplatBuilder SecondBuilder = GaussianSplatTests::MakeSyntheticBuilder(/*NumSplats=*/8, /*SHDegree=*/1);
		ASSERT_THAT(IsTrue(SecondBuilder.IsValid()));
		Asset->SetData(SecondBuilder);
		const FString SecondKey = Asset->BuildDerivedDataKeyForAutomationTest();

		ASSERT_THAT(AreEqual(FirstKey, SecondKey));
	}

	TEST_METHOD(BuildDerivedDataKey_TwoAssetsFromIdenticalSourceData_ReturnIdenticalKeys)
	{
		FGaussianSplatBuilder BuilderA = GaussianSplatTests::MakeSyntheticBuilder(/*NumSplats=*/8, /*SHDegree=*/1);
		FGaussianSplatBuilder BuilderB = GaussianSplatTests::MakeSyntheticBuilder(/*NumSplats=*/8, /*SHDegree=*/1);
		ASSERT_THAT(IsTrue(BuilderA.IsValid()));
		ASSERT_THAT(IsTrue(BuilderB.IsValid()));

		TStrongObjectPtr<UGaussianSplat> AssetA(NewObject<UGaussianSplat>(GetTransientPackage(), NAME_None, RF_Transient));
		TStrongObjectPtr<UGaussianSplat> AssetB(NewObject<UGaussianSplat>(GetTransientPackage(), NAME_None, RF_Transient));

		// SetData() moves the builder's arrays out, so each asset needs its own builder instance
		// even though the source content is identical.
		AssetA->SetData(BuilderA);
		AssetB->SetData(BuilderB);

		const FString KeyA = AssetA->BuildDerivedDataKeyForAutomationTest();
		const FString KeyB = AssetB->BuildDerivedDataKeyForAutomationTest();

		ASSERT_THAT(AreEqual(KeyA, KeyB));
	}

	// The key appends NumSplats, SHDegree, and the format/cluster/LOD settings as plain text; only the
	// BaseData and SHData bytes go through the hash. Varying the count would therefore change the key
	// even with the hash removed, so these sanity checks hold count and degree fixed and mutate one
	// element of each hashed array instead. Opacity is used for BaseData so Bounds stay identical too.
	TEST_METHOD(BuildDerivedDataKey_SameCountDifferentBaseData_ReturnsDifferentKeys)
	{
		FGaussianSplatBuilder BuilderA = GaussianSplatTests::MakeSyntheticBuilder(/*NumSplats=*/8, /*SHDegree=*/1);
		FGaussianSplatBuilder BuilderB = GaussianSplatTests::MakeSyntheticBuilder(/*NumSplats=*/8, /*SHDegree=*/1);
		BuilderB.BaseData[3].Opacity += 0.25f;

		TStrongObjectPtr<UGaussianSplat> AssetA(NewObject<UGaussianSplat>(GetTransientPackage(), NAME_None, RF_Transient));
		TStrongObjectPtr<UGaussianSplat> AssetB(NewObject<UGaussianSplat>(GetTransientPackage(), NAME_None, RF_Transient));
		AssetA->SetData(BuilderA);
		AssetB->SetData(BuilderB);

		ASSERT_THAT(AreNotEqual(AssetA->BuildDerivedDataKeyForAutomationTest(), AssetB->BuildDerivedDataKeyForAutomationTest()));
	}

	TEST_METHOD(BuildDerivedDataKey_SameCountDifferentSHData_ReturnsDifferentKeys)
	{
		FGaussianSplatBuilder BuilderA = GaussianSplatTests::MakeSyntheticBuilder(/*NumSplats=*/8, /*SHDegree=*/1);
		FGaussianSplatBuilder BuilderB = GaussianSplatTests::MakeSyntheticBuilder(/*NumSplats=*/8, /*SHDegree=*/1);
		BuilderB.SHData[5].X += 0.25f;

		TStrongObjectPtr<UGaussianSplat> AssetA(NewObject<UGaussianSplat>(GetTransientPackage(), NAME_None, RF_Transient));
		TStrongObjectPtr<UGaussianSplat> AssetB(NewObject<UGaussianSplat>(GetTransientPackage(), NAME_None, RF_Transient));
		AssetA->SetData(BuilderA);
		AssetB->SetData(BuilderB);

		ASSERT_THAT(AreNotEqual(AssetA->BuildDerivedDataKeyForAutomationTest(), AssetB->BuildDerivedDataKeyForAutomationTest()));
	}
};

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITORONLY_DATA
