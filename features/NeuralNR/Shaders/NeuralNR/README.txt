Neural NR — DLL placement
=========================

Drop nvngx_dlssnr.dll into this folder as installed in the game:

    Data\Shaders\NeuralNR\nvngx_dlssnr.dll

This directory is created by the AIO shader-deploy rule so the path exists
at runtime. The feature (src/Features/NeuralNR.cpp) loads the DLL from here
and no-ops with a log line if it is missing.

Gating: NVIDIA RTX 50-series + driver >= 616 + Upscaling = DLSS active.
