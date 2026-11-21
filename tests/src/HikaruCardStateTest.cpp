// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "hw/hikaru/state_io.h"
#include "hw/hikaru/card_reader.h"
#include "hw/hikaru/roms.h"
#include "hw/hikaru/jvs.h"
#include "hw/naomi/card_reader.h"

#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <vector>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

namespace {
void writeBytes(SerialPort::Pipe& pipe, std::initializer_list<uint8_t> bytes)
{
    for (uint8_t byte : bytes)
        pipe.write(byte);
}

std::vector<uint8_t> drain(SerialPort::Pipe& pipe)
{
    std::vector<uint8_t> bytes;
    for (int count = pipe.available(); count > 0; --count)
        bytes.push_back(pipe.read());
    return bytes;
}

void sendNewCard(SerialPort::Pipe& pipe)
{
    // CRP-1231 command packet: STX, length, CARD_NEW, XOR checksum.
    writeBytes(pipe, {0x02, 0x02, 0xb0, 0xb2});
}
}

TEST(HikaruCardStateTest, SanwaRoundTripRestoresQueuedOutputAndPartialPacket)
{
    auto original = card_reader::createSanwaReader();
    sendNewCard(*original);
    original->write(0x05); // Process the queued CARD_NEW command.
    writeBytes(*original, {0x02, 0x02, 0xb0}); // Await this packet's checksum.

    hikaru::StateWriter saved;
    card_reader::saveSanwaReaderState(*original, saved);
    auto resumed = card_reader::createSanwaReader();
    hikaru::StateReader reader(saved.data());
    card_reader::loadSanwaReaderState(*resumed, reader);
    reader.finish();

    EXPECT_GT(original->available(), 0);
    EXPECT_EQ(original->available(), resumed->available());
    EXPECT_EQ(drain(*original), drain(*resumed));

    original->write(0xb2);
    resumed->write(0xb2);
    original->write(0x05);
    resumed->write(0x05);
    EXPECT_EQ(drain(*original), drain(*resumed));
}

TEST(HikaruCardStateTest, RejectedSanwaStatePreservesQueuesAndParser)
{
    auto readerPipe = card_reader::createSanwaReader();
    sendNewCard(*readerPipe);
    readerPipe->write(0x05);
    writeBytes(*readerPipe, {0x02, 0x02, 0xb0});

    hikaru::StateWriter saved, before, after;
    card_reader::saveSanwaReaderState(*readerPipe, saved);
    card_reader::saveSanwaReaderState(*readerPipe, before);
    auto truncated = saved.data();
    truncated.pop_back();

    EXPECT_THROW({
        hikaru::StateReader broken(truncated);
        card_reader::loadSanwaReaderState(*readerPipe, broken);
    }, std::runtime_error);

    card_reader::saveSanwaReaderState(*readerPipe, after);
    EXPECT_EQ(before.data(), after.data());
}


namespace {
namespace fs = std::filesystem;
class HikaruNativeCardTest : public ::testing::Test {
protected:
    fs::path folder;
    std::unique_ptr<hikaru::CardReader> device;
    void SetUp() override {
        folder = fs::temp_directory_path() / ("hollycast-card-test-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        device = std::make_unique<hikaru::CardReader>(folder.u8string());
    }
    void TearDown() override {
        device.reset();
        std::error_code ignored;
        fs::remove_all(folder, ignored);
    }
    static std::vector<uint8_t> packet(uint8_t code, const std::vector<uint8_t>& params = {}) {
        std::vector<uint8_t> bytes{2, uint8_t(params.size() + 6), code, 0, 0, 0};
        bytes.insert(bytes.end(), params.begin(), params.end());
        bytes.push_back(3);
        uint8_t check = 0;
        for (size_t i = 1; i < bytes.size(); ++i) check ^= bytes[i];
        bytes.push_back(check);
        return bytes;
    }
    static std::vector<uint8_t> drain(hikaru::CardReader& reader) {
        std::vector<uint8_t> result;
        while (reader.available()) result.push_back(reader.read());
        return result;
    }
    static std::vector<uint8_t> command(hikaru::CardReader& reader, uint8_t code,
                                        const std::vector<uint8_t>& params = {}) {
        for (auto byte : packet(code, params)) reader.write(byte);
        EXPECT_EQ(drain(reader), std::vector<uint8_t>{6});
        reader.write(5);
        auto reply = drain(reader);
        EXPECT_GE(reply.size(), 8u);
        if (reply.size() < 8) throw std::runtime_error("Missing native card response");
        if (reply.size() >= 8) {
            EXPECT_EQ(reply[2], code);
            uint8_t check = 0;
            for (size_t i = 1; i < reply.size(); ++i) check ^= reply[i];
            EXPECT_EQ(check, 0);
        }
        return reply;
    }
    std::vector<uint8_t> command(uint8_t code, const std::vector<uint8_t>& params = {}) {
        return command(*device, code, params);
    }
    void takeIssuedCard() {
        command(0xb0);
        command(0xd0, {'1'});
        command(0x80);
        ASSERT_TRUE(device->cardAction());
    }
    static std::vector<uint8_t> data(const std::vector<uint8_t>& response) {
        return response.size() >= 8 ? std::vector<uint8_t>(response.begin() + 6, response.end() - 2)
                                    : std::vector<uint8_t>{};
    }
};
}

TEST_F(HikaruNativeCardTest, NativeIssueWriteEjectAndReinsertPreservesPilotRecord)
{
    auto issued = command(0xb0);
    ASSERT_EQ(issued[3] & 0x1f, 0x1c);
    const auto blank = command(0x33, {'0', '0', '6'});
    ASSERT_EQ(blank[4], 'V'); // The stock card's second and third tracks are blank.
    ASSERT_EQ(data(blank), std::vector<uint8_t>(207, 0));
    std::vector<uint8_t> payload(207);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i ^ 0xa5);
    std::vector<uint8_t> params{'0', '1', '6'};
    params.insert(params.end(), payload.begin(), payload.end());
    ASSERT_EQ(command(0x53, params)[4], '0');
    ASSERT_EQ(data(command(0x33, {'0', '1', '6'})), payload);
    command(0xd0, {'1'});
    command(0x80);
    EXPECT_EQ(device->info().location, hikaru::CardLocation::Slot);
    EXPECT_TRUE(device->info().cards.empty()); // Ejection still needs player removal.
    ASSERT_TRUE(device->cardAction());
    ASSERT_EQ(fs::file_size(folder / "Card1.card"), 216u);
    hikaru::CardReader restarted(folder.u8string());
    ASSERT_EQ(restarted.info().selected, "Card1.card");
    command(restarted, 0xd0, {'1'});
    ASSERT_EQ(command(restarted, 0x33, {'0', '1', '6'})[5], '4');
    ASSERT_TRUE(restarted.cardAction());
    restarted.write(5);
    ASSERT_EQ(data(drain(restarted)), payload);
    restarted.write(5);
    EXPECT_EQ(data(drain(restarted)), payload); // ENQ resend must retain the track data.
}

TEST_F(HikaruNativeCardTest, ShutterAndPhysicalRemovalGateTransport)
{
    takeIssuedCard();
    EXPECT_EQ(command(0x33, {'0', '0', '0'})[5], '2'); // Ejection latch needs a no-card status.
    command(0x20);
    ASSERT_TRUE(device->cardAction());
    command(0xd0, {'0'});
    EXPECT_EQ(command(0x80)[5], '2');
    EXPECT_FALSE(device->select(""));
    command(0xd0, {'1'});
    command(0x33, {'0', '0', '0'});
    command(0x80);
    ASSERT_TRUE(device->cardAction());
    command(0xd0, {'0'});
    EXPECT_FALSE(device->cardAction());
    EXPECT_EQ(device->info().location, hikaru::CardLocation::Hand);
}

TEST_F(HikaruNativeCardTest, WaitingReadCanBePolledOrCancelled)
{
    command(0xd0, {'1'});
    EXPECT_EQ(command(0x33, {'0', '0', '6'})[5], '4');
    // STATUS while a read is waiting reports the read's status, not a new operation.
    for (auto byte : packet(0x20)) device->write(byte);
    EXPECT_EQ(drain(*device), std::vector<uint8_t>{6});
    device->write(5);
    const auto waiting = drain(*device);
    ASSERT_GE(waiting.size(), 8u);
    EXPECT_EQ(waiting[2], 0x33);
    EXPECT_EQ(waiting[5], '4');
    EXPECT_EQ(command(0x40)[5], '0');
    EXPECT_EQ(command(0x40)[5], '2');
}

TEST_F(HikaruNativeCardTest, InvalidPacketsAreRejectedAndResendDoesNotDispenseAgain)
{
    auto broken = packet(0xb0);
    broken.back() ^= 1;
    for (auto byte : broken) device->write(byte);
    EXPECT_EQ(drain(*device), std::vector<uint8_t>{0x15});
    EXPECT_EQ(device->info().location, hikaru::CardLocation::None);
    auto response = command(0xb0);
    device->write(5);
    EXPECT_EQ(drain(*device), response);
    EXPECT_EQ(command(0xb0)[5], '2');
    for (int i = 0; i < 5000; ++i) device->write(0x99);
    EXPECT_LE(device->available(), 4096u);
    EXPECT_FALSE(device->info().error.empty());
}

TEST_F(HikaruNativeCardTest, SevenBitTracksHaveEvenParityAndWritesKeepBackup)
{
    takeIssuedCard();
    command(0x20);
    ASSERT_TRUE(device->cardAction());
    command(0x33, {'0', '0', '0'}); // Transport the inserted card to the magnetic head.
    std::vector<uint8_t> params{'0', '0', '0'};
    for (unsigned i = 0; i < 69; ++i) params.push_back(uint8_t(i));
    EXPECT_EQ(command(0x53, params)[4], '0');
    EXPECT_TRUE(fs::exists(folder / "Card1.card.bak"));
    const auto read = data(command(0x33, {'0', '0', '0'}));
    ASSERT_EQ(read, std::vector<uint8_t>(params.begin() + 3, params.end()));
    const auto raw = data(command(0x33, {'0', '1', '0'}));
    for (auto byte : raw) {
        unsigned ones = 0;
        for (unsigned bit = 0; bit < 8; ++bit) ones += (byte >> bit) & 1;
        EXPECT_EQ(ones % 2, 0u);
    }
    params[3] = 0x80;
    EXPECT_EQ(command(0x53, params)[4], '2');
    EXPECT_EQ(data(command(0x33, {'0', '0', '0'})), read);
}

TEST_F(HikaruNativeCardTest, NewCardsAndMalformedSelectionCannotOverwriteExistingPilot)
{
    takeIssuedCard();
    command(0xb0);
    command(0x80);
    ASSERT_TRUE(device->cardAction());
    EXPECT_EQ(device->info().cards, (std::vector<std::string>{"Card1.card", "Card2.card"}));
    EXPECT_EQ(device->info().selected, "Card2.card");
    { std::ofstream bad(folder / "Broken.card"); bad << "truncated"; }
    EXPECT_FALSE(device->select("Broken.card"));
    EXPECT_FALSE(device->select("../Card1.card"));
    EXPECT_EQ(device->info().selected, "Card2.card");
    ASSERT_TRUE(device->select("Card1.card"));
    device->reset();
    EXPECT_EQ(device->info().selected, "Card1.card");
}

TEST_F(HikaruNativeCardTest, UnwritableStorageDoesNotLoseEjectedCard)
{
    // A regular file in place of the card directory deterministically rejects IO.
    const auto blocked = folder / "blocked";
    { std::ofstream file(blocked); file << "not a directory"; }
    hikaru::CardReader unavailable(blocked.u8string());
    command(unavailable, 0xb0);
    command(unavailable, 0xd0, {'1'});
    command(unavailable, 0x80);
    EXPECT_FALSE(unavailable.cardAction());
    EXPECT_EQ(unavailable.info().location, hikaru::CardLocation::Slot);
    EXPECT_FALSE(unavailable.info().error.empty());
}

TEST_F(HikaruNativeCardTest, NativeSnapshotRestoresQueuesPartialPacketAndWaitingRead)
{
    command(0xd0, {'1'});
    command(0x33, {'0', '0', '6'});
    const auto status = packet(0x20);
    for (size_t i = 0; i < status.size() - 1; ++i) device->write(status[i]);
    device->write(status.back()); // ACK remains queued.
    const auto partial = packet(0x40);
    for (size_t i = 0; i < 3; ++i) device->write(partial[i]);
    hikaru::StateWriter saved;
    device->saveState(saved);
    hikaru::CardReader resumed(folder.u8string());
    hikaru::StateReader reader(saved.data());
    resumed.loadState(reader);
    reader.finish();
    EXPECT_EQ(drain(resumed), drain(*device));
    for (size_t i = 3; i < partial.size(); ++i) {
        resumed.write(partial[i]); device->write(partial[i]);
    }
    resumed.write(5); device->write(5);
    EXPECT_EQ(drain(resumed), drain(*device));
    hikaru::StateWriter before, after;
    resumed.saveState(before);
    auto truncated = saved.data(); truncated.pop_back();
    EXPECT_THROW({ hikaru::StateReader broken(truncated); resumed.loadState(broken); }, std::runtime_error);
    resumed.saveState(after);
    EXPECT_EQ(before.data(), after.data());
}

TEST_F(HikaruNativeCardTest, LegacySnapshotLeavesCoinStateAndRecoversCardWithoutOverwriting)
{
    auto legacy = card_reader::createSanwaReader();
    sendNewCard(*legacy);
    legacy->write(5);
    ::drain(*legacy);
    hikaru::StateWriter saved;
    card_reader::saveSanwaReaderState(*legacy, saved);
    hikaru::CoinPulseQueue coins;
    coins.enqueue(); coins.saveState(saved);
    hikaru::StateReader reader(saved.data());
    device->loadState(reader);
    hikaru::CoinPulseQueue resumed;
    resumed.loadState(reader);
    reader.finish();
    EXPECT_EQ(device->info().location, hikaru::CardLocation::Inside);
    EXPECT_TRUE(device->info().cards.empty());
    command(0xd0, {'1'});
    command(0x80);
    ASSERT_TRUE(device->cardAction());
    EXPECT_TRUE(fs::exists(folder / "Card1.card"));
}

TEST_F(HikaruNativeCardTest, LegacyCardFileIsImportedWithoutChangingOriginal)
{
    const auto legacyPath = folder / "old.card";
    std::vector<uint8_t> payload(207, 0x42);
    { std::ofstream file(legacyPath, std::ios::binary); file.write(reinterpret_cast<const char*>(payload.data()), payload.size()); }
    hikaru::CardReader imported((folder / "cards").u8string(), legacyPath.u8string());
    EXPECT_EQ(imported.info().selected, "Legacy.card");
    EXPECT_EQ(fs::file_size(legacyPath), 207u);
    command(imported, 0xd0, {'1'});
    ASSERT_TRUE(imported.cardAction());
    EXPECT_EQ(data(command(imported, 0x33, {'0', '1', '6'})), payload);
}

TEST(HikaruCardStateTest, OnlyCardEquippedCabinetsExposeReader)
{
    EXPECT_EQ(hikaru::VirtualOn4Game.cardReader, hikaru::CardReaderModel::SanwaCRP1231BR10);
    for (const auto* game : {&hikaru::AirTrixGame, &hikaru::BraveFirefightersGame,
            &hikaru::PlanetHarriersGame, &hikaru::SegaNascarGame, &hikaru::StarWarsRacerGame})
        EXPECT_EQ(game->cardReader, hikaru::CardReaderModel::None);
}


TEST_F(HikaruNativeCardTest, ExplicitNoCardSelectionSurvivesLegacyImportAndRestart)
{
    const auto legacyPath = folder / "old.card";
    const std::vector<uint8_t> payload(207, 0);
    { std::ofstream file(legacyPath, std::ios::binary); file.write(reinterpret_cast<const char*>(payload.data()), payload.size()); }
    const auto cards = folder / "cards";
    hikaru::CardReader imported(cards.u8string(), legacyPath.u8string());
    ASSERT_TRUE(imported.select(""));
    hikaru::CardReader restarted(cards.u8string(), legacyPath.u8string());
    EXPECT_TRUE(restarted.info().selected.empty());
    EXPECT_EQ(restarted.info().location, hikaru::CardLocation::None);
    EXPECT_EQ(restarted.info().cards, std::vector<std::string>{"Legacy.card"});
    // A damaged selection must not unexpectedly select a different pilot.
    { std::ofstream file(cards / ".selected"); file << std::string(4096, 'x'); }
    hikaru::CardReader damaged(cards.u8string());
    EXPECT_TRUE(damaged.info().selected.empty());
    EXPECT_FALSE(damaged.info().error.empty());
}
