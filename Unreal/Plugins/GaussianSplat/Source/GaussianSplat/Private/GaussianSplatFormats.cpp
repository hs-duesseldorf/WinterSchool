// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatFormats.h"
#include "GaussianSplatCluster.h"
#include "GaussianSplatCustomVersion.h"
#include "GaussianSplatShared.h"

#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GaussianSplatFormats)

void FGaussianSplatFormat::Serialize(FArchive& Ar, int32 InNumSplats, int32 InSHDegree)
{
	// Editor data serialization
	if (!Ar.IsFilterEditorOnly())
	{
		NumSplats = InNumSplats;
		SHDegree = InSHDegree;
		return;
	}

	// Runtime data serialization
	Ar << NumSplats;

	// Older runtime data does not support SH degree truncation
	if (Ar.CustomVer(FGaussianSplatCustomVersion::GUID) >= FGaussianSplatCustomVersion::RuntimeSHDegree)
	{
		Ar << SHDegree;
	}
	else
	{
		SHDegree = InSHDegree;
	}

	int32 NumClusters = Clusters.Num();
	Ar << NumClusters;

	if (Ar.IsLoading())
	{
		Clusters.SetNumUninitialized(NumClusters);
	}

	if (NumClusters > 0)
	{
		Ar.SerializeCompressed(Clusters.GetData(), NumClusters * Clusters.GetTypeSize(), NAME_Oodle);
	}
}

uint64 FGaussianSplatFormat::CalculateMemoryUsage() const
{
	return Clusters.GetAllocatedSize();
}

void FGaussianSplatFormat::TruncateSplats(int32 Count)
{
	NumSplats = FMath::Max(NumSplats - FMath::Max(Count, 0), 0);
}

void FGaussianSplatFormatFloat::TruncateSplats(int32 Count)
{
	const int32 SplatCount = FMath::Clamp(Count, 0, BaseData.Num());
	if (SplatCount > 0)
	{
		const int32 SHDimension = GaussianSplatCommon::GetSHDimension(SHDegree);
		BaseData.RemoveAt(0, SplatCount, EAllowShrinking::No);
		SHData.RemoveAt(0, FMath::Min(SplatCount * SHDimension, SHData.Num()), EAllowShrinking::No);
	}
	Super::TruncateSplats(SplatCount);
}

void FGaussianSplatFormatQuantized::TruncateSplats(int32 Count)
{
	const int32 ElementsPerSplat = int32(CalculateDataElements(1, GaussianSplatCommon::GetSHDimension(SHDegree)));
	const int32 SplatCount = ElementsPerSplat > 0 ? FMath::Clamp(Count, 0, Data.Num() / ElementsPerSplat) : 0;
	if (SplatCount > 0)
	{
		Data.RemoveAt(0, SplatCount * ElementsPerSplat, EAllowShrinking::No);
	}
	Super::TruncateSplats(SplatCount);
}

void FGaussianSplatFormat::TruncateSH(int32 InSHDegree)
{
	SHDegree = FMath::Clamp(InSHDegree, 0, SHDegree);
}

void FGaussianSplatFormatFloat::TruncateSH(int32 InSHDegree)
{
	if (InSHDegree < SHDegree)
	{
		const int32 OldSHDegree = GaussianSplatCommon::GetSHDimension(SHDegree);
		const int32 NewSHDegree = GaussianSplatCommon::GetSHDimension(InSHDegree);
		const int32 SplatCount = SHData.Num() / OldSHDegree;
		for (int32 iSplat = 0; iSplat < SplatCount; ++iSplat)
		{
			for (int32 iCoef = 0; iCoef < NewSHDegree; ++iCoef)
			{
				SHData[iSplat * NewSHDegree + iCoef] = SHData[iSplat * OldSHDegree + iCoef];
			}
		}
		SHData.SetNum(SplatCount * NewSHDegree, EAllowShrinking::No);
	}
	Super::TruncateSH(InSHDegree);
}

void FGaussianSplatFormatQuantized::TruncateSH(int32 InSHDegree)
{
	if (InSHDegree < SHDegree)
	{
		const int32 OldElementsPerSplat = int32(CalculateDataElements(1, GaussianSplatCommon::GetSHDimension(SHDegree)));
		const int32 NewElementsPerSplat = int32(CalculateDataElements(1, GaussianSplatCommon::GetSHDimension(InSHDegree)));
		const int32 SplatCount = Data.Num() / OldElementsPerSplat;
		for (int32 iSplat = 0; iSplat < SplatCount; ++iSplat)
		{
			for (int32 iElement = 0; iElement < NewElementsPerSplat; ++iElement)
			{
				Data[iSplat * NewElementsPerSplat + iElement] = Data[iSplat * OldElementsPerSplat + iElement];
			}
		}
		Data.SetNum(SplatCount * NewElementsPerSplat, EAllowShrinking::No);
	}
	Super::TruncateSH(InSHDegree);
}

void FGaussianSplatFormat::TruncateClusters(TArray<FGaussianSplatCluster>& Clusters, uint32 ClusterIndex, uint32 SplatIndex, uint32 Level)
{
	Clusters.RemoveAt(0, int32(ClusterIndex), EAllowShrinking::No);
	for (FGaussianSplatCluster& Cluster : Clusters)
	{
		Cluster.SplatOffset -= SplatIndex;
		Cluster.ParentIndex = (Cluster.ParentIndex == ~0u) ? ~0u : Cluster.ParentIndex - ClusterIndex;
		Cluster.Level -= Level;

		// Leaf clusters have 0 error by construction
		if (Cluster.Level == 0)
		{
			Cluster.Error = 0.0f;
		}
	}
}

void FGaussianSplatFormat::TruncateClusters(uint32 ClusterIndex, uint32 SplatIndex, uint32 Level)
{
	TruncateClusters(Clusters, ClusterIndex, SplatIndex, Level);
}

void FGaussianSplatFormat::Truncate(int32 Level, int32 NewSHDegree)
{
	TArray<FGaussianSplatFormat::FLevelData> LevelTable;
	BuildLevelTable(LevelTable);
	if (!LevelTable.IsEmpty())
	{
		Level = FMath::Clamp(Level, 0, LevelTable.Num() - 1);

		const FGaussianSplatFormat::FLevelData& LevelData = LevelTable[Level];
		TruncateSplats(int32(LevelData.SplatOffset));
		TruncateClusters(LevelData.ClusterOffset, LevelData.SplatOffset, uint32(Level));
	}

	TruncateSH(NewSHDegree);
}

bool FGaussianSplatFormat::BuildLevelTable(TArray<FLevelData>& LevelTable) const
{
	LevelTable.Reset();

	for (int32 ClusterIndex = 0; ClusterIndex < Clusters.Num(); ++ClusterIndex)
	{
		const FGaussianSplatCluster& Cluster = Clusters[ClusterIndex];

		// Encountered new level
		if (Cluster.Level == uint32(LevelTable.Num()))
		{
			uint32 ClusterOffset = 0;
			uint32 SplatOffset = 0;
			if (!LevelTable.IsEmpty())
			{
				FLevelData& LevelData = LevelTable.Last();
				ClusterOffset = LevelData.ClusterOffset + LevelData.ClusterCount;
				SplatOffset = LevelData.SplatOffset + LevelData.SplatCount;
			}

			FLevelData& LevelData = LevelTable.AddDefaulted_GetRef();
			LevelData.ClusterOffset = ClusterOffset;
			LevelData.ClusterCount = 1;
			LevelData.SplatOffset = SplatOffset;
			LevelData.SplatCount = Cluster.SplatCount;
		}
		// Encountered cluster at same level
		else
		{
			if (!ensureMsgf(Cluster.Level + 1 == uint32(LevelTable.Num()), TEXT("Gaussian Splat: corrupt cluster table")))
			{
				LevelTable.Reset();
				return false;
			}

			FLevelData& LevelData = LevelTable.Last();
			LevelData.ClusterCount++;
			LevelData.SplatCount += Cluster.SplatCount;
		}
	}

	return !LevelTable.IsEmpty();
}

#if WITH_EDITORONLY_DATA
void FGaussianSplatFormat::DerivedDataWrite(FMemoryWriter& Writer)
{
	Writer << NumSplats;
	Writer << SHDegree;

	int32 NumClusters = Clusters.Num();
	Writer << NumClusters;

	if (NumClusters > 0)
	{
		Writer.Serialize(Clusters.GetData(), NumClusters * Clusters.GetTypeSize());
	}
}

void FGaussianSplatFormat::DerivedDataRead(FMemoryReader& Reader)
{
	Reader << NumSplats;
	Reader << SHDegree;

	int32 NumClusters = 0;
	Reader << NumClusters;

	Clusters.SetNumUninitialized(NumClusters);
	if (NumClusters > 0)
	{
		Reader.Serialize(Clusters.GetData(), NumClusters * Clusters.GetTypeSize());
	}
}
#endif

void FGaussianSplatFormatFloat::Serialize(FArchive& Ar, int32 InNumSplats, int32 InSHDegree)
{
	Super::Serialize(Ar, InNumSplats, InSHDegree);

	if (Ar.IsLoading())
	{
		BaseData.SetNumUninitialized(NumSplats);
		SHData.SetNumUninitialized(NumSplats * GaussianSplatCommon::GetSHDimension(SHDegree));
	}

	if (NumSplats > 0)
	{
		Ar.SerializeCompressed(BaseData.GetData(), BaseData.Num() * BaseData.GetTypeSize(), NAME_Oodle);
		Ar.SerializeCompressed(SHData.GetData(), SHData.Num() * SHData.GetTypeSize(), NAME_Oodle);
	}
}

uint64 FGaussianSplatFormatFloat::CalculateMemoryUsage() const
{
	return Super::CalculateMemoryUsage() + BaseData.GetAllocatedSize() + SHData.GetAllocatedSize();
}

#if WITH_EDITORONLY_DATA
void FGaussianSplatFormatFloat::DerivedDataWrite(FMemoryWriter& Writer)
{
	Super::DerivedDataWrite(Writer);

	int32 BaseDataNum = BaseData.Num();
	int32 SHDataNum = SHData.Num();

	Writer << BaseDataNum;
	Writer << SHDataNum;

	Writer.Serialize(BaseData.GetData(), BaseDataNum * BaseData.GetTypeSize());
	Writer.Serialize(SHData.GetData(), SHDataNum * SHData.GetTypeSize());
}

void FGaussianSplatFormatFloat::DerivedDataRead(FMemoryReader& Reader)
{
	Super::DerivedDataRead(Reader);

	int32 BaseDataNum = 0;
	int32 SHDataNum = 0;

	Reader << BaseDataNum;
	Reader << SHDataNum;

	BaseData.SetNumUninitialized(BaseDataNum);
	Reader.Serialize(BaseData.GetData(), BaseDataNum* BaseData.GetTypeSize());

	SHData.SetNumUninitialized(SHDataNum);
	Reader.Serialize(SHData.GetData(), SHDataNum * SHData.GetTypeSize());
}
#endif

void FGaussianSplatFormatQuantized::Serialize(FArchive& Ar, int32 InNumSplats, int32 InSHDegree)
{
	Super::Serialize(Ar, InNumSplats, InSHDegree);

	if (Ar.IsLoading())
	{
		Data.SetNumUninitialized(CalculateDataElements(NumSplats, GaussianSplatCommon::GetSHDimension(SHDegree)));
	}

	if (NumSplats > 0)
	{
		Ar.SerializeCompressed(Data.GetData(), Data.Num() * Data.GetTypeSize(), NAME_Oodle);
	}

	Ar << PositionScaleBias[0];
	Ar << PositionScaleBias[1];
	Ar << ScaleScaleBias[0];
	Ar << ScaleScaleBias[1];
	Ar << ColorScaleBias[0];
	Ar << ColorScaleBias[1];
	Ar << SHScaleBias[0];
	Ar << SHScaleBias[1];
}

uint64 FGaussianSplatFormatQuantized::CalculateMemoryUsage() const
{
	return Super::CalculateMemoryUsage() + Data.GetAllocatedSize();
}

uint32 FGaussianSplatFormatQuantized::CalculateDataElements(int32 NumSplats, int32 SHDimension)
{
	uint32 DataSize;
	DataSize = uint32(NumSplats) * (4u + uint32(SHDimension));
	return DataSize;
}

#if WITH_EDITORONLY_DATA
void FGaussianSplatFormatQuantized::DerivedDataWrite(FMemoryWriter& Writer)
{
	Super::DerivedDataWrite(Writer);

	int32 DataNum = Data.Num();

	Writer << DataNum;
	Writer.Serialize(Data.GetData(), DataNum * Data.GetTypeSize());

	Writer << PositionScaleBias[0];
	Writer << PositionScaleBias[1];
	Writer << ScaleScaleBias[0];
	Writer << ScaleScaleBias[1];
	Writer << ColorScaleBias[0];
	Writer << ColorScaleBias[1];
	Writer << SHScaleBias[0];
	Writer << SHScaleBias[1];
}

void FGaussianSplatFormatQuantized::DerivedDataRead(FMemoryReader& Reader)
{
	Super::DerivedDataRead(Reader);

	int32 DataNum = 0;

	Reader << DataNum;
	Data.SetNumUninitialized(DataNum);
	Reader.Serialize(Data.GetData(), DataNum * Data.GetTypeSize());

	Reader << PositionScaleBias[0];
	Reader << PositionScaleBias[1];
	Reader << ScaleScaleBias[0];
	Reader << ScaleScaleBias[1];
	Reader << ColorScaleBias[0];
	Reader << ColorScaleBias[1];
	Reader << SHScaleBias[0];
	Reader << SHScaleBias[1];
}

void FGaussianSplatFormatQuantized::Build(const FGaussianSplatFormatFloat& FloatData, int32 InNumSplats, int32 SHDimension)
{
	NumSplats = InNumSplats;
	const uint32 ElementSize = CalculateDataElements(1, SHDimension);
	Data.SetNumUninitialized(NumSplats * ElementSize);

	auto LogScale =
		[](const FVector3f& LinearScale) -> FVector3f
		{
			const float MinLinearScale = FGaussianSplatShared::EQuantized_MinLinearScale;
			return FVector3f(
				FMath::Loge(FMath::Max(LinearScale.X, MinLinearScale)),
				FMath::Loge(FMath::Max(LinearScale.Y, MinLinearScale)),
				FMath::Loge(FMath::Max(LinearScale.Z, MinLinearScale)));
		};

	// Gather bounds
	{
		FVector3f PositionMin = FVector3f(TNumericLimits<float>::Max());
		FVector3f PositionMax = FVector3f(-TNumericLimits<float>::Max());
		FVector3f ScaleMin = FVector3f(TNumericLimits<float>::Max());
		FVector3f ScaleMax = FVector3f(-TNumericLimits<float>::Max());
		FVector4f ColorMin = FVector4f(TNumericLimits<float>::Max());
		FVector4f ColorMax = FVector4f(-TNumericLimits<float>::Max());
		FVector3f SHMin = FVector3f(TNumericLimits<float>::Max());
		FVector3f SHMax = FVector3f(-TNumericLimits<float>::Max());

		for (int32 iSplat = 0; iSplat < NumSplats; ++iSplat)
		{
			const FGaussianSplatFormatFloat::FBaseData& BaseData = FloatData.BaseData[iSplat];
			const uint32 SHDataOffset = iSplat * SHDimension;
			const FVector4f Color = FVector4f(FloatData.SHData[SHDataOffset], BaseData.Opacity);

			PositionMin = PositionMin.ComponentMin(BaseData.Position);
			PositionMax = PositionMax.ComponentMax(BaseData.Position);
			const FVector3f BaseDataScale = LogScale(BaseData.Scale);
			ScaleMin = ScaleMin.ComponentMin(BaseDataScale);
			ScaleMax = ScaleMax.ComponentMax(BaseDataScale);
			ColorMin = ColorMin.ComponentMin(Color);
			ColorMax = ColorMax.ComponentMax(Color);

			// Note: not efficient we waste a byte per coef
			for (int32 iSHCoef = 1; iSHCoef < SHDimension; ++iSHCoef)
			{
				const FVector3f SHCoef = FloatData.SHData[SHDataOffset + iSHCoef];
				SHMin = SHMin.ComponentMin(SHCoef);
				SHMax = SHMax.ComponentMax(SHCoef);
			}
		}

		PositionScaleBias[0] = PositionMax - PositionMin;
		PositionScaleBias[1] = PositionMin;
		ScaleScaleBias[0] = ScaleMax - ScaleMin;
		ScaleScaleBias[1] = ScaleMin;
		ColorScaleBias[0] = ColorMax - ColorMin;
		ColorScaleBias[1] = ColorMin;
		SHScaleBias[0] = SHMax - SHMin;
		SHScaleBias[1] = SHMin;
	}

	// Helper functions
	auto QuantizeFloat =
		[](const uint32 NumBits, float Value, float RangeScale, float RangeBias, float& OutError) -> uint32
		{
			if (RangeScale == 0.0f)
			{
				return 0;
			}

			const uint32 iQ = (1 << NumBits) - 1;
			const float Q = float(iQ);
			const uint32 V = FMath::Clamp(FMath::RoundToInt((Value - RangeBias) / RangeScale * Q), 0, iQ);
			const float R = ((float(V) / Q) * RangeScale) + RangeBias;
			OutError = FMath::Max(OutError, FMath::Abs(R - Value));
			return V;
		};

	// Mirrors FGaussianSplatShared::Unpack_21_21_21
	auto QuantizePosition =
		[&QuantizeFloat](const FVector3f& Value, const FVector3f& RangeScale, const FVector3f& RangeBias, float& OutError, uint32& OutWord0, uint32& OutWord1)
		{
			const uint32 QX = QuantizeFloat(21, Value.X, RangeScale.X, RangeBias.X, OutError);
			const uint32 QY = QuantizeFloat(21, Value.Y, RangeScale.Y, RangeBias.Y, OutError);
			const uint32 QZ = QuantizeFloat(21, Value.Z, RangeScale.Z, RangeBias.Z, OutError);
			const uint64 P = (uint64(QX) << 42) | (uint64(QY) << 21) | uint64(QZ);
			OutWord0 = uint32(P & 0xFFFFFFFFull);
			OutWord1 = uint32(P >> 32);
		};

	// Mirrors FGaussianSplatShared::Unpack_11_11_10_Exp
	auto QuantizeScaleLog =
		[&QuantizeFloat](const FVector3f& LogValue, const FVector3f& RangeScale, const FVector3f& RangeBias, float& OutError) -> uint32
		{
			uint32 V;
			V  = QuantizeFloat(11, LogValue.X, RangeScale.X, RangeBias.X, OutError) << 21;
			V |= QuantizeFloat(11, LogValue.Y, RangeScale.Y, RangeBias.Y, OutError) << 10;
			V |= QuantizeFloat(10, LogValue.Z, RangeScale.Z, RangeBias.Z, OutError) << 0;
			return V;
		};

	auto QuantizeVec4 =
		[&QuantizeFloat](const uint32 NumBits, const FVector4f& Value, const FVector4f& RangeScale, const FVector4f& RangeBias, float& OutError) -> uint32
		{
			uint32 V;
			V  = QuantizeFloat(NumBits, Value.X, RangeScale.X, RangeBias.X, OutError) << (NumBits * 3);
			V |= QuantizeFloat(NumBits, Value.Y, RangeScale.Y, RangeBias.Y, OutError) << (NumBits * 2);
			V |= QuantizeFloat(NumBits, Value.Z, RangeScale.Z, RangeBias.Z, OutError) << (NumBits * 1);
			V |= QuantizeFloat(NumBits, Value.W, RangeScale.W, RangeBias.W, OutError) << (NumBits * 0);
			return V;
		};

	auto QuantizeQuat =
		[&QuantizeFloat](const FQuat4f& InValue, float& OutError) -> uint32
		{
			FQuat4f Value = InValue;
			if (Value.W < 0.0f)
			{
				Value.X = -Value.X;
				Value.Y = -Value.Y;
				Value.Z = -Value.Z;
				Value.W = -Value.W;
			}
			Value.Normalize();

			uint32 V = 0;
			V  = QuantizeFloat(11, Value.X, 2.0f, -1.0f, OutError) << 21;
			V |= QuantizeFloat(11, Value.Y, 2.0f, -1.0f, OutError) << 10;
			V |= QuantizeFloat(10, Value.Z, 2.0f, -1.0f, OutError) << 0;
			return V;
		};

	// Write compressed data
	float PositionError = 0.0f;
	float ScaleError = 0.0f;
	float ColorError = 0.0f;
	float RotationError = 0.0f;
	float SHError = 0.0f;

	for (int32 iSplat = 0; iSplat < NumSplats; ++iSplat)
	{
		const FGaussianSplatFormatFloat::FBaseData& BaseData = FloatData.BaseData[iSplat];
		const uint32 SHDataOffset = iSplat * SHDimension;
		const FVector4f Color = FVector4f(FloatData.SHData[SHDataOffset], BaseData.Opacity);

		uint32 OutputOffset = iSplat * ElementSize;
		QuantizePosition(BaseData.Position, PositionScaleBias[0], PositionScaleBias[1], PositionError, Data[OutputOffset + 0], Data[OutputOffset + 1]);
		Data[OutputOffset + 2] = QuantizeScaleLog(LogScale(BaseData.Scale), ScaleScaleBias[0], ScaleScaleBias[1], ScaleError);
		Data[OutputOffset + 3] = QuantizeQuat(BaseData.Rotation, RotationError);
		Data[OutputOffset + 4] = QuantizeVec4(8, Color, ColorScaleBias[0], ColorScaleBias[1], ColorError);

		// Note: not efficient we waste a byte per coef
		for (int32 iSHCoef = 1; iSHCoef < SHDimension; ++iSHCoef)
		{
			Data[(OutputOffset + 4) + iSHCoef] = QuantizeVec4(8, FloatData.SHData[SHDataOffset + iSHCoef], SHScaleBias[0], SHScaleBias[1], SHError);
		}
	}

	PositionScaleBias[0] = PositionScaleBias[0] / float((1 << 21) - 1);
	ScaleScaleBias[0].X = ScaleScaleBias[0].X / float((1 << 11) - 1);
	ScaleScaleBias[0].Y = ScaleScaleBias[0].Y / float((1 << 11) - 1);
	ScaleScaleBias[0].Z = ScaleScaleBias[0].Z / float((1 << 10) - 1);
	ColorScaleBias[0] = ColorScaleBias[0] / float((1 << 8) - 1);
	SHScaleBias[0] = SHScaleBias[0] / float((1 << 8) - 1);

	const float ScaleRelativeError = (FMath::Exp(ScaleError) - 1.0f) * 100.0f;
	UE_LOGF(LogTemp, Log, "Quantization Errors: Position(%f) Scale(%.2f%%) Color(%f) Rotation(%f) SH(%f)", PositionError, ScaleRelativeError, ColorError, RotationError, SHError);
}

#endif
