// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GaussianSplatCommon.h"
#include "GaussianSplatFormats.h"

#include "StructUtils/SharedStruct.h"
#include "RenderGraphResources.h"
#include "RenderResource.h"

#define UE_API GAUSSIANSPLAT_API

class FGaussianSplatRenderResource : public FRenderResource
{
	using Super = FRenderResource;

public:
	//-TODO: Need to be able to create a placeholder resource for Niagara to feed into
	explicit FGaussianSplatRenderResource(const FBox3f& InBounds, int32 InNumSplats, int32 InNumLOD0Splats, int32 InSHDegree, EGaussianSplatCoordinateSpace InSHCoordinateSpace, const FSharedStruct& InResourceData, FName InAssetName);

	// BEGIN: FRenderResource Impl
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual void ReleaseRHI() override;
	virtual FString GetFriendlyName() const override;
	// END: FRenderResource Impl

	const FBox3f& GetBounds() const { return Bounds; }
	int32 GetNumSplats() const { return NumSplats; }
	int32 GetNumLOD0Splats() const { return NumLOD0Splats; }
	int32 GetSHDegree() const { return SHDegree; }
	EGaussianSplatCoordinateSpace GetSHCoordinateSpace() const { return SHCoordinateSpace; }

	uint32 GetSplatStride() const { return SplatStride; }

	// Zero when the asset was built without clustering
	uint32 GetNumClusters() const { return NumClusters; }

	static UE_API uint64 GetSplatStrideForFormat(int32 SHDimension, bool bQuantizedFormat);

	static UE_API uint64 GetResourceSizeLimitInBytes();

	// Get the runtime data struct backing both buffers, not guaranteed to be valid
	const FSharedStruct& GetRuntimeData() const { return ResourceData; }

	// Gpu accessible data, only valid on the render thread(s)
	const TRefCountPtr<FRDGPooledBuffer>& GetSplatBuffer() { check(IsInRenderingThread() || IsInParallelRenderingThread()); return SplatBuffer; }

	// Null when the asset was built without clustering
	const TRefCountPtr<FRDGPooledBuffer>& GetClusterBuffer() { check(IsInRenderingThread() || IsInParallelRenderingThread()); return ClusterBuffer; }

	// Returns a pointer to an empty resource
	// Can be used to bind valid data to the GPU in the case of needing dummy data
	static UE_API TSharedPtr<FGaussianSplatRenderResource> GetEmptyResource();

	// Explicitly releases the empty resource, must be called before RHI shutdown
	// Call from module ShutdownModule() to ensure ordering before RHIExit()
	static void ReleaseEmptyResource();

private:
	// Callers outside the class pick a format through GetSplatStrideForFormat
	static uint64 GetSplatStrideForFloatFormat(int32 SHDimension);
	static uint64 GetSplatStrideForQuantizedFormat(int32 SHDimension);

	// Allocates placeholder buffer to avoid binding failure
	void InitNullBuffer(FRHICommandListBase& RHICmdList);

	// Allocates GPU buffer for splat data
	bool InitSplatBuffer(FRHICommandListBase& RHICmdList, uint32 SplatOffset);

	// Allocates GPU buffer for cluster data
	void InitClusterBuffer(FRHICommandListBase& RHICmdList, uint32 ClusterIndex, uint32 SplatIndex, uint32 LevelShift);

	FString CreateAllocationFailureString(uint64 RequiredBytes, uint64 MaxBytes) const;

	FBox3f			Bounds = FBox3f(EForceInit::ForceInit);
	int32			NumSplats = 0;
	int32			NumLOD0Splats = 0;
	int32			SHDegree = 0;
	EGaussianSplatCoordinateSpace SHCoordinateSpace = EGaussianSplatCoordinateSpace::LH_FRU;
	FSharedStruct	ResourceData;
	FName			AssetName;

	uint32							SplatStride = 0;
	uint32							NumClusters = 0;
	TRefCountPtr<FRDGPooledBuffer>	SplatBuffer;
	TRefCountPtr<FRDGPooledBuffer>	ClusterBuffer;
};

#undef UE_API
