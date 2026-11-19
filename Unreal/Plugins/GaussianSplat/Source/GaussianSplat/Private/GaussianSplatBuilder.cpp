// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatBuilder.h"
#include "GaussianSplatCommon.h"
#include "GaussianSplatRenderResource.h"
#include "Async/ParallelFor.h"

bool FGaussianSplatBuilder::IsValid() const
{
	const int32 NumSplats = GetNumSplats();
	return 
		NumSplats > 0 &&
		Bounds.IsValid &&
		SHData.Num() == NumSplats * GetSHDimension()
		;
}

int32 FGaussianSplatBuilder::GetNumSplats() const
{
	return BaseData.Num();
}

void FGaussianSplatBuilder::SetSHDegree(int32 InSHDegree)
{
	// We can only shrink the SH basis not grow so bail if this is a no-op
	const int32 NewSHDegree = FMath::Clamp(InSHDegree, 0, SHDegree);
	if (NewSHDegree == SHDegree)
	{
		return;
	}

	const int32 NewSHDimension = GaussianSplatCommon::GetSHDimension(NewSHDegree);
	const int32 OldSHDimension = GaussianSplatCommon::GetSHDimension(SHDegree);

	check(SHData.Num() == OldSHDimension * GetNumSplats());

	TArray<FVector3f> NewSHData;
	NewSHData.AddUninitialized(NewSHDimension * GetNumSplats());
	for (int32 iSplat = 0; iSplat < GetNumSplats(); ++iSplat)
	{
		int32 NewSHOffset = iSplat * NewSHDimension;
		int32 OldSHOffset = iSplat * OldSHDimension;
		for (int32 iCoef = 0; iCoef < NewSHDimension; ++iCoef)
		{
			NewSHData[NewSHOffset++] = SHData[OldSHOffset++];
		}
	}

	Swap(SHData, NewSHData);
	SHDegree = NewSHDegree;
}

int32 FGaussianSplatBuilder::GetSHDimension() const
{
	return GaussianSplatCommon::GetSHDimension(SHDegree);
}

uint64 FGaussianSplatBuilder::GetMaxBufferBytes()
{
	return MAX_uint32;
}

int64 FGaussianSplatBuilder::GetMaxSplatCount(int32 SHDimension, bool bQuantizedFormat)
{
	return int64(GetMaxBufferBytes() / FGaussianSplatRenderResource::GetSplatStrideForFormat(SHDimension, bQuantizedFormat));
}

void FGaussianSplatBuilder::CalculateBounds()
{
	Bounds = FBox3f(EForceInit::ForceInitToZero);

	const int32 NumSplats = GetNumSplats();
	if (NumSplats > 0)
	{
		FVector3f BoundsMin = FVector3f(TNumericLimits<float>::Max());
		FVector3f BoundsMax = FVector3f(-TNumericLimits<float>::Max());

		bool bValid = false;
		const float ScaleFactor = GaussianSplatCommon::SplatBoundsScale;
		for (const FGaussianSplatFormatFloat::FBaseData& Splat : BaseData)
		{
			if (Splat.Position.ContainsNaN() || Splat.Scale.ContainsNaN())
			{
				continue;
			}

			const FVector3f Scale(ScaleFactor * Splat.Scale.GetAbsMax());
			BoundsMin = BoundsMin.ComponentMin(Splat.Position - Scale);
			BoundsMax = BoundsMax.ComponentMax(Splat.Position + Scale);
			bValid = true;
		}

		if (bValid)
		{
			Bounds = FBox3f(BoundsMin, BoundsMax);
		}
	}
}
