#include "AssetLoadRequest.h"
#include "AssetCacheManager.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"

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

	if (StreamableHandle.IsValid())
	{
		StreamableHandle->CancelHandle();
		StreamableHandle.Reset();
	}
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
// ─────────────────────────────────────────────────────────────

bool FLoadRequest::ResolveCacheAndResident(TArray<UObject*>& OutResults, TArray<int32>& OutNeedLoadIndices, bool bIsAsync)
{
	UAssetCacheManager* Mgr = CacheManager.Get();
	OutResults.SetNumZeroed(AssetPaths.Num());
	OutNeedLoadIndices.Reset();

	for (int32 Index = 0; Index < AssetPaths.Num(); ++Index)
	{
		const FSoftObjectPath& Path = AssetPaths[Index];

		// 1) Cache lookup
		if (Mgr && !bSkipCache)
		{
			if (UObject* Cached = Mgr->FindCached(Path))
			{
				OutResults[Index] = Cached;

				FAssetLoadContext Ctx;
				Ctx.AssetPath = Path;
				Ctx.Loaded = Cached;
				Ctx.LoadTimeSec = 0.f;
				Ctx.bIsAsync = bIsAsync;
				Ctx.Result = EAssetLoadResult::CacheHit;
				Mgr->CommitLoadResult(Ctx);
				continue;
			}
		}

		// 2) Conditional → 로드 안 함
		if (bConditional)
		{
			FAssetLoadContext Ctx;
			Ctx.AssetPath = Path;
			Ctx.bIsAsync = bIsAsync;
			Ctx.Result = EAssetLoadResult::MissAndLoad;
			if (Mgr) { Mgr->CommitLoadResult(Ctx); }
			continue;
		}

		// 3) Resident in memory?
		if (UObject* Resident = Path.ResolveObject())
		{
			OutResults[Index] = Resident;

			FAssetLoadContext Ctx;
			Ctx.AssetPath = Path;
			Ctx.Loaded = Resident;
			Ctx.LoadTimeSec = 0.f;
			Ctx.bIsAsync = bIsAsync;
			Ctx.Result = EAssetLoadResult::MissButResident;
			if (Mgr && !bSkipCache) { Mgr->CommitLoadResult(Ctx); }
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

	// 로드할 path 리스트 추출
	TArray<FSoftObjectPath> PathsToLoad;
	PathsToLoad.Reserve(NeedLoad.Num());
	for (int32 Idx : NeedLoad)
	{
		PathsToLoad.Add(AssetPaths[Idx]);
		if (Mgr) { Mgr->MarkPendingLoadRequest(AssetPaths[Idx]); }
	}

	const double StartTime = FPlatformTime::Seconds();

	FStreamableManager& SM = UAssetManager::GetStreamableManager();
	TSharedPtr<FStreamableHandle> Handle = SM.RequestSyncLoad(PathsToLoad);

	const float ElapsedSec = static_cast<float>(FPlatformTime::Seconds() - StartTime);

	for (int32 Idx : NeedLoad)
	{
		const FSoftObjectPath& Path = AssetPaths[Idx];
		UObject* Loaded = Path.ResolveObject();
		Results[Idx] = Loaded;

		FAssetLoadContext Ctx;
		Ctx.AssetPath = Path;
		Ctx.Loaded = Loaded;
		Ctx.LoadTimeSec = ElapsedSec;
		Ctx.bIsAsync = false;
		Ctx.Result = EAssetLoadResult::MissAndLoad;
		if (Mgr && !bSkipCache) { Mgr->CommitLoadResult(Ctx); }
		if (Mgr) { Mgr->UnmarkPendingLoadRequest(Path); }
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
		if (Callback) { Callback(MoveTemp(Results)); }
		return;
	}

	UAssetCacheManager* Mgr = CacheManager.Get();

	TArray<FSoftObjectPath> PathsToLoad;
	PathsToLoad.Reserve(NeedLoad.Num());
	for (int32 Idx : NeedLoad)
	{
		PathsToLoad.Add(AssetPaths[Idx]);
		if (Mgr) { Mgr->MarkPendingLoadRequest(AssetPaths[Idx]); }
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
		// LifeGuard 사용 시 stale이면 cleanup만 하고 콜백 skip
		if (LocalLifeGuard.IsStale())
		{
			if (UAssetCacheManager* Manager = WeakMgr.Get())
			{
				for (int32 Idx : LocalNeedLoad)
				{
					Manager->UnmarkPendingLoadRequest(LocalAllPaths[Idx]);
				}
			}
			return;
		}

		const float ElapsedSec = static_cast<float>(FPlatformTime::Seconds() - StartTime);
		UAssetCacheManager* Manager = WeakMgr.Get();

		for (int32 Idx : LocalNeedLoad)
		{
			const FSoftObjectPath& Path = LocalAllPaths[Idx];
			UObject* Loaded = Path.ResolveObject();

			// Sync fallback if async produced nothing
			if (!Loaded && LocalAllowSyncFallback)
			{
				FStreamableManager& FallbackSM = UAssetManager::GetStreamableManager();
				TSharedPtr<FStreamableHandle> FallbackHandle = FallbackSM.RequestSyncLoad(Path);
				if (FallbackHandle.IsValid() && FallbackHandle->HasLoadCompleted())
				{
					Loaded = FallbackHandle->GetLoadedAsset();
				}
			}

			Results[Idx] = Loaded;

			FAssetLoadContext Ctx;
			Ctx.AssetPath = Path;
			Ctx.Loaded = Loaded;
			Ctx.LoadTimeSec = ElapsedSec;
			Ctx.bIsAsync = true;
			Ctx.Result = EAssetLoadResult::MissAndLoad;

			if (Manager)
			{
				if (!LocalSkipCache) { Manager->CommitLoadResult(Ctx); }
				Manager->UnmarkPendingLoadRequest(Path);
			}
		}

		if (Callback) { Callback(MoveTemp(Results)); }
	};

	FStreamableManager& SM = UAssetManager::GetStreamableManager();
	StreamableHandle = SM.RequestAsyncLoad(
		PathsToLoad,
		FStreamableDelegate::CreateLambda(MoveTemp(FinishLambda))
	);

	if (!StreamableHandle.IsValid())
	{
		// 핸들 생성 실패 — 즉시 실패 처리
		if (Mgr)
		{
			for (int32 Idx : NeedLoad)
			{
				Mgr->UnmarkPendingLoadRequest(AssetPaths[Idx]);
			}
		}

		// Results는 이미 MoveTemp됐으므로 다시 채워서 콜백
		TArray<UObject*> FailResults;
		FailResults.SetNumZeroed(AssetPaths.Num());
		// 캐시 hit/resident 항목은 이미 위 ResolveCacheAndResident에서 처리되었지만
		// MoveTemp 후라 복원 불가 — 일관성을 위해 모두 nullptr로 콜백
		if (Callback) { Callback(MoveTemp(FailResults)); }
	}
}
