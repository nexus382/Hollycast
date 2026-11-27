/*
	Copyright 2022 flyinghead

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
#include "isofs.h"
#include "iso9660.h"

#include <cstddef>
#include <cstring>

static u32 decode_iso733(iso733_t v)
{
	return ((v >> 56) & 0x000000FF)
			| ((v >> 40) & 0x0000FF00)
			| ((v >> 24) & 0x00FF0000)
			| ((v >> 8) & 0xFF000000);
}

IsoFs::IsoFs(Disc *disc) : disc(disc)
{
	baseFad = disc->GetBaseFAD();
}

IsoFs::Directory *IsoFs::getRoot(bool *valid)
{
	if (valid != nullptr)
		*valid = false;
	u8 temp[2048]{};
	bool readError = false;
	const u32 descriptorCount = disc->ReadSectors(baseFad + 16, 1, temp, 2048,
			valid != nullptr, nullptr, valid != nullptr ? &readError : nullptr);
	// Primary Volume Descriptor
	const iso9660_pvd_t *pvd = (const iso9660_pvd_t *)temp;

	Directory *root = new Directory(this);
	if (valid != nullptr && (descriptorCount != 1 || readError))
		return root;
	if (pvd->type == 1 && !memcmp(pvd->id, ISO_STANDARD_ID, strlen(ISO_STANDARD_ID)) && pvd->version == 1)
	{
		u32 lba = decode_iso733(pvd->root_directory_record.extent);
		u32 len = decode_iso733(pvd->root_directory_record.size);
		const u64 rootStart = 150ULL + lba;
		const u64 sectorCount = (static_cast<u64>(len) + 2047) / 2048;
		const u64 roundedLength = sectorCount * 2048;
		if (len == 0 || rootStart + sectorCount > disc->LeadOut.StartFAD
				|| roundedLength > std::numeric_limits<u32>::max())
			return root;

		len = static_cast<u32>(roundedLength);
		root->data.resize(len);

		DEBUG_LOG(GDROM, "iso9660 root directory FAD: %d, len: %d", 150 + lba, len);
		const u32 readCount = disc->ReadSectors(150 + lba, len / 2048, root->data.data(), 2048,
				valid != nullptr, nullptr, valid != nullptr ? &readError : nullptr);
		if (valid != nullptr)
			*valid = readCount == sectorCount && !readError;
	}
	else {
		WARN_LOG(GDROM, "iso9660 PVD NOT found");
		root->data.resize(1);
		root->data[0] = 0;
	}
	return root;
}

void IsoFs::Directory::reset()
{
	index = 0;
	if (data.empty() && len != 0)
	{
		data.resize(len);
		fs->disc->ReadSectors(startFad, len / 2048, data.data(), 2048);
	}
}

IsoFs::Entry *IsoFs::Directory::nextEntry()
{
	while (index < data.size())
	{
		const size_t recordStart = index;
		const size_t recordLength = data[recordStart];
		const size_t sectorOffset = recordStart % 2048;
		if (recordLength == 0)
		{
			if (sectorOffset == 0)
				return nullptr;
			const u64 nextSector = static_cast<u64>(recordStart) + 2048 - sectorOffset;
			if (nextSector >= data.size())
				return nullptr;
			index = static_cast<u32>(nextSector);
			continue;
		}

		if (recordLength < sizeof(iso9660_dir_t) || recordLength > data.size() - recordStart
				|| recordLength > 2048 - sectorOffset)
		{
			index = static_cast<u32>(data.size());
			return nullptr;
		}
		const auto *dir = reinterpret_cast<const iso9660_dir_t *>(&data[recordStart]);
		constexpr size_t filenameOffset = offsetof(iso9660_dir_t, filename) + 1;
		const size_t filenameLength = dir->filename.len;
		if (recordLength < filenameOffset || filenameLength > recordLength - filenameOffset)
		{
			index = static_cast<u32>(data.size());
			return nullptr;
		}
		std::string name(dir->filename.str + 1, filenameLength);

		const u64 startFad64 = static_cast<u64>(decode_iso733(dir->extent)) + 150;
		const u32 originalLength = decode_iso733(dir->size);
		const u64 sectorCount = (static_cast<u64>(originalLength) + 2047) / 2048;
		if (startFad64 > fs->disc->LeadOut.StartFAD
				|| sectorCount > static_cast<u64>(fs->disc->LeadOut.StartFAD) - startFad64)
		{
			index = static_cast<u32>(data.size());
			return nullptr;
		}

		u32 len = originalLength;
		if ((dir->file_flags & ISO_DIRECTORY) != 0)
		{
			const u64 roundedLength = sectorCount * 2048;
			if (roundedLength > std::numeric_limits<u32>::max())
			{
				index = static_cast<u32>(data.size());
				return nullptr;
			}
			len = static_cast<u32>(roundedLength);
		}

		Entry *entry;
		if ((dir->file_flags & ISO_DIRECTORY) == 0)
		{
			File *file = new File(fs);
			entry = file;
		}
		else
		{
			Directory *directory = new Directory(fs);
			entry = directory;
		}
		entry->startFad = static_cast<u32>(startFad64);
		entry->len = len;
		entry->name = std::move(name);
		index = static_cast<u32>(recordStart + recordLength);
		return entry;
	}
	return nullptr;
}

IsoFs::Entry *IsoFs::Directory::getEntry(const std::string& name)
{
	std::string isoname = name + ';';
	reset();
	while (true)
	{
		Entry *entry = nextEntry();
		if (entry == nullptr)
			return nullptr;
		if (entry->getName().substr(0, isoname.size()) == isoname)
			return entry;
		delete entry;
	}
}

u32 IsoFs::File::read(u8 *buf, u32 size, u32 offset) const
{
	if (offset >= len)
		return 0;
	size = std::min(size, len - offset);
	u32 sectors = size / 2048;
	fs->disc->ReadSectors(startFad + offset / 2048, sectors, buf, 2048);
	size -= sectors * 2048;
	if (size > 0)
	{
		u8 temp[2048];
		fs->disc->ReadSectors(startFad + offset / 2048 + sectors, 1, temp, 2048);
		memcpy(buf + sectors * 2048, temp, size);
	}
	return sectors * 2048 + size;
}

std::vector<IsoFs::Entry*> IsoFs::Directory::list()
{
	std::vector<IsoFs::Entry*> v;
	reset();
	while (true)
	{
		Entry *entry = nextEntry();
		if (entry == nullptr)
			break;
		v.push_back(entry);
	}
	return v;
}
