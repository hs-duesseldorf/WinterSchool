// Copyright Epic Games, Inc. All Rights Reserved.

#include "NiagaraSplatRendererProperties.h"

#include "NiagaraRendererSplats.h"

#include "GaussianSplatCommon.h"
#include "NiagaraBoundsCalculator.h"
#include "NiagaraConstants.h"
#include "NiagaraDataSetAccessor.h"
#include "NiagaraModule.h"
#include "NiagaraParameterStore.h"

#include "Modules/ModuleManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(NiagaraSplatRendererProperties)

#if WITH_EDITOR
#include "GaussianSplatComponent.h"
#include "Styling/SlateIconFinder.h"
#include "Widgets/Text/STextBlock.h"
#endif

#define LOCTEXT_NAMESPACE "UNiagaraSplatRendererProperties"

namespace NiagaraSplatRendererPropertiesLocal
{
	TArray<TWeakObjectPtr<UNiagaraSplatRendererProperties>> RendererPropertiesToDeferredInit;

	void SetupBindings(UNiagaraSplatRendererProperties* Props)
	{
		if (Props->PositionBinding.IsValid())
		{
			return;
		}
		Props->InitRendererBaseBindings();
		Props->PositionBinding = FNiagaraConstants::GetAttributeDefaultBinding(SYS_PARAM_PARTICLES_POSITION);
		Props->ScaleBinding = FNiagaraConstants::GetAttributeDefaultBinding(SYS_PARAM_PARTICLES_SCALE);
		Props->RotationBinding = FNiagaraConstants::GetAttributeDefaultBinding(SYS_PARAM_PARTICLES_MESH_ORIENTATION);
		Props->ColorBinding = FNiagaraConstants::GetAttributeDefaultBinding(SYS_PARAM_PARTICLES_COLOR);
		Props->RendererVisibilityTagBinding = FNiagaraConstants::GetAttributeDefaultBinding(SYS_PARAM_PARTICLES_VISIBILITY_TAG);
	}

	FName ParticleAttributeName(const FNiagaraVariableAttributeBinding& Binding)
	{
		return Binding.IsParticleBinding() ? Binding.GetDataSetBindableVariable().GetName() : NAME_None;
	}

	class FNiagaraBoundsCalculatorSplats : public FNiagaraBoundsCalculator
	{
	public:
		FNiagaraBoundsCalculatorSplats(const UNiagaraSplatRendererProperties& Properties)
			: PositionName(ParticleAttributeName(Properties.PositionBinding))
			, ScaleName(ParticleAttributeName(Properties.ScaleBinding))
			, SplatScale(Properties.SplatScale)
		{
		}

		virtual void InitAccessors(const FNiagaraDataSetCompiledData* CompiledData) override
		{
			PositionAccessor.Init(CompiledData, PositionName);
			ScaleAccessor.Init(CompiledData, ScaleName);
		}

		virtual FBox CalculateBounds(const FTransform& SystemTransform, const FNiagaraDataSet& DataSet, const int32 NumInstances) const override
		{
			if (NumInstances == 0 || !PositionAccessor.IsValid())
			{
				return FBox(ForceInit);
			}
			FNiagaraPosition BoundsMin(ForceInitToZero), BoundsMax(ForceInitToZero);
			PositionAccessor.GetReader(DataSet).GetMinMax(BoundsMin, BoundsMax);

			float MaxStandardDeviation = 1.0f;
			if (ScaleAccessor.IsValid())
			{
				FVector3f ScaleMin(ForceInitToZero), ScaleMax(ForceInitToZero);
				ScaleAccessor.GetReader(DataSet).GetMinMax(ScaleMin, ScaleMax);
				MaxStandardDeviation = FMath::Max(ScaleMin.GetAbsMax(), ScaleMax.GetAbsMax());
			}
			return FBox(BoundsMin, BoundsMax).ExpandBy(MaxStandardDeviation * SplatScale * GAUSSIAN_SPLAT_RADIUS);
		}

	private:
		FNiagaraDataSetAccessor<FNiagaraPosition> PositionAccessor;
		FNiagaraDataSetAccessor<FVector3f> ScaleAccessor;
		FName PositionName;
		FName ScaleName;
		float SplatScale;
	};
}

UNiagaraSplatRendererProperties::UNiagaraSplatRendererProperties()
{
	AttributeBindings = { &PositionBinding, &ScaleBinding, &RotationBinding, &ColorBinding, &RendererVisibilityTagBinding };
}

void UNiagaraSplatRendererProperties::PostLoad()
{
	Super::PostLoad();
#if WITH_EDITORONLY_DATA
	ChangeToPositionBinding(PositionBinding);
#endif
	PostLoadBindings(ENiagaraRendererSourceDataMode::Particles);
}

void UNiagaraSplatRendererProperties::PostInitProperties()
{
	using namespace NiagaraSplatRendererPropertiesLocal;

	Super::PostInitProperties();
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		if (FModuleManager::Get().IsModuleLoaded("Niagara"))
		{
			SetupBindings(this);
		}
		else
		{
			RendererPropertiesToDeferredInit.Add(this);
		}
	}
}

void UNiagaraSplatRendererProperties::InitCDOPropertiesAfterModuleStartup()
{
	using namespace NiagaraSplatRendererPropertiesLocal;

	SetupBindings(GetMutableDefault<UNiagaraSplatRendererProperties>());
	for (TWeakObjectPtr<UNiagaraSplatRendererProperties>& WeakProps : RendererPropertiesToDeferredInit)
	{
		if (UNiagaraSplatRendererProperties* Props = WeakProps.Get())
		{
			SetupBindings(Props);
		}
	}
	RendererPropertiesToDeferredInit.Empty();
}

FNiagaraRenderer* UNiagaraSplatRendererProperties::CreateEmitterRenderer(ERHIFeatureLevel::Type FeatureLevel, const FNiagaraEmitterInstance* Emitter, const FNiagaraSystemInstanceController& InController)
{
	FNiagaraRenderer* NewRenderer = new FNiagaraRendererSplats(FeatureLevel, this, Emitter);
	NewRenderer->Initialize(this, Emitter, InController);
	return NewRenderer;
}

FNiagaraBoundsCalculator* UNiagaraSplatRendererProperties::CreateBoundsCalculator()
{
	return new NiagaraSplatRendererPropertiesLocal::FNiagaraBoundsCalculatorSplats(*this);
}

void UNiagaraSplatRendererProperties::CacheFromCompiledData(const FNiagaraDataSetCompiledData* CompiledData)
{
	UpdateSourceModeDerivates(ENiagaraRendererSourceDataMode::Particles);
}

bool UNiagaraSplatRendererProperties::PopulateRequiredBindings(FNiagaraParameterStore& InParameterStore)
{
	bool bAnyAdded = Super::PopulateRequiredBindings(InParameterStore);
	for (const FNiagaraVariableAttributeBinding* Binding : AttributeBindings)
	{
		if (Binding->CanBindToHostParameterMap())
		{
			InParameterStore.AddParameter(Binding->GetParamMapBindableVariable(), false);
			bAnyAdded = true;
		}
	}
	return bAnyAdded;
}

#if WITH_EDITORONLY_DATA
const TArray<FNiagaraVariable>& UNiagaraSplatRendererProperties::GetOptionalAttributes()
{
	static const TArray<FNiagaraVariable> Attrs = { SYS_PARAM_PARTICLES_POSITION, SYS_PARAM_PARTICLES_SCALE, SYS_PARAM_PARTICLES_MESH_ORIENTATION, SYS_PARAM_PARTICLES_COLOR, SYS_PARAM_PARTICLES_VISIBILITY_TAG };
	return Attrs;
}

const FSlateBrush* UNiagaraSplatRendererProperties::GetStackIcon() const
{
	return FSlateIconFinder::FindIconBrushForClass(UGaussianSplatComponent::StaticClass());
}

void UNiagaraSplatRendererProperties::GetRendererTooltipWidgets(const FNiagaraEmitterInstance* InEmitter, TArray<TSharedPtr<SWidget>>& OutWidgets, TSharedPtr<FAssetThumbnailPool> InThumbnailPool) const
{
	OutWidgets.Add(SNew(STextBlock).Text(LOCTEXT("SplatRenderer", "Splat Renderer")));
}

void UNiagaraSplatRendererProperties::GetRendererFeedback(const FVersionedNiagaraEmitter& InEmitter, TArray<FNiagaraRendererFeedback>& OutErrors, TArray<FNiagaraRendererFeedback>& OutWarnings, TArray<FNiagaraRendererFeedback>& OutInfo) const
{
	Super::GetRendererFeedback(InEmitter, OutErrors, OutWarnings, OutInfo);
	OutInfo.Emplace(LOCTEXT("SplatRendererPipelineInfo", "Splats are composited by the Gaussian Splat plugin's post process pass. Nothing will appear if that pass is disabled (r.GaussianSplat 0)."));
}
#endif // WITH_EDITORONLY_DATA

#undef LOCTEXT_NAMESPACE
