#include <zip.h>
#include <unzip.h>
#if HE_TEST_ASSIMP
#include <assimp/Importer.hpp>
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstring>

using namespace std::string_literals;

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv)
{
    try
    {
        Require(argc == 2, "Provide a scratch directory");
        const auto root = std::filesystem::absolute(argv[1]);
        std::filesystem::create_directories(root);
        const auto path = (root / "archive-roundtrip.zip").string();
        const std::string payload = "HEngine archive contract\nembedded\0bytes"s;
        auto* writer = zip_open(path.c_str(), 6, 'w');
        Require(writer != nullptr, "zip writer unavailable");
        Require(zip_entry_open(writer, "Content/roundtrip.bin") == 0, "entry open failed");
        Require(zip_entry_write(writer, payload.data(), payload.size()) == 0, "entry write failed");
        zip_entry_close(writer);
        zip_close(writer);
        auto reader = unzOpen(path.c_str());
        Require(reader != nullptr, "minizip cannot read zip output");
        Require(unzLocateFile(reader, "Content/roundtrip.bin", 1) == UNZ_OK, "entry missing");
        Require(unzOpenCurrentFile(reader) == UNZ_OK, "entry cannot be opened");
        std::string restored(payload.size(), '\0');
        Require(unzReadCurrentFile(reader, restored.data(), static_cast<unsigned>(restored.size())) == restored.size(), "short read");
        Require(unzCloseCurrentFile(reader) == UNZ_OK, "CRC failure");
        unzClose(reader);
        Require(restored == payload, "archive payload changed");
        const auto corrupt = (root / "corrupt.zip").string();
        std::ofstream(corrupt, std::ios::binary) << "invalid archive";
        Require(unzOpen(corrupt.c_str()) == nullptr, "invalid archive accepted");
#if HE_TEST_ASSIMP
        Assimp::Importer importer;
        for (const auto* extension : {"fbx", "gltf", "glb", "obj", "dae", "blend"})
            Require(importer.IsExtensionSupported(extension), "supported importer missing");
        for (const auto* extension : {"3ds", "ifc", "stl", "ply", "x", "md2"})
            Require(!importer.IsExtensionSupported(extension), "unused importer still enabled");
        const auto obj = (root / "triangle.obj").string();
        std::ofstream(obj) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        Require(importer.ReadFile(obj, 0) != nullptr, "OBJ import failed");
#endif
        std::cout << "PASS archive roundtrip and corrupt rejection\n";
#if HE_TEST_ASSIMP
        std::cout << "PASS enabled importer contract and OBJ import\n";
#endif
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
