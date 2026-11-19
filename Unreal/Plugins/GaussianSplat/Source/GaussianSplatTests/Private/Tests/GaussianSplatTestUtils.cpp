// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatTestUtils.h"

#include "GaussianSplatBuilder.h"
#include "GaussianSplatCommon.h"

#include "Factories/Factory.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

namespace GaussianSplatTests
{
	FString GetPlyFixturePath(const TCHAR* FileName)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GaussianSplat"));
		check(Plugin.IsValid());

		return FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source/GaussianSplatTests/TestData/Ply"), FileName);
	}

	FGaussianSplatBuilder MakeSyntheticBuilder(int32 NumSplats, int32 SHDegree)
	{
		FGaussianSplatBuilder Builder;
		Builder.SHDegree = SHDegree;
		Builder.CoordinateSpace = EGaussianSplatCoordinateSpace::LH_FRU;

		const int32 SHDimension = Builder.GetSHDimension();

		Builder.BaseData.SetNum(NumSplats);
		for (int32 SplatIndex = 0; SplatIndex < NumSplats; ++SplatIndex)
		{
			FGaussianSplatFormatFloat::FBaseData& Splat = Builder.BaseData[SplatIndex];
			Splat.Position = FVector3f(float(SplatIndex), float(SplatIndex) * 2.0f, float(SplatIndex) * 3.0f);
			Splat.Scale = FVector3f(1.0f, 1.0f, 1.0f);
			Splat.Rotation = FQuat4f::Identity;
			Splat.Opacity = 0.5f + 0.01f * float(SplatIndex);
		}

		Builder.SHData.SetNum(NumSplats * SHDimension);
		for (int32 Index = 0; Index < Builder.SHData.Num(); ++Index)
		{
			Builder.SHData[Index] = FVector3f(0.1f * float(Index), 0.2f * float(Index), 0.3f * float(Index));
		}

		Builder.CalculateBounds();

		return Builder;
	}

	UClass* FindFactoryClassForAsset(UClass* AssetClass)
	{
		for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
		{
			UClass* Class = *ClassIt;
			if (!Class->IsChildOf(UFactory::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
			{
				continue;
			}

			const UFactory* FactoryCDO = Cast<UFactory>(Class->GetDefaultObject());
			if (FactoryCDO && FactoryCDO->GetSupportedClass() == AssetClass)
			{
				return Class;
			}
		}
		return nullptr;
	}

	UClass* FindGaussianSplatComponentClass()
	{
		return FindObject<UClass>(nullptr, TEXT("/Script/GaussianSplat.GaussianSplatComponent"));
	}
}
