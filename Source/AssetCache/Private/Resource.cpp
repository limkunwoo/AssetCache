#include "Resource.h"
#include "AssetCacheManager.h"

namespace Resource
{
	FLoadRequest Load(const FSoftObjectPath& InPath)
	{
		return FLoadRequest(UAssetCacheManager::Get(), InPath);
	}

	FLoadRequest Load(const TCHAR* InPath)
	{
		return Load(FSoftObjectPath(InPath));
	}

	FLoadRequest Load(TArray<FSoftObjectPath> InPaths)
	{
		return FLoadRequest(UAssetCacheManager::Get(), MoveTemp(InPaths));
	}
}
