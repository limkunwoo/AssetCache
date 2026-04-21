#pragma once

#include "CoreMinimal.h"
#include "AssetCacheTypes.h"

/**
 * 에셋 로드/캐시 프로파일링 데이터 수집.
 * Game Thread 전용.
 */
struct ASSETCACHE_API FAssetProfileCollector
{
	/** 로드 결과 기록 */
	void RecordLoad(const FAssetLoadContext& Context);

	/** 캐시 제거 기록 */
	void RecordEviction(const FAssetLoadContext& Context);

	/** 최근 N개의 로드 기록 반환 */
	TArray<FAssetLoadContext> GetRecentRecords(int32 Count = 50) const;

	/** 평균 로드 시간 (캐시 미스 기준, 초) */
	double GetAverageLoadTime() const;

	/** 캐시 히트율 (0.0 ~ 1.0) */
	float GetHitRate() const;

	/** 전체 기록 초기화 */
	void Reset();

	/** 콘솔에 요약 출력 */
	void DumpStats() const;

	/** 최대 보관 기록 수 */
	void SetMaxRecords(int32 InMax) { MaxRecords = InMax; }

private:
	TArray<FAssetLoadContext> Records;
	int32 MaxRecords = 1024;

	uint64 HitCount = 0;
	uint64 MissCount = 0;
	uint64 EvictionCount = 0;
};
