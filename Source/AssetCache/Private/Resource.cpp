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
}
