#include "AssetCachePolicy.h"
#include "HAL/PlatformTime.h"

// ─────────────────────────────────────────────────────────────
// FCachePolicy_LRU
// ─────────────────────────────────────────────────────────────

FCachePolicy_LRU::FCachePolicy_LRU(int32 InCapacity)
	: Capacity(FMath::Max(1, InCapacity))
{
}

UObject* FCachePolicy_LRU::Find(const FSoftObjectPath& AssetPath)
{
	check(IsInGameThread());

	if (FLRUEntry* Entry = CacheMap.Find(AssetPath))
	{
		if (Entry->Asset.IsValid())
		{
			return Entry->Asset.Get();
		}
		// WeakPtr expired — clean up
		CacheMap.Remove(AssetPath);
		if (auto* Node = LRUList.FindNode(AssetPath))
		{
			LRUList.RemoveNode(Node);
		}
	}
	return nullptr;
}

void FCachePolicy_LRU::OnHit(const FSoftObjectPath& AssetPath)
{
	check(IsInGameThread());

	if (FLRUEntry* Entry = CacheMap.Find(AssetPath))
	{
		Entry->LastAccessTime = FPlatformTime::Seconds();

		// Move to tail (most recent)
		if (auto* Node = LRUList.FindNode(AssetPath))
		{
			LRUList.RemoveNode(Node);
		}
		LRUList.AddTail(AssetPath);
	}
}

void FCachePolicy_LRU::OnMiss(const FSoftObjectPath& AssetPath, UObject* LoadedAsset)
{
	if (!LoadedAsset) { return; }

	check(IsInGameThread());

	// Already exists — update
	if (FLRUEntry* Existing = CacheMap.Find(AssetPath))
	{
		Existing->Asset = LoadedAsset;
		Existing->LastAccessTime = FPlatformTime::Seconds();
		return;
	}

	// Evict if at capacity
	while (CacheMap.Num() >= Capacity)
	{
		EvictLeastRecentlyUsed();
	}

	FLRUEntry NewEntry;
	NewEntry.Asset = LoadedAsset;
	NewEntry.LastAccessTime = FPlatformTime::Seconds();

	CacheMap.Add(AssetPath, NewEntry);
	LRUList.AddTail(AssetPath);
}

bool FCachePolicy_LRU::Evict(const FSoftObjectPath& AssetPath)
{
	check(IsInGameThread());

	if (CacheMap.Remove(AssetPath) > 0)
	{
		if (auto* Node = LRUList.FindNode(AssetPath))
		{
			LRUList.RemoveNode(Node);
		}
		return true;
	}
	return false;
}

void FCachePolicy_LRU::Flush()
{
	check(IsInGameThread());
	CacheMap.Empty();
	LRUList.Empty();
}

int32 FCachePolicy_LRU::GetNum() const
{
	check(IsInGameThread());
	return CacheMap.Num();
}

FString FCachePolicy_LRU::GetDescription() const
{
	return FString::Printf(TEXT("LRU (Capacity=%d, Num=%d)"), Capacity, GetNum());
}

void FCachePolicy_LRU::EvictLeastRecentlyUsed()
{
	if (auto* HeadNode = LRUList.GetHead())
	{
		const FSoftObjectPath& OldestKey = HeadNode->GetValue();
		CacheMap.Remove(OldestKey);
		LRUList.RemoveNode(HeadNode);
	}
}
