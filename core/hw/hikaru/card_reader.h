// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace hikaru {
class StateReader;
class StateWriter;
enum class CardLocation : uint8_t { None, Hand, Slot, Inside };
struct CardReaderInfo {
    CardLocation location = CardLocation::None;
    bool shutterOpen = false;
    std::string selected;
    std::string error;
    std::vector<std::string> cards;
};

// Sanwa CRP-1231BR-10 cabinet device. The game owns transport and issuance;
// the player only chooses, inserts and takes physical cards. No pilot data is
// invented here. The serial protocol and raw magnetic tracks are independent
// of both the renderer and Naomi's other reader models.
class CardReader {
public:
    explicit CardReader(std::string directory, std::string legacyPath = {});
    void write(uint8_t byte);
    uint8_t read();
    size_t available() const { return output_.size(); }
    void reset();
    CardReaderInfo info() const;
    bool select(const std::string& filename);
    bool cardAction();
    void saveState(StateWriter& writer) const;
    void loadState(StateReader& reader);
private:
    static constexpr size_t TrackSize = 72, TrackDataSize = 69;
    static constexpr size_t MaxOutput = 4096;
    std::string directory_, selected_, active_, error_;
    std::array<uint8_t, 216> card_{};
    std::vector<uint8_t> input_, command_, reply_, waitingRead_;
    std::deque<uint8_t> output_;
    uint8_t sensors_ = 0;
    bool shutterOpen_ = false, ejected_ = false;
    bool dirty_ = false;
    void queue(const std::vector<uint8_t>& bytes);
    void respond(uint8_t command, uint8_t error = '0', uint8_t status = '0',
                 const std::vector<uint8_t>& data = {});
    void execute(const std::vector<uint8_t>& command);
    void readTracks(const std::vector<uint8_t>& command);
    void blankCard();
    bool persist();
    bool rememberSelection();
    void setTrack(unsigned track, const uint8_t* data, size_t count, bool parity);
};
} // namespace hikaru
