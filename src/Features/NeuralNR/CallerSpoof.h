#pragma once

namespace CSS::CallerSpoof
{
	void Install();
	void Uninstall();
	
	void InstallUpscalerHooks();
	bool InstallActiveInterceptors();
}

