// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatFactory.h"
#include "GaussianSplatImportOptions.h"
#include "GaussianSplatImportValidation.h"
#include "GaussianSplat.h"
#include "GaussianSplatBuilder.h"
#include "GaussianSplatCommon.h"
#include "GaussianSplatLevelOfDetailBuilder.h"
#include "GaussianSplatMortonOrder.h"
#include "GaussianSplatRenderResource.h"

#include "Async/ParallelFor.h"
#include "Internationalization/Text.h"
#include "EditorFramework/AssetImportData.h"
#include "Misc/App.h"
#include "Misc/FeedbackContext.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Input/SButton.h"
#include "Editor.h"
#include "PropertyEditorModule.h"
#include "IDetailsView.h"

THIRD_PARTY_INCLUDES_START
#include "zlib.h"
#if GAUSSIANSPLAT_WITH_ZSTD
#include "zstd.h"
#endif
THIRD_PARTY_INCLUDES_END

#include UE_INLINE_GENERATED_CPP_BY_NAME(GaussianSplatFactory)

namespace GaussianSplatFactoryPrivate
{
	constexpr EParallelForFlags ParallelForFlags = EParallelForFlags::None;
	//constexpr EParallelForFlags ParallelForFlags = EParallelForFlags::ForceSingleThread;

	float Sigmoid(float x)
	{
		return 1.0f / (1.0f + exp(-x));
	}

	float InverseSigmoid(float x)
	{
		return logf(x / (1.0f - x));
	}

	FVector3f LogToLinear(const FVector3f& Value)
	{
		return FVector3f(FMath::Exp(Value.X), FMath::Exp(Value.Y), FMath::Exp(Value.Z));
	}

	TArray<uint8> Decompress(TConstArrayView<uint8> CompressedData)
	{
		const int32 WindowSize = 16 + MAX_WBITS;

		z_stream Stream = {};
		Stream.next_in = const_cast<Bytef*>(CompressedData.GetData());
		Stream.avail_in = CompressedData.Num();

		if (inflateInit2(&Stream, WindowSize) != Z_OK)
		{
			return {};
		}

		TArray<uint8> UncompressedData;
		UncompressedData.Reserve(CompressedData.Num());

		const int32 ChunkSize = 8*1024;
		while (true)
		{
			const int32 OutLocation = UncompressedData.AddUninitialized(ChunkSize);

			Stream.next_out = UncompressedData.GetData() + OutLocation;
			Stream.avail_out = ChunkSize;

			const int32 InflateResult = inflate(&Stream, Z_NO_FLUSH);
			if (InflateResult != Z_OK && InflateResult != Z_STREAM_END)
			{
				UncompressedData.Empty();
				break;
			}

			UncompressedData.SetNum(OutLocation + ChunkSize - Stream.avail_out);

			if (InflateResult == Z_STREAM_END)
			{
				break;
			}
		}
		inflateEnd(&Stream);
		return UncompressedData;
	}

	// Resolve which coordinate space we are going to import into
	EGaussianSplatCoordinateSpace ToConcreteCoordinateSpace(EGaussianSplatImportCoordinateSpace S)
	{
		switch (S)
		{
			case EGaussianSplatImportCoordinateSpace::None:   return EGaussianSplatCoordinateSpace::LH_FRU;
			case EGaussianSplatImportCoordinateSpace::RH_RUB: return EGaussianSplatCoordinateSpace::RH_RUB;
			case EGaussianSplatImportCoordinateSpace::RH_RDF: return EGaussianSplatCoordinateSpace::RH_RDF;
			case EGaussianSplatImportCoordinateSpace::Automatic:
			default:
				checkNoEntry();
				return EGaussianSplatCoordinateSpace::LH_FRU;
		}
	}

	void ApplyImportOptions(const UGaussianSplatImportOptions* ImportOptions, FGaussianSplatBuilder& SplatBuilder, EGaussianSplatImportCoordinateSpace FileCoordinateSpace)
	{
		// Resolve which coordinate space we are going to import into
		const EGaussianSplatImportCoordinateSpace ImportCoordinateSpace = ImportOptions->CoordinateSpace == EGaussianSplatImportCoordinateSpace::Automatic ? FileCoordinateSpace : ImportOptions->CoordinateSpace;
		const float ImportScale = ImportOptions->ImportScale;

		const int32 NumSplats = SplatBuilder.GetNumSplats();

		SplatBuilder.CoordinateSpace = ToConcreteCoordinateSpace(ImportCoordinateSpace);
		switch (SplatBuilder.CoordinateSpace)
		{
			case EGaussianSplatCoordinateSpace::RH_RUB:
			{
				ParallelFor(
					NumSplats,
					[&](int32 iSplat)
					{
						FGaussianSplatFormatFloat::FBaseData& Splat = SplatBuilder.BaseData[iSplat];
						Splat.Position	= FVector3f(-Splat.Position.Z, Splat.Position.X, Splat.Position.Y) * ImportScale;
						Splat.Scale		= FVector3f(FMath::Exp(Splat.Scale.Z), FMath::Exp(Splat.Scale.X), FMath::Exp(Splat.Scale.Y)) * ImportScale;
						Splat.Rotation	= FQuat4f(-Splat.Rotation.Z, Splat.Rotation.X, Splat.Rotation.Y, -Splat.Rotation.W).GetNormalized();
						Splat.Opacity	= FMath::Clamp(Sigmoid(Splat.Opacity), 0.0f, 1.0f);
					}
				);
				break;
			}

			case EGaussianSplatCoordinateSpace::RH_RDF:
			{
				ParallelFor(
					NumSplats,
					[&](int32 iSplat)
					{
						FGaussianSplatFormatFloat::FBaseData& Splat = SplatBuilder.BaseData[iSplat];
						Splat.Position	= FVector3f(Splat.Position.Z, Splat.Position.X, -Splat.Position.Y) * ImportScale;
						Splat.Scale		= FVector3f(FMath::Exp(Splat.Scale.Z), FMath::Exp(Splat.Scale.X), FMath::Exp(Splat.Scale.Y)) * ImportScale;
						Splat.Rotation	= FQuat4f(Splat.Rotation.Z, Splat.Rotation.X, -Splat.Rotation.Y, -Splat.Rotation.W).GetNormalized();
						Splat.Opacity	= FMath::Clamp(Sigmoid(Splat.Opacity), 0.0f, 1.0f);
					}
				);
				break;
			}

			case EGaussianSplatCoordinateSpace::LH_FRU:
			{
				ParallelFor(
					NumSplats,
					[&](int32 iSplat)
					{
						FGaussianSplatFormatFloat::FBaseData& Splat = SplatBuilder.BaseData[iSplat];
						Splat.Position	= Splat.Position * ImportScale;
						Splat.Scale		= FVector3f(FMath::Exp(Splat.Scale.X), FMath::Exp(Splat.Scale.Y), FMath::Exp(Splat.Scale.Z)) * ImportScale;
						Splat.Rotation	= Splat.Rotation.GetNormalized();
						Splat.Opacity	= FMath::Clamp(Sigmoid(Splat.Opacity), 0.0f, 1.0f);
					}
				);
				break;
			}
		}

		// Set the SH degree we should use
		if (ImportOptions->SHMaxDegree.IsSet())
		{
			SplatBuilder.SetSHDegree(ImportOptions->SHMaxDegree.GetValue());
		}
	}

	void DecodeSPZPositions(TConstArrayView<uint8> Bytes, FGaussianSplatBuilder& SplatBuilder, uint32 NumSplats, uint8 FractionalBits)
	{
		const float PositionScale = 1.0f / (1 << FractionalBits);
		uint64 CurrentOffset = 0;
		for (uint32 iSplat = 0; iSplat < NumSplats; ++iSplat)
		{
			FGaussianSplatFormatFloat::FBaseData& Splat = SplatBuilder.BaseData[iSplat];
			for (int32 iComponent = 0; iComponent < 3; ++iComponent)
			{
				int32 Value;
				Value  = (Bytes[CurrentOffset++] << 0);
				Value |= (Bytes[CurrentOffset++] << 8);
				Value |= (Bytes[CurrentOffset++] << 16);
				Value |= (Value & 0x800000) ? 0xff000000 : 0;
				Splat.Position[iComponent] = float(Value) * PositionScale;
			}
		}
	}

	void DecodeSPZAlphas(TConstArrayView<uint8> Bytes, FGaussianSplatBuilder& SplatBuilder, uint32 NumSplats)
	{
		for (uint32 iSplat = 0; iSplat < NumSplats; ++iSplat)
		{
			FGaussianSplatFormatFloat::FBaseData& Splat = SplatBuilder.BaseData[iSplat];
			const int32 Value = Bytes[iSplat];
			Splat.Opacity = InverseSigmoid(float(Value) / 255.0f);
		}
	}

	void DecodeSPZColors(TConstArrayView<uint8> Bytes, FGaussianSplatBuilder& SplatBuilder, uint32 NumSplats, uint32 SHDimension)
	{
		constexpr float ColorScale = 0.15f;
		uint64 CurrentOffset = 0;
		for (uint32 iSplat = 0; iSplat < NumSplats; ++iSplat)
		{
			FVector3f& SHZeroOrder = SplatBuilder.SHData[iSplat * SHDimension];
			SHZeroOrder.X = ((float(Bytes[CurrentOffset++]) / 255.0f) - 0.5f) / ColorScale;
			SHZeroOrder.Y = ((float(Bytes[CurrentOffset++]) / 255.0f) - 0.5f) / ColorScale;
			SHZeroOrder.Z = ((float(Bytes[CurrentOffset++]) / 255.0f) - 0.5f) / ColorScale;
		}
	}

	void DecodeSPZScales(TConstArrayView<uint8> Bytes, FGaussianSplatBuilder& SplatBuilder, uint32 NumSplats)
	{
		uint64 CurrentOffset = 0;
		for (uint32 iSplat = 0; iSplat < NumSplats; ++iSplat)
		{
			FGaussianSplatFormatFloat::FBaseData& Splat = SplatBuilder.BaseData[iSplat];
			for (int32 iComponent = 0; iComponent < 3; ++iComponent)
			{
				const int32 Value = Bytes[CurrentOffset++];
				Splat.Scale[iComponent] = float(Value) / 16.0f - 10.0f;
			}
		}
	}

	void DecodeSPZRotations(TConstArrayView<uint8> Bytes, FGaussianSplatBuilder& SplatBuilder, uint32 NumSplats, uint32 Version)
	{
		uint64 CurrentOffset = 0;
		if (Version == 2)
		{
			for (uint32 iSplat = 0; iSplat < NumSplats; ++iSplat)
			{
				FGaussianSplatFormatFloat::FBaseData& Splat = SplatBuilder.BaseData[iSplat];
				Splat.Rotation.X = float(Bytes[CurrentOffset++]) * (1.0f / 127.5f) - 1.0f;
				Splat.Rotation.Y = float(Bytes[CurrentOffset++]) * (1.0f / 127.5f) - 1.0f;
				Splat.Rotation.Z = float(Bytes[CurrentOffset++]) * (1.0f / 127.5f) - 1.0f;
				const float LenXYZ = (Splat.Rotation.X * Splat.Rotation.X) + (Splat.Rotation.Y * Splat.Rotation.Y) + (Splat.Rotation.Z * Splat.Rotation.Z);
				Splat.Rotation.W = FMath::Sqrt(FMath::Max(0.0f, 1.0f - LenXYZ));
			}
			return;
		}

		const auto UnpackQuat =
			[](uint32 Packed) -> FQuat4f
			{
				constexpr uint32 CMask = (1u << 9u) - 1u;
				constexpr float RcpSqrt2 = (float)0.707106781186547524401; // 1/sqrt(2)
				const uint32 iLargestComponent = Packed >> 30u;

				FQuat4f Quat;
				float* QuatComponents[] = { &Quat.X, &Quat.Y, &Quat.Z, &Quat.W };

				float SumSquares = 0.0f;
				for (int32 iComponent = 3; iComponent >= 0; --iComponent)
				{
					if (iComponent != iLargestComponent)
					{
						const uint32 Mag = Packed & CMask;
						const uint32 Neg = (Packed >> 9u) & 1u;
						Packed = Packed >> 10u;
						float Value = RcpSqrt2 * (float(Mag) / float(CMask));
						Value = Neg ? -Value : Value;
						SumSquares += Value * Value;

						*QuatComponents[iComponent] = Value;
					}
				}
				*QuatComponents[iLargestComponent] = FMath::Sqrt(1.0f - SumSquares);
				return Quat;
			};

		for (uint32 iSplat = 0; iSplat < NumSplats; ++iSplat)
		{
			FGaussianSplatFormatFloat::FBaseData& Splat = SplatBuilder.BaseData[iSplat];
			uint32 PackedQuat;
			PackedQuat  = (Bytes[CurrentOffset++] << 0);
			PackedQuat |= (Bytes[CurrentOffset++] << 8);
			PackedQuat |= (Bytes[CurrentOffset++] << 16);
			PackedQuat |= (Bytes[CurrentOffset++] << 24);
			Splat.Rotation = UnpackQuat(PackedQuat);
		}
	}

	void DecodeSPZSphericalHarmonics(TConstArrayView<uint8> Bytes, FGaussianSplatBuilder& SplatBuilder, uint32 NumSplats, uint32 SHDimension)
	{
		// SHDimension == 1 means degree 0 (DC only, already filled by DecodeSPZColors), nothing to read.
		if (SHDimension <= 1)
		{
			return;
		}
		const auto UnquantizeSH = [](uint8 x) { return (static_cast<float>(x) - 128.0f) / 128.0f; };
		uint64 CurrentOffset = 0;
		for (uint32 iSplat = 0; iSplat < NumSplats; ++iSplat)
		{
			// +1 because the DC term (zero order) is already loaded.
			uint32 SHWriteOffset = (SHDimension * iSplat) + 1;
			for (uint32 iDimension = 1; iDimension < SHDimension; ++iDimension)
			{
				SplatBuilder.SHData[SHWriteOffset].X = UnquantizeSH(Bytes[CurrentOffset++]);
				SplatBuilder.SHData[SHWriteOffset].Y = UnquantizeSH(Bytes[CurrentOffset++]);
				SplatBuilder.SHData[SHWriteOffset].Z = UnquantizeSH(Bytes[CurrentOffset++]);
				++SHWriteOffset;
			}
		}
	}

	bool ReadSPZv3File(const UGaussianSplatImportOptions* ImportOptions, FGaussianSplatBuilder& SplatBuilder, TConstArrayView64<uint8> InCompressedData, FFeedbackContext* FeedbackContext)
	{
		struct FSPZv3Header
		{
			uint32	Magic;
			uint32	Version;
			uint32	NumSplats;
			uint8	SHDegree;
			uint8	FractionalBits;
			uint8	Flags;
			uint8	Reserved;
		};

		const TArray<uint8> InData = Decompress(InCompressedData);
		if (InData.Num() < sizeof(FSPZv3Header))
		{
			FeedbackContext->Logf(ELogVerbosity::Error, TEXT("Invalid SPZ file, file too small."));
			return false;
		}

		FSPZv3Header Header;
		FMemory::Memcpy(&Header, InData.GetData(), sizeof(Header));
		if (Header.Magic != 0x5053474e)
		{
			FeedbackContext->Logf(ELogVerbosity::Error, TEXT("Invalid SPZ file, header magic invalid."));
			return false;
		}

		if (Header.Version < 2 || Header.Version > 3)
		{
			FeedbackContext->Logf(ELogVerbosity::Error, TEXT("SPZ Version (%d) is not supported"), Header.Version);
			return false;
		}

		SplatBuilder.SHDegree = Header.SHDegree;
		const uint32 SHDimension = SplatBuilder.GetSHDimension();
		if (SHDimension == 0)
		{
			FeedbackContext->Logf(ELogVerbosity::Error, TEXT("SPZ Dimension (%d) is not supported"), SHDimension);
			return false;
		}

		// Carve the decompressed payload into per-attribute byte ranges.
		// File order: Positions -> Alphas -> Colors -> Scales -> Rotations -> SH.
		const uint64 PositionsSize = uint64(Header.NumSplats) * 9;
		const uint64 AlphasSize    = uint64(Header.NumSplats);
		const uint64 ColorsSize    = uint64(Header.NumSplats) * 3;
		const uint64 ScalesSize    = uint64(Header.NumSplats) * 3;
		const uint64 RotationsSize = uint64(Header.NumSplats) * (Header.Version == 2 ? 3 : 4);
		const uint64 SHSize        = SHDimension > 1 ? (uint64(Header.NumSplats) * (SHDimension - 1) * 3) : 0;

		uint64 CurrentOffset = sizeof(FSPZv3Header);
		const uint64 RequiredBytes = CurrentOffset + PositionsSize + AlphasSize + ColorsSize + ScalesSize + RotationsSize + SHSize;
		if (uint64(InData.Num()) < RequiredBytes)
		{
			FeedbackContext->Logf(ELogVerbosity::Error, TEXT("Truncated SPZ v%d payload (have %llu bytes, need %llu)."), Header.Version, uint64(InData.Num()), RequiredBytes);
			return false;
		}

		SplatBuilder.BaseData.AddDefaulted(Header.NumSplats);
		SplatBuilder.SHData.AddZeroed(Header.NumSplats * SHDimension);

		const auto Slice = [&](uint64 Size) -> TConstArrayView<uint8>
			{
				TConstArrayView<uint8> View(InData.GetData() + CurrentOffset, int32(Size));
				CurrentOffset += Size;
				return View;
			};

		DecodeSPZPositions(Slice(PositionsSize), SplatBuilder, Header.NumSplats, Header.FractionalBits);
		DecodeSPZAlphas(Slice(AlphasSize), SplatBuilder, Header.NumSplats);
		DecodeSPZColors(Slice(ColorsSize), SplatBuilder, Header.NumSplats, SHDimension);
		DecodeSPZScales(Slice(ScalesSize), SplatBuilder, Header.NumSplats);
		DecodeSPZRotations(Slice(RotationsSize), SplatBuilder, Header.NumSplats, Header.Version);
		DecodeSPZSphericalHarmonics(Slice(SHSize), SplatBuilder, Header.NumSplats, SHDimension);

		return true;
	}

	bool ReadSPZv4File(const UGaussianSplatImportOptions* ImportOptions, FGaussianSplatBuilder& SplatBuilder, TConstArrayView64<uint8> InRawData, FFeedbackContext* FeedbackContext)
	{
#if !GAUSSIANSPLAT_WITH_ZSTD
		FeedbackContext->Logf(ELogVerbosity::Error, TEXT("SPZ v4 import requires zstd, which is not available on this platform."));
		return false;
#else
		struct FSPZv4Header
		{
			uint32	Magic;
			uint32	Version;
			uint32	NumSplats;
			uint8	SHDegree;
			uint8	FractionalBits;
			uint8	Flags;
			uint8	NumStreams;
			uint32	TocByteOffset;
			uint8	Reserved[12];
		};
		static_assert(sizeof(FSPZv4Header) == 32, "SPZ v4 header must be 32 bytes");

		if (InRawData.Num() < int64(sizeof(FSPZv4Header)))
		{
			FeedbackContext->Logf(ELogVerbosity::Error, TEXT("Invalid SPZ v4 file, file too small for header."));
			return false;
		}

		FSPZv4Header Header;
		FMemory::Memcpy(&Header, InRawData.GetData(), sizeof(Header));
		if (Header.Magic != 0x5053474e || Header.Version != 4)
		{
			FeedbackContext->Logf(ELogVerbosity::Error, TEXT("Invalid SPZ v4 header (magic 0x%08x, version %u)."), Header.Magic, Header.Version);
			return false;
		}

		// Stream Order:
		// Required: Positions -> Alphas -> Colors -> Scales -> Rotations
		// Optional: SH
		constexpr uint8 RequiredStreams = 5;
		constexpr uint8 SHStreamIndex   = 5;
		constexpr uint8 ExpectedStreams = SHStreamIndex + 1;
		if (Header.NumStreams < RequiredStreams)
		{
			FeedbackContext->Logf(ELogVerbosity::Error, TEXT("SPZ v4 has %u streams, expected at least %u."), Header.NumStreams, RequiredStreams);
			return false;
		}

		SplatBuilder.SHDegree = Header.SHDegree;
		const uint32 SHDimension = SplatBuilder.GetSHDimension();
		if (SHDimension == 0)
		{
			FeedbackContext->Logf(ELogVerbosity::Error, TEXT("SPZ Dimension (%d) is not supported"), SHDimension);
			return false;
		}

		const bool bHasSHStream = (SHDimension > 1) && (Header.NumStreams > SHStreamIndex);
		if (SHDimension > 1 && !bHasSHStream)
		{
			FeedbackContext->Logf(ELogVerbosity::Error, TEXT("SPZ v4 file with SH degree %d is missing the SH stream (%u streams present)."), Header.SHDegree, Header.NumStreams);
			return false;
		}

		const uint64 TocOffset = uint64(Header.TocByteOffset);
		if (TocOffset < sizeof(FSPZv4Header) || TocOffset > uint64(InRawData.Num()))
		{
			FeedbackContext->Logf(ELogVerbosity::Error, TEXT("SPZ v4 TocByteOffset (%llu) is out of range."), TocOffset);
			return false;
		}

		struct FStreamEntry
		{
			uint64 CompressedSize;
			uint64 UncompressedSize;
		};
		const uint64 TocBytes = uint64(Header.NumStreams) * sizeof(FStreamEntry);
		if (uint64(InRawData.Num()) < TocOffset + TocBytes)
		{
			FeedbackContext->Logf(ELogVerbosity::Error, TEXT("SPZ v4 file truncated at TOC."));
			return false;
		}

		TArray<FStreamEntry> Toc;
		Toc.SetNumUninitialized(Header.NumStreams);
		FMemory::Memcpy(Toc.GetData(), InRawData.GetData() + TocOffset, TocBytes);

		// Expected uncompressed sizes for each of the first 6 streams.
		const uint64 ExpectedSizes[ExpectedStreams] =
		{
			uint64(Header.NumSplats) * 9,
			uint64(Header.NumSplats),
			uint64(Header.NumSplats) * 3,
			uint64(Header.NumSplats) * 3,
			uint64(Header.NumSplats) * 4,
			SHDimension > 1 ? uint64(Header.NumSplats) * (SHDimension - 1) * 3 : 0,
		};

		TArray<TArray<uint8>> Streams;
		Streams.SetNum(Header.NumStreams);

		uint64 StreamOffset = TocOffset + TocBytes;
		for (uint32 iStream = 0; iStream < Header.NumStreams; ++iStream)
		{
			const FStreamEntry& Entry = Toc[iStream];
			if (StreamOffset + Entry.CompressedSize > uint64(InRawData.Num()))
			{
				FeedbackContext->Logf(ELogVerbosity::Error, TEXT("SPZ v4 stream %u extends past end of file."), iStream);
				return false;
			}

			if (iStream < ExpectedStreams && Entry.UncompressedSize != ExpectedSizes[iStream])
			{
				FeedbackContext->Logf(ELogVerbosity::Error, TEXT("SPZ v4 stream %u has uncompressed size %llu, expected %llu."), iStream, Entry.UncompressedSize, ExpectedSizes[iStream]);
				return false;
			}

			if (Entry.UncompressedSize > 0)
			{
				Streams[iStream].SetNumUninitialized(int64(Entry.UncompressedSize));
				const size_t Decompressed = ZSTD_decompress(Streams[iStream].GetData(), size_t(Entry.UncompressedSize), InRawData.GetData() + StreamOffset, size_t(Entry.CompressedSize));
				if (ZSTD_isError(Decompressed) || Decompressed != size_t(Entry.UncompressedSize))
				{
					FeedbackContext->Logf(ELogVerbosity::Error, TEXT("SPZ v4 stream %u ZSTD decompress failed."), iStream);
					return false;
				}
			}
			StreamOffset += Entry.CompressedSize;
		}

		SplatBuilder.BaseData.AddDefaulted(Header.NumSplats);
		SplatBuilder.SHData.AddZeroed(Header.NumSplats * SHDimension);

		DecodeSPZPositions(Streams[0], SplatBuilder, Header.NumSplats, Header.FractionalBits);
		DecodeSPZAlphas(Streams[1], SplatBuilder, Header.NumSplats);
		DecodeSPZColors(Streams[2], SplatBuilder, Header.NumSplats, SHDimension);
		DecodeSPZScales(Streams[3], SplatBuilder, Header.NumSplats);
		DecodeSPZRotations(Streams[4], SplatBuilder, Header.NumSplats, /*Version=*/3);
		if (bHasSHStream)
		{
			DecodeSPZSphericalHarmonics(Streams[SHStreamIndex], SplatBuilder, Header.NumSplats, SHDimension);
		}

		return true;
#endif // GAUSSIANSPLAT_WITH_ZSTD
	}

	bool ReadSPZFile(const UGaussianSplatImportOptions* ImportOptions, FGaussianSplatBuilder& SplatBuilder, TConstArrayView64<uint8> InRawData, FFeedbackContext* FeedbackContext)
	{
		// Detect SPZ4 format
		if (InRawData.Num() >= 4 && InRawData[0] == 0x4e && InRawData[1] == 0x47 && InRawData[2] == 0x53 && InRawData[3] == 0x50)
		{
			return ReadSPZv4File(ImportOptions, SplatBuilder, InRawData, FeedbackContext);
		}
		return ReadSPZv3File(ImportOptions, SplatBuilder, InRawData, FeedbackContext);
	}

	bool ReadPLYFile(const UGaussianSplatImportOptions* ImportOptions, FGaussianSplatBuilder& SplatBuilder, TConstArrayView64<uint8> InData, FFeedbackContext* FeedbackContext)
	{
		int32 CurrentOffset = 0;
		const auto GetNextLine =
			[&CurrentOffset, &InData]() -> FAnsiStringView
			{
				const int32 StartOffset = CurrentOffset;
				while (CurrentOffset < InData.Num())
				{
					if (InData[CurrentOffset] == '\n')
					{
						FAnsiStringView Ret(reinterpret_cast<const char*>(InData.GetData()) + StartOffset, CurrentOffset - StartOffset);
						++CurrentOffset;
						return Ret;
					}
					++CurrentOffset;
				}
				return {};
			};

		// PLY magic must be the first line
		if (GetNextLine() != "ply")
		{
			FeedbackContext->Logf(ELogVerbosity::Error, TEXT("Not a valid PLY file"));
			return false;
		}

		// Parse remaining header
		bool bFoundFormat = false;
		bool bFoundVertexElement = false;
		bool bFoundEndHeader = false;
		int32 NumSplats = 0;
		TMap<FAnsiString, uint64> PropertyDataOffsets;
		uint64 PropertyDataStride = 0;
		while (CurrentOffset < InData.Num())
		{
			FAnsiString Line(GetNextLine());
			if (Line == "end_header")
			{
				bFoundEndHeader = true;
				break;
			}
			else if (Line == "format binary_little_endian 1.0")
			{
				bFoundFormat = true;
			}
			else if (Line.RemoveFromStart("element vertex "))
			{
				NumSplats = FCStringAnsi::Atoi(*Line);
				bFoundVertexElement = true;
			}
			else if (Line.RemoveFromStart("property float "))
			{
				PropertyDataOffsets.Emplace(Line, PropertyDataStride);
				PropertyDataStride += sizeof(float);
			}
			else if (Line.StartsWith("comment"))
			{
				// Ignore comment lines
			}
			else
			{
				FeedbackContext->Logf(ELogVerbosity::Error, TEXT("Unknown header line (%S)"), *Line);
				return false;
			}
		}

		if (!bFoundEndHeader)
		{
			FeedbackContext->Logf(ELogVerbosity::Error, TEXT("Unexpected end of file, missing 'end_header'."));
			return false;
		}
		if (!bFoundFormat)
		{
			FeedbackContext->Logf(ELogVerbosity::Error, TEXT("Only binary little endian 1.0 format is currently supported"));
			return false;
		}
		if (!bFoundVertexElement || NumSplats < 0)
		{
			FeedbackContext->Logf(ELogVerbosity::Error, TEXT("Expected valid 'element vertex' in header but not found."));
			return false;
		}

		// A malformed file can declare more vertices than it actually holds.
		const uint64 RequiredBytes = uint64(CurrentOffset) + uint64(NumSplats) * PropertyDataStride;
		if (uint64(InData.Num()) < RequiredBytes)
		{
			FeedbackContext->Logf(ELogVerbosity::Error, TEXT("Malformed PLY file (%llu bytes allocated, but %llu are required for %d vertices)."), uint64(InData.Num()), RequiredBytes, NumSplats);
			return false;
		}

		const auto ReadFloats =
			[&](FAnsiString PropertyName, uint64 WriteOffset)
			{
				if (PropertyDataOffsets.Contains(PropertyName))
				{
					const uint64 PropertyDataOffset = CurrentOffset + PropertyDataOffsets[PropertyName];
					ParallelFor(
						NumSplats,
						[&](int32 iSplat)
						{
							const uint64 ReadOffset = PropertyDataOffset + (iSplat * PropertyDataStride);
							uint8* Dst = reinterpret_cast<uint8*>(&SplatBuilder.BaseData[iSplat]) + WriteOffset;
							FMemory::Memcpy(Dst, InData.GetData() + ReadOffset, sizeof(float));
						},
						GaussianSplatFactoryPrivate::ParallelForFlags
					);
				}
			};

		// Import base data
		SplatBuilder.BaseData.AddDefaulted(NumSplats);
		ReadFloats("x", offsetof(FGaussianSplatFormatFloat::FBaseData, Position.X));
		ReadFloats("y", offsetof(FGaussianSplatFormatFloat::FBaseData, Position.Y));
		ReadFloats("z", offsetof(FGaussianSplatFormatFloat::FBaseData, Position.Z));
		ReadFloats("opacity", offsetof(FGaussianSplatFormatFloat::FBaseData, Opacity));
		ReadFloats("scale_0", offsetof(FGaussianSplatFormatFloat::FBaseData, Scale.X));
		ReadFloats("scale_1", offsetof(FGaussianSplatFormatFloat::FBaseData, Scale.Y));
		ReadFloats("scale_2", offsetof(FGaussianSplatFormatFloat::FBaseData, Scale.Z));
		ReadFloats("rot_0", offsetof(FGaussianSplatFormatFloat::FBaseData, Rotation.W));
		ReadFloats("rot_1", offsetof(FGaussianSplatFormatFloat::FBaseData, Rotation.X));
		ReadFloats("rot_2", offsetof(FGaussianSplatFormatFloat::FBaseData, Rotation.Y));
		ReadFloats("rot_3", offsetof(FGaussianSplatFormatFloat::FBaseData, Rotation.Z));

		// Import SH data (if present)
		SplatBuilder.SHDegree = 0;
		{
			// Find Available Coefs (we assume dc is valid)
			TArray<uint64> SHCoefOffsets;
			SHCoefOffsets.Emplace(PropertyDataOffsets.FindOrAdd("f_dc_0", -1));
			SHCoefOffsets.Emplace(PropertyDataOffsets.FindOrAdd("f_dc_1", -1));
			SHCoefOffsets.Emplace(PropertyDataOffsets.FindOrAdd("f_dc_2", -1));

			while (SHCoefOffsets.Num() < 16 * 3)
			{
				const FAnsiString CoefName = FAnsiString::Printf("f_rest_%d", SHCoefOffsets.Num() - 3);
				const uint64* CoefOffset = PropertyDataOffsets.Find(CoefName);
				if (CoefOffset == nullptr)
				{
					break;
				}
				SHCoefOffsets.Add(*CoefOffset);
			}

			// Determine SH degree
			if (SHCoefOffsets.Num() >= 4 * 3)
			{
				SplatBuilder.SHDegree = 1;
				if (SHCoefOffsets.Num() >= 9 * 3)
				{
					SplatBuilder.SHDegree = 2;
					if (SHCoefOffsets.Num() >= 16 * 3)
					{
						SplatBuilder.SHDegree = 3;
					}
				}
			}

			// Read coefs
			const int32 SHDimension = SplatBuilder.GetSHDimension();

			const auto ReadCoefValue =
				[&](int32 iSplat, int32 iCoef) -> float
				{
					float Value = 0.0f;
					if (SHCoefOffsets[iCoef] != -1)
					{
						const uint64 ReadOffset = CurrentOffset + SHCoefOffsets[iCoef] + (iSplat * PropertyDataStride);
						FMemory::Memcpy(&Value, InData.GetData() + ReadOffset, sizeof(float));
					}
					return Value;
				};

			SplatBuilder.SHData.SetNum(SHDimension * NumSplats);

			ParallelFor(
				NumSplats,
				[&](int32 iSplat)
				{
					// DC component (always interleaved)
					SplatBuilder.SHData[iSplat * SHDimension + 0].X = ReadCoefValue(iSplat, 0);
					SplatBuilder.SHData[iSplat * SHDimension + 0].Y = ReadCoefValue(iSplat, 1);
					SplatBuilder.SHData[iSplat * SHDimension + 0].Z = ReadCoefValue(iSplat, 2);

					// Higher order SH coefficients (non-interleaved: all R, then all G, then all B)
					if (SHDimension > 1)
					{
						const int32 NumRestCoefs = SHDimension - 1;
						const int32 ROffset = 3;
						const int32 GOffset = ROffset + NumRestCoefs;
						const int32 BOffset = GOffset + NumRestCoefs;

						for (int32 iCoef = 1; iCoef < SHDimension; ++iCoef)
						{
							SplatBuilder.SHData[iSplat * SHDimension + iCoef].X = ReadCoefValue(iSplat, ROffset + (iCoef - 1));
							SplatBuilder.SHData[iSplat * SHDimension + iCoef].Y = ReadCoefValue(iSplat, GOffset + (iCoef - 1));
							SplatBuilder.SHData[iSplat * SHDimension + iCoef].Z = ReadCoefValue(iSplat, BOffset + (iCoef - 1));
						}
					}
				},
				GaussianSplatFactoryPrivate::ParallelForFlags
			);
		}

		return true;
	}

	// The per-SH-degree runtime splat budget for the format, counting every LOD level
	FString CreateMaxSplatCountReportString(bool bQuantizedFormat)
	{
		FString Report;
		for (int32 Degree = 0; Degree <= GaussianSplatCommon::MaxSupportedSHDegree; ++Degree)
		{
			Report += FString::Printf(TEXT("\n  Runtime splat budget at SH=%d: %s"), Degree,
				*FText::AsNumber(FGaussianSplatBuilder::GetMaxSplatCount(GaussianSplatCommon::GetSHDimension(Degree), bQuantizedFormat)).ToString());
		}
		return Report;
	}
}

bool GaussianSplat::ValidateImportSize(const FGaussianSplatImportSize& ImportSize, FName AssetName, FFeedbackContext& FeedbackContext)
{
	if (ImportSize.SourceSplatCount <= 0)
	{
		FeedbackContext.Logf(ELogVerbosity::Error, TEXT("Import failure. Asset %s contains no splats."), *AssetName.ToString());
		return false;
	}

	const int32 SHDimension = GaussianSplatCommon::GetSHDimension(ImportSize.SHDegree);
	const int64 MaxSplatCount = FGaussianSplatBuilder::GetMaxSplatCount(SHDimension, ImportSize.bQuantizedFormat);
	const int64 RuntimeSplatCount = BoundLevelOfDetailSplatCount(ImportSize.SourceSplatCount, GaussianSplatCommon::MinSplatsPerCluster);
	if (RuntimeSplatCount > MaxSplatCount)
	{
		const uint64 SplatStride = FGaussianSplatRenderResource::GetSplatStrideForFormat(SHDimension, ImportSize.bQuantizedFormat);
		const uint64 BufferBytes = uint64(RuntimeSplatCount) * SplatStride;
		const uint64 MaxBufferBytes = FGaussianSplatBuilder::GetMaxBufferBytes();
		FeedbackContext.Logf(ELogVerbosity::Error,
			TEXT("Import failure. Asset %s (splats=%s at SH=%d) builds %s runtime splats across its LOD levels, requiring %s (%s maximum allowed). ")
			TEXT("Consider importing with fewer splats or a lower SH value.%s"),
			*AssetName.ToString(), *FText::AsNumber(ImportSize.SourceSplatCount).ToString(), ImportSize.SHDegree,
			*FText::AsNumber(RuntimeSplatCount).ToString(),
			*FText::AsMemory(BufferBytes, EMemoryUnit::Unit_MiB).ToString(), *FText::AsMemory(MaxBufferBytes, EMemoryUnit::Unit_MiB).ToString(),
			*GaussianSplatFactoryPrivate::CreateMaxSplatCountReportString(ImportSize.bQuantizedFormat));
		return false;
	}
	return true;
}

UGaussianSplatFactory::UGaussianSplatFactory(const FObjectInitializer& ObjectInitializer)
{
	bCreateNew = false;
	bEditorImport = true;  // Allows drag-and-drop
	bText = false;         // Set to true if reading as a string instead of binary buffer

	SupportedClass = UGaussianSplat::StaticClass();

	Formats.Add(TEXT("ply;Gaussian PLY Format"));
	Formats.Add(TEXT("spz;Compressed Gaussian Format"));

	ImportPriority = DefaultImportPriority + 1;
}

UObject* UGaussianSplatFactory::FactoryCreateBinary(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, UObject* Context, const TCHAR* Type, const uint8*& Buffer, const uint8* BufferEnd, FFeedbackContext* Warn)
{
	const double StartSeconds = FPlatformTime::Seconds();
	ON_SCOPE_EXIT
	{
		const double DurationSections = FPlatformTime::Seconds() - StartSeconds;
		UE_LOGF(LogTemp, Log, "Splat import took %f seconds", DurationSections);
	};

	UGaussianSplat* SplatData = NewObject<UGaussianSplat>(InParent, InClass, InName, Flags);

	SplatData->GetAssetImportData()->Update(GetCurrentFilename());

	const UGaussianSplatImportOptions* Options = ImportOptions ? ImportOptions.Get() : GetDefault<UGaussianSplatImportOptions>();

	// SPZ format
	FGaussianSplatBuilder SplatBuilder;
	EGaussianSplatImportCoordinateSpace FormatCoordinateSpace = EGaussianSplatImportCoordinateSpace::None;

	if (Type == FString(TEXT("spz")))
	{
		if (GaussianSplatFactoryPrivate::ReadSPZFile(Options, SplatBuilder, MakeArrayView64(Buffer, BufferEnd - Buffer), Warn) == false)
		{
			Warn->Logf(ELogVerbosity::Warning, TEXT("Failed to import SPZ file"));
			return nullptr;
		}
		FormatCoordinateSpace = EGaussianSplatImportCoordinateSpace::RH_RUB;
	}
	// PLY format
	else if (Type == FString(TEXT("ply")))
	{
		if (GaussianSplatFactoryPrivate::ReadPLYFile(Options, SplatBuilder, MakeArrayView64(Buffer, BufferEnd - Buffer), Warn) == false)
		{
			return nullptr;
		}
		FormatCoordinateSpace = EGaussianSplatImportCoordinateSpace::RH_RDF;
	}

	// Calculate the bounds and set the data if it's valid a failure here is unlikely
	GaussianSplatFactoryPrivate::ApplyImportOptions(Options, SplatBuilder, FormatCoordinateSpace);
	SplatBuilder.CalculateBounds();

	// Gate on the format this asset will actually build, not on the largest possible one. The runtime buffer holds
	// every LOD level, and the cluster size can be edited after import, so bound with the smallest cluster size allowed.
	const FGaussianSplatImportSize ImportSize{ SplatBuilder.GetNumSplats(), SplatBuilder.SHDegree, SplatData->GetUseQuantizedFormat() };
	if (!GaussianSplat::ValidateImportSize(ImportSize, InName, *Warn))
	{
		return nullptr;
	}

	if (SplatBuilder.IsValid())
	{
		GaussianSplat::SortMortonOrder(SplatBuilder);
		SplatData->SetData(SplatBuilder);
		return SplatData;
	}

	return nullptr;
}

bool UGaussianSplatFactory::CanReimport(UObject* InObject, TArray<FString>& OutFilenames)
{
	UGaussianSplat* SplatData = Cast<UGaussianSplat>(InObject);
	if (SplatData && SplatData->GetAssetImportData())
	{
		SplatData->GetAssetImportData()->ExtractFilenames(OutFilenames);
		return true;
	}
	return false;
}

void UGaussianSplatFactory::SetReimportPaths(UObject* InObject, const TArray<FString>& NewReimportPaths)
{
	UGaussianSplat* SplatData = Cast<UGaussianSplat>(InObject);
	if (SplatData && ensure(SplatData->GetAssetImportData()) && ensure(NewReimportPaths.Num() == 1))
	{
		SplatData->GetAssetImportData()->UpdateFilenameOnly(NewReimportPaths[0]);
	}
}

EReimportResult::Type UGaussianSplatFactory::Reimport(UObject* InObject)
{
	UGaussianSplat* SplatData = Cast<UGaussianSplat>(InObject);
	if (!SplatData || !SplatData->GetAssetImportData())
	{
		return EReimportResult::Failed;
	}

	const FString Filename = SplatData->GetAssetImportData()->GetFirstFilename();
	if (Filename.IsEmpty() || !FPaths::FileExists(Filename))
	{
		return EReimportResult::Failed;
	}

	TArray<uint8> Data;
	if (FFileHelper::LoadFileToArray(Data, *Filename))
	{
		const uint8* Buffer = Data.GetData();
		const uint8* BufferEnd = Buffer + Data.Num();

		if (FactoryCreateBinary(SplatData->GetClass(), SplatData->GetOuter(), SplatData->GetFName(), SplatData->GetFlags(), nullptr, *FPaths::GetExtension(Filename), Buffer, BufferEnd, GWarn))
		{
			SplatData->GetAssetImportData()->Update(Filename);

			return EReimportResult::Succeeded;
		}
	}

	return  EReimportResult::Failed;
}

int32 UGaussianSplatFactory::GetPriority() const
{
	return ImportPriority;
}

bool UGaussianSplatFactory::ConfigureProperties()
{
	// Create options if we don't have them
	if (!ImportOptions)
	{
		ImportOptions = NewObject<UGaussianSplatImportOptions>(this);
	}

	// Automated / unattended import
	if (IsAutomatedImport() || GIsRunningUnattendedScript || FApp::IsUnattended())
	{
		return true;
	}

	// Create details view
	FPropertyEditorModule& PropertyEditorModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");

	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.bAllowSearch = false;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;

	TSharedPtr<IDetailsView> DetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);
	DetailsView->SetObject(ImportOptions);

	// Create dialog window
	bool bDoImport = false;

	TSharedRef<SWindow> Window =
		SNew(SWindow)
		.Title(FText::FromString("Gaussian Splat Import Options"))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMaximize(false)
		.SupportsMinimize(false);

	Window->SetContent(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.Padding(5)
		[
			DetailsView.ToSharedRef()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Right)
		.Padding(5)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(5, 0)
			[
				SNew(SButton)
				.Text(FText::FromString("Import"))
				.OnClicked_Lambda(
					[&bDoImport, &Window]()
					{
						bDoImport = true;
						Window->RequestDestroyWindow();
						return FReply::Handled();
					}
				)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(5, 0)
			[
				SNew(SButton)
				.Text(FText::FromString("Cancel"))
				.OnClicked_Lambda(
					[&bDoImport, &Window]()
					{
						bDoImport = false;
						Window->RequestDestroyWindow();
						return FReply::Handled();
					}
				)
			]
		]
	);

	GEditor->EditorAddModalWindow(Window);

	return bDoImport;
}
