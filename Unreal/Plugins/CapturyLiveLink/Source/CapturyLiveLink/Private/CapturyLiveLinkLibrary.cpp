// Added for WinterSchool, see CapturyLiveLinkLibrary.h

#include "CapturyLiveLinkLibrary.h"
#include "CapturyLiveLinkSourceFactory.h"
#include "SCapturySourceConfigWidget.h"
#include "Features/IModularFeatures.h"
#include "ILiveLinkClient.h"
#include "LiveLinkSourceSettings.h"

bool UCapturyLiveLinkLibrary::AddCapturySource(const FString& Address, bool bUseTCP, bool bStreamCompressed)
{
	IModularFeatures& Features = IModularFeatures::Get();
	if (!Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
	{
		return false;
	}
	ILiveLinkClient& Client = Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);

	for (const FGuid& Guid : Client.GetSources())
	{
		const ULiveLinkSourceSettings* Settings = Client.GetSourceSettings(Guid);
		if (Settings && Settings->Factory == UCapturyLiveLinkSourceFactory::StaticClass() && Settings->ConnectionString.StartsWith(Address + TEXT(";")))
		{
			return true;
		}
	}

	// same format as the creation panel: address;tcp;artags;compressed
	const FString ConnectionString = FString::Printf(TEXT("%s;%d;%d;%d"), *Address, bUseTCP, 0, bStreamCompressed);
	TSharedPtr<ILiveLinkSource> Source = SCapturySourceConfigWidget::createSource(ConnectionString);
	if (!Source.IsValid())
	{
		return false;
	}
	const FGuid Guid = Client.AddSource(Source);
	if (ULiveLinkSourceSettings* Settings = Client.GetSourceSettings(Guid))
	{
		Settings->ConnectionString = ConnectionString;
		Settings->Factory = UCapturyLiveLinkSourceFactory::StaticClass();
	}
	return Guid.IsValid();
}
