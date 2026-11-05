// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "hw/hikaru/state_io.h"
#include "hw/naomi/card_reader.h"

#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <vector>

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
