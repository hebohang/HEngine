
#pragma once
#include "test.h"
#include <functional>
#include <map>
#include <memory>
#include <stdint.h>
#include <string>
#include <vector>

struct InternalTestHelper;

struct ParsedArgs
{
	LLGI::DeviceType Device = LLGI::DeviceType::Default;
	std::string Filter;
};

class TestFileReader
{
public:
	virtual ~TestFileReader() = default;
	virtual std::vector<uint8_t> Read(const char* path) = 0;
};

class TestHelper
{
private:
	static std::shared_ptr<InternalTestHelper> Get();
	static std::shared_ptr<TestFileReader> GetFileReader();

public:
	static void SetFileReader(std::shared_ptr<TestFileReader> fileReader);

	static ParsedArgs ParseArg(int argc, char* argv[]);

	static std::vector<uint8_t> LoadData(const char* path);

	static std::vector<uint8_t> LoadDataWithoutRoot(const char* path);

	static void SetRoot(const char* root);

	/**
		@brief create a rectangle
	*/
	static void CreateRectangle(LLGI::Graphics* graphics,
								const LLGI::Vec3F& ul,
								const LLGI::Vec3F& lr,
								const LLGI::Color8& cul,
								const LLGI::Color8& clr,
								std::shared_ptr<LLGI::Buffer>& vb,
								std::shared_ptr<LLGI::Buffer>& ib);

	static void CreateShader(LLGI::Graphics* graphics,
							 LLGI::DeviceType deviceType,
							 const char* vsBinaryPath,
							 const char* psBinaryPath,
							 std::shared_ptr<LLGI::Shader>& vs,
							 std::shared_ptr<LLGI::Shader>& ps);

	static void
	CreateComputeShader(LLGI::Graphics* graphics, LLGI::DeviceType deviceType, const char* csBinaryPath, std::shared_ptr<LLGI::Shader>& cs);

	static void Run(const ParsedArgs& args);

	static void RegisterTest(const char* name, std::function<void(LLGI::DeviceType)> func);

	static bool GetIsCaptureRequired();
	static void SetIsCaptureRequired(bool required);
	static void Dispose();

	static std::string GetDeviceName(LLGI::DeviceType device);
};

struct TestRegister
{
	TestRegister(const char* name, std::function<void(LLGI::DeviceType)> func) { TestHelper::RegisterTest(name, func); }
};

class Bitmap2D
{
public:
	struct Color
	{
		uint8_t r, g, b, a;
	};

	Bitmap2D(const std::vector<uint8_t>& data, int width, int height, LLGI::TextureFormatType format);

	Color GetPixel(int x, int y) const;
	void Save(const std::string& filePath);

private:
	std::vector<uint8_t> data_; // RGBA
	int width_;
	int height_;
};

#define VERIFY(x)                                                                                                                          \
	if (!(x))                                                                                                                              \
	{                                                                                                                                      \
		printf("Abort on %s : %d\n", __FILE__, __LINE__);                                                                                  \
		abort();                                                                                                                           \
	}
