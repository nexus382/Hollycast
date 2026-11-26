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
#pragma once
#include "types.h"

#include <limits>
#include <vector>

// Keep File's 64-bit offsets intact; long is only 32 bits on Windows.
#if defined(_WIN32)
#define HOSTFS_FSEEK _fseeki64
#define HOSTFS_FTELL _ftelli64
#else
#define HOSTFS_FSEEK fseeko
#define HOSTFS_FTELL ftello
#endif

namespace hostfs
{

struct FileInfo
{
	FileInfo() = default;
	FileInfo(const std::string& name, const std::string& path, bool isDirectory,
			size_t size = 0, bool isWritable = false, u64 updateTime = 0)
		: name(name), path(path), isDirectory(isDirectory), size(size),
		  isWritable(isWritable), updateTime(updateTime) {
	}

	std::string name;
	std::string path;
	bool isDirectory = false;
	size_t size = 0;
	bool isWritable = false;
	u64 updateTime = 0;
};

class File
{
public:
	virtual ~File() {}

	virtual size_t read(void* buffer, size_t size, size_t count) = 0;
	virtual size_t write(const void* buffer, size_t size, size_t count) = 0;
	virtual s64 tell() = 0;
	virtual int seek(s64 offset, int whence) = 0;
	virtual char* gets(char* str, int count) = 0;
	virtual s64 size() = 0;
	virtual int eof() = 0;
	virtual int error() = 0;
	virtual bool flush() = 0;
	virtual bool close() = 0;
};

class StdFile : public File
{
protected:
	FILE* file;

public:
	StdFile(FILE* file)
		: file(file)
	{}

	~StdFile() override { close(); }

	size_t read(void* buffer, size_t size, size_t count) override
	{
		return std::fread(buffer, size, count, file);
	}

	size_t write(const void* buffer, size_t size, size_t count) override
	{
		return std::fwrite(buffer, size, count, file);
	}

	s64 tell() override
	{
		return HOSTFS_FTELL(file);
	}

	int seek(s64 offset, int whence) override
	{
#ifndef _WIN32
		if constexpr (sizeof(off_t) < sizeof(offset))
			if (offset < std::numeric_limits<off_t>::min() || offset > std::numeric_limits<off_t>::max())
				return -1;
#endif
		return HOSTFS_FSEEK(file, offset, whence);
	}

	char* gets(char* str, int count) override
	{
		return std::fgets(str, count, file);
	}

	s64 size() override
	{
		const s64 position = tell();
		if (position < 0 || seek(0, SEEK_END) != 0)
			return -1;
		const s64 fileSize = tell();
		if (seek(position, SEEK_SET) != 0)
			return -1;
		return fileSize;
	}

	int eof() override
	{
		return std::feof(file);
	}

	int error() override
	{
		return file == nullptr ? 0 : std::ferror(file);
	}

	bool flush() override { return file != nullptr && std::fflush(file) == 0; }
	bool close() override
	{
		if (file == nullptr)
			return true;
		FILE *closing = file;
		file = nullptr;
		return std::fclose(closing) == 0;
	}
};

#undef HOSTFS_FSEEK
#undef HOSTFS_FTELL

class StorageException : public FlycastException
{
public:
	StorageException(const std::string& reason) : FlycastException(reason) {}
};

class Storage
{
public:
	virtual bool isKnownPath(const std::string& path) = 0;
	virtual std::vector<FileInfo> listContent(const std::string& path) = 0;
	virtual File *openFile(const std::string& path, const std::string& mode) = 0;
	virtual std::string getParentPath(const std::string& path) = 0;
	virtual std::string getSubPath(const std::string& reference, const std::string& subpath) = 0;
	virtual FileInfo getFileInfo(const std::string& path) = 0;
	virtual bool exists(const std::string& path) = 0;
	virtual File *createTemporaryFile(const std::string& destination, std::string& temporaryPath) { return nullptr; }
	virtual bool replaceFile(const std::string& temporaryPath, const std::string& destination) { return false; }
	virtual bool removeFile(const std::string& path) { return false; }

	virtual ~Storage() = default;
};

class CustomStorage : public Storage
{
public:
	virtual bool isKnownPathForWrite(const std::string& path) { return isKnownPath(path); }
	virtual bool addStorage(bool isDirectory, bool writeAccess, const std::string& description,
			void (*callback)(bool cancelled, std::string selectedPath), const std::string& mimeType) = 0;
};

class AllStorage : public Storage
{
public:
	bool isKnownPath(const std::string& path) override { return true; }

	std::vector<FileInfo> listContent(const std::string& path) override;
	File *openFile(const std::string& path, const std::string& mode) override;
	std::string getParentPath(const std::string& path) override;
	std::string getSubPath(const std::string& reference, const std::string& subpath) override;
	FileInfo getFileInfo(const std::string& path) override;
	bool exists(const std::string& path) override;
	File *createTemporaryFile(const std::string& destination, std::string& temporaryPath) override;
	bool replaceFile(const std::string& temporaryPath, const std::string& destination) override;
	bool removeFile(const std::string& path) override;
	std::string getDefaultDirectory();
};

// Owns a sibling staging path. The writer takes ownership of the File via takeFile();
// publish is valid only after the writer has successfully finished and closed it.
class AtomicFile
{
public:
	explicit AtomicFile(Storage& storage, std::string destination);
	~AtomicFile();
	AtomicFile(const AtomicFile&) = delete;
	AtomicFile& operator=(const AtomicFile&) = delete;
	File *file() const { return stream; }
	File *takeFile();
	bool publish();
	const std::string& temporaryPath() const { return temporary; }

private:
	Storage& storage;
	std::string destination;
	std::string temporary;
	File *stream = nullptr;
	bool published = false;
};

AllStorage& storage();
bool addStorage(bool isDirectory, bool writeAccess, const std::string& description,
		void (*callback)(bool cancelled, std::string selectedPath), const std::string& mimeType = {});

// iterate depth-first over the files contained in a folder hierarchy
class DirectoryTree
{
public:
	class iterator
	{
	private:
		iterator(const FileInfo dir) {
			dirs.push_back(dir);
			advance();
		}
		iterator() { }

	public:
		const FileInfo *operator->() {
			if (dirs.empty() && currentDir.empty())
				throw std::runtime_error("null iterator");
			return &currentDir[index];
		}

		const FileInfo& operator*() const {
			if (dirs.empty() && currentDir.empty())
				throw std::runtime_error("null iterator");
			return currentDir[index];
		}

		// Prefix increment
		iterator& operator++() {
			advance();
			return *this;
		}

		// Basic (in)equality implementations, just intended to work when comparing with end() or this
		friend bool operator==(const iterator& a, const iterator& b) {
			return a.dirs.size() == b.dirs.size() && a.currentDir.size() == b.currentDir.size();
		}

		friend bool operator!=(const iterator& a, const iterator& b) {
			return a.dirs.size() != b.dirs.size() || a.currentDir.size() != b.currentDir.size();
		}

	private:
		void advance()
		{
			if (!currentDir.empty())
				index++;
			while (true)
			{
				if (index >= currentDir.size())
				{
					index = 0;
					currentDir.clear();
					while (!dirs.empty() && currentDir.empty())
					{
						FileInfo& dir = dirs.back();
						currentDir = storage().listContent(dir.path);
						dirs.pop_back();
					}
				}
				if (currentDir.empty())
					return;
				while (index < currentDir.size() && currentDir[index].isDirectory)
				{
					dirs.push_back(currentDir[index]);
					index++;
				}
				if (index < currentDir.size())
					break;
			}
		}

		std::vector<FileInfo> dirs;
		std::vector<FileInfo> currentDir;
		size_t index = 0;

		friend class DirectoryTree;
	};

	DirectoryTree(const std::string& root) : root(root) {
	}

	iterator begin()
	{
		FileInfo entry(root, root, true);
		return { entry };
	}
	iterator end()
	{
		return { };
	}

private:
	const std::string& root;
};

CustomStorage& customStorage();

}	// namespace hostfs
