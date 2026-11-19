// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianSplatRenderPipeline.h"
#include "NiagaraRenderer.h"
#include "RenderGraphResources.h"

class FNiagaraGpuComputeDispatchInterface;

/** Per frame state handed from the game thread to the render thread. */
struct FNiagaraDynamicDataSplats : public FNiagaraDynamicDataBase
{
	explicit FNiagaraDynamicDataSplats(const FNiagaraEmitterInstance* InEmitter) : FNiagaraDynamicDataBase(InEmitter) {}

	/** Transform and per-emitter settings are filled in on the game thread, the GPU buffer on the render thread. */
	FGaussianSplatRenderProxy Proxy;

	FVector3f DefaultPosition = FVector3f::ZeroVector;
	FVector3f DefaultScale = FVector3f::OneVector;
	FQuat4f DefaultRotation = FQuat4f::Identity;
	FLinearColor DefaultColor = FLinearColor::White;
	int32 DefaultVisibilityTag = 0;

	/** Owner of Niagara's shared GPU instance count buffer. Null for CPU simulations. */
	FNiagaraGpuComputeDispatchInterface* ComputeDispatchInterface = nullptr;
};

/**
 * Turns an emitter's particles into Gaussian splats and hands them to the Gaussian Splat plugin. A secondary compute pass
 * converts the simulation buffers to Gaussian splat data layout.
 */
class FNiagaraRendererSplats : public FNiagaraRenderer
{
public:
	FNiagaraRendererSplats(ERHIFeatureLevel::Type FeatureLevel, const UNiagaraRendererProperties* InProps, const FNiagaraEmitterInstance* Emitter);
	virtual ~FNiagaraRendererSplats();

	//~ Begin FNiagaraRenderer interface
	virtual void ReleaseRenderThreadResources() override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View, const FNiagaraSceneProxy* SceneProxy) const override;
	virtual FNiagaraDynamicDataBase* GenerateDynamicData(const FNiagaraSceneProxy* Proxy, const UNiagaraRendererProperties* InProperties, const FNiagaraEmitterInstance* Emitter) const override;
	virtual int32 GetDynamicDataSize() const override { return sizeof(FNiagaraDynamicDataSplats); }
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& UsedMaterials, bool bGetDebugMaterials) override {}
	//~ End FNiagaraRenderer interface

private:
	/** Builds the splat buffer and appends a proxy. */
	void CollectSplatProxies_RenderThread(FRDGBuilder& GraphBuilder, TArray<const FGaussianSplatRenderProxy*>& OutProxies);
	void AddBuildSplatsPass(FRDGBuilder& GraphBuilder, const FNiagaraDynamicDataSplats& DynamicData, FNiagaraDataBuffer& ParticleData, uint32 NumSplats);

	/** Splat data */
	TRefCountPtr<FRDGPooledBuffer> SplatBuffer;
	uint32 SplatBufferCapacity = 0;
	uint64 LastBuiltFrameCounter = MAX_uint64;

	/**
	 * Owner of this renderer's binding in the world's splat renderer. Niagara deletes renderers on the render thread,
	 * so the binding expires exactly when the renderer does and nothing has to unregister.
	 */
	struct FRegistration {};
	TSharedRef<FRegistration> Registration = MakeShared<FRegistration>();

	/** Simulation registers of the bound attributes, INDEX_NONE when a binding is not a particle attribute. */
	int32 PositionRegister = INDEX_NONE;
	int32 ScaleRegister = INDEX_NONE;
	int32 RotationRegister = INDEX_NONE;
	int32 ColorRegister = INDEX_NONE;
	int32 VisibilityTagRegister = INDEX_NONE;
	int32 RendererVisibility = 0;

	/** System and emitter this renderer belongs to, for the diagnostic logging. */
	FString DebugName;
};
