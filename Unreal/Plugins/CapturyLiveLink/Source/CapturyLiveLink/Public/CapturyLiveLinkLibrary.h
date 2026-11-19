// Added for WinterSchool: create the Captury Live Link source from Blueprint/Python (the Live Link window only offers
// the interactive panel). The source is registered like the panel does it, with its factory and connection string, so
// it can be saved into a Live Link Preset and restored from there.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CapturyLiveLinkLibrary.generated.h"

UCLASS()
class CAPTURYLIVELINK_API UCapturyLiveLinkLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	/**
	 * Connects Live Link to Captury Live / Captury Replay (or Scripts/captury_replay.py) at Address, port 2101.
	 * Does nothing if a Captury source for that address exists already. Returns false if the address does not resolve.
	 */
	UFUNCTION(BlueprintCallable, Category = "Captury")
	static bool AddCapturySource(const FString& Address = TEXT("127.0.0.1"), bool bUseTCP = false, bool bStreamCompressed = false);
};
