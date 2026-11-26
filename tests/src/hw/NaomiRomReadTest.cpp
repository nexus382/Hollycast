#include "hw/naomi/naomi.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/naomi/naomi_roms.h"
#include "archive/archive.h"
#include "stdclass.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <optional>

namespace
{
	struct ReadStats
	{
		u32 calls = 0;
		u32 requested = 0;
	};

	class MemoryArchiveFile : public ArchiveFile
	{
	public:
		MemoryArchiveFile(const std::vector<u8>& bytes, ReadStats& stats, std::optional<u32> result)
			: bytes(bytes), stats(stats), result(result) {}

		u32 Read(void *buffer, u32 length) override
		{
			stats.calls++;
			stats.requested = length;
			u32 actualResult = result.value_or(length);
			u32 copyLength = std::min<u32>({ length, actualResult, (u32)bytes.size() });
			if (actualResult == std::numeric_limits<u32>::max())
				copyLength = 0;
			memcpy(buffer, bytes.data(), copyLength);
			return actualResult;
		}

		size_t length() override { return bytes.size(); }

	private:
		const std::vector<u8>& bytes;
		ReadStats& stats;
		std::optional<u32> result;
	};

	class MemoryArchive : public Archive
	{
	public:
		MemoryArchive(std::vector<u8> bytes, std::optional<u32> result = std::nullopt)
			: bytes(std::move(bytes)), result(result) {}

		ArchiveFile *OpenFile(const char *) override
		{
			return present ? new MemoryArchiveFile(bytes, stats, result) : nullptr;
		}
		ArchiveFile *OpenFileByCrc(u32) override { return OpenFile(nullptr); }

		ReadStats stats;
		bool present = true;

	protected:
		bool Open(hostfs::File *) override { return false; }

	private:
		std::vector<u8> bytes;
		std::optional<u32> result;
	};

	class TestCartridge : public Cartridge
	{
	public:
		explicit TestCartridge(u32 size) : Cartridge(size) {}
		~TestCartridge() override { free(keyData); }

		u32 ReadMem(u32, u32) override { return 0; }
		void WriteMem(u32, u32, u32) override {}
		void *GetDmaPtr(u32&) override { return nullptr; }
		void AdvancePtr(u32) override {}
		bool GetBootId(RomBootID *) override { return false; }
		void SetKeyData(u8 *data) override
		{
			free(keyData);
			keyData = data;
		}

		const u8 *bytes() const { return RomPtr; }
		const u8 *key() const { return keyData; }

	private:
		u8 *keyData = nullptr;
	};

	std::unique_ptr<ArchiveFile> openFile(MemoryArchive& archive)
	{
		return std::unique_ptr<ArchiveFile>(archive.OpenFile("synthetic.bin"));
	}

	std::vector<u8> digest(MD5Sum md5)
	{
		return md5.getDigest();
	}
}

TEST(NaomiRomReadTest, ExactReadAcceptsMemberPrefixAndReadsOnce)
{
	MemoryArchive archive({ 0x10, 0x20, 0x30, 0x40, 0x50 });
	auto file = openFile(archive);
	std::array<u8, 3> actual{};

	naomi_cart::readExactArchiveFile(*file, actual.data(), actual.size(), "synthetic.bin");

	EXPECT_EQ((std::array<u8, 3>{ 0x10, 0x20, 0x30 }), actual);
	EXPECT_EQ(1u, archive.stats.calls);
	EXPECT_EQ(3u, archive.stats.requested);
}

TEST(NaomiRomReadTest, ShortMemberIsRejectedBeforePublish)
{
	MemoryArchive archive({ 0x11, 0x22, 0x33, 0x44 }, 2);
	auto file = openFile(archive);
	TestCartridge cartridge(16);
	std::array<u8, 16> before;
	memcpy(before.data(), cartridge.bytes(), before.size());
	std::vector<u8> defaultEeprom{ 0xaa };
	MD5Sum md5;
	const std::vector<u8> digestBefore = digest(md5);
	CurrentCartridge = &cartridge;

	try
	{
		naomi_cart::loadGameBlob(*file, Normal, cartridge, 4, 4, "short-program.bin", md5, true, defaultEeprom);
		FAIL() << "Expected incomplete member read to fail";
	}
	catch (const NaomiCartException& e)
	{
		EXPECT_NE(std::string(e.what()).find("short-program.bin"), std::string::npos);
	}

	EXPECT_EQ(1u, archive.stats.calls);
	EXPECT_EQ(4u, archive.stats.requested);
	EXPECT_EQ(0, memcmp(before.data(), cartridge.bytes(), before.size()));
	EXPECT_EQ(digestBefore, digest(md5));
	EXPECT_EQ(&cartridge, CurrentCartridge);
	EXPECT_EQ((std::vector<u8>{ 0xaa }), defaultEeprom);

	MemoryArchive completeArchive({ 0x11, 0x22, 0x33, 0x44, 0x55 });
	auto completeFile = openFile(completeArchive);
	naomi_cart::loadGameBlob(*completeFile, Normal, cartridge, 4, 4, "program.bin", md5, true, defaultEeprom);
	const std::array<u8, 4> loadedProgram{ 0x11, 0x22, 0x33, 0x44 };
	EXPECT_EQ(0, memcmp(loadedProgram.data(), cartridge.bytes() + 4, loadedProgram.size()));
	EXPECT_EQ(MD5Sum().add(std::array<u8, 4>{ 0x11, 0x22, 0x33, 0x44 }).getDigest(), digest(md5));
	CurrentCartridge = nullptr;
}

TEST(NaomiRomReadTest, ZeroAndErrorCountsAreRejectedAfterOneRead)
{
	for (u32 result : { 0u, std::numeric_limits<u32>::max() })
	{
		MemoryArchive archive({ 1, 2, 3, 4 }, result);
		auto file = openFile(archive);
		std::array<u8, 4> actual{};
		EXPECT_THROW(naomi_cart::readExactArchiveFile(*file, actual.data(), actual.size(), "component.bin"), NaomiCartException);
		EXPECT_EQ(1u, archive.stats.calls);
	}
}

TEST(NaomiRomReadTest, InterleavedWordPreservesLaneLayoutAndDigest)
{
	MemoryArchive archive({ 0x11, 0x12, 0x21, 0x22 });
	auto file = openFile(archive);
	TestCartridge cartridge(16);
	std::vector<u8> defaultEeprom;
	MD5Sum md5;

	naomi_cart::loadGameBlob(*file, InterleavedWord, cartridge, 2, 4, "lane.bin", md5, true, defaultEeprom);

	const std::array<u8, 10> expected{ 0xff, 0xff, 0x11, 0x12, 0xff, 0xff, 0x21, 0x22, 0xff, 0xff };
	EXPECT_EQ(0, memcmp(expected.data(), cartridge.bytes(), expected.size()));
	const std::array<u8, 4> hashedLayout{ 0x11, 0x12, 0xff, 0xff };
	EXPECT_EQ(MD5Sum().add(hashedLayout).getDigest(), digest(md5));
	EXPECT_EQ(1u, archive.stats.calls);
}

TEST(NaomiRomReadTest, HighLaneCanEndAtTheCartridgeBoundary)
{
	TestCartridge cartridge(8);
	MemoryArchive low({ 0x10, 0x11, 0x30, 0x31 });
	MemoryArchive high({ 0x20, 0x21, 0x40, 0x41 });
	auto lowFile = openFile(low);
	auto highFile = openFile(high);
	std::vector<u8> eeprom;
	MD5Sum md5;

	naomi_cart::loadGameBlob(*lowFile, InterleavedWord, cartridge, 0, 4, "low", md5, true, eeprom);
	naomi_cart::loadGameBlob(*highFile, InterleavedWord, cartridge, 2, 4, "high", md5, true, eeprom);

	const std::array<u8, 8> expected{ 0x10, 0x11, 0x20, 0x21, 0x30, 0x31, 0x40, 0x41 };
	EXPECT_EQ(0, memcmp(expected.data(), cartridge.bytes(), expected.size()));
	// GGPO hashes each lane's original contiguous slice after that lane is mapped.
	const std::array<u8, 8> hashed{ 0x10, 0x11, 0xff, 0xff, 0x20, 0x21, 0x30, 0x31 };
	EXPECT_EQ(MD5Sum().add(hashed).getDigest(), digest(md5));
}

TEST(NaomiRomReadTest, KeyAndDefaultEepromPublishOnlyAfterExactReads)
{
	TestCartridge cartridge(16);
	std::vector<u8> defaultEeprom{ 0xee };
	MD5Sum md5;
	MemoryArchive shortKey({ 1, 2, 3, 4 }, 2);
	auto shortKeyFile = openFile(shortKey);
	EXPECT_THROW(naomi_cart::loadGameBlob(*shortKeyFile, Key, cartridge, 0, 4, "key.bin", md5, true, defaultEeprom), NaomiCartException);
	EXPECT_EQ(nullptr, cartridge.key());

	MemoryArchive keyArchive({ 1, 2, 3, 4 });
	auto keyFile = openFile(keyArchive);
	naomi_cart::loadGameBlob(*keyFile, Key, cartridge, 0, 4, "key.bin", md5, true, defaultEeprom);
	const std::array<u8, 4> loadedKey{ 1, 2, 3, 4 };
	EXPECT_EQ(0, memcmp(loadedKey.data(), cartridge.key(), loadedKey.size()));

	MemoryArchive shortEeprom({ 5, 6, 7, 8 }, 1);
	auto shortEepromFile = openFile(shortEeprom);
	EXPECT_THROW(naomi_cart::loadGameBlob(*shortEepromFile, Eeprom, cartridge, 0, 4, "eeprom.bin", md5, true, defaultEeprom), NaomiCartException);
	EXPECT_EQ((std::vector<u8>{ 0xee }), defaultEeprom);

	MemoryArchive eepromArchive({ 5, 6, 7, 8 });
	auto eepromFile = openFile(eepromArchive);
	naomi_cart::loadGameBlob(*eepromFile, Eeprom, cartridge, 0, 4, "eeprom.bin", md5, true, defaultEeprom);
	EXPECT_EQ((std::vector<u8>{ 5, 6, 7, 8 }), defaultEeprom);
}

TEST(NaomiRomReadTest, OptionalEepromMayBeAbsent)
{
	MemoryArchive archive({});
	archive.present = false;
	std::unique_ptr<ArchiveFile> file(archive.OpenFile("optional-eeprom.bin"));

	EXPECT_EQ(nullptr, file);
	EXPECT_EQ(0u, archive.stats.calls);
}

TEST(NaomiRomReadTest, SerialEepromIsValidatedBeforePublish)
{
	std::vector<u8> serial(0x84);
	for (size_t i = 0; i < serial.size(); ++i)
		serial[i] = (u8)i;
	std::array<u8, 112> previous{};
	memcpy(previous.data(), getGameSerialId(), previous.size());
	TestCartridge cartridge(16);
	std::vector<u8> defaultEeprom;
	MD5Sum md5;

	MemoryArchive shortArchive(serial, 0x83);
	auto shortFile = openFile(shortArchive);
	EXPECT_THROW(naomi_cart::loadGameBlob(*shortFile, Eeprom, cartridge, 0, serial.size(), "serial.bin", md5, true, defaultEeprom), NaomiCartException);
	EXPECT_EQ(0, memcmp(previous.data(), getGameSerialId(), previous.size()));

	MemoryArchive completeArchive(serial);
	auto completeFile = openFile(completeArchive);
	naomi_cart::loadGameBlob(*completeFile, Eeprom, cartridge, 0, serial.size(), "serial.bin", md5, true, defaultEeprom);
	EXPECT_EQ(0, memcmp(serial.data() + 20, getGameSerialId(), 112));
	EXPECT_EQ(MD5Sum().add(serial).getDigest(), digest(md5));
}
