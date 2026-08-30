#pragma once

namespace CSS::CallerSpoof
{
	void Install();
	void Uninstall();
	void InstallStreamlineHooks();
	bool TryHookNGX(); // Dynamically probes Streamline
}