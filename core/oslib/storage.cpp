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
#ifdef TARGET_UWP
#include <aclapi.h>
#include <fileapifromapp.h>
#endif
#elif defined(__APPLE__)
#include <fcntl.h>
#include <sys/acl.h>
#include <unistd.h>
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
#include <sys/types.h>
#include <fcntl.h>
#include <sys/acl.h>
#include <unistd.h>
#elif defined(__linux__)
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

#ifdef _WIN32
bool windowsAclUnsupported(DWORD error)
{
	return error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED;
}
#endif

#if defined(TARGET_UWP)
constexpr char uwpRecoverySuffix[] = ".hollycast-recovery";
constexpr char uwpTemporaryMarker[] = ".tmp-";

bool getUwpFileExists(const std::string& path, bool& exists)
{
	nowide::wstackstring name;
	if (!name.convert(path.c_str()))
		return false;
	WIN32_FILE_ATTRIBUTE_DATA attributes;
	if (GetFileAttributesExFromAppW(name.get(), GetFileExInfoStandard, &attributes))
	{
		exists = true;
		return true;
	}
	DWORD error = GetLastError();
	if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
	{
		exists = false;
		return true;
	}
	return false;
}

bool uwpFileNamesEqual(const std::string& first, const std::string& second)
{
	nowide::wstackstring firstName, secondName;
	return firstName.convert(first.c_str()) && secondName.convert(second.c_str())
		&& CompareStringOrdinal(firstName.get(), -1, secondName.get(), -1, TRUE) == CSTR_EQUAL;
}

bool deleteUwpFile(const std::string& path)
{
	nowide::wstackstring name;
	if (!name.convert(path.c_str()))
		return false;
	if (DeleteFileFromAppW(name.get()))
		return true;
	DWORD error = GetLastError();
	return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool isUwpRecoveryFileName(const std::string& name)
{
	const size_t suffixLength = sizeof(uwpRecoverySuffix) - 1;
	if (name.size() <= suffixLength
		|| name.compare(name.size() - suffixLength, suffixLength, uwpRecoverySuffix) != 0)
		return false;
	const std::string stagedName = name.substr(0, name.size() - suffixLength);
	const size_t marker = stagedName.rfind(uwpTemporaryMarker);
	if (marker == std::string::npos || marker == 0
		|| marker + sizeof(uwpTemporaryMarker) - 1 == stagedName.size())
		return false;
	for (size_t i = marker + sizeof(uwpTemporaryMarker) - 1; i < stagedName.size(); ++i)
		if ((stagedName[i] < '0' || stagedName[i] > '9') && stagedName[i] != '-')
			return false;
	return true;
}

bool recoverUwpBackup(const std::string& destination, const std::string& backup, bool requireCleanup)
{
	bool backupExists;
	if (!getUwpFileExists(backup, backupExists))
		return false;
	if (!backupExists)
		return true;
	bool destinationExists;
	if (!getUwpFileExists(destination, destinationExists))
		return false;
	if (destinationExists)
	{
		// The replacement committed before shutdown. Remove its stale backup so
		// it cannot be mistaken for a later rollback copy.
		const bool removed = deleteUwpFile(backup);
		if (!removed && requireCleanup)
			return false;
		if (removed)
			deleteUwpFile(backup.substr(0, backup.size() - (sizeof(uwpRecoverySuffix) - 1)));
		return true;
	}

	nowide::wstackstring backupName, destinationName;
	if (!backupName.convert(backup.c_str()) || !destinationName.convert(destination.c_str()))
		return false;
	if (MoveFileFromAppW(backupName.get(), destinationName.get()))
	{
		deleteUwpFile(backup.substr(0, backup.size() - (sizeof(uwpRecoverySuffix) - 1)));
		return true;
	}
	DWORD error = GetLastError();
	if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
	{
		if (!getUwpFileExists(destination, destinationExists))
			return false;
		if (destinationExists)
			return !requireCleanup || deleteUwpFile(backup);
	}
	return false;
}

bool recoverUwpBackupsInDirectory(const std::string& directory, const std::string *targetName = nullptr,
		bool requireCleanup = true)
{
	DIR *dir = flycast::opendir(directory.c_str());
	if (dir == nullptr)
		return false;
	struct RecoveryRecord
	{
		std::string destination;
		std::string backup;
	};
	std::vector<RecoveryRecord> records;
	while (dirent *entry = flycast::readdir(dir))
	{
		const std::string name(entry->d_name);
		if (!isUwpRecoveryFileName(name))
			continue;
		const size_t suffixLength = sizeof(uwpRecoverySuffix) - 1;
		const std::string stagedName = name.substr(0, name.size() - suffixLength);
		const size_t marker = stagedName.rfind(uwpTemporaryMarker);
		const std::string destinationName = stagedName.substr(0, marker);
		if (targetName != nullptr && !uwpFileNamesEqual(destinationName, *targetName))
			continue;
		std::string destination = directory;
		if (destination.find_last_of(separators) != destination.size() - 1)
			destination += native_separator;
		destination += destinationName;
		const std::string backup = directory + (directory.find_last_of(separators) == directory.size() - 1
			? "" : native_separator) + name;
		records.push_back({std::move(destination), backup});
	}
	flycast::closedir(dir);
	bool success = true;
	for (const auto& record : records)
		if (!recoverUwpBackup(record.destination, record.backup, requireCleanup))
			success = false;
	return success;
}

bool recoverUwpBackupsForPath(const std::string& path, bool requireCleanup = true)
{
	const size_t separator = path.find_last_of(separators);
	std::string directory;
	if (separator == std::string::npos)
		directory = ".";
	else if (separator == 0)
		directory = native_separator;
#ifdef _WIN32
	else if (separator == 2 && path.size() > 2 && path[1] == ':')
		directory = path.substr(0, separator + 1);
#endif
	else
		directory = path.substr(0, separator);
	const std::string targetName = separator == std::string::npos
		? path : path.substr(separator + 1);
	return recoverUwpBackupsInDirectory(directory, &targetName, requireCleanup);
}
#endif

// A temporary save must have no broader access while it is being written, and
// must inherit the existing slot's access rules before it is published.
bool preserveNativeFileAccess(const std::string& destination, const std::string& temporaryPath, int temporaryFd)
{
#ifdef _WIN32
	(void)temporaryFd;
#ifdef TARGET_UWP
	(void)temporaryPath;
	nowide::wstackstring sourceName;
	if (!sourceName.convert(destination.c_str()))
		return false;
	WIN32_FILE_ATTRIBUTE_DATA attributes;
	if (!GetFileAttributesExFromAppW(sourceName.get(), GetFileExInfoStandard, &attributes))
	{
		DWORD error = GetLastError();
		return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
	}
	if (attributes.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
		return false;
	HANDLE source = CreateFileFromAppW(sourceName.get(), READ_CONTROL,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (source == INVALID_HANDLE_VALUE)
		return windowsAclUnsupported(GetLastError());
	HANDLE staged = reinterpret_cast<HANDLE>(_get_osfhandle(temporaryFd));
	if (staged == INVALID_HANDLE_VALUE)
	{
		CloseHandle(source);
		return false;
	}
	PACL dacl = nullptr;
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	DWORD result = GetSecurityInfo(source, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
		nullptr, nullptr, &dacl, nullptr, &descriptor);
	if (result == ERROR_SUCCESS)
	{
		SECURITY_DESCRIPTOR_CONTROL control;
		DWORD revision;
		if (!GetSecurityDescriptorControl(descriptor, &control, &revision))
			result = GetLastError();
		else
		{
			SECURITY_INFORMATION information = DACL_SECURITY_INFORMATION;
			information |= control & SE_DACL_PROTECTED
				? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
			result = SetSecurityInfo(staged, SE_FILE_OBJECT, information,
				nullptr, nullptr, dacl, nullptr);
		}
	}
	if (descriptor != nullptr)
		LocalFree(descriptor);
	CloseHandle(source);
	return result == ERROR_SUCCESS || windowsAclUnsupported(result);
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
	DWORD error = GetLastError();
	if (error != ERROR_INSUFFICIENT_BUFFER || descriptorSize == 0)
		return windowsAclUnsupported(error);
	std::vector<u8> descriptor(descriptorSize);
	if (!GetFileSecurityW(sourceName.get(), information,
			reinterpret_cast<PSECURITY_DESCRIPTOR>(descriptor.data()), descriptorSize, &descriptorSize))
		return windowsAclUnsupported(GetLastError());

	SECURITY_DESCRIPTOR_CONTROL control;
	DWORD revision;
	if (!GetSecurityDescriptorControl(reinterpret_cast<PSECURITY_DESCRIPTOR>(descriptor.data()), &control, &revision))
		return false;
	if (control & SE_DACL_PROTECTED)
		information |= PROTECTED_DACL_SECURITY_INFORMATION;
	else
		information |= UNPROTECTED_DACL_SECURITY_INFORMATION;
	if (SetFileSecurityW(temporaryName.get(), information,
			reinterpret_cast<PSECURITY_DESCRIPTOR>(descriptor.data())))
		return true;
	return windowsAclUnsupported(GetLastError());
#endif // TARGET_UWP
#else
#if defined(__SWITCH__) || defined(NINTENDO_SWITCH)
	// Switch save data is normally FAT-backed and has no POSIX owner, mode, or
	// ACL metadata to copy. Avoid ownership and ACL syscalls on this target.
	struct stat existing;
	if (stat(destination.c_str(), &existing) == 0)
		return true;
	return errno == ENOENT;
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
	struct stat staged;
	bool success = fstat(sourceFd, &opened) == 0
		&& fstat(temporaryFd, &staged) == 0
		&& S_ISREG(opened.st_mode)
		&& opened.st_dev == existing.st_dev
		&& opened.st_ino == existing.st_ino;
	if (success && (opened.st_uid != staged.st_uid || opened.st_gid != staged.st_gid))
		success = fchown(temporaryFd, opened.st_uid, opened.st_gid) == 0;
	if (success)
		success = fchmod(temporaryFd, existing.st_mode & 07777) == 0;

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
#elif defined(__linux__)
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
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
	if (success)
	{
		acl_t acl = nullptr;
		acl_type_t aclType = ACL_TYPE_ACCESS;
#if defined(__FreeBSD__) || (defined(__NetBSD__) && defined(__NetBSD_Version__) && __NetBSD_Version__ >= 1000000000)
		acl = acl_get_fd_np(sourceFd, ACL_TYPE_NFS4);
		aclType = ACL_TYPE_NFS4;
		if (acl == nullptr && (errno == EINVAL || errno == EOPNOTSUPP || errno == ENOTSUP))
		{
			aclType = ACL_TYPE_ACCESS;
			acl = acl_get_fd_np(sourceFd, aclType);
		}
#else
		acl = acl_get_fd(sourceFd);
#endif
		if (acl != nullptr)
		{
			int setResult;
#if defined(__DragonFly__) || (defined(__NetBSD__) && (!defined(__NetBSD_Version__) || __NetBSD_Version__ < 1000000000))
			setResult = acl_set_fd(temporaryFd, acl);
#else
			setResult = acl_set_fd_np(temporaryFd, acl, aclType);
#endif
			success = setResult == 0;
			acl_free(acl);
		}
		else if (errno != EOPNOTSUPP && errno != ENOTSUP && errno != EINVAL)
			success = false;
	}
#endif
	close(sourceFd);
	return success;
#endif // !Switch
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

#ifdef TARGET_UWP
		if (!recoverUwpBackupsInDirectory(path, nullptr, false))
			WARN_LOG(COMMON, "Cannot recover interrupted save replacement in '%s'", path.c_str());
#endif

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
#ifdef TARGET_UWP
			if (isUwpRecoveryFileName(entry.name))
				continue;
#endif
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
#ifdef TARGET_UWP
		if (!recoverUwpBackupsForPath(path, false))
			return nullptr;
#endif
		FILE *file = nowide::fopen(path.c_str(), mode.c_str());

		if (file == nullptr)
			return nullptr;

		return new StdFile(file);
	}

	File *createTemporaryFile(const std::string& destination, std::string& temporaryPath) override
	{
#ifdef TARGET_UWP
		if (!recoverUwpBackupsForPath(destination))
			return nullptr;
#endif
		static std::atomic<u64> sequence{0};
		for (int attempt = 0; attempt < 128; attempt++)
		{
#ifdef TARGET_UWP
			temporaryPath = destination + ".tmp-" + std::to_string(GetTickCount64())
				+ "-" + std::to_string(++sequence);
#else
			temporaryPath = destination + ".tmp-" + std::to_string(++sequence);
#endif
			FILE *file = nullptr;
#ifdef _WIN32
#ifdef TARGET_UWP
			bool backupExists;
			if (!getUwpFileExists(temporaryPath + uwpRecoverySuffix, backupExists))
				break;
			if (backupExists)
				continue;
#endif
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
#ifdef TARGET_UWP
		if (!recoverUwpBackupsForPath(destination))
			return false;
		bool destinationExists;
		if (!getUwpFileExists(destination, destinationExists))
			return false;
		nowide::wstackstring temporaryName, destinationName, backupName;
		const std::string backupPath = temporaryPath + uwpRecoverySuffix;
		if (!temporaryName.convert(temporaryPath.c_str()) || !destinationName.convert(destination.c_str())
			|| !backupName.convert(backupPath.c_str()))
			return false;
		bool backupExists;
		if (!getUwpFileExists(backupPath, backupExists) || backupExists)
			return false;
		if (!destinationExists)
			return MoveFileFromAppW(temporaryName.get(), destinationName.get()) != 0;
		if (ReplaceFileFromAppW(destinationName.get(), temporaryName.get(), backupName.get(), 0, nullptr, nullptr))
		{
			// The replacement is committed. A failed backup cleanup is harmless and
			// is retried by the next access or directory listing.
			deleteUwpFile(backupPath);
			return true;
		}
		const DWORD error = GetLastError();
		if (error == 1177)
		{
			if (!recoverUwpBackup(destination, backupPath))
				WARN_LOG(COMMON, "Save replacement rollback pending for '%s'", destination.c_str());
		}
		if (error == 1176)
			return false; // Both original paths remain in place when a backup is specified.
		// Other documented failures leave the original slot in place, or the
		// stage-specific backup is picked up by recoverUwpBackupsInDirectory.
		return false;
#else
		return flycast::rename(temporaryPath.c_str(), destination.c_str()) == 0;
#endif
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
#ifdef TARGET_UWP
		if (!recoverUwpBackupsForPath(path, false))
			throw StorageException(strprintf(i18n::T("Cannot stat %s"), path.c_str()));
#endif
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
#ifdef TARGET_UWP
		if (!recoverUwpBackupsForPath(path, false))
			return false;
#endif
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
