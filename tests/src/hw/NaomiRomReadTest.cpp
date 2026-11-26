#include "hw/naomi/naomi.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/naomi/naomi_roms.h"
#include "archive/archive.h"
#include "cfg/option.h"
#include "emulator.h"
#include "stdclass.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <map>
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

	struct FixtureEntry
	{
		std::vector<u8> bytes;
		std::optional<u32> result;
		ReadStats stats;
	};

	class FixtureFile : public ArchiveFile
	{
	public:
		explicit FixtureFile(FixtureEntry& entry) : entry(entry) {}
		u32 Read(void *buffer, u32 length) override
		{
			entry.stats.calls++;
			entry.stats.requested = length;
			u32 actual = entry.result.value_or(length);
			if (actual != std::numeric_limits<u32>::max())
				memcpy(buffer, entry.bytes.data(), std::min<u32>({ actual, length, (u32)entry.bytes.size() }));
			return actual;
		}
		size_t length() override { return entry.bytes.size(); }
	private:
		FixtureEntry& entry;
	};

	class FixtureArchive : public Archive
	{
	public:
		FixtureEntry& add(const std::string& name, std::vector<u8> bytes, std::optional<u32> result = std::nullopt, u32 crc = 0)
		{
			auto [it, inserted] = entries.emplace(name, FixtureEntry{ std::move(bytes), result, {} });
			if (crc != 0)
				crcs[crc] = name;
			return it->second;
		}
		ArchiveFile *OpenFile(const char *name) override
		{
			if (name == nullptr)
				return nullptr;
			auto it = entries.find(name);
			return it == entries.end() ? nullptr : new FixtureFile(it->second);
		}
		ArchiveFile *OpenFileByCrc(u32 crc) override
		{
			auto it = crcs.find(crc);
			return it == crcs.end() ? nullptr : OpenFile(it->second.c_str());
		}
	protected:
		bool Open(hostfs::File *) override { return false; }
	private:
		std::map<std::string, FixtureEntry> entries;
		std::map<u32, std::string> crcs;
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

TEST(NaomiRomReadTest, BlobHelperDoesNotPublishBytesOrDigest)
{
	MemoryArchive archive({ 0x11, 0x22, 0x33, 0x44 }, 2);
	auto file = openFile(archive);
	TestCartridge cartridge(16);
	std::array<u8, 16> before;
	memcpy(before.data(), cartridge.bytes(), before.size());
	std::vector<u8> defaultEeprom{ 0xaa };
	std::vector<u8> serial;
	MD5Sum md5;
	const std::vector<u8> digestBefore = digest(md5);

	try
	{
		naomi_cart::loadGameBlob(*file, Normal, cartridge, 4, 4, "short-program.bin", md5, true, defaultEeprom, serial);
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
	EXPECT_EQ((std::vector<u8>{ 0xaa }), defaultEeprom);

	MemoryArchive completeArchive({ 0x11, 0x22, 0x33, 0x44, 0x55 });
	auto completeFile = openFile(completeArchive);
	naomi_cart::loadGameBlob(*completeFile, Normal, cartridge, 4, 4, "program.bin", md5, true, defaultEeprom, serial);
	const std::array<u8, 4> loadedProgram{ 0x11, 0x22, 0x33, 0x44 };
	EXPECT_EQ(0, memcmp(loadedProgram.data(), cartridge.bytes() + 4, loadedProgram.size()));
	EXPECT_EQ(MD5Sum().add(std::array<u8, 4>{ 0x11, 0x22, 0x33, 0x44 }).getDigest(), digest(md5));
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
	std::vector<u8> serial;
	MD5Sum md5;

	naomi_cart::loadGameBlob(*file, InterleavedWord, cartridge, 2, 4, "lane.bin", md5, true, defaultEeprom, serial);

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
	std::vector<u8> serial;
	MD5Sum md5;

	naomi_cart::loadGameBlob(*lowFile, InterleavedWord, cartridge, 0, 4, "low", md5, true, eeprom, serial);
	naomi_cart::loadGameBlob(*highFile, InterleavedWord, cartridge, 2, 4, "high", md5, true, eeprom, serial);

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
	std::vector<u8> serial;
	MD5Sum md5;
	MemoryArchive shortKey({ 1, 2, 3, 4 }, 2);
	auto shortKeyFile = openFile(shortKey);
	EXPECT_THROW(naomi_cart::loadGameBlob(*shortKeyFile, Key, cartridge, 0, 4, "key.bin", md5, true, defaultEeprom, serial), NaomiCartException);
	EXPECT_EQ(nullptr, cartridge.key());

	MemoryArchive keyArchive({ 1, 2, 3, 4 });
	auto keyFile = openFile(keyArchive);
	naomi_cart::loadGameBlob(*keyFile, Key, cartridge, 0, 4, "key.bin", md5, true, defaultEeprom, serial);
	const std::array<u8, 4> loadedKey{ 1, 2, 3, 4 };
	EXPECT_EQ(0, memcmp(loadedKey.data(), cartridge.key(), loadedKey.size()));

	MemoryArchive shortEeprom({ 5, 6, 7, 8 }, 1);
	auto shortEepromFile = openFile(shortEeprom);
	EXPECT_THROW(naomi_cart::loadGameBlob(*shortEepromFile, Eeprom, cartridge, 0, 4, "eeprom.bin", md5, true, defaultEeprom, serial), NaomiCartException);
	EXPECT_EQ((std::vector<u8>{ 0xee }), defaultEeprom);

	MemoryArchive eepromArchive({ 5, 6, 7, 8 });
	auto eepromFile = openFile(eepromArchive);
	naomi_cart::loadGameBlob(*eepromFile, Eeprom, cartridge, 0, 4, "eeprom.bin", md5, true, defaultEeprom, serial);
	EXPECT_EQ((std::vector<u8>{ 5, 6, 7, 8 }), defaultEeprom);
}

TEST(NaomiRomReadTest, OptionalEepromMayBeAbsent)
{
	Game game{};
	game.name = "synthetic";
	game.blobs[0] = { "optional-eeprom.bin", 0, 4, 0x1234, Eeprom, 0 };
	TestCartridge cartridge(16);
	FixtureArchive archive;
	MD5Sum md5;
	std::vector<u8> eeprom;
	std::vector<u8> serial;
	naomi_cart::loadGameBlobs(game, &archive, nullptr, cartridge, md5, true, eeprom, serial);
	EXPECT_TRUE(eeprom.empty());
	EXPECT_TRUE(serial.empty());
	EXPECT_EQ(digest(MD5Sum()), digest(md5));

	auto& incomplete = archive.add("optional-eeprom.bin", { 1, 2, 3, 4 }, 2);
	EXPECT_THROW(naomi_cart::loadGameBlobs(game, &archive, nullptr, cartridge, md5, true, eeprom, serial), NaomiCartException);
	EXPECT_EQ(1u, incomplete.stats.calls);
	EXPECT_TRUE(eeprom.empty());
}

TEST(NaomiRomReadTest, ShortMemberIsRejectedBeforePublish)
{
	InputDescriptors inputs{};
	Game game{};
	game.name = "synthetic";
	game.inputs = &inputs;
	game.blobs[0] = { "program.bin", 0, 4, 0x1111, Normal, 0 };
	game.blobs[1] = { "serial.bin", 0, 0x84, 0x3333, Eeprom, 0 };
	game.blobs[2] = { "default-eeprom.bin", 0, 4, 0x2222, Eeprom, 0 };
	game.blobs[3] = { "copy.bin", 8, 4, 0, Copy, 0 };
	FixtureArchive archive;
	auto& program = archive.add("program.bin", { 0x10, 0x20, 0x30, 0x40 }, std::nullopt, 0x1111);
	std::vector<u8> serialBytes(0x84);
	for (size_t i = 0; i < serialBytes.size(); i++)
		serialBytes[i] = (u8)i;
	auto& serialEntry = archive.add("serial.bin", serialBytes, std::nullopt, 0x3333);
	auto& shortEeprom = archive.add("default-eeprom.bin", { 0xaa, 0xbb, 0xcc, 0xdd }, 2);
	std::vector<u8> defaultEeprom{ 0xee };
	std::vector<u8> serial{ 0x99 };
	MD5Sum md5;
	const auto oldDigest = digest(md5);
	CurrentCartridge = nullptr;
	NaomiGameInputs = nullptr;
	const u8 *previousGlobalEeprom = naomi_default_eeprom;
	EXPECT_THROW(naomi_cart::loadGameArchive(game, &archive, nullptr, new TestCartridge(16), md5, true, defaultEeprom, serial), NaomiCartException);
	EXPECT_EQ(nullptr, CurrentCartridge);
	EXPECT_EQ(nullptr, NaomiGameInputs);
	EXPECT_EQ(previousGlobalEeprom, naomi_default_eeprom);
	EXPECT_EQ(1u, program.stats.calls);
	EXPECT_EQ(1u, serialEntry.stats.calls);
	EXPECT_EQ(1u, shortEeprom.stats.calls);
	EXPECT_EQ(oldDigest, digest(md5));
	EXPECT_EQ((std::vector<u8>{ 0xee }), defaultEeprom);
	EXPECT_EQ((std::vector<u8>{ 0x99 }), serial);

	FixtureArchive completeArchive;
	completeArchive.add("program.bin", { 0x10, 0x20, 0x30, 0x40 }, std::nullopt, 0x1111);
	completeArchive.add("serial.bin", serialBytes, std::nullopt, 0x3333);
	completeArchive.add("default-eeprom.bin", { 0xaa, 0xbb, 0xcc, 0xdd }); // filename fallback
	CurrentCartridge = nullptr;
	TestCartridge *retryCartridge = new TestCartridge(16);
	defaultEeprom.clear();	serial.clear();	md5 = MD5Sum();
	naomi_cart::loadGameArchive(game, &completeArchive, nullptr, retryCartridge, md5, true, defaultEeprom, serial);
	ASSERT_EQ(retryCartridge, CurrentCartridge);
	const std::array<u8, 4> loadedProgram{ 0x10, 0x20, 0x30, 0x40 };
	EXPECT_EQ(0, memcmp(loadedProgram.data(), retryCartridge->bytes(), loadedProgram.size()));
	const std::array<u8, 4> copiedProgram{ 0x10, 0x20, 0x30, 0x40 };
	EXPECT_EQ(0, memcmp(copiedProgram.data(), retryCartridge->bytes() + 8, copiedProgram.size()));
	EXPECT_EQ(serialBytes, serial);
	EXPECT_EQ((std::vector<u8>{ 0xaa, 0xbb, 0xcc, 0xdd }), defaultEeprom);
	std::vector<u8> expectedHashInput{ 0x10, 0x20, 0x30, 0x40 };
	expectedHashInput.insert(expectedHashInput.end(), serialBytes.begin(), serialBytes.end());
	expectedHashInput.insert(expectedHashInput.end(), { 0xaa, 0xbb, 0xcc, 0xdd });
	EXPECT_EQ(MD5Sum().add(expectedHashInput).getDigest(), digest(md5));
	delete CurrentCartridge;
	CurrentCartridge = nullptr;
	NaomiGameInputs = nullptr;
}

TEST(NaomiRomReadTest, BiosProgramAndEepromPublishTogetherAndSwapBE16)
{
	const int oldRegion = config::Region;
	const u32 oldBiosSize = settings.platform.bios_size;
	settings.platform.bios_size = 16;
	BIOS_t bios{};
	bios.name = "synthetic";
	bios.blobs[0] = { 0, "bios-program.bin", 2, 4, 0x1001, Normal };
	bios.blobs[1] = { 0, "bios-eeprom.bin", 0, 4, 0x1002, EepromBE16 };
	FixtureArchive archive;
	auto& program = archive.add("bios-program.bin", { 1, 2, 3, 4 }, std::nullopt, 0x1001);
	auto& shortEeprom = archive.add("bios-eeprom.bin", { 0x10, 0x20, 0x30, 0x40 }, 2, 0x1002);
	std::array<u8, 16> biosBytes;
	biosBytes.fill(0x77);
	std::vector<u8> eeprom{ 0xee };
	int region = -1;
	std::array<u8, 16> oldBiosDigest;
	memcpy(oldBiosDigest.data(), settings.network.md5.bios, oldBiosDigest.size());
	EXPECT_THROW(naomi_cart::loadBiosBlobs(bios, &archive, nullptr, nullptr, -1,
			biosBytes.data(), true, region, eeprom), NaomiCartException);
	EXPECT_EQ(1u, program.stats.calls);
	EXPECT_EQ(1u, shortEeprom.stats.calls);
	EXPECT_EQ(0, memcmp(biosBytes.data(), std::array<u8, 16>{ 0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77 }.data(), biosBytes.size()));
	EXPECT_EQ(-1, region);
	EXPECT_EQ((std::vector<u8>{ 0xee }), eeprom);
	EXPECT_TRUE(std::equal(oldBiosDigest.begin(), oldBiosDigest.end(), settings.network.md5.bios));

	FixtureArchive completeArchive;
	completeArchive.add("bios-program.bin", { 1, 2, 3, 4 }, std::nullopt, 0x1001);
	completeArchive.add("bios-eeprom.bin", { 0x10, 0x20, 0x30, 0x40 }, std::nullopt, 0x1002);
	EXPECT_TRUE(naomi_cart::loadBiosBlobs(bios, &completeArchive, nullptr, nullptr, -1,
			biosBytes.data(), true, region, eeprom));
	EXPECT_EQ((std::array<u8, 4>{ 1, 2, 3, 4 }), (std::array<u8, 4>{ biosBytes[2], biosBytes[3], biosBytes[4], biosBytes[5] }));
	EXPECT_EQ((std::vector<u8>{ 0x20, 0x10, 0x40, 0x30 }), eeprom);
	const auto expectedDigest = MD5Sum().add(std::array<u8, 4>{ 1, 2, 3, 4 }).add(std::array<u8, 4>{ 0x20, 0x10, 0x40, 0x30 }).getDigest();
	EXPECT_TRUE(std::equal(expectedDigest.begin(), expectedDigest.end(), settings.network.md5.bios));
	memcpy(settings.network.md5.bios, oldBiosDigest.data(), oldBiosDigest.size());
	config::Region.override(oldRegion);
	settings.platform.bios_size = oldBiosSize;
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
	std::vector<u8> pendingSerial;
	MD5Sum md5;

	MemoryArchive shortArchive(serial, 0x83);
	auto shortFile = openFile(shortArchive);
	EXPECT_THROW(naomi_cart::loadGameBlob(*shortFile, Eeprom, cartridge, 0, serial.size(), "serial.bin", md5, true, defaultEeprom, pendingSerial), NaomiCartException);
	EXPECT_EQ(0, memcmp(previous.data(), getGameSerialId(), previous.size()));

	MemoryArchive completeArchive(serial);
	auto completeFile = openFile(completeArchive);
	naomi_cart::loadGameBlob(*completeFile, Eeprom, cartridge, 0, serial.size(), "serial.bin", md5, true, defaultEeprom, pendingSerial);
	EXPECT_EQ(0, memcmp(previous.data(), getGameSerialId(), previous.size()));
	EXPECT_EQ(serial, pendingSerial);
	EXPECT_EQ(MD5Sum().add(serial).getDigest(), digest(md5));
}
