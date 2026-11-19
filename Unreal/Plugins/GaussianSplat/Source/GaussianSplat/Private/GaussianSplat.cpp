// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplat.h"
#include "GaussianSplatCommon.h"
#include "GaussianSplatBuilder.h"
#include "GaussianSplatCustomVersion.h"
#include "GaussianSplatMortonOrder.h"
#include "GaussianSplatClusterBuilder.h"
#include "GaussianSplatLevelOfDetailBuilder.h"
#include "GaussianSplatRenderResource.h"

#include "EditorFramework/AssetImportData.h"
#include "Engine/AssetUserData.h"
#include "Hash/Blake3.h"
#include "Misc/AxisDisplayInfo.h"
#include "ProfilingDebugging/ResourceSize.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Templates/SubclassOf.h"
#include "UObject/Package.h"

#if WITH_EDITORONLY_DATA
#include "DerivedDataCacheInterface.h"
#include "Interfaces/ITargetPlatform.h"
#endif

// Key for the gaussian derived data
#define GAUSSIANSPLAT_DERIVED_DATA_VERSION TEXT("990C7264D4584354B847B42CA3BB4ACA")

namespace GaussianSplatPrivate
{
	struct GaussianSplatResourceDeletor
	{
		void operator()(FGaussianSplatRenderResource* ObjectToDelete) const
		{
			if (IsInRenderingThread())
			{
				ObjectToDelete->ReleaseResource();
				delete ObjectToDelete;
			}
			else
			{
				ENQUEUE_RENDER_COMMAND(DeleteResource)
				(
					[ObjectToDelete](FRHICommandList&)
					{
						ObjectToDelete->ReleaseResource();
						delete ObjectToDelete;
					}
				);
			}
		}
	};
}

void UGaussianSplat::PostInitProperties()
{
#if WITH_EDITORONLY_DATA
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		AssetImportData = NewObject<UAssetImportData>(this, TEXT("AssetImportData"));
	}

	EditorDataStruct.InitializeAs<FGaussianSplatFormatFloat>();
#endif

	Super::PostInitProperties();
}

void UGaussianSplat::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	Ar.UsingCustomVersion(FGaussianSplatCustomVersion::GUID);

	// Serialize for targets that include editor data
#if WITH_EDITORONLY_DATA
	if (Ar.IsFilterEditorOnly() == false)
	{
		if (Ar.IsLoading())
		{
			ReleaseResourceAndWait();
		}

		FGaussianSplatFormatFloat& EditorData = GetEditorData();
		EditorData.Serialize(Ar, NumSplats, SHDegree);

		// Upgrade in-memory representation to MortonOrder version
		if (Ar.IsLoading() && Ar.IsPersistent() && Ar.CustomVer(FGaussianSplatCustomVersion::GUID) < FGaussianSplatCustomVersion::MortonOrder)
		{
			GaussianSplat::SortMortonOrder(EditorData.BaseData, EditorData.SHData, GetSHDimension());
			bUpgradedOnLoad = true;
		}

		if (Ar.IsLoading())
		{
			CacheDerivedData();
		}
	}
#endif

	// Serialize for targets that do not include editor data
	if (Ar.IsFilterEditorOnly() && Ar.IsPersistent())
	{
#if WITH_EDITORONLY_DATA
		if (Ar.IsSaving() && !RuntimeDataStruct.IsValid())
		{
			CacheDerivedData();
		}
#endif

		TObjectPtr<UScriptStruct> RuntimeStructType = RuntimeDataStruct.IsValid() ? const_cast<UScriptStruct*>(RuntimeDataStruct.GetScriptStruct()) : nullptr;
		Ar << RuntimeStructType;

		if (RuntimeStructType == nullptr)
		{
			if (Ar.IsLoading())
			{
				UE_LOGF(LogTemp, Error, "Gaussian Splat '%ls' could not be loaded.", *GetPathName());
				Ar.SetError();
			}

			RuntimeDataStruct.Reset();
			return;
		}

		if (Ar.IsLoading())
		{
			RuntimeDataStruct.InitializeAs(RuntimeStructType, nullptr);
		}

#if WITH_EDITOR
		if (Ar.IsSaving() && Ar.IsCooking() && Ar.CookingTarget() != nullptr && RuntimeDataStruct.IsValid())
		{
			// Remove splat data for platforms that don't render (eg: servers)
			if (!Ar.CookingTarget()->AllowAudioVisualData())
			{
				FSharedStruct EmptyData = FSharedStruct::Make(RuntimeStructType, nullptr);
				EmptyData.Get<FGaussianSplatFormat>().Serialize(Ar, 0, 0);
				return;
			}

			// WinterSchool fork: a cook flavor's own entry wins over its platform's (Wasm_ASTC, the phone/tablet texture set of
			// a web build, can carry fewer splats than Wasm, the desktop set, though both read the Wasm ini)
			const FName FlavorName(*Ar.CookingTarget()->PlatformName());
			const FName IniName(*Ar.CookingTarget()->IniPlatformName());
			auto ValueFor = [&](const FPerPlatformInt& Value)
			{
				const int32* Flavor = Value.PerPlatform.Find(FlavorName);
				return Flavor ? *Flavor : Value.GetValueForPlatform(IniName);
			};
			const int32 PlatformMinLOD = FMath::Max(ValueFor(MinLOD), 0);
			const int32 PlatformSHDegree = FMath::Clamp(ValueFor(MaxSHDegree), 0, SHDegree);
			if (PlatformMinLOD > 0 || PlatformSHDegree < SHDegree)
			{
				FSharedStruct CookedData = FSharedStruct::Make(RuntimeStructType, RuntimeDataStruct.GetMemory());

				FGaussianSplatFormat& CookedFormat = CookedData.Get<FGaussianSplatFormat>();
				CookedFormat.Truncate(PlatformMinLOD, PlatformSHDegree);
				// NumSplats is an editor value (not a cook-time value) and is unused at this call-site
				CookedFormat.Serialize(Ar, NumSplats, PlatformSHDegree);
				return;
			}
		}
#endif

		RuntimeDataStruct.Get<FGaussianSplatFormat>().Serialize(Ar, NumSplats, SHDegree);
	}
}

void UGaussianSplat::GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize)
{
	Super::GetResourceSizeEx(CumulativeResourceSize);

	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(GetRuntimeMemoryUsage());

#if WITH_EDITORONLY_DATA
	if (EditorDataStruct.IsValid())
	{
		CumulativeResourceSize.AddDedicatedSystemMemoryBytes(GetRawMemoryUsage());
	}
#endif
}

#if WITH_EDITORONLY_DATA
void UGaussianSplat::PostLoad()
{
	Super::PostLoad();

	if (bUpgradedOnLoad)
	{
		bUpgradedOnLoad = false;
		MarkPackageDirty();

		UE_LOGF(LogTemp, Warning, "Gaussian splat asset '%ls' was built with an older version. Resave to upgrade the asset.", *GetPathName());
	}
}

void UGaussianSplat::AppendToClassSchema(FAppendToClassSchemaContext& Context)
{
	Super::AppendToClassSchema(Context);

	Context.Update(GAUSSIANSPLAT_DERIVED_DATA_VERSION, FCString::Strlen(GAUSSIANSPLAT_DERIVED_DATA_VERSION));
}

void UGaussianSplat::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
	if (AssetImportData)
	{
		Context.AddTag(FAssetRegistryTag(SourceFileTagName(), AssetImportData->GetSourceData().ToJson(), FAssetRegistryTag::TT_Hidden));
	}

	Context.AddTag(FAssetRegistryTag(TEXT("NumSplats"), FString::FromInt(GetNumSplats()), FAssetRegistryTag::TT_Numerical));
	Context.AddTag(FAssetRegistryTag(TEXT("SHDegree"), FString::FromInt(GetSHDegree()), FAssetRegistryTag::TT_Numerical));
	//Context.AddTag(FAssetRegistryTag(TEXT("MemoryUsage"), FString::FromInt(float(GetRawMemoryUsage()) / (1024.0f * 1024.0f)), FAssetRegistryTag::TT_Numerical));

	if (Bounds.IsValid)
	{
		Context.AddTag(FAssetRegistryTag(TEXT("Dimensions"), AxisDisplayInfo::FormatSpatialExtent(FVector(Bounds.GetSize())), FAssetRegistryTag::TT_Dimensional));
	}

	Super::GetAssetRegistryTags(Context);
}
#endif
#if WITH_EDITOR
void UGaussianSplat::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Disable derived data build during interactive edits
	if (PropertyChangedEvent.ChangeType == EPropertyChangeType::Interactive)
	{
		return;
	}

	const FName PropertyName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplat, bUseQuantizedFormat) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplat, ClusterGrouping) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplat, MaxSplatsPerCluster) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplat, LODAggregation))
	{
		ReleaseResourceAndWait();
		CacheDerivedData();
		GetOnDataChanged().Broadcast(this);
	}
}
#endif

void UGaussianSplat::BeginDestroy()
{
	if (Resource.IsValid())
	{
		BeginReleaseResource(Resource.Get());
		ReleaseResourcesFence.BeginFence();
	}

	Super::BeginDestroy();
}

bool UGaussianSplat::IsReadyForFinishDestroy()
{
	return Super::IsReadyForFinishDestroy() && ReleaseResourcesFence.IsFenceComplete();
}

void UGaussianSplat::FinishDestroy()
{
	Resource = nullptr;
	Super::FinishDestroy();
}

void UGaussianSplat::AddAssetUserData(UAssetUserData* InUserData)
{
	if (InUserData)
	{
		if (UAssetUserData* ExistingData = GetAssetUserDataOfClass(InUserData->GetClass()))
		{
			AssetUserData.Remove(ExistingData);
		}
		AssetUserData.Add(InUserData);
	}
}

void UGaussianSplat::RemoveUserDataOfClass(TSubclassOf<UAssetUserData> InUserDataClass)
{
	for (int32 DataIndex = 0; DataIndex < AssetUserData.Num(); ++DataIndex)
	{
		const UAssetUserData* Datum = AssetUserData[DataIndex];
		if (Datum && Datum->IsA(InUserDataClass))
		{
			AssetUserData.RemoveAt(DataIndex);
			return;
		}
	}
}

UAssetUserData* UGaussianSplat::GetAssetUserDataOfClass(TSubclassOf<UAssetUserData> InUserDataClass)
{
	for (UAssetUserData* Datum : AssetUserData)
	{
		if (Datum && Datum->IsA(InUserDataClass))
		{
			return Datum;
		}
	}
	return nullptr;
}

const TArray<UAssetUserData*>* UGaussianSplat::GetAssetUserDataArray() const
{
	return &ToRawPtrTArrayUnsafe(AssetUserData);
}

#if WITH_EDITORONLY_DATA
uint64 UGaussianSplat::GetRawMemoryUsage() const
{
	const FGaussianSplatFormatFloat& EditorData = GetEditorData();
	return EditorData.BaseData.GetAllocatedSize() + EditorData.SHData.GetAllocatedSize();
}
#endif

uint64 UGaussianSplat::GetRuntimeMemoryUsage() const
{
	FGaussianSplatFormat* FormatData = RuntimeDataStruct.GetPtr<FGaussianSplatFormat>();
	return FormatData ? FormatData->CalculateMemoryUsage() : 0;
}

int32 UGaussianSplat::GetSHDimension() const
{
	return GaussianSplatCommon::GetSHDimension(SHDegree);
}

TSharedPtr<FGaussianSplatRenderResource> UGaussianSplat::GetResource() const
{
	using namespace GaussianSplatPrivate;

	check(IsInGameThread());

	if (Resource == nullptr && RuntimeDataStruct.IsValid())
	{
		const FGaussianSplatFormat& RuntimeFormat = RuntimeDataStruct.Get<FGaussianSplatFormat>();
		Resource = MakeShareable(new FGaussianSplatRenderResource(Bounds, RuntimeFormat.GetNumSplats(), NumSplats, RuntimeFormat.GetSHDegree(), SHCoordinateSpace, RuntimeDataStruct, GetFName()), GaussianSplatResourceDeletor());
		BeginInitResource(Resource.Get());
	}
	return Resource;
}

#if WITH_EDITORONLY_DATA
void UGaussianSplat::ReleaseResourceAndWait()
{
	if (Resource.IsValid())
	{
		Resource = nullptr;

		if (IsInGameThread())
		{
			ReleaseResourcesFence.BeginFence();
			ReleaseResourcesFence.Wait();
		}
	}
}

void UGaussianSplat::SetData(FGaussianSplatBuilder& InBuilder)
{
	Modify();
	MarkPackageDirty();

	ReleaseResourceAndWait();

	FGaussianSplatFormatFloat& EditorData = GetEditorData();
	if (InBuilder.IsValid())
	{
		NumSplats			= InBuilder.GetNumSplats();
		SHDegree			= InBuilder.SHDegree;
		SHCoordinateSpace	= InBuilder.CoordinateSpace;
		Bounds				= InBuilder.Bounds;
		EditorData.BaseData	= MoveTemp(InBuilder.BaseData);
		EditorData.SHData	= MoveTemp(InBuilder.SHData);
		EditorData.NumSplats = InBuilder.GetNumSplats();
		EditorData.SHDegree = InBuilder.SHDegree;
	}
	else
	{
		NumSplats			= 0;
		SHDegree			= 0;
		SHCoordinateSpace	= EGaussianSplatCoordinateSpace::LH_FRU;
		Bounds				= FBox3f(EForceInit::ForceInit);
		EditorData.BaseData.Empty();
		EditorData.SHData.Empty();
		EditorData.NumSplats = 0;
		EditorData.SHDegree = 0;
	}

	CacheDerivedData();

	GetOnDataChanged().Broadcast(this);
}

void UGaussianSplat::SetUseQuantizedFormat(bool bValue)
{
	if (bUseQuantizedFormat == bValue)
	{
		return;
	}
	Modify();
	MarkPackageDirty();
	bUseQuantizedFormat = bValue;
	ReleaseResourceAndWait();
	CacheDerivedData();
	GetOnDataChanged().Broadcast(this);
}

UGaussianSplat::FOnDataChanged& UGaussianSplat::GetOnDataChanged()
{
	static FOnDataChanged OnDataChanged;
	return OnDataChanged;
}

int32 UGaussianSplat::GetNumLODLevels() const
{
	// Clusters are sorted by level, finest first
	const FGaussianSplatFormat* RuntimeFormat = RuntimeDataStruct.GetPtr<FGaussianSplatFormat>();
	return (RuntimeFormat && !RuntimeFormat->Clusters.IsEmpty()) ? int32(RuntimeFormat->Clusters.Last().Level) + 1 : 0;
}

UGaussianSplat* UGaussianSplat::CreatePreview(int32 InMinLOD, int32 InSHDegree) const
{
	UGaussianSplat* Preview = NewObject<UGaussianSplat>(GetTransientPackage(), NAME_None, RF_Transient);
	Preview->Bounds = Bounds;
	Preview->NumSplats = NumSplats;
	Preview->SHDegree = SHDegree;
	Preview->SHCoordinateSpace = SHCoordinateSpace;

	if (RuntimeDataStruct.IsValid())
	{
		// Matches the cook-time truncation in Serialize
		FSharedStruct Truncated = FSharedStruct::Make(RuntimeDataStruct.GetScriptStruct(), RuntimeDataStruct.GetMemory());
		Truncated.Get<FGaussianSplatFormat>().Truncate(InMinLOD, InSHDegree);

		// A second copy resizes so the reported runtime memory matches the target platform
		Preview->RuntimeDataStruct = FSharedStruct::Make(Truncated.GetScriptStruct(), Truncated.GetMemory());
	}
	return Preview;
}

UScriptStruct* UGaussianSplat::GetRuntimeFormatStruct() const
{
	return bUseQuantizedFormat
		? FGaussianSplatFormatQuantized::StaticStruct()
		: FGaussianSplatFormatFloat::StaticStruct();
}

#if WITH_DEV_AUTOMATION_TESTS
FString UGaussianSplat::BuildDerivedDataKeyForAutomationTest() const
{
	return BuildDerivedDataKey();
}
#endif

FString UGaussianSplat::BuildDerivedDataKey() const
{
	FString KeySuffix;

	// Append basic information
	KeySuffix.AppendInt(NumSplats);
	KeySuffix.AppendInt(SHDegree);
	KeySuffix.Append(GetRuntimeFormatStruct()->GetFullName());
	KeySuffix.Append(UEnum::GetValueAsString(ClusterGrouping));
	KeySuffix.AppendInt(MaxSplatsPerCluster);
	KeySuffix.Append(UEnum::GetValueAsString(LODAggregation));

	// Hash the rest of the information
	//-TODO: Not including the bounds, if everything else is the same I don't think we need to include it
	{
		const FGaussianSplatFormatFloat& EditorData = GetEditorData();

		FBlake3 Hasher;
		Hasher.Update(MakeMemoryView(EditorData.BaseData));
		Hasher.Update(MakeMemoryView(EditorData.SHData));
		const FBlake3Hash SourceHash = Hasher.Finalize();
		KeySuffix.Append(LexToString(SourceHash));
	}

	return FDerivedDataCacheInterface::BuildCacheKey(TEXT("GaussianSplat"), GAUSSIANSPLAT_DERIVED_DATA_VERSION, *KeySuffix);
}

void UGaussianSplat::BuildDerivedData()
{
	if (NumSplats <= 0)
	{
		RuntimeDataStruct.InitializeAs(GetRuntimeFormatStruct(), nullptr);
		return;
	}

	const FGaussianSplatFormatFloat& EditorData = GetEditorData();
	const int32 SHDimension = GetSHDimension();

	// Copy editor data, then permute it
	FGaussianSplatFormatFloat RuntimeData = EditorData;
	RuntimeData.NumSplats = NumSplats;

	TArray<FGaussianSplatCluster> Clusters;
	GaussianSplat::BuildClusters(RuntimeData, NumSplats, MaxSplatsPerCluster, ClusterGrouping, Clusters);
	RuntimeData.Clusters = MoveTemp(Clusters);

	GaussianSplat::BuildLevelsOfDetail(RuntimeData, LODAggregation);

	if (bUseQuantizedFormat)
	{
		RuntimeDataStruct.InitializeAs(FGaussianSplatFormatQuantized::StaticStruct(), nullptr);
		FGaussianSplatFormatQuantized& Quantized = RuntimeDataStruct.Get<FGaussianSplatFormatQuantized>();

		Quantized.Build(RuntimeData, RuntimeData.GetNumSplats(), SHDimension);
		Quantized.Clusters = MoveTemp(RuntimeData.Clusters);
		Quantized.SHDegree = SHDegree;
	}
	else
	{
		RuntimeDataStruct.InitializeAs(FGaussianSplatFormatFloat::StaticStruct(), nullptr);
		FGaussianSplatFormatFloat& Runtime = RuntimeDataStruct.Get<FGaussianSplatFormatFloat>();

		Runtime.BaseData	= MoveTemp(RuntimeData.BaseData);
		Runtime.SHData		= MoveTemp(RuntimeData.SHData);
		Runtime.Clusters	= MoveTemp(RuntimeData.Clusters);
		Runtime.NumSplats	= RuntimeData.NumSplats;
		Runtime.SHDegree = SHDegree;
	}
}

void UGaussianSplat::CacheDerivedData()
{
	if (!bUseQuantizedFormat || IsRunningCookCommandlet())
	{
		BuildDerivedData();
		return;
	}

	const FString DerivedDataKey = BuildDerivedDataKey();

	TArray<uint8> DerivedDataBytes;
	if (GetDerivedDataCacheRef().GetSynchronous(*DerivedDataKey, DerivedDataBytes, GetPathName()))
	{
		FMemoryReader Reader(DerivedDataBytes);

		RuntimeDataStruct.InitializeAs(GetRuntimeFormatStruct(), nullptr);
		RuntimeDataStruct.Get<FGaussianSplatFormat>().DerivedDataRead(Reader);
	}
	else
	{
		BuildDerivedData();

		FMemoryWriter Writer(DerivedDataBytes);
		RuntimeDataStruct.Get<FGaussianSplatFormat>().DerivedDataWrite(Writer);
		GetDerivedDataCacheRef().Put(*DerivedDataKey, DerivedDataBytes, GetPathName());
	}
}
#endif //WITH_EDITORONLY_DATA
