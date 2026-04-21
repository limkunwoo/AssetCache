#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "AssetCacheTypes.h"
#include "AssetProfileCollector.h"
#include "AssetCacheManager.generated.h"

class IAssetCachePolicy;

/**
 * 엔진 서브시스템 — 캐시/프로파일 전용.
 * 에셋 로드 자체는 FLoadRequest가 UAssetManager를 통해 수행하며,
 * 이 매니저는 캐시 조회/저장/정책/프로파일만 담당합니다.
 */
UCLASS()
class ASSETCACHE_API UAssetCacheManager : public UEngineSubsystem
{
	GENERATED_BODY()

	struct FObservedPackageLoadInfo
	{
		double StartTimeSec = 0.0;
		bool bIsAsync = false;
	};

public:
	UAssetCacheManager();
	virtual ~UAssetCacheManager();

	static UAssetCacheManager* Get();

	//~ USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override { return true; }

	// ─── 캐시 조회/저장 ─────────────────────────────────────

	/** 캐시에서 에셋 검색. 없으면 nullptr */
	UObject* FindCached(const FSoftObjectPath& AssetPath) const;

	/** 로드 완료 후 캐시에 저장 + 프로파일 기록 */
	void CommitLoadResult(const FAssetLoadContext& Context);

	/** FLoadRequest 경로에서 전역 관찰자 중복 처리를 막는다 */
	void MarkPendingLoadRequest(const FSoftObjectPath& AssetPath);

	/** FLoadRequest 경로 완료 후 전역 관찰자 중복 처리를 푼다 */
	void UnmarkPendingLoadRequest(const FSoftObjectPath& AssetPath);

	// ─── 캐시 관리 ──────────────────────────────────────────

	UFUNCTION(BlueprintCallable, Category = "AssetCache")
	void FlushAllCaches();

	// ─── 프로파일 ───────────────────────────────────────────

	UFUNCTION(BlueprintCallable, Category = "AssetCache|Profile")
	float GetCacheHitRate() const;

	UFUNCTION(BlueprintCallable, Category = "AssetCache|Profile")
	double GetAverageLoadTime() const;

	UFUNCTION(BlueprintCallable, Category = "AssetCache|Profile")
	void DumpProfileStats() const;

	UFUNCTION(BlueprintCallable, Category = "AssetCache|Profile")
	void ResetProfileData();

private:
	void InitPoliciesFromSettings();
	void OnGlobalAssetLoaded(UObject* Asset);
	void OnSyncPackageLoadStarted(const FString& PackageName);
	void OnAsyncPackageLoadStarted(FStringView PackageName);

	IAssetCachePolicy* FindPolicyForClass(UClass* AssetClass) const;

	TMap<UClass*, TSharedPtr<IAssetCachePolicy>> ClassPolicyMap;
	TSet<FSoftObjectPath> PendingLoadRequestPaths;
	TMap<FName, FObservedPackageLoadInfo> PendingPackageLoads;
	FDelegateHandle OnAssetLoadedHandle;
	FDelegateHandle OnSyncLoadPackageHandle;
	FDelegateHandle OnAsyncLoadPackageHandle;

	TUniquePtr<FAssetProfileCollector> ProfileCollector;
};
