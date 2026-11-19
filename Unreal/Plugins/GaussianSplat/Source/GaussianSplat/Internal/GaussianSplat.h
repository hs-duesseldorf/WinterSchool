// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GaussianSplatCommon.h"
#include "GaussianSplatFormats.h"

#include "Interfaces/Interface_AssetUserData.h"
#include "RenderCommandFence.h"
#include "StructUtils/SharedStruct.h"
#include "UObject/PerPlatformProperties.h"

#include "GaussianSplat.generated.h"

#define UE_API GAUSSIANSPLAT_API

struct FGaussianSplatBuilder;
class FGaussianSplatRenderResource;
class UAssetImportData;
class UAssetUserData;

namespace GaussianSplat
{
	UE_API bool IsEnabled();
}

UCLASS(BlueprintType, MinimalAPI, meta = (Valkyrie_DevelopmentStatus = "Experimental", UEFN_DevelopmentStatus = "Experimental"))
class UGaussianSplat : public UObject, public IInterface_AssetUserData
{
	GENERATED_BODY()

public:
#if WITH_EDITORONLY_DATA
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnDataChanged, UGaussianSplat*);
#endif

public:
	// Begin: UObject Impl
	virtual void PostInitProperties() override;
	virtual void Serialize(FArchive& Ar) override;
	virtual void GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize) override;
#if WITH_EDITORONLY_DATA
	virtual void PostLoad() override;
	static void AppendToClassSchema(FAppendToClassSchemaContext& Context);
	virtual void GetAssetRegistryTags(FAssetRegistryTagsContext Context) const override;
#endif
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	virtual void BeginDestroy() override;
	virtual bool IsReadyForFinishDestroy() override;
	virtual void FinishDestroy() override;
	// End: UObject Impl

	//~ Begin IInterface_AssetUserData Interface
	virtual void AddAssetUserData(UAssetUserData* InUserData) override;
	virtual void RemoveUserDataOfClass(TSubclassOf<UAssetUserData> InUserDataClass) override;
	virtual UAssetUserData* GetAssetUserDataOfClass(TSubclassOf<UAssetUserData> InUserDataClass) override;
	virtual const TArray<UAssetUserData*>* GetAssetUserDataArray() const override;
	//~ End IInterface_AssetUserData Interface

#if WITH_EDITORONLY_DATA
	UE_API uint64 GetRawMemoryUsage() const;
#endif
	UE_API uint64 GetRuntimeMemoryUsage() const;

	int32 GetNumSplats() const { return NumSplats; }

	FBox3f GetBounds() const { return Bounds; }

	int32 GetSHDegree() const { return SHDegree; }

	UE_API int32 GetSHDimension() const;

	EGaussianSplatCoordinateSpace GetSHCoordinateSpace() const { return SHCoordinateSpace; }

	const UScriptStruct* GetRuntimeDataType() const { return RuntimeDataStruct.GetScriptStruct(); }
	template<typename T> const T* GetRuntimeData() const { return RuntimeDataStruct.GetPtr<T>(); }

	UE_API TSharedPtr<FGaussianSplatRenderResource> GetResource() const;

#if WITH_EDITORONLY_DATA
	// Get / set using quantized data or not
	bool GetUseQuantizedFormat() const { return bUseQuantizedFormat; }
	UE_API void SetUseQuantizedFormat(bool bValue);

	// Set the gaussian data from the builder
	// This will create any derived data / dirty the package / etc
	UE_API void SetData(FGaussianSplatBuilder& InBuilder);

	// Callback to notify any users that data has changed for a splat
	UE_API static FOnDataChanged& GetOnDataChanged();

	UAssetImportData* GetAssetImportData() { return AssetImportData; }

	// Number of LOD levels in the runtime data
	UE_API int32 GetNumLODLevels() const;

	// Creates a transient copy for preview
	UE_API UGaussianSplat* CreatePreview(int32 InMinLOD, int32 InSHDegree) const;

#if WITH_DEV_AUTOMATION_TESTS
	/** Exposes the source-data cache key without exporting the private key builder. */
	UE_API FString BuildDerivedDataKeyForAutomationTest() const;
#endif
#endif //WITH_EDITORONLY_DATA

private:
#if WITH_EDITORONLY_DATA
	UScriptStruct* GetRuntimeFormatStruct() const;

	// Drops the render resource and waits for the release to complete
	void ReleaseResourceAndWait();

	// Builds the key to look up into the derived data cache
	FString BuildDerivedDataKey() const;

	// Builds the derived data
	void BuildDerivedData();

	// Caches the derived data, it if exists otherwise builds it
	void CacheDerivedData();

	FGaussianSplatFormatFloat& GetEditorData() { return EditorDataStruct.Get<FGaussianSplatFormatFloat>(); }
	const FGaussianSplatFormatFloat& GetEditorData() const { return EditorDataStruct.Get<FGaussianSplatFormatFloat>(); }
#endif

private:
#if WITH_EDITORONLY_DATA
	UPROPERTY(VisibleAnywhere, Instanced, Category = ImportSettings)
	TObjectPtr<class UAssetImportData> AssetImportData;
#endif

	UPROPERTY()
	FBox3f Bounds = FBox3f(EForceInit::ForceInit);

	UPROPERTY()
	int32 NumSplats = 0;

	UPROPERTY()
	int32 SHDegree = 0;

	UPROPERTY(VisibleAnywhere, Category = "Splat Data")
	EGaussianSplatCoordinateSpace SHCoordinateSpace = EGaussianSplatCoordinateSpace::LH_FRU;

	UPROPERTY(EditAnywhere, Category = "LOD", meta = (DisplayName = "Min LOD"))
	FPerPlatformInt MinLOD = 0;

	/** Array of user data stored with the asset */
	UPROPERTY(EditAnywhere, AdvancedDisplay, Instanced, Category = "User Data")
	TArray<TObjectPtr<UAssetUserData>> AssetUserData;

	UPROPERTY(EditAnywhere, Category = "Splat Data", meta = (DisplayName = "Max SH Degree", ClampMin = "0", ClampMax = "3"))
	FPerPlatformInt MaxSHDegree = 3;

#if WITH_EDITORONLY_DATA
	// Compress the runtime data. PostEditChangeProperty rebuilds the derived data when this changes.
	UPROPERTY(EditAnywhere, Category = "Splat Data")
	bool bUseQuantizedFormat = true;

	// How the runtime splats are grouped into clusters for culling.
	UPROPERTY(EditAnywhere, Category = "Splat Data")
	EGaussianSplatClusterGrouping ClusterGrouping = EGaussianSplatClusterGrouping::Sequential;

	// Maximum splats allowed per cluster.
	UPROPERTY(EditAnywhere, Category = "Splat Data", meta = (ClampMin = "16", ClampMax = "1024"))
	int32 MaxSplatsPerCluster = 64;

	// How child splats are combined when building coarser levels.
	UPROPERTY(EditAnywhere, Category = "Splat Data")
	EGaussianSplatAggregation LODAggregation = EGaussianSplatAggregation::MomentPreserving;

	// Always FGaussianSplatFormatFloat and uses custom serialization for performance
	FSharedStruct EditorDataStruct;

	bool bUpgradedOnLoad = false;
#endif
	FSharedStruct RuntimeDataStruct;
	mutable TSharedPtr<FGaussianSplatRenderResource> Resource;
	FRenderCommandFence ReleaseResourcesFence;
};

#undef UE_API
