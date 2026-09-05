# HEngine NVRHI source record

- Upstream: https://github.com/NVIDIA-RTX/NVRHI
- Commit: `8e8c36e37558acec333204619b95d9d2fcdc4a79`
- Snapshot date: 2026-08-13
- License: MIT (`LICENSE.txt`)

This directory is a source snapshot with upstream Git metadata and build artifacts excluded. HEngine configures only the static NVRHI core and Direct3D 11 backend; Direct3D 12, Vulkan, NVAPI, Nsight Aftermath, RTXMU, and install rules are disabled.

HEngine compatibility patch: the enabled Direct3D 11 backend uses `LESS_EQUAL` for comparison samplers, preserving the existing shadow sampler behavior.
