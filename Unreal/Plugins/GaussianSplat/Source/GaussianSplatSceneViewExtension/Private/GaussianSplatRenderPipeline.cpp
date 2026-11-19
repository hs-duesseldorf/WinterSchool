// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatRenderPipeline.h"

#include "GaussianSplatCluster.h"
#include "GaussianSplatInverseToneCurveLUT.h"
#include "GaussianSplatRenderStats.h"

#include "SceneView.h"
#include "ScreenPass.h"
#include "GPUSort.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "ShaderCompilerCore.h"
#include "RenderGraphUtils.h"
#include "RenderGraphResources.h"
#include "PixelShaderUtils.h"
#include "RHIStaticStates.h"
#include "CommonRenderResources.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "PixelFormat.h"
#include "RHIGlobals.h"
#include "BlueNoise.h"
#include "SystemTextures.h"
#include "SceneRendering.h"
#include "GlobalRenderResources.h"
#include "SceneTextures.h"
#include "RenderUtils.h"
#include "SceneUtils.h"
#include "PostProcess/SceneRenderTargets.h"

DECLARE_GPU_STAT(GaussianSplat);

static TAutoConsoleVariable<int32> CVarGaussianSplatDebug(
	TEXT("r.GaussianSplat.Debug"),
	0,
	TEXT("Enables debugging environment (default = 0)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatTileSize(
	TEXT("r.GaussianSplat.TileSize"),
	8,
	TEXT("The dimension for tiled rasterization (default = 8)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatMaxSplatOverlap(
	TEXT("r.GaussianSplat.MaxSplatOverlap"),
	16,
	TEXT("The maximum number of splat overlap to support (default = 16)\n")
	TEXT("Presented as the high-water mark for splat budget allocation: NumSplats * MaxSplatOverlap"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatHardwareRasterization(
	TEXT("r.GaussianSplat.HardwareRasterization"),
	1,
	TEXT("Use hardware rasterization (VS+PS) instead of tiled compute (default = 1)"),
	ECVF_RenderThreadSafe | ECVF_ReadOnly);

static TAutoConsoleVariable<int32> CVarGaussianSplatGPUDrivenPipeline(
	TEXT("r.GaussianSplat.Debug.GPUDrivenPipeline"),
	1,
	TEXT("Enables GPU-Driven Pipeline (default = 1)\n")
	TEXT("Requires debug mode to toggle"),
	ECVF_RenderThreadSafe | ECVF_ReadOnly);

static TAutoConsoleVariable<int32> CVarGaussianSplatBlueNoise(
	TEXT("r.GaussianSplat.BlueNoise"),
	1,
	TEXT("Determines if Blue Noise is used (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatDepth(
	TEXT("r.GaussianSplat.Depth"),
	0,
	TEXT("Determines if Gaussian Splats write to the depth buffer:\n")
	TEXT("0 = off (default)\n")
	TEXT("1 = hardware depth\n")
	TEXT("2 = stochastic depth\n")
	TEXT("3 = expected depth"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatStochasticDepthFormat(
	TEXT("r.GaussianSplat.StochasticDepthFormat"),
	1,
	TEXT("Backing format for depth:\n")
	TEXT("0 = 8-bit unorm\n")
	TEXT("1 = 16-bit unorm (default)\n")
	TEXT("2 = 16-bit floating point\n")
	TEXT("3 = 32-bit floating point\n"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatVelocity(
	TEXT("r.GaussianSplat.Velocity"),
	0,
	TEXT("Write into the scene velocity buffer (default = 0)\n")
	TEXT("Velocity writes require depth writes to also be enabled"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGaussianSplatHardwareRasterizationOpacityThreshold(
	TEXT("r.GaussianSplat.HardwareRasterization.OpacityThreshold"),
	0.001f,
	TEXT("Minimum alpha for a fragment to be accepted (default = 0.001)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGaussianSplatShadingOpacityThreshold(
	TEXT("r.GaussianSplat.Shading.OpacityThreshold"),
	0.0f,
	TEXT("Minimum alpha for a Gaussian to be shaded (default = 0)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatShadingFrustumCull(
	TEXT("r.GaussianSplat.Debug.FrustumCull"),
	1,
	TEXT("Apply frustum culling during shading (default = 1)\n")
	TEXT("Requires debug mode to toggle"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatClusterCulling(
	TEXT("r.GaussianSplat.Debug.ClusterCulling"),
	1,
	TEXT("Bundle splats into clusters and apply view-based culling (default = 1)\n")
	TEXT("Requires debug mode to toggle"),
	ECVF_RenderThreadSafe | ECVF_ReadOnly);

static TAutoConsoleVariable<float> CVarGaussianSplatExpectedDepthScale(
	TEXT("r.GaussianSplat.ExpectedDepthScale"),
	100.0f,
	TEXT("The scaling divisor when computing expected depth (default = 100)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatSortQuality(
	TEXT("r.GaussianSplat.SortQuality"),
	3,
	TEXT("Depth sort precision, lower bit-count leads to faster sorting with less stability\n")
	TEXT("0 = Reference, 32-bit precision \n")
	TEXT("1 = Low, 16-bit precision \n")
	TEXT("2 = Medium, 20-bit precision\n")
	TEXT("3 = High, 24-bit precision (default)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatGatherShadedSplats(
	TEXT("r.GaussianSplat.GatherShadedSplats"),
	-1,
	TEXT("Gather shaded splats into sorted order. Required for ES 3.1, optional for SM5\n")
	TEXT("-1 = auto-detect for the current platform (default)\n")
	TEXT(" 0 = off\n")
	TEXT(" 1 = on"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatVertexAttributeFetch(
	TEXT("r.GaussianSplat.VertexAttributeFetch"),
	-1,
	TEXT("Use per-instance vertex attributes instead of SRV input during hardware rasterization\n")
	TEXT("Required for ES 3.1, optional for SM5\n")
	TEXT("-1 = auto-detect for the current platform (default)\n")
	TEXT(" 0 = off\n")
	TEXT(" 1 = on"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatEmulateGLES31(
	TEXT("r.GaussianSplat.EmulateGLES31"),
	0,
	TEXT("Emulate execution for GL ES 3.1 (default=0)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatLOD(
	TEXT("r.GaussianSplat.LOD"),
	-1,
	TEXT("Determines active LOD setting\n")
	TEXT("-1: Screen-based LOD (default)\n")
	TEXT(" 0: LOD Level 0\n")
	TEXT(" 1: LOD Level 1\n")
	TEXT(" X: LOD Level X"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGaussianSplatErrorThreshold(
	TEXT("r.GaussianSplat.ErrorThreshold"),
	1.0f,
	TEXT("Error Threshold for LOD in pixels (default=1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatFalloff(
	TEXT("r.GaussianSplat.Falloff"),
	1,
	TEXT("Gaussian falloff function used during rasterization\n")
	TEXT("0: Standard Gaussian falloff, truncated at the boundary\n")
	TEXT("1: C0 continuous, peak normalized. Drives falloff to 0 at the boundary, and normalized based on peak intensity (default)\n")
	TEXT("2: C0 continuous, mass normalized. Drives falloff to 0 at the boundary, and normalized based on integrated energy\n")
	TEXT("3: C1 continuous, peak normalized. Drives falloff and slope to 0 at the boundary, and normalized based on peak intensity\n")
	TEXT("4: C1 continuous, mass normalized. Drives falloff and slope to 0 at the boundary, and normalized based on integrated energy"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatGlobalSort(
	TEXT("r.GaussianSplat.GlobalSort"),
	1,
	TEXT("WinterSchool fork: how splats of several components are composited\n")
	TEXT("0 = per component (Epic): one sort + draw per component, components ordered by their bounds centre. Splats of\n")
	TEXT("    different components never interleave, so a small one inside a big one (a character in a scene) can vanish\n")
	TEXT("1 = global (default): the visible splats of all components go into one buffer, one sort, one draw. The inverse\n")
	TEXT("    tone curve is then applied per splat instead of to the composited result.\n")
	TEXT("Needs the hardware-rasterised, GPU-driven pipeline (the default, also on ES 3.1 / WebGPU); falls back to 0 otherwise."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatLighting(
	TEXT("r.GaussianSplat.Lighting"),
	1,
	TEXT("WinterSchool fork: lets the scene's lights and shadows act on splats\n")
	TEXT("0 = off: splats are composited unlit after the opaque pass (Epic)\n")
	TEXT("1 = on (default): with the deferred renderer, splats are written into the GBuffer after the base pass (depth, normal,\n")
	TEXT("    albedo and the baked remainder), so every light, its shadows (also from meshes onto splats), fog and TSR treat them\n")
	TEXT("    like opaque surfaces; the capture's indirect light stays baked in (see .ShadowStrength). Desktop deferred and mobile\n")
	TEXT("    deferred (multipass, e.g. WebGPU); per component amount: LightingStrength. Forward renderers and unlit views keep\n")
	TEXT("    the composite."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatLightingDither(
	TEXT("r.GaussianSplat.Lighting.DitheredCoverage"),
	0,
	TEXT("How partly covered pixels (soft splat edges) enter the GBuffer, which stores one opaque surface per pixel:\n")
	TEXT("0 = a pixel becomes the splat surface if its coverage is at least r.GaussianSplat.Lighting.CoverageThreshold, the\n")
	TEXT("    rest is composited unlit (default; stable, as on mobile/web)\n")
	TEXT("1 = temporally dithered against the coverage like a dithered opacity mask, resolved by TSR/TAA. Softer edges, but\n")
	TEXT("    the depth of the pixel changes every frame, which shows as flicker on fuzzy splats"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGaussianSplatLightingCoverageThreshold(
	TEXT("r.GaussianSplat.Lighting.CoverageThreshold"),
	0.9f,
	TEXT("Coverage a pixel needs to become a splat surface without dithering (used when there is no temporal AA, e.g. FXAA\n")
	TEXT("on mobile/web; default = 0.9). Lower pixels are composited unlit over what is behind them: a low threshold turns\n")
	TEXT("semi-transparent floaters (often in a capture's sky) into solid dark blobs."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGaussianSplatLightingSpecular(
	TEXT("r.GaussianSplat.Lighting.Specular"),
	0.0f,
	TEXT("GBuffer Specular of lit splats (default = 0: the captured colors already contain the view-dependent highlights)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGaussianSplatLightingRoughness(
	TEXT("r.GaussianSplat.Lighting.Roughness"),
	1.0f,
	TEXT("GBuffer Roughness of lit splats (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGaussianSplatLightingShadowStrength(
	TEXT("r.GaussianSplat.Lighting.ShadowStrength"),
	1.0f,
	TEXT("Global multiplier on the components' ShadowStrength, the share of a lit splat's captured colour that a full shadow\n")
	TEXT("of the directional light takes away (default = 1; 0 = splats receive no shadows). The shadow's darkness does not\n")
	TEXT("depend on the light's intensity: that sets how bright the capture's sun is taken to be, so how strongly other lights\n")
	TEXT("brighten splats. The shadow's hue follows the lights, as a real shadow is lit by the sky alone: Sky / (Sun + Sky),\n")
	TEXT("so a bluer sky light or a warmer sun gives bluer shadows. Where a channel of the sun is too weak for its share\n")
	TEXT("(albedo > 1) that channel's shadow is weakened; see .ShowClipping."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatLightingShowClipping(
	TEXT("r.GaussianSplat.Lighting.ShowClipping"),
	0,
	TEXT("1 = tint lit splats magenta where the lights are too weak for their shadow (the albedo the shadow needs is above 1,\n")
	TEXT("so the shadow there comes out lighter). Raise the directional light's intensity until it is gone."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGaussianSplatLightingSurfaceOffset(
	TEXT("r.GaussianSplat.Lighting.SurfaceOffset"),
	4.0f,
	TEXT("Distance in cm a lit splat surface is lifted out of the splats along its normal (default = 4). The surface is the\n")
	TEXT("depth averaged over the splats seen through a pixel, which lies inside a fuzzy surface; without the lift shadows\n")
	TEXT("cast on it look sunk in."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGaussianSplatDownsampleFactor(
	TEXT("r.GaussianSplat.DownsampleFactor"),
	1,
	TEXT("Integer divisor applied to rendered viewport (default = 1)"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

namespace
{
	int32 GetTileSize()
	{
		return FMath::Max(CVarGaussianSplatTileSize.GetValueOnRenderThread(), 1);
	}

	int32 GetMaxSplatOverlap()
	{
		return FMath::Max(CVarGaussianSplatMaxSplatOverlap.GetValueOnRenderThread(), 1);
	}

	bool IsDebug()
	{
		return CVarGaussianSplatDebug.GetValueOnRenderThread() != 0;
	}

	enum class EGaussianSplatDepthMode : uint8
	{
		Off = 0,
		Hardware = 1,
		Stochastic = 2,
		Expected = 3
	};

	EGaussianSplatDepthMode GetDepthMode()
	{
		return static_cast<EGaussianSplatDepthMode>(FMath::Clamp(CVarGaussianSplatDepth.GetValueOnRenderThread(), 0, 3));
	}

	bool IsBlueNoiseEnabled()
	{
		return CVarGaussianSplatBlueNoise.GetValueOnRenderThread() != 0;
	}

	EPixelFormat GetStochasticDepthFormat()
	{
		switch (CVarGaussianSplatStochasticDepthFormat.GetValueOnRenderThread())
		{
		case 0:  return PF_G8;
		case 1:  return PF_G16;
		case 2:  return PF_R16F;
		default: return PF_R32_FLOAT;
		}
	}

	bool IsVelocityEnabled()
	{
		return CVarGaussianSplatVelocity.GetValueOnRenderThread() != 0;
	}

	float GetHardwareRasterizationOpacityThreshold()
	{
		return FMath::Max(CVarGaussianSplatHardwareRasterizationOpacityThreshold.GetValueOnRenderThread(), 0.0f);
	}

	float GetShadingOpacityThreshold()
	{
		return FMath::Max(CVarGaussianSplatShadingOpacityThreshold.GetValueOnRenderThread(), 0.0f);
	}

	bool IsFrustumCullingEnabledForShading()
	{
		return CVarGaussianSplatShadingFrustumCull.GetValueOnRenderThread() != 0;
	}

	bool IsClusterCullingEnabled()
	{
		return CVarGaussianSplatClusterCulling.GetValueOnRenderThread() != 0;
	}

	bool PlatformSupportsWaveOps(EShaderPlatform ShaderPlatform)
	{
		return FDataDrivenShaderPlatformInfo::GetSupportsWaveOperations(ShaderPlatform) == ERHIFeatureSupport::RuntimeGuaranteed;
	}

	int32 GetSortKeyBits(EShaderPlatform ShaderPlatform)
	{
		int32 SortKeyBits = 32;

		if (GaussianSplat::IsHardwareRasterization(ShaderPlatform))
		{
			int32 SortKeyMode = CVarGaussianSplatSortQuality.GetValueOnRenderThread();
			switch (SortKeyMode)
			{
			case 1:
				SortKeyBits = 16;
				break;
			case 2:
				SortKeyBits = 20;
				break;
			case 3:
				SortKeyBits = 24;
				break;
			//case 0:
			//default:
			//	SortKeyBits = 32;
			}
		}

		return SortKeyBits;
	}

	// Bits set in the radix-sort KeyMask (drives GetGPUSortPassCount): low N bits.
	uint32 GetSortKeyMask(EShaderPlatform ShaderPlatform)
	{
		const int32 Bits = GetSortKeyBits(ShaderPlatform);
		return (Bits >= 32) ? 0xFFFFFFFFu : ((1u << Bits) - 1u);
	}

	float GetExpectedDepthScale()
	{
		return FMath::Max(CVarGaussianSplatExpectedDepthScale.GetValueOnRenderThread(), 0.001f);
	}

	enum class EGaussianSplatFalloff : uint8
	{
		Standard = 0,
		C0_PeakNormalized = 1,
		C0_MassNormalized = 2,
		C1_PeakNormalized = 3,
		C1_MassNormalized = 4
	};

	EGaussianSplatFalloff GetFalloff()
	{
		return static_cast<EGaussianSplatFalloff>(FMath::Clamp(CVarGaussianSplatFalloff.GetValueOnRenderThread(), 0, 4));
	}

	FVector3f GetFalloffParameters()
	{
		const float RadiusSquared = GAUSSIAN_SPLAT_RADIUS_SQUARED;
		const float E = FMath::Exp(-RadiusSquared);

		// Gaussian function falloff applied in the form:
		// A * exp(-r^2) + B * r^2 + C
		// E = exp(-R^2), R = GAUSSIAN_SPLAT_RADIUS
		switch (GetFalloff())
		{
			// (exp(-r^2) - E) / (1 - E)
			case EGaussianSplatFalloff::C0_PeakNormalized:
			{
				const float Scale = 1.0f / (1.0f - E);
				return FVector3f(Scale, 0.0f, -E * Scale);
			}
			// (exp(-r^2) - E) / (1 - E * (1 + R^2))
			case EGaussianSplatFalloff::C0_MassNormalized:
			{
				const float Scale = 1.0f / (1.0f - E * (1.0f + RadiusSquared));
				return FVector3f(Scale, 0.0f, -E * Scale);
			}
			// (exp(-r^2) + E * r^2 - E * (1 + R^2)) / (1 - E * (1 + R^2))
			case EGaussianSplatFalloff::C1_PeakNormalized:
			{
				const float Scale = 1.0f / (1.0f - E * (1.0f + RadiusSquared));
				return FVector3f(Scale, E * Scale, -E * (1.0f + RadiusSquared) * Scale);
			}
			// (exp(-r^2) + E * r^2 - E * (1 + R^2)) / I, with I = (1 - E) + 0.5 * E * R^4 - E * (1 + R^2) * R^2
			case EGaussianSplatFalloff::C1_MassNormalized:
			{
				const float Integral = (1.0f - E) + 0.5f * E * RadiusSquared * RadiusSquared - E * (1.0f + RadiusSquared) * RadiusSquared;
				const float Scale = 1.0f / Integral;
				return FVector3f(Scale, E * Scale, -E * (1.0f + RadiusSquared) * Scale);
			}
			// exp(-r^2)
			default:
				return FVector3f(1.0f, 0.0f, 0.0f);
		}
	}

	bool IsGLES31EmulationEnabled()
	{
		return CVarGaussianSplatEmulateGLES31.GetValueOnRenderThread() != 0;
	}

	int32 GetDownsampleFactor()
	{
		return FMath::Clamp(CVarGaussianSplatDownsampleFactor.GetValueOnRenderThread(), 1, 8);
	}

	bool IsFeatureLevelES31(EShaderPlatform ShaderPlatform)
	{
		return FDataDrivenShaderPlatformInfo::GetMaxFeatureLevel(ShaderPlatform) == ERHIFeatureLevel::ES3_1;
	}

	// ES 3.1 restricts thread group to 128
	constexpr int32 GaussianSplatMaxES31ThreadGroupSize = 128;
	bool PlatformSupportsThreadGroupSize(EShaderPlatform ShaderPlatform, int32 ThreadGroupSize)
	{
		return ThreadGroupSize <= GaussianSplatMaxES31ThreadGroupSize || !IsFeatureLevelES31(ShaderPlatform);
	}

	bool PlatformSupportsSoftwareRasterization(EShaderPlatform ShaderPlatform)
	{
		return !IsFeatureLevelES31(ShaderPlatform);
	}

	bool PlatformSupportsStochasticDepth(EShaderPlatform ShaderPlatform)
	{
		return !IsFeatureLevelES31(ShaderPlatform);
	}

	// GBuffer path (WinterSchool fork): platforms with a deferred renderer whose GBuffer the splats can be written into,
	// the desktop one (SM5, e.g. SP_WEBGPU_SM5) or mobile deferred (e.g. SP_WEBGPU_ES3_1_DEFERRED, the web's tier for
	// desktop-class GPUs). Mobile forward (phones) has no GBuffer and keeps the composite. The raster pass reads the
	// per-splat surface data by index, so vertex shader SRVs are needed.
	bool PlatformSupportsDeferredSplats(EShaderPlatform ShaderPlatform)
	{
		if (!FDataDrivenShaderPlatformInfo::GetSupportsVertexShaderSRVs(ShaderPlatform))
		{
			return false;
		}
		return IsMobilePlatform(ShaderPlatform)
			? IsMobileDeferredShadingEnabled(ShaderPlatform)
			: IsFeatureLevelSupported(ShaderPlatform, ERHIFeatureLevel::SM5);
	}

	bool SupportsSplatScratch(bool bHWRaster)
	{
		const EPixelFormatCapabilities RequiredCapabilities = EPixelFormatCapabilities::Texture2D
			| EPixelFormatCapabilities::TextureSample
			| EPixelFormatCapabilities::TextureLoad
			| EPixelFormatCapabilities::RenderTarget
			| (bHWRaster ? EPixelFormatCapabilities::TextureBlendable
				: EPixelFormatCapabilities::UAV | EPixelFormatCapabilities::TypedUAVLoad | EPixelFormatCapabilities::TypedUAVStore);
		
		const bool bSupported = GSupportsRenderTargetFormat_PF_FloatRGBA
			&& GPixelFormats[PF_FloatRGBA].Supported
			&& UE::PixelFormat::HasCapabilities(PF_FloatRGBA, RequiredCapabilities);
		
		static bool bWarnedUnsupportedScratch = false;
		if (!bSupported && !bWarnedUnsupportedScratch)
		{
			UE_LOGF(LogGaussianSplat, Warning, "Gaussian Splat RGBA16F scratch capabilities are unavailable. Inverse tone curve and expected depth are unavailable on unsupported rasterization paths.");
			bWarnedUnsupportedScratch = true;
		}

		return bSupported;
	}

	EGaussianSplatDepthMode GetDepthMode(EShaderPlatform ShaderPlatform)
	{
		const EGaussianSplatDepthMode Mode = GetDepthMode();
		if (Mode == EGaussianSplatDepthMode::Stochastic
			&& (!PlatformSupportsStochasticDepth(ShaderPlatform) || IsGLES31EmulationEnabled()))
		{
			// Degrade to a plain depth write rather than silently rendering without depth.
			return EGaussianSplatDepthMode::Hardware;
		}

		if (Mode == EGaussianSplatDepthMode::Expected
			&& !SupportsSplatScratch(GaussianSplat::IsHardwareRasterization(ShaderPlatform)))
		{
			// Expected depth uses RGBA16F for both color transmittance and depth moments.
			return EGaussianSplatDepthMode::Hardware;
		}

		return Mode;
	}

	// Sorting splats is required if the platform does not support SRVs in Vertex Shading
	bool IsGatherShadedSplatsEnabled(EShaderPlatform ShaderPlatform)
	{
		if (IsGLES31EmulationEnabled())
		{
			return true;
		}

		const int32 Mode = CVarGaussianSplatGatherShadedSplats.GetValueOnRenderThread();
		if (Mode >= 0)
		{
			return Mode != 0;
		}

		// TODO: Confirm that tiled GPUs perform better with the gather stage
		return !FDataDrivenShaderPlatformInfo::GetSupportsVertexShaderSRVs(ShaderPlatform)
			|| FDataDrivenShaderPlatformInfo::GetTargetsTiledGPU(ShaderPlatform);
	}

	// Vertex attribute fetch is required if the platform does not support SRVs in Vertex Shading
	bool IsVertexAttributeFetchEnabled(EShaderPlatform ShaderPlatform)
	{
		if (IsGLES31EmulationEnabled())
		{
			return true;
		}

		if (!FDataDrivenShaderPlatformInfo::GetSupportsVertexShaderSRVs(ShaderPlatform))
		{
			return true;
		}

		const int32 Mode = CVarGaussianSplatVertexAttributeFetch.GetValueOnRenderThread();
		if (Mode >= 0)
		{
			return Mode != 0;
		}

		return false;
	}
}

bool GaussianSplat::SupportsDeferredSplats(EShaderPlatform ShaderPlatform)
{
	return PlatformSupportsDeferredSplats(ShaderPlatform);
}

FVector3f GaussianSplat::GetSplatFalloffParameters()
{
	return GetFalloffParameters();
}

static FScreenPassTextureViewport GetSplatViewport(const FScreenPassRenderTarget& Output, int32 DownsampleFactor)
{
	if (DownsampleFactor > 1)
	{
		const FIntRect ViewRect(FIntPoint::ZeroValue, FIntPoint::DivideAndRoundUp(Output.ViewRect.Size(), DownsampleFactor));
		return FScreenPassTextureViewport(ViewRect.Size(), ViewRect);
	}
	return FScreenPassTextureViewport(Output);
}

static TUniformBufferRef<FViewUniformShaderParameters> CreateDownsampledViewUniformBuffer(const FSceneView& InView, const FScreenPassTextureViewport& Viewport)
{
	const FViewInfo& View = static_cast<const FViewInfo&>(InView);

	FViewUniformShaderParameters DownsampledViewParameters = *View.CachedViewUniformShaderParameters;
	View.SetupViewRectUniformBufferParameters(DownsampledViewParameters, Viewport.Extent, Viewport.Rect, View.ViewMatrices, View.PrevViewInfo.ViewMatrices);
	return TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(DownsampledViewParameters, UniformBuffer_SingleFrame);
}

bool GaussianSplat::IsHardwareRasterization()
{
	return CVarGaussianSplatHardwareRasterization.GetValueOnRenderThread() != 0
		|| IsGLES31EmulationEnabled();
}

bool GaussianSplat::IsHardwareRasterization(EShaderPlatform ShaderPlatform)
{
	return IsHardwareRasterization() || !PlatformSupportsSoftwareRasterization(ShaderPlatform);
}

bool GaussianSplat::IsGPUDrivenPipeline()
{
	return CVarGaussianSplatHardwareRasterization.GetValueOnRenderThread() != 0 &&
		CVarGaussianSplatGPUDrivenPipeline.GetValueOnRenderThread() != 0;
}

struct FShadedGaussianSplatData
{
	FVector4f Position;
	FVector4f Axes;
	FVector4f Color;
};

struct FPackedShadedGaussianSplatData
{
	uint32 Data[6];
};
// Mirrors FPackedShadedGaussianSplatData in GaussianSplatCommon.ush
static_assert(sizeof(FPackedShadedGaussianSplatData) == 24, "Packed shaded splat stride must match the shader layout");

constexpr uint32 GaussianSplatPackedRecordUInt32Count = sizeof(FPackedShadedGaussianSplatData) / sizeof(uint32);

// Attribute layout must match FPackedShadedGaussianSplatData
class FGaussianSplatInstanceVertexDeclaration : public FRenderResource
{
public:
	FVertexDeclarationRHIRef VertexDeclarationRHI;

	virtual ~FGaussianSplatInstanceVertexDeclaration() = default;

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		// TODO: Consider VET_Half2 for implicit floating-point conversion if platform supports it
		constexpr uint16 Stride = sizeof(FPackedShadedGaussianSplatData);
		FVertexDeclarationElementList Elements;
		Elements.Add(FVertexElement(0,  0, VET_UInt2, 0, Stride, /*bUseInstanceIndex*/ true)); // ViewZ, NDC
		Elements.Add(FVertexElement(0,  8, VET_UInt2, 1, Stride, /*bUseInstanceIndex*/ true)); // Axes
		Elements.Add(FVertexElement(0, 16, VET_UInt2, 2, Stride, /*bUseInstanceIndex*/ true)); // Color
		VertexDeclarationRHI = PipelineStateCache::GetOrCreateVertexDeclaration(Elements);
	}

	virtual void ReleaseRHI() override
	{
		VertexDeclarationRHI.SafeRelease();
	}
};

static TGlobalResource<FGaussianSplatInstanceVertexDeclaration> GGaussianSplatInstanceVertexDeclaration;

// Mirrors FGaussianSplatCluster in GaussianSplatCommon.ush
static_assert(sizeof(FGaussianSplatCluster) == 48, "Cluster stride must match the shader layout");

class FCullClustersCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCullClustersCS);
	SHADER_USE_PARAMETER_STRUCT(FCullClustersCS, FGlobalShader);

	// Permutations
	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Scene data
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		// Transform data
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)

		// Cluster data
		SHADER_PARAMETER(uint32, NumClusters)
		SHADER_PARAMETER(float, PointScaleFactor)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGaussianSplatCluster>, ClusterBuffer)

		// LOD data
		SHADER_PARAMETER(int32, LOD)
		SHADER_PARAMETER(float, ErrorThreshold)

		// Output
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWClusterOutputBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWClusterCount)
	END_SHADER_PARAMETER_STRUCT()

	static int32 GetThreadGroupSize1D() { return 64; }

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());

		// Unconditional, unlike the shading pass this has no non-compacting path to opt out of
		if (PlatformSupportsWaveOps(Parameters.Platform))
		{
			OutEnvironment.SetDefine(TEXT("GAUSSIAN_SPLAT_USE_WAVE_OPS"), 1);
			OutEnvironment.CompilerFlags.Add(CFLAG_WaveOperations);
		}
	}
};

IMPLEMENT_GLOBAL_SHADER(FCullClustersCS, "/Plugin/GaussianSplat/Private/GaussianSplatCullClusters.usf", "MainCS", SF_Compute);

class FGaussianSplatShadeIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatShadeIndirectArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatShadeIndirectArgsCS, FGlobalShader);

	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ClusterCountBuffer)
		SHADER_PARAMETER(FUintVector2, DimensionLimit)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWShadeIndirectArgs)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FGaussianSplatShadeIndirectArgsCS, "/Plugin/GaussianSplat/Private/GaussianSplatShadeIndirectArgs.usf", "MainCS", SF_Compute);


class FShadeGaussianSplatCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FShadeGaussianSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FShadeGaussianSplatCS, FGlobalShader);

	// Permutations
	class FShaderDebug : SHADER_PERMUTATION_BOOL("SHADER_DEBUG");
	class FSHDegree : SHADER_PERMUTATION_INT ("SH_DEGREE", 4);
	class FDeferred : SHADER_PERMUTATION_BOOL("GAUSSIAN_SPLAT_DEFERRED");
	using FPermutationDomain = TShaderPermutationDomain<FShaderDebug, FSHDegree, FDeferred>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Scene data
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		// Need private dependency on the Renderer module in your Build.cs:
		// PrivateDependencyModuleNames.Add("Renderer");
		// SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		// GBuffer path (FDeferred): per shaded splat its surface normal and lighting weight
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FUintVector2>, RWShadedSplatSurfaceBuffer)
		SHADER_PARAMETER(float, LightingStrength)

		// Debug pipeline configurations require FShaderDebug=1
		SHADER_PARAMETER(uint32, DebugGpuDrivenPipeline)
		SHADER_PARAMETER(uint32, DebugClustering)
		SHADER_PARAMETER(uint32, DebugFrustumCull)
		SHADER_PARAMETER(uint32, DebugWriteSortKeys)

		// Transform data
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)
		SHADER_PARAMETER(FMatrix44f, WorldToLocal)

		// Buffer data
		SHADER_PARAMETER(uint32, NumSplats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, GaussianSplatBuffer)

		// Cluster dispatch
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGaussianSplatCluster>, ClusterBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ClusterIndexBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ClusterCountBuffer)
		RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)

		// Compression
		SHADER_PARAMETER(uint32, BufferFormat)
		SHADER_PARAMETER(uint32, BufferStride)
		SHADER_PARAMETER(FVector3f, PositionScale)
		SHADER_PARAMETER(FVector3f, PositionBias)
		SHADER_PARAMETER(FVector3f, ScaleScale)
		SHADER_PARAMETER(FVector3f, ScaleBias)
		SHADER_PARAMETER(FVector3f, SHScale)
		SHADER_PARAMETER(FVector3f, SHBias)
		SHADER_PARAMETER(FVector4f, ColorScale)
		SHADER_PARAMETER(FVector4f, ColorBias)

		// Spherical harmonics
		SHADER_PARAMETER(uint32, SHCoordinateSpace)

		// Per-component overrides
		SHADER_PARAMETER(float, PointScaleFactor)
		SHADER_PARAMETER(float, OpacityScale)

		SHADER_PARAMETER(float, OpacityThreshold)
		SHADER_PARAMETER(float, CovarianceScale)

		// Per-splat inverse tone curve (global sort), strength 0 = off
		SHADER_PARAMETER_TEXTURE(Texture3D, InverseToneCurveLUT)
		SHADER_PARAMETER_SAMPLER(SamplerState, InverseToneCurveLUTSampler)
		SHADER_PARAMETER(float, InverseToneCurveStrength)
		SHADER_PARAMETER(float, InverseToneCurveLUTEdge)

		// Output
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FPackedShadedGaussianSplatData>, RWShadedGaussianSplatBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWSplatCount)

		// Optional sort-key data
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWSortKeys)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWSortValues)
		SHADER_PARAMETER(uint32, SortKeyFrontToBack)
		SHADER_PARAMETER(uint32, SortKeyBits)
	END_SHADER_PARAMETER_STRUCT()

	static int32 GetThreadGroupSize1D() { return 64; }

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		return !PermutationVector.Get<FDeferred>() || PlatformSupportsDeferredSplats(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());

		if (PlatformSupportsWaveOps(Parameters.Platform))
		{
			OutEnvironment.SetDefine(TEXT("GAUSSIAN_SPLAT_USE_WAVE_OPS"), 1);
			OutEnvironment.CompilerFlags.Add(CFLAG_WaveOperations);
		}
	}
};

IMPLEMENT_GLOBAL_SHADER(FShadeGaussianSplatCS, "/Plugin/GaussianSplat/Private/GaussianSplatShading.usf", "MainCS", SF_Compute);

// Applies the inverse tone curve to the splat scratch, then composites onto SceneColor
// via ROP premultiplied alpha-over with transmittance carried in .a to match UE
// SceneColor convention.
class FInverseToneCurvePostBlendPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FInverseToneCurvePostBlendPS);
	SHADER_USE_PARAMETER_STRUCT(FInverseToneCurvePostBlendPS, FGlobalShader);

	class FApplyInverseToneCurve : SHADER_PERMUTATION_BOOL("APPLY_INVERSE_TONE_CURVE");
	using FPermutationDomain = TShaderPermutationDomain<FApplyInverseToneCurve>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT(FScreenPassTextureInput, SplatColor)
		SHADER_PARAMETER_TEXTURE(Texture3D, InverseToneCurveLUT)
		SHADER_PARAMETER_SAMPLER(SamplerState, InverseToneCurveLUTSampler)
		SHADER_PARAMETER(FScreenTransform, SvPositionToInputTextureUV)
		SHADER_PARAMETER(float, Strength)
		SHADER_PARAMETER(float, InverseToneCurveLUTEdge)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FInverseToneCurvePostBlendPS, "/Plugin/GaussianSplat/Private/GaussianSplatInverseToneCurvePostBlend.usf", "MainPS", SF_Pixel);

struct FTileCountDebugData
{
	FIntPoint Tile0;
	FIntPoint Tile1;
};

class FTileCountGaussianSplatCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FTileCountGaussianSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FTileCountGaussianSplatCS, FGlobalShader);

	// Permutations
	class FShaderDebug : SHADER_PERMUTATION_BOOL("SHADER_DEBUG");
	using FPermutationDomain = TShaderPermutationDomain<FShaderDebug>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		SHADER_PARAMETER(uint32, NumSplats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FShadedGaussianSplatData>, ShadedGaussianSplatBuffer)

		SHADER_PARAMETER(FIntPoint, TileSize)
		SHADER_PARAMETER(FIntPoint, TileDimension)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWTileCountBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWMinDepthPerTileBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWMaxDepthPerTileBuffer)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FTileCountDebugData>, RWTileDebugBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static int32 GetThreadGroupSize1D() { return 64; }

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return PlatformSupportsSoftwareRasterization(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
	}
};

IMPLEMENT_GLOBAL_SHADER(FTileCountGaussianSplatCS, "/Plugin/GaussianSplat/Private/GaussianSplatTileCount.usf", "MainCS", SF_Compute);

class FPrefixSumGaussianSplatCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FPrefixSumGaussianSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FPrefixSumGaussianSplatCS, FGlobalShader);

	// Permutations
	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, NumItems)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ItemBuffer)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWPrefixSumBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static int32 GetThreadGroupSize1D() { return 1024; }

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return PlatformSupportsSoftwareRasterization(Parameters.Platform)
			&& PlatformSupportsThreadGroupSize(Parameters.Platform, GetThreadGroupSize1D());
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("NUM_THREADS_PER_GROUP"), GetThreadGroupSize1D());
	}
};

IMPLEMENT_GLOBAL_SHADER(FPrefixSumGaussianSplatCS, "/Plugin/GaussianSplat/Private/GaussianSplatPrefixSum.usf", "MainCS", SF_Compute);

class FFillSplatIndexListGaussianSplatCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFillSplatIndexListGaussianSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FFillSplatIndexListGaussianSplatCS, FGlobalShader);

	// Permutations
	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		// Splat data
		SHADER_PARAMETER(uint32, NumSplats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FShadedGaussianSplatData>, ShadedGaussianSplatBuffer)

		SHADER_PARAMETER(FIntPoint, TileSize)
		SHADER_PARAMETER(FIntPoint, TileDimension)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SplatOffsetPerTileBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MinDepthPerTileBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MaxDepthPerTileBuffer)

		// Output
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWNumSplatsPerTileBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWSortKeyBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWSplatIndexBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static int32 GetThreadGroupSize1D() { return 64; }

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return PlatformSupportsSoftwareRasterization(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
	}
};

IMPLEMENT_GLOBAL_SHADER(FFillSplatIndexListGaussianSplatCS, "/Plugin/GaussianSplat/Private/GaussianSplatFillSplatIndexBuffer.usf", "MainCS", SF_Compute);

class FGaussianSplatDrawIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatDrawIndirectArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatDrawIndirectArgsCS, FGlobalShader);

	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SplatCountBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWDrawIndirectArgs)
		SHADER_PARAMETER_STRUCT_INCLUDE(FGaussianSplatStatsParameters, Stats)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FGaussianSplatDrawIndirectArgsCS, "/Plugin/GaussianSplat/Private/GaussianSplatDrawIndirectArgs.usf", "MainCS", SF_Compute);

BEGIN_SHADER_PARAMETER_STRUCT(FGPUSortPassParameters, )
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RemoteKeySRVs0)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RemoteKeySRVs1)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RemoteKeyUAVs0)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RemoteKeyUAVs1)

	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RemoteValueSRVs0)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RemoteValueSRVs1)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RemoteValueUAVs0)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RemoteValueUAVs1)

	// Bound only on the indirect path (SortGPUBuffersIndirect); left null for the direct path.
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SplatCountSRV)
END_SHADER_PARAMETER_STRUCT()

class FRasterizeGaussianSplatCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRasterizeGaussianSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FRasterizeGaussianSplatCS, FGlobalShader);

	// Permutations
	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Scene data
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		// Need private dependency on the Renderer module in your Build.cs:
		// PrivateDependencyModuleNames.Add("Renderer");
		// SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		// Splat data
		SHADER_PARAMETER(uint32, NumSplats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FShadedGaussianSplatData>, ShadedGaussianSplatBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint32>, SortedIndexBuffer)
		SHADER_PARAMETER(FVector3f, FalloffParameters)

		// Output
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWColorTexture)
	END_SHADER_PARAMETER_STRUCT()

	static int32 GetThreadGroupSize2D() { return 8; }

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return PlatformSupportsSoftwareRasterization(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}
};

IMPLEMENT_GLOBAL_SHADER(FRasterizeGaussianSplatCS, "/Plugin/GaussianSplat/Private/GaussianSplatRasterization.usf", "MainCS", SF_Compute);

class FTileRasterizeGaussianSplatCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FTileRasterizeGaussianSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FTileRasterizeGaussianSplatCS, FGlobalShader);

	// Permutations
	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Scene data
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		// Need private dependency on the Renderer module in your Build.cs:
		// PrivateDependencyModuleNames.Add("Renderer");
		// SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		// Splat data
		SHADER_PARAMETER(uint32, NumSplats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FShadedGaussianSplatData>, ShadedGaussianSplatBuffer)

		SHADER_PARAMETER(FIntPoint, TileSize)
		SHADER_PARAMETER(FIntPoint, TileDimension)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, NumSplatsPerTileBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SplatOffsetPerTileBuffer)

		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SplatIndexBuffer)
		SHADER_PARAMETER(FVector3f, FalloffParameters)

		// Output
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWColorTexture)
	END_SHADER_PARAMETER_STRUCT()

	static int32 GetThreadGroupSize2D() { return 8; }

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return PlatformSupportsSoftwareRasterization(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}
};

IMPLEMENT_GLOBAL_SHADER(FTileRasterizeGaussianSplatCS, "/Plugin/GaussianSplat/Private/GaussianSplatTileRasterize.usf", "MainCS", SF_Compute);

class FGatherShadedGaussianSplatCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGatherShadedGaussianSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FGatherShadedGaussianSplatCS, FGlobalShader);

	class FGpuDrivenPipeline : SHADER_PERMUTATION_BOOL("GAUSSIAN_SPLAT_GPU_DRIVEN_PIPELINE");
	class FVertexAttributeFetch : SHADER_PERMUTATION_BOOL("GAUSSIAN_SPLAT_VERTEX_ATTRIBUTE_FETCH");
	using FPermutationDomain = TShaderPermutationDomain<FGpuDrivenPipeline, FVertexAttributeFetch>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		SHADER_PARAMETER(uint32, NumSplats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FShadedGaussianSplatData>, ShadedGaussianSplatBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SortedIndexBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SplatCountBuffer)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FPackedShadedGaussianSplatData>, RWGatheredShadedGaussianSplatBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWGatheredShadedGaussianSplatVertexBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static int32 GetThreadGroupSize1D() { return 64; }

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
	}
};

IMPLEMENT_GLOBAL_SHADER(FGatherShadedGaussianSplatCS, "/Plugin/GaussianSplat/Private/GaussianSplatGatherShadedSplats.usf", "MainCS", SF_Compute);

class FHardwareRasterizeGaussianSplatVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHardwareRasterizeGaussianSplatVS);
	SHADER_USE_PARAMETER_STRUCT(FHardwareRasterizeGaussianSplatVS, FGlobalShader);

	class FGatherShadedSplats : SHADER_PERMUTATION_BOOL("GAUSSIAN_SPLAT_GATHER_SHADED_SPLATS");
	class FVertexAttributeFetch : SHADER_PERMUTATION_BOOL("GAUSSIAN_SPLAT_VERTEX_ATTRIBUTE_FETCH");
	class FDeferred : SHADER_PERMUTATION_BOOL("GAUSSIAN_SPLAT_DEFERRED");
	using FPermutationDomain = TShaderPermutationDomain<FGatherShadedSplats, FVertexAttributeFetch, FDeferred>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FShadedGaussianSplatData>, ShadedGaussianSplatBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SortedIndexBuffer)
		// GBuffer path (FDeferred)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUintVector2>, ShadedSplatSurfaceBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		const FPermutationDomain PermutationVector(Parameters.PermutationId);

		// The GBuffer path reads the sorted records by index
		if (PermutationVector.Get<FDeferred>()
			&& (PermutationVector.Get<FGatherShadedSplats>() || PermutationVector.Get<FVertexAttributeFetch>() || !PlatformSupportsDeferredSplats(Parameters.Platform)))
		{
			return false;
		}

		if (PermutationVector.Get<FVertexAttributeFetch>() && !PermutationVector.Get<FGatherShadedSplats>())
		{
			return false;
		}

		if (!FDataDrivenShaderPlatformInfo::GetSupportsVertexShaderSRVs(Parameters.Platform)
			&& !PermutationVector.Get<FVertexAttributeFetch>())
		{
			return false;
		}

		return true;
	}
};

class FHardwareRasterizeGaussianSplatPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHardwareRasterizeGaussianSplatPS);
	SHADER_USE_PARAMETER_STRUCT(FHardwareRasterizeGaussianSplatPS, FGlobalShader);

	class FWriteStochasticDepth : SHADER_PERMUTATION_BOOL("WRITE_STOCHASTIC_DEPTH");
	class FWriteVelocity : SHADER_PERMUTATION_BOOL("WRITE_VELOCITY");
	class FUseBlueNoise : SHADER_PERMUTATION_BOOL("USE_BLUE_NOISE");
	class FWriteExpectedDepth : SHADER_PERMUTATION_BOOL("WRITE_EXPECTED_DEPTH");
	class FSuperSample : SHADER_PERMUTATION_BOOL("GAUSSIAN_SPLAT_SUPERSAMPLE");
	using FPermutationDomain = TShaderPermutationDomain<FWriteStochasticDepth, FWriteVelocity, FUseBlueNoise, FWriteExpectedDepth, FSuperSample>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_REF(FBlueNoise, BlueNoise)
		SHADER_PARAMETER(float, OpacityThreshold)
		SHADER_PARAMETER(float, ExpectedDepthScale)
		SHADER_PARAMETER(FVector3f, FalloffParameters)
		SHADER_PARAMETER(int32, SuperSampleGrid)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		// Stochastic depth needs per-render-target blend state
		if (PermutationVector.Get<FWriteStochasticDepth>() && !PlatformSupportsStochasticDepth(Parameters.Platform))
		{
			return false;
		}
		// Blue Noise is only conditionally used when stochastic depth is also active
		if (!PermutationVector.Get<FWriteStochasticDepth>() && PermutationVector.Get<FUseBlueNoise>())
		{
			return false;
		}
		if (PermutationVector.Get<FWriteStochasticDepth>() && PermutationVector.Get<FWriteVelocity>())
		{
			return false;
		}
		if (PermutationVector.Get<FWriteExpectedDepth>() &&
			(PermutationVector.Get<FWriteStochasticDepth>() || PermutationVector.Get<FWriteVelocity>() || PermutationVector.Get<FUseBlueNoise>()))
		{
			return false;
		}
		return true;
	}
};

BEGIN_SHADER_PARAMETER_STRUCT(FHardwareRasterizeGaussianSplatPassParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FHardwareRasterizeGaussianSplatVS::FParameters, VS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FHardwareRasterizeGaussianSplatPS::FParameters, PS)
	RDG_BUFFER_ACCESS(DrawIndirectArgs, ERHIAccess::IndirectArgs)
	RDG_BUFFER_ACCESS(GatheredSplatVertexBuffer, ERHIAccess::VertexOrIndexBuffer)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

IMPLEMENT_GLOBAL_SHADER(FHardwareRasterizeGaussianSplatVS, "/Plugin/GaussianSplat/Private/GaussianSplatHardwareRasterize.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FHardwareRasterizeGaussianSplatPS, "/Plugin/GaussianSplat/Private/GaussianSplatHardwareRasterize.usf", "MainPS", SF_Pixel);

// GBuffer path (WinterSchool fork): accumulates colour, depth + lighting strength and normal of the splats front to back
class FHardwareRasterizeGaussianSplatDeferredPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHardwareRasterizeGaussianSplatDeferredPS);
	SHADER_USE_PARAMETER_STRUCT(FHardwareRasterizeGaussianSplatDeferredPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER(float, OpacityThreshold)
		SHADER_PARAMETER(float, ExpectedDepthScale)
		SHADER_PARAMETER(FVector3f, FalloffParameters)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return PlatformSupportsDeferredSplats(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("GAUSSIAN_SPLAT_DEFERRED"), 1);
	}
};

BEGIN_SHADER_PARAMETER_STRUCT(FHardwareRasterizeGaussianSplatDeferredPassParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FHardwareRasterizeGaussianSplatVS::FParameters, VS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FHardwareRasterizeGaussianSplatDeferredPS::FParameters, PS)
	RDG_BUFFER_ACCESS(DrawIndirectArgs, ERHIAccess::IndirectArgs)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

IMPLEMENT_GLOBAL_SHADER(FHardwareRasterizeGaussianSplatDeferredPS, "/Plugin/GaussianSplat/Private/GaussianSplatHardwareRasterize.usf", "MainPS_Deferred", SF_Pixel);

// GBuffer path (WinterSchool fork): resolves the accumulated splats into SceneColor, GBufferA-C and depth (+ velocity)
class FGaussianSplatDeferredResolvePS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatDeferredResolvePS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatDeferredResolvePS, FGlobalShader);

	class FWriteVelocity : SHADER_PERMUTATION_BOOL("WRITE_VELOCITY");
	using FPermutationDomain = TShaderPermutationDomain<FWriteVelocity>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, SplatColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, SplatDepthAndLightingTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, SplatNormalTexture)
		SHADER_PARAMETER(float, ExpectedDepthScale)
		SHADER_PARAMETER(float, CoverageThreshold)
		SHADER_PARAMETER(uint32, DitheredCoverage)
		SHADER_PARAMETER(float, GBufferSpecular)
		SHADER_PARAMETER(float, GBufferRoughness)
		SHADER_PARAMETER(float, ShadowStrength)
		SHADER_PARAMETER(float, SurfaceOffset)
		SHADER_PARAMETER(uint32, ShowClipping)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		// Only the desktop renderer has a velocity target this pass can write
		return PlatformSupportsDeferredSplats(Parameters.Platform)
			&& (!PermutationVector.Get<FWriteVelocity>() || !IsMobilePlatform(Parameters.Platform));
	}
};

IMPLEMENT_GLOBAL_SHADER(FGaussianSplatDeferredResolvePS, "/Plugin/GaussianSplat/Private/GaussianSplatDeferred.usf", "MainPS", SF_Pixel);

// GBuffer path: unlit composite of the pixels the resolve does not turn into a splat surface (soft edges, floaters, sky)
class FGaussianSplatDeferredRemainderPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatDeferredRemainderPS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatDeferredRemainderPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, SplatColorTexture)
		SHADER_PARAMETER(float, CoverageThreshold)
		SHADER_PARAMETER(uint32, DitheredCoverage)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return PlatformSupportsDeferredSplats(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FGaussianSplatDeferredRemainderPS, "/Plugin/GaussianSplat/Private/GaussianSplatDeferred.usf", "RemainderPS", SF_Pixel);

// Resolves the stochastic splat-depth target into scene depth (writes SV_Depth).
class FGaussianSplatResolveDepthPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatResolveDepthPS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatResolveDepthPS, FGlobalShader);

	class FWriteVelocity : SHADER_PERMUTATION_BOOL("WRITE_VELOCITY");
	using FPermutationDomain = TShaderPermutationDomain<FWriteVelocity>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, DepthTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return PlatformSupportsStochasticDepth(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FGaussianSplatResolveDepthPS, "/Plugin/GaussianSplat/Private/GaussianSplatHardwareRasterize.usf", "MainPS_ResolveDepth", SF_Pixel);

class FGaussianSplatExpectedDepthPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatExpectedDepthPS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatExpectedDepthPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DepthMomentsTexture)
		SHADER_PARAMETER(float, ExpectedDepthScale)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FGaussianSplatExpectedDepthPS, "/Plugin/GaussianSplat/Private/GaussianSplatHardwareRasterize.usf", "MainPS_ExpectedDepth", SF_Pixel);

class FGaussianSplatResolveVelocityPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatResolveVelocityPS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatResolveVelocityPS, FGlobalShader);

	class FUseBlueNoise : SHADER_PERMUTATION_BOOL("USE_BLUE_NOISE");
	using FPermutationDomain = TShaderPermutationDomain<FUseBlueNoise>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_REF(FBlueNoise, BlueNoise)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DepthMomentsTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FGaussianSplatResolveVelocityPS, "/Plugin/GaussianSplat/Private/GaussianSplatHardwareRasterize.usf", "MainPS_ResolveVelocity", SF_Pixel);

class FGaussianSplatDownsampleDepthPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatDownsampleDepthPS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatDownsampleDepthPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
		SHADER_PARAMETER(FScreenTransform, SvPositionToSourcePixel)
		SHADER_PARAMETER(FIntPoint, SourcePixelMax)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FGaussianSplatDownsampleDepthPS, "/Plugin/GaussianSplat/Private/GaussianSplatDownsample.usf", "MainPS_DownsampleDepth", SF_Pixel);

class FGaussianSplatUpsampleDepthPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatUpsampleDepthPS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatUpsampleDepthPS, FGlobalShader);

	class FWriteVelocity : SHADER_PERMUTATION_BOOL("WRITE_VELOCITY");
	using FPermutationDomain = TShaderPermutationDomain<FWriteVelocity>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SplatDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
		SHADER_PARAMETER(FScreenTransform, SvPositionToDownsampledPixel)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FGaussianSplatUpsampleDepthPS, "/Plugin/GaussianSplat/Private/GaussianSplatDownsample.usf", "MainPS_UpsampleDepth", SF_Pixel);

class FHardwareRasterizeStochasticDepthPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHardwareRasterizeStochasticDepthPS);
	SHADER_USE_PARAMETER_STRUCT(FHardwareRasterizeStochasticDepthPS, FGlobalShader);

	class FUseBlueNoise : SHADER_PERMUTATION_BOOL("USE_BLUE_NOISE");
	class FWriteVelocity : SHADER_PERMUTATION_BOOL("WRITE_VELOCITY");
	using FPermutationDomain = TShaderPermutationDomain<FUseBlueNoise, FWriteVelocity>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_REF(FBlueNoise, BlueNoise)
		SHADER_PARAMETER(FVector3f, FalloffParameters)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return PlatformSupportsStochasticDepth(Parameters.Platform);
	}
};

BEGIN_SHADER_PARAMETER_STRUCT(FHardwareRasterizeStochasticDepthPassParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FHardwareRasterizeGaussianSplatVS::FParameters, VS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FHardwareRasterizeStochasticDepthPS::FParameters, PS)
	RDG_BUFFER_ACCESS(DrawIndirectArgs, ERHIAccess::IndirectArgs)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

IMPLEMENT_GLOBAL_SHADER(FHardwareRasterizeStochasticDepthPS, "/Plugin/GaussianSplat/Private/GaussianSplatHardwareRasterize.usf", "MainPS_StochasticDepth", SF_Pixel);

FIntPoint CalcTileDimension(const FIntRect& ViewRect)
{
	FIntPoint TileDimension = FIntPoint(
		FMath::DivideAndRoundUp(ViewRect.Width(), GetTileSize()),
		FMath::DivideAndRoundUp(ViewRect.Height(), GetTileSize())
	);
	return TileDimension;
}

uint32 CalcTiles(const FIntRect& ViewRect)
{
	const int32 TilesX = FMath::DivideAndRoundUp(ViewRect.Width(), GetTileSize());
	const int32 TilesY = FMath::DivideAndRoundUp(ViewRect.Height(), GetTileSize());
	const uint32 NumTiles = TilesX * TilesY;
	return NumTiles;
}

// Indirect draw arguments (one quad instance per visible splat) from the splat counter of the shading pass
static FRDGBufferRef AddDrawIndirectArgsPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	uint32 NumSplats,
	FRDGBufferRef SplatCountBuffer,
	uint32 NumClusters,
	FRDGBufferRef ClusterCountBuffer,
	GaussianSplat::FRenderStats& RenderStats)
{
	FRDGBufferRef DrawIndirectArgs = nullptr;
	{
		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetShaderPlatform());

		DrawIndirectArgs = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateIndirectDesc<FRHIDrawIndirectParameters>(1),
			TEXT("GaussianSplat.DrawIndirectArgs")
		);

		FGaussianSplatDrawIndirectArgsCS::FParameters* Parameters = GraphBuilder.AllocParameters<FGaussianSplatDrawIndirectArgsCS::FParameters>();
		Parameters->SplatCountBuffer = GraphBuilder.CreateSRV(SplatCountBuffer, PF_R32_UINT);
		Parameters->RWDrawIndirectArgs = GraphBuilder.CreateUAV(DrawIndirectArgs, PF_R32_UINT);
		RenderStats.SetShaderParameters(GraphBuilder, NumClusters, NumSplats, ClusterCountBuffer, Parameters->Stats);

		TShaderMapRef<FGaussianSplatDrawIndirectArgsCS> SetupCS(ShaderMap);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GaussianSplat.DrawIndirectArgs"),
			SetupCS,
			Parameters,
			FIntVector(1, 1, 1));
	}

	return DrawIndirectArgs;
}

// Sorts the shaded splats by their keys (radix sort, indirect on the GPU-driven pipeline). Returns the buffer holding the
// sorted splat indices: SplatIndexBuffer or the scratch buffer, depending on the number of passes.
static FRDGBufferRef AddSortPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	uint32 NumSplats,
	FRDGBufferRef SplatCountBuffer,
	FRDGBufferRef SortKeyBuffer,
	FRDGBufferRef SplatIndexBuffer)
{
	const bool bGpuDrivenPipeline = (SplatCountBuffer != nullptr);
	const uint32 SortElementCount = NumSplats;

	// Back-to-front sort.
	const FRDGBufferDesc ScratchDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), SortElementCount);
	// GPU-driven pipeline does not require secondary sort buffers to be initialized, so mark as external to avoid RDG errors
	const bool bExternalScratch = bGpuDrivenPipeline;

	FRDGBufferRef GPUSortKeysBuffer[] = {
		SortKeyBuffer,
		bExternalScratch
			? GraphBuilder.RegisterExternalBuffer(AllocatePooledBuffer(ScratchDesc, TEXT("GaussianSplat.SortKeys1")))
			: GraphBuilder.CreateBuffer(ScratchDesc, TEXT("GaussianSplat.SortKeys1")),
	};
	FRDGBufferRef GPUSortValuesBuffer[] = {
		SplatIndexBuffer,
		bExternalScratch
			? GraphBuilder.RegisterExternalBuffer(AllocatePooledBuffer(ScratchDesc, TEXT("GaussianSplat.SortValues1")))
			: GraphBuilder.CreateBuffer(ScratchDesc, TEXT("GaussianSplat.SortValues1")),
	};

	if (!bExternalScratch)
	{
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(GPUSortKeysBuffer[1], PF_R32_UINT), 0);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(GPUSortValuesBuffer[1], PF_R32_UINT), 0);
	}

	const uint32 KeyMask = GetSortKeyMask(View.GetShaderPlatform());
	const int32 SortedBufferIndex = GetGPUSortPassCount(KeyMask) % 2;

	{
		FGPUSortPassParameters* PassParameters = GraphBuilder.AllocParameters<FGPUSortPassParameters>();
		PassParameters->RemoteKeySRVs0 = GraphBuilder.CreateSRV(GPUSortKeysBuffer[0], PF_R32_UINT);
		PassParameters->RemoteKeySRVs1 = GraphBuilder.CreateSRV(GPUSortKeysBuffer[1], PF_R32_UINT);
		PassParameters->RemoteKeyUAVs0 = GraphBuilder.CreateUAV(GPUSortKeysBuffer[0], PF_R32_UINT);
		PassParameters->RemoteKeyUAVs1 = GraphBuilder.CreateUAV(GPUSortKeysBuffer[1], PF_R32_UINT);
		PassParameters->RemoteValueSRVs0 = GraphBuilder.CreateSRV(GPUSortValuesBuffer[0], PF_R32_UINT);
		PassParameters->RemoteValueSRVs1 = GraphBuilder.CreateSRV(GPUSortValuesBuffer[1], PF_R32_UINT);
		PassParameters->RemoteValueUAVs0 = GraphBuilder.CreateUAV(GPUSortValuesBuffer[0], PF_R32_UINT);
		PassParameters->RemoteValueUAVs1 = GraphBuilder.CreateUAV(GPUSortValuesBuffer[1], PF_R32_UINT);
		PassParameters->SplatCountSRV = SplatCountBuffer ? GraphBuilder.CreateSRV(SplatCountBuffer, PF_R32_UINT) : nullptr;

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GaussianSplat.SortGPU%s", SplatCountBuffer ? TEXT(".Indirect") : TEXT("")),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, bGpuDrivenPipeline, SortElementCount, KeyMask, SortedBufferIndex, FeatureLevel = View.GetFeatureLevel()]
			(FRDGAsyncTask, FRHICommandList& RHICmdList)
			{
				FGPUSortBuffers SortBuffers;
				SortBuffers.RemoteKeySRVs[0] = PassParameters->RemoteKeySRVs0->GetRHI();
				SortBuffers.RemoteKeySRVs[1] = PassParameters->RemoteKeySRVs1->GetRHI();
				SortBuffers.RemoteKeyUAVs[0] = PassParameters->RemoteKeyUAVs0->GetRHI();
				SortBuffers.RemoteKeyUAVs[1] = PassParameters->RemoteKeyUAVs1->GetRHI();
				SortBuffers.RemoteValueSRVs[0] = PassParameters->RemoteValueSRVs0->GetRHI();
				SortBuffers.RemoteValueSRVs[1] = PassParameters->RemoteValueSRVs1->GetRHI();
				SortBuffers.RemoteValueUAVs[0] = PassParameters->RemoteValueUAVs0->GetRHI();
				SortBuffers.RemoteValueUAVs[1] = PassParameters->RemoteValueUAVs1->GetRHI();

				const FRHITransitionInfo SortBufferToUAV[] = {
					FRHITransitionInfo(SortBuffers.RemoteKeyUAVs[0],   ERHIAccess::Unknown, ERHIAccess::UAVCompute),
					FRHITransitionInfo(SortBuffers.RemoteKeyUAVs[1],   ERHIAccess::Unknown, ERHIAccess::UAVCompute),
					FRHITransitionInfo(SortBuffers.RemoteValueUAVs[0], ERHIAccess::Unknown, ERHIAccess::UAVCompute),
					FRHITransitionInfo(SortBuffers.RemoteValueUAVs[1], ERHIAccess::Unknown, ERHIAccess::UAVCompute),
				};
				RHICmdList.Transition(MakeArrayView(SortBufferToUAV));

				const int32 ResultIndex = bGpuDrivenPipeline
					? SortGPUBuffersIndirect(RHICmdList, SortBuffers, 0, KeyMask, PassParameters->SplatCountSRV->GetRHI(), FeatureLevel)
					: SortGPUBuffers(RHICmdList, SortBuffers, 0, KeyMask, SortElementCount, FeatureLevel);
				check(ResultIndex == SortedBufferIndex);
				RHICmdList.Transition(MakeArrayView(SortBufferToUAV));
			});
	}

	return GPUSortValuesBuffer[SortedBufferIndex];
}

void RasterizeHardware(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	TUniformBufferRef<FViewUniformShaderParameters> ViewUniformBuffer,
	int32 DownsampleFactor,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneVelocityTexture,
	uint32 NumSplats,
	FRDGBufferRef ShadedGaussianSplatBuffer,
	const FScreenPassRenderTarget& Output,
	FRDGBufferRef SplatCountBuffer,
	FRDGBufferRef SortKeyBuffer,
	FRDGBufferRef SplatIndexBuffer,
	uint32 NumClusters,
	FRDGBufferRef ClusterCountBuffer,
	GaussianSplat::FRenderStats& RenderStats,
	ERenderTargetLoadAction ColorLoadAction = ERenderTargetLoadAction::ELoad
)
{
	const bool bGpuDrivenPipeline = (SplatCountBuffer != nullptr);

	const EGaussianSplatDepthMode DepthMode = GetDepthMode(View.GetShaderPlatform());
	FRDGBufferRef DrawIndirectArgs = bGpuDrivenPipeline
		? AddDrawIndirectArgsPass(GraphBuilder, View, NumSplats, SplatCountBuffer, NumClusters, ClusterCountBuffer, RenderStats)
		: nullptr;
	FRDGBufferRef SortedIndexBuffer = AddSortPass(GraphBuilder, View, NumSplats, SplatCountBuffer, SortKeyBuffer, SplatIndexBuffer);

	const bool bVertexAttributeFetch = IsVertexAttributeFetchEnabled(View.GetShaderPlatform());
	const bool bGatherShadedSplats = bVertexAttributeFetch || IsGatherShadedSplatsEnabled(View.GetShaderPlatform());
	if (bGatherShadedSplats)
	{
		FRDGBufferRef GatheredShadedGaussianSplatBuffer = GraphBuilder.CreateBuffer(
			bVertexAttributeFetch
				? FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats * GaussianSplatPackedRecordUInt32Count)
				: FRDGBufferDesc::CreateStructuredDesc(sizeof(FPackedShadedGaussianSplatData), NumSplats),
			TEXT("GaussianSplat.GatheredShadedBuffer")
		);

		FGatherShadedGaussianSplatCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FGatherShadedGaussianSplatCS::FGpuDrivenPipeline>(bGpuDrivenPipeline);
		PermutationVector.Set<FGatherShadedGaussianSplatCS::FVertexAttributeFetch>(bVertexAttributeFetch);
		TShaderMapRef<FGatherShadedGaussianSplatCS> ComputeShader(GetGlobalShaderMap(View.GetShaderPlatform()), PermutationVector);

		FGatherShadedGaussianSplatCS::FParameters* PassParameters =
			GraphBuilder.AllocParameters<FGatherShadedGaussianSplatCS::FParameters>();
		PassParameters->View = ViewUniformBuffer;
		PassParameters->NumSplats = NumSplats;
		PassParameters->ShadedGaussianSplatBuffer = GraphBuilder.CreateSRV(ShadedGaussianSplatBuffer);
		PassParameters->SortedIndexBuffer = GraphBuilder.CreateSRV(SortedIndexBuffer, PF_R32_UINT);
		PassParameters->SplatCountBuffer = bGpuDrivenPipeline ? GraphBuilder.CreateSRV(SplatCountBuffer, PF_R32_UINT) : nullptr;
		if (bVertexAttributeFetch)
		{
			PassParameters->RWGatheredShadedGaussianSplatVertexBuffer = GraphBuilder.CreateUAV(GatheredShadedGaussianSplatBuffer, PF_R32_UINT);
		}
		else
		{
			PassParameters->RWGatheredShadedGaussianSplatBuffer = GraphBuilder.CreateUAV(GatheredShadedGaussianSplatBuffer);
		}

		const FIntVector GroupCount = FComputeShaderUtils::GetGroupCountWrapped(NumSplats, FGatherShadedGaussianSplatCS::GetThreadGroupSize1D());
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GaussianSplat.GatherShadedSplats"),
			ERDGPassFlags::Compute,
			ComputeShader,
			PassParameters,
			GroupCount);
		ShadedGaussianSplatBuffer = GatheredShadedGaussianSplatBuffer;
	}

	const bool bVelocity = IsVelocityEnabled()
		&& DepthMode != EGaussianSplatDepthMode::Off
		&& SceneVelocityTexture != nullptr
		&& EnumHasAnyFlags(SceneVelocityTexture->Desc.Flags, TexCreate_RenderTargetable);

	const bool bHardwareDepth = (DepthMode == EGaussianSplatDepthMode::Hardware);
	const bool bStochasticDepth = (DepthMode == EGaussianSplatDepthMode::Stochastic);
	const bool bExpectedDepth = (DepthMode == EGaussianSplatDepthMode::Expected);

	FRDGTextureRef DepthTexture = nullptr;
	if (DepthMode == EGaussianSplatDepthMode::Stochastic)
	{
		const FRDGTextureDesc SplatDepthDesc = FRDGTextureDesc::Create2D(
			Output.Texture->Desc.Extent,
			GetStochasticDepthFormat(),
			FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_RenderTargetable);
		DepthTexture = GraphBuilder.CreateTexture(SplatDepthDesc, TEXT("GaussianSplat.StochasticDepth"));
	}

	FRDGTextureRef DepthMomentsTexture = nullptr;
	if (bExpectedDepth)
	{
		const FRDGTextureDesc MomentsDesc = FRDGTextureDesc::Create2D(
			Output.Texture->Desc.Extent,
			PF_FloatRGBA,
			FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_RenderTargetable);
		DepthMomentsTexture = GraphBuilder.CreateTexture(MomentsDesc, TEXT("GaussianSplat.DepthMoments"));
	}

	// Rasterization
	{
		FHardwareRasterizeGaussianSplatVS::FPermutationDomain VSPermutationVector;
		VSPermutationVector.Set<FHardwareRasterizeGaussianSplatVS::FGatherShadedSplats>(bGatherShadedSplats);
		VSPermutationVector.Set<FHardwareRasterizeGaussianSplatVS::FVertexAttributeFetch>(bVertexAttributeFetch);
		TShaderMapRef<FHardwareRasterizeGaussianSplatVS> VertexShader(GetGlobalShaderMap(View.GetShaderPlatform()), VSPermutationVector);

		FHardwareRasterizeGaussianSplatPS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FHardwareRasterizeGaussianSplatPS::FWriteStochasticDepth>(bStochasticDepth);
		PermutationVector.Set<FHardwareRasterizeGaussianSplatPS::FWriteVelocity>(bVelocity && bHardwareDepth);
		PermutationVector.Set<FHardwareRasterizeGaussianSplatPS::FUseBlueNoise>(bStochasticDepth && IsBlueNoiseEnabled());
		PermutationVector.Set<FHardwareRasterizeGaussianSplatPS::FWriteExpectedDepth>(bExpectedDepth);
		PermutationVector.Set<FHardwareRasterizeGaussianSplatPS::FSuperSample>(DownsampleFactor > 1);
		TShaderMapRef<FHardwareRasterizeGaussianSplatPS> PixelShader(GetGlobalShaderMap(View.GetShaderPlatform()), PermutationVector);

		FHardwareRasterizeGaussianSplatPassParameters* PassParameters = GraphBuilder.AllocParameters<FHardwareRasterizeGaussianSplatPassParameters>();
		{
			PassParameters->VS.View = ViewUniformBuffer;
			PassParameters->VS.ShadedGaussianSplatBuffer = bVertexAttributeFetch
				? nullptr
				: GraphBuilder.CreateSRV(ShadedGaussianSplatBuffer);
			PassParameters->VS.SortedIndexBuffer = bGatherShadedSplats
				? nullptr
				: GraphBuilder.CreateSRV(SortedIndexBuffer, PF_R32_UINT);
			PassParameters->GatheredSplatVertexBuffer = bVertexAttributeFetch ? ShadedGaussianSplatBuffer : nullptr;
			PassParameters->PS.View = ViewUniformBuffer;
			PassParameters->PS.BlueNoise = CreateUniformBufferImmediate(GetBlueNoiseGlobalParameters(), EUniformBufferUsage::UniformBuffer_SingleDraw);
			PassParameters->PS.OpacityThreshold = GetHardwareRasterizationOpacityThreshold();
			PassParameters->PS.ExpectedDepthScale = GetExpectedDepthScale();
			PassParameters->PS.FalloffParameters = GetFalloffParameters();
			PassParameters->PS.SuperSampleGrid = DownsampleFactor;
			PassParameters->RenderTargets[0] = FRenderTargetBinding(Output.Texture, ColorLoadAction);
			if (bStochasticDepth)
			{
				PassParameters->RenderTargets[1] = FRenderTargetBinding(DepthTexture, ERenderTargetLoadAction::EClear);
			}
			if (bVelocity && bHardwareDepth)
			{
				PassParameters->RenderTargets[1] = FRenderTargetBinding(SceneVelocityTexture, ERenderTargetLoadAction::ELoad);
			}
			if (bExpectedDepth)
			{
				PassParameters->RenderTargets[1] = FRenderTargetBinding(DepthMomentsTexture, ERenderTargetLoadAction::EClear);
			}
			const FExclusiveDepthStencil::Type DepthStencilAccess = bHardwareDepth
				? FExclusiveDepthStencil::DepthWrite_StencilNop
				: FExclusiveDepthStencil::DepthRead_StencilNop;
			PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, DepthStencilAccess);

			// Optional GPU-Driven Pipeline binding
			PassParameters->DrawIndirectArgs = DrawIndirectArgs;
		}

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GaussianSplat.HardwareRasterize"),
			PassParameters,
			ERDGPassFlags::Raster,
			[PassParameters, bGpuDrivenPipeline, bStochasticDepth, bHardwareDepth, bExpectedDepth, bVertexAttributeFetch, NumSplats, VertexShader, PixelShader, ViewRect = Output.ViewRect](FRDGAsyncTask, FRHICommandList& RHICmdList)
			{
				FGraphicsPipelineStateInitializer GraphicsPSOInit;
				RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

				// RT0: premultiplied alpha OVER
				// RT1: stochastic depth
				if (bStochasticDepth)
				{
					GraphicsPSOInit.BlendState = TStaticBlendState<
						CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_Zero, BF_InverseSourceAlpha,
						CW_RED,  BO_Max, BF_One, BF_One, BO_Max, BF_One, BF_One
					>::GetRHI();
				}
				else if (bExpectedDepth)
				{
					// UNDER operator:
					// rgb += src * DestAlpha
					// a = (1 - srcAlpha) * DestAlpha
					GraphicsPSOInit.BlendState = TStaticBlendState<
						CW_RGBA, BO_Add, BF_DestAlpha, BF_One, BO_Add, BF_Zero, BF_InverseSourceAlpha,
						CW_RGBA, BO_Add, BF_DestAlpha, BF_One, BO_Add, BF_Zero, BF_InverseSourceAlpha
					>::GetRHI();
				}
				else
				{
					GraphicsPSOInit.BlendState = TStaticBlendState<
						CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_Zero, BF_InverseSourceAlpha
					>::GetRHI();
				}
				GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
				GraphicsPSOInit.DepthStencilState = bHardwareDepth
					? TStaticDepthStencilState<true, CF_GreaterEqual>::GetRHI()
					: TStaticDepthStencilState<false, CF_GreaterEqual>::GetRHI();
				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = bVertexAttributeFetch
					? GGaussianSplatInstanceVertexDeclaration.VertexDeclarationRHI
					: GEmptyVertexDeclaration.VertexDeclarationRHI;
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;
				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

				RHICmdList.SetViewport(ViewRect.Min.X, ViewRect.Min.Y, 0.0f, ViewRect.Max.X, ViewRect.Max.Y, 1.0f);

				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), PassParameters->VS);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParameters->PS);

				if (bVertexAttributeFetch)
				{
					PassParameters->GatheredSplatVertexBuffer->MarkResourceAsUsed();
					RHICmdList.SetStreamSource(0, PassParameters->GatheredSplatVertexBuffer->GetRHI(), 0);
				}

				if (PassParameters->DrawIndirectArgs != nullptr)
				{
					PassParameters->DrawIndirectArgs->MarkResourceAsUsed();
					RHICmdList.DrawPrimitiveIndirect(PassParameters->DrawIndirectArgs->GetIndirectRHICallBuffer(), 0);
				}
				else
				{
					// 2 triangles (triangle strip, 4 vertices) per splat instance
					RHICmdList.DrawPrimitive(0, 2, NumSplats);
				}
			}
		);
	}

	if (bStochasticDepth)
	{
		FGaussianSplatResolveDepthPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianSplatResolveDepthPS::FParameters>();
		PassParameters->DepthTexture = DepthTexture;
		if (bVelocity)
		{
			PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneVelocityTexture, ERenderTargetLoadAction::ELoad);
		}
		PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);

		FGaussianSplatResolveDepthPS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FGaussianSplatResolveDepthPS::FWriteVelocity>(bVelocity);
		TShaderMapRef<FGaussianSplatResolveDepthPS> PixelShader(GetGlobalShaderMap(View.GetShaderPlatform()), PermutationVector);
		FRHIBlendState* BlendState = nullptr;
		FRHIRasterizerState* RasterizerState = nullptr;
		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder,
			GetGlobalShaderMap(View.GetShaderPlatform()),
			RDG_EVENT_NAME("GaussianSplat.ResolveDepth"),
			PixelShader,
			PassParameters,
			Output.ViewRect,
			BlendState,
			RasterizerState,
			TStaticDepthStencilState<true, CF_GreaterEqual>::GetRHI());
	}
	else if (bExpectedDepth)
	{
		FGaussianSplatExpectedDepthPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianSplatExpectedDepthPS::FParameters>();
		PassParameters->View = ViewUniformBuffer;
		PassParameters->DepthMomentsTexture = DepthMomentsTexture;
		PassParameters->ExpectedDepthScale = GetExpectedDepthScale();
		PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);

		TShaderMapRef<FGaussianSplatExpectedDepthPS> PixelShader(GetGlobalShaderMap(View.GetShaderPlatform()));
		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder,
			GetGlobalShaderMap(View.GetShaderPlatform()),
			RDG_EVENT_NAME("GaussianSplat.ExpectedDepth"),
			PixelShader,
			PassParameters,
			Output.ViewRect,
			nullptr,
			nullptr,
			TStaticDepthStencilState<true, CF_GreaterEqual>::GetRHI());

		if (bVelocity)
		{
			FGaussianSplatResolveVelocityPS::FParameters* VelocityParams = GraphBuilder.AllocParameters<FGaussianSplatResolveVelocityPS::FParameters>();
			VelocityParams->View = ViewUniformBuffer;
			VelocityParams->BlueNoise = CreateUniformBufferImmediate(GetBlueNoiseGlobalParameters(), EUniformBufferUsage::UniformBuffer_SingleDraw);
			VelocityParams->DepthMomentsTexture = DepthMomentsTexture;
			VelocityParams->RenderTargets[0] = FRenderTargetBinding(SceneVelocityTexture, ERenderTargetLoadAction::ELoad);

			FGaussianSplatResolveVelocityPS::FPermutationDomain VelocityPermutationVector;
			VelocityPermutationVector.Set<FGaussianSplatResolveVelocityPS::FUseBlueNoise>(IsBlueNoiseEnabled());
			TShaderMapRef<FGaussianSplatResolveVelocityPS> VelocityShader(GetGlobalShaderMap(View.GetShaderPlatform()), VelocityPermutationVector);
			FPixelShaderUtils::AddFullscreenPass(
				GraphBuilder,
				GetGlobalShaderMap(View.GetShaderPlatform()),
				RDG_EVENT_NAME("GaussianSplat.ResolveVelocity"),
				VelocityShader,
				VelocityParams,
				Output.ViewRect);
		}
	}
#if 0
	// TODO: Consider re-enabling two-pass approach for animated splats
	if VelocityMode == 2
	{
		TShaderMapRef<FHardwareRasterizeGaussianSplatVS> VertexShader(GetGlobalShaderMap(View.GetShaderPlatform()));

		FHardwareRasterizeStochasticDepthPS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FHardwareRasterizeStochasticDepthPS::FUseBlueNoise>(IsBlueNoiseEnabled());
		PermutationVector.Set<FHardwareRasterizeStochasticDepthPS::FWriteVelocity>(bVelocity);
		TShaderMapRef<FHardwareRasterizeStochasticDepthPS> PixelShader(GetGlobalShaderMap(View.GetShaderPlatform()), PermutationVector);

		FHardwareRasterizeStochasticDepthPassParameters* PassParameters = GraphBuilder.AllocParameters<FHardwareRasterizeStochasticDepthPassParameters>();
		{
			PassParameters->VS.View = ViewUniformBuffer;
			PassParameters->VS.ShadedGaussianSplatBuffer = GraphBuilder.CreateSRV(ShadedGaussianSplatBuffer);
			PassParameters->VS.SortedIndexBuffer = GraphBuilder.CreateSRV(SortedIndexBuffer, PF_R32_UINT);
			PassParameters->PS.View = ViewUniformBuffer;
			PassParameters->PS.BlueNoise = CreateUniformBufferImmediate(GetBlueNoiseGlobalParameters(), EUniformBufferUsage::UniformBuffer_SingleDraw);
			PassParameters->PS.FalloffParameters = GetFalloffParameters();
			if (bVelocity)
			{
				PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneVelocityTexture, ERenderTargetLoadAction::ELoad);
			}
			PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);
			PassParameters->DrawIndirectArgs = DrawIndirectArgs;
		}

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GaussianSplat.StochasticDepth"),
			PassParameters,
			ERDGPassFlags::Raster,
			[PassParameters, NumSplats, VertexShader, PixelShader, ViewRect = Output.ViewRect](FRDGAsyncTask, FRHICommandList& RHICmdList)
			{
				FGraphicsPipelineStateInitializer GraphicsPSOInit;
				RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

				GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
				GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
				GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_GreaterEqual>::GetRHI();
				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;
				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

				RHICmdList.SetViewport(ViewRect.Min.X, ViewRect.Min.Y, 0.0f, ViewRect.Max.X, ViewRect.Max.Y, 1.0f);

				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), PassParameters->VS);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParameters->PS);

				if (PassParameters->DrawIndirectArgs != nullptr)
				{
					PassParameters->DrawIndirectArgs->MarkResourceAsUsed();
					RHICmdList.DrawPrimitiveIndirect(PassParameters->DrawIndirectArgs->GetIndirectRHICallBuffer(), 0);
				}
				else
				{
					RHICmdList.DrawPrimitive(0, 2, NumSplats);
				}
			}
		);
	}
#endif
}

void RasterizeTiledCompute(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	TUniformBufferRef<FViewUniformShaderParameters> ViewUniformBuffer,
	uint32 NumSplats,
	FRDGBufferRef ShadedGaussianSplatBuffer,
	const FScreenPassRenderTarget& Output
)
{
	FIntPoint TileDimension = CalcTileDimension(Output.ViewRect);
	uint32 NumTiles = TileDimension.X * TileDimension.Y;

	FRDGBufferRef TileCountBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumTiles),
		TEXT("GaussianSplat.TileCount")
	);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(TileCountBuffer, PF_R32_UINT), 0);

	FRDGBufferRef MinDepthPerTileBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumTiles),
		TEXT("GaussianSplat.MinDepthPerTile")
	);
	const uint32 asuint_FLOAT_MAX = 0x7F7FFFFF;
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(MinDepthPerTileBuffer, PF_R32_UINT), asuint_FLOAT_MAX);

	FRDGBufferRef MaxDepthPerTileBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumTiles),
		TEXT("GaussianSplat.MaxDepthPerTile")
	);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(MaxDepthPerTileBuffer, PF_R32_UINT), 0);

	FRDGBufferRef TileDebugBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FShadedGaussianSplatData), NumSplats),
		TEXT("GaussianSplat.TileDebugBuffer")
	);

	{
		FTileCountGaussianSplatCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FTileCountGaussianSplatCS::FShaderDebug>(IsDebug());
		TShaderMapRef<FTileCountGaussianSplatCS> ComputeShader(GetGlobalShaderMap(View.GetShaderPlatform()), PermutationVector);

		FTileCountGaussianSplatCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FTileCountGaussianSplatCS::FParameters>();
		{
			PassParameters->View = ViewUniformBuffer;
			PassParameters->NumSplats = NumSplats;
			PassParameters->ShadedGaussianSplatBuffer = GraphBuilder.CreateSRV(ShadedGaussianSplatBuffer);
			PassParameters->TileSize = GetTileSize();
			PassParameters->TileDimension = TileDimension;
			PassParameters->RWTileCountBuffer = GraphBuilder.CreateUAV(TileCountBuffer, PF_R32_UINT);
			PassParameters->RWMinDepthPerTileBuffer = GraphBuilder.CreateUAV(MinDepthPerTileBuffer, PF_R32_UINT);
			PassParameters->RWMaxDepthPerTileBuffer = GraphBuilder.CreateUAV(MaxDepthPerTileBuffer, PF_R32_UINT);
			PassParameters->RWTileDebugBuffer = GraphBuilder.CreateUAV(TileDebugBuffer);
		}

		FIntVector GroupCount = FComputeShaderUtils::GetGroupCountWrapped(PassParameters->NumSplats, FTileCountGaussianSplatCS::GetThreadGroupSize1D());
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GaussianSplat.TileCount"), ERDGPassFlags::Compute, ComputeShader, PassParameters, GroupCount);
	}

	// Prefix sum
	FRDGBufferRef SplatOffsetPerTileBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumTiles),
		TEXT("GaussianSplat.SplatOffsetPerTileBuffer")
	);

	{
		TShaderMapRef<FPrefixSumGaussianSplatCS> ComputeShader(GetGlobalShaderMap(View.GetShaderPlatform()));
		FPrefixSumGaussianSplatCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FPrefixSumGaussianSplatCS::FParameters>();
		{
			PassParameters->NumItems = NumTiles;
			PassParameters->ItemBuffer = GraphBuilder.CreateSRV(TileCountBuffer, PF_R32_UINT);
			PassParameters->RWPrefixSumBuffer = GraphBuilder.CreateUAV(SplatOffsetPerTileBuffer, PF_R32_UINT);
		}

		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GaussianSplat.PrefixSum"), ERDGPassFlags::Compute, ComputeShader, PassParameters, FIntVector(1, 1, 1));
	}

	// Allocate and fill sort buffers
	int32 MaxSplatOverlap = GetMaxSplatOverlap();
	FRDGBufferRef SortKeyBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats * MaxSplatOverlap),
		TEXT("GaussianSplat.SortKeyBuffer")
	);
	FRDGBufferRef SplatIndexBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats * MaxSplatOverlap),
		TEXT("GaussianSplat.SplatIndexBuffer")
	);
	FRDGBufferRef NumSplatsPerTileBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumTiles),
		TEXT("GaussianSplat.SplatIndexAllocBuffer")
	);
	const uint32 SortElementCount = NumSplats * MaxSplatOverlap;

	{
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SortKeyBuffer, PF_R32_UINT), 0xffffffff);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(NumSplatsPerTileBuffer, PF_R32_UINT), 0);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SplatIndexBuffer, PF_R32_UINT), 0xffffffff);

		TShaderMapRef<FFillSplatIndexListGaussianSplatCS> ComputeShader(GetGlobalShaderMap(View.GetShaderPlatform()));
		FFillSplatIndexListGaussianSplatCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FFillSplatIndexListGaussianSplatCS::FParameters>();
		{
			PassParameters->View = ViewUniformBuffer;
			PassParameters->NumSplats = NumSplats;
			PassParameters->ShadedGaussianSplatBuffer = GraphBuilder.CreateSRV(ShadedGaussianSplatBuffer);
			PassParameters->TileSize = GetTileSize();
			PassParameters->TileDimension = TileDimension;
			PassParameters->SplatOffsetPerTileBuffer = GraphBuilder.CreateSRV(SplatOffsetPerTileBuffer, PF_R32_UINT);
			PassParameters->MinDepthPerTileBuffer = GraphBuilder.CreateSRV(MinDepthPerTileBuffer, PF_R32_UINT);
			PassParameters->MaxDepthPerTileBuffer = GraphBuilder.CreateSRV(MaxDepthPerTileBuffer, PF_R32_UINT);
			PassParameters->RWNumSplatsPerTileBuffer = GraphBuilder.CreateUAV(NumSplatsPerTileBuffer, PF_R32_UINT);
			PassParameters->RWSortKeyBuffer = GraphBuilder.CreateUAV(SortKeyBuffer, PF_R32_UINT);
			PassParameters->RWSplatIndexBuffer = GraphBuilder.CreateUAV(SplatIndexBuffer, PF_R32_UINT);
		}

		FIntVector GroupCount = FComputeShaderUtils::GetGroupCountWrapped(NumSplats, FFillSplatIndexListGaussianSplatCS::GetThreadGroupSize1D());
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GaussianSplat.FillSplatIndexList"), ERDGPassFlags::Compute, ComputeShader, PassParameters, GroupCount);
	}

	// GPU Radix sort
	FRDGBufferRef GPUSortKeysBuffer[] = {
		SortKeyBuffer,
		GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), SortElementCount), TEXT("GaussianSplat.SortKeys1")),
	};
	FRDGBufferRef GPUSortValuesBuffer[] = {
		SplatIndexBuffer,
		GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), SortElementCount), TEXT("GaussianSplat.SortValues1")),
	};
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(GPUSortKeysBuffer[1], PF_R32_UINT), 0);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(GPUSortValuesBuffer[1], PF_R32_UINT), 0);

	const uint32 KeyMask = 0xFFFFFFFF;
	const int32 SortedBufferIndex = GetGPUSortPassCount(KeyMask) % 2;
	{
		FGPUSortPassParameters* PassParameters = GraphBuilder.AllocParameters<FGPUSortPassParameters>();
		{
			PassParameters->RemoteKeySRVs0 = GraphBuilder.CreateSRV(GPUSortKeysBuffer[0], PF_R32_UINT);
			PassParameters->RemoteKeySRVs1 = GraphBuilder.CreateSRV(GPUSortKeysBuffer[1], PF_R32_UINT);
			PassParameters->RemoteKeyUAVs0 = GraphBuilder.CreateUAV(GPUSortKeysBuffer[0], PF_R32_UINT);
			PassParameters->RemoteKeyUAVs1 = GraphBuilder.CreateUAV(GPUSortKeysBuffer[1], PF_R32_UINT);
			PassParameters->RemoteValueSRVs0 = GraphBuilder.CreateSRV(GPUSortValuesBuffer[0], PF_R32_UINT);
			PassParameters->RemoteValueSRVs1 = GraphBuilder.CreateSRV(GPUSortValuesBuffer[1], PF_R32_UINT);
			PassParameters->RemoteValueUAVs0 = GraphBuilder.CreateUAV(GPUSortValuesBuffer[0], PF_R32_UINT);
			PassParameters->RemoteValueUAVs1 = GraphBuilder.CreateUAV(GPUSortValuesBuffer[1], PF_R32_UINT);
		}

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GaussianSplat.SortGPU"),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, SortElementCount, KeyMask, SortedBufferIndex, FeatureLevel = View.GetFeatureLevel()](FRDGAsyncTask, FRHICommandList& RHICmdList)
			{
				FGPUSortBuffers SortBuffers;
				SortBuffers.RemoteKeySRVs[0] = PassParameters->RemoteKeySRVs0->GetRHI();
				SortBuffers.RemoteKeySRVs[1] = PassParameters->RemoteKeySRVs1->GetRHI();
				SortBuffers.RemoteKeyUAVs[0] = PassParameters->RemoteKeyUAVs0->GetRHI();
				SortBuffers.RemoteKeyUAVs[1] = PassParameters->RemoteKeyUAVs1->GetRHI();
				SortBuffers.RemoteValueSRVs[0] = PassParameters->RemoteValueSRVs0->GetRHI();
				SortBuffers.RemoteValueSRVs[1] = PassParameters->RemoteValueSRVs1->GetRHI();
				SortBuffers.RemoteValueUAVs[0] = PassParameters->RemoteValueUAVs0->GetRHI();
				SortBuffers.RemoteValueUAVs[1] = PassParameters->RemoteValueUAVs1->GetRHI();

				int RetVal = SortGPUBuffers(RHICmdList, SortBuffers, 0, KeyMask, SortElementCount, FeatureLevel);
				check(RetVal == SortedBufferIndex);
			}
		);
	}

	// Tile rasterize
	{
		TShaderMapRef<FTileRasterizeGaussianSplatCS> ComputeShader(GetGlobalShaderMap(View.GetShaderPlatform()));
		FTileRasterizeGaussianSplatCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FTileRasterizeGaussianSplatCS::FParameters>();
		{
			PassParameters->View = ViewUniformBuffer;
			PassParameters->NumSplats = NumSplats;
			PassParameters->ShadedGaussianSplatBuffer = GraphBuilder.CreateSRV(ShadedGaussianSplatBuffer);
			PassParameters->TileSize = GetTileSize();
			PassParameters->TileDimension = TileDimension;
			PassParameters->NumSplatsPerTileBuffer = GraphBuilder.CreateSRV(NumSplatsPerTileBuffer, PF_R32_UINT);
			PassParameters->SplatOffsetPerTileBuffer = GraphBuilder.CreateSRV(SplatOffsetPerTileBuffer, PF_R32_UINT);
			PassParameters->SplatIndexBuffer = GraphBuilder.CreateSRV(GPUSortValuesBuffer[SortedBufferIndex], PF_R32_UINT);
			PassParameters->FalloffParameters = GetFalloffParameters();
			PassParameters->RWColorTexture = GraphBuilder.CreateUAV(Output.Texture);
		}

		FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(Output.ViewRect.Size(), FTileRasterizeGaussianSplatCS::GetThreadGroupSize2D());
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GaussianSplat.TileRasterize"), ERDGPassFlags::Compute, ComputeShader, PassParameters, GroupCount);
	}
}

// Where the shade pass of one proxy writes its splats. On the GPU-driven pipeline the shade pass appends the visible
// splats (atomic counter in SplatCountBuffer), so several proxies can share one set of buffers: that is the global sort.
struct FGaussianSplatShadeTargets
{
	FRDGBufferRef ShadedBuffer = nullptr;
	FRDGBufferRef SortKeyBuffer = nullptr;    // HW raster only
	FRDGBufferRef SplatIndexBuffer = nullptr; // HW raster only
	FRDGBufferRef SplatCountBuffer = nullptr; // GPU-driven only
	FRDGBufferRef SurfaceBuffer = nullptr;    // GBuffer path only: per splat normal + lighting strength
	bool bFrontToBack = false;                // sort keys front to back (expected depth, GBuffer path)
};

// How many shaded-splat slots a proxy can need in the worst case
static uint32 GetShadeCapacity(const FGaussianSplatRenderProxy& RenderProxy, bool bClusterDispatch)
{
	return bClusterDispatch ? RenderProxy.NumSplats : RenderProxy.GetNumLOD0Splats();
}

static bool UsesClusterDispatch(const FGaussianSplatRenderProxy& RenderProxy, bool bGpuDrivenPipeline)
{
	return bGpuDrivenPipeline && RenderProxy.HasClusters() && (!IsDebug() || IsClusterCullingEnabled());
}

// Cluster culling + shading of one proxy into Targets. PerSplatLUT != null applies the inverse tone curve per splat.
// Returns the cluster count buffer (null without cluster dispatch) for the stats.
static FRDGBufferRef ShadeProxy(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	TUniformBufferRef<FViewUniformShaderParameters> ViewUniformBuffer,
	int32 DownsampleFactor,
	const FGaussianSplatRenderProxy& RenderProxy,
	FRHITexture* PerSplatLUT,
	const FGaussianSplatShadeTargets& Targets)
{
	const bool bHWRaster = GaussianSplat::IsHardwareRasterization(View.GetShaderPlatform());
	const bool bExpectedDepth = (GetDepthMode(View.GetShaderPlatform()) == EGaussianSplatDepthMode::Expected);
	const bool bDebug = IsDebug();
	const bool bShadeDebug = bDebug || !RenderProxy.HasClusters() || !bHWRaster;
	const bool bGpuDrivenPipeline = Targets.SplatCountBuffer != nullptr;
	const bool bClusterDispatch = UsesClusterDispatch(RenderProxy, bGpuDrivenPipeline);
	const bool bShadeFrustumCull = !bDebug || IsFrustumCullingEnabledForShading();
	const uint32 NumSplats = GetShadeCapacity(RenderProxy, bClusterDispatch);

	FRDGBufferRef GaussianSplatBuffer = GraphBuilder.RegisterExternalBuffer(RenderProxy.GpuBuffer);

	// Cluster culling
	FRDGBufferRef ClusterCountBuffer = nullptr;
	FRDGBufferRef ClusterIndexBuffer = nullptr;
	FRDGBufferRef ClusterBuffer = nullptr;
	if (bClusterDispatch)
	{
		ClusterIndexBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), RenderProxy.NumClusters), TEXT("GaussianSplat.ClusterIndexBuffer"));
		ClusterCountBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("GaussianSplat.ClusterCount"));

		ClusterBuffer = GraphBuilder.RegisterExternalBuffer(RenderProxy.ClusterBuffer);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ClusterCountBuffer, PF_R32_UINT), 0u);

		TShaderMapRef<FCullClustersCS> ComputeShader(GetGlobalShaderMap(View.GetShaderPlatform()));
		FCullClustersCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FCullClustersCS::FParameters>();
		{
			PassParameters->View = ViewUniformBuffer;
			PassParameters->LocalToWorld = RenderProxy.LocalToWorld;
			PassParameters->NumClusters = RenderProxy.NumClusters;
			PassParameters->PointScaleFactor = RenderProxy.PointScaleFactor;
			PassParameters->ClusterBuffer = GraphBuilder.CreateSRV(ClusterBuffer);
			PassParameters->LOD = CVarGaussianSplatLOD.GetValueOnRenderThread();
			PassParameters->ErrorThreshold = CVarGaussianSplatErrorThreshold.GetValueOnRenderThread();
			PassParameters->RWClusterOutputBuffer = GraphBuilder.CreateUAV(ClusterIndexBuffer, PF_R32_UINT);
			PassParameters->RWClusterCount = GraphBuilder.CreateUAV(ClusterCountBuffer, PF_R32_UINT);
		}

		const FIntVector GroupCount = FComputeShaderUtils::GetGroupCountWrapped(RenderProxy.NumClusters, FCullClustersCS::GetThreadGroupSize1D());
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GaussianSplat.CullClusters"), ERDGPassFlags::Compute, ComputeShader, PassParameters, GroupCount);
	}

	// Shade splats
	FRDGBufferRef ShadeIndirectArgs = nullptr;
	if (bClusterDispatch)
	{
		ShadeIndirectArgs = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), TEXT("GaussianSplat.ShadeIndirectArgs"));

		FGaussianSplatShadeIndirectArgsCS::FParameters* Parameters = GraphBuilder.AllocParameters<FGaussianSplatShadeIndirectArgsCS::FParameters>();
		Parameters->ClusterCountBuffer = GraphBuilder.CreateSRV(ClusterCountBuffer, PF_R32_UINT);
		Parameters->DimensionLimit = FUintVector2(GRHIMaxDispatchThreadGroupsPerDimension.X, GRHIMaxDispatchThreadGroupsPerDimension.Y);
		Parameters->RWShadeIndirectArgs = GraphBuilder.CreateUAV(ShadeIndirectArgs, PF_R32_UINT);

		TShaderMapRef<FGaussianSplatShadeIndirectArgsCS> ComputeShader(GetGlobalShaderMap(View.GetShaderPlatform()));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GaussianSplat.ShadeIndirectArgs"), ComputeShader, Parameters, FIntVector(1, 1, 1));
	}

	FShadeGaussianSplatCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FShadeGaussianSplatCS::FShaderDebug>(bShadeDebug);
	PermutationVector.Set<FShadeGaussianSplatCS::FSHDegree>(FMath::Clamp(FMath::Min(RenderProxy.SHDegree, RenderProxy.MaxSHDegree), 0, 3));
	PermutationVector.Set<FShadeGaussianSplatCS::FDeferred>(Targets.SurfaceBuffer != nullptr);
	TShaderMapRef<FShadeGaussianSplatCS> ComputeShader(GetGlobalShaderMap(View.GetShaderPlatform()), PermutationVector);
	FShadeGaussianSplatCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FShadeGaussianSplatCS::FParameters>();

	auto CreateDummyUAV = [&GraphBuilder](const TCHAR* Name)
	{
		return GraphBuilder.CreateUAV(GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), Name), PF_R32_UINT);
	};
	{
		PassParameters->View = ViewUniformBuffer;

		PassParameters->DebugGpuDrivenPipeline = bGpuDrivenPipeline ? 1u : 0u;
		PassParameters->DebugClustering = bClusterDispatch ? 1u : 0u;
		PassParameters->DebugFrustumCull = bShadeFrustumCull ? 1u : 0u;
		PassParameters->DebugWriteSortKeys = bHWRaster ? 1u : 0u;

		PassParameters->LocalToWorld = RenderProxy.LocalToWorld;
		PassParameters->WorldToLocal = RenderProxy.WorldToLocal;

		PassParameters->NumSplats = NumSplats;
		PassParameters->GaussianSplatBuffer = GraphBuilder.CreateSRV(GaussianSplatBuffer);

		if (bClusterDispatch)
		{
			PassParameters->ClusterBuffer = GraphBuilder.CreateSRV(ClusterBuffer);
			PassParameters->ClusterIndexBuffer = GraphBuilder.CreateSRV(ClusterIndexBuffer, PF_R32_UINT);
			PassParameters->ClusterCountBuffer = GraphBuilder.CreateSRV(ClusterCountBuffer, PF_R32_UINT);
			PassParameters->IndirectArgs = ShadeIndirectArgs;
		}
		else
		{
			PassParameters->ClusterBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(FGaussianSplatCluster)));
			PassParameters->ClusterIndexBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultBuffer(GraphBuilder, sizeof(uint32)), PF_R32_UINT);
			PassParameters->ClusterCountBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultBuffer(GraphBuilder, sizeof(uint32)), PF_R32_UINT);
		}

		PassParameters->BufferFormat = RenderProxy.BufferFormat;
		PassParameters->BufferStride = RenderProxy.GpuDataStride;
		PassParameters->PositionScale = RenderProxy.PositionScale;
		PassParameters->PositionBias = RenderProxy.PositionBias;
		PassParameters->ScaleScale = RenderProxy.ScaleScale;
		PassParameters->ScaleBias = RenderProxy.ScaleBias;
		PassParameters->ColorScale = RenderProxy.ColorScale;
		PassParameters->ColorBias = RenderProxy.ColorBias;
		PassParameters->SHScale = RenderProxy.SHScale;
		PassParameters->SHBias = RenderProxy.SHBias;

		PassParameters->SHCoordinateSpace = static_cast<uint32>(RenderProxy.SHCoordinateSpace);

		PassParameters->PointScaleFactor = RenderProxy.PointScaleFactor;
		PassParameters->OpacityScale     = RenderProxy.OpacityScale;

		PassParameters->OpacityThreshold = GetShadingOpacityThreshold();
		PassParameters->CovarianceScale = bHWRaster ? 1.0f / float(DownsampleFactor * DownsampleFactor) : 1.0f;

		const bool bPerSplatLUT = PerSplatLUT != nullptr && RenderProxy.InverseToneCurveStrength > 0.0f;
		PassParameters->InverseToneCurveLUT = bPerSplatLUT ? PerSplatLUT : GBlackVolumeTexture->TextureRHI.GetReference();
		PassParameters->InverseToneCurveLUTSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		PassParameters->InverseToneCurveStrength = bPerSplatLUT ? RenderProxy.InverseToneCurveStrength : 0.0f;
		PassParameters->InverseToneCurveLUTEdge = bPerSplatLUT ? float(PerSplatLUT->GetSizeX()) : 1.0f;

		PassParameters->RWShadedGaussianSplatBuffer = GraphBuilder.CreateUAV(Targets.ShadedBuffer);
		PassParameters->RWSplatCount = Targets.SplatCountBuffer
			? GraphBuilder.CreateUAV(Targets.SplatCountBuffer, PF_R32_UINT)
			: CreateDummyUAV(TEXT("GaussianSplat.SplatCount.Dummy"));

		PassParameters->RWSortKeys   = bHWRaster ? GraphBuilder.CreateUAV(Targets.SortKeyBuffer, PF_R32_UINT)    : CreateDummyUAV(TEXT("GaussianSplat.SortKeyBuffer.Dummy"));
		PassParameters->RWSortValues = bHWRaster ? GraphBuilder.CreateUAV(Targets.SplatIndexBuffer, PF_R32_UINT) : CreateDummyUAV(TEXT("GaussianSplat.SplatIndexBuffer.Dummy"));
		PassParameters->SortKeyFrontToBack = (bExpectedDepth || Targets.bFrontToBack) ? 1u : 0u;

		if (Targets.SurfaceBuffer)
		{
			PassParameters->RWShadedSplatSurfaceBuffer = GraphBuilder.CreateUAV(Targets.SurfaceBuffer);
			// Both only scale the albedo (the sun's share of the captured color), see GaussianSplatDeferred.usf
			PassParameters->LightingStrength = FMath::Clamp(RenderProxy.LightingStrength, 0.0f, 1.0f) * FMath::Clamp(RenderProxy.ShadowStrength, 0.0f, 1.0f);
		}
		PassParameters->SortKeyBits        = GetSortKeyBits(View.GetShaderPlatform());
	}

	if (bClusterDispatch)
	{
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GaussianSplat.Shade(Clusters)"), ERDGPassFlags::Compute, ComputeShader, PassParameters, ShadeIndirectArgs, 0);
	}
	else
	{
		const FIntVector GroupCount = FComputeShaderUtils::GetGroupCountWrapped(PassParameters->NumSplats, FShadeGaussianSplatCS::GetThreadGroupSize1D());
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GaussianSplat.Shade"), ERDGPassFlags::Compute, ComputeShader, PassParameters, GroupCount);
	}

	return ClusterCountBuffer;
}

// Full-screen scratch the splats are rasterised into before compositing (inverse tone curve per component, expected
// depth, downsampling)
static FScreenPassRenderTarget CreateSplatScratch(FRDGBuilder& GraphBuilder, const FScreenPassRenderTarget& Output, int32 DownsampleFactor, bool bHWRaster)
{
	const FScreenPassTextureViewport ScratchViewport = GetSplatViewport(Output, DownsampleFactor);
	ETextureCreateFlags ScratchFlags = TexCreate_ShaderResource | TexCreate_RenderTargetable;
	if (!bHWRaster)
	{
		ScratchFlags |= TexCreate_UAV;
	}
	// The post-blend pass requires transmittance in alpha even when SceneColor uses an RGB-only format
	const FRDGTextureDesc ScratchDesc = FRDGTextureDesc::Create2D(ScratchViewport.Extent, PF_FloatRGBA, FClearValueBinding::Black, ScratchFlags);
	FRDGTextureRef ScratchTexture = GraphBuilder.CreateTexture(ScratchDesc, TEXT("GaussianSplat.SplatScratch"));

	// HW raster consumes the desc's clear binding on first bind (EClear); tiled compute writes via UAV, so clear here
	if (!bHWRaster)
	{
		AddClearRenderTargetPass(GraphBuilder, ScratchTexture);
	}
	return FScreenPassRenderTarget(ScratchTexture, ScratchViewport.Rect, ERenderTargetLoadAction::ELoad);
}

// Composites the scratch onto Output, optionally through the inverse tone curve LUT
static void AddSplatScratchPostBlend(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FScreenPassRenderTarget& Output,
	const FScreenPassRenderTarget& ScratchRT,
	bool bDownsampled,
	FRHITexture* LUTRHI,
	float Strength,
	bool bInvertExpandGamutLUT)
{
	FInverseToneCurvePostBlendPS::FParameters* Params = GraphBuilder.AllocParameters<FInverseToneCurvePostBlendPS::FParameters>();
	Params->RenderTargets[0] = FRenderTargetBinding(Output.Texture, ERenderTargetLoadAction::ELoad);
	Params->View = View.ViewUniformBuffer;
	Params->SplatColor = GetScreenPassTextureInput(
		FScreenPassTexture(ScratchRT.Texture, ScratchRT.ViewRect),
		bDownsampled ? TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI() : TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI());
	Params->SvPositionToInputTextureUV =
		FScreenTransform::ChangeTextureBasisFromTo(FScreenPassTextureViewport(Output), FScreenTransform::ETextureBasis::TexelPosition, FScreenTransform::ETextureBasis::ViewportUV) *
		FScreenTransform::ChangeTextureBasisFromTo(FScreenPassTextureViewport(ScratchRT.Texture, ScratchRT.ViewRect), FScreenTransform::ETextureBasis::ViewportUV, FScreenTransform::ETextureBasis::TextureUV);
	if (LUTRHI)
	{
		Params->InverseToneCurveLUT        = LUTRHI;
		Params->InverseToneCurveLUTSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		Params->Strength                   = Strength;
		Params->InverseToneCurveLUTEdge    = static_cast<float>(LUTRHI->GetSizeX());
	}

	FInverseToneCurvePostBlendPS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FInverseToneCurvePostBlendPS::FApplyInverseToneCurve>(LUTRHI != nullptr);
	TShaderMapRef<FInverseToneCurvePostBlendPS> PixelShader(GetGlobalShaderMap(View.GetShaderPlatform()), PermutationVector);
	FPixelShaderUtils::AddFullscreenPass(
		GraphBuilder,
		GetGlobalShaderMap(View.GetShaderPlatform()),
		RDG_EVENT_NAME("GaussianSplat.InverseToneCurvePostBlend.%s",
			LUTRHI ? (bInvertExpandGamutLUT ? TEXT(".FilmicTC_Full") : TEXT(".FilmicTC")) : TEXT("")),
		PixelShader,
		Params,
		Output.ViewRect,
		// PS outputs (Color, Transmittance): color uses premult alpha-over via SrcAlpha (= transmittance); alpha multiplies
		// transmittances to match UE SceneColor convention.
		TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_SourceAlpha, BO_Add, BF_Zero, BF_SourceAlpha>::GetRHI());
}

// Epic's path: one proxy, its own sort + draw (+ scratch / inverse tone curve post-blend)
static void RenderProxyInternal(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	TUniformBufferRef<FViewUniformShaderParameters> ViewUniformBuffer,
	int32 DownsampleFactor,
	const FGaussianSplatRenderProxy& RenderProxy,
	FRHITexture* ProxyLUTRHI,
	GaussianSplat::FRenderStats& RenderStats,
	const FScreenPassRenderTarget& Output,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneVelocityTexture)
{
	if (RenderProxy.NumSplats == 0)
	{
		return;
	}

	const bool bHWRaster = GaussianSplat::IsHardwareRasterization(View.GetShaderPlatform());
	const bool bProxyUsesInverseTC = ProxyLUTRHI != nullptr && SupportsSplatScratch(bHWRaster);
	const bool bExpectedDepth = (GetDepthMode(View.GetShaderPlatform()) == EGaussianSplatDepthMode::Expected);
	const bool bDownsampled = DownsampleFactor > 1;
	const bool bUsesScratch = bProxyUsesInverseTC || bExpectedDepth || bDownsampled;

	FScreenPassRenderTarget ProxyScratchRT;
	if (bUsesScratch)
	{
		ProxyScratchRT = CreateSplatScratch(GraphBuilder, Output, DownsampleFactor, bHWRaster);
	}
	const FScreenPassRenderTarget& RasterTarget = bUsesScratch ? ProxyScratchRT : Output;

	const bool bGpuDrivenPipeline = bHWRaster && (!IsDebug() || GaussianSplat::IsGPUDrivenPipeline());
	const bool bClusterDispatch = UsesClusterDispatch(RenderProxy, bGpuDrivenPipeline);
	const uint32 NumSplats = GetShadeCapacity(RenderProxy, bClusterDispatch);

	// Shaded splats are always stored in the packed 24B view.z layout (FPackedShadedGaussianSplatData).
	FGaussianSplatShadeTargets Targets;
	Targets.ShadedBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FPackedShadedGaussianSplatData), NumSplats), TEXT("GaussianSplat.ShadedBuffer"));
	if (bHWRaster)
	{
		Targets.SortKeyBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats), TEXT("GaussianSplat.SortKeyBuffer"));
		Targets.SplatIndexBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats), TEXT("GaussianSplat.SplatIndexBuffer"));
	}
	if (bGpuDrivenPipeline)
	{
		Targets.SplatCountBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("GaussianSplat.SplatCount"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Targets.SplatCountBuffer, PF_R32_UINT), 0u);
	}

	FRDGBufferRef ClusterCountBuffer = ShadeProxy(GraphBuilder, View, ViewUniformBuffer, DownsampleFactor, RenderProxy, nullptr, Targets);

	if (bHWRaster)
	{
		const ERenderTargetLoadAction ColorLoadAction = bUsesScratch ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad;
		const uint32 NumClusters = bClusterDispatch ? RenderProxy.NumClusters : 0u;
		RasterizeHardware(GraphBuilder, View, ViewUniformBuffer, DownsampleFactor, SceneDepthTexture, SceneVelocityTexture, NumSplats, Targets.ShadedBuffer, RasterTarget, Targets.SplatCountBuffer,
			Targets.SortKeyBuffer, Targets.SplatIndexBuffer, NumClusters, ClusterCountBuffer, RenderStats, ColorLoadAction);
	}
	else
	{
		// TODO: Missing indirect dispatch and depth + velocity support
		RasterizeTiledCompute(GraphBuilder, View, ViewUniformBuffer, NumSplats, Targets.ShadedBuffer, RasterTarget);
	}

	if (bUsesScratch)
	{
		AddSplatScratchPostBlend(GraphBuilder, View, Output, ProxyScratchRT, bDownsampled,
			bProxyUsesInverseTC ? ProxyLUTRHI : nullptr, RenderProxy.InverseToneCurveStrength, RenderProxy.bInvertExpandGamutLUT);
	}
}

// Global sort (WinterSchool fork): all proxies shade into one shared buffer, then one sort and one draw, so splats of
// different components are composited in true per-splat depth order. The inverse tone curve is applied per splat.
static void RenderGlobalSorted(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	TUniformBufferRef<FViewUniformShaderParameters> ViewUniformBuffer,
	int32 DownsampleFactor,
	TConstArrayView<FGaussianSplatRenderItem> RenderItems,
	GaussianSplat::FRenderStats& RenderStats,
	const FScreenPassRenderTarget& Output,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneVelocityTexture)
{
	uint64 Capacity = 0;
	uint32 NumClusters = 0;
	for (const FGaussianSplatRenderItem& Item : RenderItems)
	{
		const bool bClusterDispatch = UsesClusterDispatch(*Item.Proxy, true);
		Capacity += Item.Proxy->NumSplats > 0 ? GetShadeCapacity(*Item.Proxy, bClusterDispatch) : 0u;
		NumClusters += bClusterDispatch ? Item.Proxy->NumClusters : 0u;
	}
	if (Capacity == 0)
	{
		return;
	}
	const uint32 NumSplats = uint32(FMath::Min<uint64>(Capacity, MAX_uint32 / sizeof(FPackedShadedGaussianSplatData)));

	const bool bExpectedDepth = (GetDepthMode(View.GetShaderPlatform()) == EGaussianSplatDepthMode::Expected);
	const bool bDownsampled = DownsampleFactor > 1;
	const bool bUsesScratch = bExpectedDepth || bDownsampled;
	FScreenPassRenderTarget ScratchRT;
	if (bUsesScratch)
	{
		ScratchRT = CreateSplatScratch(GraphBuilder, Output, DownsampleFactor, true);
	}
	const FScreenPassRenderTarget& RasterTarget = bUsesScratch ? ScratchRT : Output;

	FGaussianSplatShadeTargets Targets;
	Targets.ShadedBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FPackedShadedGaussianSplatData), NumSplats), TEXT("GaussianSplat.ShadedBuffer(Global)"));
	Targets.SortKeyBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats), TEXT("GaussianSplat.SortKeyBuffer(Global)"));
	Targets.SplatIndexBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats), TEXT("GaussianSplat.SplatIndexBuffer(Global)"));
	Targets.SplatCountBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("GaussianSplat.SplatCount(Global)"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Targets.SplatCountBuffer, PF_R32_UINT), 0u);

	FRDGBufferRef StatsClusterCount = nullptr;
	for (const FGaussianSplatRenderItem& Item : RenderItems)
	{
		if (Item.Proxy->NumSplats == 0)
		{
			continue;
		}
		FRDGBufferRef ClusterCount = ShadeProxy(GraphBuilder, View, ViewUniformBuffer, DownsampleFactor, *Item.Proxy, Item.InverseToneCurveLUTRHI, Targets);
		// the stats read one visible-cluster count; with several clustered proxies only the last one is reported
		StatsClusterCount = ClusterCount ? ClusterCount : StatsClusterCount;
	}

	const ERenderTargetLoadAction ColorLoadAction = bUsesScratch ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad;
	RasterizeHardware(GraphBuilder, View, ViewUniformBuffer, DownsampleFactor, SceneDepthTexture, SceneVelocityTexture, NumSplats, Targets.ShadedBuffer, RasterTarget, Targets.SplatCountBuffer,
		Targets.SortKeyBuffer, Targets.SplatIndexBuffer, NumClusters, StatsClusterCount, RenderStats, ColorLoadAction);

	if (bUsesScratch)
	{
		AddSplatScratchPostBlend(GraphBuilder, View, Output, ScratchRT, bDownsampled, nullptr, 0.0f, false);
	}
}

void GaussianSplat::Render(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	TConstArrayView<FGaussianSplatRenderItem> RenderItems,
	GaussianSplat::FRenderStats& RenderStats,
	const FScreenPassRenderTarget& Output,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneVelocityTexture)
{
	RDG_EVENT_SCOPE_STAT(GraphBuilder, GaussianSplat, "GaussianSplat");

	if (View.Family == nullptr || View.Family->Scene == nullptr || RenderItems.IsEmpty())
	{
		return;
	}

	TUniformBufferRef<FViewUniformShaderParameters> ViewUniformBuffer = View.ViewUniformBuffer;
	FRDGTextureRef DepthTexture = SceneDepthTexture;
	FRDGTextureRef VelocityTexture = SceneVelocityTexture;
	FRDGTextureRef DownsampledSceneDepthTexture = nullptr;

	const int32 DownsampleFactor = View.bIsViewInfo ? GetDownsampleFactor() : 1;
	if (DownsampleFactor > 1)
	{
		const FScreenPassTextureViewport DownsampledViewport = GetSplatViewport(Output, DownsampleFactor);
		ViewUniformBuffer = CreateDownsampledViewUniformBuffer(View, DownsampledViewport);
		DepthTexture = nullptr;
		VelocityTexture = nullptr;

		if (SceneDepthTexture != nullptr && GaussianSplat::IsHardwareRasterization(View.GetShaderPlatform()))
		{
			const FRDGTextureDesc DepthDesc = FRDGTextureDesc::Create2D(
				DownsampledViewport.Extent,
				SceneDepthTexture->Desc.Format,
				FClearValueBinding::DepthFar,
				TexCreate_DepthStencilTargetable | TexCreate_ShaderResource);
			DepthTexture = GraphBuilder.CreateTexture(DepthDesc, TEXT("GaussianSplat.DownsampledDepth"));

			FGaussianSplatDownsampleDepthPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianSplatDownsampleDepthPS::FParameters>();
			PassParameters->SceneDepthTexture = SceneDepthTexture;
			PassParameters->SvPositionToSourcePixel = FScreenTransform(FVector2f(Output.ViewRect.Size()) / FVector2f(DownsampledViewport.Extent), FVector2f(Output.ViewRect.Min));
			PassParameters->SourcePixelMax = Output.ViewRect.Max - FIntPoint(1, 1);
			PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(DepthTexture, ERenderTargetLoadAction::ENoAction, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);

			TShaderMapRef<FGaussianSplatDownsampleDepthPS> PixelShader(GetGlobalShaderMap(View.GetShaderPlatform()));
			FPixelShaderUtils::AddFullscreenPass(
				GraphBuilder,
				GetGlobalShaderMap(View.GetShaderPlatform()),
				RDG_EVENT_NAME("GaussianSplat.DownsampleDepth(%dx)", DownsampleFactor),
				PixelShader,
				PassParameters,
				DownsampledViewport.Rect,
				nullptr,
				nullptr,
				TStaticDepthStencilState<true, CF_Always>::GetRHI());

			if (GetDepthMode(View.GetShaderPlatform()) != EGaussianSplatDepthMode::Off)
			{
				DownsampledSceneDepthTexture = GraphBuilder.CreateTexture(DepthDesc, TEXT("GaussianSplat.DownsampledSceneDepth"));
				AddCopyTexturePass(GraphBuilder, DepthTexture, DownsampledSceneDepthTexture);
			}
		}
	}

	const bool bHWRaster = GaussianSplat::IsHardwareRasterization(View.GetShaderPlatform());
	const bool bGpuDrivenPipeline = bHWRaster && (!IsDebug() || GaussianSplat::IsGPUDrivenPipeline());
	if (CVarGaussianSplatGlobalSort.GetValueOnRenderThread() != 0 && bGpuDrivenPipeline)
	{
		RenderGlobalSorted(GraphBuilder, View, ViewUniformBuffer, DownsampleFactor, RenderItems, RenderStats, Output, DepthTexture, VelocityTexture);
	}
	else
	{
		for (const FGaussianSplatRenderItem& Item : RenderItems)
		{
			RenderProxyInternal(GraphBuilder, View, ViewUniformBuffer, DownsampleFactor, *Item.Proxy, Item.InverseToneCurveLUTRHI, RenderStats, Output, DepthTexture, VelocityTexture);
		}
	}

	if (DownsampledSceneDepthTexture != nullptr)
	{
		const bool bVelocity = IsVelocityEnabled()
			&& SceneVelocityTexture != nullptr
			&& EnumHasAnyFlags(SceneVelocityTexture->Desc.Flags, TexCreate_RenderTargetable);

		FGaussianSplatUpsampleDepthPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianSplatUpsampleDepthPS::FParameters>();
		PassParameters->SplatDepthTexture = DepthTexture;
		PassParameters->SceneDepthTexture = DownsampledSceneDepthTexture;
		PassParameters->SvPositionToDownsampledPixel = (FScreenTransform::Identity - Output.ViewRect.Min) * (FVector2f(DepthTexture->Desc.Extent) / FVector2f(Output.ViewRect.Size()));
		if (bVelocity)
		{
			PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneVelocityTexture, ERenderTargetLoadAction::ELoad);
		}
		PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);

		FGaussianSplatUpsampleDepthPS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FGaussianSplatUpsampleDepthPS::FWriteVelocity>(bVelocity);
		TShaderMapRef<FGaussianSplatUpsampleDepthPS> PixelShader(GetGlobalShaderMap(View.GetShaderPlatform()), PermutationVector);
		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder,
			GetGlobalShaderMap(View.GetShaderPlatform()),
			RDG_EVENT_NAME("GaussianSplat.UpsampleDepth(%dx)", DownsampleFactor),
			PixelShader,
			PassParameters,
			Output.ViewRect,
			nullptr,
			nullptr,
			TStaticDepthStencilState<true, CF_GreaterEqual>::GetRHI());
	}
}

bool GaussianSplat::UsesDeferredLighting(const FSceneView& View)
{
	const EShaderPlatform ShaderPlatform = View.GetShaderPlatform();
	if (CVarGaussianSplatLighting.GetValueOnRenderThread() == 0
		|| !View.bIsViewInfo
		|| View.Family == nullptr
		|| !View.Family->EngineShowFlags.Lighting
		|| !PlatformSupportsDeferredSplats(ShaderPlatform)
		// The GBuffer path is built on the hardware-rasterised, GPU-driven pipeline at full resolution
		|| !GaussianSplat::IsHardwareRasterization(ShaderPlatform)
		|| !GaussianSplat::IsGPUDrivenPipeline()
		|| IsGLES31EmulationEnabled()
		|| GetDownsampleFactor() > 1
		|| !SupportsSplatScratch(true))
	{
		return false;
	}

	if (IsMobilePlatform(ShaderPlatform))
	{
		// The splats are drawn from the post-opaque extensions, which run between base pass and lighting only on multipass
		// mobile deferred. That is every platform without framebuffer fetch, e.g. WebGPU (FMobileSceneRenderer::RequiresMultiPass);
		// single-pass deferred (Vulkan, iOS) lights inside the base render pass and keeps the post-process composite.
		return IsMobileDeferredShadingEnabled(ShaderPlatform) && !MobileAllowFramebufferFetch(ShaderPlatform);
	}

	// Substrate's adaptive GBuffer stores materials in a byte stream the legacy encoding can't write
	return !IsForwardShadingEnabled(ShaderPlatform)
		&& (!Substrate::IsSubstrateEnabled() || Substrate::IsSubstrateBlendableGBufferEnabled(ShaderPlatform));
}

void GaussianSplat::RenderDeferred(
	FRDGBuilder& GraphBuilder,
	const FSceneView& InView,
	TConstArrayView<FGaussianSplatRenderItem> RenderItems,
	GaussianSplat::FRenderStats& RenderStats,
	const FGaussianSplatDeferredTargets& DeferredTargets)
{
	RDG_EVENT_SCOPE_STAT(GraphBuilder, GaussianSplat, "GaussianSplat.Deferred");

	if (RenderItems.IsEmpty() || !InView.bIsViewInfo)
	{
		return;
	}

	const FViewInfo& View = static_cast<const FViewInfo&>(InView);
	const EShaderPlatform ShaderPlatform = View.GetShaderPlatform();
	const bool bMobile = IsMobilePlatform(ShaderPlatform);
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(ShaderPlatform);
	const TUniformBufferRef<FViewUniformShaderParameters> ViewUniformBuffer = View.ViewUniformBuffer;
	const FIntRect ViewRect = View.ViewRect;

	// Shade all items into one buffer (global sort), front to back, with their surfaces
	uint64 Capacity = 0;
	uint32 NumClusters = 0;
	for (const FGaussianSplatRenderItem& Item : RenderItems)
	{
		const bool bClusterDispatch = UsesClusterDispatch(*Item.Proxy, true);
		Capacity += Item.Proxy->NumSplats > 0 ? GetShadeCapacity(*Item.Proxy, bClusterDispatch) : 0u;
		NumClusters += bClusterDispatch ? Item.Proxy->NumClusters : 0u;
	}
	if (Capacity == 0)
	{
		return;
	}
	const uint32 NumSplats = uint32(FMath::Min<uint64>(Capacity, MAX_uint32 / sizeof(FPackedShadedGaussianSplatData)));

	FGaussianSplatShadeTargets Targets;
	Targets.ShadedBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FPackedShadedGaussianSplatData), NumSplats), TEXT("GaussianSplat.ShadedBuffer(Deferred)"));
	Targets.SurfaceBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector2), NumSplats), TEXT("GaussianSplat.SurfaceBuffer(Deferred)"));
	Targets.SortKeyBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats), TEXT("GaussianSplat.SortKeyBuffer(Deferred)"));
	Targets.SplatIndexBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats), TEXT("GaussianSplat.SplatIndexBuffer(Deferred)"));
	Targets.SplatCountBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("GaussianSplat.SplatCount(Deferred)"));
	Targets.bFrontToBack = true;
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Targets.SplatCountBuffer, PF_R32_UINT), 0u);

	FRDGBufferRef StatsClusterCount = nullptr;
	for (const FGaussianSplatRenderItem& Item : RenderItems)
	{
		if (Item.Proxy->NumSplats == 0)
		{
			continue;
		}
		FRDGBufferRef ClusterCount = ShadeProxy(GraphBuilder, View, ViewUniformBuffer, 1, *Item.Proxy, Item.InverseToneCurveLUTRHI, Targets);
		StatsClusterCount = ClusterCount ? ClusterCount : StatsClusterCount;
	}

	FRDGBufferRef DrawIndirectArgs = AddDrawIndirectArgsPass(GraphBuilder, View, NumSplats, Targets.SplatCountBuffer, NumClusters, StatsClusterCount, RenderStats);
	FRDGBufferRef SortedIndexBuffer = AddSortPass(GraphBuilder, View, NumSplats, Targets.SplatCountBuffer, Targets.SortKeyBuffer, Targets.SplatIndexBuffer);

	// Accumulation targets, cleared to transmittance 1
	const FRDGTextureDesc AccumulationDesc = FRDGTextureDesc::Create2D(
		DeferredTargets.SceneColor->Desc.Extent,
		PF_FloatRGBA,
		FClearValueBinding(FLinearColor(0.0f, 0.0f, 0.0f, 1.0f)),
		TexCreate_ShaderResource | TexCreate_RenderTargetable);
	FRDGTextureRef SplatColor = GraphBuilder.CreateTexture(AccumulationDesc, TEXT("GaussianSplat.Deferred.Color"));
	FRDGTextureRef SplatDepthAndLighting = GraphBuilder.CreateTexture(AccumulationDesc, TEXT("GaussianSplat.Deferred.DepthAndLighting"));
	FRDGTextureRef SplatNormal = GraphBuilder.CreateTexture(AccumulationDesc, TEXT("GaussianSplat.Deferred.Normal"));

	// Front-to-back accumulation, depth tested against the opaque meshes of the base pass
	{
		FHardwareRasterizeGaussianSplatVS::FPermutationDomain VSPermutationVector;
		VSPermutationVector.Set<FHardwareRasterizeGaussianSplatVS::FDeferred>(true);
		TShaderMapRef<FHardwareRasterizeGaussianSplatVS> VertexShader(ShaderMap, VSPermutationVector);
		TShaderMapRef<FHardwareRasterizeGaussianSplatDeferredPS> PixelShader(ShaderMap);

		FHardwareRasterizeGaussianSplatDeferredPassParameters* PassParameters = GraphBuilder.AllocParameters<FHardwareRasterizeGaussianSplatDeferredPassParameters>();
		PassParameters->VS.View = ViewUniformBuffer;
		PassParameters->VS.ShadedGaussianSplatBuffer = GraphBuilder.CreateSRV(Targets.ShadedBuffer);
		PassParameters->VS.SortedIndexBuffer = GraphBuilder.CreateSRV(SortedIndexBuffer, PF_R32_UINT);
		PassParameters->VS.ShadedSplatSurfaceBuffer = GraphBuilder.CreateSRV(Targets.SurfaceBuffer);
		PassParameters->PS.View = ViewUniformBuffer;
		PassParameters->PS.OpacityThreshold = GetHardwareRasterizationOpacityThreshold();
		PassParameters->PS.ExpectedDepthScale = GetExpectedDepthScale();
		PassParameters->PS.FalloffParameters = GetFalloffParameters();
		PassParameters->DrawIndirectArgs = DrawIndirectArgs;
		PassParameters->RenderTargets[0] = FRenderTargetBinding(SplatColor, ERenderTargetLoadAction::EClear);
		PassParameters->RenderTargets[1] = FRenderTargetBinding(SplatDepthAndLighting, ERenderTargetLoadAction::EClear);
		PassParameters->RenderTargets[2] = FRenderTargetBinding(SplatNormal, ERenderTargetLoadAction::EClear);
		PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
			DeferredTargets.SceneDepth, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthRead_StencilNop);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GaussianSplat.Deferred.Accumulate"),
			PassParameters,
			ERDGPassFlags::Raster,
			[PassParameters, VertexShader, PixelShader, ViewRect](FRDGAsyncTask, FRHICommandList& RHICmdList)
			{
				FGraphicsPipelineStateInitializer GraphicsPSOInit;
				RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

				// UNDER operator on all targets: rgb += src * DestAlpha, a *= 1 - srcAlpha
				GraphicsPSOInit.BlendState = TStaticBlendState<
					CW_RGBA, BO_Add, BF_DestAlpha, BF_One, BO_Add, BF_Zero, BF_InverseSourceAlpha,
					CW_RGBA, BO_Add, BF_DestAlpha, BF_One, BO_Add, BF_Zero, BF_InverseSourceAlpha,
					CW_RGBA, BO_Add, BF_DestAlpha, BF_One, BO_Add, BF_Zero, BF_InverseSourceAlpha
				>::GetRHI();
				GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
				GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI();
				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;
				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

				RHICmdList.SetViewport(ViewRect.Min.X, ViewRect.Min.Y, 0.0f, ViewRect.Max.X, ViewRect.Max.Y, 1.0f);

				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), PassParameters->VS);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParameters->PS);

				PassParameters->DrawIndirectArgs->MarkResourceAsUsed();
				RHICmdList.DrawPrimitiveIndirect(PassParameters->DrawIndirectArgs->GetIndirectRHICallBuffer(), 0);
			});
	}

	const float CoverageThreshold = FMath::Clamp(CVarGaussianSplatLightingCoverageThreshold.GetValueOnRenderThread(), 0.0f, 1.0f);
	// Dithered coverage needs temporal AA to resolve it; without (e.g. FXAA on mobile) a threshold gives clean edges
	const uint32 DitheredCoverage = (CVarGaussianSplatLightingDither.GetValueOnRenderThread() != 0 && IsTemporalAccumulationBasedMethod(View.AntiAliasingMethod)) ? 1u : 0u;

	// Resolve into the base pass outputs
	{
		const bool bVelocity = !bMobile
			&& DeferredTargets.Velocity != nullptr
			&& HasBeenProduced(DeferredTargets.Velocity)
			&& EnumHasAnyFlags(DeferredTargets.Velocity->Desc.Flags, TexCreate_RenderTargetable);

		FGaussianSplatDeferredResolvePS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianSplatDeferredResolvePS::FParameters>();
		PassParameters->View = ViewUniformBuffer;
		PassParameters->SplatColorTexture = SplatColor;
		PassParameters->SplatDepthAndLightingTexture = SplatDepthAndLighting;
		PassParameters->SplatNormalTexture = SplatNormal;
		PassParameters->ExpectedDepthScale = GetExpectedDepthScale();
		PassParameters->CoverageThreshold = CoverageThreshold;
		PassParameters->DitheredCoverage = DitheredCoverage;
		PassParameters->GBufferSpecular = FMath::Clamp(CVarGaussianSplatLightingSpecular.GetValueOnRenderThread(), 0.0f, 1.0f);
		PassParameters->GBufferRoughness = FMath::Clamp(CVarGaussianSplatLightingRoughness.GetValueOnRenderThread(), 0.0f, 1.0f);
		PassParameters->ShadowStrength = FMath::Max(CVarGaussianSplatLightingShadowStrength.GetValueOnRenderThread(), 0.0f);
		PassParameters->ShowClipping = CVarGaussianSplatLightingShowClipping.GetValueOnRenderThread() != 0 ? 1u : 0u;
		PassParameters->SurfaceOffset = FMath::Max(CVarGaussianSplatLightingSurfaceOffset.GetValueOnRenderThread(), 0.0f);
		PassParameters->RenderTargets[0] = FRenderTargetBinding(DeferredTargets.SceneColor, ERenderTargetLoadAction::ELoad);
		PassParameters->RenderTargets[1] = FRenderTargetBinding(DeferredTargets.GBufferA, ERenderTargetLoadAction::ELoad);
		PassParameters->RenderTargets[2] = FRenderTargetBinding(DeferredTargets.GBufferB, ERenderTargetLoadAction::ELoad);
		PassParameters->RenderTargets[3] = FRenderTargetBinding(DeferredTargets.GBufferC, ERenderTargetLoadAction::ELoad);
		if (bVelocity)
		{
			PassParameters->RenderTargets[4] = FRenderTargetBinding(DeferredTargets.Velocity, ERenderTargetLoadAction::ELoad);
		}

		FRHIDepthStencilState* DepthStencilState = nullptr;
		uint32 StencilRef = 0;
		if (bMobile)
		{
			// Mobile deferred lights only pixels whose stencil marks their shading model (MobileBasePass.cpp): default lit,
			// lighting channel 0, receives decals
			PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
				DeferredTargets.SceneDepth, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthWrite_StencilWrite);
			DepthStencilState = TStaticDepthStencilState<
				true, CF_DepthNearOrEqual,
				true, CF_Always, SO_Keep, SO_Keep, SO_Replace,
				false, CF_Always, SO_Keep, SO_Keep, SO_Keep,
				0x00, 0xff>::GetRHI();
			StencilRef = GET_STENCIL_BIT_MASK(MOBILE_DEFAULTLIT, 1) | GET_STENCIL_BIT_MASK(RECEIVE_DECAL, 1);
		}
		else
		{
			PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
				DeferredTargets.SceneDepth, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);
			DepthStencilState = TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI();
		}

		FGaussianSplatDeferredResolvePS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FGaussianSplatDeferredResolvePS::FWriteVelocity>(bVelocity);
		TShaderMapRef<FGaussianSplatDeferredResolvePS> PixelShader(ShaderMap, PermutationVector);
		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder,
			ShaderMap,
			RDG_EVENT_NAME("GaussianSplat.Deferred.Resolve"),
			PixelShader,
			PassParameters,
			ViewRect,
			nullptr,
			nullptr,
			DepthStencilState,
			StencilRef);
	}

	// The pixels that did not become a splat surface: splats composited unlit over what is behind them. Mesh pixels
	// in front of the splats have no coverage (the accumulation was depth tested), so no depth test is needed.
	{
		FGaussianSplatDeferredRemainderPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGaussianSplatDeferredRemainderPS::FParameters>();
		PassParameters->View = ViewUniformBuffer;
		PassParameters->SplatColorTexture = SplatColor;
		PassParameters->CoverageThreshold = CoverageThreshold;
		PassParameters->DitheredCoverage = DitheredCoverage;
		PassParameters->RenderTargets[0] = FRenderTargetBinding(DeferredTargets.SceneColor, ERenderTargetLoadAction::ELoad);

		TShaderMapRef<FGaussianSplatDeferredRemainderPS> PixelShader(ShaderMap);
		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder,
			ShaderMap,
			RDG_EVENT_NAME("GaussianSplat.Deferred.Remainder"),
			PixelShader,
			PassParameters,
			ViewRect,
			// premultiplied over: SceneColor = Splat + SceneColor * Transmittance
			TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_SourceAlpha>::GetRHI());
	}
}
