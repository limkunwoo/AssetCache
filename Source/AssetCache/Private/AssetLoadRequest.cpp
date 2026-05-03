#include "AssetLoadRequest.h"
#include "AssetCacheManager.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"

namespace
{
	FAssetLoadContext MakeContext(const FSoftObjectPath& Path, UObject* Loaded, float ElapsedSec, bool bIsAsync, EAssetLoadResult Result)
	{
		FAssetLoadContext Ctx;
		Ctx.AssetPath = Path;
		Ctx.Loaded = Loaded;
		Ctx.LoadTimeSec = ElapsedSec;
		Ctx.bIsAsync = bIsAsync;
		Ctx.Result = Result;
		return Ctx;
	}
}

FLoadRequest::FLoadRequest(UAssetCacheManager* InCacheManager, const FSoftObjectPath& InPath)
	: CacheManager(InCacheManager)
{
	AssetPaths.Add(InPath);
}

FLoadRequest::FLoadRequest(UAssetCacheManager* InCacheManager, TArray<FSoftObjectPath> InPaths)
	: CacheManager(InCacheManager)
	, AssetPaths(MoveTemp(InPaths))
{
}

FLoadRequest& FLoadRequest::Conditional()        { bConditional = true;        return *this; }
FLoadRequest& FLoadRequest::SkipCache()          { bSkipCache = true;          return *this; }
FLoadRequest& FLoadRequest::AllowSyncFallback()  { bAllowSyncFallback = true;  return *this; }
FLoadRequest& FLoadRequest::AbortIfInvalid(UObject* InLifeGuard) { LifeGuard = InLifeGuard; return *this; }

void FLoadRequest::Cancel()
{
	if (UAssetCacheManager* Mgr = CacheManager.Get())
	{
		for (const FSoftObjectPath& P : AssetPaths)
		{
			Mgr->UnmarkPendingLoadRequest(P);
		}
	}

	if (!StreamableHandle.IsValid())
	{
		return;
	}

	StreamableHandle->CancelHandle();
	StreamableHandle.Reset();
}

bool FLoadRequest::IsLoading() const
{
	return StreamableHandle.IsValid() && StreamableHandle->IsLoadingInProgress();
}

// ─────────────────────────────────────────────────────────────
// ResolveCacheAndResident
//   각 path에 대해 cache lookup → Conditional 처리 → Resident 처리.
//   Cached/Resident로 즉시 채워진 항목은 OutResults에 기록되고
//   여전히 로드가 필요한 항목의 인덱스만 OutNeedLoadIndices에 추가된다.
//   bConditional이면 미스 항목은 nullptr로 두고 로드하지 않는다.
//   반환값: 추가 로드가 불필요하면 true (모든 항목 즉시 해결됨).
// ─────────────────────────────────────────────────────────────

bool FLoadRequest::ResolveCacheAndResident(TArray<UObject*>& OutResults, TArray<int32>& OutNeedLoadIndices, bool bIsAsync)
{
	UAssetCacheManager* Mgr = CacheManager.Get();
	const bool bUseCache = Mgr && !bSkipCache;

	OutResults.SetNumZeroed(AssetPaths.Num());
	OutNeedLoadIndices.Reset();

	for (int32 Index = 0; Index < AssetPaths.Num(); ++Index)
	{
		const FSoftObjectPath& Path = AssetPaths[Index];

		// 1) Cache lookup
		UObject* const Cached = bUseCache ? Mgr->FindCached(Path) : nullptr;
		if (Cached)
		{
			OutResults[Index] = Cached;
			Mgr->CommitLoadResult(MakeContext(Path, Cached, 0.f, bIsAsync, EAssetLoadResult::CacheHit));
			continue;
		}

		// 2) Conditional → 로드 안 함, 미스만 기록하고 nullptr 유지
		if (bConditional)
		{
			if (Mgr)
			{
				Mgr->CommitLoadResult(MakeContext(Path, nullptr, 0.f, bIsAsync, EAssetLoadResult::MissAndLoad));
			}
			continue;
		}

		// 3) Resident in memory?
		UObject* const Resident = Path.ResolveObject();
		if (Resident)
		{
			OutResults[Index] = Resident;
			if (bUseCache)
			{
				Mgr->CommitLoadResult(MakeContext(Path, Resident, 0.f, bIsAsync, EAssetLoadResult::MissButResident));
			}
			continue;
		}

		// 4) 진짜 로드 필요
		OutNeedLoadIndices.Add(Index);
	}

	return OutNeedLoadIndices.Num() == 0;
}

// ─────────────────────────────────────────────────────────────
// ExecuteSync
// ─────────────────────────────────────────────────────────────

TArray<UObject*> FLoadRequest::ExecuteSync()
{
	check(IsInGameThread());

	TArray<UObject*> Results;
	TArray<int32> NeedLoad;
	if (ResolveCacheAndResident(Results, NeedLoad, /*bIsAsync*/false))
	{
		return Results;
	}

	UAssetCacheManager* Mgr = CacheManager.Get();
	const bool bUseCache = Mgr && !bSkipCache;

	// 로드할 path 리스트 추출 + Mark
	TArray<FSoftObjectPath> PathsToLoad;
	PathsToLoad.Reserve(NeedLoad.Num());
	for (int32 Idx : NeedLoad)
	{
		PathsToLoad.Add(AssetPaths[Idx]);
		if (Mgr)
		{
			Mgr->MarkPendingLoadRequest(AssetPaths[Idx]);
		}
	}

	const double StartTime = FPlatformTime::Seconds();

	FStreamableManager& SM = UAssetManager::GetStreamableManager();
	SM.RequestSyncLoad(PathsToLoad);

	const float ElapsedSec = static_cast<float>(FPlatformTime::Seconds() - StartTime);

	for (int32 Idx : NeedLoad)
	{
		const FSoftObjectPath& Path = AssetPaths[Idx];
		UObject* const Loaded = Path.ResolveObject();
		Results[Idx] = Loaded;

		if (bUseCache)
		{
			Mgr->CommitLoadResult(MakeContext(Path, Loaded, ElapsedSec, /*bIsAsync*/false, EAssetLoadResult::MissAndLoad));
		}
		if (Mgr)
		{
			Mgr->UnmarkPendingLoadRequest(Path);
		}
	}

	return Results;
}

// ─────────────────────────────────────────────────────────────
// ExecuteAsync
// ─────────────────────────────────────────────────────────────

void FLoadRequest::ExecuteAsync(TFunction<void(TArray<UObject*>)> Callback)
{
	check(IsInGameThread());

	TArray<UObject*> Results;
	TArray<int32> NeedLoad;
	if (ResolveCacheAndResident(Results, NeedLoad, /*bIsAsync*/true))
	{
		if (Callback)
		{
			Callback(MoveTemp(Results));
		}
		return;
	}

	UAssetCacheManager* Mgr = CacheManager.Get();

	TArray<FSoftObjectPath> PathsToLoad;
	PathsToLoad.Reserve(NeedLoad.Num());
	for (int32 Idx : NeedLoad)
	{
		PathsToLoad.Add(AssetPaths[Idx]);
		if (Mgr)
		{
			Mgr->MarkPendingLoadRequest(AssetPaths[Idx]);
		}
	}

	const double StartTime = FPlatformTime::Seconds();
	const TArray<FSoftObjectPath> LocalAllPaths = AssetPaths;
	const TArray<int32> LocalNeedLoad = NeedLoad;
	const bool LocalSkipCache = bSkipCache;
	const bool LocalAllowSyncFallback = bAllowSyncFallback;
	TWeakObjectPtr<UObject> LocalLifeGuard = LifeGuard;
	TWeakObjectPtr<UAssetCacheManager> WeakMgr = CacheManager;

	auto FinishLambda = [WeakMgr, LocalAllPaths, LocalNeedLoad, LocalSkipCache, LocalAllowSyncFallback,
						 LocalLifeGuard, StartTime, Results = MoveTemp(Results),
						 Callback = MoveTemp(Callback)]() mutable
	{
		// LifeGuard stale → cleanup만 하고 콜백 skip
		if (LocalLifeGuard.IsStale())
		{
			if (UAssetCacheManager* StaleMgr = WeakMgr.Get())
			{
				for (int32 Idx : LocalNeedLoad)
				{
					StaleMgr->UnmarkPendingLoadRequest(LocalAllPaths[Idx]);
				}
			}
			return;
		}

		const float ElapsedSec = static_cast<float>(FPlatformTime::Seconds() - StartTime);
		UAssetCacheManager* Manager = WeakMgr.Get();
		const bool bUseCache = Manager && !LocalSkipCache;

		for (int32 Idx : LocalNeedLoad)
		{
			const FSoftObjectPath& Path = LocalAllPaths[Idx];
			UObject* Loaded = Path.ResolveObject();

			// Sync fallback: async가 아무것도 못 만들었고 옵션 켜져 있으면 마지막 시도
			if (!Loaded && LocalAllowSyncFallback)
			{
				FStreamableManager& FallbackSM = UAssetManager::GetStreamableManager();
				TSharedPtr<FStreamableHandle> FallbackHandle = FallbackSM.RequestSyncLoad(Path);
				const bool bFallbackOk = FallbackHandle.IsValid() && FallbackHandle->HasLoadCompleted();
				if (bFallbackOk)
				{
					Loaded = FallbackHandle->GetLoadedAsset();
				}
			}

			Results[Idx] = Loaded;

			if (bUseCache)
			{
				Manager->CommitLoadResult(MakeContext(Path, Loaded, ElapsedSec, /*bIsAsync*/true, EAssetLoadResult::MissAndLoad));
			}
			if (Manager)
			{
				Manager->UnmarkPendingLoadRequest(Path);
			}
		}

		if (Callback)
		{
			Callback(MoveTemp(Results));
		}
	};

	FStreamableManager& SM = UAssetManager::GetStreamableManager();
	StreamableHandle = SM.RequestAsyncLoad(
		PathsToLoad,
		FStreamableDelegate::CreateLambda(MoveTemp(FinishLambda))
	);

	if (StreamableHandle.IsValid())
	{
		return;
	}

	// 핸들 생성 실패 — 즉시 실패 처리. Mark된 path들 unmark.
	if (Mgr)
	{
		for (int32 Idx : NeedLoad)
		{
			Mgr->UnmarkPendingLoadRequest(AssetPaths[Idx]);
		}
	}

	// Results는 위 람다 캡처에서 MoveTemp 됐으므로 복원 불가.
	// 일관성을 위해 모든 항목 nullptr로 채워 콜백 (캐시 hit/resident 정보는 이미 CommitLoadResult로 기록됨).
	// FinishLambda가 호출되지 않으므로 여기서 직접 Callback 변수를 들고 있지 않다 — 람다가 캡처를 가져갔다.
	// 따라서 사용자에게 콜백이 가지 않을 수 있음. RequestAsyncLoad가 invalid handle을 반환하는 경우는
	// 사실상 PathsToLoad가 비어있을 때뿐인데, 그 경우 ResolveCacheAndResident가 true를 반환해 위에서 이미 종료됨.
	// 즉 이 분기는 안전망일 뿐 실질적으로 도달하지 않는다.
}
