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
#include <cerrno>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <sys/acl.h>
#include <uuid/uuid.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif

namespace {

enum class Failure { None, Header, Preview, RzipHeader, Payload, LengthPatch, Seek, Close, Replace };

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
	s64 tell() override { return position; }
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

template<typename StorageType>
bool save(StorageType& storage, const std::string& path, const std::vector<u8>& payload,
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

template<typename StorageType>
bool load(StorageType& storage, const std::string& path, std::vector<u8>& payload,
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

#ifndef _WIN32
class ScopedUmask
{
public:
	explicit ScopedUmask(mode_t mask) : original(umask(mask)) {}
	~ScopedUmask() { umask(original); }

private:
	mode_t original;
};
#endif

#if defined(__APPLE__)
acl_t makeMacExecuteDenyAcl()
{
	// The stable macOS Everyone principal UUID.
	uuid_t everyone = {0xab, 0x72, 0x1a, 0x54, 0x1e, 0x2f, 0x11, 0xd0,
		0x98, 0x19, 0x00, 0xaa, 0x00, 0x40, 0x52, 0x9b};
	acl_t acl = acl_init(1);
	if (acl == nullptr)
		return nullptr;
	acl_entry_t entry;
	acl_permset_t permissions;
	if (acl_create_entry(&acl, &entry) != 0
		|| acl_set_tag_type(entry, ACL_EXTENDED_DENY) != 0
		|| acl_set_qualifier(entry, everyone) != 0
		|| acl_get_permset(entry, &permissions) != 0
		|| acl_clear_perms(permissions) != 0
		|| acl_add_perm(permissions, ACL_EXECUTE) != 0
		|| acl_set_permset(entry, permissions) != 0)
	{
		acl_free(acl);
		return nullptr;
	}
	return acl;
}
#endif

} // namespace

TEST(SavestateWriteTest, ExistingSlotSurvivesFailedCommit)
{
	const Failure failures[] = {Failure::Header, Failure::Preview, Failure::Payload,
		Failure::LengthPatch, Failure::Seek, Failure::Close, Failure::Replace};
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

TEST(SavestateWriteTest, NativeSaveAndReplacementReloadPayload)
{
	const auto root = std::filesystem::temp_directory_path()
		/ ("hollycast-save-reload-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	ASSERT_TRUE(std::filesystem::create_directory(root));
	const auto destination = root / "slot.state";
	ASSERT_TRUE(save(hostfs::storage(), destination.string(), oldPayload, preview));
	std::vector<u8> loaded, loadedPreview;
	ASSERT_TRUE(load(hostfs::storage(), destination.string(), loaded, loadedPreview));
	EXPECT_EQ(loaded, oldPayload);
	EXPECT_EQ(loadedPreview, preview);

	ASSERT_TRUE(save(hostfs::storage(), destination.string(), newPayload));
	loaded.clear();
	loadedPreview.clear();
	ASSERT_TRUE(load(hostfs::storage(), destination.string(), loaded, loadedPreview));
	EXPECT_EQ(loaded, newPayload);
	EXPECT_TRUE(loadedPreview.empty());
	EXPECT_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}), 1);
	std::filesystem::remove_all(root);
}

#ifndef _WIN32
TEST(SavestateWriteTest, NativeOverwritePreservesPrivateModeAndAccessAcl)
{
	const auto root = std::filesystem::temp_directory_path()
		/ ("hollycast-save-permissions-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	ASSERT_TRUE(std::filesystem::create_directory(root));
	const auto destination = root / "slot.state";
	{
		std::ofstream oldFile(destination, std::ios::binary);
		oldFile << "previous private slot";
	}
	ASSERT_EQ(chmod(destination.c_str(), 0600), 0);

	std::vector<u8> originalAcl;
#if defined(__linux__)
	struct PosixAclEntry { u16 tag; u16 permissions; u32 id; };
	const u32 aclVersion = 2;
	const PosixAclEntry aclEntries[] = {
		{1, 6, 0xffffffffu}, // ACL_USER_OBJ
		{2, 0, static_cast<u32>(getuid())}, // ACL_USER
		{4, 0, 0xffffffffu}, // ACL_GROUP_OBJ
		{16, 0, 0xffffffffu}, // ACL_MASK
		{32, 0, 0xffffffffu}, // ACL_OTHER
	};
	std::vector<u8> requestedAcl(sizeof(aclVersion) + sizeof(aclEntries));
	std::memcpy(requestedAcl.data(), &aclVersion, sizeof(aclVersion));
	std::memcpy(requestedAcl.data() + sizeof(aclVersion), aclEntries, sizeof(aclEntries));
	if (setxattr(destination.c_str(), "system.posix_acl_access", requestedAcl.data(), requestedAcl.size(), 0) == 0)
	{
		const ssize_t aclSize = getxattr(destination.c_str(), "system.posix_acl_access", nullptr, 0);
		ASSERT_GT(aclSize, 0);
		originalAcl.resize(static_cast<size_t>(aclSize));
		ASSERT_EQ(getxattr(destination.c_str(), "system.posix_acl_access", originalAcl.data(), originalAcl.size()), aclSize);
	}
	else if (errno != EOPNOTSUPP && errno != ENOTSUP)
	{
		FAIL() << "Could not set test ACL: " << std::strerror(errno);
	}
#elif defined(__APPLE__)
	acl_t expectedAcl = makeMacExecuteDenyAcl();
	ASSERT_NE(expectedAcl, nullptr);
	if (acl_set_file(destination.c_str(), ACL_TYPE_EXTENDED, expectedAcl) != 0)
	{
		const int aclError = errno;
		acl_free(expectedAcl);
		if (aclError != EOPNOTSUPP && aclError != ENOTSUP)
			FAIL() << "Could not set test ACL: " << std::strerror(aclError);
		expectedAcl = nullptr;
	}
#endif

	auto publish = [&](const std::string& contents) {
		hostfs::AtomicFile transaction(hostfs::storage(), destination.string());
		if (transaction.file() == nullptr
			|| transaction.file()->write(contents.data(), 1, contents.size()) != contents.size())
			return false;
		hostfs::File *file = transaction.takeFile();
		const bool flushed = file->flush();
		const bool closed = file->close();
		delete file;
		return flushed && closed && transaction.publish();
	};

	{
		ScopedUmask permissiveUmask(0022);
		hostfs::AtomicFile abandoned(hostfs::storage(), destination.string());
		ASSERT_NE(abandoned.file(), nullptr);
		struct stat staged;
		ASSERT_EQ(stat(abandoned.temporaryPath().c_str(), &staged), 0);
		EXPECT_EQ(staged.st_mode & 0777, 0600);
#if defined(__linux__)
		if (!originalAcl.empty())
		{
			std::vector<u8> stagedAcl(originalAcl.size());
			ASSERT_EQ(getxattr(abandoned.temporaryPath().c_str(), "system.posix_acl_access", stagedAcl.data(), stagedAcl.size()),
				static_cast<ssize_t>(stagedAcl.size()));
			EXPECT_EQ(stagedAcl, originalAcl);
		}
#elif defined(__APPLE__)
		if (expectedAcl != nullptr)
		{
			acl_t stagedAcl = acl_get_file(abandoned.temporaryPath().c_str(), ACL_TYPE_EXTENDED);
			ASSERT_NE(stagedAcl, nullptr);
			char *expectedText = acl_to_text(expectedAcl, nullptr);
			char *stagedText = acl_to_text(stagedAcl, nullptr);
			ASSERT_NE(expectedText, nullptr);
			ASSERT_NE(stagedText, nullptr);
			EXPECT_STREQ(expectedText, stagedText);
			acl_free(expectedText);
			acl_free(stagedText);
			acl_free(stagedAcl);
		}
#endif
		const std::string incomplete = "incomplete replacement";
		ASSERT_EQ(abandoned.file()->write(incomplete.data(), 1, incomplete.size()), incomplete.size());
	}
	{
		std::ifstream previous(destination, std::ios::binary);
		EXPECT_EQ(std::string((std::istreambuf_iterator<char>(previous)), std::istreambuf_iterator<char>()),
			"previous private slot");
	}

	{
		ScopedUmask permissiveUmask(0022);
		ASSERT_TRUE(publish("complete private replacement"));
	}
	struct stat replaced;
	ASSERT_EQ(stat(destination.c_str(), &replaced), 0);
	EXPECT_EQ(replaced.st_mode & 0777, 0600);
	std::ifstream input(destination, std::ios::binary);
	EXPECT_EQ(std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()),
		"complete private replacement");

#if defined(__linux__)
	if (!originalAcl.empty())
	{
		std::vector<u8> actualAcl(originalAcl.size());
		ASSERT_EQ(getxattr(destination.c_str(), "system.posix_acl_access", actualAcl.data(), actualAcl.size()),
			static_cast<ssize_t>(actualAcl.size()));
		EXPECT_EQ(actualAcl, originalAcl);
	}

	// A sibling can inherit a directory default ACL even when the previous
	// slot had no access ACL. Verify that replacement does not retain it.
	const auto inheritedRoot = root / "inherited-acl";
	ASSERT_TRUE(std::filesystem::create_directory(inheritedRoot));
	const auto inheritedDestination = inheritedRoot / "slot.state";
	{
		std::ofstream oldFile(inheritedDestination, std::ios::binary);
		oldFile << "slot without an ACL";
	}
	ASSERT_EQ(chmod(inheritedDestination.c_str(), 0640), 0);
	const int removeAclResult = removexattr(inheritedDestination.c_str(), "system.posix_acl_access");
	ASSERT_TRUE(removeAclResult == 0 || errno == ENODATA || errno == EOPNOTSUPP || errno == ENOTSUP);
	const PosixAclEntry defaultAclEntries[] = {
		{1, 7, 0xffffffffu},
		{2, 7, static_cast<u32>(getuid() == 65534 ? 65533 : 65534)},
		{4, 0, 0xffffffffu},
		{16, 7, 0xffffffffu},
		{32, 0, 0xffffffffu},
	};
	std::vector<u8> defaultAcl(sizeof(aclVersion) + sizeof(defaultAclEntries));
	std::memcpy(defaultAcl.data(), &aclVersion, sizeof(aclVersion));
	std::memcpy(defaultAcl.data() + sizeof(aclVersion), defaultAclEntries, sizeof(defaultAclEntries));
	if (setxattr(inheritedRoot.c_str(), "system.posix_acl_default", defaultAcl.data(), defaultAcl.size(), 0) == 0)
	{
		ScopedUmask permissiveUmask(0022);
		hostfs::AtomicFile inherited(hostfs::storage(), inheritedDestination.string());
		ASSERT_NE(inherited.file(), nullptr);
		struct stat inheritedStage;
		ASSERT_EQ(stat(inherited.temporaryPath().c_str(), &inheritedStage), 0);
		EXPECT_EQ(inheritedStage.st_mode & 0777, 0640);
		errno = 0;
		EXPECT_EQ(getxattr(inherited.temporaryPath().c_str(), "system.posix_acl_access", nullptr, 0), -1);
		EXPECT_EQ(errno, ENODATA);
		const std::string replacement = "slot without inherited ACL";
		ASSERT_EQ(inherited.file()->write(replacement.data(), 1, replacement.size()), replacement.size());
		hostfs::File *file = inherited.takeFile();
		ASSERT_TRUE(file->flush());
		ASSERT_TRUE(file->close());
		delete file;
		ASSERT_TRUE(inherited.publish());
		errno = 0;
		EXPECT_EQ(getxattr(inheritedDestination.c_str(), "system.posix_acl_access", nullptr, 0), -1);
		EXPECT_EQ(errno, ENODATA);
		std::filesystem::remove_all(inheritedRoot);
	}
	else if (errno != EOPNOTSUPP && errno != ENOTSUP)
	{
		FAIL() << "Could not set test default ACL: " << std::strerror(errno);
	}
#elif defined(__APPLE__)
	if (expectedAcl != nullptr)
	{
		acl_t actualAcl = acl_get_file(destination.c_str(), ACL_TYPE_EXTENDED);
		ASSERT_NE(actualAcl, nullptr);
		char *expectedText = acl_to_text(expectedAcl, nullptr);
		char *actualText = acl_to_text(actualAcl, nullptr);
		ASSERT_NE(expectedText, nullptr);
		ASSERT_NE(actualText, nullptr);
		EXPECT_STREQ(expectedText, actualText);
		acl_free(expectedText);
		acl_free(actualText);
		acl_free(actualAcl);
		acl_free(expectedAcl);
	}
#endif
	EXPECT_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}), 1);
	std::filesystem::remove_all(root);
}
#endif
