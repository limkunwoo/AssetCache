#include "AssetCacheManager.h"
#include "AssetCacheSettings.h"
#include "AssetCachePolicy.h"
#include "AssetProfileCollector.h"
#include "Async/Async.h"
#include "Engine/Engine.h"
#include "Misc/CoreDelegates.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogAssetCache, Log, All);

// ─────────────────────────────────────────────────────────────
// Constructor / Destructor (needed for TUniquePtr with forward decl)
// ─────────────────────────────────────────────────────────────

UAssetCacheManager::UAssetCacheManager() = default;
UAssetCacheManager::~UAssetCacheManager() = default;

// ─────────────────────────────────────────────────────────────
// Static accessor
// ─────────────────────────────────────────────────────────────

UAssetCacheManager* UAssetCacheManager::Get()
{
	return GEngine ? GEngine->GetEngineSubsystem<UAssetCacheManager>() : nullptr;
}

// ─────────────────────────────────────────────────────────────
// Subsystem lifecycle
// ─────────────────────────────────────────────────────────────

void UAssetCacheManager::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	ProfileCollector = MakeUnique<FAssetProfileCollector>();

	const UAssetCacheSettings* Settings = UAssetCacheSettings::Get();
	if (Settings)
	{
		ProfileCollector->SetMaxRecords(Settings->MaxProfileRecords);
	}

	InitPoliciesFromSettings();

	OnAssetLoadedHandle = FCoreUObjectDelegates::OnAssetLoaded.AddUObject(
		this, &UAssetCacheManager::OnGlobalAssetLoaded);
	OnSyncLoadPackageHandle = FCoreDelegates::OnSyncLoadPackage.AddUObject(
		this, &UAssetCacheManager::OnSyncPackageLoadStarted);
	OnAsyncLoadPackageHandle = FCoreDelegates::GetOnAsyncLoadPackage().AddUObject(
		this, &UAssetCacheManager::OnAsyncPackageLoadStarted);

	UE_LOG(LogAssetCache, Log, TEXT("UAssetCacheManager initialized. %d class policies registered."),
		ClassPolicyMap.Num());
}

void UAssetCacheManager::Deinitialize()
{
	FCoreUObjectDelegates::OnAssetLoaded.Remove(OnAssetLoadedHandle);
	FCoreDelegates::OnSyncLoadPackage.Remove(OnSyncLoadPackageHandle);
	FCoreDelegates::GetOnAsyncLoadPackage().Remove(OnAsyncLoadPackageHandle);

	FlushAllCaches();
	PendingLoadRequestPaths.Empty();
	PendingPackageLoads.Empty();

	ClassPolicyMap.Empty();
	ProfileCollector.Reset();

	UE_LOG(LogAssetCache, Log, TEXT("UAssetCacheManager deinitialized."));

	Super::Deinitialize();
}

// ─────────────────────────────────────────────────────────────
// Policy management
// ─────────────────────────────────────────────────────────────

void UAssetCacheManager::InitPoliciesFromSettings()
{
	const UAssetCacheSettings* Settings = UAssetCacheSettings::Get();
	if (!Settings) { return; }

	for (const auto& Pair : Settings->ClassPolicyMap)
	{
		UClass* ResolvedClass = Pair.Key.LoadSynchronous();
		if (!ResolvedClass) { continue; }

		if (Pair.Value.IsValid())
		{
			if (const FCachePolicyFactoryBase* Factory = Pair.Value.GetPtr<FCachePolicyFactoryBase>())
			{
				TSharedPtr<IAssetCachePolicy> Policy = Factory->Create();
				if (Policy.IsValid())
				{
					ClassPolicyMap.Add(ResolvedClass, Policy);
					UE_LOG(LogAssetCache, Log, TEXT("  Class policy: %s -> %s"),
						*ResolvedClass->GetName(), *Policy->GetDescription());
				}
			}
		}
	}
}

IAssetCachePolicy* UAssetCacheManager::FindPolicyForClass(UClass* AssetClass) const
{
	if (!AssetClass) { return nullptr; }

	UClass* TestClass = AssetClass;
	while (TestClass)
	{
		if (const TSharedPtr<IAssetCachePolicy>* Found = ClassPolicyMap.Find(TestClass))
		{
			return Found->Get();
		}
		TestClass = TestClass->GetSuperClass();
	}
	return nullptr;
}

// ─────────────────────────────────────────────────────────────
// Cache query / commit
// ─────────────────────────────────────────────────────────────

UObject* UAssetCacheManager::FindCached(const FSoftObjectPath& AssetPath) const
{
	check(IsInGameThread());

	// We need the asset's class to find the right policy.
	// If the asset is resident, we can check its class.
	// Otherwise we can't determine which policy to use → return nullptr.
	UObject* Existing = AssetPath.ResolveObject();
	if (!Existing) { return nullptr; }

	IAssetCachePolicy* Policy = FindPolicyForClass(Existing->GetClass());
	if (!Policy) { return nullptr; }

	UObject* Cached = Policy->Find(AssetPath);
	if (Cached)
	{
		Policy->OnHit(AssetPath);
	}
	return Cached;
}

void UAssetCacheManager::CommitLoadResult(const FAssetLoadContext& Context)
{
	check(IsInGameThread());

	// Record profiling
	if (ProfileCollector.IsValid())
	{
		ProfileCollector->RecordLoad(Context);
	}

	// Store in cache if we have a loaded object and a matching policy
	if (Context.Loaded && Context.Result != EAssetLoadResult::CacheHit)
	{
		IAssetCachePolicy* Policy = FindPolicyForClass(Context.Loaded->GetClass());
		if (Policy)
		{
			Policy->OnMiss(Context.AssetPath, Context.Loaded);
		}
	}
}

void UAssetCacheManager::MarkPendingLoadRequest(const FSoftObjectPath& AssetPath)
{
	check(IsInGameThread());
	PendingLoadRequestPaths.Add(AssetPath);
}

void UAssetCacheManager::UnmarkPendingLoadRequest(const FSoftObjectPath& AssetPath)
{
	check(IsInGameThread());
	PendingLoadRequestPaths.Remove(AssetPath);
}

void UAssetCacheManager::OnGlobalAssetLoaded(UObject* Asset)
{
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UAssetCacheManager> WeakThis(this);
		TWeakObjectPtr<UObject> WeakAsset(Asset);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, WeakAsset]()
		{
			if (UAssetCacheManager* Manager = WeakThis.Get())
			{
				Manager->OnGlobalAssetLoaded(WeakAsset.Get());
			}
		});
		return;
	}

	check(IsInGameThread());

	if (!Asset)
	{
		return;
	}

	const FSoftObjectPath AssetPath(Asset);
	if (!AssetPath.IsValid())
	{
		return;
	}

	if (PendingLoadRequestPaths.Contains(AssetPath))
	{
		return;
	}

	IAssetCachePolicy* Policy = FindPolicyForClass(Asset->GetClass());
	if (!Policy)
	{
		return;
	}

	if (Policy->Find(AssetPath) != nullptr)
	{
		return;
	}

	FAssetLoadContext Context;
	Context.AssetPath = AssetPath;
	Context.Loaded = Asset;
	Context.Result = EAssetLoadResult::MissAndLoad;
	Context.ProfileTier = EProfileTier::Observed;

	if (const UPackage* Package = Asset->GetOutermost())
	{
		const FName PackageName = Package->GetFName();
		if (const FObservedPackageLoadInfo* LoadInfo = PendingPackageLoads.Find(PackageName))
		{
			Context.LoadTimeSec = static_cast<float>(FPlatformTime::Seconds() - LoadInfo->StartTimeSec);
			Context.bIsAsync = LoadInfo->bIsAsync;
			PendingPackageLoads.Remove(PackageName);
		}
	}

	CommitLoadResult(Context);
}

void UAssetCacheManager::OnSyncPackageLoadStarted(const FString& PackageName)
{
	check(IsInGameThread());

	FObservedPackageLoadInfo& LoadInfo = PendingPackageLoads.FindOrAdd(FName(*PackageName));
	LoadInfo.StartTimeSec = FPlatformTime::Seconds();
	LoadInfo.bIsAsync = false;
}

void UAssetCacheManager::OnAsyncPackageLoadStarted(FStringView PackageName)
{
	check(IsInGameThread());

	FObservedPackageLoadInfo& LoadInfo = PendingPackageLoads.FindOrAdd(FName(PackageName));
	LoadInfo.StartTimeSec = FPlatformTime::Seconds();
	LoadInfo.bIsAsync = true;
}

// ─────────────────────────────────────────────────────────────
// Cache management
// ─────────────────────────────────────────────────────────────

void UAssetCacheManager::FlushAllCaches()
{
	check(IsInGameThread());

	for (auto& Pair : ClassPolicyMap)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->Flush();
		}
	}
}

// ─────────────────────────────────────────────────────────────
// Profiling
// ─────────────────────────────────────────────────────────────

float UAssetCacheManager::GetCacheHitRate() const
{
	return ProfileCollector.IsValid() ? ProfileCollector->GetHitRate() : 0.f;
}

double UAssetCacheManager::GetAverageLoadTime() const
{
	return ProfileCollector.IsValid() ? ProfileCollector->GetAverageLoadTime() : 0.0;
}

void UAssetCacheManager::DumpProfileStats() const
{
	if (ProfileCollector.IsValid())
	{
		ProfileCollector->DumpStats();
	}
}

void UAssetCacheManager::ResetProfileData()
{
	if (ProfileCollector.IsValid())
	{
		ProfileCollector->Reset();
	}
}
