// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITORONLY_DATA

#include "GaussianSplatTestUtils.h"

#include "GaussianSplat.h"

#include "Factories/Factory.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// Exercises UGaussianSplatFactory's binary .ply import path with small on-disk fixtures under
// TestData/Ply. UGaussianSplatFactory itself has no public header (it lives entirely in
// GaussianSplatEditor's Private folder), so these tests go through the same public UFactory
// entry point (ImportObject) that the editor's importer UI uses, having located the factory
// class generically via its SupportedClass (see GaussianSplatTestUtils::FindFactoryClassForAsset).
TEST_CLASS_WITH_FLAGS(FGaussianSplatFactoryImportTests, "GaussianSplat.Editor.Import", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
{
	TStrongObjectPtr<UFactory> Factory;

	BEFORE_EACH()
	{
		UClass* FactoryClass = GaussianSplatTests::FindFactoryClassForAsset(UGaussianSplat::StaticClass());
		ASSERT_THAT(IsNotNull(FactoryClass));

		Factory = TStrongObjectPtr<UFactory>(NewObject<UFactory>(GetTransientPackage(), FactoryClass));
		ASSERT_THAT(IsNotNull(Factory.Get()));
	}

	UObject* ImportFixture(const TCHAR* FixtureFileName, const TCHAR* ObjectBaseName)
	{
		const FString FilePath = GaussianSplatTests::GetPlyFixturePath(FixtureFileName);
		const FName ObjectName = MakeUniqueObjectName(GetTransientPackage(), UGaussianSplat::StaticClass(), ObjectBaseName);

		bool bOutCanceled = false;
		return Factory->ImportObject(UGaussianSplat::StaticClass(), GetTransientPackage(), ObjectName, RF_Transient, FilePath, TEXT(""), bOutCanceled);
	}

	// Baseline: proves the hand-authored fixture is actually a valid, importable file, so the
	// negative tests below are genuinely exercising truncation/corruption and not just a bad fixture.
	TEST_METHOD(MinimalPly_IsWellFormed_ImportsSuccessfully)
	{
		TStrongObjectPtr<UObject> Result(ImportFixture(TEXT("Minimal.ply"), TEXT("MinimalSplat")));

		ASSERT_THAT(IsNotNull(Result.Get()));

		UGaussianSplat* Splat = Cast<UGaussianSplat>(Result.Get());
		ASSERT_THAT(IsNotNull(Splat));
		ASSERT_THAT(AreEqual(4, Splat->GetNumSplats()));
	}

	// A .ply whose header declares more vertices than the file actually contains must fail
	// cleanly (return null, log an error) rather than reading past the end of the buffer.
	TEST_METHOD(TruncatedPly_FailsCleanlyWithMalformedFileError)
	{
		this->Assert.ExpectError(TEXT("Malformed PLY file"));

		TStrongObjectPtr<UObject> Result(ImportFixture(TEXT("Truncated.ply"), TEXT("TruncatedSplat")));

		ASSERT_THAT(IsNull(Result.Get()));
	}

	// A file that isn't PLY at all must be rejected at the very first header check.
	TEST_METHOD(CorruptPly_FailsCleanlyWithNotAValidPlyError)
	{
		this->Assert.ExpectError(TEXT("Not a valid PLY file"));

		TStrongObjectPtr<UObject> Result(ImportFixture(TEXT("Corrupt.ply"), TEXT("CorruptSplat")));

		ASSERT_THAT(IsNull(Result.Get()));
	}

	TEST_METHOD(ZeroVertexPly_FailsCleanlyWithEmptyAssetError)
	{
		this->Assert.ExpectError(TEXT("contains no splats"));
		TStrongObjectPtr<UObject> Result(ImportFixture(TEXT("ZeroVertex.ply"), TEXT("ZeroVertexSplat")));

		ASSERT_THAT(IsNull(Result.Get()));
	}
};

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITORONLY_DATA
