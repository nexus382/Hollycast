// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "phone_vmu_backup.h"

#ifdef HAVE_PHONE_VMU
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <map>
#include <string_view>

namespace phone_vmu
{
namespace fs = std::filesystem;
namespace
{
constexpr std::string_view Marker = ".phone-backup-";

bool digits(std::string_view text)
{
	return text.find_first_not_of("0123456789") == text.npos;
}

std::string cardName(const fs::path& path)
{
	const std::string filename = path.filename().u8string();
	const auto marker = filename.rfind(Marker);
	if (marker == std::string::npos || marker == 0)
		return {};
	const std::string_view stamp = std::string_view(filename).substr(marker + Marker.size());
	if ((stamp.size() != 15 && stamp.size() != 22) || stamp[8] != '-'
			|| !digits(stamp.substr(0, 8)) || !digits(stamp.substr(9, 6)))
		return {};
	if (stamp.size() == 22 && (stamp[15] != '-' || !digits(stamp.substr(16))))
		return {};
	return filename.substr(0, marker);
}

template<typename Visitor>
void visitBackups(const fs::path& directory, Visitor visit, std::error_code& ec)
{
	for (fs::directory_iterator it(directory.empty() ? fs::path(".") : directory, ec), end;
			!ec && it != end; it.increment(ec))
	{
		const std::string name = cardName(it->path());
		std::error_code fileError;
		if (!name.empty() && it->is_regular_file(fileError))
			visit(name, it->path());
	}
}

template<typename Write>
std::error_code writeExclusive(const fs::path& path, Write write)
{
#ifdef _WIN32
	FILE *file = _wfopen(path.c_str(), L"wbx");
#else
	FILE *file = std::fopen(path.c_str(), "wbx");
#endif
	if (file == nullptr)
		return { errno, std::generic_category() };
	const bool written = write(file);
	if (std::fclose(file) == 0 && written)
		return {};
	std::error_code ignored;
	fs::remove(path, ignored);
	return std::make_error_code(std::errc::io_error);
}

std::error_code writeImage(const fs::path& path, const std::uint8_t *data, std::size_t size)
{
	return writeExclusive(path, [&](FILE *file) { return std::fwrite(data, 1, size, file) == size; });
}

std::error_code copyCard(const fs::path& source, const fs::path& destination)
{
	std::ifstream input(source, std::ios::binary);
	if (!input)
		return std::make_error_code(std::errc::io_error);
	return writeExclusive(destination, [&](FILE *file) {
		std::array<char, 8192> block;
		while (input)
		{
			input.read(block.data(), block.size());
			const auto count = static_cast<std::size_t>(input.gcount());
			if (std::fwrite(block.data(), 1, count, file) != count)
				return false;
		}
		return input.eof() && !input.bad();
	});
}

template<typename Write>
bool createBackup(const fs::path& card, std::time_t now, Write write)
{
	std::tm local {};
#ifdef _WIN32
	if (localtime_s(&local, &now) != 0)
#else
	if (localtime_r(&now, &local) == nullptr)
#endif
		return false;
	char stamp[16];
	if (std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local) == 0)
		return false;

	const std::string filename = card.filename().u8string();
	const std::string base = filename + std::string(Marker) + stamp;
	unsigned suffix = 0;
	std::error_code ec;
	visitBackups(card.parent_path(), [&](const std::string& name, const fs::path& path) {
		if (name != filename)
			return;
		const std::string found = path.filename().u8string();
		if (found.compare(0, base.size(), base) == 0)
		{
			const unsigned sequence = found == base ? 0 : std::stoul(found.substr(base.size() + 1));
			suffix = std::max(suffix, sequence + 1);
		}
	}, ec);
	if (ec)
		return false;
	for (; suffix <= 999999; ++suffix)
	{
		std::string name = base;
		if (suffix != 0)
		{
			const std::string number = std::to_string(suffix);
			name += "-" + std::string(6 - number.size(), '0') + number;
		}
		const fs::path path = card.parent_path() / fs::u8path(name);
		ec = write(path);
		if (ec == std::errc::file_exists)
			continue;
		return !ec;
	}
	return false;
}
}

std::vector<CardBackup> newestBackups(const std::vector<fs::path>& directories)
{
	std::map<fs::path, fs::path> latest;
	for (const fs::path& directory : directories)
	{
		std::error_code ec;
		visitBackups(directory, [&](const std::string& name, const fs::path& path) {
			auto& newest = latest[path.parent_path() / fs::u8path(name)];
			if (path > newest)
				newest = path;
		}, ec);
	}
	std::vector<CardBackup> result;
	for (const auto& [card, snapshot] : latest)
		result.push_back({card, snapshot});
	return result;
}

bool backupCard(const fs::path& card, const std::uint8_t *data, std::size_t size, std::time_t now)
{
	if (data == nullptr || size != CardSize)
		return false;
	if (!createBackup(card, now, [&](const fs::path& path) { return writeImage(path, data, size); }))
		return false;

	const std::string filename = card.filename().u8string();
	std::vector<fs::path> snapshots;
	std::error_code ec;
	visitBackups(card.parent_path(), [&](const std::string& name, const fs::path& path) {
		if (name == filename)
			snapshots.push_back(path);
	}, ec);
	if (!ec && snapshots.size() > 5)
	{
		std::sort(snapshots.begin(), snapshots.end());
		for (std::size_t i = 0; i < snapshots.size() - 5; ++i)
			if (!fs::remove(snapshots[i], ec) && ec)
				break;
	}
	return true;
}

bool restoreCard(const CardBackup& backup, std::time_t now)
{
	std::vector<std::uint8_t> data(CardSize);
	std::ifstream in(backup.snapshot, std::ios::binary);
	if (!in.read(reinterpret_cast<char *>(data.data()), data.size()) || in.peek() != std::ifstream::traits_type::eof() || in.bad())
		return false;
	fs::path temp = backup.card;
	temp += ".phone-restore.tmp";
	std::error_code ec = writeImage(temp, data.data(), data.size());
	for (unsigned suffix = 1; ec == std::errc::file_exists && suffix <= 999999; ++suffix)
	{
		temp = backup.card;
		temp += ".phone-restore.tmp-" + std::to_string(suffix);
		ec = writeImage(temp, data.data(), data.size());
	}
	if (ec)
		return false;
	if (fs::exists(backup.card, ec))
	{
		if (!createBackup(backup.card, now, [&](const fs::path& path) { return copyCard(backup.card, path); }))
			ec = std::make_error_code(std::errc::io_error);
	}
	if (!ec)
		fs::rename(temp, backup.card, ec);
	if (!ec)
		return true;
	fs::remove(temp, ec);
	return false;
}
}
#endif
