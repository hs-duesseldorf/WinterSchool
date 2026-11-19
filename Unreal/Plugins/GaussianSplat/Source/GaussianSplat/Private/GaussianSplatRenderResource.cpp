// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatRenderResource.h"

#include "GaussianSplatShared.h"
#include "GaussianSplatCommon.h"
#include "GaussianSplatFormats.h"

#include "Containers/ArrayView.h"
#include "HAL/IConsoleManager.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGlobals.h"

#include "Internationalization/Text.h"

static TAutoConsoleVariable<int32> CVarGaussianSplatMaxResourceSizeMB(
	TEXT("r.GaussianSplat.Debug.MaxResourceSizeMB"),
	0,
	TEXT("Maximum size (in MiB) for a single Gaussian Splat GPU resource\n")
	TEXT("0 = RHI limit (default)"),
	ECVF_RenderThreadSafe
);

static UE::FMutex GEmptyResourceGuard;
static TSharedPtr<FGaussianSplatRenderResource> GEmptyResource;

FGaussianSplatRenderResource::FGaussianSplatRenderResource(const FBox3f& InBounds, int32 InNumSplats, int32 InNumLOD0Splats, int32 InSHDegree, EGaussianSplatCoordinateSpace InSHCoordinateSpace, const FSharedStruct& InResourceData, FName InAssetName)
	: Bounds(InBounds)
	, NumSplats(InNumSplats)
	, NumLOD0Splats(FMath::Clamp(InNumLOD0Splats, 0, InNumSplats))
	, SHDegree(InSHDegree)
	, SHCoordinateSpace(InSHCoordinateSpace)
	, ResourceData(InResourceData)
	, AssetName(InAssetName)
{
	SetOwnerName(InAssetName);
}

FString FGaussianSplatRenderResource::GetFriendlyName() const
{
	if (AssetName.IsNone())
	{
		return TEXT("FGaussianSplatRenderResource");
	}
	return FString::Printf(TEXT("FGaussianSplatRenderResource %s"), *AssetName.ToString());
}

uint64 FGaussianSplatRenderResource::GetSplatStrideForFloatFormat(int32 SHDimension)
{
	return uint64(FGaussianSplatShared::EFloat_Stride) + uint64(SHDimension - 1) * sizeof(FVector3f);
}

uint64 FGaussianSplatRenderResource::GetSplatStrideForQuantizedFormat(int32 SHDimension)
{
	return uint64(FGaussianSplatShared::EQuantized_Stride) + uint64(SHDimension - 1) * sizeof(uint32);
}

uint64 FGaussianSplatRenderResource::GetSplatStrideForFormat(int32 SHDimension, bool bQuantizedFormat)
{
	return bQuantizedFormat
		? GetSplatStrideForQuantizedFormat(SHDimension)
		: GetSplatStrideForFloatFormat(SHDimension);
}

uint64 FGaussianSplatRenderResource::GetResourceSizeLimitInBytes()
{
	uint64 Limit = FMath::Min<uint64>(GRHIGlobals.MaxViewSizeBytesForNonTypedBuffer, MAX_uint32);

	const int32 MaxResourceSizeMB = CVarGaussianSplatMaxResourceSizeMB.GetValueOnAnyThread();
	if (MaxResourceSizeMB > 0)
	{
		Limit = FMath::Min<uint64>(Limit, uint64(MaxResourceSizeMB) << 20);
	}
	return Limit;
}

FString FGaussianSplatRenderResource::CreateAllocationFailureString(uint64 RequiredBytes, uint64 MaxBytes) const
{
	return FString::Printf(
		TEXT("Allocation failure. Asset %s (splats=%s at SH=%d) requires %s (%s maximum allowed)."),
		*AssetName.ToString(), *FText::AsNumber(NumSplats).ToString(), SHDegree,
		*FText::AsMemory(RequiredBytes, EMemoryUnit::Unit_MiB).ToString(),
		*FText::AsMemory(MaxBytes, EMemoryUnit::Unit_MiB).ToString());
}

static int32 CalcLargestLevelForResourceLimit(const FGaussianSplatFormat& Format, const TArray<FGaussianSplatFormat::FLevelData>& LevelTable, uint64 SplatStride, uint64 ResourceLimitInBytes)
{
	for (int32 LevelIndex = 0; LevelIndex < LevelTable.Num(); ++LevelIndex)
	{
		FGaussianSplatFormat::FLevelData LevelData = LevelTable[LevelIndex];
		uint32 SplatsRemaining = Format.NumSplats - LevelData.SplatOffset;
		const uint64 SplatsInBytes = SplatsRemaining * SplatStride;
		if (SplatsInBytes <= ResourceLimitInBytes)
		{
			return LevelIndex;
		}
	}

	return -1;
}

void FGaussianSplatRenderResource::InitNullBuffer(FRHICommandListBase& RHICmdList)
{
	NumSplats = 0;
	NumLOD0Splats = 0;
	SplatStride = 0;
	NumClusters = 0;

	const uint64 BufferSize = FGaussianSplatShared::EFloat_Stride;
	SplatBuffer = AllocatePooledBuffer(FRDGBufferDesc::CreateByteAddressDesc(BufferSize), TEXT("GaussianSplatRenderResource_Null"));
	uint8* UploadMemory = static_cast<uint8*>(RHICmdList.LockBuffer(SplatBuffer->GetRHI(), 0, BufferSize, RLM_WriteOnly));
	FMemory::Memset(UploadMemory, 0, BufferSize);
	RHICmdList.UnlockBuffer(SplatBuffer->GetRHI());
}

void FGaussianSplatRenderResource::InitRHI(FRHICommandListBase& RHICmdList)
{
	Super::InitRHI(RHICmdList);

	const FGaussianSplatFormat* Format = ResourceData.GetPtr<FGaussianSplatFormat>();
	if (Format == nullptr)
	{
		InitNullBuffer(RHICmdList);
		return;
	}

	const bool bQuantizedFormat = ResourceData.GetPtr<FGaussianSplatFormatQuantized>() != nullptr;
	SplatStride = uint32(GetSplatStrideForFormat(GaussianSplatCommon::GetSHDimension(SHDegree), bQuantizedFormat));

	// Construct hierarchy table
	TArray<FGaussianSplatFormat::FLevelData> LevelTable;
	if (!Format->BuildLevelTable(LevelTable))
	{
		LevelTable.AddDefaulted();
		LevelTable[0].SplatCount = uint32(NumSplats);
		LevelTable[0].ClusterCount = uint32(Format->Clusters.Num());
	}

	// If the complete hierarchy can't load within the active RHI's limits, truncate it
	const uint64 ResourceLimitInBytes = GetResourceSizeLimitInBytes();
	const int32 Level = CalcLargestLevelForResourceLimit(*Format, LevelTable, SplatStride, ResourceLimitInBytes);
	if (Level < 0)
	{
		ensureMsgf(false, TEXT("%s"), *CreateAllocationFailureString(uint64(NumSplats) * uint64(SplatStride), ResourceLimitInBytes));
		InitNullBuffer(RHICmdList);
		return;
	}

	const FGaussianSplatFormat::FLevelData& LevelData = LevelTable[Level];
	NumSplats = NumSplats - int32(LevelData.SplatOffset);
	if (InitSplatBuffer(RHICmdList, LevelData.SplatOffset))
	{
		NumLOD0Splats = LevelData.SplatCount;
		InitClusterBuffer(RHICmdList, LevelData.ClusterOffset, LevelData.SplatOffset, uint32(Level));
	}
}

bool FGaussianSplatRenderResource::InitSplatBuffer(FRHICommandListBase& RHICmdList, uint32 SplatOffset)
{
	const int32 SHDimension = GaussianSplatCommon::GetSHDimension(SHDegree);
	if (const FGaussianSplatFormatFloat* FloatFormat = ResourceData.GetPtr<FGaussianSplatFormatFloat>())
	{
		if (NumSplats == 0)
		{
			InitNullBuffer(RHICmdList);
			return false;
		}

		const uint64 BufferSize = uint64(NumSplats) * uint64(SplatStride);
		const uint64 EndSplat = uint64(SplatOffset) + uint64(NumSplats);
		if (EndSplat > uint64(FloatFormat->BaseData.Num()) || EndSplat * uint64(SHDimension) > uint64(FloatFormat->SHData.Num()))
		{
			ensureMsgf(false, TEXT("Corrupted splat data for GaussianSplat: %s"), *AssetName.ToString());

			InitNullBuffer(RHICmdList);
			return false;
		}

		SplatBuffer = AllocatePooledBuffer(FRDGBufferDesc::CreateByteAddressDesc(BufferSize), TEXT("GaussianSplatRenderResource_Float"));

		uint8* UploadMemory = static_cast<uint8*>(RHICmdList.LockBuffer(SplatBuffer->GetRHI(), 0, BufferSize, RLM_WriteOnly));
		for (int32 iSplat = 0; iSplat < NumSplats; ++iSplat)
		{
			const FGaussianSplatFormatFloat::FBaseData& BaseData = FloatFormat->BaseData[SplatOffset + iSplat];
			const FVector3f* SHData = &FloatFormat->SHData[(SplatOffset + iSplat) * SHDimension];

			const uint64 WriteOffset = uint64(iSplat) * uint64(SplatStride);
			FMemory::Memcpy(&UploadMemory[WriteOffset + FGaussianSplatShared::EFloat_PositionOffset],			&BaseData.Position,	sizeof(FVector3f));
			FMemory::Memcpy(&UploadMemory[WriteOffset + FGaussianSplatShared::EFloat_ScaleOffset],				&BaseData.Scale,	sizeof(FVector3f));
			FMemory::Memcpy(&UploadMemory[WriteOffset + FGaussianSplatShared::EFloat_RotationOffset],			&BaseData.Rotation,	sizeof(FQuat4f));
			FMemory::Memcpy(&UploadMemory[WriteOffset + FGaussianSplatShared::EFloat_ColorOpacityOffset],		&SHData[0],			sizeof(FVector3f));
			FMemory::Memcpy(&UploadMemory[WriteOffset + FGaussianSplatShared::EFloat_ColorOpacityOffset + 12u],	&BaseData.Opacity,	sizeof(float));

			uint64 SHWriteOffset = WriteOffset + FGaussianSplatShared::EFloat_Stride;
			for (int32 iCoef = 1; iCoef < SHDimension; ++iCoef)
			{
				FMemory::Memcpy(&UploadMemory[SHWriteOffset], &SHData[iCoef], sizeof(FVector3f));
				SHWriteOffset += sizeof(FVector3f);
			}
		}
		RHICmdList.UnlockBuffer(SplatBuffer->GetRHI());
		return true;
	}
	else if (const FGaussianSplatFormatQuantized* QuantizedFormat = ResourceData.GetPtr<FGaussianSplatFormatQuantized>())
	{
		if (NumSplats == 0)
		{
			InitNullBuffer(RHICmdList);
			return false;
		}

		const uint64 BufferSize = uint64(NumSplats) * uint64(SplatStride);
		const uint64 DataOffset = uint64(SplatOffset) * uint64(SplatStride);

		const uint64 DataSize = uint64(QuantizedFormat->Data.Num()) * QuantizedFormat->Data.GetTypeSize();
		if (DataOffset + BufferSize > DataSize)
		{
			ensureMsgf(false, TEXT("Corrupted splat data for GaussianSplat: %s"), *AssetName.ToString());

			InitNullBuffer(RHICmdList);
			return false;
		}

		SplatBuffer = AllocatePooledBuffer(FRDGBufferDesc::CreateByteAddressDesc(BufferSize), TEXT("GaussianSplatRenderResource_Quantized"));

		void* UploadMemory = RHICmdList.LockBuffer(SplatBuffer->GetRHI(), 0, BufferSize, RLM_WriteOnly);
		FMemory::Memcpy(UploadMemory, reinterpret_cast<const uint8*>(QuantizedFormat->Data.GetData()) + DataOffset, BufferSize);
		RHICmdList.UnlockBuffer(SplatBuffer->GetRHI());
		return true;
	}

	InitNullBuffer(RHICmdList);
	return false;
}

void FGaussianSplatRenderResource::InitClusterBuffer(FRHICommandListBase& RHICmdList, uint32 ClusterIndex, uint32 SplatIndex, uint32 LevelShift)
{
	const FGaussianSplatFormat* Format = ResourceData.GetPtr<FGaussianSplatFormat>();
	NumClusters = Format ? uint32(Format->Clusters.Num()) : 0;
	if (NumClusters == 0)
	{
		return;
	}

	TConstArrayView<FGaussianSplatCluster> Clusters = Format->Clusters;
	TArray<FGaussianSplatCluster> TruncatedClusters;
	if (ClusterIndex > 0 || SplatIndex > 0 || LevelShift > 0)
	{
		TruncatedClusters = Format->Clusters;
		FGaussianSplatFormat::TruncateClusters(TruncatedClusters, ClusterIndex, SplatIndex, LevelShift);
		Clusters = TruncatedClusters;
	}
	NumClusters = uint32(Clusters.Num());

	uint64 SplatCountSum = 0;
	for (int32 Index = 0; Index < Clusters.Num(); ++Index)
	{
		const FGaussianSplatCluster& Cluster = Clusters[Index];
		const uint64 SplatIndexEnd = uint64(Cluster.SplatOffset) + uint64(Cluster.SplatCount);
		SplatCountSum += uint64(Cluster.SplatCount);

		const bool bParentValid = Cluster.ParentIndex == ~0u || Cluster.ParentIndex < NumClusters;
		if (Cluster.SplatCount == 0 || SplatIndexEnd > uint64(NumSplats) || SplatCountSum > uint64(NumSplats) || !bParentValid)
		{
			UE_LOGF(LogTemp, Error, "Corrupted cluster table for GaussianSplat: %ls", *AssetName.ToString());
			ensureMsgf(false, TEXT("Corrupted cluster table for GaussianSplat: %s"), *AssetName.ToString());

			NumClusters = 0;
			return;
		}
	}

	const uint64 BufferSize = uint64(NumClusters) * sizeof(FGaussianSplatCluster);

	ClusterBuffer = AllocatePooledBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FGaussianSplatCluster), NumClusters),
		TEXT("GaussianSplatRenderResource_Clusters"));

	void* UploadMemory = RHICmdList.LockBuffer(ClusterBuffer->GetRHI(), 0, BufferSize, RLM_WriteOnly);
	FMemory::Memcpy(UploadMemory, Clusters.GetData(), BufferSize);
	RHICmdList.UnlockBuffer(ClusterBuffer->GetRHI());
}

void FGaussianSplatRenderResource::ReleaseRHI()
{
	Super::ReleaseRHI();
	SplatBuffer = nullptr;
	ClusterBuffer = nullptr;
	NumClusters = 0;
	ResourceData = FSharedStruct();
}

// Returns a pointer to an empty resource
// Can be used to bind valid data to the GPU in the case of needing dummy data
TSharedPtr<FGaussianSplatRenderResource> FGaussianSplatRenderResource::GetEmptyResource()
{
	check(IsInGameThread());

	UE::TScopeLock Lock(GEmptyResourceGuard);

	if (GEmptyResource.IsValid() == false)
	{
		GEmptyResource = MakeShareable(new FGaussianSplatRenderResource(FBox3f(EForceInit::ForceInitToZero), 0, 0, 0, EGaussianSplatCoordinateSpace::LH_FRU, FSharedStruct(), NAME_None));
		BeginInitResource(GEmptyResource.Get());
	}
	return GEmptyResource;
}

void FGaussianSplatRenderResource::ReleaseEmptyResource()
{
	check(IsInGameThread());

	UE::TScopeLock Lock(GEmptyResourceGuard);

	if (GEmptyResource.IsValid())
	{
		BeginReleaseResource(GEmptyResource.Get());
		FlushRenderingCommands();
		GEmptyResource.Reset();
	}
}
