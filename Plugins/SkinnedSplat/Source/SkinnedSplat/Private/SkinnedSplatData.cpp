#include "SkinnedSplatData.h"
#include "Misc/FileHelper.h"
#include "Serialization/Archive.h"

DEFINE_LOG_CATEGORY_STATIC(LogSkinnedSplatData, Log, All);

bool USkinnedSplatData::LoadFromFile(const FString& Path)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() < 16 || FMemory::Memcmp(Bytes.GetData(), "SKSP", 4) != 0)
	{
		UE_LOG(LogSkinnedSplatData, Error, TEXT("not a .skinsplat file: %s"), *Path);
		return false;
	}
	Raw = MoveTemp(Bytes);
	Modify();
	MarkPackageDirty();
	UE_LOG(LogSkinnedSplatData, Display, TEXT("loaded %s (%d bytes)"), *Path, Raw.Num());
	return true;
}

void USkinnedSplatData::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
	Ar << Raw;
}
