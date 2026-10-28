// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "imgread/common.h"
#include "oslib/storage.h"
#include "stdclass.h"

#include <cstdio>
#include <vector>

#define TRACK_FILE FLYCAST_TEST_FILES "/test_cues/d/cs (0,1).bin"
static const std::vector<u8> TRACK_MD5 {
	0xd7, 0x8c, 0x60, 0x0a, 0x1b, 0x41, 0xa0, 0x2d,
	0x26, 0xfa, 0x78, 0x07, 0x37, 0x3f, 0xba, 0xa1
};

TEST(Md5SumTest, HostfsFileHashesContentsFromBeginning)
{
	hostfs::File *file = hostfs::storage().openFile(TRACK_FILE, "rb");
	ASSERT_NE(nullptr, file);
	ASSERT_EQ(0, file->seek(1000, SEEK_SET));
	const std::vector<u8> digest = MD5Sum().add(file).getDigest();
	delete file;
	EXPECT_EQ(TRACK_MD5, digest);
}

TEST(Md5SumTest, CueDigestUsesTrackContents)
{
	std::vector<u8> digest;
	Disc *disc = OpenDisc(FLYCAST_TEST_FILES "/test_cues/d/cs.cue", &digest);
	ASSERT_NE(nullptr, disc);
	delete disc;
	EXPECT_EQ(TRACK_MD5, digest);
}
