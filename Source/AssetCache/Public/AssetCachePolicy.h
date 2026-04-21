#pragma once

#include "CoreMinimal.h"
#include "AssetCacheTypes.h"

/**
 * 캐시 정책 인터페이스.
 * 캐시 항목의 삽입/조회/제거 전략을 정의합니다.
 */
class ASSETCACHE_API IAssetCachePolicy : public TSharedFromThis<IAssetCachePolicy>
{
public:
	virtual ~IAssetCachePolicy() = default;

	/** 캐시에서 에셋 조회. 없으면 nullptr */
	virtual UObject* Find(const FSoftObjectPath& AssetPath) = 0;

	/** 캐시 히트 시 호출 (LRU 갱신 등) */
	virtual void OnHit(const FSoftObjectPath& AssetPath) = 0;

	/** 캐시 미스 후 로드 완료 시 호출 — 새 항목 삽입 */
	virtual void OnMiss(const FSoftObjectPath& AssetPath, UObject* LoadedAsset) = 0;

	/** 특정 에셋을 정책에서 제거 */
	virtual bool Evict(const FSoftObjectPath& AssetPath) = 0;

	/** 전체 캐시 비움 */
	virtual void Flush() = 0;

	/** 현재 캐시된 항목 수 */
	virtual int32 GetNum() const = 0;

	/** 최대 용량 */
	virtual int32 GetCapacity() const = 0;

	/** 정책 설명 문자열 */
	virtual FString GetDescription() const = 0;
};

// ─────────────────────────────────────────────────────────────
// FNoCachePolicy — 캐시하지 않는 정책
// ─────────────────────────────────────────────────────────────

class ASSETCACHE_API FNoCachePolicy : public IAssetCachePolicy
{
public:
	virtual UObject* Find(const FSoftObjectPath&) override { return nullptr; }
	virtual void OnHit(const FSoftObjectPath&) override {}
	virtual void OnMiss(const FSoftObjectPath&, UObject*) override {}
	virtual bool Evict(const FSoftObjectPath&) override { return false; }
	virtual void Flush() override {}
	virtual int32 GetNum() const override { return 0; }
	virtual int32 GetCapacity() const override { return 0; }
	virtual FString GetDescription() const override { return TEXT("NoCache"); }
};

// ─────────────────────────────────────────────────────────────
// FCachePolicy_LRU — LRU 기반 캐시 정책
// ─────────────────────────────────────────────────────────────

class ASSETCACHE_API FCachePolicy_LRU : public IAssetCachePolicy
{
public:
	explicit FCachePolicy_LRU(int32 InCapacity = 256);

	virtual UObject* Find(const FSoftObjectPath& AssetPath) override;
	virtual void OnHit(const FSoftObjectPath& AssetPath) override;
	virtual void OnMiss(const FSoftObjectPath& AssetPath, UObject* LoadedAsset) override;
	virtual bool Evict(const FSoftObjectPath& AssetPath) override;
	virtual void Flush() override;
	virtual int32 GetNum() const override;
	virtual int32 GetCapacity() const override { return Capacity; }
	virtual FString GetDescription() const override;

private:
	struct FLRUEntry
	{
		TWeakObjectPtr<UObject> Asset;
		double LastAccessTime = 0.0;
	};

	void EvictLeastRecentlyUsed();

	int32 Capacity;
	TMap<FSoftObjectPath, FLRUEntry> CacheMap;
	TDoubleLinkedList<FSoftObjectPath> LRUList;
};
