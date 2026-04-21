#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"
#include "AssetLoadRequest.h"

class UAssetCacheManager;

/**
 * namespace Resource — 에셋 로딩 진입점.
 *
 * Single-path:
 *   Resource::Load("/Game/Meshes/SM_Chair").Get<UStaticMesh>();
 *   Resource::Load(SoftPath).SkipCache().OnComplete<UTexture2D>([](UTexture2D* T){ });
 *
 * Multi-path:
 *   Resource::Load({P1, P2, P3}).GetAll<UStaticMesh>();
 *   Resource::Load(SoftPtrArray).OnCompleteAll<UTexture2D>([](TArray<UTexture2D*> Ts){ });
 */
namespace Resource
{
	// ─── Single-path ────────────────────────────────────────

	ASSETCACHE_API FLoadRequest Load(const FSoftObjectPath& InPath);
	ASSETCACHE_API FLoadRequest Load(const TCHAR* InPath);

	template<typename T>
	FLoadRequest Load(const TSoftObjectPtr<T>& InPtr)
	{
		return Load(InPtr.ToSoftObjectPath());
	}

	// ─── Multi-path ─────────────────────────────────────────

	ASSETCACHE_API FLoadRequest Load(TArray<FSoftObjectPath> InPaths);

	template<typename T>
	FLoadRequest Load(const TArray<TSoftObjectPtr<T>>& InPtrs)
	{
		TArray<FSoftObjectPath> Paths;
		Paths.Reserve(InPtrs.Num());
		for (const TSoftObjectPtr<T>& Ptr : InPtrs)
		{
			Paths.Add(Ptr.ToSoftObjectPath());
		}
		return Load(MoveTemp(Paths));
	}
}
