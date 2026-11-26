/*
	Copyright 2020 flyinghead

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
#include "rzip.h"
#include <zlib.h>

#include <cstring>
#include <limits>

const u8 RZipHeader[8] = { '#', 'R', 'Z', 'I', 'P', 'v', 1, '#' };

bool RZipFile::Open(hostfs::File *file, bool write)
{
	verify(this->file == nullptr);
	verify(file != nullptr);
	writeFailed = false;
	completionSucceeded = true;
	startOffset = file->tell();
	if (!write)
	{
		u8 header[sizeof(RZipHeader)];
		if (file->read(header, sizeof(header), 1) != 1
			|| memcmp(header, RZipHeader, sizeof(header))
			|| file->read(&maxChunkSize, sizeof(maxChunkSize), 1) != 1
			|| file->read(&size, sizeof(size), 1) != 1)
		{
			file->seek(startOffset, SEEK_SET);
			return false;
		}
		// savestates created on 32-bit platforms used to have a 32-bit size
		if (size >> 32 != 0)
		{
			size &= 0xffffffff;
			file->seek(-4, SEEK_CUR);
		}
		chunk = new u8[maxChunkSize];
		chunkIndex = 0;
		chunkSize = 0;
	}
	else
	{
		constexpr s64 headerPrefixSize = sizeof(RZipHeader) + sizeof(maxChunkSize);
		if (startOffset < 0 || startOffset > std::numeric_limits<s64>::max() - headerPrefixSize)
			return completionSucceeded = false;
		size = 0;
		maxChunkSize = 1_MB;
		if (file->write(RZipHeader, sizeof(RZipHeader), 1) != 1
			|| file->write(&maxChunkSize, sizeof(maxChunkSize), 1) != 1
			|| file->write(&size, sizeof(size), 1) != 1)
		{
			file->seek(startOffset, SEEK_SET);
			return completionSucceeded = false;
		}
	}
	this->write = write;
	this->file = file;
	return true;
}

bool RZipFile::Open(const std::string& path, bool write)
{
	hostfs::File *f = hostfs::storage().openFile(path.c_str(), write ? "wb" : "rb");
	if (f == nullptr)
		return false;
	if (!Open(f, write)) {
		delete f;
		return false;
	}
	return true;
}

void RZipFile::Close()
{
	if (write)
		Finish();
	else if (file != nullptr)
	{
		delete file;
		file = nullptr;
	}
	delete [] chunk;
	chunk = nullptr;
}

bool RZipFile::Finish()
{
	if (file == nullptr)
		return completionSucceeded;
	if (!write)
	{
		delete file;
		file = nullptr;
		return completionSucceeded = true;
	}

	hostfs::File *closing = file;
	file = nullptr;
	bool success = !writeFailed;
	const s64 lengthOffset = startOffset + sizeof(RZipHeader) + sizeof(maxChunkSize);
	if (closing->seek(lengthOffset, SEEK_SET) != 0
		|| closing->write(&size, sizeof(size), 1) != 1)
		success = false;
	if (!closing->flush())
		success = false;
	if (!closing->close())
		success = false;
	delete closing;
	write = false;
	completionSucceeded = success;
	return success;
}

size_t RZipFile::Read(void *data, size_t length)
{
	verify(file != nullptr);
	verify(!write);

	u8 *p = (u8 *)data;
	size_t rv = 0;
	while (rv < length)
	{
		if (chunkIndex == chunkSize)
		{
			chunkSize = 0;
			chunkIndex = 0;
			u32 zippedSize;
			if (file->read(&zippedSize, sizeof(zippedSize), 1) != 1)
				break;
			if (zippedSize == 0)
				continue;
			u8 *zipped = new u8[zippedSize];
			if (file->read(zipped, zippedSize, 1) != 1)
			{
				delete [] zipped;
				break;
			}
			uLongf tl = maxChunkSize;
			if (uncompress(chunk, &tl, zipped, zippedSize) != Z_OK)
			{
				delete [] zipped;
				break;
			}
			delete [] zipped;
			chunkSize = (u32)tl;
		}
		u32 l = std::min(chunkSize - chunkIndex, (u32)(length - rv));
		memcpy(p, chunk + chunkIndex, l);
		p += l;
		chunkIndex += l;
		rv += l;
	}

	return rv;
}

size_t RZipFile::Write(const void *data, size_t length)
{
	verify(file != nullptr);
	verify(write);

	if (length == 0)
		return 0;
	const u8 *p = (const u8 *)data;
	// compression output buffer must be 0.1% larger + 12 bytes
	uLongf maxZippedSize = maxChunkSize + maxChunkSize / 1000 + 12;
	u8 *zipped = new u8[maxZippedSize];
	size_t rv = 0;
	while (rv < length)
	{
		uLongf zippedSize = maxZippedSize;
		uLongf uncompressedSize = std::min(maxChunkSize, (u32)(length - rv));
		u32 rc = compress(zipped, &zippedSize, p, uncompressedSize);
		if (rc != Z_OK)
		{
			WARN_LOG(SAVESTATE, "Compression error: %d", rc);
			writeFailed = true;
			break;
		}
		u32 sz = (u32)zippedSize;
		if (file->write(&sz, sizeof(sz), 1) != 1
			|| file->write(zipped, zippedSize, 1) != 1)
		{
			writeFailed = true;
			break;
		}
		p += uncompressedSize;
		rv += uncompressedSize;
		size += uncompressedSize;
	}
	delete [] zipped;
	
	if (rv != length)
		writeFailed = true;
	return rv;
}
