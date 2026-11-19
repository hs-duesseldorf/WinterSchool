// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GaussianSplatCluster.h"
#include "GaussianSplatCommon.h"

#include "GaussianSplatFormats.generated.h"

class FMemoryWriter;
class FMemoryReader;

// Base format class
USTRUCT()
struct FGaussianSplatFormat
{
	GENERATED_BODY();

	virtual ~FGaussianSplatFormat() = default;

	virtual void Serialize(FArchive& Ar, int32 InNumSplats, int32 InSHDegree);
	virtual uint64 CalculateMemoryUsage() const;

	virtual void TruncateSplats(int32 Count);
	virtual void TruncateSH(int32 InSHDegree);
	void TruncateClusters(uint32 ClusterIndex, uint32 SplatIndex, uint32 Level);
	void Truncate(int32 Level, int32 NewSHDegree);

	int32 GetNumSplats() const { return NumSplats; }
	int32 GetSHDegree() const { return SHDegree; }

	static void TruncateClusters(TArray<FGaussianSplatCluster>& Clusters, uint32 ClusterIndex, uint32 SplatIndex, uint32 Level);

	struct FLevelData
	{
		uint32 ClusterOffset = 0;
		uint32 ClusterCount = 0;
		uint32 SplatOffset = 0;
		uint32 SplatCount = 0;
	};
	bool BuildLevelTable(TArray<FLevelData>& LevelTable) const;

#if WITH_EDITORONLY_DATA
	virtual void DerivedDataWrite(FMemoryWriter& Writer);
	virtual void DerivedDataRead(FMemoryReader& Reader);
#endif

	int32 NumSplats = 0;
	int32 SHDegree = 0;
	TArray<FGaussianSplatCluster> Clusters;
};

// Standard full float format
USTRUCT()
struct FGaussianSplatFormatFloat : public FGaussianSplatFormat
{
	GENERATED_BODY();

	struct FBaseData
	{
		FVector3f	Position = FVector3f::ZeroVector;
		FVector3f	Scale = FVector3f::ZeroVector;
		uint32		Padding0[2] = {};
		FQuat4f		Rotation = FQuat4f::Identity;
		float		Opacity = 1.0f;
		uint32		Padding1[3] = {};
	};
	static_assert(sizeof(FBaseData) == 64);

	virtual void Serialize(FArchive& Ar, int32 InNumSplats, int32 InSHDegree) override;
	virtual uint64 CalculateMemoryUsage() const override;
	virtual void TruncateSplats(int32 Count) override;
	virtual void TruncateSH(int32 InSHDegree) override;

	int32 GetSHDimension() const { return BaseData.IsEmpty() ? 0 : SHData.Num() / BaseData.Num(); }

#if WITH_EDITORONLY_DATA
	virtual void DerivedDataWrite(FMemoryWriter& Writer) override;
	virtual void DerivedDataRead(FMemoryReader& Reader) override;
#endif

	TArray<FBaseData> BaseData;
	TArray<FVector3f> SHData;
};

// Quantized format
USTRUCT()
struct FGaussianSplatFormatQuantized : public FGaussianSplatFormat
{
	GENERATED_BODY();

	virtual void Serialize(FArchive& Ar, int32 InNumSplats, int32 InSHDegree) override;
	virtual uint64 CalculateMemoryUsage() const override;
	virtual void TruncateSplats(int32 Count) override;
	virtual void TruncateSH(int32 InSHDegree) override;

	static uint32 CalculateDataElements(int32 NumSplats, int32 SHDimension);

#if WITH_EDITORONLY_DATA
	virtual void DerivedDataWrite(FMemoryWriter& Writer) override;
	virtual void DerivedDataRead(FMemoryReader& Reader) override;
	void Build(const FGaussianSplatFormatFloat& FloatData, int32 NumSplats, int32 SHDimension);
#endif

	FVector3f PositionScaleBias[2] = { FVector3f::ZeroVector, FVector3f::ZeroVector };
	FVector3f ScaleScaleBias[2] = { FVector3f::ZeroVector, FVector3f::ZeroVector };
	FVector4f ColorScaleBias[2] = { FVector4f::Zero(), FVector4f::Zero() };
	FVector3f SHScaleBias[2] = { FVector3f::ZeroVector, FVector3f::ZeroVector };
	TArray<uint32> Data;
};

