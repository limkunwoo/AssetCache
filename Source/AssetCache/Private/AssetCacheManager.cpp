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

	if (const UAssetCacheSettings* Settings = UAssetCacheSettings::Get())
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
	if (!Settings)
	{
		return;
	}

	for (const auto& Pair : Settings->ClassPolicyMap)
	{
		UClass* ResolvedClass = Pair.Key.LoadSynchronous();
		if (!ResolvedClass)
		{
			continue;
		}

		if (!Pair.Value.IsValid())
		{
			continue;
		}

		const FCachePolicyFactoryBase* Factory = Pair.Value.GetPtr<FCachePolicyFactoryBase>();
		if (!Factory)
		{
			continue;
		}

		TSharedPtr<IAssetCachePolicy> Policy = Factory->Create();
		if (!Policy.IsValid())
		{
			continue;
		}

		ClassPolicyMap.Add(ResolvedClass, Policy);
		UE_LOG(LogAssetCache, Log, TEXT("  Class policy: %s -> %s"),
			*ResolvedClass->GetName(), *Policy->GetDescription());
	}
}

IAssetCachePolicy* UAssetCacheManager::FindPolicyForClass(UClass* AssetClass) const
{
	for (UClass* TestClass = AssetClass; TestClass; TestClass = TestClass->GetSuperClass())
	{
		if (const TSharedPtr<IAssetCachePolicy>* Found = ClassPolicyMap.Find(TestClass))
		{
			return Found->Get();
		}
	}
	return nullptr;
}

// ─────────────────────────────────────────────────────────────
// Cache query / commit
// ─────────────────────────────────────────────────────────────

UObject* UAssetCacheManager::FindCached(const FSoftObjectPath& AssetPath) const
{
	check(IsInGameThread());

	// 정책 lookup에 클래스가 필요 → resident가 아니면 어떤 정책을 적용할지 모르므로 미스 처리.
	UObject* Existing = AssetPath.ResolveObject();
	if (!Existing)
	{
		return nullptr;
	}

	IAssetCachePolicy* Policy = FindPolicyForClass(Existing->GetClass());
	if (!Policy)
	{
		return nullptr;
	}

	UObject* Cached = Policy->Find(AssetPath);
	if (!Cached)
	{
		return nullptr;
	}

	Policy->OnHit(AssetPath);
	return Cached;
}

void UAssetCacheManager::CommitLoadResult(const FAssetLoadContext& Context)
{
	check(IsInGameThread());

	// 1) 프로파일 기록
	if (ProfileCollector.IsValid())
	{
		ProfileCollector->RecordLoad(Context);
	}

	// 2) 캐시 저장 — Loaded 없거나 이미 CacheHit이면 skip
	if (!Context.Loaded || Context.Result == EAssetLoadResult::CacheHit)
	{
		return;
	}

	IAssetCachePolicy* Policy = FindPolicyForClass(Context.Loaded->GetClass());
	if (!Policy)
	{
		return;
	}

	Policy->OnMiss(Context.AssetPath, Context.Loaded);
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
	// 비게임스레드에서 들어오면 게임스레드로 던지고 종료
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

	if (!Asset)
	{
		return;
	}

	const FSoftObjectPath AssetPath(Asset);
	if (!AssetPath.IsValid())
	{
		return;
	}

	// FLoadRequest 경로에서 이미 처리 중이면 중복 기록 방지
	if (PendingLoadRequestPaths.Contains(AssetPath))
	{
		return;
	}

	IAssetCachePolicy* Policy = FindPolicyForClass(Asset->GetClass());
	if (!Policy)
	{
		return;
	}

	// 이미 캐시에 있는 동일 path면 skip
	if (Policy->Find(AssetPath) != nullptr)
	{
		return;
	}

	FAssetLoadContext Context;
	Context.AssetPath = AssetPath;
	Context.Loaded = Asset;
	Context.Result = EAssetLoadResult::MissAndLoad;
	Context.ProfileTier = EProfileTier::Observed;

	// 패키지 시작 이벤트가 있었다면 elapsed/async 정보 채우기
	const UPackage* Package = Asset->GetOutermost();
	const FName PackageName = Package ? Package->GetFName() : NAME_None;
	if (FObservedPackageLoadInfo* LoadInfo = (PackageName.IsNone() ? nullptr : PendingPackageLoads.Find(PackageName)))
	{
		Context.LoadTimeSec = static_cast<float>(FPlatformTime::Seconds() - LoadInfo->StartTimeSec);
		Context.bIsAsync = LoadInfo->bIsAsync;
		PendingPackageLoads.Remove(PackageName);
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
		if (!Pair.Value.IsValid())
		{
			continue;
		}
		Pair.Value->Flush();
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
	if (!ProfileCollector.IsValid())
	{
		return;
	}
	ProfileCollector->DumpStats();
}

void UAssetCacheManager::ResetProfileData()
{
	if (!ProfileCollector.IsValid())
	{
		return;
	}
	ProfileCollector->Reset();
}
