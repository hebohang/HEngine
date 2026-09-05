
## Debug

./ShaderTranspler --input /path/to/input --output /path/to/output (--vert/--frag/--comp) -S

Use `-S --vulkan-spv` when the SPIR-V is consumed directly by LLGI Vulkan.
This applies Vulkan coordinate conversion and descriptor set remapping.

https://www.khronos.org/spir/visualizer/
