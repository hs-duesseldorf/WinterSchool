// Splat shadow map (WinterSchool fork).
//
// Unreal's shadow passes only draw primitives with a scene proxy, so splats cast their shadow through a map of their
// own: every proxy with bCastShadow (UGaussianSplatComponent: Cast Shadow / Hidden Shadow, skinned splats: their
// bCastShadow) is drawn into orthographic cascades of the sun, fitted around the camera like Unreal's CSM. The cascades
// hold the product of the splats' transmittance and the nearest depth of the fairly opaque ones (no sort needed).
//
// The sun applies it with a light function: a light function material referencing two render targets named
// RT_GaussianSplatShadowMap (RGBA16F, the cascades side by side, square each) and RT_GaussianSplatShadowParams (RGBA32F,
// >= 32 x 1, cascade transforms), sampling them with GaussianSplatShadow() from /Plugin/GaussianSplat/Public/
// GaussianSplatShadow.ush. Light functions multiply only the light's direct contribution, after Unreal's own shadows,
// on the desktop deferred renderer and mobile deferred (WebGPU) alike. The plugin's M_GaussianSplatShadow is that material.

#include "GaussianSplatRenderPipeline.h"
#include "GaussianSplatCluster.h"

#include "SceneRendering.h"
#include "ScenePrivate.h"
#include "LightSceneInfo.h"
#include "LightSceneProxy.h"
#include "Materials/MaterialRenderProxy.h"
#include "MaterialShared.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphUtils.h"
#include "PixelShaderUtils.h"
#include "PooledRenderTarget.h"
#include "RHIStaticStates.h"
#include "CommonRenderResources.h"
#include "SystemTextures.h"

static TAutoConsoleVariable<int32> CVarGaussianSplatShadow(
	TEXT("r.GaussianSplat.Shadow"),
	1,
	TEXT("Splats cast shadows from the sun (needs the splat shadow light function on the directional light)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGaussianSplatShadowDistance(
	TEXT("r.GaussianSplat.Shadow.Distance"),
	3000.0f,
	TEXT("Distance from the camera (cm) the splat shadow cascades cover."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatShadowCascades(
	TEXT("r.GaussianSplat.Shadow.Cascades"),
	2,
	TEXT("Number of splat shadow cascades (1-4, limited by the slots of the shadow map)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGaussianSplatShadowSplitExponent(
	TEXT("r.GaussianSplat.Shadow.SplitExponent"),
	2.0f,
	TEXT("Cascade i ends at Distance * (i / Cascades)^SplitExponent."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGaussianSplatShadowBias(
	TEXT("r.GaussianSplat.Shadow.Bias"),
	6.0f,
	TEXT("Depth bias of the splat shadow (cm), plus one texel of the cascade."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGaussianSplatShadowFade(
	TEXT("r.GaussianSplat.Shadow.Fade"),
	20.0f,
	TEXT("Depth (cm) over which a receiver behind the nearest occluder fades into its shadow."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGaussianSplatShadowErrorThreshold(
	TEXT("r.GaussianSplat.Shadow.ErrorThreshold"),
	1.0f,
	TEXT("Level of detail of the casters: cluster error allowed in shadow map texels."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGaussianSplatShadowOpacityThreshold(
	TEXT("r.GaussianSplat.Shadow.OpacityThreshold"),
	0.02f,
	TEXT("Splats below this opacity cast no shadow."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGaussianSplatShadowDepthOpacity(
	TEXT("r.GaussianSplat.Shadow.DepthOpacity"),
	0.3f,
	TEXT("Alpha a splat texel needs to count as the nearest occluder of a shadow map texel."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatShadowMaxSplats(
	TEXT("r.GaussianSplat.Shadow.MaxSplats"),
	2 * 1024 * 1024,
	TEXT("Capacity of a cascade's splat list."),
	ECVF_RenderThreadSafe);

namespace
{
	constexpr int32 MaxCascades = 4;
	constexpr int32 MaxParams = 32; // GAUSSIAN_SPLAT_SHADOW_MAX_PARAMS, >= 1 + 4 * MaxCascades
	constexpr int32 ThreadGroupSize = 64;

	class FShadowShader : public FGlobalShader
	{
	public:
		FShadowShader() = default;
		FShadowShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer) : FGlobalShader(Initializer) {}

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return GaussianSplat::SupportsDeferredSplats(Parameters.Platform);
		}

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), ThreadGroupSize);
			OutEnvironment.SetDefine(TEXT("GAUSSIAN_SPLAT_SHADOW_MAX_PARAMS"), MaxParams);
		}
	};

	BEGIN_SHADER_PARAMETER_STRUCT(FCascadeParameters, )
		SHADER_PARAMETER(FVector4f, CascadeU)
		SHADER_PARAMETER(FVector4f, CascadeV)
		SHADER_PARAMETER(FVector4f, CascadeDepth)
		SHADER_PARAMETER(float, CascadeResolution)
	END_SHADER_PARAMETER_STRUCT()

	class FShadowCullClustersCS : public FShadowShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FShadowCullClustersCS);
		SHADER_USE_PARAMETER_STRUCT(FShadowCullClustersCS, FShadowShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_INCLUDE(FCascadeParameters, Cascade)
			SHADER_PARAMETER(FMatrix44f, LocalToWorld)
			SHADER_PARAMETER(uint32, NumClusters)
			SHADER_PARAMETER(float, PointScaleFactor)
			SHADER_PARAMETER(float, TexelWorldSize)
			SHADER_PARAMETER(float, ErrorThreshold)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGaussianSplatCluster>, ClusterBuffer)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWClusterOutputBuffer)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWClusterCount)
		END_SHADER_PARAMETER_STRUCT()
	};
	IMPLEMENT_GLOBAL_SHADER(FShadowCullClustersCS, "/Plugin/GaussianSplat/Private/GaussianSplatShadow.usf", "CullClustersCS", SF_Compute);

	class FShadowDispatchArgsCS : public FShadowShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FShadowDispatchArgsCS);
		SHADER_USE_PARAMETER_STRUCT(FShadowDispatchArgsCS, FShadowShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ClusterCountBuffer)
			SHADER_PARAMETER(FUintVector2, DimensionLimit)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWDispatchIndirectArgs)
		END_SHADER_PARAMETER_STRUCT()
	};
	IMPLEMENT_GLOBAL_SHADER(FShadowDispatchArgsCS, "/Plugin/GaussianSplat/Private/GaussianSplatShadow.usf", "DispatchArgsCS", SF_Compute);

	class FShadowProjectSplatsCS : public FShadowShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FShadowProjectSplatsCS);
		SHADER_USE_PARAMETER_STRUCT(FShadowProjectSplatsCS, FShadowShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_INCLUDE(FCascadeParameters, Cascade)
			SHADER_PARAMETER(FMatrix44f, LocalToWorld)
			SHADER_PARAMETER(uint32, NumSplats)
			SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, GaussianSplatBuffer)
			SHADER_PARAMETER(uint32, BufferFormat)
			SHADER_PARAMETER(uint32, BufferStride)
			SHADER_PARAMETER(FVector3f, PositionScale)
			SHADER_PARAMETER(FVector3f, PositionBias)
			SHADER_PARAMETER(FVector3f, ScaleScale)
			SHADER_PARAMETER(FVector3f, ScaleBias)
			SHADER_PARAMETER(FVector4f, ColorScale)
			SHADER_PARAMETER(FVector4f, ColorBias)
			SHADER_PARAMETER(float, PointScaleFactor)
			SHADER_PARAMETER(float, OpacityScale)
			SHADER_PARAMETER(float, OpacityThreshold)
			SHADER_PARAMETER(uint32, UseClusters)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGaussianSplatCluster>, ClusterBuffer)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ClusterIndexBuffer)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ClusterCountBuffer)
			SHADER_PARAMETER(uint32, MaxShadowSplats)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, RWShadowSplats)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWShadowSplatCount)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
		END_SHADER_PARAMETER_STRUCT()
	};
	IMPLEMENT_GLOBAL_SHADER(FShadowProjectSplatsCS, "/Plugin/GaussianSplat/Private/GaussianSplatShadow.usf", "ProjectSplatsCS", SF_Compute);

	class FShadowDrawArgsCS : public FShadowShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FShadowDrawArgsCS);
		SHADER_USE_PARAMETER_STRUCT(FShadowDrawArgsCS, FShadowShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ShadowSplatCount)
			SHADER_PARAMETER(uint32, MaxShadowSplats)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWDrawIndirectArgs)
		END_SHADER_PARAMETER_STRUCT()
	};
	IMPLEMENT_GLOBAL_SHADER(FShadowDrawArgsCS, "/Plugin/GaussianSplat/Private/GaussianSplatShadow.usf", "DrawArgsCS", SF_Compute);

	class FShadowVS : public FShadowShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FShadowVS);
		SHADER_USE_PARAMETER_STRUCT(FShadowVS, FShadowShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, ShadowSplats)
			SHADER_PARAMETER(float, CascadeResolution)
		END_SHADER_PARAMETER_STRUCT()
	};
	IMPLEMENT_GLOBAL_SHADER(FShadowVS, "/Plugin/GaussianSplat/Private/GaussianSplatShadow.usf", "ShadowVS", SF_Vertex);

	class FShadowPS : public FShadowShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FShadowPS);
		SHADER_USE_PARAMETER_STRUCT(FShadowPS, FShadowShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(FVector3f, FalloffParameters)
			SHADER_PARAMETER(float, DepthOpacityThreshold)
		END_SHADER_PARAMETER_STRUCT()
	};
	IMPLEMENT_GLOBAL_SHADER(FShadowPS, "/Plugin/GaussianSplat/Private/GaussianSplatShadow.usf", "ShadowPS", SF_Pixel);

	BEGIN_SHADER_PARAMETER_STRUCT(FShadowRasterParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FShadowVS::FParameters, VS)
		SHADER_PARAMETER_STRUCT_INCLUDE(FShadowPS::FParameters, PS)
		RDG_BUFFER_ACCESS(DrawIndirectArgs, ERHIAccess::IndirectArgs)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	class FShadowParamsPS : public FShadowShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FShadowParamsPS);
		SHADER_USE_PARAMETER_STRUCT(FShadowParamsPS, FShadowShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_ARRAY(FVector4f, ShadowParams, [MaxParams])
			RENDER_TARGET_BINDING_SLOTS()
		END_SHADER_PARAMETER_STRUCT()
	};
	IMPLEMENT_GLOBAL_SHADER(FShadowParamsPS, "/Plugin/GaussianSplat/Private/GaussianSplatShadow.usf", "WriteParamsPS", SF_Pixel);

	struct FShadowLight
	{
		const FLightSceneInfo* Light = nullptr;
		UTextureRenderTarget2D* Map = nullptr;
		UTextureRenderTarget2D* Params = nullptr;
	};

	// The first directional light whose light function references the splat shadow render targets
	FShadowLight FindShadowLight(const FViewInfo& View)
	{
		const FScene* Scene = View.Family->Scene ? View.Family->Scene->GetRenderScene() : nullptr;
		if (!Scene || !View.Family->EngineShowFlags.LightFunctions)
		{
			return {};
		}
		static const FName MapName(TEXT("RT_GaussianSplatShadowMap"));
		static const FName ParamsName(TEXT("RT_GaussianSplatShadowParams"));
		for (const FLightSceneInfo* Light : Scene->DirectionalLights)
		{
			const FMaterialRenderProxy* LightFunction = Light->Proxy ? Light->Proxy->GetLightFunctionMaterial() : nullptr;
			const FMaterial* Material = LightFunction ? LightFunction->GetMaterialNoFallback(View.GetFeatureLevel()) : nullptr;
			if (!Material)
			{
				continue;
			}
			FShadowLight Result;
			for (const TObjectPtr<UObject>& Texture : Material->GetReferencedTextures())
			{
				if (UTextureRenderTarget2D* Target = Cast<UTextureRenderTarget2D>(Texture.Get()))
				{
					if (Target->GetFName() == MapName) { Result.Map = Target; }
					if (Target->GetFName() == ParamsName) { Result.Params = Target; }
				}
			}
			if (Result.Map && Result.Params)
			{
				Result.Light = Light;
				return Result;
			}
		}
		return {};
	}

	FRDGTextureRef RegisterTarget(FRDGBuilder& GraphBuilder, UTextureRenderTarget2D* Target, const TCHAR* Name)
	{
		FTextureRenderTargetResource* Resource = Target->GetRenderTargetResource();
		FRHITexture* Texture = Resource ? Resource->GetRenderTargetTexture() : nullptr;
		return Texture ? GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Texture, Name)) : nullptr;
	}

	struct FCascade
	{
		FCascadeParameters Parameters;
		float TexelWorldSize = 0.0f;
		FVector4f Bias = FVector4f::Zero(); // depth bias, 1 / depth fade (normalised depth)
		TArray<const FGaussianSplatRenderProxy*, TInlineAllocator<8>> Casters;
	};

	// World-space interval of Box along Axis
	FVector2D ProjectBox(const FBox& Box, const FVector& Axis)
	{
		const double Center = FVector::DotProduct(Box.GetCenter(), Axis);
		const FVector Extent = Box.GetExtent();
		const double Radius = Extent.X * FMath::Abs(Axis.X) + Extent.Y * FMath::Abs(Axis.Y) + Extent.Z * FMath::Abs(Axis.Z);
		return FVector2D(Center - Radius, Center + Radius);
	}

	// Bounding sphere of each slice of the view frustum (stable under camera rotation), snapped to whole texels
	TArray<FCascade, TInlineAllocator<MaxCascades>> FitCascades(const FViewInfo& View, const FVector& LightDirection, int32 NumCascades, int32 Resolution, TConstArrayView<const FGaussianSplatRenderProxy*> Casters)
	{
		const FMatrix& Projection = View.ViewMatrices.GetProjectionMatrix();
		const double K2 = FMath::Square(1.0 / Projection.M[0][0]) + FMath::Square(1.0 / Projection.M[1][1]);
		const FVector Origin = View.ViewMatrices.GetViewOrigin();
		const FVector Forward = View.GetViewDirection();
		const double Distance = FMath::Max(CVarGaussianSplatShadowDistance.GetValueOnRenderThread(), 100.0f);
		const double Exponent = FMath::Max(CVarGaussianSplatShadowSplitExponent.GetValueOnRenderThread(), 1.0f);

		FVector Right, Up;
		LightDirection.FindBestAxisVectors(Right, Up);

		TArray<FBox, TInlineAllocator<16>> CasterBounds;
		for (const FGaussianSplatRenderProxy* Caster : Casters)
		{
			CasterBounds.Add(Caster->LocalBounds.IsValid ? FBox(Caster->LocalBounds.TransformBy(Caster->LocalToWorld)) : FBox(ForceInit));
		}

		TArray<FCascade, TInlineAllocator<MaxCascades>> Cascades;
		double Near = 0.0;
		for (int32 Index = 0; Index < NumCascades; ++Index)
		{
			const double Far = Distance * FMath::Pow(double(Index + 1) / NumCascades, Exponent);
			double Z = 0.5 * (Near + Far) * (1.0 + K2);
			double Radius;
			if (Z >= Far)
			{
				Z = Far;
				Radius = Far * FMath::Sqrt(K2);
			}
			else
			{
				Radius = FMath::Sqrt(FMath::Square(Far - Z) + Far * Far * K2);
			}
			Near = Far;

			const FVector Center = Origin + Forward * Z;
			const double TexelSize = 2.0 * Radius / Resolution;
			const double MinU = FMath::FloorToDouble(FVector::DotProduct(Center, Right) / TexelSize) * TexelSize - Radius;
			const double MaxV = FMath::FloorToDouble(FVector::DotProduct(Center, Up) / TexelSize) * TexelSize + Radius;
			const double CenterDepth = FVector::DotProduct(Center, LightDirection);
			double DepthMin = CenterDepth - Radius;
			const double DepthMax = CenterDepth + Radius;

			FCascade& Cascade = Cascades.AddDefaulted_GetRef();
			for (int32 CasterIndex = 0; CasterIndex < Casters.Num(); ++CasterIndex)
			{
				const FBox& Bounds = CasterBounds[CasterIndex];
				if (Bounds.IsValid)
				{
					const FVector2D U = ProjectBox(Bounds, Right);
					const FVector2D V = ProjectBox(Bounds, Up);
					const FVector2D D = ProjectBox(Bounds, LightDirection);
					if (U.Y < MinU || U.X > MinU + 2.0 * Radius || V.Y < MaxV - 2.0 * Radius || V.X > MaxV || D.X > DepthMax)
					{
						continue;
					}
					// Casters between the sun and the cascade still shadow it
					DepthMin = FMath::Min(DepthMin, D.X);
				}
				Cascade.Casters.Add(Casters[CasterIndex]);
			}

			const double InvTexel = 1.0 / TexelSize;
			const double InvDepth = 1.0 / FMath::Max(DepthMax - DepthMin, 1.0);
			Cascade.Parameters.CascadeU = FVector4f(FVector3f(Right * InvTexel), float(-MinU * InvTexel));
			Cascade.Parameters.CascadeV = FVector4f(FVector3f(-Up * InvTexel), float(MaxV * InvTexel));
			Cascade.Parameters.CascadeDepth = FVector4f(FVector3f(LightDirection * InvDepth), float(-DepthMin * InvDepth));
			Cascade.Parameters.CascadeResolution = float(Resolution);
			Cascade.TexelWorldSize = float(TexelSize);
			const double Bias = CVarGaussianSplatShadowBias.GetValueOnRenderThread() + TexelSize;
			const double Fade = FMath::Max(CVarGaussianSplatShadowFade.GetValueOnRenderThread(), 0.1f);
			Cascade.Bias = FVector4f(float(Bias * InvDepth), float(1.0 / (Fade * InvDepth)), 0.0f, 0.0f);
		}
		return Cascades;
	}

	// Appends the splats of Caster that touch the cascade to ShadowSplats
	void ProjectCaster(FRDGBuilder& GraphBuilder, FGlobalShaderMap* ShaderMap, const FCascade& Cascade, const FGaussianSplatRenderProxy& Caster,
		uint32 MaxShadowSplats, FRDGBufferRef ShadowSplats, FRDGBufferRef ShadowSplatCount)
	{
		FRDGBufferRef SplatBuffer = GraphBuilder.RegisterExternalBuffer(Caster.GpuBuffer);
		const bool bClusters = Caster.HasClusters();

		FShadowProjectSplatsCS::FParameters* Parameters = GraphBuilder.AllocParameters<FShadowProjectSplatsCS::FParameters>();
		Parameters->Cascade = Cascade.Parameters;
		Parameters->LocalToWorld = Caster.LocalToWorld;
		Parameters->NumSplats = Caster.NumSplats;
		Parameters->GaussianSplatBuffer = GraphBuilder.CreateSRV(SplatBuffer);
		Parameters->BufferFormat = Caster.BufferFormat;
		Parameters->BufferStride = Caster.GpuDataStride;
		Parameters->PositionScale = Caster.PositionScale;
		Parameters->PositionBias = Caster.PositionBias;
		Parameters->ScaleScale = Caster.ScaleScale;
		Parameters->ScaleBias = Caster.ScaleBias;
		Parameters->ColorScale = Caster.ColorScale;
		Parameters->ColorBias = Caster.ColorBias;
		Parameters->PointScaleFactor = Caster.PointScaleFactor;
		Parameters->OpacityScale = Caster.OpacityScale;
		Parameters->OpacityThreshold = FMath::Max(CVarGaussianSplatShadowOpacityThreshold.GetValueOnRenderThread(), 0.0f);
		Parameters->UseClusters = bClusters ? 1u : 0u;
		Parameters->MaxShadowSplats = MaxShadowSplats;
		Parameters->RWShadowSplats = GraphBuilder.CreateUAV(ShadowSplats);
		Parameters->RWShadowSplatCount = GraphBuilder.CreateUAV(ShadowSplatCount, PF_R32_UINT);

		TShaderMapRef<FShadowProjectSplatsCS> ProjectCS(ShaderMap);
		if (!bClusters)
		{
			Parameters->ClusterBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(FGaussianSplatCluster)));
			Parameters->ClusterIndexBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultBuffer(GraphBuilder, sizeof(uint32)), PF_R32_UINT);
			Parameters->ClusterCountBuffer = Parameters->ClusterIndexBuffer;
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("ProjectSplats"), ERDGPassFlags::Compute, ProjectCS, Parameters,
				FComputeShaderUtils::GetGroupCountWrapped(Caster.NumSplats, ThreadGroupSize));
			return;
		}

		// Clusters in the cascade at its level of detail, then one group per cluster
		FRDGBufferRef ClusterBuffer = GraphBuilder.RegisterExternalBuffer(Caster.ClusterBuffer);
		FRDGBufferRef ClusterIndexBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), Caster.NumClusters), TEXT("GaussianSplat.Shadow.Clusters"));
		FRDGBufferRef ClusterCountBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("GaussianSplat.Shadow.ClusterCount"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ClusterCountBuffer, PF_R32_UINT), 0u);
		{
			FShadowCullClustersCS::FParameters* CullParameters = GraphBuilder.AllocParameters<FShadowCullClustersCS::FParameters>();
			CullParameters->Cascade = Cascade.Parameters;
			CullParameters->LocalToWorld = Caster.LocalToWorld;
			CullParameters->NumClusters = Caster.NumClusters;
			CullParameters->PointScaleFactor = Caster.PointScaleFactor;
			CullParameters->TexelWorldSize = Cascade.TexelWorldSize;
			CullParameters->ErrorThreshold = CVarGaussianSplatShadowErrorThreshold.GetValueOnRenderThread();
			CullParameters->ClusterBuffer = GraphBuilder.CreateSRV(ClusterBuffer);
			CullParameters->RWClusterOutputBuffer = GraphBuilder.CreateUAV(ClusterIndexBuffer, PF_R32_UINT);
			CullParameters->RWClusterCount = GraphBuilder.CreateUAV(ClusterCountBuffer, PF_R32_UINT);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CullClusters"), ERDGPassFlags::Compute, TShaderMapRef<FShadowCullClustersCS>(ShaderMap), CullParameters,
				FComputeShaderUtils::GetGroupCountWrapped(Caster.NumClusters, ThreadGroupSize));
		}
		FRDGBufferRef DispatchArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), TEXT("GaussianSplat.Shadow.DispatchArgs"));
		{
			FShadowDispatchArgsCS::FParameters* ArgsParameters = GraphBuilder.AllocParameters<FShadowDispatchArgsCS::FParameters>();
			ArgsParameters->ClusterCountBuffer = GraphBuilder.CreateSRV(ClusterCountBuffer, PF_R32_UINT);
			ArgsParameters->DimensionLimit = FUintVector2(GRHIMaxDispatchThreadGroupsPerDimension.X, GRHIMaxDispatchThreadGroupsPerDimension.Y);
			ArgsParameters->RWDispatchIndirectArgs = GraphBuilder.CreateUAV(DispatchArgs, PF_R32_UINT);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DispatchArgs"), TShaderMapRef<FShadowDispatchArgsCS>(ShaderMap), ArgsParameters, FIntVector(1, 1, 1));
		}

		Parameters->ClusterBuffer = GraphBuilder.CreateSRV(ClusterBuffer);
		Parameters->ClusterIndexBuffer = GraphBuilder.CreateSRV(ClusterIndexBuffer, PF_R32_UINT);
		Parameters->ClusterCountBuffer = GraphBuilder.CreateSRV(ClusterCountBuffer, PF_R32_UINT);
		Parameters->IndirectArgs = DispatchArgs;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("ProjectSplats(Clusters)"), ERDGPassFlags::Compute, ProjectCS, Parameters, DispatchArgs, 0);
	}
}

void GaussianSplat::RenderShadows(FRDGBuilder& GraphBuilder, const FViewInfo& View, TConstArrayView<const FGaussianSplatRenderProxy*> Casters)
{
	// One map per family, fitted to its first view, drawn before the family's lighting passes
	if (Casters.IsEmpty() || CVarGaussianSplatShadow.GetValueOnRenderThread() == 0 || View.Family->Views.IsEmpty() || View.Family->Views[0] != &View
		|| !View.IsPerspectiveProjection() || !View.CachedViewUniformShaderParameters.IsValid())
	{
		return;
	}
	const FShadowLight ShadowLight = FindShadowLight(View);
	if (!ShadowLight.Light)
	{
		return;
	}
	FRDGTextureRef Map = RegisterTarget(GraphBuilder, ShadowLight.Map, TEXT("GaussianSplat.ShadowMap"));
	FRDGTextureRef Params = RegisterTarget(GraphBuilder, ShadowLight.Params, TEXT("GaussianSplat.ShadowParams"));
	if (!Map || !Params || Params->Desc.Extent.X < MaxParams)
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "GaussianSplat.Shadow");
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetShaderPlatform());

	const int32 Resolution = Map->Desc.Extent.Y;
	const int32 NumCascades = FMath::Clamp(FMath::Min(CVarGaussianSplatShadowCascades.GetValueOnRenderThread(), Map->Desc.Extent.X / Resolution), 1, MaxCascades);
	const TArray<FCascade, TInlineAllocator<MaxCascades>> Cascades = FitCascades(View, ShadowLight.Light->Proxy->GetDirection(), NumCascades, Resolution, Casters);

	AddClearRenderTargetPass(GraphBuilder, Map, FLinearColor::White);

	for (int32 Index = 0; Index < Cascades.Num(); ++Index)
	{
		const FCascade& Cascade = Cascades[Index];
		if (Cascade.Casters.IsEmpty())
		{
			continue;
		}
		RDG_EVENT_SCOPE(GraphBuilder, "Cascade%d", Index);

		uint64 Capacity = 0;
		for (const FGaussianSplatRenderProxy* Caster : Cascade.Casters)
		{
			Capacity += Caster->HasClusters() ? Caster->NumSplats : Caster->GetNumLOD0Splats();
		}
		const uint32 MaxShadowSplats = uint32(FMath::Clamp<uint64>(Capacity, 1, FMath::Max(CVarGaussianSplatShadowMaxSplats.GetValueOnRenderThread(), 1)));

		FRDGBufferRef ShadowSplats = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector4), MaxShadowSplats), TEXT("GaussianSplat.Shadow.Splats"));
		FRDGBufferRef ShadowSplatCount = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("GaussianSplat.Shadow.SplatCount"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ShadowSplatCount, PF_R32_UINT), 0u);
		for (const FGaussianSplatRenderProxy* Caster : Cascade.Casters)
		{
			ProjectCaster(GraphBuilder, ShaderMap, Cascade, *Caster, MaxShadowSplats, ShadowSplats, ShadowSplatCount);
		}

		FRDGBufferRef DrawArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDrawIndirectParameters>(1), TEXT("GaussianSplat.Shadow.DrawArgs"));
		{
			FShadowDrawArgsCS::FParameters* ArgsParameters = GraphBuilder.AllocParameters<FShadowDrawArgsCS::FParameters>();
			ArgsParameters->ShadowSplatCount = GraphBuilder.CreateSRV(ShadowSplatCount, PF_R32_UINT);
			ArgsParameters->MaxShadowSplats = MaxShadowSplats;
			ArgsParameters->RWDrawIndirectArgs = GraphBuilder.CreateUAV(DrawArgs, PF_R32_UINT);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DrawArgs"), TShaderMapRef<FShadowDrawArgsCS>(ShaderMap), ArgsParameters, FIntVector(1, 1, 1));
		}

		FShadowRasterParameters* RasterParameters = GraphBuilder.AllocParameters<FShadowRasterParameters>();
		RasterParameters->VS.ShadowSplats = GraphBuilder.CreateSRV(ShadowSplats);
		RasterParameters->VS.CascadeResolution = float(Resolution);
		RasterParameters->PS.FalloffParameters = GaussianSplat::GetSplatFalloffParameters();
		RasterParameters->PS.DepthOpacityThreshold = CVarGaussianSplatShadowDepthOpacity.GetValueOnRenderThread();
		RasterParameters->DrawIndirectArgs = DrawArgs;
		RasterParameters->RenderTargets[0] = FRenderTargetBinding(Map, ERenderTargetLoadAction::ELoad);

		TShaderMapRef<FShadowVS> VertexShader(ShaderMap);
		TShaderMapRef<FShadowPS> PixelShader(ShaderMap);
		const FIntRect Slot(Index * Resolution, 0, (Index + 1) * Resolution, Resolution);
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("Draw"),
			RasterParameters,
			ERDGPassFlags::Raster,
			[RasterParameters, VertexShader, PixelShader, Slot](FRHICommandList& RHICmdList)
			{
				FGraphicsPipelineStateInitializer GraphicsPSOInit;
				RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
				// rgb *= 1 - alpha (transmittance), a = min(a, depth)
				GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_DestColor, BF_Zero, BO_Min, BF_One, BF_One>::GetRHI();
				GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
				GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;
				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

				RHICmdList.SetViewport(Slot.Min.X, Slot.Min.Y, 0.0f, Slot.Max.X, Slot.Max.Y, 1.0f);
				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), RasterParameters->VS);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), RasterParameters->PS);

				RasterParameters->DrawIndirectArgs->MarkResourceAsUsed();
				RHICmdList.DrawPrimitiveIndirect(RasterParameters->DrawIndirectArgs->GetIndirectRHICallBuffer(), 0);
			});
	}

	// The light function reads the cascades through these; the frame number tells it the map is current
	{
		FShadowParamsPS::FParameters* Parameters = GraphBuilder.AllocParameters<FShadowParamsPS::FParameters>();
		const uint32 FrameNumber = View.CachedViewUniformShaderParameters->FrameNumber;
		Parameters->ShadowParams[0] = FVector4f(float(FrameNumber & 0xFFFFFFu), float(Cascades.Num()), float(Resolution), 0.0f);
		for (int32 Index = 0; Index < Cascades.Num(); ++Index)
		{
			const FCascade& Cascade = Cascades[Index];
			Parameters->ShadowParams[1 + 4 * Index] = Cascade.Parameters.CascadeU;
			Parameters->ShadowParams[2 + 4 * Index] = Cascade.Parameters.CascadeV;
			Parameters->ShadowParams[3 + 4 * Index] = Cascade.Parameters.CascadeDepth;
			Parameters->ShadowParams[4 + 4 * Index] = Cascade.Bias;
		}
		Parameters->RenderTargets[0] = FRenderTargetBinding(Params, ERenderTargetLoadAction::ENoAction);
		FPixelShaderUtils::AddFullscreenPass(GraphBuilder, ShaderMap, RDG_EVENT_NAME("Params"), TShaderMapRef<FShadowParamsPS>(ShaderMap), Parameters, FIntRect(0, 0, MaxParams, 1));
	}

	GraphBuilder.UseExternalAccessMode(Map, ERHIAccess::SRVMask);
	GraphBuilder.UseExternalAccessMode(Params, ERHIAccess::SRVMask);
}
