#include "AssetLoadRequest.h"
#include "AssetCacheManager.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"

FLoadRequest::FLoadRequest(UAssetCacheManager* InCacheManager, const FSoftObjectPath& InPath)
	: CacheManager(InCacheManager)
	, AssetPath(InPath)
{
}

FLoadRequest& FLoadRequest::Conditional()
{
	bConditional = true;
	return *this;
}

FLoadRequest& FLoadRequest::SkipCache()
{
	bSkipCache = true;
	return *this;
}

FLoadRequest& FLoadRequest::AllowSyncFallback()
{
	bAllowSyncFallback = true;
	return *this;
}

FLoadRequest& FLoadRequest::AbortIfInvalid(UObject* InLifeGuard)
{
	LifeGuard = InLifeGuard;
	return *this;
}

void FLoadRequest::Cancel()
{
	if (UAssetCacheManager* Mgr = CacheManager.Get())
	{
		Mgr->UnmarkPendingLoadRequest(AssetPath);
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
// ExecuteSync
// ─────────────────────────────────────────────────────────────

UObject* FLoadRequest::ExecuteSync(FAssetLoadContext& OutContext)
{
	check(IsInGameThread());

	OutContext.AssetPath = AssetPath;
	OutContext.bIsAsync = false;

	UAssetCacheManager* Mgr = CacheManager.Get();

	// 1) Cache lookup
	if (Mgr && !bSkipCache)
	{
		if (UObject* Cached = Mgr->FindCached(AssetPath))
		{
			OutContext.Loaded = Cached;
			OutContext.LoadTimeSec = 0.f;
			OutContext.Result = EAssetLoadResult::CacheHit;
			Mgr->CommitLoadResult(OutContext);
			return Cached;
		}
	}

	// Conditional mode: don't load if not cached
	if (bConditional)
	{
		OutContext.Result = EAssetLoadResult::MissAndLoad;
		if (Mgr) { Mgr->CommitLoadResult(OutContext); }
		return nullptr;
	}

	// 2) Resolve (already resident in memory?)
	if (UObject* Resident = AssetPath.ResolveObject())
	{
		OutContext.Loaded = Resident;
		OutContext.LoadTimeSec = 0.f;
		OutContext.Result = EAssetLoadResult::MissButResident;

		if (Mgr && !bSkipCache)
		{
			Mgr->CommitLoadResult(OutContext);
		}
		return Resident;
	}

	// 3) Actual synchronous load
	if (Mgr)
	{
		Mgr->MarkPendingLoadRequest(AssetPath);
	}

	const double StartTime = FPlatformTime::Seconds();

	FStreamableManager& SM = UAssetManager::GetStreamableManager();
	TSharedPtr<FStreamableHandle> Handle = SM.RequestSyncLoad(AssetPath);

	UObject* Loaded = nullptr;
	if (Handle.IsValid() && Handle->HasLoadCompleted())
	{
		Loaded = Handle->GetLoadedAsset();
	}

	const float ElapsedSec = static_cast<float>(FPlatformTime::Seconds() - StartTime);

	OutContext.Loaded = Loaded;
	OutContext.LoadTimeSec = ElapsedSec;
	OutContext.Result = EAssetLoadResult::MissAndLoad;

	if (Mgr && !bSkipCache)
	{
		Mgr->CommitLoadResult(OutContext);
	}

	if (Mgr)
	{
		Mgr->UnmarkPendingLoadRequest(AssetPath);
	}

	return Loaded;
}

// ─────────────────────────────────────────────────────────────
// ExecuteAsync
// ─────────────────────────────────────────────────────────────

void FLoadRequest::ExecuteAsync(TFunction<void(UObject*, const FAssetLoadContext&)> Callback)
{
	check(IsInGameThread());

	UAssetCacheManager* Mgr = CacheManager.Get();

	// 1) Cache lookup
	if (Mgr && !bSkipCache)
	{
		if (UObject* Cached = Mgr->FindCached(AssetPath))
		{
			FAssetLoadContext Ctx;
			Ctx.AssetPath = AssetPath;
			Ctx.Loaded = Cached;
			Ctx.LoadTimeSec = 0.f;
			Ctx.bIsAsync = true;
			Ctx.Result = EAssetLoadResult::CacheHit;
			Mgr->CommitLoadResult(Ctx);

			if (Callback) { Callback(Cached, Ctx); }
			return;
		}
	}

	// Conditional mode
	if (bConditional)
	{
		FAssetLoadContext Ctx;
		Ctx.AssetPath = AssetPath;
		Ctx.bIsAsync = true;
		Ctx.Result = EAssetLoadResult::MissAndLoad;
		if (Mgr) { Mgr->CommitLoadResult(Ctx); }
		if (Callback) { Callback(nullptr, Ctx); }
		return;
	}

	// 2) Resolve
	if (UObject* Resident = AssetPath.ResolveObject())
	{
		FAssetLoadContext Ctx;
		Ctx.AssetPath = AssetPath;
		Ctx.Loaded = Resident;
		Ctx.LoadTimeSec = 0.f;
		Ctx.bIsAsync = true;
		Ctx.Result = EAssetLoadResult::MissButResident;

		if (Mgr && !bSkipCache)
		{
			Mgr->CommitLoadResult(Ctx);
		}

		if (Callback) { Callback(Resident, Ctx); }
		return;
	}

	// 3) Async load
	const double StartTime = FPlatformTime::Seconds();
	const FSoftObjectPath LocalPath = AssetPath;
	const bool LocalSkipCache = bSkipCache;
	const bool LocalAllowSyncFallback = bAllowSyncFallback;
	TWeakObjectPtr<UObject> LocalLifeGuard = LifeGuard;
	TWeakObjectPtr<UAssetCacheManager> WeakMgr = CacheManager;
	if (Mgr)
	{
		Mgr->MarkPendingLoadRequest(LocalPath);
	}

	FStreamableManager& SM = UAssetManager::GetStreamableManager();
	StreamableHandle = SM.RequestAsyncLoad(
		AssetPath,
		FStreamableDelegate::CreateLambda(
			[WeakMgr, LocalPath, LocalSkipCache, LocalAllowSyncFallback,
			 LocalLifeGuard, StartTime, Callback = MoveTemp(Callback)]()
			{
				TWeakObjectPtr<UAssetCacheManager> InnerWeakMgr = WeakMgr;

				// LifeGuard check — abort callback if invalid
				if (LocalLifeGuard.IsStale())
				{
					if (UAssetCacheManager* Manager = InnerWeakMgr.Get())
					{
						Manager->UnmarkPendingLoadRequest(LocalPath);
					}
					return;
				}

				UObject* Loaded = LocalPath.ResolveObject();
				const float ElapsedSec = static_cast<float>(FPlatformTime::Seconds() - StartTime);

				// Sync fallback if async produced nothing
				if (!Loaded && LocalAllowSyncFallback)
				{
					FStreamableManager& FallbackSM = UAssetManager::GetStreamableManager();
					TSharedPtr<FStreamableHandle> FallbackHandle = FallbackSM.RequestSyncLoad(LocalPath);
					if (FallbackHandle.IsValid() && FallbackHandle->HasLoadCompleted())
					{
						Loaded = FallbackHandle->GetLoadedAsset();
					}
				}

				FAssetLoadContext Ctx;
				Ctx.AssetPath = LocalPath;
				Ctx.Loaded = Loaded;
				Ctx.LoadTimeSec = ElapsedSec;
				Ctx.bIsAsync = true;
				Ctx.Result = EAssetLoadResult::MissAndLoad;

				if (UAssetCacheManager* Manager = InnerWeakMgr.Get())
				{
					if (!LocalSkipCache)
					{
						Manager->CommitLoadResult(Ctx);
					}

					Manager->UnmarkPendingLoadRequest(LocalPath);
				}

				if (Callback) { Callback(Loaded, Ctx); }
			}
		)
	);

	if (!StreamableHandle.IsValid())
	{
		if (Mgr)
		{
			Mgr->UnmarkPendingLoadRequest(LocalPath);
		}

		FAssetLoadContext Ctx;
		Ctx.AssetPath = LocalPath;
		Ctx.LoadTimeSec = 0.f;
		Ctx.bIsAsync = true;
		Ctx.Result = EAssetLoadResult::MissAndLoad;

		if (Mgr && !LocalSkipCache)
		{
			Mgr->CommitLoadResult(Ctx);
		}

		if (Callback)
		{
			Callback(nullptr, Ctx);
		}
	}
}
