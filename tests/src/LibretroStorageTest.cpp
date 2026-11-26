// Portions Copyright 2026 The Hollycast Authors
#include <gtest/gtest.h>

#include "../../shell/libretro/storage.h"
#include "../../core/oslib/storage.h"

#include <condition_variable>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct retro_vfs_file_handle
{
	FILE *file;
};

namespace {

struct CallbackState
{
	std::mutex mutex;
	std::condition_variable condition;
	std::string target;
	bool failPublish = false;
	bool failRollback = false;
	bool failBackupCleanup = false;
	bool blockBackupRename = false;
	bool backupRenameEntered = false;
	bool releaseBackupRename = false;
};

CallbackState callbacks;
retro_vfs_interface vfs{};
std::atomic<unsigned> rootId{0};

retro_vfs_file_handle *RETRO_CALLCONV openFile(const char *path, unsigned mode, unsigned)
{
	const bool read = (mode & RETRO_VFS_FILE_ACCESS_READ) != 0;
	const bool write = (mode & RETRO_VFS_FILE_ACCESS_WRITE) != 0;
	const bool update = (mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING) != 0;
	const char *openMode = read && write ? (update ? "r+b" : "w+b")
			: write ? (update ? "r+b" : "wb") : "rb";
	FILE *file = std::fopen(path, openMode);
	if (file == nullptr)
		return nullptr;
	return new retro_vfs_file_handle{file};
}

int RETRO_CALLCONV closeFile(retro_vfs_file_handle *handle)
{
	const int result = std::fclose(handle->file);
	delete handle;
	return result;
}

int64_t RETRO_CALLCONV fileSize(retro_vfs_file_handle *handle)
{
	const long current = std::ftell(handle->file);
	if (current < 0 || std::fseek(handle->file, 0, SEEK_END) != 0)
		return -1;
	const long size = std::ftell(handle->file);
	std::fseek(handle->file, current, SEEK_SET);
	return size;
}

int64_t RETRO_CALLCONV fileTell(retro_vfs_file_handle *handle)
{
	return std::ftell(handle->file);
}

int64_t RETRO_CALLCONV fileSeek(retro_vfs_file_handle *handle, int64_t offset, int position)
{
	const int origin = position == RETRO_VFS_SEEK_POSITION_START ? SEEK_SET
			: position == RETRO_VFS_SEEK_POSITION_CURRENT ? SEEK_CUR : SEEK_END;
	return std::fseek(handle->file, static_cast<long>(offset), origin);
}

int64_t RETRO_CALLCONV fileRead(retro_vfs_file_handle *handle, void *buffer, uint64_t size)
{
	return static_cast<int64_t>(std::fread(buffer, 1, static_cast<size_t>(size), handle->file));
}

int64_t RETRO_CALLCONV fileWrite(retro_vfs_file_handle *handle, const void *buffer, uint64_t size)
{
	return static_cast<int64_t>(std::fwrite(buffer, 1, static_cast<size_t>(size), handle->file));
}

int RETRO_CALLCONV fileFlush(retro_vfs_file_handle *handle)
{
	return std::fflush(handle->file);
}

int RETRO_CALLCONV removeFile(const char *path)
{
	{
		std::lock_guard<std::mutex> lock(callbacks.mutex);
		if (callbacks.failBackupCleanup
				&& std::string(path) == callbacks.target + ".hollycast-vfs-backup")
			return -1;
	}
	std::error_code error;
	return std::filesystem::remove(path, error) && !error ? 0 : -1;
}

int RETRO_CALLCONV renameFile(const char *source, const char *destination)
{
	const std::string from(source);
	const std::string to(destination);
	{
		std::unique_lock<std::mutex> lock(callbacks.mutex);
		const std::string backup = callbacks.target + ".hollycast-vfs-backup";
		if (from == callbacks.target && to == backup && callbacks.blockBackupRename)
		{
			callbacks.backupRenameEntered = true;
			callbacks.condition.notify_all();
			callbacks.condition.wait(lock, [] { return callbacks.releaseBackupRename; });
		}
		if (callbacks.failPublish && to == callbacks.target && from != backup)
			return -1;
		if (callbacks.failRollback && from == backup && to == callbacks.target)
			return -1;
	}
	std::error_code error;
	if (std::filesystem::exists(destination, error) || error)
		return -1;
	std::filesystem::rename(source, destination, error);
	return error ? -1 : 0;
}

int RETRO_CALLCONV pathStat(const char *path, int32_t *size)
{
	std::error_code error;
	const auto status = std::filesystem::status(path, error);
	if (error || !std::filesystem::exists(status))
		return 0;
	int result = RETRO_VFS_STAT_IS_VALID;
	if (std::filesystem::is_directory(status))
		result |= RETRO_VFS_STAT_IS_DIRECTORY;
	if (size != nullptr && std::filesystem::is_regular_file(status))
	{
		const auto fileSize = std::filesystem::file_size(path, error);
		if (!error)
			*size = static_cast<int32_t>(fileSize);
	}
	return result;
}

bool RETRO_CALLCONV environment(unsigned command, void *data)
{
	if (command != RETRO_ENVIRONMENT_GET_VFS_INTERFACE)
		return false;
	auto *info = static_cast<retro_vfs_interface_info *>(data);
	info->iface = &vfs;
	return true;
}

void initializeVfs(const std::filesystem::path& root)
{
	std::filesystem::create_directories(root);
	vfs = {};
	vfs.open = openFile;
	vfs.close = closeFile;
	vfs.size = fileSize;
	vfs.tell = fileTell;
	vfs.seek = fileSeek;
	vfs.read = fileRead;
	vfs.write = fileWrite;
	vfs.flush = fileFlush;
	vfs.remove = removeFile;
	vfs.rename = renameFile;
	vfs.stat = pathStat;
	hostfs::LibretroStorage::initialise(environment);
	std::lock_guard<std::mutex> lock(callbacks.mutex);
	callbacks.target.clear();
	callbacks.failPublish = false;
	callbacks.failRollback = false;
	callbacks.failBackupCleanup = false;
	callbacks.blockBackupRename = false;
	callbacks.backupRenameEntered = false;
	callbacks.releaseBackupRename = false;
}

std::filesystem::path makeRoot()
{
	return std::filesystem::temp_directory_path()
			/ ("hollycast-libretro-storage-" + std::to_string(++rootId));
}

void writeBytes(const std::filesystem::path& path, const std::string& bytes)
{
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string readStorageFile(hostfs::AllStorage& storage, const std::string& path)
{
	std::unique_ptr<hostfs::File> file(storage.openFile(path, "rb"));
	if (!file)
		return {};
	std::string result(static_cast<size_t>(file->size()), '\0');
	if (!result.empty())
		file->read(result.data(), 1, result.size());
	return result;
}

std::string stage(hostfs::AllStorage& storage, const std::string& destination, const std::string& bytes)
{
	std::string temporary;
	std::unique_ptr<hostfs::File> file(storage.createTemporaryFile(destination, temporary));
	if (!file)
		return {};
	if (file->write(bytes.data(), 1, bytes.size()) != bytes.size() || !file->close())
		return {};
	return temporary;
}

} // namespace

TEST(LibretroStorageTest, AllStorageRecoversSavestateAndMappingOnFirstLookup)
{
	const auto root = makeRoot();
	initializeVfs(root);
	hostfs::AllStorage& storage = hostfs::storage();

	for (const std::string name : {"slot.state", "mapping.cfg.save"})
	{
		const auto destination = root / name;
		const std::string destinationPath = destination.string();
		writeBytes(destination.string() + ".hollycast-vfs-backup", "old bytes");
		writeBytes(destination.string() + ".hollycast-vfs-transaction", "HCV1\n");

		if (name == "slot.state")
			EXPECT_TRUE(storage.exists(destinationPath));
		else
			EXPECT_EQ(readStorageFile(storage, destinationPath), "old bytes");
		EXPECT_EQ(readStorageFile(storage, destinationPath), "old bytes");
		EXPECT_EQ(storage.getFileInfo(destinationPath).size, std::string("old bytes").size());
		EXPECT_TRUE(std::filesystem::exists(destination));
		EXPECT_FALSE(std::filesystem::exists(destination.string() + ".hollycast-vfs-backup"));
		EXPECT_FALSE(std::filesystem::exists(destination.string() + ".hollycast-vfs-transaction"));
	}

	const auto unrelated = root / "lookalike.state.backup";
	writeBytes(unrelated.string(), "still usable");
	writeBytes(unrelated.string() + ".hollycast-vfs-transaction", "not a slot marker");
	EXPECT_TRUE(storage.exists(unrelated.string()));
	EXPECT_EQ(readStorageFile(storage, unrelated.string()), "still usable");
}

TEST(LibretroStorageTest, FrontendStatRecognizesConfiguredDirectoryForNewChildren)
{
	const auto root = makeRoot();
	initializeVfs(root);
	hostfs::AllStorage& storage = hostfs::storage();
	const std::string destination = (root / "fresh.state").string();

	EXPECT_TRUE(hostfs::customStorage().isKnownPath(root.string()));
	EXPECT_TRUE(hostfs::customStorage().isKnownPathForWrite(destination));
	std::string temporary;
	std::unique_ptr<hostfs::File> file(storage.createTemporaryFile(destination, temporary));
	ASSERT_NE(file, nullptr);
	EXPECT_FALSE(temporary.empty());
	EXPECT_TRUE(std::filesystem::exists(temporary));
	file.reset();
	EXPECT_TRUE(storage.removeFile(temporary));
}

TEST(LibretroStorageTest, PublicLookupRetriesRollbackAfterInitialFailure)
{
	const auto root = makeRoot();
	initializeVfs(root);
	const auto destination = root / "slot.state";
	const std::string destinationPath = destination.string();
	writeBytes(destination, "old bytes");
	const std::string temporary = stage(hostfs::storage(), destinationPath, "new bytes");
	ASSERT_FALSE(temporary.empty());
	{
		std::lock_guard<std::mutex> lock(callbacks.mutex);
		callbacks.target = destinationPath;
		callbacks.failPublish = true;
		callbacks.failRollback = true;
	}

	EXPECT_FALSE(hostfs::storage().replaceFile(temporary, destinationPath));
	EXPECT_FALSE(std::filesystem::exists(destination));
	EXPECT_TRUE(std::filesystem::exists(destinationPath + ".hollycast-vfs-backup"));
	EXPECT_TRUE(std::filesystem::exists(destinationPath + ".hollycast-vfs-transaction"));
	EXPECT_FALSE(hostfs::storage().exists(destinationPath));
	{
		std::lock_guard<std::mutex> lock(callbacks.mutex);
		callbacks.failRollback = false;
	}

	EXPECT_TRUE(hostfs::storage().exists(destinationPath));
	EXPECT_EQ(readStorageFile(hostfs::storage(), destinationPath), "old bytes");
}

TEST(LibretroStorageTest, FailedCommittedCleanupKeepsBothCompleteVersions)
{
	const auto root = makeRoot();
	initializeVfs(root);
	const auto destination = root / "slot.state";
	const std::string destinationPath = destination.string();
	writeBytes(destination, "old bytes");
	const std::string temporary = stage(hostfs::storage(), destinationPath, "new bytes");
	ASSERT_FALSE(temporary.empty());
	{
		std::lock_guard<std::mutex> lock(callbacks.mutex);
		callbacks.target = destinationPath;
		callbacks.failBackupCleanup = true;
	}

	ASSERT_TRUE(hostfs::storage().replaceFile(temporary, destinationPath));
	EXPECT_EQ(readStorageFile(hostfs::storage(), destinationPath), "new bytes");
	EXPECT_TRUE(std::filesystem::exists(destinationPath + ".hollycast-vfs-backup"));
	EXPECT_TRUE(std::filesystem::exists(destinationPath + ".hollycast-vfs-transaction"));
	{
		std::lock_guard<std::mutex> lock(callbacks.mutex);
		callbacks.failBackupCleanup = false;
	}
	EXPECT_TRUE(hostfs::storage().exists(destinationPath));
	EXPECT_FALSE(std::filesystem::exists(destinationPath + ".hollycast-vfs-backup"));
	EXPECT_EQ(readStorageFile(hostfs::storage(), destinationPath), "new bytes");
}

TEST(LibretroStorageTest, ReaderWaitsForStagedOverwriteTransaction)
{
	const auto root = makeRoot();
	initializeVfs(root);
	const auto destination = root / "slot.state";
	const std::string destinationPath = destination.string();
	writeBytes(destination, "old bytes");
	const std::string temporary = stage(hostfs::storage(), destinationPath, "new bytes");
	ASSERT_FALSE(temporary.empty());
	{
		std::lock_guard<std::mutex> lock(callbacks.mutex);
		callbacks.target = destinationPath;
		callbacks.blockBackupRename = true;
	}

	auto writer = std::async(std::launch::async, [&] {
		return hostfs::storage().replaceFile(temporary, destinationPath);
	});
	bool enteredBackupRename;
	{
		std::unique_lock<std::mutex> lock(callbacks.mutex);
		enteredBackupRename = callbacks.condition.wait_for(lock, std::chrono::seconds(5), [] {
			return callbacks.backupRenameEntered;
		});
		if (!enteredBackupRename)
		{
			callbacks.releaseBackupRename = true;
			callbacks.condition.notify_all();
		}
	}
	ASSERT_TRUE(enteredBackupRename);
	auto reader = std::async(std::launch::async, [&] {
		return hostfs::storage().exists(destinationPath);
	});
	EXPECT_EQ(reader.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
	{
		std::lock_guard<std::mutex> lock(callbacks.mutex);
		callbacks.releaseBackupRename = true;
		callbacks.condition.notify_all();
	}
	ASSERT_TRUE(writer.get());
	EXPECT_TRUE(reader.get());
	EXPECT_EQ(readStorageFile(hostfs::storage(), destinationPath), "new bytes");
}
