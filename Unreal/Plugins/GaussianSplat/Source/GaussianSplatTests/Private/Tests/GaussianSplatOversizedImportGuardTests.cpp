// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITORONLY_DATA

#include "GaussianSplatBuilder.h"
#include "GaussianSplatCommon.h"
#include "GaussianSplatImportValidation.h"
#include "GaussianSplatLevelOfDetailBuilder.h"
#include "Misc/FeedbackContext.h"

namespace GaussianSplatTests
{
	// Capture the factory's user-facing diagnostics without forwarding expected failures to GLog.
	struct FImportFeedback : public FFeedbackContext
	{
		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Verbosity == ELogVerbosity::Error)
			{
				AddError(FString(Message));
			}
			else if (Verbosity == ELogVerbosity::Warning)
			{
				AddWarning(FString(Message));
			}
		}
	};
}

// Exercise the rejection function used by FactoryCreateBinary without allocating a multi-GB fixture.
TEST_CLASS_WITH_FLAGS(FGaussianSplatOversizedImportGuardTests, "GaussianSplat.Editor.OversizedImportGuard", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
{
	TEST_METHOD(GetMaxBufferBytes_IsFixedAtTheD3D12Limit_NotTheRuntimeRHILimit)
	{
		ASSERT_THAT(AreEqual(uint64(MAX_uint32), FGaussianSplatBuilder::GetMaxBufferBytes()));
	}

	TEST_METHOD(GetMaxSplatCount_EqualsBufferBudgetDividedByFormatStride)
	{
		// Independent format layout expectations, not values returned by the stride helper under test.
		const uint64 FloatStrides[] = { 56, 92, 152, 236 };
		const uint64 QuantizedStrides[] = { 20, 32, 52, 80 };
		for (int32 SHDegree = 0; SHDegree <= GaussianSplatCommon::MaxSupportedSHDegree; ++SHDegree)
		{
			const int32 SHDimension = GaussianSplatCommon::GetSHDimension(SHDegree);
			ASSERT_THAT(AreEqual(int64(uint64(MAX_uint32) / FloatStrides[SHDegree]), FGaussianSplatBuilder::GetMaxSplatCount(SHDimension, false)));
			ASSERT_THAT(AreEqual(int64(uint64(MAX_uint32) / QuantizedStrides[SHDegree]), FGaussianSplatBuilder::GetMaxSplatCount(SHDimension, true)));
		}
	}

	TEST_METHOD(BoundLevelOfDetailSplatCount_ClusterEdgesAndLargeCounts_ReturnExpectedBounds)
	{
		ASSERT_THAT(AreEqual(int64(0), GaussianSplat::BoundLevelOfDetailSplatCount(0, 16)));
		ASSERT_THAT(AreEqual(int64(36), GaussianSplat::BoundLevelOfDetailSplatCount(1, 16)));
		ASSERT_THAT(AreEqual(int64(66), GaussianSplat::BoundLevelOfDetailSplatCount(16, 16)));
		ASSERT_THAT(AreEqual(int64(70), GaussianSplat::BoundLevelOfDetailSplatCount(17, 16)));
		ASSERT_THAT(AreEqual(int64(2158), GaussianSplat::BoundLevelOfDetailSplatCount(1000, 16)));
		ASSERT_THAT(AreEqual(int64(4563402782LL), GaussianSplat::BoundLevelOfDetailSplatCount(MAX_int32, 16)));
	}

	TEST_METHOD(OversizedImportGuard_SplatCountWellUnderBudget_IsAccepted)
	{
		for (int32 SHDegree = 0; SHDegree <= GaussianSplatCommon::MaxSupportedSHDegree; ++SHDegree)
		{
			for (bool bQuantizedFormat : { false, true })
			{
				GaussianSplatTests::FImportFeedback Feedback;
				const FGaussianSplatImportSize ImportSize{ 1000, SHDegree, bQuantizedFormat };
				ASSERT_THAT(IsTrue(GaussianSplat::ValidateImportSize(ImportSize, "SmallSplat", Feedback)));
				ASSERT_THAT(AreEqual(0, Feedback.GetNumErrors()));
				ASSERT_THAT(AreEqual(0, Feedback.GetNumWarnings()));
			}
		}
	}

	TEST_METHOD(OversizedImportGuard_SourceFitsButLODsExceedBudget_IsRejectedWithError)
	{
		for (int32 SHDegree = 0; SHDegree <= GaussianSplatCommon::MaxSupportedSHDegree; ++SHDegree)
		{
			for (bool bQuantizedFormat : { false, true })
			{
				const int64 MaxSplatCount = FGaussianSplatBuilder::GetMaxSplatCount(GaussianSplatCommon::GetSHDimension(SHDegree), bQuantizedFormat);
				GaussianSplatTests::FImportFeedback Feedback;
				const FGaussianSplatImportSize ImportSize{ int32(MaxSplatCount), SHDegree, bQuantizedFormat };
				ASSERT_THAT(IsFalse(GaussianSplat::ValidateImportSize(ImportSize, "OversizedSplat", Feedback)));
				TArray<FString> Errors;
				Feedback.GetErrors(Errors);
				ASSERT_THAT(AreEqual(1, Errors.Num()));
				ASSERT_THAT(IsTrue(Errors[0].Contains(TEXT("Import failure. Asset OversizedSplat"))));
				ASSERT_THAT(IsTrue(Errors[0].Contains(TEXT("runtime splats across its LOD levels"))));
				ASSERT_THAT(IsTrue(Errors[0].Contains(TEXT("Consider importing with fewer splats or a lower SH value."))));
			}
		}
	}

	TEST_METHOD(OversizedImportGuard_LastAcceptedAndFirstRejected_AreAdjacent)
	{
		for (int32 SHDegree = 0; SHDegree <= GaussianSplatCommon::MaxSupportedSHDegree; ++SHDegree)
		{
			for (bool bQuantizedFormat : { false, true })
			{
				const int64 MaxSplatCount = FGaussianSplatBuilder::GetMaxSplatCount(GaussianSplatCommon::GetSHDimension(SHDegree), bQuantizedFormat);
				int32 Low = 1;
				int32 High = int32(MaxSplatCount);
				while (Low + 1 < High)
				{
					const int32 Middle = Low + (High - Low) / 2;
					if (GaussianSplat::BoundLevelOfDetailSplatCount(Middle, GaussianSplatCommon::MinSplatsPerCluster) <= MaxSplatCount)
					{
						Low = Middle;
					}
					else
					{
						High = Middle;
					}
				}

				GaussianSplatTests::FImportFeedback Feedback;
				ASSERT_THAT(IsTrue(GaussianSplat::ValidateImportSize({ Low, SHDegree, bQuantizedFormat }, "BoundarySplat", Feedback)));
				ASSERT_THAT(AreEqual(0, Feedback.GetNumErrors()));
				ASSERT_THAT(IsFalse(GaussianSplat::ValidateImportSize({ High, SHDegree, bQuantizedFormat }, "BoundarySplat", Feedback)));
				ASSERT_THAT(AreEqual(1, Feedback.GetNumErrors()));
			}
		}
	}
};

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITORONLY_DATA
