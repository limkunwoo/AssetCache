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
 * 내부적으로 항상 TArray<FSoftObjectPath>를 다루며, single/multi API를 제공한다.
 *
 * Single-path 사용 예 (paths.Num() == 1):
 *   Resource::Load(Path).Get<UStaticMesh>();
 *   Resource::Load(Path).OnComplete<UTexture2D>([](UTexture2D* T){ });
 *   Resource::Load(Path).Then<UStaticMesh>([](UStaticMesh* M){ return M->GetName(); })
 *                       .Then([](SD::TExpected<FString> R){ });
 *
 * Multi-path 사용 예:
 *   Resource::Load({P1, P2}).GetAll<UStaticMesh>();
 *   Resource::Load({P1, P2}).OnCompleteAll<UTexture2D>([](TArray<UTexture2D*> Ts){ });
 *   Resource::Load({P1, P2}).ThenAll<UStaticMesh>([](TArray<UStaticMesh*> Ms){ ... });
 */
struct ASSETCACHE_API FLoadRequest
{
	FLoadRequest() = default;
	FLoadRequest(UAssetCacheManager* InCacheManager, const FSoftObjectPath& InPath);
	FLoadRequest(UAssetCacheManager* InCacheManager, TArray<FSoftObjectPath> InPaths);

	// ─── 빌더 옵션 ──────────────────────────────────────────

	/** 캐시에 없으면 로드하지 않는다 (캐시 전용 조회) */
	FLoadRequest& Conditional();

	/** 캐시에 기록하지 않는다 (1회성 로드) */
	FLoadRequest& SkipCache();

	/** 비동기 실패 시 동기 폴백 */
	FLoadRequest& AllowSyncFallback();

	/** 비동기 완료 시 LifeGuard가 Invalid이면 콜백 취소 */
	FLoadRequest& AbortIfInvalid(UObject* InLifeGuard);

	// ─── Single-path 터미널 메서드 (AssetPaths.Num() == 1 가정) ───

	/** 동기 로드 */
	template<typename T = UObject>
	T* Get();

	/** 비동기 로드 + 콜백 */
	template<typename T = UObject>
	void OnComplete(TFunction<void(T*)> Callback);

	/**
	 * 비동기 로드 + 람다 + Future 체인.
	 *
	 * 람다 시그니처는 SD::TExpectedFuture<T*>::Then(F)로 위임된다.
	 * - F가 (T*)를 받으면 에러 시 콜백 skip, 다음 future로 에러 전파.
	 * - F가 (SD::TExpected<T*>)를 받으면 에러도 함께 전달.
	 */
	template<typename T = UObject, typename F>
	auto Then(F&& Func);

	// ─── Multi-path 터미널 메서드 ────────────────────────────

	/** 동기 다중 로드 — 결과는 입력 경로 순서 유지, 실패 항목은 nullptr */
	template<typename T = UObject>
	TArray<T*> GetAll();

	/** 비동기 다중 로드 + 콜백 */
	template<typename T = UObject>
	void OnCompleteAll(TFunction<void(TArray<T*>)> Callback);

	/** 비동기 다중 로드 + 람다 + Future 체인 (Promise 값 = TArray<T*>) */
	template<typename T = UObject, typename F>
	auto ThenAll(F&& Func);

	// ─── 제어 ────────────────────────────────────────────────

	/** 진행 중인 비동기 로드 취소 */
	void Cancel();

	/** 비동기 로드 진행 중인지 */
	bool IsLoading() const;

private:
	/** 동기 실행 — 결과는 입력 경로 순서 유지 */
	TArray<UObject*> ExecuteSync();

	/** 비동기 실행 — 콜백은 입력 경로 순서대로 결과를 받는다 */
	void ExecuteAsync(TFunction<void(TArray<UObject*>)> Callback);

	/** 캐시 조회 + Conditional/Resident 처리. true 반환 시 즉시 완료(추가 로드 불필요) */
	bool ResolveCacheAndResident(TArray<UObject*>& OutResults, TArray<int32>& OutNeedLoadIndices, bool bIsAsync);

	TWeakObjectPtr<UAssetCacheManager> CacheManager;
	TArray<FSoftObjectPath> AssetPaths;
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
	check(AssetPaths.Num() == 1);
	TArray<UObject*> Results = ExecuteSync();
	return Results.Num() > 0 ? Cast<T>(Results[0]) : nullptr;
}

template<typename T>
void FLoadRequest::OnComplete(TFunction<void(T*)> Callback)
{
	check(AssetPaths.Num() == 1);
	ExecuteAsync([Callback = MoveTemp(Callback)](TArray<UObject*> Results)
	{
		if (Callback)
		{
			UObject* Obj = Results.Num() > 0 ? Results[0] : nullptr;
			Callback(Cast<T>(Obj));
		}
	});
}

template<typename T, typename F>
auto FLoadRequest::Then(F&& Func)
{
	check(AssetPaths.Num() == 1);
	auto Promise = MakeShared<SD::TExpectedPromise<T*>>();

	const FString PathStr = AssetPaths[0].ToString();
	ExecuteAsync([Promise, PathStr](TArray<UObject*> Results)
	{
		UObject* Obj = Results.Num() > 0 ? Results[0] : nullptr;
		if (T* Typed = Cast<T>(Obj))
		{
			Promise->SetValue(Typed);
		}
		else
		{
			Promise->SetValue(SD::Error(Obj ? -1 : -2, FString::Printf(
				TEXT("%s: %s"),
				Obj ? TEXT("Asset cast failed") : TEXT("Asset load failed"),
				*PathStr)));
		}
	});

	return Promise->GetFuture().Then(Forward<F>(Func));
}

template<typename T>
TArray<T*> FLoadRequest::GetAll()
{
	TArray<UObject*> Results = ExecuteSync();
	TArray<T*> Typed;
	Typed.Reserve(Results.Num());
	for (UObject* Obj : Results)
	{
		Typed.Add(Cast<T>(Obj));
	}
	return Typed;
}

template<typename T>
void FLoadRequest::OnCompleteAll(TFunction<void(TArray<T*>)> Callback)
{
	ExecuteAsync([Callback = MoveTemp(Callback)](TArray<UObject*> Results)
	{
		if (Callback)
		{
			TArray<T*> Typed;
			Typed.Reserve(Results.Num());
			for (UObject* Obj : Results)
			{
				Typed.Add(Cast<T>(Obj));
			}
			Callback(MoveTemp(Typed));
		}
	});
}

template<typename T, typename F>
auto FLoadRequest::ThenAll(F&& Func)
{
	auto Promise = MakeShared<SD::TExpectedPromise<TArray<T*>>>();

	ExecuteAsync([Promise](TArray<UObject*> Results)
	{
		TArray<T*> Typed;
		Typed.Reserve(Results.Num());
		for (UObject* Obj : Results)
		{
			Typed.Add(Cast<T>(Obj));
		}
		Promise->SetValue(MoveTemp(Typed));
	});

	return Promise->GetFuture().Then(Forward<F>(Func));
}
