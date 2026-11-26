/*
    This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "storage.h"

#include "oslib/directory.h"
#include <file/file_path.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>

struct retro_vfs_interface* hostfs::LibretroStorage::vfs_interface = nullptr;

namespace {
std::mutex vfsReplacementMutex;

static bool isTransactionalReplacementPath(const std::string& path)
{
	auto endsWith = [&path](const char *suffix) {
		const size_t length = std::strlen(suffix);
		return path.size() >= length && path.compare(path.size() - length, length, suffix) == 0;
	};
	return endsWith(".state") || endsWith(".state.net") || endsWith(".state.tmp")
			|| endsWith(".cfg.save");
}
}

bool hostfs::LibretroStorage::isFileWriteable(const std::string& path)
{
	// Not ideal; would be nicer if the VFS API gave us a direct way of checking this.
	RFILE* file = filestream_open(path.c_str(), RETRO_VFS_FILE_ACCESS_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING, RETRO_VFS_FILE_ACCESS_HINT_NONE);

	if (file == nullptr)
		return false;

	filestream_close(file);
	return true;
}

hostfs::FileInfo hostfs::LibretroStorage::makeFileInfo(std::string name, std::string path)
{
	const bool is_directory = path_is_directory(path.c_str());
	const size_t size = path_get_size(path.c_str());
	const bool is_writeable = isFileWriteable(path);

	return {std::move(name), std::move(path), is_directory, size, is_writeable};
}

void hostfs::LibretroStorage::initialise(retro_environment_t environ_cb)
{
	retro_vfs_interface_info vfs_interface_info;
	vfs_interface_info.required_interface_version = 3; // We really want those directory functions.

	if (!environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_interface_info))
		return;

	vfs_interface = vfs_interface_info.iface;

	filestream_vfs_init(&vfs_interface_info);
	path_vfs_init(&vfs_interface_info);
}

bool hostfs::LibretroStorage::isKnownPath(const std::string& path)
{
	std::lock_guard<std::mutex> lock(vfsReplacementMutex);
	if (isTransactionalReplacementPath(path))
	{
		recoverInterruptedReplacementUnlocked(path);
		if (rawPathExists(path))
			return true;
		return rawPathExists(path + ".hollycast-vfs-transaction");
	}
	return rawPathExists(path);
}

bool hostfs::LibretroStorage::isKnownPathForWrite(const std::string& path)
{
	std::lock_guard<std::mutex> lock(vfsReplacementMutex);
	if (isTransactionalReplacementPath(path))
		recoverInterruptedReplacementUnlocked(path);
	if (rawPathExists(path))
		return true;
	std::string parent(path);
	parent.resize(path_parent_dir(parent.data(), parent.size()));
	return !parent.empty() && rawPathExists(parent);
}

bool hostfs::LibretroStorage::rawPathExists(const std::string& path)
{
	if (vfs_interface != nullptr && vfs_interface->stat != nullptr
			&& (vfs_interface->stat(path.c_str(), nullptr) & RETRO_VFS_STAT_IS_VALID) != 0)
		return true;
	// Keep libretro-common's regular-file and native fallback behavior for paths
	// that the frontend stat callback does not recognize.
	return filestream_exists(path.c_str());
}

std::vector<hostfs::FileInfo> hostfs::LibretroStorage::listContent(const std::string& path)
{
	std::lock_guard<std::mutex> lock(vfsReplacementMutex);
	std::vector<FileInfo> list;

	if (vfs_interface == nullptr || vfs_interface->opendir == nullptr
			|| vfs_interface->readdir == nullptr || vfs_interface->dirent_get_name == nullptr
			|| vfs_interface->closedir == nullptr)
		return list;
	struct retro_vfs_dir_handle* dir = vfs_interface->opendir(path.c_str(), true);

	if (dir == nullptr)
		return list;

	while (vfs_interface->readdir(dir))
	{
		const auto entry_name = vfs_interface->dirent_get_name(dir);
		if (entry_name == nullptr)
			continue;
		const auto entry_path = path + PATH_DEFAULT_SLASH_C() + entry_name;

		list.emplace_back(makeFileInfo(entry_name, entry_path));
	}

	vfs_interface->closedir(dir);

	return list;
}

hostfs::File *hostfs::LibretroStorage::openFile(const std::string& path, const std::string& mode)
{
	std::lock_guard<std::mutex> lock(vfsReplacementMutex);
	if (isTransactionalReplacementPath(path) && !recoverInterruptedReplacementUnlocked(path))
		return nullptr;
	return openFileUnlocked(path, mode);
}

hostfs::File *hostfs::LibretroStorage::openFileUnlocked(const std::string& path, const std::string& mode)
{
	bool extended = false;
	bool append = false;
	unsigned int mode_bitfield = 0;

	for (char character : mode)
	{
		switch (character)
		{
			case 'R':
			case 'r':
				if (mode_bitfield != 0)
					return nullptr;

				mode_bitfield = RETRO_VFS_FILE_ACCESS_READ;
				break;

			case 'W':
			case 'w':
				if (mode_bitfield != 0)
					return nullptr;

				mode_bitfield = RETRO_VFS_FILE_ACCESS_WRITE;
				break;

			case 'A':
			case 'a':
				if (mode_bitfield != 0)
					return nullptr;

				mode_bitfield = RETRO_VFS_FILE_ACCESS_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
				append = true;
				break;

			case '+':
				extended = true;
				break;

			case 'B':
			case 'b':
			case 'T':
			case 't':
				// Binary mode unfortunately just gets assumed by default...
				// TODO: Do any of Flycast's text parsers require text mode?
				break;

			default:
				// Unrecognised character.
				return nullptr;
		}
	}

	if (mode_bitfield == 0)
		return nullptr;

	if (extended)
		mode_bitfield = RETRO_VFS_FILE_ACCESS_READ_WRITE;

	RFILE *rfile = filestream_open(path.c_str(), mode_bitfield, RETRO_VFS_FILE_ACCESS_HINT_NONE);

	if (rfile == nullptr)
		return nullptr;

	auto file = new LibretroFile(rfile);

	if (append)
		file->seek(0, SEEK_END);

	return file;
}

std::string hostfs::LibretroStorage::getParentPath(const std::string& path)
{
	std::string parent_path = path;

	parent_path.resize(path_parent_dir(parent_path.data(), parent_path.size()));

	return parent_path;
}

std::string hostfs::LibretroStorage::getSubPath(const std::string& reference, const std::string& relative)
{
	return reference + PATH_DEFAULT_SLASH_C() + relative;
}

hostfs::FileInfo hostfs::LibretroStorage::getFileInfo(const std::string& path)
{
	std::lock_guard<std::mutex> lock(vfsReplacementMutex);
	if (isTransactionalReplacementPath(path) && !recoverInterruptedReplacementUnlocked(path))
		return {};
	return makeFileInfo(path_basename(path.c_str()), path);
}

bool hostfs::LibretroStorage::exists(const std::string& path)
{
	std::lock_guard<std::mutex> lock(vfsReplacementMutex);
	if (isTransactionalReplacementPath(path) && !recoverInterruptedReplacementUnlocked(path))
		return false;
	return rawPathExists(path);
}

hostfs::File *hostfs::LibretroStorage::createTemporaryFile(const std::string& destination, std::string& temporaryPath)
{
	std::lock_guard<std::mutex> lock(vfsReplacementMutex);
	if (!recoverInterruptedReplacementUnlocked(destination))
	{
		temporaryPath.clear();
		return nullptr;
	}

	static std::atomic<u64> sequence{0};
	for (int attempt = 0; attempt < 128; attempt++)
	{
		temporaryPath = destination + ".tmp-" + std::to_string(++sequence);
		if (rawPathExists(temporaryPath))
			continue;
		if (File *file = openFileUnlocked(temporaryPath, "wb"))
			return file;
	}
	temporaryPath.clear();
	return nullptr;
}

bool hostfs::LibretroStorage::replaceFile(const std::string& temporaryPath, const std::string& destination)
{
	std::lock_guard<std::mutex> lock(vfsReplacementMutex);
	if (!recoverInterruptedReplacementUnlocked(destination))
		return false;

	if (vfs_interface != nullptr)
	{
		if (vfs_interface->rename == nullptr)
			return false;
		if (vfs_interface->rename(temporaryPath.c_str(), destination.c_str()) == 0)
			return true;

		// Some VFS implementations (including libretro-common on Windows) do
		// not replace an existing destination. Move the old file aside, publish
		// into the now-empty slot, and restore the old file if publication fails.
		if (!rawPathExists(destination) || vfs_interface->remove == nullptr)
			return false;
		const std::string backup = destination + ".hollycast-vfs-backup";
		const std::string marker = destination + ".hollycast-vfs-transaction";
		if (rawPathExists(backup) || rawPathExists(marker) || !writeVfsTransactionMarker(destination))
			return false;
		if (vfs_interface->rename(destination.c_str(), backup.c_str()) != 0)
		{
			vfs_interface->remove(marker.c_str());
			return false;
		}
		if (vfs_interface->rename(temporaryPath.c_str(), destination.c_str()) == 0)
		{
			// Recovery sees a live destination and completes committed cleanup.
			recoverInterruptedReplacementUnlocked(destination);
			return true;
		}

		// If rollback itself fails, the marker and backup remain; the next
		// open restores the original before exposing the slot.
		recoverInterruptedReplacementUnlocked(destination);
		return false;
	}
	// libretro-common falls back to host stdio when no VFS interface was
	// supplied, so publish through the same native replacement wrapper.
	return flycast::rename(temporaryPath.c_str(), destination.c_str()) == 0;
}

bool hostfs::LibretroStorage::recoverInterruptedReplacementUnlocked(const std::string& destination)
{
	if (vfs_interface == nullptr)
		return true;
	const std::string marker = destination + ".hollycast-vfs-transaction";
	if (!rawPathExists(marker))
		return true;
	if (!validVfsTransactionMarker(destination))
		return false;

	const std::string backup = destination + ".hollycast-vfs-backup";
	const bool hasBackup = rawPathExists(backup);
	const bool hasDestination = rawPathExists(destination);
	if (hasBackup && !hasDestination)
	{
		if (vfs_interface->rename == nullptr
				|| vfs_interface->rename(backup.c_str(), destination.c_str()) != 0)
			return false;
	}
	else if (!hasDestination)
		return false;

	// A destination alongside the backup means publication completed. A
	// destination without a backup means rollback (or preparation) completed.
	if (hasBackup && vfs_interface->remove != nullptr
			&& vfs_interface->remove(backup.c_str()) != 0 && rawPathExists(backup))
		return true;
	if (vfs_interface->remove != nullptr)
		vfs_interface->remove(marker.c_str());
	return true;
}

bool hostfs::LibretroStorage::writeVfsTransactionMarker(const std::string& destination)
{
	static std::atomic<u64> sequence{0};
	const std::string marker = destination + ".hollycast-vfs-transaction";
	std::string temporary;
	for (int attempt = 0; attempt < 128; attempt++)
	{
		temporary = marker + ".tmp-" + std::to_string(++sequence);
		if (!rawPathExists(temporary))
			break;
		temporary.clear();
	}
	if (temporary.empty())
		return false;

	static constexpr char contents[] = "HCV1\n";
	File *file = openFileUnlocked(temporary, "wb");
	if (file == nullptr)
		return false;
	bool written = file->write(contents, 1, sizeof(contents) - 1) == sizeof(contents) - 1;
	if (!file->flush())
		written = false;
	if (!file->close())
		written = false;
	delete file;
	if (!written)
	{
		if (vfs_interface != nullptr && vfs_interface->remove != nullptr)
			vfs_interface->remove(temporary.c_str());
		return false;
	}

	if (vfs_interface == nullptr || vfs_interface->rename == nullptr
			|| vfs_interface->rename(temporary.c_str(), marker.c_str()) != 0)
	{
		if (vfs_interface != nullptr && vfs_interface->remove != nullptr)
			vfs_interface->remove(temporary.c_str());
		return false;
	}
	if (validVfsTransactionMarker(destination))
		return true;
	if (vfs_interface != nullptr && vfs_interface->remove != nullptr)
		vfs_interface->remove(marker.c_str());
	return false;
}

bool hostfs::LibretroStorage::validVfsTransactionMarker(const std::string& destination)
{
	const std::string marker = destination + ".hollycast-vfs-transaction";
	File *file = openFileUnlocked(marker, "rb");
	if (file == nullptr)
		return false;
	static constexpr char expected[] = "HCV1\n";
	char actual[sizeof(expected) - 1];
	const bool valid = file->size() == sizeof(actual)
			&& file->read(actual, 1, sizeof(actual)) == sizeof(actual)
			&& std::memcmp(actual, expected, sizeof(actual)) == 0;
	const bool closed = file->close();
	delete file;
	return valid && closed;
}

bool hostfs::LibretroStorage::removeFile(const std::string& path)
{
	std::lock_guard<std::mutex> lock(vfsReplacementMutex);
	if (vfs_interface != nullptr)
		return vfs_interface->remove != nullptr
			&& (vfs_interface->remove(path.c_str()) == 0 || !rawPathExists(path));
	return flycast::unlink(path.c_str()) == 0 || errno == ENOENT;
}

bool hostfs::LibretroStorage::addStorage(bool isDirectory, bool writeAccess, const std::string& description,
		void (*callback)(bool cancelled, std::string selectedPath), const std::string& mimeType)
{
	// TODO: Should this do anything?
	return false;
}

hostfs::CustomStorage& hostfs::customStorage()
{
	static LibretroStorage storage;

	return storage;
}
