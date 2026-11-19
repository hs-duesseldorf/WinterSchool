// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatLevelOfDetailBuilder.h"
#include "GaussianSplatCommon.h"
#include "GaussianSplatClusterBuilder.h"
#include "GaussianSplatMortonOrder.h"

#if WITH_EDITORONLY_DATA

#include "MatrixUtil.h"

using FBaseData = FGaussianSplatFormatFloat::FBaseData;

namespace GaussianSplat
{
	namespace Private
	{
		static constexpr uint32 MaxLevels = 32;
		static constexpr int32 BranchingFactor = 2;

		struct FSplatMergeWeights
		{
			float LHS = 0.5f;
			float RHS = 0.5f;
			float Total = 1.0f;
		};

		// Upper triangle of covariance matrix
		struct FCovariance3f
		{
			float XX = 0.0f;
			float YY = 0.0f;
			float ZZ = 0.0f;
			float XY = 0.0f;
			float XZ = 0.0f;
			float YZ = 0.0f;
		};

		// Represent the footprint as a surface, rather than a volume, since most splats are disc-like
		float ComputeSplatFootprint(const FVector3f& Scale)
		{
			const FVector3f AbsScale(FMath::Abs(Scale.X), FMath::Abs(Scale.Y), FMath::Abs(Scale.Z));
			const float Smallest = AbsScale.GetMin();
			return (AbsScale.X * AbsScale.Y * AbsScale.Z) / FMath::Max(Smallest, UE_SMALL_NUMBER);
		}

		FSplatMergeWeights ComputeMergeWeights(
			const FBaseData& SplatLHS,
			const FBaseData& SplatRHS,
			EGaussianSplatAggregation Mode
		)
		{
			FSplatMergeWeights Weights;
			if (Mode == EGaussianSplatAggregation::Average)
			{
				Weights.Total = SplatLHS.Opacity + SplatRHS.Opacity;
				return Weights;
			}

			const float WeightLHS = FMath::Max(SplatLHS.Opacity, 0.0f) * ComputeSplatFootprint(SplatLHS.Scale);
			const float WeightRHS = FMath::Max(SplatRHS.Opacity, 0.0f) * ComputeSplatFootprint(SplatRHS.Scale);

			Weights.Total = WeightLHS + WeightRHS;
			if (Weights.Total > UE_SMALL_NUMBER)
			{
				Weights.LHS = WeightLHS / Weights.Total;
				Weights.RHS = WeightRHS / Weights.Total;
			}

			return Weights;
		}

		// CPU implementation of SG_ComputeCovariance3D in SphericalGaussianCommon.ush.
		FCovariance3f SplatToCovariance(const FVector3f& Scale, const FQuat4f& Rotation)
		{
			const FMatrix44f R = Rotation.GetNormalized().ToMatrix();
			const FVector3f ScaleSq = Scale * Scale;

			FCovariance3f Covariance;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				const FVector3f Row(R.M[Axis][0], R.M[Axis][1], R.M[Axis][2]);
				const float Weight = ScaleSq[Axis];

				Covariance.XX += Weight * Row.X * Row.X;
				Covariance.YY += Weight * Row.Y * Row.Y;
				Covariance.ZZ += Weight * Row.Z * Row.Z;
				Covariance.XY += Weight * Row.X * Row.Y;
				Covariance.XZ += Weight * Row.X * Row.Z;
				Covariance.YZ += Weight * Row.Y * Row.Z;
			}

			return Covariance;
		}

		void CovarianceToSplat(const FCovariance3f& Covariance, FVector3f& OutScale, FQuat4f& OutRotation)
		{
			float A[9] =
			{
				Covariance.XX, Covariance.XY, Covariance.XZ,
				Covariance.XY, Covariance.YY, Covariance.YZ,
				Covariance.XZ, Covariance.YZ, Covariance.ZZ
			};
			float S[3] = {};
			float V[9] = {};
			JacobiSVD::EigenSolver3(A, S, V, 1.0e-8f);

			FVector3f Axes[3];
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				Axes[Axis] = FVector3f(V[Axis], V[Axis + 3], V[Axis + 6]).GetSafeNormal();
				if (Axes[Axis].IsNearlyZero())
				{
					Axes[Axis] = FVector3f::ZeroVector;
					Axes[Axis][Axis] = 1.0f;
				}

				OutScale[Axis] = FMath::Sqrt(FMath::Max(S[Axis], 0.0f));
			}

			// A left-handed eigenbasis would decode as a reflection rather than a rotation.
			if ((FVector3f::CrossProduct(Axes[0], Axes[1]) | Axes[2]) < 0.0f)
			{
				Axes[2] = -Axes[2];
			}

			const FMatrix44f Basis(
				FPlane4f(Axes[0].X, Axes[0].Y, Axes[0].Z, 0.0f),
				FPlane4f(Axes[1].X, Axes[1].Y, Axes[1].Z, 0.0f),
				FPlane4f(Axes[2].X, Axes[2].Y, Axes[2].Z, 0.0f),
				FPlane4f(0.0f, 0.0f, 0.0f, 1.0f));
			OutRotation = FQuat4f(Basis);
			OutRotation.Normalize();
		}

		void MergeSplatPair(
			const FBaseData& SplatLHS,
			const FBaseData& SplatRHS,
			EGaussianSplatAggregation Mode,
			const FSplatMergeWeights& Weights,
			FBaseData& OutSplat,
			float& OutError
		)
		{
			OutSplat = FBaseData();
			OutSplat.Position = Weights.LHS * SplatLHS.Position + Weights.RHS * SplatRHS.Position;

			const FVector3f DeltaLHS = SplatLHS.Position - OutSplat.Position;
			const FVector3f DeltaRHS = SplatRHS.Position - OutSplat.Position;
			OutError = FMath::Sqrt(FMath::Max(Weights.LHS * DeltaLHS.SizeSquared() + Weights.RHS * DeltaRHS.SizeSquared(), 0.0f));

			// Build covariance matrices, then use SVD to extract the interpolated scale and rotation
			const FCovariance3f CovarianceLHS = SplatToCovariance(SplatLHS.Scale, SplatLHS.Rotation);
			const FCovariance3f CovarianceRHS = SplatToCovariance(SplatRHS.Scale, SplatRHS.Rotation);

			// Apply positional drift from the centroid as spread (ie: Steiner term)
			const bool bIncludeSpread = Mode == EGaussianSplatAggregation::MomentPreserving;
			const FVector3f SpreadLHS = bIncludeSpread ? DeltaLHS : FVector3f::ZeroVector;
			const FVector3f SpreadRHS = bIncludeSpread ? DeltaRHS : FVector3f::ZeroVector;

			FCovariance3f Merged;
			Merged.XX = Weights.LHS * (CovarianceLHS.XX + SpreadLHS.X * SpreadLHS.X) + Weights.RHS * (CovarianceRHS.XX + SpreadRHS.X * SpreadRHS.X);
			Merged.YY = Weights.LHS * (CovarianceLHS.YY + SpreadLHS.Y * SpreadLHS.Y) + Weights.RHS * (CovarianceRHS.YY + SpreadRHS.Y * SpreadRHS.Y);
			Merged.ZZ = Weights.LHS * (CovarianceLHS.ZZ + SpreadLHS.Z * SpreadLHS.Z) + Weights.RHS * (CovarianceRHS.ZZ + SpreadRHS.Z * SpreadRHS.Z);
			Merged.XY = Weights.LHS * (CovarianceLHS.XY + SpreadLHS.X * SpreadLHS.Y) + Weights.RHS * (CovarianceRHS.XY + SpreadRHS.X * SpreadRHS.Y);
			Merged.XZ = Weights.LHS * (CovarianceLHS.XZ + SpreadLHS.X * SpreadLHS.Z) + Weights.RHS * (CovarianceRHS.XZ + SpreadRHS.X * SpreadRHS.Z);
			Merged.YZ = Weights.LHS * (CovarianceLHS.YZ + SpreadLHS.Y * SpreadLHS.Z) + Weights.RHS * (CovarianceRHS.YZ + SpreadRHS.Y * SpreadRHS.Z);

			CovarianceToSplat(Merged, OutSplat.Scale, OutSplat.Rotation);

			if (Mode == EGaussianSplatAggregation::MomentPreserving)
			{
				// Redistribute opacity over the parent's footprint
				const float Footprint = ComputeSplatFootprint(OutSplat.Scale);
				OutSplat.Opacity = FMath::Clamp(Weights.Total / FMath::Max(Footprint, UE_SMALL_NUMBER), 0.0f, 1.0f);
			}
			else
			{
				OutSplat.Opacity = Weights.LHS * SplatLHS.Opacity + Weights.RHS * SplatRHS.Opacity;
			}
		}

		void MergeSplatSH(
			TConstArrayView<FVector3f> SHLHS,
			TConstArrayView<FVector3f> SHRHS,
			const FSplatMergeWeights& Weights,
			TArrayView<FVector3f> OutSH
		)
		{
			check(SHLHS.Num() == OutSH.Num() && SHRHS.Num() == OutSH.Num());
			for (int32 Index = 0; Index < OutSH.Num(); ++Index)
			{
				OutSH[Index] = Weights.LHS * SHLHS[Index] + Weights.RHS * SHRHS[Index];
			}
		}

		void AggregateSplats(
			const FGaussianSplatFormatFloat& Splats,
			int32 IndexLHS,
			int32 IndexRHS,
			int32 IndexResult,
			EGaussianSplatAggregation Mode,
			TArray<FBaseData>& OutBaseData,
			TArray<FVector3f>& OutSHData,
			float& OutError
		)
		{
			const int32 SHDimension = Splats.GetSHDimension();
			const FSplatMergeWeights Weights = ComputeMergeWeights(Splats.BaseData[IndexLHS], Splats.BaseData[IndexRHS], Mode);

			MergeSplatPair(
				Splats.BaseData[IndexLHS],
				Splats.BaseData[IndexRHS],
				Mode,
				Weights,
				OutBaseData[IndexResult],
				OutError);

			MergeSplatSH(
				MakeArrayView(&Splats.SHData[IndexLHS * SHDimension], SHDimension),
				MakeArrayView(&Splats.SHData[IndexRHS * SHDimension], SHDimension),
				Weights,
				MakeArrayView(&OutSHData[IndexResult * SHDimension], SHDimension));
		}

		void SimplifyCluster(
			const FGaussianSplatFormatFloat& Splats,
			const FGaussianSplatCluster& Cluster,
			int32 IndexResult,
			EGaussianSplatAggregation Mode,
			TArray<FBaseData>& OutBaseData,
			TArray<FVector3f>& OutSHData,
			float& OutMaxError
		)
		{
			const int32 ClusterSplatOffset = int32(Cluster.SplatOffset);
			const int32 ClusterSplatCount = int32(Cluster.SplatCount);
			const int32 SHDimension = Splats.GetSHDimension();

			for (int32 SplatIndex = 0; SplatIndex < ClusterSplatCount; SplatIndex += BranchingFactor, IndexResult++)
			{
				int32 IndexLHS = ClusterSplatOffset + SplatIndex;

				if (SplatIndex + 1 < ClusterSplatCount)
				{
					int32 IndexRHS = IndexLHS + 1;
					float Error = 0.0f;
					AggregateSplats(
						Splats,
						IndexLHS,
						IndexRHS,
						IndexResult,
						Mode,
						OutBaseData,
						OutSHData,
						Error
					);
					OutMaxError = FMath::Max(OutMaxError, Error);
				}
				else
				{
					// Copy remaining splat verbatim
					OutBaseData[IndexResult] = Splats.BaseData[IndexLHS];

					const int32 SourceSHOffset = IndexLHS * SHDimension;
					const int32 ResultSHOffset = IndexResult * SHDimension;
					for (int32 SHIndex = 0; SHIndex < SHDimension; ++SHIndex)
					{
						OutSHData[ResultSHOffset + SHIndex] = Splats.SHData[SourceSHOffset + SHIndex];
					}
				}
			}
		}

		int32 BuildNextLevel(
			FGaussianSplatFormatFloat& Splats,
			int32 InClusterBegin,
			int32 InClusterEnd,
			uint32 OutLevel,
			EGaussianSplatAggregation Mode
		)
		{
			const int32 ClusterCount       = InClusterEnd - InClusterBegin;
			const int32 ParentClusterCount = FMath::DivideAndRoundUp(ClusterCount, BranchingFactor);
			const int32 SHDimension        = Splats.GetSHDimension();

			const TArray<FGaussianSplatCluster> ChildClusters(Splats.Clusters.GetData() + InClusterBegin, ClusterCount);

			// Compute the expected splat count from each simplified child cluster to prepopulate the parent cluster lookup table
			TArray<int32> ParentSplatOffsets;
			ParentSplatOffsets.SetNumUninitialized(ParentClusterCount + 1);
			ParentSplatOffsets[0] = Splats.BaseData.Num();
			for (int32 ParentClusterIndex = 0; ParentClusterIndex < ParentClusterCount; ++ParentClusterIndex)
			{
				int32 SplatCount = 0;
				const int32 ChildClusterBegin = ParentClusterIndex * BranchingFactor;
				const int32 ChildClusterEnd = FMath::Min(ChildClusterBegin + BranchingFactor, ClusterCount);
				for (int32 ChildClusterIndex = ChildClusterBegin; ChildClusterIndex < ChildClusterEnd; ++ChildClusterIndex)
				{
					const int32 ChildSplatCount = int32(ChildClusters[ChildClusterIndex].SplatCount);
					SplatCount += FMath::DivideAndRoundUp(ChildSplatCount, BranchingFactor);
				}
				ParentSplatOffsets[ParentClusterIndex + 1] = ParentSplatOffsets[ParentClusterIndex] + SplatCount;
			}

			// Allocate new splats (base + SH data)
			const int32 OutSplatCount = ParentSplatOffsets[ParentClusterCount] - ParentSplatOffsets[0];
			Splats.BaseData.AddUninitialized(OutSplatCount);
			Splats.SHData.AddUninitialized(OutSplatCount * SHDimension);

			// Allocate new clusters
			const int32 OutClusterBegin = Splats.Clusters.Num();
			Splats.Clusters.AddDefaulted(ParentClusterCount);

			// TODO: Iterate over parent clusters in parallel
			for (int32 ParentClusterIndex = 0; ParentClusterIndex < ParentClusterCount; ++ParentClusterIndex)
			{
				const int32 ChildClusterBegin = ParentClusterIndex * BranchingFactor;
				const int32 ChildClusterEnd   = FMath::Min(ChildClusterBegin + BranchingFactor, ClusterCount);
				const int32 SplatIndexBegin = ParentSplatOffsets[ParentClusterIndex];
				const int32 SplatIndexEnd   = ParentSplatOffsets[ParentClusterIndex + 1];

				// Simplify child clusters, recording the aggregation error
				float Error = 0.0f;
				int32 IndexResult = SplatIndexBegin;
				for (int32 ChildClusterIndex = ChildClusterBegin; ChildClusterIndex < ChildClusterEnd; ++ChildClusterIndex)
				{
					const int32 ChildSplatCount = int32(ChildClusters[ChildClusterIndex].SplatCount);
					SimplifyCluster(Splats, ChildClusters[ChildClusterIndex], IndexResult, Mode, Splats.BaseData, Splats.SHData, Error);
					IndexResult += FMath::DivideAndRoundUp(ChildSplatCount, BranchingFactor);
					Error = FMath::Max(Error, ChildClusters[ChildClusterIndex].Error);
				}
				check(IndexResult == SplatIndexEnd);

				// Sort simplified splats 
				int32 SplatCount = SplatIndexEnd - SplatIndexBegin;
				SortMortonOrder(
					MakeArrayView(Splats.BaseData.GetData() + SplatIndexBegin, SplatCount),
					MakeArrayView(Splats.SHData.GetData() + SplatIndexBegin * SHDimension, SplatCount * SHDimension),
					SHDimension
				);
				
				// Record parent cluster
				FGaussianSplatCluster& ParentCluster = Splats.Clusters[OutClusterBegin + ParentClusterIndex];
				ParentCluster.SplatOffset = uint32(SplatIndexBegin);
				ParentCluster.SplatCount  = uint32(SplatCount);
				ParentCluster.Error       = Error;
				ParentCluster.ParentIndex = ~0u;
				ParentCluster.Level       = OutLevel;
				ComputeClusterBounds(Splats, ParentCluster);

				// Assign parent index for LOD
				for (int32 ChildClusterIndex = ChildClusterBegin; ChildClusterIndex < ChildClusterEnd; ++ChildClusterIndex)
				{
					Splats.Clusters[InClusterBegin + ChildClusterIndex].ParentIndex = OutClusterBegin + ParentClusterIndex;
				}
			}

			return ParentClusterCount;
		}
	}

	void BuildLevelsOfDetail(FGaussianSplatFormatFloat& Splats, EGaussianSplatAggregation Mode)
	{
		using namespace Private;
		check(Splats.BaseData.Num() == Splats.GetNumSplats());

		int32 LevelClusterBegin = 0;
		int32 LevelClusterEnd = Splats.Clusters.Num();
		int32 LevelClusterCount = LevelClusterEnd - LevelClusterBegin;

		for (uint32 Level = 1; Level < MaxLevels && LevelClusterCount > 1; ++Level)
		{
			const int32 NumEmitted = BuildNextLevel(Splats, LevelClusterBegin, LevelClusterEnd, Level, Mode);
			LevelClusterBegin = LevelClusterEnd;
			LevelClusterEnd += NumEmitted;
			LevelClusterCount = LevelClusterEnd - LevelClusterBegin;
		}

		Splats.NumSplats = Splats.BaseData.Num();
	}

	int64 BoundLevelOfDetailSplatCount(int32 NumSplats, int32 MaxSplatsPerCluster)
	{
		using namespace Private;
		if (NumSplats <= 0 || MaxSplatsPerCluster <= 0)
		{
			return FMath::Max(NumSplats, 0);
		}

		const int64 NumClusters = FMath::DivideAndRoundUp(int64(NumSplats), int64(MaxSplatsPerCluster));
		return FMath::DivideAndRoundUp(BranchingFactor * (NumSplats + NumClusters), int64(BranchingFactor - 1)) + MaxLevels;
	}
}

#endif // WITH_EDITORONLY_DATA
