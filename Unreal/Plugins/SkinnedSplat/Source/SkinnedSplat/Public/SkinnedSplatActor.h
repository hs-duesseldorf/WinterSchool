#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SkinnedSplatActor.generated.h"

class USceneComponent;
class USkeletalMeshComponent;
class UBoxComponent;
class UBillboardComponent;
class UAnimSequence;
class UMaterialInterface;
class USkinnedSplatData;
class FSkinnedSplatRenderState;

/**
 * Renders a USkinnedSplatData through Epic's GaussianSplat renderer, deformed by a (hidden) skeletal mesh: every frame
 * one compute pass dual-quaternion skins the capture-pose splats straight into the renderer's splat buffer, and an
 * optional material pass recolors them. Works the same in editor viewports and in game.
 */
UCLASS(ClassGroup = Rendering, hidecategories = (Physics, Collision, Input, LOD, Cooking, Replication, Networking, HLOD))
class SKINNEDSPLAT_API ASkinnedSplatActor : public AActor
{
	GENERATED_BODY()
public:
	ASkinnedSplatActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Skinned Splat")
	TObjectPtr<USceneComponent> Root;

	/** Drives the splats. Hidden unless bShowBones; animate it like any skeletal mesh (anim blueprint, montages...). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Skinned Splat")
	TObjectPtr<USkeletalMeshComponent> Mesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skinned Splat")
	TObjectPtr<USkinnedSplatData> SplatData;

	/** Clip to loop; empty = the splat's default clip. Ignored when the mesh uses an anim blueprint. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skinned Splat|Animation")
	TObjectPtr<UAnimSequence> Animation;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skinned Splat|Animation")
	bool bPlayAnimation = true;

	/** Also play the clip in editor viewports (not only in game). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skinned Splat|Animation")
	bool bAnimateInEditor = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skinned Splat|Animation", meta = (ClampMin = "0"))
	float PlayRate = 1.0f;

	/**
	 * Optional surface material evaluated once per splat. Emissive Color becomes the splat color, Opacity multiplies the
	 * splat opacity. Vertex Color = the splat's own color and opacity, World Position = splat center, normal = the
	 * splat's flattest axis, UV0 = position inside the actor bounds (front view). Unlit + Translucent is the usual setup.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skinned Splat|Look")
	TObjectPtr<UMaterialInterface> Material;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skinned Splat|Look", meta = (ClampMin = "0"))
	float Brightness = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skinned Splat|Look", meta = (ClampMin = "0"))
	float PointScaleFactor = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skinned Splat|Look", meta = (ClampMin = "0"))
	float OpacityScale = 1.0f;

	/** Undo the camera tone curve baked into splats trained on photos/video, like Epic's splat component (off by default
	 *  there too). It is applied per splat before blending, so it blows out highlights and streaks saturated colours;
	 *  leave it off when the splats are lit (r.GaussianSplat.Lighting). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skinned Splat|Look")
	bool bInvertToneCurve = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skinned Splat|Look", meta = (ClampMin = "0", ClampMax = "1", EditCondition = "bInvertToneCurve"))
	float InverseToneCurveStrength = 1.0f;

	/** Show the proxy skeleton (a tetrahedron per bone). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skinned Splat")
	bool bShowBones = false;

	/** The splats cast a shadow from the sun (GaussianSplat's splat shadow map, r.GaussianSplat.Shadow). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skinned Splat|Look")
	bool bCastShadow = true;

	/** How dark the sun's shadows on the splats are: the share of the captured color a full shadow takes away (see GaussianSplatComponent's ShadowStrength) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skinned Splat|Look", meta = (ClampMin = "0", ClampMax = "1", UIMin = "0", UIMax = "1"))
	float ShadowStrength = 0.75f;

	/** Loops (or plays once) a clip on the driving mesh. */
	UFUNCTION(BlueprintCallable, Category = "Skinned Splat")
	void PlayClip(UAnimSequence* Clip, bool bLoop = true);

	UFUNCTION(BlueprintCallable, Category = "Skinned Splat")
	void SetMaterial(UMaterialInterface* NewMaterial);

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostRegisterAllComponents() override;
	virtual void PostUnregisterAllComponents() override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif

private:
	void SetupMesh();
	void BuildRenderState();
	void ReleaseRenderState();
	static void EnsureMaterialUsage(UMaterialInterface* InMaterial);

	UPROPERTY(VisibleAnywhere, Category = "Skinned Splat")
	TObjectPtr<UBoxComponent> BoundsBox;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UBillboardComponent> Sprite;
#endif

	TSharedPtr<FSkinnedSplatRenderState, ESPMode::ThreadSafe> RenderState;
	FGuid BuiltGuid;
	TWeakObjectPtr<UObject> BuiltMesh;
	TArray<int32> MeshBoneIndex;   // data bone -> skeletal mesh bone (INDEX_NONE if missing)
	TArray<FTransform> RefInverse; // per data bone: inverse ref pose, mesh component space
	FTransform SplatToMesh;        // actor (splat) space -> mesh component space
	TArray<FVector4f> BoneDQ;      // 2 per bone: real, dual (xyzw), actor space
	FBox3f LocalBounds;
};
