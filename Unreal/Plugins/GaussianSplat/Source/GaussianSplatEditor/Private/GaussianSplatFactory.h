// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Factories/Factory.h"
#include "EditorReimportHandler.h"

#include "GaussianSplatFactory.generated.h"

class UGaussianSplatImportOptions;

UCLASS(hidecategories=Object, MinimalAPI)
class UGaussianSplatFactory : public UFactory, public FReimportHandler
{
	GENERATED_UCLASS_BODY()

	// Begin: UFactory Impl
	virtual UObject* FactoryCreateBinary(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, UObject* Context, const TCHAR* Type, const uint8*& Buffer, const uint8* BufferEnd, FFeedbackContext* Warn) override;
	// End: UFactory Impl

	// Begin: FReimportHandler Impl
	virtual bool CanReimport(UObject* InObject, TArray<FString>& OutFilenames) override;
	virtual void SetReimportPaths(UObject* InObject, const TArray<FString>& NewReimportPaths) override;
	virtual EReimportResult::Type Reimport(UObject* InObject) override;
	virtual int32 GetPriority() const override;

	virtual bool ConfigureProperties() override;
	// End: FReimportHandler Impl

	UPROPERTY()
	TObjectPtr<UGaussianSplatImportOptions> ImportOptions;
};
