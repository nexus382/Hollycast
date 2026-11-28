// Portions Copyright 2026 The Hollycast Authors

#include "build.h"
#ifdef USE_WINDOWS_CDROM

#include "imgread/windows_cdrom.h"
#include "imgread/common.h"
#include "oslib/i18n.h"
#include "oslib/oslib.h"

#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <ntddscsi.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>

namespace
{

constexpr u32 firstSectorFad = 150;
constexpr unsigned maxTrack = 99;
constexpr unsigned maxSession = 99;
constexpr size_t rawSectorSize = 2352;
constexpr std::string_view devicePrefix = "\\\\.\\";

bool isCdromDrive(char letter)
{
	const wchar_t root[] = { static_cast<wchar_t>(letter), L':', L'\\', L'\0' };
	return GetDriveTypeW(root) == DRIVE_CDROM;
}

struct CloseDevice
{
	void operator()(void *handle) const { CloseHandle(handle); }
};

using DeviceHandle = std::unique_ptr<void, CloseDevice>;

struct SectorRequest
{
	SCSI_PASS_THROUGH command;
	std::array<u8, 32> sense;
	alignas(8) std::array<u8, rawSectorSize> data;
};

bool readRawSector(HANDLE drive, u32 fad, bool audio, u8 *dst)
{
	if (fad < firstSectorFad)
		return false;

	SectorRequest request{};
	auto& command = request.command;
	command.Length = sizeof(SCSI_PASS_THROUGH);
	command.CdbLength = 12;
	command.SenseInfoLength = request.sense.size();
	command.DataIn = SCSI_IOCTL_DATA_IN;
	command.DataTransferLength = rawSectorSize;
	command.TimeOutValue = 5;
	command.DataBufferOffset = offsetof(SectorRequest, data);
	command.SenseInfoOffset = offsetof(SectorRequest, sense);

	const u32 lba = fad - firstSectorFad;
	command.Cdb[0] = 0xbe; // MMC READ CD
	command.Cdb[1] = audio ? 1 << 2 : 0; // CD-DA or any data sector type
	command.Cdb[2] = static_cast<u8>(lba >> 24);
	command.Cdb[3] = static_cast<u8>(lba >> 16);
	command.Cdb[4] = static_cast<u8>(lba >> 8);
	command.Cdb[5] = static_cast<u8>(lba);
	command.Cdb[8] = 1;
	// A CD-DA sector is all user data. Data sectors also need sync, headers and EDC/ECC.
	command.Cdb[9] = audio ? 0x10 : 0xf8;

	DWORD bytesRead = 0;
	if (!DeviceIoControl(drive, IOCTL_SCSI_PASS_THROUGH, &request, sizeof(request),
			&request, sizeof(request), &bytesRead, nullptr))
	{
		const DWORD error = GetLastError();
		DEBUG_LOG(GDROM, "READ CD failed at FAD %u (Windows error %lu)", fad, error);
		return false;
	}
	if (command.ScsiStatus != 0 || command.DataTransferLength != rawSectorSize
			|| bytesRead < offsetof(SectorRequest, data) + rawSectorSize)
	{
		DEBUG_LOG(GDROM, "READ CD failed at FAD %u (SCSI status %02x, transfer %lu, returned %lu)",
				fad, static_cast<unsigned>(command.ScsiStatus), command.DataTransferLength, bytesRead);
		return false;
	}
	std::memcpy(dst, request.data.data(), rawSectorSize);
	return true;
}

struct WindowsCdromDisc final : Disc
{
	explicit WindowsCdromDisc(DeviceHandle drive) : drive(std::move(drive)) {}

	~WindowsCdromDisc() override
	{
		if (mediaLocked)
		{
			PREVENT_MEDIA_REMOVAL removal{FALSE};
			DWORD bytesReturned;
			DeviceIoControl(drive.get(), IOCTL_STORAGE_EJECTION_CONTROL, &removal, sizeof(removal),
					nullptr, 0, &bytesReturned, nullptr);
		}
	}

	DeviceHandle drive;
	bool mediaLocked = false;
};

struct WindowsCdromTrack final : TrackFile
{
	WindowsCdromTrack(HANDLE drive, bool audio) : drive(drive), audio(audio) {}

	bool Read(u32 fad, u8 *dst, SectorFormat *format, u8 *, SubcodeFormat *subcodeFormat) override
	{
		if (!readRawSector(drive, fad, audio, dst))
			return false;
		*format = SECFMT_2352;
		*subcodeFormat = SUBFMT_NONE;
		return true;
	}

	HANDLE drive;
	bool audio;
};

struct SessionToc
{
	u8 firstTrack = 0;
	u8 lastTrack = 0;
	bool xa = false;
	Track leadout;
};

u32 tocFad(const CDROM_TOC_FULL_TOC_DATA_BLOCK& entry)
{
	// The drive returns binary PMIN/PSEC/PFRAME for track starts and A2 lead-outs.
	if (entry.Msf[1] >= 60 || entry.Msf[2] >= 75)
		return 0;
	return (entry.Msf[0] * 60 + entry.Msf[1]) * 75 + entry.Msf[2];
}

bool parseToc(Disc& disc, const u8 *buffer, size_t bytesRead, unsigned& bootTrack, bool& bootSessionXa)
{
	constexpr size_t headerSize = offsetof(CDROM_TOC_FULL_TOC_DATA, Descriptors);
	if (bytesRead < headerSize)
		return false;
	const size_t tocSize = (buffer[0] << 8 | buffer[1]) + 2;
	const unsigned firstSession = buffer[2];
	const unsigned lastSession = buffer[3];
	if (tocSize <= headerSize || tocSize > bytesRead
			|| (tocSize - headerSize) % sizeof(CDROM_TOC_FULL_TOC_DATA_BLOCK) != 0
			|| firstSession == 0 || lastSession > maxSession || firstSession > lastSession)
		return false;

	std::array<SessionToc, maxSession + 1> sessionToc;
	std::array<Track, maxTrack + 1> tracks;
	std::array<u8, maxTrack + 1> trackSessions{};
	for (size_t offset = headerSize; offset < tocSize; offset += sizeof(CDROM_TOC_FULL_TOC_DATA_BLOCK))
	{
		CDROM_TOC_FULL_TOC_DATA_BLOCK entry;
		std::memcpy(&entry, buffer + offset, sizeof(entry));
		if (entry.Adr != 1)
			continue; // Other ADR values carry metadata such as the next writable address.
		if (entry.SessionNumber < firstSession || entry.SessionNumber > lastSession)
			return false;
		SessionToc& session = sessionToc[entry.SessionNumber];
		switch (entry.Point)
		{
		case 0xa0: // First track and session format
			if (session.firstTrack != 0 || entry.Msf[0] == 0 || entry.Msf[0] > maxTrack)
				return false;
			session.firstTrack = entry.Msf[0];
			session.xa = entry.Msf[1] == 0x20;
			break;
		case 0xa1: // Last track
			if (session.lastTrack != 0 || entry.Msf[0] == 0 || entry.Msf[0] > maxTrack)
				return false;
			session.lastTrack = entry.Msf[0];
			break;
		case 0xa2: // Lead-out
			if (session.leadout.StartFAD != 0)
				return false;
			session.leadout.StartFAD = tocFad(entry);
			if (session.leadout.StartFAD < firstSectorFad)
				return false;
			session.leadout.CTRL = entry.Control;
			session.leadout.ADR = entry.Adr;
			break;
		default:
			if (entry.Point == 0 || entry.Point > maxTrack)
				break;
			if (trackSessions[entry.Point] != 0)
				return false;
			trackSessions[entry.Point] = entry.SessionNumber;
			Track& track = tracks[entry.Point];
			track.StartFAD = tocFad(entry);
			if (track.StartFAD < firstSectorFad)
				return false;
			track.CTRL = entry.Control;
			track.ADR = entry.Adr;
			break;
		}
	}

	unsigned nextTrack = 1;
	u32 previousLeadout = 0;
	for (unsigned number = firstSession; number <= lastSession; ++number)
	{
		const SessionToc& session = sessionToc[number];
		if (session.firstTrack != nextTrack || session.lastTrack < session.firstTrack
				|| tracks[nextTrack].StartFAD <= previousLeadout)
			return false;
		disc.sessions.push_back({tracks[nextTrack].StartFAD, session.firstTrack});
		bool hasSessionData = false;
		for (; nextTrack <= session.lastTrack; ++nextTrack)
		{
			Track& track = tracks[nextTrack];
			const u32 end = nextTrack == session.lastTrack ? session.leadout.StartFAD : tracks[nextTrack + 1].StartFAD;
			if (trackSessions[nextTrack] != number || end <= track.StartFAD)
				return false;
			track.EndFAD = end - 1;
			if (track.isDataTrack() && !hasSessionData)
			{
				bootTrack = nextTrack;
				bootSessionXa = session.xa;
				hasSessionData = true;
			}
			disc.tracks.push_back(track);
		}
		previousLeadout = session.leadout.StartFAD;
	}
	for (; nextTrack <= maxTrack; ++nextTrack)
		if (trackSessions[nextTrack] != 0)
			return false;

	disc.LeadOut = sessionToc[lastSession].leadout;
	disc.EndFAD = disc.LeadOut.StartFAD; // Disc session info reports the lead-out address.
	return true;
}

void readToc(WindowsCdromDisc& disc, const std::string& path)
{
	CDROM_READ_TOC_EX request{};
	request.Format = CDROM_READ_TOC_EX_FORMAT_FULL_TOC;
	request.Msf = 1;
	// Leave SessionTrack at zero to include every complete session.
	std::array<u8, 32 * 1024> buffer;
	DWORD bytesRead = 0;
	if (!DeviceIoControl(disc.drive.get(), IOCTL_CDROM_READ_TOC_EX, &request, sizeof(request),
			buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr))
	{
		const DWORD error = GetLastError();
		WARN_LOG(GDROM, "Cannot read full CD TOC from %s (Windows error %lu)", path.c_str(), error);
		throw FlycastException(i18n::Ts("Cannot read the CD. Insert a finalized disc and try again."));
	}

	unsigned bootTrack = 0;
	bool bootSessionXa = false;
	if (bytesRead > buffer.size() || !parseToc(disc, buffer.data(), bytesRead, bootTrack, bootSessionXa))
	{
		WARN_LOG(GDROM, "Invalid full CD TOC from %s", path.c_str());
		throw FlycastException(i18n::Ts("The CD has an invalid or incomplete table of contents."));
	}

	bool hasAudio = false;
	for (Track& track : disc.tracks)
	{
		const bool audio = !track.isDataTrack();
		hasAudio |= audio;
		track.file = new WindowsCdromTrack(disc.drive.get(), audio);
	}

	if (bootTrack == 0)
		disc.type = CdDA;
	else
	{
		std::array<u8, rawSectorSize> sector;
		const u32 bootFad = disc.tracks[bootTrack - 1].StartFAD;
		if (!readRawSector(disc.drive.get(), bootFad, false, sector.data()))
		{
			WARN_LOG(GDROM, "Cannot read the data track on %s at FAD %u", path.c_str(), bootFad);
			throw FlycastException(i18n::Ts("Cannot read the CD data track. Check the disc and optical drive."));
		}
		disc.type = GuessDiscType(true, bootSessionXa || sector[15] == 2, hasAudio);
	}
}

}

std::string windowsCdromDevicePath(std::string_view path)
{
	if (path.substr(0, devicePrefix.size()) == devicePrefix)
		path.remove_prefix(devicePrefix.size());
	if (path.size() != 2 || path[1] != ':')
		return {};

	char letter = path[0];
	if (letter >= 'a' && letter <= 'z')
		letter -= 'a' - 'A';
	if (letter < 'A' || letter > 'Z' || !isCdromDrive(letter))
		return {};
	return std::string(devicePrefix) + letter + ':';
}

std::vector<std::string> hostfs::getCdromDrives()
{
	std::vector<std::string> drives;
	const DWORD mask = GetLogicalDrives();
	for (char letter = 'A'; letter <= 'Z'; ++letter)
		if ((mask & (1UL << (letter - 'A'))) != 0 && isCdromDrive(letter))
			drives.push_back(std::string(devicePrefix) + letter + ':');
	return drives;
}

Disc *windows_cdrom_parse(const std::string& devicePath)
{
	// SCSI pass-through requires read and write access to the handle even for READ CD.
	const HANDLE handle = CreateFileA(devicePath.c_str(), GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
	{
		const DWORD error = GetLastError();
		WARN_LOG(GDROM, "Cannot open CD drive %s (Windows error %lu)", devicePath.c_str(), error);
		throw FlycastException(i18n::Ts("Cannot open the CD drive. Check that it is connected and accessible."));
	}

	auto disc = std::make_unique<WindowsCdromDisc>(DeviceHandle(handle));
	// Keep the mounted disc in place so reads cannot use another disc with this TOC.
	PREVENT_MEDIA_REMOVAL removal{TRUE};
	DWORD bytesReturned;
	if (!DeviceIoControl(disc->drive.get(), IOCTL_STORAGE_EJECTION_CONTROL, &removal, sizeof(removal),
			nullptr, 0, &bytesReturned, nullptr))
	{
		const DWORD error = GetLastError();
		WARN_LOG(GDROM, "Cannot lock CD drive %s (Windows error %lu)", devicePath.c_str(), error);
		throw FlycastException(i18n::Ts("Cannot keep the CD mounted in this drive. Check the drive and try again."));
	}
	disc->mediaLocked = true;
	readToc(*disc, devicePath);
	return disc.release();
}

#endif
