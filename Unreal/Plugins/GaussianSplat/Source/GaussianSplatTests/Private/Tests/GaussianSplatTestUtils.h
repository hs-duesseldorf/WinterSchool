// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/UnrealString.h"

struct FGaussianSplatBuilder;
class UObject;
class UClass;
class UFactory;

namespace GaussianSplatTests
{
	// Returns the absolute path to a fixture file under this module's TestData/Ply folder.
	FString GetPlyFixturePath(const TCHAR* FileName);

	// Builds a small, deterministic, valid splat data set (NumSplats > 0, valid bounds, SH data
	// sized to match) suitable for feeding UGaussianSplat::SetData(). Two calls with the same
	// arguments always produce byte-identical BaseData/SHData.
	FGaussianSplatBuilder MakeSyntheticBuilder(int32 NumSplats, int32 SHDegree);

	// UGaussianSplatFactory is compiled into GaussianSplatEditor's Private folder, so it has no
	// header this module can include. Find it the same way the editor's importer UI does: by
	// scanning registered UFactory classes for the one whose SupportedClass matches.
	UClass* FindFactoryClassForAsset(UClass* AssetClass);

	// UGaussianSplatComponent has no MinimalAPI/module API export, so its StaticClass() and
	// constructor are NO_API and unlinkable from this module. Resolve the UClass by object path
	// and construct through NewObject<USceneComponent>() instead, as Blueprint's AddComponent does.
	UClass* FindGaussianSplatComponentClass();
}
