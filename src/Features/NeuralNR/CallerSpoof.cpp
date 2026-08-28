// ISOLATED SEAM — intentionally a no-op. This is the single point where the
// NGX per-title caller-verification check would be defeated. That bypass is
// deliberately NOT provided here. If you have your own working implementation,
// replace this body behind the same Install()/Uninstall() API. Otherwise
// CreateFeature fails at the gate and the feature disables — signalled below.
#include "CallerSpoof.h"

namespace CSS::CallerSpoof
{
	void Install()
	{
		logger::info("NeuralNR: caller-spoof NOT installed (no-op seam). "
		           "If CreateFeature fails on caller verification, this is why.");
	}
	void Uninstall() {}
}
