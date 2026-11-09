// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#include "card_reader.h"
#include "state_io.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#ifdef __linux__
#include <fcntl.h>
#include <sys/syscall.h>
#endif
#endif

namespace hikaru {
namespace {
namespace fs = std::filesystem;
constexpr uint32_t StateExtension = 0x435244f2;
constexpr uint32_t StateVersion = 1;
uint8_t checksum(const uint8_t* begin, const uint8_t* end) {
    uint8_t result = 0;
    while (begin != end) result ^= *begin++;
    return result;
}
bool cardName(const std::string& name) {
    // Save-state and UI filenames are basenames, never paths supplied by a card.
    return !name.empty() && name.size() <= 128 && name != "." && name != ".."
        && name.find_first_of("/\\:") == std::string::npos
        && name.find('\0') == std::string::npos
        && fs::u8path(name).extension() == ".card";
}
unsigned trackMask(uint8_t selector) {
    constexpr unsigned masks[] = {1, 2, 4, 3, 5, 6, 7};
    return selector >= '0' && selector <= '6' ? masks[selector - '0'] : 0;
}
uint8_t evenParity(uint8_t byte) {
    byte &= 0x7f;
    uint8_t folded = byte;
    folded ^= folded >> 4;
    folded ^= folded >> 2;
    folded ^= folded >> 1;
    return byte | ((folded & 1) << 7);
}
std::vector<uint8_t> readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open card file");
    const auto size = file.tellg();
    if (size != 207 && size != 216) throw std::runtime_error("Invalid card file size (expected 207 or 216 bytes)");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("Cannot read card file");
    return bytes;
}
void atomicWrite(const fs::path& path, const uint8_t* bytes, size_t size, bool replace) {
    // Write beside the destination so publication stays on one filesystem.
    // A failed write leaves the original card intact and is reported to the player.
    static std::atomic<unsigned> sequence{0};
#ifdef _WIN32
    const unsigned pid = GetCurrentProcessId();
#else
    const unsigned pid = unsigned(getpid());
#endif
    fs::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(pid) + "-" + std::to_string(sequence++);
    try {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file || !file.write(reinterpret_cast<const char*>(bytes), size))
            throw std::runtime_error("Cannot write card file");
        file.close();
        if (!file) throw std::runtime_error("Cannot finish card file");
        if (replace && fs::exists(path)) {
            auto backup = path;
            backup += ".bak";
            fs::copy_file(path, backup, fs::copy_options::overwrite_existing);
        }
#ifdef _WIN32
        const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!MoveFileExW(temporary.c_str(), path.c_str(), flags))
            throw std::system_error(GetLastError(), std::system_category(), "Cannot publish card file");
#else
        if (replace) fs::rename(temporary, path);
        else {
            // Publish exclusively, including on Android's emulated storage where
            // hard links are unavailable. Older kernels retain the link fallback.
#if defined(__linux__) && defined(SYS_renameat2)
            if (syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD, path.c_str(), 1 /* RENAME_NOREPLACE */) == 0)
                return;
            if (errno != ENOSYS && errno != EINVAL)
                throw std::system_error(errno, std::generic_category(), "Cannot publish card file");
#endif
            fs::create_hard_link(temporary, path);
            fs::remove(temporary);
        }
#endif
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}
void writeVector(StateWriter& state, const std::vector<uint8_t>& bytes) {
    state.value(uint32_t(bytes.size()));
    state.bytes(bytes.data(), bytes.size());
}
std::vector<uint8_t> readVector(StateReader& state, size_t maximum) {
    uint32_t count;
    state.value(count);
    StateReader::require(count <= maximum, "Invalid Hikaru card buffer size");
    const auto* bytes = state.view(count);
    return count ? std::vector<uint8_t>(bytes, bytes + count) : std::vector<uint8_t>{};
}
void writeString(StateWriter& state, const std::string& text) {
    state.value(uint32_t(text.size()));
    state.bytes(text.data(), text.size());
}
std::string readString(StateReader& state) {
    const auto bytes = readVector(state, 128);
    return std::string(bytes.begin(), bytes.end());
}
}

CardReader::CardReader(std::string directory, std::string legacyPath) : directory_(std::move(directory)) {
    try {
        const auto folder = fs::u8path(directory_);
        fs::create_directories(folder);
        // Import an existing Hollycast card once, without modifying its original.
        if (!legacyPath.empty() && fs::exists(fs::u8path(legacyPath)) && !fs::exists(folder / "Legacy.card")) {
            const auto bytes = readFile(fs::u8path(legacyPath));
            atomicWrite(folder / "Legacy.card", bytes.data(), bytes.size(), false);
        }
        std::ifstream selection(folder / ".selected", std::ios::binary);
        if (selection.is_open()) {
            // An empty saved selection means None, including after importing a
            // legacy card. Never silently substitute another pilot on restart.
            std::array<char, 130> name{};
            selection.getline(name.data(), name.size());
            if (selection.fail() && !selection.eof())
                throw std::runtime_error("Invalid saved card selection");
            const std::string filename(name.data());
            if (!filename.empty()) {
                if (!cardName(filename)) throw std::runtime_error("Invalid saved card selection");
                readFile(folder / fs::u8path(filename));
                selected_ = filename;
            }
        } else if (fs::exists(folder / "Legacy.card")) selected_ = "Legacy.card";
    } catch (const std::exception& e) { error_ = e.what(); }
}
void CardReader::reset() {
    // Cabinet reset resets electronics, not the physical card or its saved data.
    input_.clear(); command_.clear(); reply_.clear(); waitingRead_.clear(); output_.clear();
    shutterOpen_ = false;
    ejected_ = false;
}
void CardReader::queue(const std::vector<uint8_t>& bytes) {
    if (bytes.size() > MaxOutput - output_.size()) {
        error_ = "Card reader response queue overflow";
        return;
    }
    output_.insert(output_.end(), bytes.begin(), bytes.end());
}
uint8_t CardReader::read() {
    if (output_.empty()) return 0;
    const auto byte = output_.front();
    output_.pop_front();
    return byte;
}
void CardReader::respond(uint8_t command, uint8_t error, uint8_t status, const std::vector<uint8_t>& data) {
    reply_ = {0x02, uint8_t(data.size() + 6), command,
        uint8_t(sensors_ | 0x20 | (shutterOpen_ ? 0x80 : 0x40)), error, status};
    reply_.insert(reply_.end(), data.begin(), data.end());
    reply_.push_back(0x03);
    reply_.push_back(checksum(reply_.data() + 1, reply_.data() + reply_.size()));
    queue(reply_);
}
void CardReader::write(uint8_t byte) {
    if (input_.empty()) {
        if (byte == 0x05 || byte == 0x15) {
            if (!command_.empty()) {
                auto command = std::move(command_);
                command_.clear();
                execute(command);
            } else if (!waitingRead_.empty()) readTracks(waitingRead_);
            else if (!reply_.empty()) {
                // Re-polling a completed command updates sensors; it must never
                // dispense, write, or eject the card a second time.
                reply_[3] = sensors_ | 0x20 | (shutterOpen_ ? 0x80 : 0x40);
                reply_[4] = '0';
                if (reply_[5] != '3') reply_[5] = '0';
                reply_.back() = checksum(reply_.data() + 1, reply_.data() + reply_.size() - 1);
                queue(reply_);
            }
            return;
        }
        if (byte == 0x06) return;
        if (byte != 0x02) { queue({0x15}); return; }
    }
    input_.push_back(byte);
    if (input_.size() == 2 && (byte < 6 || byte > 254)) {
        input_.clear(); queue({0x15}); return;
    }
    if (input_.size() < 2 || input_.size() < size_t(input_[1]) + 2) return;
    if (input_[input_.size() - 2] != 0x03 || checksum(input_.data() + 1, input_.data() + input_.size()) != 0) {
        input_.clear(); queue({0x15}); return;
    }
    command_.assign(input_.begin() + 2, input_.end() - 2);
    input_.clear();
    queue({0x06});
}
void CardReader::setTrack(unsigned track, const uint8_t* data, size_t count, bool parity) {
    auto* raw = card_.data() + track * TrackSize;
    raw[0] = raw[70] = 0xff;
    for (size_t i = 0; i < TrackDataSize; ++i) {
        const auto byte = i < count ? data[i] : uint8_t(0);
        raw[i + 1] = parity ? evenParity(byte) : byte;
    }
    raw[71] = checksum(raw + 1, raw + 71);
}
void CardReader::blankCard() {
    // Stock from this cabinet has a recorded, zero-filled first track; the
    // other two are unrecorded. The game checks those partial-read statuses.
    card_.fill(0xff);
    setTrack(0, nullptr, 0, true);
    active_.clear(); dirty_ = true;
}
void CardReader::readTracks(const std::vector<uint8_t>& command) {
    const uint8_t code = command[0];
    if (command.size() < 7 || command[4] < '0' || command[4] > '2'
            || (command[5] != '0' && command[5] != '1') || !trackMask(command[6])) {
        waitingRead_.clear(); respond(code, '0', '2'); return;
    }
    if (ejected_ || (!sensors_ && !shutterOpen_)) {
        waitingRead_.clear(); respond(code, '0', '2'); return;
    }
    if (!sensors_) {
        waitingRead_ = command;
        respond(code, '0', '4'); return;
    }
    const unsigned mask = trackMask(command[6]);
    unsigned bad = 0;
    std::vector<uint8_t> data;
    for (unsigned track = 0; track < 3; ++track) {
        if (!(mask & (1 << track))) continue;
        const auto* raw = card_.data() + track * TrackSize;
        const bool valid = raw[0] == 0xff && raw[70] == 0xff
            && checksum(raw + 1, raw + 72) == 0;
        if (!valid) bad |= 1 << track;
        for (unsigned i = 1; i <= TrackDataSize; ++i)
            data.push_back(valid ? (raw[i] & (command[5] == '0' ? 0x7f : 0xff)) : 0);
    }
    waitingRead_.clear();
    sensors_ = bad == mask ? 0x03 : 0x18;
    uint8_t error = '0';
    if (bad == mask) { error = '1'; data.clear(); }
    else if (bad) {
        constexpr uint8_t errors[] = {'0', 'Q', 'R', 'T', 'S', 'U', 'V'};
        error = errors[bad];
    }
    respond(code, error, '0', data);
}
void CardReader::execute(const std::vector<uint8_t>& command) {
    const auto code = command[0];
    if (!waitingRead_.empty()) {
        if (code == 0x20) { readTracks(waitingRead_); return; }
        if (code == 0x40 || code == 0x10 || code == 0x1c || code == 0xe0 || code == 0xff)
            waitingRead_.clear();
        else return;
    } else if (code == 0x40) { respond(code, '0', '2'); return; }
    const auto param = command.size() > 4 ? command[4] : uint8_t('0');
    switch (code) {
    case 0xff: reset(); return;
    case 0x10: case 0x1c: case 0xe0:
        ejected_ = false;
        if (param == '1' && sensors_ && shutterOpen_) { sensors_ = 1; ejected_ = true; }
        respond(code); return;
    case 0x20:
        if (!sensors_) ejected_ = false;
        respond(code); return;
    case 0x40: respond(code); return;
    case 0xd0: {
        const bool open = param == '1';
        if (open == shutterOpen_) respond(code, '0', '2');
        else { shutterOpen_ = open; respond(code); }
        return;
    }
    case 0xb0:
        if (sensors_ && sensors_ != 0x10) { respond(code, '0', '2'); return; }
        blankCard(); sensors_ = 0x1c; ejected_ = false; respond(code); return;
    case 0x33: readTracks(command); return;
    case 0x53: {
        const unsigned mask = command.size() >= 7 ? trackMask(command[6]) : 0;
        if (sensors_ <= 1 || !mask || command[4] != '0' || (command[5] != '0' && command[5] != '1')) {
            respond(code, '0', '2'); return;
        }
        if (command[5] == '0' && std::any_of(command.begin() + 7, command.end(), [](uint8_t b) { return b >= 0x80; })) {
            respond(code, '2'); return;
        }
        size_t offset = 7;
        for (unsigned track = 0; track < 3; ++track) {
            if (!(mask & (1 << track))) continue;
            const size_t count = std::min(TrackDataSize, command.size() - std::min(command.size(), offset));
            setTrack(track, count ? command.data() + offset : nullptr, count, command[5] == '0');
            offset += TrackDataSize;
        }
        dirty_ = true; sensors_ = 0x18;
        if (!active_.empty() && !persist()) respond(code, '2');
        else respond(code);
        return;
    }
    case 0x80:
        if (!sensors_ || !shutterOpen_) { respond(code, '0', '2'); return; }
        sensors_ = 1; ejected_ = true; respond(code); return;
    case 0x7c: case 0x7d:
        // Thermal artwork is not needed to preserve the magnetic pilot record.
        respond(code, '0', sensors_ > 1 ? '0' : '2'); return;
    case 0x78: case 0x7a: case 0xc1: respond(code); return;
    case 0xa0: respond(code, '0', shutterOpen_ ? '0' : '2'); return;
    default: respond(code, '0', '2'); return;
    }
}
CardReaderInfo CardReader::info() const {
    CardReaderInfo result;
    result.location = sensors_ == 1 ? CardLocation::Slot : sensors_ ? CardLocation::Inside
        : selected_.empty() ? CardLocation::None : CardLocation::Hand;
    result.shutterOpen = shutterOpen_;
    result.selected = selected_;
    result.error = error_;
    std::error_code error;
    for (fs::directory_iterator it(fs::u8path(directory_), error), end; !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error)) {
            const auto name = it->path().filename().u8string();
            if (cardName(name)) result.cards.push_back(name);
        }
    }
    if (error && result.error.empty()) result.error = "Cannot list saved cards";
    std::sort(result.cards.begin(), result.cards.end());
    return result;
}
bool CardReader::rememberSelection() {
    try {
        atomicWrite(fs::u8path(directory_) / ".selected",
            reinterpret_cast<const uint8_t*>(selected_.data()), selected_.size(), true);
        return true;
    } catch (const std::exception& e) { error_ = e.what(); return false; }
}
bool CardReader::select(const std::string& filename) {
    if (sensors_) { error_ = "Take the card before selecting another"; return false; }
    if (!filename.empty() && !cardName(filename)) { error_ = "Invalid card filename"; return false; }
    try {
        if (!filename.empty()) readFile(fs::u8path(directory_) / fs::u8path(filename));
        const auto previous = selected_;
        selected_ = filename;
        if (!rememberSelection()) { selected_ = previous; return false; }
        error_.clear(); return true;
    } catch (const std::exception& e) { error_ = e.what(); return false; }
}
bool CardReader::persist() {
    try {
        if (active_.empty()) {
            unsigned index = 1;
            const auto folder = fs::u8path(directory_);
            do { active_ = "Card" + std::to_string(index++) + ".card"; }
            while (fs::exists(folder / fs::u8path(active_)));
            try { atomicWrite(folder / fs::u8path(active_), card_.data(), card_.size(), false); }
            catch (...) { active_.clear(); throw; }
        } else if (dirty_) atomicWrite(fs::u8path(directory_) / fs::u8path(active_), card_.data(), card_.size(), true);
        dirty_ = false; error_.clear(); return true;
    } catch (const std::exception& e) { error_ = e.what(); return false; }
}
bool CardReader::cardAction() {
    if (sensors_ == 1) {
        if (!persist()) return false;
        const auto previous = selected_;
        selected_ = active_;
        if (!rememberSelection()) { selected_ = previous; return false; }
        active_.clear(); sensors_ = 0; error_.clear(); return true;
    }
    if (sensors_ || !shutterOpen_ || selected_.empty()) return false;
    try {
        const auto bytes = readFile(fs::u8path(directory_) / fs::u8path(selected_));
        if (bytes.size() == card_.size()) std::copy(bytes.begin(), bytes.end(), card_.begin());
        else for (unsigned track = 0; track < 3; ++track)
            setTrack(track, bytes.data() + track * TrackDataSize, TrackDataSize, false);
        active_ = selected_; dirty_ = false; sensors_ = 1; error_.clear();
        return true;
    } catch (const std::exception& e) { error_ = e.what(); return false; }
}
void CardReader::saveState(StateWriter& state) const {
    // Retain the old Sanwa prefix so existing VON4 states remain readable. The
    // tagged extension is before coin state and exceeds the maximum coin count.
    state.value(true);
    state.value(sensors_ != 0);
    state.value(uint32_t(output_.size()));
    for (auto byte : output_) state.value(byte);
    writeVector(state, input_);
    std::array<uint8_t, 256> oldCommand{};
    std::copy(command_.begin(), command_.end(), oldCommand.begin());
    state.value(oldCommand);
    state.value(uint32_t(command_.size()));
    for (unsigned track = 0; track < 3; ++track) state.bytes(card_.data() + track * TrackSize + 1, TrackDataSize);
    state.value(shutterOpen_);
    state.value(StateExtension); state.value(StateVersion);
    state.value(card_); state.value(sensors_); state.value(ejected_); state.value(dirty_);
    writeVector(state, reply_); writeVector(state, waitingRead_);
    writeString(state, selected_); writeString(state, active_);
}
void CardReader::loadState(StateReader& source) {
    // Parse into a copy and commit only after validation. Loading a snapshot
    // never writes a card; legacy snapshots without a filename become recovered
    // cards, preventing them from silently overwriting an unrelated pilot.
    auto staged = *this;
    auto state = source;
    bool sanwa, inserted;
    state.value(sanwa); state.value(inserted);
    StateReader::require(sanwa, "Incompatible Hikaru card reader");
    const auto output = readVector(state, 1024 * 1024);
    staged.output_.assign(output.begin(), output.end());
    staged.input_ = readVector(state, 255);
    std::array<uint8_t, 256> command{};
    uint32_t count;
    state.value(command); state.value(count);
    StateReader::require(count <= command.size(), "Invalid Hikaru card command");
    staged.command_.assign(command.begin(), command.begin() + count);
    for (unsigned track = 0; track < 3; ++track) {
        std::array<uint8_t, TrackDataSize> bytes;
        state.value(bytes);
        staged.setTrack(track, bytes.data(), bytes.size(), false);
    }
    state.value(staged.shutterOpen_);
    staged.sensors_ = inserted ? 0x18 : 0;
    staged.ejected_ = false; staged.dirty_ = inserted;
    staged.active_.clear(); staged.reply_.clear(); staged.waitingRead_.clear();
    bool extended = false;
    if (state.remaining() >= sizeof(uint32_t)) {
        auto probe = state;
        uint32_t marker; probe.value(marker);
        if (marker == StateExtension) {
            extended = true;
            state = probe;
            uint32_t version; state.value(version);
            StateReader::require(version == StateVersion, "Unsupported Hikaru card state");
            state.value(staged.card_); state.value(staged.sensors_);
            state.value(staged.ejected_); state.value(staged.dirty_);
            staged.reply_ = readVector(state, 256);
            staged.waitingRead_ = readVector(state, 254);
            staged.selected_ = readString(state); staged.active_ = readString(state);
        }
    }
    // The legacy parser retained ETX in its pending command; the native parser
    // excludes framing. Remove it only for legacy payloads, never magnetic data.
    if (!extended && staged.command_.size() > 1 && staged.command_.back() == 3)
        staged.command_.pop_back();
    StateReader::require(staged.output_.size() <= MaxOutput && staged.command_.size() <= 252
        && (staged.sensors_ == 0 || staged.sensors_ == 1 || staged.sensors_ == 3
            || staged.sensors_ == 0x18 || staged.sensors_ == 0x1c)
        && (staged.selected_.empty() || cardName(staged.selected_))
        && (staged.active_.empty() || cardName(staged.active_)), "Invalid Hikaru card state");
    StateReader::require(staged.input_.empty() || (staged.input_[0] == 2
        && (staged.input_.size() < 2 || (staged.input_[1] >= 6 && staged.input_[1] <= 254
            && staged.input_.size() < size_t(staged.input_[1]) + 2))), "Invalid partial card packet");
    StateReader::require(staged.reply_.empty() || (staged.reply_.size() >= 8
        && staged.reply_[0] == 2 && size_t(staged.reply_[1]) + 2 == staged.reply_.size()
        && staged.reply_[staged.reply_.size() - 2] == 3
        && checksum(staged.reply_.data() + 1, staged.reply_.data() + staged.reply_.size()) == 0), "Invalid card reply");
    StateReader::require(staged.waitingRead_.empty() || (staged.waitingRead_.size() >= 7
        && staged.waitingRead_[0] == 0x33), "Invalid pending card read");
    StateReader::require(!extended || inserted == (staged.sensors_ != 0), "Inconsistent card sensor state");
    staged.error_.clear();
    *this = std::move(staged);
    source = state;
}
} // namespace hikaru
