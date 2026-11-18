// Portions Copyright 2026 The Hollycast Authors
#include "rend/TexCache.h"
#include "util/worker_thread.h"
#include "emulator.h"
#include "hw/mem/addrspace.h"
#include "hw/pvr/pvr_mem.h"
#include <gtest/gtest.h>
#include <stb_image.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>

namespace
{
class DumpTexture : public BaseTextureCacheData
{
public:
	DumpTexture(u32 hash, TCW control = {}) : BaseTextureCacheData(TSP{}, control, 0)
	{
		texture_hash = hash;
		tex_type = TextureType::_8888;
	}
	~DumpTexture() override { unprotectVRam(); }
	std::string GetId() override { return "atlas-dump-test"; }
	void UploadToGPU(int width, int height, const u8 *, bool, bool) override
	{
		uploadedWidth = width;
		uploadedHeight = height;
		uploadedType = tex_type;
		if (onUpload)
			onUpload();
	}
	bool Force32BitTexture(TextureType) const override { return force32; }
	int uploadedWidth = 0, uploadedHeight = 0;
	TextureType uploadedType = TextureType::_8888;
	bool force32 = false;
	std::function<void()> onUpload;
};

std::string readText(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class CustomTextureDumpTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		oldDump = config::DumpTextures.get();
		oldSplit = config::SplitTextureSheets.get();
		oldPath = config::TextureDumpPath.get();
		oldGameId = settings.content.gameId;
		oldRenderer = config::RendererType.get();
		const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
		root = std::filesystem::temp_directory_path() / ("hollycast-atlas-dump-" + std::to_string(nonce));
		std::filesystem::create_directories(root);
		config::TextureDumpPath = root.string();
		config::DumpTextures = true;
		config::SplitTextureSheets = true;
		config::RendererType = RenderType::OpenGL;
		settings.content.gameId = "ATLAS-TEST";
	}

	void TearDown() override
	{
		config::DumpTextures = oldDump;
		config::SplitTextureSheets = oldSplit;
		config::TextureDumpPath = oldPath;
		config::RendererType = oldRenderer;
		settings.content.gameId = oldGameId;
		std::filesystem::remove_all(root);
	}

	void dump(DumpTexture& texture, int size)
	{
		std::vector<u8> pixels(size_t(size) * size * 4, 255);
		dumper->dumpTexture(&texture, size, size, pixels.data());
	}

	void extract(DumpTexture& texture, float maxV = 0.5f, CustomTexture *target = nullptr)
	{
		rend_context context {};
		context.verts.resize(4);
		context.verts[0].u = 0;
		context.verts[0].v = 0;
		context.verts[1].u = 0.5f;
		context.verts[1].v = 0;
		context.verts[2].u = 0;
		context.verts[2].v = maxV;
		context.verts[3].u = 0.5f;
		context.verts[3].v = maxV;
		context.global_param_op.emplace_back();
		auto& polygon = context.global_param_op.back();
		polygon.init();
		polygon.texture = &texture;
		polygon.pcw.Texture = 1;
		polygon.count = 4;
		RenderPass pass {};
		pass.op_count = 1;
		(target != nullptr ? target : dumper.get())->dumpTextureRegions(context, RenderPass{}, pass);
	}

	std::filesystem::path titleDirectory() const
	{
		return std::filesystem::path(config::TextureDumpPath.get()) / "ATLAS-TEST";
	}

	std::unique_ptr<CustomTexture> dumper = std::make_unique<CustomTexture>();
	std::filesystem::path root;
	std::string oldPath, oldGameId;
	RenderType oldRenderer {};
	bool oldDump = false, oldSplit = false;
};

class NativeTextureDumpTest : public CustomTextureDumpTest
{
protected:
	void SetUp() override
	{
		if (!addrspace::reserve())
			die("addrspace::reserve failed");
		emu.init();
		CustomTextureDumpTest::SetUp();
		oldUpscale = config::TextureUpscale.get();
		oldFilterSize = config::MaxFilteredTextureSize.get();
		oldCustom = config::CustomTextures.get();
		oldUnique = config::DumpUniqueTextures.get();
		config::CustomTextures = false;
		config::DumpUniqueTextures = false;
		config::MaxFilteredTextureSize = 1024;
		addrspace::unprotectVram(0, PAGE_SIZE);
		std::copy_n(&vram[0], savedVram.size(), savedVram.begin());
		for (size_t i = 0; i < savedVram.size(); i++)
			vram[i] = static_cast<u8>(i * 17 + 31);
		custom_texture.terminate();
		custom_texture.init();
	}

	void TearDown() override
	{
		custom_texture.terminate();
		addrspace::unprotectVram(0, PAGE_SIZE);
		std::copy(savedVram.begin(), savedVram.end(), &vram[0]);
		config::TextureUpscale = oldUpscale;
		config::MaxFilteredTextureSize = oldFilterSize;
		config::CustomTextures = oldCustom;
		config::DumpUniqueTextures = oldUnique;
		CustomTextureDumpTest::TearDown();
	}

	std::array<u8, 128> savedVram {};
	int oldUpscale = 1, oldFilterSize = 0;
	bool oldCustom = false, oldUnique = false;
};
}

TEST_F(CustomTextureDumpTest, RedumpingPreservesTheExistingParentAndEditedPieces)
{
	DumpTexture texture(0x123);
	dump(texture, 8);
	extract(texture);
	const auto parent = titleDirectory() / "123.png";
	const auto mapping = titleDirectory() / texture_atlas::MappingFilename;
	const auto before = readText(parent);
	const auto sheets = texture_atlas::parseMapping(readText(mapping));
	ASSERT_EQ(sheets.size(), 1u);
	ASSERT_FALSE(sheets[0].regions.empty());
	const auto piece = titleDirectory() / texture_atlas::regionFilename(texture.texture_hash, sheets[0].regions[0]);
	std::ofstream(piece, std::ios::binary) << "user edited piece";
	dump(texture, 16);
	extract(texture);
	EXPECT_EQ(readText(parent), before);
	EXPECT_EQ(readText(piece), "user edited piece");
	const auto after = texture_atlas::parseMapping(readText(mapping));
	ASSERT_EQ(after.size(), 1u);
	EXPECT_EQ(after[0].width, 8u);
}

TEST_F(CustomTextureDumpTest, ChangingDirectoriesStartsAnIndependentMappingAndReexportsPieces)
{
	DumpTexture first(0x123), second(0x456);
	dump(first, 8);
	extract(first);
	config::TextureDumpPath = (root / "other").string();
	extract(first);
	dump(second, 8);
	extract(second);
	const auto mapping = titleDirectory() / texture_atlas::MappingFilename;
	auto sheets = texture_atlas::parseMapping(readText(mapping));
	ASSERT_EQ(sheets.size(), 1u);
	EXPECT_EQ(sheets[0].hash, second.texture_hash);
	dump(first, 8);
	extract(first);
	sheets = texture_atlas::parseMapping(readText(mapping));
	ASSERT_EQ(sheets.size(), 2u);
	for (const auto& sheet : sheets)
	{
		EXPECT_TRUE(std::filesystem::exists(titleDirectory() / texture_atlas::imageFilename(sheet.hash)));
		for (const auto& region : sheet.regions)
			EXPECT_TRUE(std::filesystem::exists(titleDirectory() / texture_atlas::regionFilename(sheet.hash, region)));
	}
}

TEST_F(CustomTextureDumpTest, InvalidExistingImagesArePreserved)
{
	DumpTexture texture(0x123);
	std::filesystem::create_directories(titleDirectory());
	const auto parent = titleDirectory() / "123.png";
	std::ofstream(parent, std::ios::binary) << "invalid PNG fixture";
	dump(texture, 8);
	EXPECT_EQ(readText(parent), "invalid PNG fixture");
	EXPECT_FALSE(std::filesystem::exists(titleDirectory() / texture_atlas::MappingFilename));
}

TEST_F(CustomTextureDumpTest, RestartSkipsExistingFilesAndOnlyRestoresMissingPieces)
{
	DumpTexture texture(0x123);
	dump(texture, 8);
	extract(texture);
	const auto parent = titleDirectory() / "123.png";
	const auto mapping = titleDirectory() / texture_atlas::MappingFilename;
	const auto sheets = texture_atlas::parseMapping(readText(mapping));
	ASSERT_EQ(sheets.size(), 1u);
	ASSERT_EQ(sheets[0].regions.size(), 1u);
	const auto piece = titleDirectory() / texture_atlas::regionFilename(texture.texture_hash, sheets[0].regions[0]);
	const auto parentBytes = readText(parent), pieceBytes = readText(piece), mappingBytes = readText(mapping);
	const auto oldTime = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
	for (const auto& path : {parent, piece, mapping})
		std::filesystem::last_write_time(path, oldTime);
	const auto parentTime = std::filesystem::last_write_time(parent);
	const auto pieceTime = std::filesystem::last_write_time(piece);
	const auto mappingTime = std::filesystem::last_write_time(mapping);

	dumper = std::make_unique<CustomTexture>();
	dump(texture, 8);
	extract(texture);
	EXPECT_EQ(readText(parent), parentBytes);
	EXPECT_EQ(readText(piece), pieceBytes);
	EXPECT_EQ(readText(mapping), mappingBytes);
	EXPECT_EQ(std::filesystem::last_write_time(parent), parentTime);
	EXPECT_EQ(std::filesystem::last_write_time(piece), pieceTime);
	EXPECT_EQ(std::filesystem::last_write_time(mapping), mappingTime);
	EXPECT_EQ(std::distance(std::filesystem::directory_iterator(titleDirectory()), std::filesystem::directory_iterator()), 3);

	std::filesystem::remove(piece);
	dumper = std::make_unique<CustomTexture>();
	dump(texture, 8);
	extract(texture);
	EXPECT_EQ(readText(piece), pieceBytes);
	EXPECT_EQ(readText(mapping), mappingBytes);
	EXPECT_EQ(std::filesystem::last_write_time(mapping), mappingTime);
	EXPECT_EQ(std::filesystem::last_write_time(parent), parentTime);
}

TEST_F(CustomTextureDumpTest, IndependentInstancesKeepTheSameNamesWhenCropsArriveInOppositeOrders)
{
	DumpTexture texture(0x123);
	dump(texture, 8);
	extract(texture, 0.375f);
	const auto firstDirectory = titleDirectory();
	const auto mapping = firstDirectory / texture_atlas::MappingFilename;
	const auto first = texture_atlas::parseMapping(readText(mapping));
	ASSERT_EQ(first.size(), 1u);
	ASSERT_EQ(first[0].regions.size(), 1u);
	const auto partial = firstDirectory / texture_atlas::regionFilename(texture.texture_hash, first[0].regions[0]);
	std::ofstream(partial, std::ios::binary) << "user edited partial crop";
	const auto stamp = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
	std::filesystem::last_write_time(partial, stamp);
	const auto partialTime = std::filesystem::last_write_time(partial);

	dumper = std::make_unique<CustomTexture>();
	dump(texture, 8);
	extract(texture);
	const auto extended = texture_atlas::parseMapping(readText(mapping));
	ASSERT_EQ(extended.size(), 1u);
	ASSERT_EQ(extended[0].regions.size(), 2u);
	EXPECT_NE(std::find(extended[0].regions.begin(), extended[0].regions.end(), first[0].regions[0]), extended[0].regions.end());
	EXPECT_EQ(readText(partial), "user edited partial crop");
	EXPECT_EQ(std::filesystem::last_write_time(partial), partialTime);

	config::TextureDumpPath = (root / "independent").string();
	dumper = std::make_unique<CustomTexture>();
	dump(texture, 8);
	extract(texture);
	extract(texture, 0.375f);
	EXPECT_EQ(readText(titleDirectory() / texture_atlas::MappingFilename), readText(mapping));
	for (const auto& region : extended[0].regions)
		EXPECT_TRUE(std::filesystem::exists(titleDirectory() / texture_atlas::regionFilename(texture.texture_hash, region)));
}

TEST_F(NativeTextureDumpTest, SplitDumpsUseTheSameNativePixelsAndNamesWithDifferentXbrzSettings)
{
	std::string expectedMapping, expectedParent;
	for (int factor : {1, 2, 4})
	{
		config::TextureUpscale = factor;
		config::TextureDumpPath = (root / ("xbrz-" + std::to_string(factor))).string();
		DumpTexture texture(0);
		texture.onUpload = [&] {
			EXPECT_TRUE(std::filesystem::exists(titleDirectory() / texture_atlas::imageFilename(texture.texture_hash)));
		};
		ASSERT_TRUE(texture.Update());
		EXPECT_EQ(texture.uploadedWidth, 8 * factor);
		EXPECT_EQ(texture.uploadedHeight, 8 * factor);
		extract(texture, 0.5f, &custom_texture);
		const auto mapping = readText(titleDirectory() / texture_atlas::MappingFilename);
		const auto parent = readText(titleDirectory() / texture_atlas::imageFilename(texture.texture_hash));
		const auto sheets = texture_atlas::parseMapping(mapping);
		ASSERT_EQ(sheets.size(), 1u);
		EXPECT_EQ(sheets[0].width, 8u);
		EXPECT_EQ(sheets[0].height, 8u);
		if (factor == 1)
		{
			expectedMapping = mapping;
			expectedParent = parent;
		}
		else
		{
			EXPECT_EQ(mapping, expectedMapping);
			EXPECT_EQ(parent, expectedParent);
		}
	}
}

TEST_F(NativeTextureDumpTest, OrdinaryDumpsUseTheSameNativePixelsWithDifferentXbrzSettings)
{
	config::SplitTextureSheets = false;
	std::string expected;
	for (int factor : {1, 2, 4})
	{
		config::TextureUpscale = factor;
		config::TextureDumpPath = (root / ("ordinary-" + std::to_string(factor))).string();
		DumpTexture texture(0);
		texture.onUpload = [&] {
			EXPECT_TRUE(std::filesystem::exists(titleDirectory() / texture_atlas::imageFilename(texture.texture_hash)));
		};
		ASSERT_TRUE(texture.Update());
		EXPECT_EQ(texture.uploadedWidth, 8 * factor);
		EXPECT_EQ(texture.uploadedHeight, 8 * factor);
		const auto path = titleDirectory() / texture_atlas::imageFilename(texture.texture_hash);
		const auto bytes = readText(path);
		int width = 0, height = 0, channels = 0;
		ASSERT_NE(stbi_info_from_memory(reinterpret_cast<const stbi_uc *>(bytes.data()), static_cast<int>(bytes.size()),
				&width, &height, &channels), 0);
		EXPECT_EQ(width, 8);
		EXPECT_EQ(height, 8);
		if (factor == 1)
			expected = bytes;
		else
			EXPECT_EQ(bytes, expected);
		EXPECT_FALSE(std::filesystem::exists(titleDirectory() / texture_atlas::MappingFilename));
	}
}

TEST_F(CustomTextureDumpTest, OrdinaryRedumpsPreserveExistingFiles)
{
	config::SplitTextureSheets = false;
	DumpTexture texture(0x123);
	dump(texture, 8);
	const auto path = titleDirectory() / "123.png";
	const auto before = readText(path);
	const auto oldTime = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
	std::filesystem::last_write_time(path, oldTime);
	const auto stamp = std::filesystem::last_write_time(path);
	dumper = std::make_unique<CustomTexture>();
	dump(texture, 16);
	EXPECT_EQ(readText(path), before);
	EXPECT_EQ(std::filesystem::last_write_time(path), stamp);
}

TEST_F(NativeTextureDumpTest, DumpPixelsDoNotDependOnRendererPixelPrecision)
{
	config::SplitTextureSheets = false;
	config::TextureUpscale = 1;
	for (u32 format : {Pixel1555, Pixel565, Pixel4444, PixelYUV})
	{
		std::string expected;
		for (bool force32 : {false, true})
		{
			config::TextureDumpPath = (root / ("precision-" + std::to_string(format) + '-' + std::to_string(force32))).string();
			TCW control {};
			control.PixelFmt = format;
			DumpTexture texture(0, control);
			texture.force32 = force32;
			ASSERT_TRUE(texture.Update());
			EXPECT_EQ(texture.uploadedType, TextureType::_8888);
			const auto pixels = readText(titleDirectory() / texture_atlas::imageFilename(texture.texture_hash));
			ASSERT_FALSE(pixels.empty());
			if (!force32)
				expected = pixels;
			else
				EXPECT_EQ(pixels, expected);
		}
	}
}

TEST_F(NativeTextureDumpTest, DumpingDisabledRetainsTheNormalTextureFormat)
{
	config::DumpTextures = false;
	config::TextureUpscale = 1;
	DumpTexture texture(0);
	ASSERT_TRUE(texture.Update());
	EXPECT_EQ(texture.uploadedType, TextureType::_5551);
	EXPECT_FALSE(std::filesystem::exists(titleDirectory()));
}
