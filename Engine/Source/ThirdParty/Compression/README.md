# Compression dependencies

Independent snapshot from the vendored Assimp contrib tree: zlib 1.2.13, minizip/unzip, kuba--/zip (including miniz). Licenses and upstream sources are preserved in each directory. Runtime and Cooker depend on HEArchive directly, independently of Assimp. Version updates must audit all three components and rerun archive round-trip tests.
