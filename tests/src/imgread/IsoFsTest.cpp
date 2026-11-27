#include "gtest/gtest.h"
#include "imgread/common.h"
#include "imgread/iso9660.h"
#include "imgread/isofs.h"

namespace {
u64 encode733(u32 value)
{
	const u32 reversed = ((value & 0x000000ff) << 24) | ((value & 0x0000ff00) << 8)
			| ((value & 0x00ff0000) >> 8) | ((value & 0xff000000) >> 24);
	return value | (static_cast<u64>(reversed) << 32);
}

class MemoryTrack final : public TrackFile
{
public:
	explicit MemoryTrack(size_t sectorCount) : bytes(sectorCount * 2048) {}
	bool Read(u32 fad, u8 *dst, SectorFormat *sectorType, u8 *, SubcodeFormat *subcodeType) override
	{
		if (fad < 150 || static_cast<size_t>(fad - 150) >= bytes.size() / 2048)
			return false;
		memcpy(dst, bytes.data() + static_cast<size_t>(fad - 150) * 2048, 2048);
		*sectorType = SECFMT_2048_MODE1;
		*subcodeType = SUBFMT_NONE;
		return true;
	}
	std::vector<u8> bytes;
};

class IsoFsTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		disc.tracks.emplace_back();
		disc.tracks.back().StartFAD = 150;
		disc.tracks.back().EndFAD = 199;
		disc.tracks.back().CTRL = 4;
		auto *track = new MemoryTrack(50);
		disc.tracks.back().file = track;
		disc.sessions.push_back({150, 1});
		disc.LeadOut.StartFAD = 200;
		disc.EndFAD = 200;

		auto *pvd = reinterpret_cast<iso9660_pvd_t *>(track->bytes.data() + 16 * 2048);
		pvd->type = 1;
		memcpy(pvd->id, ISO_STANDARD_ID, 5);
		pvd->version = 1;
		pvd->root_directory_record.extent = encode733(17);
		pvd->root_directory_record.size = encode733(2048);

		auto *fileRecord = reinterpret_cast<iso9660_dir_t *>(track->bytes.data() + 17 * 2048);
		const char filename[] = "TEST.BIN;1";
		fileRecord->length = static_cast<u8>(sizeof(iso9660_dir_t) + sizeof(filename) - 1);
		fileRecord->extent = encode733(18);
		fileRecord->size = encode733(16);
		fileRecord->filename.str[0] = sizeof(filename) - 1;
		memcpy(fileRecord->filename.str + 1, filename, sizeof(filename) - 1);
		memset(track->bytes.data() + 18 * 2048, 0x5a, 16);
	}
	Disc disc;
};
}

TEST_F(IsoFsTest, ReadsValidFileAndReturnsZeroAfterItsEnd)
{
	IsoFs filesystem(&disc);
	std::unique_ptr<IsoFs::Directory> root(filesystem.getRoot());
	std::unique_ptr<IsoFs::Entry> entry(root->getEntry("TEST.BIN"));
	ASSERT_NE(nullptr, entry);
	ASSERT_FALSE(entry->isDirectory());
	EXPECT_EQ(16u, entry->getLength());

	auto *file = static_cast<IsoFs::File *>(entry.get());
	u8 buffer[16]{};
	EXPECT_EQ(16u, file->read(buffer, sizeof(buffer), 0));
	for (u8 byte : buffer)
		EXPECT_EQ(0x5a, byte);
	EXPECT_EQ(0u, file->read(buffer, sizeof(buffer), 16));
	EXPECT_EQ(0u, file->read(buffer, sizeof(buffer), 17));
}
