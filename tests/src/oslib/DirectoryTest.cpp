// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "oslib/directory.h"

#ifdef NOWIDE_WINDOWS
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>

namespace {
using Directory = std::unique_ptr<DIR, decltype(&flycast::closedir)>;

dirent *nextFile(DIR *dir)
{
	while (dirent *entry = flycast::readdir(dir))
		if (std::string(entry->d_name) != "." && std::string(entry->d_name) != "..")
			return entry;
	return nullptr;
}

class DirectoryTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
		root = std::filesystem::temp_directory_path() / ("hollycast-directory-" + std::to_string(nonce));
		std::filesystem::create_directories(root / "library");
		std::filesystem::create_directories(root / "artwork");
		std::ofstream(root / "library" / "Long library title.chd") << "fixture";
		std::ofstream(root / "artwork" / "cover.png") << "fixture";
	}

	void TearDown() override { std::filesystem::remove_all(root); }

	std::filesystem::path root;
};

TEST_F(DirectoryTest, ReadingAnotherStreamPreservesThePreviousEntry)
{
	Directory library(flycast::opendir((root / "library").string().c_str()), flycast::closedir);
	Directory artwork(flycast::opendir((root / "artwork").string().c_str()), flycast::closedir);
	ASSERT_NE(library, nullptr);
	ASSERT_NE(artwork, nullptr);
	const dirent *title = nextFile(library.get());
	ASSERT_NE(title, nullptr);
	ASSERT_STREQ(title->d_name, "Long library title.chd");
	const dirent *cover = nextFile(artwork.get());
	ASSERT_NE(cover, nullptr);
	EXPECT_STREQ(cover->d_name, "cover.png");
	EXPECT_STREQ(title->d_name, "Long library title.chd");
	EXPECT_NE(title, cover);
}

TEST_F(DirectoryTest, ConcurrentScansPreserveAnEntryUntilItsCallerCopiesIt)
{
	std::promise<bool> entryRead;
	std::promise<void> otherScanFinished;
	const auto finish = otherScanFinished.get_future();
	auto libraryScan = std::async(std::launch::async, [&] {
		Directory library(flycast::opendir((root / "library").string().c_str()), flycast::closedir);
		const dirent *title = library ? nextFile(library.get()) : nullptr;
		entryRead.set_value(title != nullptr);
		finish.wait();
		return title ? std::string(title->d_name) : std::string();
	});
	const bool ready = entryRead.get_future().get();
	Directory artwork(flycast::opendir((root / "artwork").string().c_str()), flycast::closedir);
	const dirent *cover = artwork ? nextFile(artwork.get()) : nullptr;
	// Release the worker before assertions so a failure cannot strand its future.
	otherScanFinished.set_value();
	EXPECT_TRUE(ready);
	ASSERT_NE(cover, nullptr);
	EXPECT_STREQ(cover->d_name, "cover.png");
	EXPECT_EQ(libraryScan.get(), "Long library title.chd");
}

TEST_F(DirectoryTest, ConcurrentScanCannotTurnAFileIntoADotDirectory)
{
	std::promise<bool> entryRead;
	std::promise<void> otherScanFinished;
	const auto finish = otherScanFinished.get_future();
	auto libraryScan = std::async(std::launch::async, [&] {
		Directory library(flycast::opendir((root / "library").string().c_str()), flycast::closedir);
		const dirent *title = library ? nextFile(library.get()) : nullptr;
		const size_t length = title ? std::strlen(title->d_name) : 0;
		entryRead.set_value(title != nullptr);
		finish.wait();
		// A string constructor measures then copies its source. Force a second
		// scan between those steps: a shared buffer can produce ".\0...", which
		// bypasses the dot-entry check but opens the same directory again.
		return title ? std::string(title->d_name, length) : std::string();
	});
	const bool ready = entryRead.get_future().get();
	Directory artwork(flycast::opendir((root / "artwork").string().c_str()), flycast::closedir);
	const dirent *dot = nullptr;
	if (artwork)
		while ((dot = flycast::readdir(artwork.get())) != nullptr && std::strcmp(dot->d_name, ".") != 0) {}
	otherScanFinished.set_value();
	EXPECT_TRUE(ready);
	ASSERT_NE(dot, nullptr);
	const std::string name = libraryScan.get();
	EXPECT_EQ(name.find('\0'), std::string::npos);
	EXPECT_EQ(name, "Long library title.chd");
	struct stat info{};
	const std::string path = (root / "library").string() + '/' + name;
	ASSERT_EQ(flycast::stat(path.c_str(), &info), 0);
	EXPECT_TRUE(S_ISREG(info.st_mode));
}

} // namespace
#endif
