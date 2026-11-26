// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "imgread/common.h"
#include <algorithm>
#include <iterator>
#include <locale>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

class CueTest : public ::testing::Test {
};

namespace {

class TemporaryCue
{
public:
	TemporaryCue()
	{
		const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
		path = std::filesystem::temp_directory_path() / ("hollycast-cue-test-" + std::to_string(nonce));
		std::filesystem::create_directories(path);
	}

	~TemporaryCue()
	{
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}

	void writeSectors(const std::string& name, const std::vector<std::pair<u32, u8>>& sectors)
	{
		std::ofstream output(path / name, std::ios::binary);
		for (const auto& [size, value] : sectors) {
			std::vector<char> sector(size, (char)value);
			output.write(sector.data(), sector.size());
		}
		ASSERT_TRUE(output.good());
	}

	void writeSparseFile(const std::string& name, u64 prefixBytes, u32 lastSectorSize, u8 value)
	{
		std::ofstream output(path / name, std::ios::binary);
		output.seekp((std::streamoff)prefixBytes);
		std::vector<char> sector(lastSectorSize, (char)value);
		output.write(sector.data(), sector.size());
		ASSERT_TRUE(output.good());
	}

	std::string writeCue(const std::string& cue)
	{
		const auto cuePath = path / "disc.cue";
		std::ofstream output(cuePath);
		output << cue;
		output.close();
		EXPECT_TRUE(output.good());
		return cuePath.string();
	}

	std::filesystem::path path;
};

static bool readRawSector(Disc* disc, u32 FAD, SectorFormat& sectorFormat, u8* bytes)
{
	SubcodeFormat subcodeFormat = SUBFMT_NONE;
	return disc->tracks.back().Read(FAD, bytes, &sectorFormat, nullptr, &subcodeFormat);
}

} // namespace

static void verifyDisk(Disc *disk)
{
	ASSERT_EQ(GdRom, disk->type);
	ASSERT_EQ(3, disk->tracks.size());

	ASSERT_TRUE(disk->tracks[0].isDataTrack());
	ASSERT_EQ(150, disk->tracks[0].StartFAD);

	ASSERT_FALSE(disk->tracks[1].isDataTrack());
	ASSERT_EQ(600, disk->tracks[1].StartFAD);

	ASSERT_TRUE(disk->tracks[2].isDataTrack());
	ASSERT_EQ(45150, disk->tracks[2].StartFAD);
}

TEST_F(CueTest, LoadD)
{
	struct RestoreLocale { std::locale original = std::locale(); ~RestoreLocale() { std::locale::global(original); } } restoreLocale;
	std::locale::global(std::locale("fr_FR.UTF-8"));
	Disc *disk = OpenDisc(FLYCAST_TEST_FILES "/test_cues/d/cs.cue");
	verifyDisk(disk);
	delete disk;
}

TEST_F(CueTest, LoadE)
{
	Disc *disk = OpenDisc(FLYCAST_TEST_FILES "/test_cues/e/cs.cue");

	ASSERT_EQ(CdRom_XA, disk->type);
	ASSERT_EQ(2, disk->tracks.size());

	ASSERT_FALSE(disk->tracks[0].isDataTrack());
	ASSERT_EQ(150, disk->tracks[0].StartFAD);

	ASSERT_TRUE(disk->tracks[1].isDataTrack());
	ASSERT_EQ(11550, disk->tracks[1].StartFAD);
	delete disk;
}

TEST_F(CueTest, LoadAudio)
{
	Disc *disk = OpenDisc(FLYCAST_TEST_FILES "/test_cues/d/audio.cue");

	ASSERT_EQ(CdRom, disk->type);
	ASSERT_EQ(3, disk->tracks.size());

	ASSERT_FALSE(disk->tracks[0].isDataTrack());
	ASSERT_EQ(150, disk->tracks[0].StartFAD);

	ASSERT_FALSE(disk->tracks[1].isDataTrack());
	ASSERT_EQ(225, disk->tracks[1].StartFAD);

	ASSERT_FALSE(disk->tracks[2].isDataTrack());
	ASSERT_EQ(300, disk->tracks[2].StartFAD);
	delete disk;
}

TEST_F(CueTest, ReadsFirstSectorAfterMixedWidthBoundary)
{
	TemporaryCue fixture;
	fixture.writeSectors("mixed.bin", {
		{2048, 0x10}, {2048, 0x11}, {2048, 0x12},
		{2352, 0x80}, {2352, 0x81}
	});
	const std::string cue = fixture.writeCue(
		"FILE \"mixed.bin\" BINARY\n"
		"  TRACK 01 MODE1/2048\n"
		"    INDEX 01 00:00:00\n"
		"  TRACK 02 AUDIO\n"
		"    INDEX 01 00:00:03\n");

	Disc* disk = OpenDisc(cue);
	ASSERT_NE(nullptr, disk);
	ASSERT_EQ(2, disk->tracks.size());
	EXPECT_EQ(153, disk->tracks[1].StartFAD);
	EXPECT_EQ(155, disk->EndFAD);
	EXPECT_EQ(155, disk->LeadOut.StartFAD);

	u8 sector[2448] = {};
	SectorFormat format;
	ASSERT_TRUE(disk->tracks[0].Read(152, sector, &format, nullptr, nullptr));
	EXPECT_EQ(SECFMT_2048_MODE2_FORM1, format);
	EXPECT_EQ(0x12, sector[0]);
	ASSERT_TRUE(readRawSector(disk, 153, format, sector));
	EXPECT_EQ(SECFMT_2352, format);
	EXPECT_EQ(0x80, sector[0]);
	ASSERT_TRUE(readRawSector(disk, 154, format, sector));
	EXPECT_EQ(0x81, sector[0]);
	delete disk;

	// The digest path must still add a physical FILE exactly once.
	std::vector<u8> digest;
	disk = OpenDisc(cue, &digest);
	ASSERT_NE(nullptr, disk);
	EXPECT_EQ(16, digest.size());
	delete disk;
}

TEST_F(CueTest, ReadsMixedWidthTracksInReverseOrderAndCalculatesLeadOut)
{
	TemporaryCue fixture;
	fixture.writeSectors("reverse.bin", {
		{2352, 0x20}, {2352, 0x21}, {2352, 0x22},
		{2048, 0x30}, {2048, 0x31}
	});
	const std::string cue = fixture.writeCue(
		"FILE \"reverse.bin\" BINARY\n"
		"  TRACK 01 AUDIO\n"
		"    INDEX 01 00:00:00\n"
		"  TRACK 02 MODE1/2048\n"
		"    INDEX 01 00:00:03\n");

	Disc* disk = OpenDisc(cue);
	ASSERT_EQ(155, disk->LeadOut.StartFAD);
	u8 sector[2448] = {};
	SectorFormat format;
	ASSERT_TRUE(disk->tracks[1].Read(153, sector, &format, nullptr, nullptr));
	EXPECT_EQ(SECFMT_2048_MODE2_FORM1, format);
	EXPECT_EQ(0x30, sector[0]);
	ASSERT_TRUE(disk->tracks[1].Read(154, sector, &format, nullptr, nullptr));
	EXPECT_EQ(0x31, sector[0]);
	delete disk;

	// The old first-width whole-file division yields 11 frames here; the
	// physical spans contain 3 audio and 10 cooked sectors, or 13 frames.
	TemporaryCue longer;
	std::vector<std::pair<u32, u8>> sectors(3, {2352, 0x40});
	sectors.insert(sectors.end(), 10, {2048, 0x50});
	longer.writeSectors("long.bin", sectors);
	const std::string longCue = longer.writeCue(
		"FILE \"long.bin\" BINARY\n"
		"  TRACK 01 AUDIO\n"
		"    INDEX 01 00:00:00\n"
		"  TRACK 02 MODE1/2048\n"
		"    INDEX 01 00:00:03\n");
	disk = OpenDisc(longCue);
	ASSERT_EQ(163, disk->LeadOut.StartFAD);
	ASSERT_TRUE(disk->tracks[1].Read(162, sector, &format, nullptr, nullptr));
	EXPECT_EQ(0x50, sector[0]);
	delete disk;
}

TEST_F(CueTest, AccountsForStoredAndSyntheticPregapsAcrossFiles)
{
	TemporaryCue fixture;
	fixture.writeSectors("first.bin", {{2048, 0x60}, {2048, 0x61}});
	fixture.writeSectors("second.bin", {{2352, 0x70}, {2352, 0x71}, {2352, 0x72}});
	const std::string cue = fixture.writeCue(
		"FILE \"first.bin\" BINARY\n"
		"  TRACK 01 MODE1/2048\n"
		"    INDEX 01 00:00:00\n"
		"FILE \"second.bin\" BINARY\n"
		"  TRACK 02 AUDIO\n"
		"    PREGAP 00:00:01\n"
		"    INDEX 00 00:00:00\n"
		"    INDEX 01 00:00:02\n");

	Disc* disk = OpenDisc(cue);
	ASSERT_EQ(2, disk->tracks.size());
	EXPECT_EQ(155, disk->tracks[1].StartFAD);
	EXPECT_EQ(156, disk->LeadOut.StartFAD);
	u8 sector[2448] = {};
	SectorFormat format;
	ASSERT_TRUE(disk->tracks[1].Read(155, sector, &format, nullptr, nullptr));
	EXPECT_EQ(SECFMT_2352, format);
	EXPECT_EQ(0x72, sector[0]);
	delete disk;

	// INDEX 00 consumes incoming audio sectors before INDEX 01, while the
	// first FILE's two cooked sectors still set the next FILE's base FAD.
	TemporaryCue sameFile;
	sameFile.writeSectors("pregap.bin", {
		{2048, 0x61}, {2048, 0x62},
		{2352, 0x63}, {2352, 0x64}, {2352, 0x65}
	});
	const std::string sameFileCue = sameFile.writeCue(
		"FILE \"pregap.bin\" BINARY\n"
		"  TRACK 01 MODE1/2048\n"
		"    INDEX 01 00:00:00\n"
		"  TRACK 02 AUDIO\n"
		"    INDEX 00 00:00:02\n"
		"    INDEX 01 00:00:04\n");
	disk = OpenDisc(sameFileCue);
	ASSERT_EQ(154, disk->tracks[1].StartFAD);
	EXPECT_EQ(155, disk->LeadOut.StartFAD);
	EXPECT_EQ(152, disk->tracks[1].ReadStartFAD);
	std::vector<u8> publicRead(2 * 2352);
	ASSERT_EQ(2u, disk->ReadSectors(152, 2, publicRead.data(), 2352));
	EXPECT_TRUE(std::all_of(publicRead.begin(), publicRead.begin() + 2352, [](u8 value) { return value == 0x63; }));
	EXPECT_TRUE(std::all_of(publicRead.begin() + 2352, publicRead.end(), [](u8 value) { return value == 0x64; }));
	ASSERT_EQ(1u, disk->ReadSectors(154, 1, sector, 2352));
	EXPECT_TRUE(std::all_of(std::begin(sector), std::begin(sector) + 2352, [](u8 value) { return value == 0x65; }));
	ASSERT_TRUE(disk->tracks[1].Read(154, sector, &format, nullptr, nullptr));
	EXPECT_EQ(0x65, sector[0]);
	delete disk;
}

TEST_F(CueTest, SyntheticPregapDoesNotReadProgramData)
{
	TemporaryCue fixture;
	fixture.writeSectors("synthetic.bin", {
		{2048, 0x51}, {2048, 0x52}, {2352, 0x70}, {2352, 0x71}
	});
	const std::string cue = fixture.writeCue(
		"FILE \"synthetic.bin\" BINARY\n"
		"  TRACK 01 MODE1/2048\n"
		"    INDEX 01 00:00:00\n"
		"  TRACK 02 AUDIO\n"
		"    PREGAP 00:00:01\n"
		"    INDEX 01 00:00:02\n");

	Disc* disk = OpenDisc(cue);
	ASSERT_EQ(2, disk->tracks.size());
	EXPECT_EQ(153, disk->tracks[1].StartFAD);
	EXPECT_EQ(155, disk->LeadOut.StartFAD);
	u8 sector[2352];
	bool readError = false;
	ASSERT_EQ(1u, disk->ReadSectors(152, 1, sector, sizeof(sector), false, nullptr, &readError));
	EXPECT_TRUE(readError);
	EXPECT_TRUE(std::all_of(std::begin(sector), std::end(sector), [](u8 value) { return value == 0; }));
	readError = false;
	ASSERT_EQ(1u, disk->ReadSectors(153, 1, sector, sizeof(sector), false, nullptr, &readError));
	EXPECT_FALSE(readError);
	EXPECT_TRUE(std::all_of(std::begin(sector), std::end(sector), [](u8 value) { return value == 0x70; }));
	delete disk;
}

TEST_F(CueTest, RejectsInvalidMixedWidthCueIndices)
{
	TemporaryCue nonMonotonic;
	nonMonotonic.writeSectors("bad.bin", {{2048, 0x01}, {2352, 0x02}, {2352, 0x03}});
	const std::string badOrder = nonMonotonic.writeCue(
		"FILE \"bad.bin\" BINARY\n"
		"  TRACK 01 MODE1/2048\n"
		"    INDEX 01 00:00:03\n"
		"  TRACK 02 AUDIO\n"
		"    INDEX 01 00:00:02\n");
	EXPECT_THROW(OpenDisc(badOrder), FlycastException);

	TemporaryCue outOfRange;
	outOfRange.writeSectors("short.bin", {{2048, 0x01}});
	const std::string badOffset = outOfRange.writeCue(
		"FILE \"short.bin\" BINARY\n"
		"  TRACK 01 MODE1/2048\n"
		"    INDEX 01 00:00:02\n");
	EXPECT_THROW(OpenDisc(badOffset), FlycastException);

	const std::string badTimestamp = outOfRange.writeCue(
		"FILE \"short.bin\" BINARY\n"
		"  TRACK 01 MODE1/2048\n"
		"    INDEX 01 00:00:75\n");
	EXPECT_THROW(OpenDisc(badTimestamp), FlycastException);
}

TEST_F(CueTest, ReadsMixedWidthOffsetBeyond32Bits)
{
	constexpr u32 dataFrames = 8 * 1024 * 1024;
	constexpr u64 audioOffset = (u64)dataFrames * 2048;
	TemporaryCue fixture;
	fixture.writeSparseFile("large.bin", audioOffset, 2352, 0x7a);
	const u32 minutes = dataFrames / (60 * 75);
	const u32 seconds = (dataFrames / 75) % 60;
	const u32 frames = dataFrames % 75;
	char time[32];
	snprintf(time, sizeof(time), "%02u:%02u:%02u", minutes, seconds, frames);
	const std::string cue = fixture.writeCue(std::string(
		"FILE \"large.bin\" BINARY\n"
		"  TRACK 01 MODE1/2048\n"
		"    INDEX 01 00:00:00\n"
		"  TRACK 02 AUDIO\n"
		"    INDEX 01 ") + time + "\n");

	Disc* disk = OpenDisc(cue);
	ASSERT_NE(nullptr, disk);
	u8 sector[2448] = {};
	SectorFormat format;
	ASSERT_TRUE(disk->tracks[1].Read(disk->tracks[1].StartFAD, sector, &format, nullptr, nullptr));
	EXPECT_EQ(SECFMT_2352, format);
	EXPECT_EQ(0x7a, sector[0]);
	delete disk;
}
