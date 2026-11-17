#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SkinnedSplatActor.generated.h"

class USkeletalMeshComponent;
class UAnimSequence;
class USkinnedSplatData;
class FSkinnedSplatRenderState;

/**
 * A hidden skeletal mesh plays the animation; every frame its bones drive a GPU skinning pass over the rest-pose
 * splats, whose result goes to the GaussianSplat renderer as an external proxy (same path as Epic's Niagara writer).
 */
UCLASS()
class SKINNEDSPLAT_API ASkinnedSplatActor : public AActor
{
	GENERATED_BODY()
public:
	ASkinnedSplatActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Skinned Splat")
	TObjectPtr<USkeletalMeshComponent> Mesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skinned Splat")
	TObjectPtr<USkinnedSplatData> SplatData;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skinned Splat")
	TObjectPtr<UAnimSequence> Animation;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skinned Splat")
	float PlayRate = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skinned Splat")
	float PointScaleFactor = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skinned Splat")
	float OpacityScale = 1.0f;

	/** Show the proxy mesh (a tetrahedron per bone) instead of hiding it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skinned Splat")
	bool bShowBones = false;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaSeconds) override;

private:
	bool Initialize();

	TSharedPtr<FSkinnedSplatRenderState, ESPMode::ThreadSafe> RenderState;
	TArray<int32> MeshBoneIndex;          // data bone -> skeletal mesh bone (INDEX_NONE if missing)
	TArray<FTransform> RefComponentSpace; // ref pose, component space, per data bone
	TArray<FVector4f> BoneDQ;             // 2 per bone: real, dual (xyzw)
	FBox3f RestBounds;
};
