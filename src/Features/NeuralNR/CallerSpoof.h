#pragma once

#include <windows.h>

namespace CSS::CallerSpoof
{
	void Install();
	void Uninstall();
	
	void InstallUpscalerHooks();
	bool InstallActiveInterceptors();
}