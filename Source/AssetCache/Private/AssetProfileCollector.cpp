#include "AssetProfileCollector.h"

DEFINE_LOG_CATEGORY_STATIC(LogAssetProfile, Log, All);

void FAssetProfileCollector::RecordLoad(const FAssetLoadContext& Context)
{
	check(IsInGameThread());

	if (Context.Result == EAssetLoadResult::CacheHit)
	{
		++HitCount;
	}
	else
	{
		++MissCount;
	}

	if (Records.Num() >= MaxRecords)
	{
		Records.RemoveAt(0, 1, EAllowShrinking::No);
	}
	Records.Add(Context);
}

void FAssetProfileCollector::RecordEviction(const FAssetLoadContext& Context)
{
	check(IsInGameThread());
	++EvictionCount;

	UE_LOG(LogAssetProfile, Verbose, TEXT("Cache eviction: %s"), *Context.AssetPath.ToString());
}

TArray<FAssetLoadContext> FAssetProfileCollector::GetRecentRecords(int32 Count) const
{
	check(IsInGameThread());

	const int32 StartIdx = FMath::Max(0, Records.Num() - Count);
	TArray<FAssetLoadContext> Result;
	Result.Reserve(Records.Num() - StartIdx);
	for (int32 i = StartIdx; i < Records.Num(); ++i)
	{
		Result.Add(Records[i]);
	}
	return Result;
}

double FAssetProfileCollector::GetAverageLoadTime() const
{
	check(IsInGameThread());

	double Total = 0.0;
	int32 Count = 0;

	for (const FAssetLoadContext& R : Records)
	{
		if (R.Result != EAssetLoadResult::CacheHit)
		{
			Total += R.LoadTimeSec;
			++Count;
		}
	}

	return (Count > 0) ? (Total / static_cast<double>(Count)) : 0.0;
}

float FAssetProfileCollector::GetHitRate() const
{
	check(IsInGameThread());
	const uint64 Total = HitCount + MissCount;
	return (Total > 0) ? static_cast<float>(HitCount) / static_cast<float>(Total) : 0.f;
}

void FAssetProfileCollector::Reset()
{
	check(IsInGameThread());
	Records.Empty();
	HitCount = 0;
	MissCount = 0;
	EvictionCount = 0;
}

void FAssetProfileCollector::DumpStats() const
{
	check(IsInGameThread());

	const uint64 Total = HitCount + MissCount;
	const float Rate = (Total > 0) ? (static_cast<float>(HitCount) / static_cast<float>(Total)) : 0.f;
	int32 PreciseCount = 0;
	int32 ObservedCount = 0;

	for (const FAssetLoadContext& Record : Records)
	{
		if (Record.ProfileTier == EProfileTier::Observed)
		{
			++ObservedCount;
		}
		else
		{
			++PreciseCount;
		}
	}

	UE_LOG(LogAssetProfile, Log, TEXT("===== AssetCache Profile ====="));
	UE_LOG(LogAssetProfile, Log, TEXT("  Total requests : %llu"), Total);
	UE_LOG(LogAssetProfile, Log, TEXT("  Precise records: %d"), PreciseCount);
	UE_LOG(LogAssetProfile, Log, TEXT("  Observed records: %d"), ObservedCount);
	UE_LOG(LogAssetProfile, Log, TEXT("  Cache hits     : %llu"), HitCount);
	UE_LOG(LogAssetProfile, Log, TEXT("  Cache misses   : %llu"), MissCount);
	UE_LOG(LogAssetProfile, Log, TEXT("  Evictions      : %llu"), EvictionCount);
	UE_LOG(LogAssetProfile, Log, TEXT("  Hit rate       : %.2f%%"), Rate * 100.f);
	UE_LOG(LogAssetProfile, Log, TEXT("  Avg load time  : %.4f sec (misses only)"), GetAverageLoadTime());
	UE_LOG(LogAssetProfile, Log, TEXT("=============================="));
}
