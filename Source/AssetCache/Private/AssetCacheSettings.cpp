#include "AssetCacheSettings.h"

UAssetCacheSettings::UAssetCacheSettings()
{
}

const UAssetCacheSettings* UAssetCacheSettings::Get()
{
	return GetDefault<UAssetCacheSettings>();
}
