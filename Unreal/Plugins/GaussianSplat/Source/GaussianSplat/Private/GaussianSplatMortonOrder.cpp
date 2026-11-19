// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatMortonOrder.h"

#include "GaussianSplatBuilder.h"

#include "Algo/Sort.h"
#include "Async/ParallelFor.h"

#if WITH_EDITORONLY_DATA

namespace GaussianSplat
{
	void BuildMortonOrder(
		TConstArrayView<FGaussianSplatFormatFloat::FBaseData> BaseData, 
		TArray<uint32>& OutSplatIndex
	)
	{
		const int32 NumSplats = BaseData.Num();

		OutSplatIndex.Reset();
		if (NumSplats <= 0)
		{
			return;
		}

		// Build a box over splat centers
		FVector3f CenterMin(TNumericLimits<float>::Max());
		FVector3f CenterMax(-TNumericLimits<float>::Max());
		int32 NumNonFinite = 0;
		for (int32 iSplat = 0; iSplat < NumSplats; ++iSplat)
		{
			const FVector3f& Position = BaseData[iSplat].Position;
			if (Position.ContainsNaN())
			{
				++NumNonFinite;
				continue;
			}
			CenterMin = CenterMin.ComponentMin(Position);
			CenterMax = CenterMax.ComponentMax(Position);
		}

		if (NumNonFinite > 0)
		{
			UE_LOGF(LogTemp, Warning, "Encountered non-finite positions during import.");
		}

		OutSplatIndex.SetNumUninitialized(NumSplats);

		// Degenerate guard
		const FVector3f CenterSize = CenterMax - CenterMin;
		const FVector3f InvCenterSize(
			CenterSize.X > UE_SMALL_NUMBER ? 1.0f / CenterSize.X : 0.0f,
			CenterSize.Y > UE_SMALL_NUMBER ? 1.0f / CenterSize.Y : 0.0f,
			CenterSize.Z > UE_SMALL_NUMBER ? 1.0f / CenterSize.Z : 0.0f);

		struct FMortonKey
		{
			uint64 Key;
			uint32 SplatIndex;
		};

		TArray<FMortonKey> Keys;
		Keys.SetNumUninitialized(NumSplats);

		// Generate sort keys based on Morton encoding
		ParallelFor(
			NumSplats,
			[&](int32 iSplat)
			{
				const FVector3f UnitPosition = (BaseData[iSplat].Position - CenterMin) * InvCenterSize;
				Keys[iSplat].Key = EncodeMorton3(UnitPosition);
				Keys[iSplat].SplatIndex = uint32(iSplat);
			},
			NumSplats < 4096 ? EParallelForFlags::ForceSingleThread : EParallelForFlags::None
		);

		// Sort based on keys
		Algo::Sort(
			Keys,
			[](const FMortonKey& A, const FMortonKey& B)
			{
				return A.Key != B.Key ? A.Key < B.Key : A.SplatIndex < B.SplatIndex;
			}
		);

		// Copy indices in sorted order
		for (int32 iSplat = 0; iSplat < NumSplats; ++iSplat)
		{
			OutSplatIndex[iSplat] = Keys[iSplat].SplatIndex;
		}
	}

	static void SortMortonOrderInternal(
		TArrayView<FGaussianSplatFormatFloat::FBaseData> BaseData,
		TArrayView<FVector3f> SHData,
		int32 SHDimension,
		TArray<FGaussianSplatFormatFloat::FBaseData>& OutBaseData,
		TArray<FVector3f>& OutSHData
	)
	{
		const int32 NumSplats = BaseData.Num();
		check(SHData.Num() == NumSplats * SHDimension);

		TArray<uint32> SplatIndex;
		BuildMortonOrder(BaseData, SplatIndex);
		check(SplatIndex.Num() == NumSplats);

		OutBaseData.SetNumZeroed(NumSplats);
		OutSHData.SetNumUninitialized(NumSplats * SHDimension);

		ParallelFor(
			NumSplats,
			[&](int32 iSplat)
			{
				const int32 SrcSplat = int32(SplatIndex[iSplat]);
				OutBaseData[iSplat] = BaseData[SrcSplat];

				const int32 DstOffset = iSplat * SHDimension;
				const int32 SrcOffset = SrcSplat * SHDimension;
				for (int32 iCoef = 0; iCoef < SHDimension; ++iCoef)
				{
					OutSHData[DstOffset + iCoef] = SHData[SrcOffset + iCoef];
				}
			},
			NumSplats < 4096 ? EParallelForFlags::ForceSingleThread : EParallelForFlags::None
		);
	}

	void SortMortonOrder(
		TArrayView<FGaussianSplatFormatFloat::FBaseData> BaseData,
		TArrayView<FVector3f> SHData,
		int32 SHDimension
	)
	{
		const int32 NumSplats = BaseData.Num();
		if (NumSplats > 0)
		{
			TArray<FGaussianSplatFormatFloat::FBaseData> NewBaseData;
			TArray<FVector3f> NewSHData;
			SortMortonOrderInternal(
				BaseData,
				SHData,
				SHDimension,
				NewBaseData,
				NewSHData
			);

			FMemory::Memcpy(BaseData.GetData(), NewBaseData.GetData(), NumSplats * sizeof(FGaussianSplatFormatFloat::FBaseData));
			FMemory::Memcpy(SHData.GetData(), NewSHData.GetData(), NumSplats * SHDimension * sizeof(FVector3f));
		}
	}

	void SortMortonOrder(
		TArray<FGaussianSplatFormatFloat::FBaseData>& BaseData,
		TArray<FVector3f>& SHData,
		int32 SHDimension
	)
	{
		const int32 NumSplats = BaseData.Num();
		if (NumSplats > 0)
		{
			TArray<FGaussianSplatFormatFloat::FBaseData> NewBaseData;
			TArray<FVector3f> NewSHData;
			SortMortonOrderInternal(
				BaseData,
				SHData,
				SHDimension,
				NewBaseData,
				NewSHData
			);

			Swap(BaseData, NewBaseData);
			Swap(SHData, NewSHData);
		}
	}

	void SortMortonOrder(FGaussianSplatBuilder& Builder)
	{
		SortMortonOrder(Builder.BaseData, Builder.SHData, Builder.GetSHDimension());
	}
}

#endif // WITH_EDITORONLY_DATA
