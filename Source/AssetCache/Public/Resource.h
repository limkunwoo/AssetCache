#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"
#include "AssetLoadRequest.h"

class UAssetCacheManager;

/**
 * namespace Resource — 에셋 로딩 진입점.
 *
 * 사용 예:
 *   Resource::Load("/Game/Meshes/SM_Chair").Get<UStaticMesh>();
 *   Resource::Load(SoftPath).SkipCache().OnComplete<UTexture2D>([](auto* T, auto& Ctx){ });
 *   Resource::Load(Path).AbortIfInvalid(this).Then<UAnimSequence>();
 */
namespace Resource
{
	/** FSoftObjectPath로 로드 요청 빌더 생성 */
	ASSETCACHE_API FLoadRequest Load(const FSoftObjectPath& InPath);

	/** 문자열 경로로 로드 요청 빌더 생성 */
	ASSETCACHE_API FLoadRequest Load(const TCHAR* InPath);

	/** TSoftObjectPtr에서 로드 요청 빌더 생성 */
	template<typename T>
	FLoadRequest Load(const TSoftObjectPtr<T>& InPtr)
	{
		return Load(InPtr.ToSoftObjectPath());
	}
}
