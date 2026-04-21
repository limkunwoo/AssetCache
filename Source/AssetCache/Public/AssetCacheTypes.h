#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"
#include "AssetCacheTypes.generated.h"

// ─────────────────────────────────────────────────────────────
// 로드 결과 분류
// ─────────────────────────────────────────────────────────────

UENUM(BlueprintType)
enum class EAssetLoadResult : uint8
{
	/** 캐시에서 즉시 반환 */
	CacheHit,
	/** 캐시 미스지만 메모리에 이미 Resident (ResolveObject 성공) */
	MissButResident,
	/** 캐시 미스 + 실제 로드 수행 */
	MissAndLoad,
};

UENUM(BlueprintType)
enum class EProfileTier : uint8
{
	/** FLoadRequest 경로에서 직접 측정 */
	Precise,
	/** 전역 델리게이트로 관찰된 우회 로드 */
	Observed,
};

// ─────────────────────────────────────────────────────────────
// 로드 컨텍스트 — 결과 메타데이터
// ─────────────────────────────────────────────────────────────

USTRUCT(BlueprintType)
struct ASSETCACHE_API FAssetLoadContext
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	FSoftObjectPath AssetPath;

	UPROPERTY(BlueprintReadOnly)
	TObjectPtr<UObject> Loaded = nullptr;

	UPROPERTY(BlueprintReadOnly)
	float LoadTimeSec = 0.f;

	UPROPERTY(BlueprintReadOnly)
	bool bIsAsync = false;

	UPROPERTY(BlueprintReadOnly)
	EAssetLoadResult Result = EAssetLoadResult::MissAndLoad;

	UPROPERTY(BlueprintReadOnly)
	EProfileTier ProfileTier = EProfileTier::Precise;
};

// ─────────────────────────────────────────────────────────────
// 로드 옵션 — FLoadRequest → UAssetCacheManager 전달용
// ─────────────────────────────────────────────────────────────

struct FLoadOptions
{
	bool bConditional = false;
	bool bSkipCache = false;
	bool bAllowSyncFallback = false;
	TWeakObjectPtr<UObject> LifeGuard;
};
