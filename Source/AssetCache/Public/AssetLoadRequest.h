#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"
#include "Engine/StreamableManager.h"
#include "AssetCacheTypes.h"
#include "ExpectedFuture.h"
#include "FutureExtensions.h"

class UAssetCacheManager;

/**
 * 빌더 패턴 로드 요청.
 * UAssetManager/FStreamableHandle를 래핑하여 로드를 실행하고,
 * 캐시 조회/저장은 UAssetCacheManager에 위임합니다.
 *
 * 사용 예:
 *   Resource::Load(Path).Get<UStaticMesh>();
 *   Resource::Load(Path).SkipCache().OnComplete<UTexture2D>([](auto* T, auto& Ctx){ });
 *   auto Req = Resource::Load(Path).AbortIfInvalid(this);
 *   Req.OnComplete<UStaticMesh>([](auto* M, auto& Ctx){ });
 *   Req.Cancel();
 */
struct ASSETCACHE_API FLoadRequest
{
	FLoadRequest() = default;
	explicit FLoadRequest(UAssetCacheManager* InCacheManager, const FSoftObjectPath& InPath);

	// ─── 빌더 옵션 ──────────────────────────────────────────

	/** 캐시에 없으면 로드하지 않는다 (캐시 전용 조회) */
	FLoadRequest& Conditional();

	/** 캐시에 기록하지 않는다 (1회성 로드) */
	FLoadRequest& SkipCache();

	/** 비동기 실패 시 동기 폴백 */
	FLoadRequest& AllowSyncFallback();

	/** 비동기 완료 시 LifeGuard가 Invalid이면 콜백 취소 */
	FLoadRequest& AbortIfInvalid(UObject* InLifeGuard);

	// ─── 터미널 메서드 ───────────────────────────────────────

	/** 동기 로드 */
	template<typename T = UObject>
	T* Get();

	/** 비동기 로드 + 콜백 */
	template<typename T = UObject>
	void OnComplete(TFunction<void(T*, const FAssetLoadContext&)> Callback);

	/** 비동기 로드 + Future 체인 */
	template<typename T = UObject>
	SD::TExpectedFuture<T*> Then();

	// ─── 제어 ────────────────────────────────────────────────

	/** 진행 중인 비동기 로드 취소 */
	void Cancel();

	/** 비동기 로드 진행 중인지 */
	bool IsLoading() const;

private:
	/** 동기 실행 (non-template) */
	UObject* ExecuteSync(FAssetLoadContext& OutContext);

	/** 비동기 실행 (non-template) */
	void ExecuteAsync(TFunction<void(UObject*, const FAssetLoadContext&)> Callback);

	TWeakObjectPtr<UAssetCacheManager> CacheManager;
	FSoftObjectPath AssetPath;
	TSharedPtr<FStreamableHandle> StreamableHandle;

	bool bConditional = false;
	bool bSkipCache = false;
	bool bAllowSyncFallback = false;
	TWeakObjectPtr<UObject> LifeGuard;
};

// ─────────────────────────────────────────────────────────────
// Template implementations
// ─────────────────────────────────────────────────────────────

template<typename T>
T* FLoadRequest::Get()
{
	FAssetLoadContext Context;
	UObject* Result = ExecuteSync(Context);
	return Cast<T>(Result);
}

template<typename T>
void FLoadRequest::OnComplete(TFunction<void(T*, const FAssetLoadContext&)> Callback)
{
	ExecuteAsync([Callback = MoveTemp(Callback)](UObject* Obj, const FAssetLoadContext& Ctx)
	{
		if (Callback)
		{
			Callback(Cast<T>(Obj), Ctx);
		}
	});
}

template<typename T>
SD::TExpectedFuture<T*> FLoadRequest::Then()
{
	auto Promise = MakeShared<SD::TExpectedPromise<T*>>();

	ExecuteAsync([Promise](UObject* Obj, const FAssetLoadContext& Ctx)
	{
		if (Obj)
		{
			if (T* Typed = Cast<T>(Obj))
			{
				Promise->SetValue(Typed);
			}
			else
			{
				Promise->SetValue(SD::Error(-1, FString::Printf(
					TEXT("Asset cast failed: %s"), *Ctx.AssetPath.ToString())));
			}
		}
		else
		{
			Promise->SetValue(SD::Error(-2, FString::Printf(
				TEXT("Asset load failed: %s"), *Ctx.AssetPath.ToString())));
		}
	});

	return Promise->GetFuture();
}
