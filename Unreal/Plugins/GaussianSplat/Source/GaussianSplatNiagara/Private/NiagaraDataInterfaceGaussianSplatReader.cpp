// Copyright Epic Games, Inc. All Rights Reserved.

#include "NiagaraDataInterfaceGaussianSplatReader.h"
#include "GaussianSplatCommon.h"
#include "GaussianSplat.h"
#include "GaussianSplatRenderResource.h"
#include "GaussianSplatShared.h"
#include "NiagaraFunctionLibrary.h"

#include "NiagaraCompileHashVisitor.h"
#include "NiagaraShaderParametersBuilder.h"
#include "NiagaraSystemInstance.h"

#include "Engine/World.h"
#include "UObject/UObjectIterator.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "UnifiedBuffer.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(NiagaraDataInterfaceGaussianSplatReader)

#define LOCTEXT_NAMESPACE "NiagaraDataInterfaceGaussianSplatReader"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace NDIGaussianSplatLocal
{
	BEGIN_SHADER_PARAMETER_STRUCT(FShaderParameters, )
		SHADER_PARAMETER(int32,								NumSplats)
		SHADER_PARAMETER(int32,								IndexClamp)
		SHADER_PARAMETER(FVector3f,							BoundsMin)
		SHADER_PARAMETER(FVector3f,							BoundsMax)
		SHADER_PARAMETER(int32,								SHDegree)
		SHADER_PARAMETER(int32,								SHCoordinateSpace)

		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer,	SplatData)
		SHADER_PARAMETER(uint32,							SplatDataStride)
		SHADER_PARAMETER(uint32,							SplatFormat)

		SHADER_PARAMETER(FVector3f,							PositionScale)
		SHADER_PARAMETER(FVector3f,							PositionBias)
		SHADER_PARAMETER(FVector3f,							ScaleScale)
		SHADER_PARAMETER(FVector3f,							ScaleBias)
		SHADER_PARAMETER(FVector4f,							ColorScale)
		SHADER_PARAMETER(FVector4f,							ColorBias)
		SHADER_PARAMETER(FVector3f,							SHScale)
		SHADER_PARAMETER(FVector3f,							SHBias)
	END_SHADER_PARAMETER_STRUCT()

	const TCHAR* TemplateShaderFilePath = TEXT("/Plugin/GaussianSplat/Private/NiagaraDataInterfaceGaussianSplatReaderTemplate.ush");

	static const FName NAME_GetNumSplats(TEXT("GetNumSplats"));
	static const FName NAME_GetBounds(TEXT("GetBounds"));
	static const FName NAME_GetBaseData(TEXT("GetBaseData"));
	static const FName NAME_Calculate(TEXT("Calculate"));
	static const FName NAME_CalculateColor(TEXT("CalculateColor"));
	static const FName NAME_CalculateSpriteSizeRotation(TEXT("CalculateSpriteSizeRotation"));
	static const FName NAME_GetSHDegree(TEXT("GetSHDegree"));
	static const FName NAME_GetSHCoefficient(TEXT("GetSHCoefficient"));

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	struct FInstanceData_RenderThread
	{
		TSharedPtr<FGaussianSplatRenderResource>	Resource;
		int32											MaxSHDegree = 0;
	};

	struct FGameToRenderData
	{
		TSharedPtr<FGaussianSplatRenderResource>	Resource;
		int32											MaxSHDegree = 0;
	};

	struct FInstanceData_GameThread
	{
		TSharedPtr<FGaussianSplatRenderResource>	Resource;
		int32											MaxSHDegree = 0;
	};

	struct FNDIProxy : public FNiagaraDataInterfaceProxy
	{
		virtual void ConsumePerInstanceDataFromGameThread(void* PerInstanceData, const FNiagaraSystemInstanceID& Instance) override {}
		virtual int32 PerInstanceDataPassedToRenderThreadSize() const override { return 0; }
	
		void SendGameToRender(FInstanceData_GameThread* InstanceData_GT, FNiagaraSystemInstanceID SystemInstanceID)
		{
			FGameToRenderData GameToRenderData;
			GameToRenderData.Resource = InstanceData_GT->Resource;
			GameToRenderData.MaxSHDegree = InstanceData_GT->MaxSHDegree;

			ENQUEUE_RENDER_COMMAND(FNDINiagaraGaussianSplat_SendGameToRender)
			(
				[this, SystemInstanceID, GameToRenderData=MoveTemp(GameToRenderData)](FRHICommandListImmediate& RHICmdList)
				{
					FInstanceData_RenderThread* InstanceData_RT = &PerInstanceData_RenderThread.FindOrAdd(SystemInstanceID);
					InstanceData_RT->Resource = GameToRenderData.Resource;
					InstanceData_RT->MaxSHDegree = GameToRenderData.MaxSHDegree;
				}
			);
		}
	
		TMap<FNiagaraSystemInstanceID, FInstanceData_RenderThread>	PerInstanceData_RenderThread;
	};

	void VMGetNumSplats(FVectorVMExternalFunctionContext& Context)
	{
		VectorVM::FUserPtrHandler<FInstanceData_GameThread> InstanceData(Context);
		FNDIOutputParam<int32>	OutNumSplats(Context);

		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutNumSplats.SetAndAdvance(InstanceData->Resource->GetNumLOD0Splats());
		}
	}

	void VMGetBounds(FVectorVMExternalFunctionContext& Context)
	{
		VectorVM::FUserPtrHandler<FInstanceData_GameThread> InstanceData(Context);
		FNDIOutputParam<FVector3f>	OutBoundsMin(Context);
		FNDIOutputParam<FVector3f>	OutBoundsMax(Context);

		const FBox3f Bounds = InstanceData->Resource->GetBounds();
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutBoundsMin.SetAndAdvance(Bounds.Min);
			OutBoundsMax.SetAndAdvance(Bounds.Max);
		}
	}

	void VMGetBaseData(FVectorVMExternalFunctionContext& Context)
	{
		VectorVM::FUserPtrHandler<FInstanceData_GameThread> InstanceData(Context);
		FNDIInputParam<int32>		InIndex(Context);
		FNDIOutputParam<FVector3f>	OutPosition(Context);
		FNDIOutputParam<FQuat4f>	OutRotation(Context);
		FNDIOutputParam<FVector3f>	OutScale(Context);
		FNDIOutputParam<float>	OutOpacity(Context);

		const int32 NumSplats = InstanceData->Resource->GetNumLOD0Splats();
		const int32 IndexClamp = FMath::Max(NumSplats - 1, 0);

		const FSharedStruct& CpuSplatData = InstanceData->Resource->GetRuntimeData();
		if (const FGaussianSplatFormatFloat* FloatFormat = CpuSplatData.GetPtr<FGaussianSplatFormatFloat>())
		{
			for (int32 i = 0; i < Context.GetNumInstances(); ++i)
			{
				const int32 RawIndex = InIndex.GetAndAdvance();
				const int32 Index = FMath::Clamp(RawIndex, 0, IndexClamp);

				const FGaussianSplatFormatFloat::FBaseData& SplatData = FloatFormat->BaseData[Index];
				OutPosition.SetAndAdvance(SplatData.Position);
				OutRotation.SetAndAdvance(SplatData.Rotation);
				OutScale.SetAndAdvance(SplatData.Scale);
				OutOpacity.SetAndAdvance(SplatData.Opacity);
			}
		}
		else if (const FGaussianSplatFormatQuantized* QuantizedFormat = CpuSplatData.GetPtr<FGaussianSplatFormatQuantized>())
		{
			const uint32 SplatDataStride = InstanceData->Resource->GetSplatStride();
			for (int32 i = 0; i < Context.GetNumInstances(); ++i)
			{
				const int32 RawIndex = InIndex.GetAndAdvance();
				const int32 Index = FMath::Clamp(RawIndex, 0, IndexClamp);
				const uint32 ReadOffset = (Index * SplatDataStride) / sizeof(uint32);

				const uint32* QuantizedData = &QuantizedFormat->Data[ReadOffset];

				const FVector3f Position	= FGaussianSplatShared::Unpack_21_21_21(QuantizedData[0], QuantizedData[1], QuantizedFormat->PositionScaleBias[0], QuantizedFormat->PositionScaleBias[1]);
				const FVector3f Scale		= FGaussianSplatShared::Unpack_11_11_10_Exp(QuantizedData[2], QuantizedFormat->ScaleScaleBias[0], QuantizedFormat->ScaleScaleBias[1]);
				const FQuat4f   Rotation	= FGaussianSplatShared::UnpackQuat(QuantizedData[3]);
				const float     Opacity		= FGaussianSplatShared::Unpack_8_8_8_8(QuantizedData[4], QuantizedFormat->ColorScaleBias[0], QuantizedFormat->ColorScaleBias[1]).W;

				OutPosition.SetAndAdvance(Position);
				OutRotation.SetAndAdvance(Rotation);
				OutScale.SetAndAdvance(Scale);
				OutOpacity.SetAndAdvance(Opacity);
			}
		}
		else
		{
			for (int32 i=0; i < Context.GetNumInstances(); ++i)
			{
				OutPosition.SetAndAdvance(FVector3f::ZeroVector);
				OutRotation.SetAndAdvance(FQuat4f::Identity);
				OutScale.SetAndAdvance(FVector3f::ZeroVector);
				OutOpacity.SetAndAdvance(0.0f);
			}
		}
	}

	void VMGetSHDegree(FVectorVMExternalFunctionContext& Context)
	{
		VectorVM::FUserPtrHandler<FInstanceData_GameThread> InstanceData(Context);
		FNDIOutputParam<int32> OutDegree(Context);

		const int32 SHDegree = FMath::Min(InstanceData->Resource->GetSHDegree(), InstanceData->MaxSHDegree);
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutDegree.SetAndAdvance(SHDegree);
		}
	}

	void VMGetSHCoefficient(FVectorVMExternalFunctionContext& Context)
	{
		VectorVM::FUserPtrHandler<FInstanceData_GameThread> InstanceData(Context);
		FNDIInputParam<int32>		InIndex(Context);
		FNDIInputParam<int32>		InCoefficient(Context);
		FNDIOutputParam<FVector3f>	OutCoefficient(Context);

		const FSharedStruct& CpuSplatData = InstanceData->Resource->GetRuntimeData();
		const FGaussianSplatFormatFloat* FloatFormat = CpuSplatData.GetPtr<FGaussianSplatFormatFloat>();
		const FGaussianSplatFormatQuantized* QuantizedFormat = CpuSplatData.GetPtr<FGaussianSplatFormatQuantized>();

		const int32 IndexClamp = FMath::Max(InstanceData->Resource->GetNumLOD0Splats() - 1, 0);
		const int32 SHDimension = GaussianSplatCommon::GetSHDimension(FMath::Min(InstanceData->Resource->GetSHDegree(), InstanceData->MaxSHDegree));
		const int32 SHSourceDimension = GaussianSplatCommon::GetSHDimension(InstanceData->Resource->GetSHDegree());
		const uint32 SplatStride = InstanceData->Resource->GetSplatStride();

		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 Index = FMath::Clamp(InIndex.GetAndAdvance(), 0, IndexClamp);
			const int32 Coefficient = InCoefficient.GetAndAdvance();
			FVector3f Value = FVector3f::ZeroVector;
			if (Coefficient >= 0 && Coefficient < SHDimension)
			{
				if (FloatFormat)
				{
					Value = FloatFormat->SHData[SIZE_T(Index) * SHSourceDimension + Coefficient];
				}
				else if (QuantizedFormat)
				{
					// The color word is word 4 of a record, the higher coefficients follow it one word each
					const uint32 Packed = QuantizedFormat->Data[(SIZE_T(Index) * SplatStride) / sizeof(uint32) + 4 + Coefficient];
					Value = Coefficient == 0
						? FVector3f(FGaussianSplatShared::Unpack_8_8_8_8(Packed, QuantizedFormat->ColorScaleBias[0], QuantizedFormat->ColorScaleBias[1]))
						: FVector3f(FGaussianSplatShared::Unpack_8_8_8_8(Packed, FVector4f(QuantizedFormat->SHScaleBias[0], 0.0f), FVector4f(QuantizedFormat->SHScaleBias[1], 0.0f)));
				}
			}
			OutCoefficient.SetAndAdvance(Value);
		}
	}

	void VMCalculateColor(FVectorVMExternalFunctionContext& Context)
	{
		VectorVM::FUserPtrHandler<FInstanceData_GameThread> InstanceData(Context);
		FNDIInputParam<int32>		InIndex(Context);
		FNDIInputParam<FVector3f>	InDirection(Context);
		FNDIOutputParam<FVector3f>	OutColor(Context);

		const FSharedStruct& CpuSplatData = InstanceData->Resource->GetRuntimeData();

		const int32 NumSplats	= InstanceData->Resource->GetNumLOD0Splats();
		const int32 IndexClamp	= FMath::Max(NumSplats - 1, 0);
		const int32 SHDegree	= FMath::Min(InstanceData->Resource->GetSHDegree(), InstanceData->MaxSHDegree);
		const EGaussianSplatCoordinateSpace SHCoordinateSpace = InstanceData->Resource->GetSHCoordinateSpace();
		const int32 SHDimension	= GaussianSplatCommon::GetSHDimension(SHDegree);

		const int32 SHSourceDimension = GaussianSplatCommon::GetSHDimension(InstanceData->Resource->GetSHDegree());

		const FGaussianSplatFormatFloat* FloatFormat = CpuSplatData.GetPtr<FGaussianSplatFormatFloat>();
		const FGaussianSplatFormatQuantized* QuantizedFormat = CpuSplatData.GetPtr<FGaussianSplatFormatQuantized>();

		for (int32 iInstance=0; iInstance < Context.GetNumInstances(); ++iInstance)
		{
			const int32 RawIndex		= InIndex.GetAndAdvance();
			const int32 Index			= FMath::Clamp(RawIndex, 0, IndexClamp);
			FVector3f Direction	= InDirection.GetAndAdvance();
			if (SHCoordinateSpace == EGaussianSplatCoordinateSpace::RH_RUB)
			{
				Direction = FVector3f(Direction.Y, Direction.Z, -Direction.X);
			}
			else if (SHCoordinateSpace == EGaussianSplatCoordinateSpace::RH_RDF)
			{
				Direction = FVector3f(Direction.Y, -Direction.Z, Direction.X);
			}

			// Read SH coefficients into Coefs[0..SHDimension-1]
			FVector3f Coefs[16];

			if (FloatFormat)
			{
				const uint32 SHDataOffset = SHSourceDimension * Index;
				for (int32 iCoef = 0; iCoef < SHDimension; ++iCoef)
				{
					Coefs[iCoef] = FloatFormat->SHData[SHDataOffset + iCoef];
				}
			}
			else if (QuantizedFormat)
			{
				const uint32 ReadOffset = Index * InstanceData->Resource->GetSplatStride();

				uint32 PackedColor;
				FMemory::Memcpy(&PackedColor, &QuantizedFormat->Data[(ReadOffset + FGaussianSplatShared::EQuantized_ColorOpacityOffset) / sizeof(uint32)], sizeof(uint32));
				const FVector4f UnpackedColor = FGaussianSplatShared::Unpack_8_8_8_8(PackedColor, QuantizedFormat->ColorScaleBias[0], QuantizedFormat->ColorScaleBias[1]);
				Coefs[0] = FVector3f(UnpackedColor.X, UnpackedColor.Y, UnpackedColor.Z);

				uint32 SHOffset = ReadOffset + FGaussianSplatShared::EQuantized_Stride;
				for (int32 iCoef = 1; iCoef < SHDimension; ++iCoef)
				{
					uint32 PackedSH;
					FMemory::Memcpy(&PackedSH, &QuantizedFormat->Data[SHOffset / sizeof(uint32)], sizeof(uint32));
					const FVector4f UnpackedSH = FGaussianSplatShared::Unpack_8_8_8_8(PackedSH, QuantizedFormat->SHScaleBias[0], QuantizedFormat->SHScaleBias[1]);
					Coefs[iCoef] = FVector3f(UnpackedSH.X, UnpackedSH.Y, UnpackedSH.Z);
					SHOffset += 4u;
				}
			}
			else
			{
				// Invalid data write output and continue
				OutColor.SetAndAdvance(FVector3f::ZeroVector);
				continue;
			}

			// Evaluate spherical harmonics
			FVector3f Color = FGaussianSplatSHConstants::SH_C0 * Coefs[0];

			if (SHDegree >= 1)
			{
				const float X = Direction.X;
				const float Y = Direction.Y;
				const float Z = Direction.Z;

				Color = Color
					- FGaussianSplatSHConstants::SH_C1 * Y * Coefs[1]
					+ FGaussianSplatSHConstants::SH_C1 * Z * Coefs[2]
					- FGaussianSplatSHConstants::SH_C1 * X * Coefs[3];

				if (SHDegree >= 2)
				{
					const float XX = X * X;
					const float YY = Y * Y;
					const float ZZ = Z * Z;
					const float XY = X * Y;
					const float XZ = X * Z;
					const float YZ = Y * Z;

					Color = Color
						+ FGaussianSplatSHConstants::SH_C2_0 * XY * Coefs[4]
						+ FGaussianSplatSHConstants::SH_C2_1 * YZ * Coefs[5]
						+ FGaussianSplatSHConstants::SH_C2_2 * (2.0f * ZZ - XX - YY) * Coefs[6]
						+ FGaussianSplatSHConstants::SH_C2_3 * XZ * Coefs[7]
						+ FGaussianSplatSHConstants::SH_C2_4 * (XX - YY) * Coefs[8];

					if (SHDegree >= 3)
					{
						Color = Color
							+ FGaussianSplatSHConstants::SH_C3_0 * Y * (3.0f * XX - YY) * Coefs[9]
							+ FGaussianSplatSHConstants::SH_C3_1 * XY * Z * Coefs[10]
							+ FGaussianSplatSHConstants::SH_C3_2 * Y * (4.0f * ZZ - XX - YY) * Coefs[11]
							+ FGaussianSplatSHConstants::SH_C3_3 * Z * (2.0f * ZZ - 3.0f * XX - 3.0f * YY) * Coefs[12]
							+ FGaussianSplatSHConstants::SH_C3_4 * X * (4.0f * ZZ - XX - YY) * Coefs[13]
							+ FGaussianSplatSHConstants::SH_C3_5 * Z * (XX - YY) * Coefs[14]
							+ FGaussianSplatSHConstants::SH_C3_6 * X * (XX - 3.0f * YY) * Coefs[15];
					}
				}
			}

			// Clamp to minimum positive denormal
			Color.X = FMath::Max(Color.X + 0.5f, 6.10352e-5f);
			Color.Y = FMath::Max(Color.Y + 0.5f, 6.10352e-5f);
			Color.Z = FMath::Max(Color.Z + 0.5f, 6.10352e-5f);

			// Convert sRGB -> linear
			Color.X = Color.X > 0.04045f ? FMath::Pow(Color.X * (1.0f / 1.055f) + 0.0521327f, 2.4f) : Color.X * (1.0f / 12.92f);
			Color.Y = Color.Y > 0.04045f ? FMath::Pow(Color.Y * (1.0f / 1.055f) + 0.0521327f, 2.4f) : Color.Y * (1.0f / 12.92f);
			Color.Z = Color.Z > 0.04045f ? FMath::Pow(Color.Z * (1.0f / 1.055f) + 0.0521327f, 2.4f) : Color.Z * (1.0f / 12.92f);

			OutColor.SetAndAdvance(Color);
		}
	}

} //namespace NDIGaussianSplatLocal

//////////////////////////////////////////////////////////////////////////

UNiagaraDataInterfaceGaussianSplatReader::UNiagaraDataInterfaceGaussianSplatReader(FObjectInitializer const& ObjectInitializer)
	: Super(ObjectInitializer)
{
	using namespace NDIGaussianSplatLocal;

	Proxy.Reset(new FNDIProxy());
}

void UNiagaraDataInterfaceGaussianSplatReader::PostInitProperties()
{
	Super::PostInitProperties();

	//Can we register data interfaces as regular types and fold them into the FNiagaraVariable framework for UI and function calls etc?
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		ENiagaraTypeRegistryFlags Flags = ENiagaraTypeRegistryFlags::AllowAnyVariable | ENiagaraTypeRegistryFlags::AllowParameter;
		FNiagaraTypeRegistry::Register(FNiagaraTypeDefinition(GetClass()), Flags);

	#if WITH_EDITOR
		// Add handler to reset any systems using this splat asset
		UGaussianSplat::GetOnDataChanged().AddLambda(
			[](UGaussianSplat* Asset)
			{
				for (TObjectIterator<UNiagaraDataInterfaceGaussianSplatReader> It; It; ++It)
				{
					UNiagaraDataInterfaceGaussianSplatReader* DataInterface = *It;
					if (::IsValid(DataInterface) == false || DataInterface->GaussianSplat != Asset)
					{
						continue;
					}

					UNiagaraComponent* Component = DataInterface->GetTypedOuter<UNiagaraComponent>();
					if (!Component || !Component->IsActive())
					{
						continue;
					}

					Component->ReinitializeSystem();
				}
			}
		);
	#endif //WITH_EDITOR
	}
}

bool UNiagaraDataInterfaceGaussianSplatReader::Equals(const UNiagaraDataInterface* Other) const
{
	if (!Super::Equals(Other))
	{
		return false;
	}

	// Note: We intentionally do not include bReadsAnyRow for equality as that's part of PostCompile
	const UNiagaraDataInterfaceGaussianSplatReader* OtherTyped = CastChecked<const UNiagaraDataInterfaceGaussianSplatReader>(Other);
	return
		OtherTyped->GaussianSplat == GaussianSplat &&
		OtherTyped->MaxSHDegree == MaxSHDegree;
}

bool UNiagaraDataInterfaceGaussianSplatReader::CopyToInternal(UNiagaraDataInterface* Destination) const
{
	if (!Super::CopyToInternal(Destination))
	{
		return false;
	}

	UNiagaraDataInterfaceGaussianSplatReader* DestinationTyped = CastChecked<UNiagaraDataInterfaceGaussianSplatReader>(Destination);
	DestinationTyped->GaussianSplat = GaussianSplat;
	DestinationTyped->MaxSHDegree = MaxSHDegree;

	return true;
}

bool UNiagaraDataInterfaceGaussianSplatReader::InitPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance)
{
	using namespace NDIGaussianSplatLocal;

	const int32 MaxSupportedSHDegree = GaussianSplatCommon::MaxSupportedSHDegree;

	FInstanceData_GameThread* InstanceData_GT = new(PerInstanceData) FInstanceData_GameThread();
	InstanceData_GT->Resource = GaussianSplat ? GaussianSplat->GetResource() : FGaussianSplatRenderResource::GetEmptyResource();
	InstanceData_GT->MaxSHDegree = FMath::Clamp(MaxSHDegree.Get(MaxSupportedSHDegree), 0, MaxSupportedSHDegree);

	if ( IsUsedWithGPUScript() )
	{
		GetProxyAs<FNDIProxy>()->SendGameToRender(InstanceData_GT, SystemInstance->GetId());
	}
	return true;
}

void UNiagaraDataInterfaceGaussianSplatReader::DestroyPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance)
{
	using namespace NDIGaussianSplatLocal;

	FInstanceData_GameThread* InstanceData = static_cast<FInstanceData_GameThread*>(PerInstanceData);
	InstanceData->~FInstanceData_GameThread();

	if ( IsUsedWithGPUScript() )
	{
		ENQUEUE_RENDER_COMMAND(NDINiagaraGaussianSplat_RemoveInstance)
		(
			[Proxy=GetProxyAs<FNDIProxy>(), InstanceID=SystemInstance->GetId()](FRHICommandListImmediate& RHICmdList)
			{
				Proxy->PerInstanceData_RenderThread.Remove(InstanceID);
			}
		);
	}
}

int32 UNiagaraDataInterfaceGaussianSplatReader::PerInstanceDataSize() const
{
	using namespace NDIGaussianSplatLocal;

	return sizeof(FInstanceData_GameThread);
}

#if WITH_EDITORONLY_DATA
void UNiagaraDataInterfaceGaussianSplatReader::GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& OutFunctions) const
{
	using namespace NDIGaussianSplatLocal;

	FNiagaraFunctionSignature DefaultSig;
	DefaultSig.bMemberFunction = true;
	DefaultSig.bRequiresContext = false;
	DefaultSig.Inputs.Emplace(FNiagaraTypeDefinition(GetClass()), TEXT("NiagaraGaussianSplat"));

	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Emplace_GetRef(DefaultSig);
		Sig.Name = NAME_GetNumSplats;
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("NumSplats"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Emplace_GetRef(DefaultSig);
		Sig.Name = NAME_GetBounds;
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("BoundsMin"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("BoundsMax"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Emplace_GetRef(DefaultSig);
		Sig.Name = NAME_GetBaseData;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetPositionDef(), TEXT("Position"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetQuatDef(), TEXT("Rotation"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Scale"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Opacity"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Emplace_GetRef(DefaultSig);
		Sig.Name = NAME_GetSHDegree;
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("SHDegree"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Emplace_GetRef(DefaultSig);
		Sig.Name = NAME_GetSHCoefficient;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index"));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Coefficient"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Coefficient"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Emplace_GetRef(DefaultSig);
		Sig.Name = NAME_CalculateColor;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index"));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Direction"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Color"));
		Sig.Description = LOCTEXT("CalculateColorDesc", "Projects are currently expected to have an sRGB working color space.");
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Emplace_GetRef(DefaultSig);
		Sig.Name = NAME_CalculateSpriteSizeRotation;
		Sig.bSupportsCPU = false;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetPositionDef(), TEXT("Position"));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetQuatDef(), TEXT("Rotation"));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Scale"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetBoolDef(), TEXT("IsVisible"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec2Def(), TEXT("SpriteSize"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetFloatDef(), TEXT("SpriteRotation"));
	}
}
#endif

void UNiagaraDataInterfaceGaussianSplatReader::GetVMExternalFunction(const FVMExternalFunctionBindingInfo& BindingInfo, void* PerInstanceData, FVMExternalFunction& OutFunc)
{
	using namespace NDIGaussianSplatLocal;

	if (BindingInfo.Name == NAME_GetNumSplats)
	{
		OutFunc.BindStatic(&VMGetNumSplats);
	}
	else if (BindingInfo.Name == NAME_GetBounds)
	{
		OutFunc.BindStatic(&VMGetBounds);
	}
	else if (BindingInfo.Name == NAME_GetBaseData)
	{
		OutFunc.BindStatic(&VMGetBaseData);
	}
	else if (BindingInfo.Name == NAME_CalculateColor)
	{
		OutFunc.BindStatic(&VMCalculateColor);
	}
	else if (BindingInfo.Name == NAME_GetSHDegree)
	{
		OutFunc.BindStatic(&VMGetSHDegree);
	}
	else if (BindingInfo.Name == NAME_GetSHCoefficient)
	{
		OutFunc.BindStatic(&VMGetSHCoefficient);
	}
}

#if WITH_EDITORONLY_DATA
void UNiagaraDataInterfaceGaussianSplatReader::GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, FString& OutHLSL)
{
	using namespace NDIGaussianSplatLocal;

	const TMap<FString, FStringFormatArg> TemplateArgs =
	{
		{TEXT("ParameterName"),	ParamInfo.DataInterfaceHLSLSymbol},
	};
	AppendTemplateHLSL(OutHLSL, TemplateShaderFilePath, TemplateArgs);
}

bool UNiagaraDataInterfaceGaussianSplatReader::GetFunctionHLSL(const FNiagaraDataInterfaceHlslGenerationContext& HlslGenContext, FString& OutHLSL)
{
	return true;
}

bool UNiagaraDataInterfaceGaussianSplatReader::AppendCompileHash(FNiagaraCompileHashVisitor* InVisitor) const
{
	using namespace NDIGaussianSplatLocal;

	bool bSuccess = Super::AppendCompileHash(InVisitor);
	InVisitor->UpdateShaderFile(TemplateShaderFilePath);
	InVisitor->UpdateShaderParameters<FShaderParameters>();
	return bSuccess;
}
#endif

void UNiagaraDataInterfaceGaussianSplatReader::BuildShaderParameters(FNiagaraShaderParametersBuilder& ShaderParametersBuilder) const
{
	using namespace NDIGaussianSplatLocal;

	ShaderParametersBuilder.AddNestedStruct<FShaderParameters>();
}

void UNiagaraDataInterfaceGaussianSplatReader::SetShaderParameters(const FNiagaraDataInterfaceSetShaderParametersContext& Context) const
{
	using namespace NDIGaussianSplatLocal;

	FNDIProxy& DIProxy = Context.GetProxy<FNDIProxy>();
	FInstanceData_RenderThread& InstanceData = DIProxy.PerInstanceData_RenderThread.FindChecked(Context.GetSystemInstanceID());

	FRDGBuilder& GraphBuilder = Context.GetGraphBuilder();
	FRDGBufferRef RDGBuffer = GraphBuilder.RegisterExternalBuffer(InstanceData.Resource->GetSplatBuffer());
	FRDGBufferSRVRef RDGBufferSRV = GraphBuilder.CreateSRV(RDGBuffer);

	FShaderParameters* ShaderParameters = Context.GetParameterNestedStruct<FShaderParameters>();
	ShaderParameters->NumSplats			= InstanceData.Resource->GetNumLOD0Splats();
	ShaderParameters->IndexClamp		= FMath::Max(InstanceData.Resource->GetNumLOD0Splats() - 1, 0);
	ShaderParameters->BoundsMin			= InstanceData.Resource->GetBounds().Min;
	ShaderParameters->BoundsMax			= InstanceData.Resource->GetBounds().Max;
	ShaderParameters->SHDegree			= FMath::Min(InstanceData.Resource->GetSHDegree(), InstanceData.MaxSHDegree);
	ShaderParameters->SHCoordinateSpace = static_cast<int32>(InstanceData.Resource->GetSHCoordinateSpace());

	ShaderParameters->SplatDataStride	= InstanceData.Resource->GetSplatStride();
	ShaderParameters->SplatData			= RDGBufferSRV;

	if (const FGaussianSplatFormatQuantized* QuantizedFormat = InstanceData.Resource->GetRuntimeData().GetPtr<FGaussianSplatFormatQuantized>())
	{
		ShaderParameters->SplatFormat		= 1;
		ShaderParameters->PositionScale		= QuantizedFormat->PositionScaleBias[0];
		ShaderParameters->PositionBias		= QuantizedFormat->PositionScaleBias[1];
		ShaderParameters->ScaleScale		= QuantizedFormat->ScaleScaleBias[0];
		ShaderParameters->ScaleBias			= QuantizedFormat->ScaleScaleBias[1];
		ShaderParameters->ColorScale		= QuantizedFormat->ColorScaleBias[0];
		ShaderParameters->ColorBias			= QuantizedFormat->ColorScaleBias[1];
		ShaderParameters->SHScale			= QuantizedFormat->SHScaleBias[0];
		ShaderParameters->SHBias			= QuantizedFormat->SHScaleBias[1];
	}
	else
	{
		ShaderParameters->SplatFormat		= 0;
		ShaderParameters->PositionScale		= FVector3f::Zero();
		ShaderParameters->PositionBias		= FVector3f::Zero();
		ShaderParameters->ScaleScale		= FVector3f::Zero();
		ShaderParameters->ScaleBias			= FVector3f::Zero();
		ShaderParameters->ColorScale		= FVector4f::Zero();
		ShaderParameters->ColorBias			= FVector4f::Zero();
		ShaderParameters->SHScale			= FVector3f::Zero();
		ShaderParameters->SHBias			= FVector3f::Zero();
	}
}

UGaussianSplat* UNiagaraDataInterfaceGaussianSplatReader::GetGaussianSplat() const
{
	return GaussianSplat;
}

int32 UNiagaraDataInterfaceGaussianSplatReader::GetSHDegree() const
{
	const int32 MaxSupportedSHDegree = GaussianSplatCommon::MaxSupportedSHDegree;
	const FGaussianSplatFormat* RuntimeFormat = GaussianSplat ? GaussianSplat->GetRuntimeData<FGaussianSplatFormat>() : nullptr;
	const int32 RuntimeSHDegree = RuntimeFormat ? RuntimeFormat->GetSHDegree() : 0;
	return FMath::Clamp(FMath::Min(RuntimeSHDegree, MaxSHDegree.Get(MaxSupportedSHDegree)), 0, MaxSupportedSHDegree);
}

void UNiagaraDataInterfaceGaussianSplatReader::SetGaussianSplat(UNiagaraComponent* Component, const FName& ParameterName, UGaussianSplat* GaussianSplat)
{
	if (UNiagaraDataInterfaceGaussianSplatReader* DataInterface = UNiagaraFunctionLibrary::GetDataInterface<UNiagaraDataInterfaceGaussianSplatReader>(Component, ParameterName))
	{
		DataInterface->GaussianSplat = GaussianSplat;
	#if WITH_EDITORONLY_DATA
		UWorld* World = Component->GetWorld();
		if (World && !World->IsGameWorld())
		{
			Component->Modify();

			UNiagaraDataInterfaceGaussianSplatReader* VariantDI = CastChecked<UNiagaraDataInterfaceGaussianSplatReader>(DuplicateObject(DataInterface, Component));
			DataInterface->CopyTo(VariantDI);
			Component->SetParameterOverride(FNiagaraVariableBase(FNiagaraTypeDefinition(UNiagaraDataInterfaceGaussianSplatReader::StaticClass()), ParameterName), FNiagaraVariant(VariantDI));
		}
	#endif //WITH_EDITORONLY_DATA
		if (Component->IsActive())
		{
			Component->ReinitializeSystem();
		}
	}
}

TOptional<int32> UNiagaraDataInterfaceGaussianSplatReader::GetGaussianSplatMaxSHDegree(UNiagaraComponent* Component, const FName& ParameterName)
{
	if (UNiagaraDataInterfaceGaussianSplatReader* DataInterface = UNiagaraFunctionLibrary::GetDataInterface<UNiagaraDataInterfaceGaussianSplatReader>(Component, ParameterName))
	{
		return DataInterface->MaxSHDegree;
	}
	return {};
}

void UNiagaraDataInterfaceGaussianSplatReader::SetGaussianSplatMaxSHDegree(UNiagaraComponent* Component, const FName& ParameterName, int32 Value)
{
	if (UNiagaraDataInterfaceGaussianSplatReader* DataInterface = UNiagaraFunctionLibrary::GetDataInterface<UNiagaraDataInterfaceGaussianSplatReader>(Component, ParameterName))
	{
		DataInterface->MaxSHDegree = Value;
		Component->ReinitializeSystem();
	}
}

#undef LOCTEXT_NAMESPACE
