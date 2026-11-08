// Portions Copyright 2026 The Hollycast Authors
#include "rend/TexCache.h"
#include "util/worker_thread.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace
{
class DumpTexture : public BaseTextureCacheData
{
public:
	DumpTexture(u32 hash) : BaseTextureCacheData(TSP{}, TCW{}, 0)
	{
		texture_hash = hash;
		tex_type = TextureType::_8888;
	}
	std::string GetId() override { return "atlas-dump-test"; }
	void UploadToGPU(int, int, const u8 *, bool, bool) override {}
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
		dumper.dumpTexture(&texture, size, size, pixels.data());
	}

	void extract(DumpTexture& texture)
	{
		rend_context context {};
		context.verts.resize(4);
		context.verts[0].u = 0;
		context.verts[0].v = 0;
		context.verts[1].u = 0.5f;
		context.verts[1].v = 0;
		context.verts[2].u = 0;
		context.verts[2].v = 0.5f;
		context.verts[3].u = 0.5f;
		context.verts[3].v = 0.5f;
		context.global_param_op.emplace_back();
		auto& polygon = context.global_param_op.back();
		polygon.init();
		polygon.texture = &texture;
		polygon.pcw.Texture = 1;
		polygon.count = 4;
		RenderPass pass {};
		pass.op_count = 1;
		dumper.dumpTextureRegions(context, RenderPass{}, pass);
	}

	std::filesystem::path titleDirectory() const
	{
		return std::filesystem::path(config::TextureDumpPath.get()) / "ATLAS-TEST";
	}

	CustomTexture dumper;
	std::filesystem::path root;
	std::string oldPath, oldGameId;
	RenderType oldRenderer {};
	bool oldDump = false, oldSplit = false;
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
