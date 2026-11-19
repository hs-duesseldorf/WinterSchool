// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatClusterBuilder.h"
#include "GaussianSplatCommon.h"

#include "Async/ParallelFor.h"

#if WITH_EDITORONLY_DATA

namespace GaussianSplat
{
	namespace Private
	{
		static constexpr float GaussianTailScale = GaussianSplatCommon::SplatBoundsScale;

		void GroupSequential(int32 NumSplats, int32 MaxSplatsPerCluster, TArray<FGaussianSplatCluster>& OutClusters)
		{
			OutClusters.Reset(FMath::DivideAndRoundUp(NumSplats, MaxSplatsPerCluster));

			for (int32 Offset = 0; Offset < NumSplats; Offset += MaxSplatsPerCluster)
			{
				FGaussianSplatCluster& Cluster = OutClusters.AddDefaulted_GetRef();
				Cluster.SplatOffset = uint32(Offset);
				Cluster.SplatCount  = uint32(FMath::Min(MaxSplatsPerCluster, NumSplats - Offset));
				Cluster.Level = 0;
				Cluster.Error       = 0.0f;
				Cluster.ParentIndex = ~0u;
			}
		}
	}

	void ComputeClusterBounds(const FGaussianSplatFormatFloat& Source, FGaussianSplatCluster& Cluster)
	{
		using namespace Private;

		FVector3f BoundsMin(TNumericLimits<float>::Max());
		FVector3f BoundsMax(-TNumericLimits<float>::Max());

		bool bValid = false;
		const int32 BeginSplat = int32(Cluster.SplatOffset);
		const int32 EndSplat   = BeginSplat + int32(Cluster.SplatCount);
		for (int32 iSplat = BeginSplat; iSplat < EndSplat; ++iSplat)
		{
			const FGaussianSplatFormatFloat::FBaseData& Splat = Source.BaseData[iSplat];

			if (Splat.Position.ContainsNaN() || Splat.Scale.ContainsNaN())
			{
				continue;
			}

			bValid = true;
			const FVector3f Extent(GaussianTailScale * Splat.Scale.GetAbsMax());
			BoundsMin = BoundsMin.ComponentMin(Splat.Position - Extent);
			BoundsMax = BoundsMax.ComponentMax(Splat.Position + Extent);
		}

		Cluster.BoundsMin = bValid ? BoundsMin : FVector3f::ZeroVector;
		Cluster.BoundsMax = bValid ? BoundsMax : FVector3f::ZeroVector;
	}

	void BuildClusters(
		const FGaussianSplatFormatFloat& Source,
		int32 NumSplats,
		int32 MaxSplatsPerCluster,
		EGaussianSplatClusterGrouping GroupingMode,
		TArray<FGaussianSplatCluster>& OutClusters)
	{
		using namespace Private;
		check(Source.BaseData.Num() >= NumSplats);

		OutClusters.Reset();

		if (NumSplats <= 0 || MaxSplatsPerCluster <= 0 || GroupingMode == EGaussianSplatClusterGrouping::None)
		{
			return;
		}

		switch (GroupingMode)
		{
			case EGaussianSplatClusterGrouping::Sequential:
				GroupSequential(NumSplats, MaxSplatsPerCluster, OutClusters);
				break;

			default:
				checkNoEntry();
				return;
		}

		ParallelFor(
			OutClusters.Num(),
			[&](int32 iCluster)
			{
				ComputeClusterBounds(Source, OutClusters[iCluster]);
			}
		);
	}
}

#endif //WITH_EDITORONLY_DATA
