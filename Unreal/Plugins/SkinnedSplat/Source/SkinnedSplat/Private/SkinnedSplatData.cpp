#include "SkinnedSplatData.h"
#include "Misc/FileHelper.h"
#include "Serialization/Archive.h"
#include "UObject/AssetRegistryTagsContext.h"
#if WITH_EDITORONLY_DATA
#include "EditorFramework/AssetImportData.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogSkinnedSplatData, Log, All);

namespace SkinnedSplatPLY
{
	struct FHeader
	{
		int64 DataOffset = 0;
		int32 NumVertices = 0;
		int32 Stride = 0;                  // bytes per vertex
		TMap<FString, int32> FloatOffsets; // property -> byte offset
		TArray<FString> Comments;
	};

	static bool ParseHeader(TConstArrayView<uint8> Data, FHeader& Out, FString& OutError)
	{
		int64 Pos = 0;
		auto NextLine = [&](FString& Line) -> bool
		{
			const int64 Start = Pos;
			while (Pos < Data.Num() && Data[Pos] != '\n') ++Pos;
			if (Pos >= Data.Num()) return false;
			Line = FString(int32(Pos - Start), reinterpret_cast<const ANSICHAR*>(Data.GetData() + Start)).TrimEnd();
			++Pos;
			return true;
		};
		FString Line;
		if (!NextLine(Line) || Line != TEXT("ply")) { OutError = TEXT("not a PLY file"); return false; }
		bool bFormat = false, bInVertex = false;
		while (NextLine(Line))
		{
			if (Line == TEXT("end_header")) { Out.DataOffset = Pos; break; }
			if (Line == TEXT("format binary_little_endian 1.0")) { bFormat = true; }
			else if (Line.StartsWith(TEXT("comment "))) { Out.Comments.Add(Line.Mid(8)); }
			else if (Line.StartsWith(TEXT("element ")))
			{
				TArray<FString> T; Line.ParseIntoArrayWS(T);
				bInVertex = T.Num() == 3 && T[1] == TEXT("vertex");
				if (bInVertex) Out.NumVertices = FCString::Atoi(*T[2]);
				else if (Out.NumVertices > 0) { OutError = TEXT("elements after 'vertex' are not supported"); return false; }
			}
			else if (Line.StartsWith(TEXT("property ")))
			{
				TArray<FString> T; Line.ParseIntoArrayWS(T);
				if (!bInVertex || T.Num() != 3 || T[1] != TEXT("float")) { OutError = FString::Printf(TEXT("unsupported property line '%s' (only float vertex properties)"), *Line); return false; }
				Out.FloatOffsets.Add(T[2], Out.Stride);
				Out.Stride += 4;
			}
		}
		if (!bFormat) { OutError = TEXT("only binary_little_endian 1.0 PLY files are supported"); return false; }
		if (Out.DataOffset == 0 || Out.NumVertices <= 0) { OutError = TEXT("missing end_header or vertex element"); return false; }
		if (Out.DataOffset + int64(Out.NumVertices) * Out.Stride > Data.Num()) { OutError = TEXT("file is truncated"); return false; }
		return true;
	}

	// COLMAP / 3DGS PLY space (right handed, X right, Y down, Z forward) -> Unreal, exactly like Epic's PLY importer
	static FVector3f ToUE(float X, float Y, float Z, float S) { return FVector3f(Z, X, -Y) * S; }
}

bool USkinnedSplatData::IsSkinnedPLY(const FString& Path)
{
	TArray<uint8> Head;
	TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileReader(*Path));
	if (!Ar) return false;
	Head.SetNumUninitialized(int32(FMath::Min<int64>(Ar->TotalSize(), 64 * 1024)));
	Ar->Serialize(Head.GetData(), Head.Num());
	const FAnsiStringView View(reinterpret_cast<const ANSICHAR*>(Head.GetData()), Head.Num());
	const int32 End = View.Find("end_header");
	return End != INDEX_NONE && View.Left(End).Find("property float bone_0") != INDEX_NONE;
}

bool USkinnedSplatData::LoadFromPLY(const FString& Path, FString& OutError, float ImportScale)
{
	using namespace SkinnedSplatPLY;
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path)) { OutError = TEXT("cannot read ") + Path; return false; }
	FHeader H;
	if (!ParseHeader(Bytes, H, OutError)) return false;

	static const TCHAR* Required[] = { TEXT("x"), TEXT("y"), TEXT("z"), TEXT("f_dc_0"), TEXT("f_dc_1"), TEXT("f_dc_2"), TEXT("opacity"),
		TEXT("scale_0"), TEXT("scale_1"), TEXT("scale_2"), TEXT("rot_0"), TEXT("rot_1"), TEXT("rot_2"), TEXT("rot_3"),
		TEXT("bone_0"), TEXT("bone_1"), TEXT("bone_2"), TEXT("bone_3"), TEXT("weight_0"), TEXT("weight_1"), TEXT("weight_2"), TEXT("weight_3") };
	int32 Off[UE_ARRAY_COUNT(Required)];
	for (int32 i = 0; i < UE_ARRAY_COUNT(Required); ++i)
	{
		const int32* O = H.FloatOffsets.Find(Required[i]);
		if (!O) { OutError = FString::Printf(TEXT("missing vertex property '%s'"), Required[i]); return false; }
		Off[i] = *O;
	}

	// bones from "comment bone <index> <name> <parent> <x> <y> <z>"
	TArray<FName> Names; TArray<int32> Parents; TArray<FVector3f> Joints;
	for (const FString& C : H.Comments)
	{
		TArray<FString> T; C.ParseIntoArrayWS(T);
		if (T.Num() != 7 || T[0] != TEXT("bone")) continue;
		const int32 Index = FCString::Atoi(*T[1]);
		if (Index != Names.Num()) { OutError = TEXT("bone comments must be in index order"); return false; }
		Names.Add(FName(*T[2])); Parents.Add(FCString::Atoi(*T[3]));
		Joints.Add(ToUE(FCString::Atof(*T[4]), FCString::Atof(*T[5]), FCString::Atof(*T[6]), ImportScale));
	}
	if (Names.Num() == 0 || Names.Num() > 255) { OutError = FString::Printf(TEXT("need 1..255 'comment bone' lines, found %d"), Names.Num()); return false; }

	constexpr double SH_C0 = 0.28209479177387814;
	const int32 N = H.NumVertices;
	TArray<FVector4f> NewRest; NewRest.SetNumUninitialized(N * 5);
	FBox3f Box(ForceInit);
	const uint8* Base = Bytes.GetData() + H.DataOffset;
	int32 BadBones = 0;
	for (int32 i = 0; i < N; ++i)
	{
		const uint8* V = Base + int64(i) * H.Stride;
		float F[UE_ARRAY_COUNT(Required)];
		for (int32 k = 0; k < UE_ARRAY_COUNT(Required); ++k) FMemory::Memcpy(&F[k], V + Off[k], 4);
		const FVector3f P = ToUE(F[0], F[1], F[2], ImportScale);
		const float Opacity = FMath::Clamp(1.0f / (1.0f + FMath::Exp(-F[6])), 0.0f, 1.0f);
		const FVector3f S = FVector3f(FMath::Exp(F[9]), FMath::Exp(F[7]), FMath::Exp(F[8])) * ImportScale;
		const FQuat4f Q = FQuat4f(F[13], F[11], -F[12], -F[10]).GetNormalized(); // file rot_0..3 = w x y z
		uint32 Packed = 0; float W[4]; float WSum = 0;
		for (int32 k = 0; k < 4; ++k)
		{
			int32 B = FMath::RoundToInt(F[14 + k]);
			if (B < 0 || B >= Names.Num()) { B = 0; ++BadBones; }
			Packed |= uint32(B) << (8 * k);
			W[k] = FMath::Max(F[18 + k], 0.0f); WSum += W[k];
		}
		if (WSum <= 0) { W[0] = 1; WSum = 1; }
		FVector4f* O = &NewRest[i * 5];
		O[0] = FVector4f(P, Opacity);
		O[1] = FVector4f(Q.X, Q.Y, Q.Z, Q.W);
		O[2] = FVector4f(S, *reinterpret_cast<const float*>(&Packed));
		O[3] = FVector4f(F[3], F[4], F[5], 0.0f);
		O[4] = FVector4f(W[0] / WSum, W[1] / WSum, W[2] / WSum, W[3] / WSum);
		Box += P;
	}
	if (BadBones) UE_LOG(LogSkinnedSplatData, Warning, TEXT("%s: %d out-of-range bone indices clamped to 0"), *Path, BadBones);

	Rest = MoveTemp(NewRest);
	NumSplats = N; BoneNames = MoveTemp(Names); BoneParents = MoveTemp(Parents); BoneRefPositions = MoveTemp(Joints);
	Bounds = FBox(Box);
	DataGuid = FGuid::NewGuid();
#if WITH_EDITORONLY_DATA
	if (!AssetImportData) AssetImportData = NewObject<UAssetImportData>(this, TEXT("AssetImportData"));
	AssetImportData->Update(Path);
#endif
	UE_LOG(LogSkinnedSplatData, Display, TEXT("%s: %d splats, %d bones"), *Path, NumSplats, BoneNames.Num());
	return true;
}

void USkinnedSplatData::PostInitProperties()
{
	Super::PostInitProperties();
#if WITH_EDITORONLY_DATA
	if (!HasAnyFlags(RF_ClassDefaultObject | RF_NeedLoad) && !AssetImportData)
	{
		AssetImportData = NewObject<UAssetImportData>(this, TEXT("AssetImportData"));
	}
#endif
}

void USkinnedSplatData::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
	Rest.BulkSerialize(Ar);
	if (Ar.IsLoading())
	{
		DataGuid = FGuid::NewGuid();
	}
}

#if WITH_EDITOR
void USkinnedSplatData::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
	if (AssetImportData)
	{
		Context.AddTag(FAssetRegistryTag(SourceFileTagName(), AssetImportData->GetSourceData().ToJson(), FAssetRegistryTag::TT_Hidden));
	}
	Super::GetAssetRegistryTags(Context);
}
#endif
