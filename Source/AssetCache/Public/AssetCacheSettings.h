#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "AssetCachePolicy.h"
#include "StructUtils/InstancedStruct.h"
#include "AssetCacheSettings.generated.h"

// ─────────────────────────────────────────────────────────────
// 캐시 정책 팩토리 (프로젝트 세팅에서 UPROPERTY로 노출)
// ─────────────────────────────────────────────────────────────

/**
 * 캐시 정책 팩토리 베이스.
 * FInstancedStruct로 프로젝트 세팅에서 다형적으로 사용됩니다.
 */
USTRUCT(BlueprintType)
struct ASSETCACHE_API FCachePolicyFactoryBase
{
	GENERATED_BODY()

	virtual ~FCachePolicyFactoryBase() = default;

	/** 캐시 정책 인스턴스 생성 */
	virtual TSharedPtr<IAssetCachePolicy> Create() const
	{
		return MakeShared<FNoCachePolicy>();
	}
};

/** LRU 캐시 정책 팩토리 */
USTRUCT(BlueprintType, DisplayName = "LRU Cache Policy")
struct ASSETCACHE_API FCachePolicyFactory_LRU : public FCachePolicyFactoryBase
{
	GENERATED_BODY()

	/** 최대 캐시 항목 수 */
	UPROPERTY(config, EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1"))
	int32 Capacity = 256;

	virtual TSharedPtr<IAssetCachePolicy> Create() const override
	{
		return MakeShared<FCachePolicy_LRU>(Capacity);
	}
};

// ─────────────────────────────────────────────────────────────
// 프로젝트 세팅 (UDeveloperSettings)
// ─────────────────────────────────────────────────────────────

/**
 * AssetCache 프로젝트 세팅.
 * Project Settings > Plugins > Asset Cache 에서 편집 가능.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Asset Cache"))
class ASSETCACHE_API UAssetCacheSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UAssetCacheSettings();

	/**
	 * 클래스별 캐시 정책 매핑.
	 * 매핑되지 않은 클래스의 에셋은 캐시하지 않습니다.
	 * 예: UStaticMesh → LRU(512), UTexture2D → LRU(1024)
	 */
	UPROPERTY(config, EditAnywhere, Category = "Cache Policy",
		meta = (BaseStruct = "/Script/AssetCache.CachePolicyFactoryBase"))
	TMap<TSoftClassPtr<UObject>, FInstancedStruct> ClassPolicyMap;

	/** 프로파일링 활성화 여부 */
	UPROPERTY(config, EditAnywhere, Category = "Profiling")
	bool bEnableProfiling = true;

	/** 프로파일 기록 최대 보관 수 */
	UPROPERTY(config, EditAnywhere, Category = "Profiling",
		meta = (ClampMin = "100", EditCondition = "bEnableProfiling"))
	int32 MaxProfileRecords = 1024;

	// UDeveloperSettings interface
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	/** 싱글턴 접근 */
	static const UAssetCacheSettings* Get();
};
