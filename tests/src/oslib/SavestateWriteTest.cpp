#include "gtest/gtest.h"
#include "oslib/storage.h"
#include "archive/rzip.h"

#include <map>
#include <memory>
#include <algorithm>
#include <cstring>
#include <vector>
#include <array>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {

enum class Failure { None, Header, Preview, RzipHeader, Payload, LengthPatch, Tell, Seek, Close, Replace };

struct FakeStorageState
{
	std::map<std::string, std::vector<u8>> files;
	std::map<std::string, u64> dates;
	Failure failure = Failure::None;
	int writeCall = 0;
	int nextTemporary = 0;
	int closeCalls = 0;
	int fileDestructions = 0;
};

class FakeFile : public hostfs::File
{
public:
	FakeFile(std::shared_ptr<FakeStorageState> state, std::vector<u8>& bytes, bool reading)
		: state(std::move(state)), bytes(bytes), reading(reading) {}
	~FakeFile() override { close(); ++state->fileDestructions; }

	size_t read(void *buffer, size_t size, size_t count) override
	{
		const size_t available = position < bytes.size() ? bytes.size() - position : 0;
		const size_t amount = std::min(available, size * count);
		if (amount != 0)
			std::memcpy(buffer, bytes.data() + position, amount);
		position += amount;
		return size == 0 ? 0 : amount / size;
	}
	size_t write(const void *buffer, size_t size, size_t count) override
	{
		const int call = ++state->writeCall;
		if ((state->failure == Failure::Header && call == 1)
			|| (state->failure == Failure::Preview && call == 2)
			|| (state->failure == Failure::RzipHeader && call == 3)
			|| (state->failure == Failure::Payload && call == 7)
			|| (state->failure == Failure::LengthPatch && call == 8))
			return 0;
		const size_t amount = size * count;
		if (position + amount > bytes.size())
			bytes.resize(position + amount);
		std::memcpy(bytes.data() + position, buffer, amount);
		position += amount;
		return count;
	}
	s64 tell() override { return !reading && state->failure == Failure::Tell ? -1 : static_cast<s64>(position); }
	int seek(s64 offset, int whence) override
	{
		if (!reading && state->failure == Failure::Seek)
			return -1;
		s64 target = whence == SEEK_SET ? offset : whence == SEEK_CUR ? s64(position) + offset : s64(bytes.size()) + offset;
		if (target < 0)
			return -1;
		position = size_t(target);
		return 0;
	}
	char *gets(char *, int) override { return nullptr; }
	s64 size() override { return bytes.size(); }
	int eof() override { return position >= bytes.size(); }
	int error() override { return 0; }
	bool flush() override { return true; }
	bool close() override
	{
		if (closed)
			return true;
		closed = true;
		++state->closeCalls;
		return reading || state->failure != Failure::Close;
	}

private:
	std::shared_ptr<FakeStorageState> state;
	std::vector<u8>& bytes;
	size_t position = 0;
	bool reading;
	bool closed = false;
};

class FakeStorage : public hostfs::Storage
{
public:
	FakeStorage() : state(std::make_shared<FakeStorageState>()) {}
	bool isKnownPath(const std::string&) override { return true; }
	std::vector<hostfs::FileInfo> listContent(const std::string&) override { return {}; }
	hostfs::File *openFile(const std::string& path, const std::string& mode) override
	{
		auto found = state->files.find(path);
		if (mode[0] == 'r')
			return found == state->files.end() ? nullptr : new FakeFile(state, found->second, true);
		return nullptr;
	}
	std::string getParentPath(const std::string&) override { return {}; }
	std::string getSubPath(const std::string& parent, const std::string& child) override { return parent + "/" + child; }
	hostfs::FileInfo getFileInfo(const std::string&) override { return {}; }
	bool exists(const std::string& path) override { return state->files.count(path) != 0; }
	hostfs::File *createTemporaryFile(const std::string& destination, std::string& temporary) override
	{
		state->writeCall = 0;
		temporary = destination + ".tmp-" + std::to_string(++state->nextTemporary);
		return new FakeFile(state, state->files[temporary], false);
	}
	bool replaceFile(const std::string& temporary, const std::string& destination) override
	{
		if (state->failure == Failure::Replace)
			return false;
		auto found = state->files.find(temporary);
		if (found == state->files.end())
			return false;
		state->files[destination] = std::move(found->second);
		state->files.erase(found);
		state->dates[destination] = ++date;
		return true;
	}
	bool removeFile(const std::string& path) override { state->files.erase(path); return true; }

	std::shared_ptr<FakeStorageState> state;
	static u64 date;
};
u64 FakeStorage::date = 100;

struct StateHeader
{
	char magic[8] = {'F','L','Y','S','A','V','E','1'};
	u64 date = 50;
	u32 version = 64;
	u32 pngSize = 0;
};

bool save(FakeStorage& storage, const std::string& path, const std::vector<u8>& payload,
		const std::vector<u8>& preview = {})
{
	hostfs::AtomicFile transaction(storage, path);
	if (transaction.file() == nullptr)
		return false;
	StateHeader header;
	header.pngSize = preview.size();
	if (transaction.file()->write(&header, sizeof(header), 1) != 1
		|| (!preview.empty() && transaction.file()->write(preview.data(), 1, preview.size()) != preview.size()))
		return false;
	RZipFile writer;
	if (!writer.Open(transaction.file(), true))
		return false;
	transaction.takeFile();
	if (writer.Write(payload.data(), payload.size()) != payload.size() || !writer.Finish())
		return false;
	return transaction.publish();
}

bool load(FakeStorage& storage, const std::string& path, std::vector<u8>& payload,
		std::vector<u8>& preview)
{
	std::unique_ptr<hostfs::File> file(storage.openFile(path, "rb"));
	if (!file)
		return false;
	StateHeader header;
	if (file->read(&header, sizeof(header), 1) != 1 || std::memcmp(header.magic, "FLYSAVE1", 8) != 0)
		return false;
	preview.resize(header.pngSize);
	if (!preview.empty() && file->read(preview.data(), 1, preview.size()) != preview.size())
		return false;
	RZipFile reader;
	if (!reader.Open(file.release(), false))
		return false;
	payload.resize(reader.Size());
	return reader.Read(payload.data(), payload.size()) == payload.size();
}

const std::vector<u8> oldPayload{1, 2, 3, 4, 5, 6, 7};
const std::vector<u8> newPayload{9, 8, 7};
const std::vector<u8> preview{0x89, 'P', 'N', 'G', 0x0d, 0x0a};

} // namespace

TEST(SavestateWriteTest, ExistingSlotSurvivesFailedCommit)
{
	const Failure failures[] = {Failure::Header, Failure::Preview, Failure::RzipHeader, Failure::Payload,
		Failure::LengthPatch, Failure::Tell, Failure::Seek, Failure::Close, Failure::Replace};
	for (Failure failure : failures)
	{
		FakeStorage storage;
		ASSERT_TRUE(save(storage, "slot.state", oldPayload, preview));
		const auto previousBytes = storage.state->files.at("slot.state");
		const u64 previousDate = storage.state->dates.at("slot.state");
		storage.state->failure = failure;
		EXPECT_FALSE(save(storage, "slot.state", newPayload, preview));
		EXPECT_EQ(storage.state->files.at("slot.state"), previousBytes);
		EXPECT_EQ(storage.state->dates.at("slot.state"), previousDate);
		EXPECT_EQ(storage.state->files.size(), 1u) << "failed temporary file was not removed";
		std::vector<u8> previousPayload, previousPreview;
		ASSERT_TRUE(load(storage, "slot.state", previousPayload, previousPreview));
		EXPECT_EQ(previousPayload, oldPayload);
		EXPECT_EQ(previousPreview, preview);
		storage.state->failure = Failure::None;
		ASSERT_TRUE(save(storage, "slot.state", newPayload));
		std::vector<u8> loaded, loadedPreview;
		ASSERT_TRUE(load(storage, "slot.state", loaded, loadedPreview));
		EXPECT_EQ(loaded, newPayload);
		EXPECT_TRUE(loadedPreview.empty());
	}

	FakeStorage firstSave;
	firstSave.state->failure = Failure::Payload;
	EXPECT_FALSE(save(firstSave, "new.state", newPayload, preview));
	EXPECT_FALSE(firstSave.exists("new.state"));
	EXPECT_TRUE(firstSave.state->files.empty());
}

TEST(SavestateWriteTest, SuccessfulReplacementReopensWithPreviewAndSmallerPayload)
{
	FakeStorage storage;
	ASSERT_TRUE(save(storage, "slot.state", oldPayload, preview));
	EXPECT_EQ(storage.state->closeCalls, 1);
	EXPECT_EQ(storage.state->fileDestructions, 1);
	ASSERT_TRUE(save(storage, "slot.state", newPayload));
	std::vector<u8> loaded, loadedPreview;
	ASSERT_TRUE(load(storage, "slot.state", loaded, loadedPreview));
	EXPECT_EQ(loaded, newPayload);
	EXPECT_TRUE(loadedPreview.empty());
	ASSERT_TRUE(save(storage, "slot.state", oldPayload, preview));
	ASSERT_TRUE(load(storage, "slot.state", loaded, loadedPreview));
	EXPECT_EQ(loaded, oldPayload);
	EXPECT_EQ(loadedPreview, preview);
}

TEST(SavestateWriteTest, AutosaveSlotUsesTheSameAtomicReplacement)
{
	FakeStorage storage;
	ASSERT_TRUE(save(storage, "autosave.state", oldPayload));
	const auto previous = storage.state->files.at("autosave.state");
	storage.state->failure = Failure::Payload;
	EXPECT_FALSE(save(storage, "autosave.state", newPayload, preview));
	EXPECT_EQ(storage.state->files.at("autosave.state"), previous);
	storage.state->failure = Failure::None;
	ASSERT_TRUE(save(storage, "autosave.state", newPayload));
	std::vector<u8> loaded, loadedPreview;
	ASSERT_TRUE(load(storage, "autosave.state", loaded, loadedPreview));
	EXPECT_EQ(loaded, newPayload);
}

TEST(SavestateWriteTest, OverlappingAttemptsUseIndependentTemporaryFiles)
{
	FakeStorage storage;
	hostfs::AtomicFile first(storage, "slot.state");
	hostfs::AtomicFile second(storage, "slot.state");
	ASSERT_NE(first.file(), nullptr);
	ASSERT_NE(second.file(), nullptr);
	EXPECT_NE(first.temporaryPath(), second.temporaryPath());
}

TEST(SavestateWriteTest, FailedRzipOpenReleasesTheUnclaimedStreamOnce)
{
	FakeStorage storage;
	storage.state->failure = Failure::RzipHeader;
	EXPECT_FALSE(save(storage, "new.state", newPayload, preview));
	EXPECT_TRUE(storage.state->files.empty());
	EXPECT_EQ(storage.state->closeCalls, 1);
	EXPECT_EQ(storage.state->fileDestructions, 1);
}

TEST(SavestateWriteTest, RamStreamCompletesCheckedRzip)
{
#if defined(__APPLE__) || defined(__linux__)
	std::array<u8, 1024> bytes{};
	FILE *output = fmemopen(bytes.data(), bytes.size(), "wb");
	ASSERT_NE(output, nullptr);
	RZipFile writer;
	ASSERT_TRUE(writer.Open(new hostfs::StdFile(output), true));
	ASSERT_EQ(writer.Write(newPayload.data(), newPayload.size()), newPayload.size());
	ASSERT_TRUE(writer.Finish());

	FILE *input = fmemopen(bytes.data(), bytes.size(), "rb");
	ASSERT_NE(input, nullptr);
	RZipFile reader;
	ASSERT_TRUE(reader.Open(new hostfs::StdFile(input), false));
	std::vector<u8> loaded(reader.Size());
	EXPECT_EQ(reader.Read(loaded.data(), loaded.size()), loaded.size());
	EXPECT_EQ(loaded, newPayload);
#else
	GTEST_SKIP() << "fmemopen is unavailable on this host";
#endif
}

TEST(SavestateWriteTest, NativeSiblingTransactionReplacesAndCleansUp)
{
	const auto root = std::filesystem::temp_directory_path()
		/ ("hollycast-save-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	ASSERT_TRUE(std::filesystem::create_directory(root));
	const auto destination = root / "slot.state";
	{
		std::ofstream oldFile(destination, std::ios::binary);
		oldFile << "previous slot";
	}
	auto publish = [&](const std::string& contents) {
		hostfs::AtomicFile transaction(hostfs::storage(), destination.string());
		if (transaction.file() == nullptr)
			return false;
		if (transaction.file()->write(contents.data(), 1, contents.size()) != contents.size())
			return false;
		hostfs::File *file = transaction.takeFile();
		const bool flushed = file->flush();
		const bool closed = file->close();
		delete file;
		return flushed && closed && transaction.publish();
	};
	ASSERT_TRUE(publish("complete replacement"));
	std::ifstream input(destination, std::ios::binary);
	const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	EXPECT_EQ(contents, "complete replacement");
	EXPECT_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}), 1);
	input.close();
	std::filesystem::remove(destination);
	ASSERT_TRUE(publish("first slot"));
	std::ifstream firstSlot(destination, std::ios::binary);
	const std::string firstContents((std::istreambuf_iterator<char>(firstSlot)), std::istreambuf_iterator<char>());
	EXPECT_EQ(firstContents, "first slot");
	EXPECT_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}), 1);
	std::filesystem::remove_all(root);
}
