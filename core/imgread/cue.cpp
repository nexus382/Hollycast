/*
	Copyright 2019 flyinghead
	Portions Copyright 2026 The Hollycast Authors

	This file is part of reicast.

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

#include "common.h"
#include "stdclass.h"
#include "oslib/storage.h"
#include "oslib/i18n.h"
#include <sstream>
#include <limits>
#include <optional>

static u32 getSectorSize(const std::string& type) {
		if (type == "AUDIO")
			return 2352;	// PCM Audio
		else if (type == "CDG")
			return 2352;	// Karaoke cd+g
		else if (type == "MODE1/2048")
			return 2048;	// CDROM Mode1 Data (cooked)
		else if (type == "MODE1/2352")
			return 2352;	// CDROM Mode1 Data (raw)
		else if (type == "MODE2/2336")
			return 2336;	// CDROM XA Mode2 Data
		else if (type == "MODE2/2352")
			return 2352;	// CDROM XA Mode2 Data
		else if (type == "CDI/2336")
			return 2336;	// CDI Mode2 Data
		else if (type == "CDI/2352")
			return 2352;	// CDI Mode2 Data
		else
			return 0;
}

struct CueTrackLayout
{
	u32 sectorSize = 0;
	std::string type;
	std::string isrc;
	std::optional<u32> index00;
	std::optional<u32> index01;
	u32 pregap = 0;
	u64 index00ByteOffset = 0;
	u64 index01ByteOffset = 0;
};

static u32 parseCueIndex(const std::string& time)
{
	int minutes = 0, seconds = 0, frames = 0;
	char extra = 0;
	if (sscanf(time.c_str(), "%d:%d:%d%c", &minutes, &seconds, &frames, &extra) != 3
			|| minutes < 0 || seconds < 0 || seconds >= 60 || frames < 0 || frames >= 75)
		throw FlycastException(i18n::T("Invalid INDEX in CUE file"));
	const u64 result = (u64)minutes * 60 * 75 + (u64)seconds * 75 + frames;
	if (result > std::numeric_limits<u32>::max())
		throw FlycastException(i18n::T("Invalid INDEX in CUE file"));
	return (u32)result;
}

Disc* cue_parse(const char* file, std::vector<u8> *digest)
{
	if (get_file_extension(file) != "cue")
		return nullptr;

	hostfs::File *fsource = hostfs::storage().openFile(file, "rb");

	if (fsource == nullptr)
	{
		WARN_LOG(COMMON, "Cannot open file '%s' errno %d", file, errno);
		throw FlycastException(strprintf(i18n::T("Cannot open CUE file %s"), file));
	}

	hostfs::FileInfo fileInfo = hostfs::storage().getFileInfo(file);
	size_t cue_len = fileInfo.size;

	char cue_data[64_KB] = { 0 };

	if (cue_len >= sizeof(cue_data))
	{
		delete fsource;
		throw FlycastException(i18n::Ts("CUE parse error: CUE file too big"));
	}

	if (fsource->read(cue_data, 1, cue_len) != cue_len)
		WARN_LOG(GDROM, "Failed or truncated read of cue file '%s'", file);
	delete fsource;

	std::istringstream istream(cue_data);
	istream.imbue(std::locale::classic());

	std::string basepath = hostfs::storage().getParentPath(file);

	MD5Sum md5;

	std::unique_ptr<Disc> disc = std::make_unique<Disc>();
	u64 currentFAD = 150;
	// SESSION context
	u32 session_number = 0;
	bool firstTrackOfSession = false;
	// FILE context
	std::string track_filename;
	u64 fileStartFAD = 0;
	std::vector<CueTrackLayout> fileTracks;
	// TRACK context
	u32 track_number = -1;
	std::string track_type;
	u32 track_secsize = 0;
	std::string track_isrc;
	u32 pregap = 0;

	auto checkedAdd = [](u64 lhs, u64 rhs) {
		if (rhs > std::numeric_limits<u64>::max() - lhs)
			throw FlycastException(i18n::T("CUE file layout is too large"));
		return lhs + rhs;
	};
	auto checkedMultiply = [](u64 lhs, u64 rhs) {
		if (lhs != 0 && rhs > std::numeric_limits<u64>::max() / lhs)
			throw FlycastException(i18n::T("CUE file layout is too large"));
		return lhs * rhs;
	};
	auto finalizeFile = [&]() {
		if (fileTracks.empty())
			return;
		for (const CueTrackLayout& track : fileTracks)
			if (!track.index01.has_value())
				throw FlycastException(i18n::T("Invalid CUE file"));

		CueTrackLayout& firstTrack = fileTracks.front();
		firstTrack.index01ByteOffset = checkedMultiply(*firstTrack.index01, firstTrack.sectorSize);
		if (firstTrack.index00.has_value())
			firstTrack.index00ByteOffset = checkedMultiply(*firstTrack.index00, firstTrack.sectorSize);
		for (size_t i = 1; i < fileTracks.size(); i++) {
			CueTrackLayout& previous = fileTracks[i - 1];
			CueTrackLayout& track = fileTracks[i];
			const u32 boundary = track.index00.value_or(*track.index01);
			if (boundary < *previous.index01 || *track.index01 < boundary)
				throw FlycastException(i18n::T("Invalid or out-of-range INDEX in CUE file"));
			u64 offset = checkedAdd(previous.index01ByteOffset,
					checkedMultiply(boundary - *previous.index01, previous.sectorSize));
			track.index00ByteOffset = offset;
			if (track.index00.has_value())
				offset = checkedAdd(offset, checkedMultiply(*track.index01 - boundary, track.sectorSize));
			track.index01ByteOffset = offset;
		}

		const u64 fileSize = fileInfo.size;
		const CueTrackLayout& lastTrack = fileTracks.back();
		// Existing TOC-only CUE fixtures (and some placeholder images) have
		// empty FILEs. Keep their advertised INDEX positions; reads still fail
		// naturally if a caller tries to access those sectors.
		if (fileSize != 0 && lastTrack.index01ByteOffset > fileSize)
			throw FlycastException(i18n::T("INDEX is outside the CUE track file"));
		const u64 remainingBytes = fileSize == 0 ? 0 : fileSize - lastTrack.index01ByteOffset;
		if (remainingBytes % lastTrack.sectorSize != 0)
			WARN_LOG(GDROM, "Warning: Size of track %s is not multiple of sector size %d", track_filename.c_str(), lastTrack.sectorSize);
		u64 syntheticPregapFrames = 0;
		for (const CueTrackLayout& track : fileTracks)
			syntheticPregapFrames = checkedAdd(syntheticPregapFrames, track.pregap);
		const u64 fileFrames = checkedAdd(fileSize == 0 ? 0 : checkedAdd(*lastTrack.index01, remainingBytes / lastTrack.sectorSize),
				syntheticPregapFrames);
		const u64 fileEndFAD = checkedAdd(fileStartFAD, fileFrames);
		if (fileEndFAD == 0 || fileEndFAD > std::numeric_limits<u32>::max())
			throw FlycastException(i18n::T("CUE file layout exceeds the supported FAD range"));

		u64 priorSyntheticPregap = 0;
		for (CueTrackLayout& layout : fileTracks) {
			const u64 trackStartFAD = checkedAdd(fileStartFAD,
					checkedAdd(*layout.index01, checkedAdd(priorSyntheticPregap, layout.pregap)));
			const u64 storedPregapFrames = layout.index00.has_value() ? *layout.index01 - *layout.index00 : 0;
			const u64 readStartFAD = trackStartFAD - storedPregapFrames;
			const u64 trackEndFAD = fileEndFAD - 1;
			if (trackStartFAD > std::numeric_limits<u32>::max() || trackEndFAD > std::numeric_limits<u32>::max()
					|| readStartFAD > std::numeric_limits<u32>::max()
					|| layout.index01ByteOffset > (u64)std::numeric_limits<s64>::max()
					|| layout.index00ByteOffset > (u64)std::numeric_limits<s64>::max())
				throw FlycastException(i18n::T("CUE file layout exceeds the supported range"));

			Track track;
			track.StartFAD = (u32)trackStartFAD;
			track.ReadStartFAD = layout.index00.has_value() ? (u32)readStartFAD : 0;
			track.EndFAD = (u32)trackEndFAD;
			track.CTRL = (layout.type == "AUDIO" || layout.type == "CDG") ? 0 : 4;
			track.isrc = layout.isrc;
			DEBUG_LOG(GDROM, "file[%zd] \"%s\": session %d type %s FAD:%d -> %d %s", disc->tracks.size() + 1, track_filename.c_str(),
					session_number, layout.type.c_str(), track.StartFAD, track.EndFAD, track.isrc.empty() ? "" : ("ISRC " + track.isrc).c_str());
			hostfs::File *trackFile = hostfs::storage().openFile(track_filename, "rb");
			if (trackFile == nullptr)
				throw FlycastException(strprintf(i18n::T("CUE file: cannot open track %s"), track_filename.c_str()));
			const u64 byteOffset = layout.index00.has_value() ? layout.index00ByteOffset : layout.index01ByteOffset;
			const u32 firstFAD = layout.index00.has_value() ? track.ReadStartFAD : track.StartFAD;
			track.file = new RawTrackFile(trackFile, byteOffset, firstFAD, layout.sectorSize);
			disc->tracks.push_back(track);
			if (disc->tracks.size() >= 2) {
				Track& previous = disc->tracks[disc->tracks.size() - 2];
				const u32 pregapStartFAD = (u32)(readStartFAD - layout.pregap);
				previous.EndFAD = std::min(previous.EndFAD, pregapStartFAD - 1);
			}
			priorSyntheticPregap = checkedAdd(priorSyntheticPregap, layout.pregap);
		}
		currentFAD = fileEndFAD;
		fileTracks.clear();
	};

	std::string line;
	while (std::getline(istream, line))
	{
		std::istringstream cuesheet(line);
		cuesheet.imbue(std::locale::classic());
		std::string token;
		cuesheet >> token;

		if (token == "REM")
		{
			cuesheet >> token;
			if (token == "SESSION" || token == "HIGH-DENSITY" || token == "SINGLE-DENSITY")
				finalizeFile();
			if (token == "SESSION")
			{
				// Multi-session
				u32 cur_session;
				cuesheet >> cur_session;
				if (cur_session == 0)
					WARN_LOG(GDROM, "CUE parse error: invalid session number %s", token.c_str());
				else if (cur_session != session_number)
				{
					session_number = cur_session;
					if (session_number == 2) {
						// session 1 lead-out: 01:30:00, session 2 lead-in: 01:00:00, pregap: 00:02:00
						currentFAD = checkedAdd(currentFAD, 6750 + 4500 + 150);
						if (currentFAD > std::numeric_limits<u32>::max())
							throw FlycastException(i18n::T("CUE file layout exceeds the supported FAD range"));
						firstTrackOfSession = true;
					}

					Session ses;
					ses.FirstTrack = (u8)disc->tracks.size() + 1;
					ses.StartFAD = (u32)currentFAD;
					disc->sessions.push_back(ses);
					DEBUG_LOG(GDROM, "session[%zd]: 1st track: %d FAD:%d", disc->sessions.size(), ses.FirstTrack, ses.StartFAD);
				}
			}
			else
			{
				// GD-Rom
				if (token == "HIGH-DENSITY") {
					currentFAD = 45000 + 150;
					disc->type = GdRom;
				}
				else if (token != "SINGLE-DENSITY")
				{
					INFO_LOG(GDROM, "CUE parse: unrecognized REM token %s. Expected SINGLE-DENSITY, HIGH-DENSITY or SESSION", token.c_str());
					continue;
				}
				cuesheet >> token;
				if (token != "AREA")
					WARN_LOG(GDROM, "CUE parse error: unrecognized REM token %s. Assuming AREA", token.c_str());
			}
			// Clear file context
			track_filename.clear();
			fileStartFAD = 0;
			fileTracks.clear();
			// Clear track context
			track_number = -1;
			track_type.clear();
			track_isrc.clear();
			track_secsize = 0;
		}
		else if (token == "FILE")
		{
			finalizeFile();
			track_filename.clear();
			char last;
			do {
				if (!(cuesheet >> last))
					throw FlycastException(i18n::T("Invalid CUE file"));
			} while (isspace((unsigned char)last));

			if (last == '"')
			{
				cuesheet >> std::noskipws;
				for (;;) {
					if (!(cuesheet >> last))
						throw FlycastException(i18n::T("Invalid CUE file"));
					if (last == '"')
						break;
					track_filename += last;
				}
				cuesheet >> std::skipws;
			}
			else
			{
				cuesheet >> track_filename;
				track_filename = last + track_filename;
			}
			cuesheet >> token;	// BINARY
			if (token != "BINARY") {
				WARN_LOG(GDROM, "CUE parse error: unsupported FILE format %s. Expected BINARY", token.c_str());
				throw FlycastException(i18n::T("Invalid CUE file"));
			}
			track_filename = hostfs::storage().getSubPath(basepath, track_filename);
			hostfs::File *track_file = hostfs::storage().openFile(track_filename, "rb");
			if (track_file == nullptr)
				throw FlycastException(strprintf(i18n::T("CUE file: cannot open track %s"), track_filename.c_str()));
			if (digest != nullptr)
				md5.add(track_file);
			delete track_file;
			fileInfo = hostfs::storage().getFileInfo(track_filename);
			fileStartFAD = currentFAD;
			fileTracks.clear();
			// Clear track context
			track_number = -1;
			track_type.clear();
			track_isrc.clear();
			track_secsize = 0;
		}
		else if (token == "PREGAP")
		{
			cuesheet >> token;
			try {
				pregap = parseCueIndex(token);
			}
			catch (const FlycastException&) {
				throw FlycastException(i18n::T("Invalid PREGAP in CUE file"));
			}
		}
		else if (token == "TRACK")
		{
			cuesheet >> track_number;
			cuesheet >> track_type;
			if (track_filename.empty()) {
				WARN_LOG(GDROM, "CUE parse error: TRACK %02d not in a FILE context", track_number);
				throw FlycastException(i18n::T("Invalid CUE file"));
			}
			if (track_number <= 0 || track_number > 99) {
				WARN_LOG(GDROM, "CUE parse error: Invalid track number %d", track_number);
				throw FlycastException(i18n::T("Invalid CUE file"));
			}
			track_secsize = getSectorSize(track_type);
			if (track_secsize == 0)
				throw FlycastException(strprintf(i18n::T("CUE file: track has unknown sector type: %s"), track_type.c_str()));
			CueTrackLayout layout;
			layout.sectorSize = track_secsize;
			layout.type = track_type;
			layout.isrc = track_isrc;
			fileTracks.push_back(layout);
		}
		else if (token == "INDEX")
		{
			u32 index_num = std::numeric_limits<u32>::max();
			if (!(cuesheet >> index_num) || !(cuesheet >> token))
				throw FlycastException(i18n::T("Invalid INDEX in CUE file"));
			if (fileTracks.empty())
				throw FlycastException(i18n::T("INDEX without TRACK in CUE file"));
			CueTrackLayout& layout = fileTracks.back();
			const u32 indexFAD = parseCueIndex(token);
			if (index_num == 0)
			{
				if (layout.index00.has_value() || layout.index01.has_value())
					throw FlycastException(i18n::T("Invalid INDEX 00 in CUE file"));
				layout.index00 = indexFAD;
			}
			else if (index_num == 1)
			{
				if (layout.index01.has_value() || (layout.index00.has_value() && indexFAD < *layout.index00))
					throw FlycastException(i18n::T("Invalid INDEX 01 in CUE file"));
				if (firstTrackOfSession && indexFAD > 0) {
					// The session gap above already includes this track's pregap so don't
					// count the sectors stored before INDEX 01 twice.
					const u64 sessionPregap = std::min(indexFAD, 150u);
					if (fileStartFAD < sessionPregap || currentFAD < sessionPregap)
						throw FlycastException(i18n::T("Invalid session pregap in CUE file"));
					fileStartFAD -= sessionPregap;
					currentFAD -= sessionPregap;
				}
				firstTrackOfSession = false;
				layout.index01 = indexFAD;
				layout.pregap = pregap;
				pregap = 0;
			}
			else {
				WARN_LOG(GDROM, "INDEX %02d not supported", index_num);
				throw FlycastException(i18n::T("Invalid CUE file"));
			}
		}
		else if (token == "CATALOG")
		{
			cuesheet >> disc->catalog;
		}
		else if (token == "ISRC")
		{
			cuesheet >> track_isrc;
			if (!fileTracks.empty() && !fileTracks.back().index01.has_value())
				fileTracks.back().isrc = track_isrc;
		}
	}
	finalizeFile();
	if (disc->tracks.empty())
		throw FlycastException(i18n::Ts("CUE parse error: failed to parse or invalid file with 0 tracks"));
	if (disc->tracks.size() > 99) {
		WARN_LOG(GDROM, "CUE: more than 99 tracks");
		throw FlycastException(i18n::Ts("Invalid CUE file"));
	}

	if (session_number == 0)
	{
		if (disc->type == GdRom)
		{
			// GD-Rom
			if (disc->tracks.size() < 3)
				throw FlycastException(i18n::Ts("CUE parse error: less than 3 tracks"));
			if (!disc->tracks[0].isDataTrack()) {
				WARN_LOG(GDROM, "CUE: track 1 must be a data track");
				throw FlycastException(i18n::Ts("Invalid CUE file"));
			}
			if (disc->tracks[1].isDataTrack()) {
				WARN_LOG(GDROM, "CUE: track 2 must be an audio track");
				throw FlycastException(i18n::Ts("Invalid CUE file"));
			}
			if (!disc->tracks[2].isDataTrack()) {
				WARN_LOG(GDROM, "CUE: track 3 must be a data track");
				throw FlycastException(i18n::Ts("Invalid CUE file"));
			}
			if (disc->tracks[2].StartFAD != 45150) {
				WARN_LOG(GDROM, "CUE: track 3 must start at FAD 45000+150, not %d", disc->tracks[2].StartFAD);
				throw FlycastException(i18n::Ts("Invalid CUE file"));
			}
			disc->FillGDSession();
		}
		else
		{
			// CD-Rom
			Session ses;
			ses.FirstTrack = 1;
			ses.StartFAD = disc->tracks[0].StartFAD;
			disc->sessions.push_back(ses);
		}
	}
	else {
		// CD-Rom XA
		disc->type = CdRom_XA;
	}
	if (disc->type != GdRom)
	{
		disc->LeadOut.ADR = 1;
		disc->LeadOut.CTRL = 4;
		disc->EndFAD = disc->LeadOut.StartFAD = currentFAD;
	}

	if (digest != nullptr)
		*digest = md5.getDigest();

	return disc.release();
}
