/*
	Copyright 2023 flyinghead

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
#include "directory.h"
#include "stdclass.h"
#include "nowide/stackstring.hpp"
#include "i18n.h"
#include <atomic>
#include <vector>
#include <sys/stat.h>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#include <sys/acl.h>
#include <unistd.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <sys/xattr.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

// For macOS
std::string os_PrecomposedString(std::string string);

namespace hostfs
{

#if !defined(__ANDROID__) && !defined(LIBRETRO)
CustomStorage& customStorage()
{
	class NullStorage : public CustomStorage
	{
	public:
		bool isKnownPath(const std::string& path) override { return false; }
		std::vector<FileInfo> listContent(const std::string& path) override { return std::vector<FileInfo>(); }
		File *openFile(const std::string& path, const std::string& mode) override { die("Not implemented"); }
		std::string getParentPath(const std::string& path) override { die("Not implemented"); }
		std::string getSubPath(const std::string& reference, const std::string& relative) override { die("Not implemented"); }
		FileInfo getFileInfo(const std::string& path) override { die("Not implemented"); }
		bool exists(const std::string& path) override { die("Not implemented"); }
		bool addStorage(bool isDirectory, bool writeAccess, const std::string& description,
				void (*callback)(bool cancelled, std::string selectedPath), const std::string& mimeType) override {
			die("Not implemented");
		}
	};
	static NullStorage storage;

	return storage;
}
#endif

#ifdef _WIN32
static const std::string separators = "/\\";
static const std::string native_separator = "\\";
#else
static const std::string separators = "/";
static const std::string native_separator = "/";
#endif

namespace {

// A temporary save must have no broader access while it is being written, and
// must inherit the existing slot's access rules before it is published.
bool preserveNativeFileAccess(const std::string& destination, const std::string& temporaryPath, int temporaryFd)
{
#ifdef _WIN32
	(void)temporaryFd;
#ifdef TARGET_UWP
	struct stat existing;
	if (flycast::stat(destination.c_str(), &existing) != 0)
		return errno == ENOENT;
	// The UWP API surface cannot copy the native file DACL. Do not overwrite an
	// existing slot when its access rules cannot be retained.
	return false;
#else
	nowide::wstackstring sourceName, temporaryName;
	if (!sourceName.convert(destination.c_str()) || !temporaryName.convert(temporaryPath.c_str()))
		return false;
	DWORD attributes = GetFileAttributesW(sourceName.get());
	if (attributes == INVALID_FILE_ATTRIBUTES)
	{
		DWORD error = GetLastError();
		return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
	}
	if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
		return false;

	DWORD information = OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
	DWORD descriptorSize = 0;
	GetFileSecurityW(sourceName.get(), information, nullptr, 0, &descriptorSize);
	if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || descriptorSize == 0)
		return false;
	std::vector<u8> descriptor(descriptorSize);
	if (!GetFileSecurityW(sourceName.get(), information,
			reinterpret_cast<PSECURITY_DESCRIPTOR>(descriptor.data()), descriptorSize, &descriptorSize))
		return false;

	SECURITY_DESCRIPTOR_CONTROL control;
	DWORD revision;
	if (!GetSecurityDescriptorControl(reinterpret_cast<PSECURITY_DESCRIPTOR>(descriptor.data()), &control, &revision))
		return false;
	if (control & SE_DACL_PROTECTED)
		information |= PROTECTED_DACL_SECURITY_INFORMATION;
	else
		information |= UNPROTECTED_DACL_SECURITY_INFORMATION;
	return SetFileSecurityW(temporaryName.get(), information,
			reinterpret_cast<PSECURITY_DESCRIPTOR>(descriptor.data())) != 0;
#endif // TARGET_UWP
#else
	struct stat existing;
	if (lstat(destination.c_str(), &existing) != 0)
		return errno == ENOENT;
	if (!S_ISREG(existing.st_mode))
		return false;

	int flags = O_RDONLY;
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
	flags |= O_NOFOLLOW;
#endif
	const int sourceFd = open(destination.c_str(), flags);
	if (sourceFd < 0)
		return false;
	struct stat opened;
	bool success = fstat(sourceFd, &opened) == 0
		&& S_ISREG(opened.st_mode)
		&& opened.st_dev == existing.st_dev
		&& opened.st_ino == existing.st_ino;
	if (success && (opened.st_uid != getuid() || opened.st_gid != getgid()))
		success = fchown(temporaryFd, opened.st_uid, opened.st_gid) == 0;

#if defined(__APPLE__)
	if (success)
	{
		errno = 0;
		acl_t acl = acl_get_fd_np(sourceFd, ACL_TYPE_EXTENDED);
		if (acl != nullptr)
		{
			success = acl_set_fd_np(temporaryFd, acl, ACL_TYPE_EXTENDED) == 0;
			acl_free(acl);
		}
		else if (errno == ENOENT)
		{
			// The new file can inherit an ACL from its parent even though the old
			// slot had none. An empty extended ACL removes that inherited ACL.
			acl_t emptyAcl = acl_init(0);
			if (emptyAcl == nullptr)
				success = false;
			else
			{
				success = acl_set_fd_np(temporaryFd, emptyAcl, ACL_TYPE_EXTENDED) == 0;
				acl_free(emptyAcl);
			}
		}
		else if (errno != EOPNOTSUPP && errno != ENOTSUP)
			success = false;
	}
#elif defined(__linux__) && !defined(__ANDROID__)
	if (success)
	{
		ssize_t aclSize = fgetxattr(sourceFd, "system.posix_acl_access", nullptr, 0);
		if (aclSize >= 0)
		{
			std::vector<u8> acl(static_cast<size_t>(aclSize));
			ssize_t readSize = fgetxattr(sourceFd, "system.posix_acl_access", acl.data(), acl.size());
			success = readSize == aclSize
				&& fsetxattr(temporaryFd, "system.posix_acl_access", acl.data(), acl.size(), 0) == 0;
		}
		else if (errno == ENODATA)
		{
			// A directory default ACL can be inherited by the temporary file. Do
			// not let that ACL change access when the destination had no ACL.
			if (fremovexattr(temporaryFd, "system.posix_acl_access") != 0
				&& errno != ENODATA && errno != EOPNOTSUPP && errno != ENOTSUP)
				success = false;
		}
		else if (errno != EOPNOTSUPP && errno != ENOTSUP)
			success = false;
	}
#elif !defined(__ANDROID__)
	// There is no portable ACL query API for the remaining native targets. Fail
	// closed on overwrites rather than silently dropping ACLs we cannot inspect.
	success = false;
#endif
	if (success)
		success = fchmod(temporaryFd, existing.st_mode & 07777) == 0;
	close(sourceFd);
	return success;
#endif
}

} // namespace

class StdStorage : public Storage
{
public:
	bool isKnownPath(const std::string& path) override {
		// assume it's known since standard storage should be asked last
		return true;
	}

	std::vector<FileInfo> listContent(const std::string& path) override
	{
		if (path.empty())
			return listRoots();

		DIR *dir = flycast::opendir(path.c_str());
		if (dir == nullptr) {
			WARN_LOG(COMMON, "Cannot read directory '%s' errno 0x%x", path.c_str(), errno);
			throw StorageException(strprintf(i18n::T("Can't read directory %s"), path.c_str()));
		}
		std::vector<FileInfo> entries;
		while (true)
		{
			dirent *direntry = flycast::readdir(dir);
			if (direntry == nullptr)
				break;

			FileInfo entry;
			entry.name = direntry->d_name;
#ifdef __APPLE__
			entry.name = os_PrecomposedString(entry.name);
#endif
			if (entry.name == "." || entry.name == "..")
				continue;
			if (path.find_last_of(separators) != path.size() - 1)
				entry.path = path + native_separator + entry.name;
			else
				entry.path = path + entry.name;
#ifndef TARGET_UWP
			// Silently skip unreadable entries
			// On UWP with broadFileSystemAccess, we should have read access to everything we can enumerate
			if (flycast::access(entry.path.c_str(), R_OK) != 0)
				continue;
#endif

			bool isDir = false;
#ifdef TARGET_UWP
			// On UWP, use the d_type we set in our custom readdir implementation
			if (direntry->d_type == DT_DIR)
				isDir = true;
			// Skip hidden files - but we already check this in readdir
#elif defined(__HAIKU__)
			struct stat st;
			if (flycast::stat(entry.path.c_str(), &st) != 0)
			{
				WARN_LOG(COMMON, "Cannot stat file '%s' errno 0x%x", entry.path.c_str(), errno);
				continue;
			}
			if (S_ISDIR(st.st_mode))
				isDir = true;
#elif !defined(_WIN32)
			if (direntry->d_type == DT_DIR)
				isDir = true;
			else if (direntry->d_type == DT_UNKNOWN || direntry->d_type == DT_LNK)
			{
				struct stat st;
				if (flycast::stat(entry.path.c_str(), &st) != 0)
				{
					WARN_LOG(COMMON, "Cannot stat file '%s' errno 0x%x", entry.path.c_str(), errno);
					continue;
				}
				if (S_ISDIR(st.st_mode))
					isDir = true;
			}
#else // _WIN32 non-UWP
			nowide::wstackstring wname;
			if (wname.convert(entry.path.c_str()))
			{
				DWORD attr = GetFileAttributesW(wname.get());
				if (attr != INVALID_FILE_ATTRIBUTES)
				{
					if (attr & FILE_ATTRIBUTE_HIDDEN)
						continue;
					if (attr & FILE_ATTRIBUTE_DIRECTORY)
						isDir = true;
				}
			}
#endif
			entry.isDirectory = isDir;
			entries.emplace_back(entry);
		}
		flycast::closedir(dir);

		return entries;
	}

	File *openFile(const std::string& path, const std::string& mode) override
	{
		FILE *file = nowide::fopen(path.c_str(), mode.c_str());

		if (file == nullptr)
			return nullptr;

		return new StdFile(file);
	}

	File *createTemporaryFile(const std::string& destination, std::string& temporaryPath) override
	{
		static std::atomic<u64> sequence{0};
		for (int attempt = 0; attempt < 128; attempt++)
		{
			temporaryPath = destination + ".tmp-" + std::to_string(++sequence);
			FILE *file = nullptr;
#ifdef _WIN32
			file = nowide::fopen(temporaryPath.c_str(), "wbx");
#else
			int flags = O_CREAT | O_EXCL | O_RDWR;
#ifdef O_CLOEXEC
			flags |= O_CLOEXEC;
#endif
			int fd = open(temporaryPath.c_str(), flags, 0600);
			if (fd >= 0)
			{
				file = fdopen(fd, "wb");
				if (file == nullptr)
				{
					close(fd);
					flycast::unlink(temporaryPath.c_str());
				}
			}
#endif
			if (file != nullptr)
			{
				if (preserveNativeFileAccess(destination, temporaryPath,
#ifdef _WIN32
						_fileno(file)
#else
						fileno(file)
#endif
						))
					return new StdFile(file);
				fclose(file);
				flycast::unlink(temporaryPath.c_str());
				temporaryPath.clear();
				return nullptr;
			}
			if (errno != EEXIST)
				break;
		}
		temporaryPath.clear();
		return nullptr;
	}

	bool replaceFile(const std::string& temporaryPath, const std::string& destination) override
	{
		return flycast::rename(temporaryPath.c_str(), destination.c_str()) == 0;
	}

	bool removeFile(const std::string& path) override
	{
		return flycast::unlink(path.c_str()) == 0 || errno == ENOENT;
	}

	std::string getParentPath(const std::string& path) override
	{
		auto lastSlash = path.find_last_of(separators);
#ifdef _WIN32
		if (path.size() == 2 && std::isalpha(static_cast<u8>(path[0])) && path[1] == ':')
			return "";
		if (lastSlash == 2 && path.size() == 3 && std::isalpha(static_cast<u8>(path[0])) && path[1] == ':')
			return "";
#else
		if (lastSlash == 0)
			return "/";
#endif
		if (lastSlash == std::string::npos)
			return "." + native_separator;
		std::string parentPath = path.substr(0, lastSlash);
#ifdef _WIN32
		if (parentPath.size() == 2 && std::isalpha(static_cast<u8>(parentPath[0])) && parentPath[1] == ':')
			parentPath += '\\';
#endif
		if (flycast::access(parentPath.c_str(), R_OK) != 0)
			return "";
		else
			return parentPath;
	}

	std::string getSubPath(const std::string& reference, const std::string& subpath) override
	{
		// FIXME this is incorrect if reference isn't a directory
		return reference + native_separator + subpath;
	}

	FileInfo getFileInfo(const std::string& path) override
	{
		FileInfo info;
		info.path = path;
		size_t slash = get_last_slash_pos(path);
		if (slash != std::string::npos && slash < path.size() - 1)
			info.name = path.substr(slash + 1);
		else
			info.name = path;
		info.isWritable = flycast::access(path.c_str(), W_OK) == 0;
#ifndef _WIN32
		struct stat st;
		if (flycast::stat(path.c_str(), &st) != 0) {
			if (errno != ENOENT)
				INFO_LOG(COMMON, "Cannot stat file '%s' errno %d", path.c_str(), errno);
			throw StorageException(strprintf(i18n::T("Cannot stat %s"), path.c_str()));
		}
		info.isDirectory = S_ISDIR(st.st_mode);
		info.size = st.st_size;
		info.updateTime = st.st_mtime;
#else // _WIN32
		std::string lpath(path);
		if (path.length() == 2 && isalpha(path[0]) && path[1] == ':')
			/* D: -> \\.\D:\ */
			lpath = "\\\\.\\" + path + "\\";
		else if (path.substr(0, 4) == "\\\\.\\" && path.length() == 6)
			/* \\.\D: -> \\.\D:\ */
			lpath += "\\";
		nowide::wstackstring wname;
		if (wname.convert(lpath.c_str()))
		{
			if (lpath.substr(0, 4) == "\\\\.\\")
			{
				// Win32 device namespace
				UINT type = GetDriveTypeW(wname.get());
				if (type != DRIVE_CDROM)
					throw StorageException(strprintf(i18n::T("Invalid device %s"), lpath.substr(4, 2).c_str()));
				info.isDirectory = false;
				info.isWritable = false;
			}
			else
			{
				WIN32_FILE_ATTRIBUTE_DATA fileAttribs;
#ifdef TARGET_UWP
				if (GetFileAttributesExFromAppW(wname.get(), GetFileExInfoStandard, &fileAttribs))
#else
				if (GetFileAttributesExW(wname.get(), GetFileExInfoStandard, &fileAttribs))
#endif
				{
					info.isDirectory = (fileAttribs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
					info.size = fileAttribs.nFileSizeLow + ((u64)fileAttribs.nFileSizeHigh << 32);
					u64 t = ((u64)fileAttribs.ftLastWriteTime.dwHighDateTime << 32) | fileAttribs.ftLastWriteTime.dwLowDateTime;
					info.updateTime = t / 10000000 - 11644473600LL;	// 100-nano to secs minus (unix epoch - windows epoch)
				}
				else
				{
					const int error = GetLastError();
					if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
						INFO_LOG(COMMON, "Cannot get attributes of '%s' error 0x%x", lpath.c_str(), error);
					_set_errno(error);
					throw StorageException(strprintf(i18n::T("Cannot get attributes of %s"), lpath.c_str()));
				}
			}
		}
		else
		{
			_set_errno(EINVAL);
			throw StorageException(strprintf(i18n::T("Invalid file name %s"), path.c_str()));
		}
#endif
		return info;
	}

	bool exists(const std::string& path) override
	{
#ifndef _WIN32
		struct stat st;
		return flycast::stat(path.c_str(), &st) == 0;
#else // _WIN32
		nowide::wstackstring wname;
		if (wname.convert(path.c_str()))
		{
			WIN32_FILE_ATTRIBUTE_DATA fileAttribs;
#ifdef TARGET_UWP
			// Use UWP-specific API
			if (GetFileAttributesExFromAppW(wname.get(), GetFileExInfoStandard, &fileAttribs))
				return true;
#else
			if (GetFileAttributesExW(wname.get(), GetFileExInfoStandard, &fileAttribs))
				return true;
#endif
		}
		return false;
#endif
	}

private:
	std::vector<FileInfo> listRoots()
	{
#ifdef _WIN32
		std::vector<FileInfo> driveList;
		// List all the drives
		u32 drives = GetLogicalDrives();
		for (int i = 0; i < 32; i++)
			if ((drives & (1 << i)) != 0)
			{
				std::string name = std::string(1, (char)('A' + i)) + ":";
				driveList.emplace_back(name, name + "\\", true);
			}
#ifdef TARGET_UWP
		// Add the home directory to the list of drives as it's not accessible from the root
		std::string home;
		const char *home_drive = nowide::getenv("HOMEDRIVE");
		if (home_drive != nullptr)
			home = home_drive;
		home += nowide::getenv("HOMEPATH");
		driveList.emplace_back(home, home, true);
#endif
		return driveList;

#elif defined(__ANDROID__)
		std::vector<FileInfo> roots;
		const char *home = nowide::getenv("FLYCAST_HOME");
		while (home != nullptr)
		{
			const char *pcolon = strchr(home, ':');
			if (pcolon != nullptr)
			{
				roots.emplace_back(std::string(home, pcolon - home), std::string(home, pcolon - home), true);
				home = pcolon + 1;
			}
			else
			{
				roots.emplace_back(home, home, true);
				home = nullptr;
			}
		}

		return roots;

#else

		return listContent("/");
#endif
	}
};

static StdStorage stdStorage;

std::vector<FileInfo> AllStorage::listContent(const std::string& path)
{
	if (path.empty())
	{
		std::vector<FileInfo> entries = stdStorage.listContent(path);
		std::vector<FileInfo> customEntries = customStorage().listContent(path);
		entries.insert(entries.end(), customEntries.begin(), customEntries.end());

		return entries;
	}
	if (customStorage().isKnownPath(path))
		return customStorage().listContent(path);
	else
		return stdStorage.listContent(path);
}

File *AllStorage::openFile(const std::string& path, const std::string& mode)
{
	if (customStorage().isKnownPath(path))
		return customStorage().openFile(path, mode);
	else
		return stdStorage.openFile(path, mode);
}

std::string AllStorage::getParentPath(const std::string& path)
{
	if (customStorage().isKnownPath(path))
		return customStorage().getParentPath(path);
	else
		return stdStorage.getParentPath(path);
}

std::string AllStorage::getSubPath(const std::string& reference, const std::string& subpath)
{
	if (customStorage().isKnownPath(reference))
		return customStorage().getSubPath(reference, subpath);
	else
		return stdStorage.getSubPath(reference, subpath);
}

FileInfo AllStorage::getFileInfo(const std::string& path)
{
	if (customStorage().isKnownPath(path))
		return customStorage().getFileInfo(path);
	else
		return stdStorage.getFileInfo(path);
}

bool AllStorage::exists(const std::string& path)
{
	if (customStorage().isKnownPath(path))
		return customStorage().exists(path);
	else
		return stdStorage.exists(path);
}

File *AllStorage::createTemporaryFile(const std::string& destination, std::string& temporaryPath)
{
	if (customStorage().isKnownPathForWrite(destination))
		return customStorage().createTemporaryFile(destination, temporaryPath);
	return stdStorage.createTemporaryFile(destination, temporaryPath);
}

bool AllStorage::replaceFile(const std::string& temporaryPath, const std::string& destination)
{
	if (customStorage().isKnownPathForWrite(destination))
		return customStorage().replaceFile(temporaryPath, destination);
	return stdStorage.replaceFile(temporaryPath, destination);
}

bool AllStorage::removeFile(const std::string& path)
{
	if (customStorage().isKnownPathForWrite(path))
		return customStorage().removeFile(path);
	return stdStorage.removeFile(path);
}

AtomicFile::AtomicFile(Storage& storage, std::string destination)
	: storage(storage), destination(std::move(destination))
{
	stream = storage.createTemporaryFile(this->destination, temporary);
}

AtomicFile::~AtomicFile()
{
	delete stream;
	if (!published && !temporary.empty())
		storage.removeFile(temporary);
}

File *AtomicFile::takeFile()
{
	File *file = stream;
	stream = nullptr;
	return file;
}

bool AtomicFile::publish()
{
	if (published || stream != nullptr || temporary.empty())
		return false;
	if (!storage.replaceFile(temporary, destination))
		return false;
	published = true;
	temporary.clear();
	return true;
}

std::string AllStorage::getDefaultDirectory()
{
	std::string directory;
#if defined(__ANDROID__)
	const char *home = nowide::getenv("FLYCAST_HOME");
	if (home != NULL)
	{
		const char *pcolon = strchr(home, ':');
		if (pcolon != NULL)
			directory = std::string(home, pcolon - home);
		else
			directory = home;
	}
#elif defined(__unix__) || defined(__APPLE__)
	const char *home = nowide::getenv("HOME");
	if (home != NULL)
		directory = home;
#elif defined(_WIN32)
	if (nowide::getenv("HOMEPATH") != NULL)
	{
		const char *home_drive = nowide::getenv("HOMEDRIVE");
		if (home_drive != NULL)
			directory = home_drive;
		directory += nowide::getenv("HOMEPATH");
	}
#elif defined(__SWITCH__)
	directory = "/";
#endif
	if (directory.empty())
	{
		directory = get_writable_config_path("");
		if (directory.empty())
			directory = ".";
	}
	return directory;
}

AllStorage& storage()
{
	static AllStorage storage;

	return storage;
}

bool addStorage(bool isDirectory, bool writeAccess, const std::string& description,
		void (*callback)(bool cancelled, std::string selectedPath), const std::string& mimeType)
{
	return customStorage().addStorage(isDirectory, writeAccess, description, callback, mimeType);
}

}
