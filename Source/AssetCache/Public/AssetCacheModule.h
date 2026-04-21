#pragma once

#include "Modules/ModuleManager.h"

class FAssetCacheModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
