/*
	This file is part of reicast.
	Portions Copyright 2026 The Hollycast Authors

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.
*/
// Naomi comm board emulation from mame
// https://github.com/mamedev/mame/blob/master/src/mame/machine/m3comm.cpp
// license:BSD-3-Clause
// copyright-holders:MetalliC

#include <memory>
#include "naomi_cart.h"
#include "naomi_regs.h"
#include "naomi.h"
#include "decrypt.h"
#include "naomi_roms.h"
#include "hw/flashrom/nvmem.h"
#include "m1cartridge.h"
#include "m4cartridge.h"
#include "awcartridge.h"
#include "gdcartridge.h"
#include "archive/archive.h"
#include "stdclass.h"
#include "emulator.h"
#include "cfg/option.h"
#include "oslib/oslib.h"
#include "serialize.h"
#include "card_reader.h"
#include "naomi_flashrom.h"
#include "touchscreen.h"
#include "printer.h"
#include "oslib/storage.h"
#include "network/alienfnt_modem.h"
#include "netdimm.h"
#include "systemsp.h"
#include "hopper.h"
#include "midiffb.h"
#include "oslib/i18n.h"

using namespace i18n;

Cartridge *CurrentCartridge;
bool bios_loaded = false;

InputDescriptors *NaomiGameInputs;
u8 *naomi_default_eeprom;

bool atomiswaveForceFeedback;

static std::unique_ptr<u8[]> ownedDefaultEeprom;

static void publishDefaultEeprom(const std::vector<u8>& data)
{
	std::unique_ptr<u8[]> replacement;
	if (!data.empty())
	{
		replacement = std::make_unique<u8[]>(data.size());
		memcpy(replacement.get(), data.data(), data.size());
	}
	ownedDefaultEeprom = std::move(replacement);
	naomi_default_eeprom = ownedDefaultEeprom.get();
}

void naomi_cart::readExactArchiveFile(ArchiveFile& file, void *buffer, u32 expectedLength, const char *component)
{
	u32 read = file.Read(buffer, expectedLength);
	if (read != expectedLength)
		throw NaomiCartException(strprintf(T("Invalid ROM: incomplete read of %s (%u of %u bytes)"),
				component, read, expectedLength));
}

void naomi_cart::loadGameBlob(ArchiveFile& file, u32 type, Cartridge& cartridge, u32 offset, u32 length,
		const char *component, MD5Sum& md5, bool hash, std::vector<u8>& defaultEeprom, std::vector<u8>& serial)
{
	std::vector<u8> data(length);
	readExactArchiveFile(file, data.data(), length, component);

	switch (type)
	{
	case Normal:
		{
			u32 mappedLength = length;
			u8 *destination = (u8 *)cartridge.GetPtr(offset, mappedLength);
			if (destination == nullptr)
				throw NaomiCartException(strprintf(T("Invalid ROM: truncated %s"), component));
			memcpy(destination, data.data(), length);
			if (hash)
				md5.add(destination, length);
		}
		break;

	case InterleavedWord:
		{
			if (length == 0)
				break;
			// The final word uses its own two bytes at the end of the lane.
			const u64 span = (u64)length * 2 - sizeof(u16);
			if (length % sizeof(u16) != 0 || span > cartridge.getSize())
				throw NaomiCartException(strprintf(T("Invalid ROM: truncated %s"), component));
			u32 mappedLength = (u32)span;
			u8 *destination = (u8 *)cartridge.GetPtr(offset, mappedLength);
			if (destination == nullptr)
				throw NaomiCartException(strprintf(T("Invalid ROM: truncated %s"), component));
			for (u32 i = 0; i < length; i += sizeof(u16))
				memcpy(destination + (size_t)i * 2, data.data() + i, sizeof(u16));
			if (hash)
				md5.add(destination, length);
		}
		break;

	case Key:
		{
			u8 *keyData = (u8 *)malloc(length);
			if (keyData == nullptr)
				throw NaomiCartException(Ts("Memory allocation failed"));
			memcpy(keyData, data.data(), length);
			cartridge.SetKeyData(keyData);
			if (hash)
				md5.add(data.data(), length);
		}
		break;

	case Eeprom:
		if (length == 0x84)
		{
			if (hash)
				md5.add(data.data(), data.size());
			serial = std::move(data);
		}
		else
		{
			defaultEeprom = std::move(data);
			if (hash)
				md5.add(defaultEeprom.data(), defaultEeprom.size());
		}
		break;

	default:
		throw NaomiCartException(Ts("Unsupported game ROM blob type"));
	}
}

void naomi_cart::loadGameBlobs(const Game& game, Archive *archive, Archive *parentArchive, Cartridge& cartridge,
		MD5Sum& md5, bool hash, std::vector<u8>& defaultEeprom, std::vector<u8>& serial, LoadProgress *progress)
{
	MD5Sum pendingMd5 = md5;
	std::vector<u8> pendingEeprom = defaultEeprom;
	std::vector<u8> pendingSerial = serial;
	int romCount = 0;
	while (game.blobs[romCount].filename != nullptr)
		romCount++;
	for (int romid = 0; romid < romCount; romid++)
	{
		if (progress != nullptr)
		{
			if (progress->cancelled)
				throw LoadCancelledException();
			if (game.cart_type != GD)
			{
				static std::string label;
				label = "ROM " + std::to_string(romid + 1);
				progress->label = label.c_str();
				progress->progress = (float)(romid + 1) / romCount;
			}
		}
		const auto& blob = game.blobs[romid];
		if (blob.blob_type == Copy)
		{
			u32 dstLength = blob.length;
			u32 srcLength = blob.length;
			u8 *dst = (u8 *)cartridge.GetPtr(blob.offset, dstLength);
			u8 *src = (u8 *)cartridge.GetPtr(blob.src_offset, srcLength);
			if (dst == nullptr || src == nullptr)
				throw NaomiCartException(Ts("Invalid ROM"));
			memcpy(dst, src, blob.length);
			DEBUG_LOG(NAOMI, "Copied: %x bytes from %07x to %07x", blob.length, blob.src_offset, blob.offset);
			continue;
		}

		std::unique_ptr<ArchiveFile> file;
		if (archive != nullptr)
			file.reset(archive->OpenFileByCrc(blob.crc));
		if (!file && parentArchive != nullptr)
			file.reset(parentArchive->OpenFileByCrc(blob.crc));
		if (!file && archive != nullptr)
			file.reset(archive->OpenFile(blob.filename));
		if (!file && parentArchive != nullptr)
			file.reset(parentArchive->OpenFile(blob.filename));
		if (!file)
		{
			WARN_LOG(NAOMI, "%s: Cannot open %s", game.name, blob.filename);
			if (blob.blob_type != Eeprom)
				throw NaomiCartException(strprintf(T("Cannot find %s"), blob.filename));
			continue;
		}
		if (blob.blob_type == Normal)
		{
			u32 mappedLength = blob.length;
			u8 *destination = (u8 *)cartridge.GetPtr(blob.offset, mappedLength);
			if (destination == nullptr)
				throw NaomiCartException(strprintf(T("Invalid ROM: truncated %s"), blob.filename));
			readExactArchiveFile(*file, destination, blob.length, blob.filename);
			if (hash)
				pendingMd5.add(destination, blob.length);
			continue;
		}
		loadGameBlob(*file, blob.blob_type, cartridge, blob.offset, blob.length, blob.filename,
				pendingMd5, hash, pendingEeprom, pendingSerial);
	}
	md5 = pendingMd5;
	defaultEeprom = std::move(pendingEeprom);
	serial = std::move(pendingSerial);
}

void naomi_cart::loadGameArchive(const Game& game, Archive *archive, Archive *parentArchive, Cartridge *cartridge,
		MD5Sum& md5, bool hash, std::vector<u8>& defaultEeprom, std::vector<u8>& serial, LoadProgress *progress)
{
	CurrentCartridge = cartridge;
	NaomiGameInputs = game.inputs;
	cartridge->game = &game;
	try
	{
		loadGameBlobs(game, archive, parentArchive, *cartridge, md5, hash, defaultEeprom, serial, progress);
	}
	catch (...)
	{
		delete CurrentCartridge;
		CurrentCartridge = nullptr;
		NaomiGameInputs = nullptr;
		throw;
	}
}

bool naomi_cart::loadBiosBlobs(const BIOS_t& bios, Archive *childArchive, Archive *parentArchive, Archive *biosArchive,
		int region, u8 *biosData, bool hash, int& loadedRegion, std::vector<u8>& publishedEeprom)
{
	struct BiosWrite { u32 offset; std::vector<u8> data; };
	std::vector<BiosWrite> writes;
	std::vector<u8> defaultEeprom;
	MD5Sum md5;
	bool foundEeprom = false;
	bool foundRegion = false;
	int selectedRegion = region;
	for (int romid = 0; bios.blobs[romid].filename != nullptr; romid++)
	{
		const auto& blob = bios.blobs[romid];
		if (region != -1 && blob.region != (u32)region)
			continue;
		std::unique_ptr<ArchiveFile> file;
		if (childArchive != nullptr)
			file.reset(childArchive->OpenFileByCrc(blob.crc));
		if (!file && parentArchive != nullptr)
			file.reset(parentArchive->OpenFileByCrc(blob.crc));
		if (!file && biosArchive != nullptr)
			file.reset(biosArchive->OpenFileByCrc(blob.crc));
		if (!file && childArchive != nullptr)
			file.reset(childArchive->OpenFile(blob.filename));
		if (!file && parentArchive != nullptr)
			file.reset(parentArchive->OpenFile(blob.filename));
		if (!file && biosArchive != nullptr)
			file.reset(biosArchive->OpenFile(blob.filename));
		if (!file)
			continue;
		switch (blob.blob_type)
		{
		case Normal:
			if (!foundRegion)
			{
				if ((u64)blob.offset + blob.length > BIOS_SIZE)
					throw NaomiCartException(strprintf(T("Invalid ROM: truncated %s"), blob.filename));
				BiosWrite write{ blob.offset, std::vector<u8>(blob.length) };
				readExactArchiveFile(*file, write.data.data(), blob.length, blob.filename);
				if (hash)
					md5.add(write.data.data(), write.data.size());
				writes.emplace_back(std::move(write));
				foundRegion = true;
				if (region == -1)
					selectedRegion = blob.region;
			}
			break;
		case EepromBE16:
			defaultEeprom.resize(blob.length);
			readExactArchiveFile(*file, defaultEeprom.data(), blob.length, blob.filename);
			for (size_t i = 0; i + 1 < defaultEeprom.size(); i += 2)
				std::swap(defaultEeprom[i], defaultEeprom[i + 1]);
			foundEeprom = true;
			if (hash)
				md5.add(defaultEeprom.data(), defaultEeprom.size());
			break;
		default:
			die("Unsupported BIOS blob type");
		}
	}
	for (const BiosWrite& write : writes)
		memcpy(biosData + write.offset, write.data.data(), write.data.size());
	if (foundEeprom)
		publishedEeprom = std::move(defaultEeprom);
	loadedRegion = selectedRegion;
	if (hash)
		md5.getDigest(settings.network.md5.bios);
	if (region == -1 && foundRegion)
		config::Region.override(selectedRegion);
	return foundRegion;
}

static bool loadBios(const char *biosName, Archive *child_archive, Archive *parent_archive, int region)
{
	int biosid = 0;
	for (; BIOS[biosid].name != nullptr; biosid++)
		if (!stricmp(BIOS[biosid].name, biosName))
			break;
	if (BIOS[biosid].name == nullptr)
	{
		WARN_LOG(NAOMI, "Unknown BIOS %s", biosName);
		return false;
	}

	const BIOS_t *bios = &BIOS[biosid];

	std::string arch_name(bios->filename != nullptr ? bios->filename : biosName);
	std::string path = hostfs::findNaomiBios(arch_name + ".zip");
	if (path.empty())
		path = hostfs::findNaomiBios(arch_name + ".7z");

	DEBUG_LOG(NAOMI, "Loading BIOS from %s", path.c_str());
	std::unique_ptr<Archive> bios_archive(OpenArchive(path));

	u8 *biosData = nvmem::getBiosData();
	int loadedRegion = region;
	std::vector<u8> loadedEeprom;
	bool foundRegion = naomi_cart::loadBiosBlobs(*bios, child_archive, parent_archive, bios_archive.get(), region,
			biosData, config::GGPOEnable, loadedRegion, loadedEeprom);
	if (!loadedEeprom.empty())
		publishDefaultEeprom(loadedEeprom);
	return foundRegion;
}

static const Game *FindGame(const char *filename)
{
	std::string gameName = get_file_basename(filename);
	size_t folder_pos = get_last_slash_pos(gameName);	// Only for standard path
	if (folder_pos != std::string::npos)
		gameName = gameName.substr(folder_pos + 1);

	for (int i = 0; Games[i].name != nullptr; i++)
		if (gameName == Games[i].name)
			return &Games[i];

	return nullptr;
}

void naomi_cart_LoadBios(const char *filename)
{
	const Game *game = FindGame(filename);
	if (game == nullptr)
	{
		if (settings.naomi.slave)
		{
			if (!loadBios(filename, nullptr, nullptr, config::Region))
				throw NaomiCartException(strprintf(T("Error: cannot load BIOS %s"), filename));
			bios_loaded = true;
		}
		return;
	}

	// Open archive and parent archive if any
	std::unique_ptr<Archive> archive(OpenArchive(filename));

	std::unique_ptr<Archive> parent_archive;
	if (game->parent_name != nullptr)
	{
		try {
			std::string parentPath = hostfs::storage().getParentPath(filename);
			parentPath = hostfs::storage().getSubPath(parentPath, game->parent_name);
			parent_archive.reset(OpenArchive(parentPath));
		} catch (const FlycastException& e) {
		}
	}

	const char *bios = "naomi";
	if (game->bios != nullptr)
		bios = game->bios;
	if (!loadBios(bios, archive.get(), parent_archive.get(), config::Region))
	{
		WARN_LOG(NAOMI, "Warning: Region %d bios not found in %s", (int)config::Region, bios);
		if (!loadBios(bios, archive.get(), parent_archive.get(), -1))
		{
			// If a specific BIOS is needed for this game, fail.
			if (game->bios != nullptr || !bios_loaded)
				throw NaomiCartException(strprintf(T("Error: cannot load BIOS %s"), game->bios != nullptr ? game->bios : "naomi.zip"));

			// otherwise use the default BIOS
		}
	}
	bios_loaded = true;
}

static void loadMameRom(const std::string& path, const std::string& fileName, LoadProgress *progress)
{
	const Game *game = FindGame(fileName.c_str());
	if (game == nullptr)
		throw NaomiCartException(Ts("Unknown game"));

	// Open archive and parent archive if any
	std::unique_ptr<Archive> archive(OpenArchive(path));
	if (archive != NULL)
		INFO_LOG(NAOMI, "Opened %s", path.c_str());

	std::unique_ptr<Archive> parent_archive;
	if (game->parent_name != nullptr)
	{
		try {
			std::string parentPath = hostfs::storage().getParentPath(path);
			parentPath = hostfs::storage().getSubPath(parentPath, game->parent_name);
			parent_archive.reset(OpenArchive(parentPath));
		} catch (const FlycastException& e) {
		}
		if (parent_archive != nullptr)
			INFO_LOG(NAOMI, "Opened %s", game->parent_name);
		else
			WARN_LOG(NAOMI, "Parent not found: %s", game->parent_name);

	}

	if (archive == nullptr && parent_archive == nullptr)
	{
		if (game->parent_name != nullptr)
			throw NaomiCartException(strprintf(T("Cannot open %s or %s"), fileName.c_str(), game->parent_name));
		else
			throw NaomiCartException(strprintf(T("Cannot open %s"), fileName.c_str()));
	}

	// Load the BIOS
	naomi_cart_LoadBios(fileName.c_str());

	// Now load the cartridge data
	std::unique_ptr<u8[]> previousOwnedEeprom;
	u8 *previousDefaultEeprom = nullptr;
	bool eepromPublicationStarted = false;
	try {
		switch (game->cart_type)
		{
		case M1:
			CurrentCartridge = new M1Cartridge(game->size);
			break;
		case M2:
			CurrentCartridge = new M2Cartridge(game->size);
			break;
		case M4:
			if (game->bios != nullptr && !strcmp(game->bios, "segasp"))
			{
				systemsp::SystemSpCart *spcart = new systemsp::SystemSpCart(game->size);
				if (game->gdrom_name != nullptr)
					spcart->setMediaName(game->gdrom_name, game->parent_name);
				CurrentCartridge = spcart;
			}
			else {
				CurrentCartridge = new M4Cartridge(game->size);
			}
			break;
		case AW:
			CurrentCartridge = new AWCartridge(game->size);
			break;
		case GD:
			{
				GDCartridge *gdcart;
				if (strncmp(game->name, "vf4", 3) == 0
						|| strcmp(game->name, "mj1") == 0
						|| strncmp(game->name, "wccf", 4) == 0
						|| strncmp(game->name, "dragntr", 7) == 0)
					gdcart = new NetDimm(game->size);
				else
					gdcart = new GDCartridge(game->size);
				gdcart->SetGDRomName(game->gdrom_name, game->parent_name);
				CurrentCartridge = gdcart;
			}
			break;
		default:
			die("Unsupported cartridge type\n");
			break;
		}
		CurrentCartridge->SetKey(game->key);

		MD5Sum md5;
		std::vector<u8> loadedEeprom;
		std::vector<u8> loadedSerial;
		if (progress != nullptr && progress->cancelled)
			throw LoadCancelledException();
		naomi_cart::loadGameArchive(*game, archive.get(), parent_archive.get(), CurrentCartridge,
				md5, config::GGPOEnable, loadedEeprom, loadedSerial, progress);

		if (!loadedEeprom.empty())
		{
			previousDefaultEeprom = naomi_default_eeprom;
			previousOwnedEeprom = std::move(ownedDefaultEeprom);
			eepromPublicationStarted = true;
			publishDefaultEeprom(loadedEeprom);
		}
		else if (naomi_default_eeprom == NULL && game->eeprom_dump != NULL)
		{
			previousDefaultEeprom = naomi_default_eeprom;
			previousOwnedEeprom = std::move(ownedDefaultEeprom);
			eepromPublicationStarted = true;
			naomi_default_eeprom = game->eeprom_dump;
		}
		if (game->rotation_flag == ROT270)
			config::Rotate90.override(true);

		std::vector<u8> gdromDigest;
		CurrentCartridge->Init(progress, config::GGPOEnable ? &gdromDigest : nullptr);
		if (!loadedSerial.empty())
			setGameSerialId(loadedSerial.data());

		if (config::GGPOEnable)
		{
			if (game->cart_type == GD)
			{
				std::vector<u8> romMD5 = md5.getDigest();
				md5 = MD5Sum().add(romMD5).add(gdromDigest);
			}
			md5.getDigest(settings.network.md5.game);
		}
		// Default game name if ROM boot id isn't found
		settings.content.gameId = game->name;

	} catch (...) {
		if (eepromPublicationStarted)
		{
			ownedDefaultEeprom = std::move(previousOwnedEeprom);
			naomi_default_eeprom = previousDefaultEeprom;
		}
		delete CurrentCartridge;
		CurrentCartridge = NULL;
		NaomiGameInputs = nullptr;
		throw;
	}
}

static void loadDecryptedRom(const std::string& path, const std::string& fileName, LoadProgress *progress)
{
	// Try to load BIOS from naomi.zip
	if (!loadBios("naomi", NULL, NULL, config::Region))
	{
		WARN_LOG(NAOMI, "Warning: Region %d bios not found in naomi.zip", config::Region.get());
		if (!loadBios("naomi", NULL, NULL, -1))
		{
			if (!bios_loaded)
				throw FlycastException(Ts("Error: cannot load BIOS from naomi.zip"));
		}
	}

	std::string folder;
	std::vector<std::string> files;
	std::vector<u32> fstart;
	std::vector<u32> fsize;
	u32 romSize = 0;

	std::string extension = get_file_extension(fileName);
	if (extension == "lst")
	{
		// LST file
		hostfs::File *fl = hostfs::storage().openFile(path, "r");
		if (!fl)
			throw FlycastException(strprintf(T("Error: can't open %s"), path.c_str()));
		folder = hostfs::storage().getParentPath(path);

		char t[512];
		char* line = fl->gets(t, sizeof(t));
		if (!line)
		{
			delete fl;
			throw FlycastException(Ts("Error: Invalid LST file"));
		}

		char* eon = strstr(line, "\n");
		if (!eon)
			DEBUG_LOG(NAOMI, "+Loading naomi rom that has no name");
		else
		{
			*eon = 0;
			DEBUG_LOG(NAOMI, "+Loading naomi rom : %s", line);
		}

		line = fl->gets(t, sizeof(t));
		if (!line)
		{
			delete fl;
			throw FlycastException(Ts("Error: Invalid LST file"));
		}

		while (line)
		{
			char filename[512];
			u32 addr, sz;
			if (sscanf(line, "\"%[^\"]\",%x,%x", filename, &addr, &sz) == 3)
			{
				files.emplace_back(filename);
				fstart.push_back(addr);
				fsize.push_back(sz);
				romSize = std::max(romSize, (addr + sz));
			}
			else if (line[0] != 0 && line[0] != '\n' && line[0] != '\r')
				WARN_LOG(NAOMI, "Warning: invalid line in .lst file: %s", line);

			line = fl->gets(t, sizeof(t));
		}
		delete fl;
	}
	else
	{
		// BIN loading
		hostfs::File* fp = hostfs::storage().openFile(path, "rb");
		if (fp == nullptr)
			throw FlycastException(strprintf(T("Error: can't open %s"), path.c_str()));

		fp->seek(0, SEEK_END);
		u32 file_size = (u32)fp->tell();
		delete fp;
		files.emplace_back(path);
		fstart.push_back(0);
		fsize.push_back(file_size);
		romSize = file_size;
	}

	INFO_LOG(NAOMI, "+%zd romfiles, %.2f MB set address space", files.size(), romSize / 1024.f / 1024.f);
	if (romSize == 0)
		throw FlycastException(Ts("Invalid empty ROM"));

	MD5Sum md5;

	// Allocate space for the rom
	u8 *romBase = (u8 *)malloc(romSize);
	if (romBase == nullptr)
		throw FlycastException(Ts("Out of memory"));

	bool load_error = false;

	for (size_t i = 0; i<files.size(); i++)
	{
		hostfs::File *fp = nullptr;

		if (files[i] != "null")
		{
			std::string filePath;
			if (folder.empty())
				filePath = files[i];
			else
				filePath = hostfs::storage().getSubPath(folder, files[i]);

			fp = hostfs::storage().openFile(filePath, "rb");
			if (fp == nullptr)
			{
				ERROR_LOG(NAOMI, "Unable to open file %s: error %d", filePath.c_str(), errno);
				load_error = true;
				break;
			}
		}
		u8* romDest = romBase + fstart[i];

		if (fp == nullptr)
		{
			//printf("-Reserving ram at 0x%08X, size 0x%08X\n", fstart[i], fsize[i]);
			memset(romDest, -1, fsize[i]);
		}
		else
		{
			//printf("-Mapping \"%s\" at 0x%08X, size 0x%08X\n", files[i].c_str(), fstart[i], fsize[i]);
			bool mapped = fp->read(romDest, 1, fsize[i]) == fsize[i];
			if (config::GGPOEnable)
				md5.add(fp);
			delete fp;
			if (!mapped)
			{
				ERROR_LOG(NAOMI, "Unable to read file %s @ %08x size %x", files[i].c_str(),
						fstart[i], fsize[i]);
				load_error = true;
				break;
			}
		}
	}

	if (load_error)
	{
		free(romBase);
		throw FlycastException(Ts("Error: Failed to load BIN/DAT file"));
	}
	if (config::GGPOEnable)
		md5.getDigest(settings.network.md5.game);

	DEBUG_LOG(NAOMI, "Legacy ROM loaded successfully");

	CurrentCartridge = new DecryptedCartridge(romBase, romSize);
}

void naomi_cart_LoadRom(const std::string& path, const std::string& fileName, LoadProgress *progress)
{
	naomi_cart_Close();

	if (settings.naomi.slave)
	{
		// Normal slaves only get the BIOS name, while derby gets the rom path, which has a .zip or .7z extension.
		// Not a very robust test though
		if (path.find('.') == path.npos)
		{
			CurrentCartridge = new NaomiCartridge(0);
#ifndef LIBRETRO
			settings.content.gameId = config::loadStr("naomi", "gameId", "");
#endif
			return;
		}
	}
	std::string extension = get_file_extension(fileName);

	if (extension == "zip" || extension == "7z")
		loadMameRom(path, fileName, progress);
	else
		loadDecryptedRom(path, fileName, progress);

	atomiswaveForceFeedback = false;
	RomBootID bootId;
	if (CurrentCartridge->GetBootId(&bootId)
			&& (!memcmp(bootId.boardName, "NAOMI", 5)
					|| !memcmp(bootId.boardName, "Naomi2", 6)
					|| !memcmp(bootId.boardName, "SYSTEM_X_APP", 12) // Atomiswave
					|| !memcmp(bootId.boardName, "SystemSP", 8)))	 // System SP
	{
		bool systemSP = memcmp(bootId.boardName, "SystemSP", 8) == 0;
		std::string gameId = trim_trailing_ws(std::string(bootId.gameTitle[systemSP ? 1 : 0], &bootId.gameTitle[systemSP ? 1 : 0][32]));
		std::string romName;
		if (CurrentCartridge->game != nullptr) {
			romName = CurrentCartridge->game->name;
			settings.content.title = CurrentCartridge->game->description;
		}
		if (gameId == "SAMPLE GAME MAX LONG NAME-")
		{
			// Use better game names
			if (romName == "sgdrvsim")
				gameId = "SEGA DRIVING SIMULATOR";
			else if (romName == "dragntr")
				gameId = "DRAGON TREASURE";
			else if (romName == "dragntr2")
				gameId = "DRAGON TREASURE 2";
			else if (romName == "dragntr3")
				gameId = "DRAGON TREASURE 3";
		}
		if (!gameId.empty())
			settings.content.gameId = gameId;
		NOTICE_LOG(NAOMI, "NAOMI GAME ID [%s] region %x players %x vertical %x", settings.content.gameId.c_str(), (u8)bootId.country, bootId.cabinet, bootId.vertical);

		if (gameId == "INITIAL D"
				|| gameId == "INITIAL D Ver.2"
				|| gameId == "INITIAL D Ver.3"
				|| gameId == "INITIAL D CYCRAFT")
		{
			card_reader::initdInit();
			midiffb::init();
		}
		else if (gameId == "MAXIMUM SPEED" || gameId == "FASTER THAN SPEED")
		{
			atomiswaveForceFeedback = true;
		}
		else if (gameId == "THE KING OF ROUTE66"
				|| gameId == "CLUB KART IN JAPAN"
				|| gameId == "SEGA DRIVING SIMULATOR")
		{
			if (settings.naomi.drivingSimSlave == 0)
				midiffb::init();
			if (romName == "clubkrt" || romName == "clubkrto"
					|| romName == "clubkrta" || romName == "clubkrtc")
				card_reader::clubkInit();
		}
		else if (gameId == "POKASUKA GHOST (JAPANESE)"	// Manic Panic Ghosts
				|| gameId == "TOUCH DE ZUNO (JAPAN)")
		{
			touchscreen::init();
		}
		else if (gameId.substr(0, 8) == "MKG TKOB"
				|| gameId.substr(0, 9) == "MUSHIKING"
				|| gameId == "MUSHIUSA '04 1ST VER0.900-"
				|| gameId.substr(0, 13) == "DINOSAUR KING"
				|| gameId == "INW PUPPY 2008 VER1.001"	// SystemSP isshoni
				|| gameId.substr(0, 14) == "LOVE AND BERRY"
				|| gameId == "BTR 2K9 VER 1.004")		// SystemSP Battle Racer
		{
			card_reader::barcodeInit();
		}
		else if (gameId == "ALIEN FRONT")
		{
			serialModemInit();
		}
		else if ((gameId.substr(0, 4) == "WCCF" && config::MultiboardSlaves <= 1)
					|| gameId.substr(0, 15) == "DRAGON TREASURE")
		{
			card_reader::wccfInit();
		}
		if (gameId == " TOUCH DE UNOH -------------"
			|| gameId == " TOUCH DE UNOH 2 -----------"
			|| (gameId == "F355 CHALLENGE JAPAN" && (config::MultiboardSlaves == 2 || romName == "f355"))
			|| gameId == "MIRAI YOSOU STUDIO")
		{
			printer::init();
		}
		if (romName == "clubkprz" || romName.substr(0, 7) == "clubkpz"
			|| romName.substr(0, 7) == "shootpl"
			|| gameId == "KICK '4' CASH")
		{
			hopper::init();
		}
		Naomi_setDmaDelay();

#ifdef NAOMI_MULTIBOARD
		// Not a multiboard game but needs the same desktop environment
		if (gameId == "SEGA DRIVING SIMULATOR")
		{
			initDriveSimSerialPipe();

			config::NetworkEnable.override(true);
			config::ActAsServer.override(settings.naomi.drivingSimSlave == 0);
			config::NetworkServer.override("localhost:" + std::to_string(config::LocalPort));
			config::LocalPort.override(config::LocalPort + settings.naomi.drivingSimSlave);
			if (settings.naomi.drivingSimSlave == 0)
			{
				int x = config::loadInt("window", "left", (1920 - 640) / 2);
				int w = config::loadInt("window", "width", 640);
				std::string region = "config:Dreamcast.Region=" + std::to_string(config::Region);
				for (int i = 0; i < 2; i++)
				{
					std::string slaveNum = "naomi:DrivingSimSlave=" + std::to_string(i + 1);
					std::string left = "window:left=" + std::to_string(i == 1 ? x - w : x + w);
					std::string title = "window:title=\""
							+ (i == 1 ? Ts("Left") : Ts("Right"))
							+ " - " + settings.content.title + '"';
					const char *args[] = {
						"-config", slaveNum.c_str(),
						"-config", region.c_str(),
						"-config", left.c_str(),
						"-config", title.c_str(),
						settings.content.path.c_str()
					};
					os_RunInstance(std::size(args), args);
				}
			}
		}
		else if (gameId.substr(0, 4) == "WCCF" && config::MultiboardSlaves > 1)
		{
			// Main projectors are directly linked by a null-modem cable
			config::BattleCableEnable.override(true);
			if (!config::loadBool("naomi", "WCCFSlave"))
			{
				config::ActAsServer.override(true);
				config::NetworkServer.override("localhost:37392");
				config::LocalPort.override(37391);
				int x = config::loadInt("window", "left", (1920 - 640) / 2);
				int w = config::loadInt("window", "width", 640);
				std::string region = "config:Dreamcast.Region=" + std::to_string(config::Region);
				std::string left = "window:left=" + std::to_string(x + w);
				std::string title = "window:title=\"" + Ts("Right")
						+ " - " + settings.content.title + '"';
				std::string server = "naomi:WCCFServer=" + config::loadStr("naomi", "WCCFServer");

				const char *args[] = {
						"-config", "naomi:WCCFSlave=yes",
						"-config", server.c_str(),
						"-config", region.c_str(),
						"-config", left.c_str(),
						"-config", title.c_str(),
						"-config", "network:MultiboardSlaves=2",
						settings.content.path.c_str()
				};
				os_RunInstance(std::size(args), args);
			}
			else
			{
				config::ActAsServer.override(false);
				config::NetworkServer.override("localhost:37391");
				config::LocalPort.override(37392);
			}
		}
#endif
	}
	else
	{
		NOTICE_LOG(NAOMI, "NAOMI GAME ID [%s]", settings.content.gameId.c_str());
	}
}

void naomi_cart_ConfigureEEPROM()
{
	if (!settings.platform.isNaomi())
		return;

	RomBootID bootId;
	if (CurrentCartridge->GetBootId(&bootId)
			&& (!memcmp(bootId.boardName, "NAOMI", 5) || !memcmp(bootId.boardName, "Naomi2", 6)))
		configure_naomi_eeprom(&bootId);
	else if (!settings.naomi.slave)
		WARN_LOG(NAOMI, "Can't read ROM boot ID");
}

void naomi_cart_Close()
{
	touchscreen::term();
	printer::term();
	card_reader::term();
	card_reader::barcodeTerm();
	serialModemTerm();
	hopper::term();
	delete CurrentCartridge;
	CurrentCartridge = nullptr;
	NaomiGameInputs = nullptr;
	bios_loaded = false;
	ownedDefaultEeprom.reset();
	naomi_default_eeprom = nullptr;
}

void naomi_cart_serialize(Serializer& ser)
{
	if (CurrentCartridge != nullptr)
		CurrentCartridge->Serialize(ser);
	touchscreen::serialize(ser);
	printer::serialize(ser);
	hopper::serialize(ser);
}

void naomi_cart_deserialize(Deserializer& deser)
{
	if (CurrentCartridge != nullptr)
		CurrentCartridge->Deserialize(deser);
	touchscreen::deserialize(deser);
	printer::deserialize(deser);
	hopper::deserialize(deser);
}

int naomi_cart_GetPlatform(const char *path, bool *multiboard)
{
	if (multiboard != nullptr)
		*multiboard = false;
	const Game *game = FindGame(path);
	if (game == nullptr)
		return DC_PLATFORM_NAOMI;
	const int platform = game->platform();
#ifdef NAOMI_MULTIBOARD
	if (multiboard != nullptr && platform == DC_PLATFORM_NAOMI && game->multiboard > 0
			&& (!strncmp("f355", game->name, 4) || config::MultiboardSlaves == 2))
		*multiboard = true;
#endif
	return platform;
}

Cartridge::Cartridge(u32 size)
{
	RomPtr = (u8 *)malloc(size);
	if (RomPtr == nullptr)
		throw NaomiCartException(Ts("Memory allocation failed"));
	RomSize = size;
	if (size != 0)
		memset(RomPtr, 0xFF, RomSize);
}

Cartridge::~Cartridge()
{
	if (RomPtr != NULL)
		free(RomPtr);
}

bool Cartridge::Read(u32 offset, u32 size, void* dst)
{
	offset &= 0x1FFFFFFF;
	if (offset >= RomSize || (offset + size) > RomSize)
	{
		static u32 ones = 0xffffffff;

		// Makes Outtrigger boot
		DEBUG_LOG(NAOMI, "Cartridge::Read: overrun: offset %x > rom size %x", offset, RomSize);
		memcpy(dst, &ones, size);
	}
	else
	{
		memcpy(dst, &RomPtr[offset], size);
	}

	return true;
}

bool Cartridge::Write(u32 offset, u32 size, u32 data)
{
	INFO_LOG(NAOMI, "Invalid write @ %08x data %x", offset, data);
	return false;
}

void* Cartridge::GetPtr(u32 offset, u32& size)
{
	offset &= 0x1fffffff;

	if (offset >= RomSize || size > RomSize - offset)
	{
		WARN_LOG(NAOMI, "Invalid naomi cart: offset %x size %x rom size %x", offset, size, RomSize);
		size = 0;
		return nullptr;
	}

	return &RomPtr[offset];
}

bool NaomiCartridge::GetBootId(RomBootID *bootId)
{
	if (RomSize < sizeof(RomBootID))
		return false;
	u8 *p = (u8 *)bootId;
	u32 size = sizeof(RomBootID);
	DmaOffset = 0;
	while (size > 0)
	{
		u32 chunkSize = size;
		void *src = GetDmaPtr(chunkSize);
		if (chunkSize == 0)
			return false;
		memcpy(p, src, chunkSize);
		p += chunkSize;
		size -= chunkSize;
		AdvancePtr(chunkSize);
	}
	return true;
}

void* NaomiCartridge::GetDmaPtr(u32& size)
{
	if ((DmaOffset & 0x1fffffff) >= RomSize)
	{
		INFO_LOG(NAOMI, "Error: DmaOffset (%x) >= RomSize (%x)", DmaOffset, RomSize);
		size = 0;
		return nullptr;
	}
	size = std::min(size, RomSize - (DmaOffset & 0x1fffffff));
	return GetPtr(DmaOffset, size);
}

u32 NaomiCartridge::ReadMem(u32 address, u32 size)
{
//	verify(size != 1); not true anymore with multiboard

	switch (address)
	{
	case NAOMI_DIMM_COMMAND:
		//DEBUG_LOG(NAOMI, "DIMM COMMAND read");
		return 0xffff;
	case NAOMI_DIMM_OFFSETL:
		DEBUG_LOG(NAOMI, "DIMM OFFSETL read");
		return 0xffff;
	case NAOMI_DIMM_PARAMETERL:
		DEBUG_LOG(NAOMI, "DIMM PARAMETERL read");
		return 0xffff;
	case NAOMI_DIMM_PARAMETERH:
		DEBUG_LOG(NAOMI, "DIMM PARAMETERH read");
		return 0xffff;
	case NAOMI_DIMM_STATUS:
		DEBUG_LOG(NAOMI, "DIMM STATUS read");
		return 0x7fff;

	case NAOMI_ROM_OFFSETH_addr:
		return RomPioOffset >> 16 | (RomPioAutoIncrement << 15);

	case NAOMI_ROM_OFFSETL_addr:
		return RomPioOffset & 0xFFFF;

	case NAOMI_ROM_DATA_addr:
		{
			u32 rv = 0;
			Read(RomPioOffset, 2, &rv);
			if (RomPioAutoIncrement)
				RomPioOffset += 2;

			return rv;
		}

	case NAOMI_DMA_COUNT_addr:
		return (u16)DmaCount;

	case NAOMI_BOARDID_READ_addr:
		return NaomiGameIDRead();

	case NAOMI_DMA_OFFSETH_addr:
		return DmaOffset >> 16;
	case NAOMI_DMA_OFFSETL_addr:
		return DmaOffset & 0xFFFF;

	case NAOMI_BOARDID_WRITE_addr:
		return 1;

	default:
		break;
	}

	if (address == NAOMI_MBOARD_DATA_addr || address == NAOMI_MBOARD_OFFSET_addr)
		return 1;
	else {
		DEBUG_LOG(NAOMI, "naomiCart::ReadMem<%d> unknown: %08x", size, address);
		return 0xFFFF;
	}
}

void NaomiCartridge::WriteMem(u32 address, u32 data, u32 size)
{
	switch (address)
	{
	case NAOMI_DIMM_COMMAND:
		 DEBUG_LOG(NAOMI, "DIMM COMMAND Write<%d>: %x", size, data);
		 return;

	case NAOMI_DIMM_OFFSETL:
		DEBUG_LOG(NAOMI, "DIMM OFFSETL Write<%d>: %x", size, data);
		return;
	case NAOMI_DIMM_PARAMETERL:
		DEBUG_LOG(NAOMI, "DIMM PARAMETERL Write<%d>: %x", size, data);
		return;
	case NAOMI_DIMM_PARAMETERH:
		DEBUG_LOG(NAOMI, "DIMM PARAMETERH Write<%d>: %x", size, data);
		return;

	case NAOMI_DIMM_STATUS:
		DEBUG_LOG(NAOMI, "DIMM STATUS Write<%d>: %x", size, data);
		return;

		//These are known to be valid on normal ROMs and DIMM board
	case NAOMI_ROM_OFFSETH_addr:
		RomPioAutoIncrement = (data & 0x8000) != 0;
		RomPioOffset &= 0x0000ffff;
		RomPioOffset |= (data << 16) & 0x7fff0000;
		PioOffsetChanged(RomPioOffset);
		return;

	case NAOMI_ROM_OFFSETL_addr:
		RomPioOffset &= 0xffff0000;
		RomPioOffset |= data;
		PioOffsetChanged(RomPioOffset);
		return;

	case NAOMI_ROM_DATA_addr:
		Write(RomPioOffset, size, data);
		if (RomPioAutoIncrement)
			RomPioOffset += 2;

		return;

	case NAOMI_DMA_OFFSETH_addr:
		DmaOffset &= 0x0000ffff;
		DmaOffset |= (data & 0x7fff) << 16;
		DmaOffsetChanged(DmaOffset);
		return;

	case NAOMI_DMA_OFFSETL_addr:
		DmaOffset &= 0xffff0000;
		DmaOffset |= data;
		DmaOffsetChanged(DmaOffset);
		return;

	case NAOMI_DMA_COUNT_addr:
		DmaCount = data;
		return;

	case NAOMI_BOARDID_WRITE_addr:
		NaomiGameIDWrite((u16)data);
		return;

	case NAOMI_BOARDID_READ_addr:
		return;

	case NAOMI_LED_addr:
		//DEBUG_LOG(NAOMI, "LED %d %d %d %d %d %d %d %d", (data >> 7) & 1, (data >> 6) & 1, (data >> 5) & 1, (data >> 4) & 1,
		//		(data >> 3) & 1, (data >> 2) & 1, (data >> 1) & 1, data & 1);
		return;

	default:
		break;
	}
	if (address != NAOMI_MBOARD_DATA_addr
			&& address != NAOMI_MBOARD_OFFSET_addr
			&& address != NAOMI_MBOARD_STATUS_addr)
		DEBUG_LOG(NAOMI, "naomiCart::WriteMem<%d>: unknown %08x <= %x", size, address, data);
}

void NaomiCartridge::Serialize(Serializer& ser) const
{
	ser << RomPioOffset;
	ser << RomPioAutoIncrement;
	ser << DmaOffset;
	ser << DmaCount;
	Cartridge::Serialize(ser);
}

void NaomiCartridge::Deserialize(Deserializer& deser)
{
	deser >> RomPioOffset;
	deser >> RomPioAutoIncrement;
	deser >> DmaOffset;
	deser >> DmaCount;
	Cartridge::Deserialize(deser);
}

bool M2Cartridge::Read(u32 offset, u32 size, void* dst)
{
	if (offset & 0x40000000)
	{
		if (offset == 0x4001fffe)
		{
			//printf("NAOMI CART DECRYPT read: %08x sz %d\n", offset, size);
			cyptoSetKey(key);
			u16 data = cryptoDecrypt();
			*(u16 *)dst = data;
			return true;
		}
		INFO_LOG(NAOMI, "Invalid read @ %08x", offset);
		return false;
	}
	else if (!(RomPioOffset & 0x20000000))
	{
		// 4MB mode
		offset = (offset & 0x103fffff) | ((offset & 0x07c00000) << 1);
	}
	return NaomiCartridge::Read(offset, size, dst);
}

void* M2Cartridge::GetDmaPtr(u32& size)
{
	if (RomPioOffset & 0x20000000)
		return NaomiCartridge::GetDmaPtr(size);

	// 4MB mode
	u32 offset4mb = (DmaOffset & 0x103fffff) | ((DmaOffset & 0x07c00000) << 1);
	size = std::min(std::min(size, 0x400000 - (offset4mb & 0x3FFFFF)), RomSize - offset4mb);

	return GetPtr(offset4mb, size);
}

bool M2Cartridge::Write(u32 offset, u32 size, u32 data)
{
	if (offset & 0x40000000)
	{
		//printf("NAOMI CART CRYPT write: %08x data %x sz %d\n", offset, data, size);
		if (offset & 0x00020000)
		{
			offset &= sizeof(naomi_cart_ram) - 1;
			naomi_cart_ram[offset] = data;
			naomi_cart_ram[offset + 1] = data >> 8;
			return true;
		}
		switch (offset & 0x1ffff)
		{
			case 0x1fff8:
				cyptoSetLowAddr(data);
				return true;
			case 0x1fffa:
				cyptoSetHighAddr(data);
				return true;
			case 0x1fffc:
				cyptoSetSubkey(data);
				return true;
		}
	}
	return NaomiCartridge::Write(offset, size, data);
}

u16 M2Cartridge::ReadCipheredData(u32 offset)
{
	if ((offset & 0xffff0000) == 0x01000000)
	{
		int base = 2 * (offset & 0x7fff);
		return naomi_cart_ram[base + 1] | (naomi_cart_ram[base] << 8);
	}
	verify(2 * offset + 1 < RomSize);
	return RomPtr[2 * offset + 1] | (RomPtr[2 * offset] << 8);

}

bool M2Cartridge::GetBootId(RomBootID *bootId)
{
	if (RomSize < sizeof(RomBootID))
		return false;
	RomBootID *pBootId = (RomBootID *)RomPtr;
	if ((pBootId->gameTitle[0][0] == '\0'
			|| ((u8)pBootId->gameTitle[0][0] == 0xff && (u8)pBootId->gameTitle[0][1] == 0xff)))
	{
		if (RomSize < 0x800000 + sizeof(RomBootID))
			return false;
		pBootId = (RomBootID *)(RomPtr + 0x800000);
	}
	memcpy(bootId, pBootId, sizeof(RomBootID));

	return true;
}

void M2Cartridge::Serialize(Serializer& ser) const {
	ser << naomi_cart_ram;
	NaomiCartridge::Serialize(ser);
}

void M2Cartridge::Deserialize(Deserializer& deser) {
	deser >> naomi_cart_ram;
	NaomiCartridge::Deserialize(deser);
}
