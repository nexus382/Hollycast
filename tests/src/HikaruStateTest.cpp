// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "serialize.h"
#include "hw/hikaru/state_io.h"
#include <array>
#include <vector>

class HikaruStateTest : public ::testing::Test {
protected:
    decltype(settings.platform) previous = settings.platform;
    void SetUp() override { settings.platform.system = DC_PLATFORM_HIKARU; }
    void TearDown() override { settings.platform = previous; }
};

TEST_F(HikaruStateTest, PlatformTagRejectsAnotherMachine) {
    std::array<u8, 32> bytes{};
    Serializer writer(bytes.data(), bytes.size());
    const u32 marker = 0x12345678;
    writer << marker;
    Deserializer reader(bytes.data(), writer.size());
    u32 restored = 0;
    reader >> restored;
    EXPECT_EQ(marker, restored);
    EXPECT_EQ(0u, reader.remaining());
    settings.platform.system = DC_PLATFORM_NAOMI;
    EXPECT_THROW(Deserializer(bytes.data(), writer.size()), Deserializer::Exception);
}

TEST_F(HikaruStateTest, LegacyHeaderAndTruncatedHeaders) {
    settings.platform.system = DC_PLATFORM_NAOMI;
    const std::array<u32, 2> legacy{u32(Deserializer::V63), 0x12345678};
    Deserializer reader(legacy.data(), sizeof(legacy));
    u32 restored = 0;
    reader >> restored;
    EXPECT_EQ(legacy[1], restored);
    for (size_t size = 0; size < sizeof(u32); ++size)
        EXPECT_THROW(Deserializer(legacy.data(), size), Deserializer::Exception);
    const u32 current = Deserializer::Current;
    EXPECT_THROW(Deserializer(&current, sizeof(current)), Deserializer::Exception);
}

TEST_F(HikaruStateTest, PayloadMeasuresAndRejectsInvalidBounds) {
    hikaru::StateWriter measured(true), writer;
    const std::array<u16, 3> values{0, 0xffff, 1234};
    measured.value(values); measured.value(true);
    writer.value(values); writer.value(true);
    EXPECT_EQ(measured.size(), writer.size());
    EXPECT_TRUE(measured.data().empty());
    hikaru::StateReader reader(writer.data());
    std::array<u16, 3> restored{};
    bool flag = false;
    reader.value(restored); reader.value(flag); reader.finish();
    EXPECT_EQ(values, restored);
    EXPECT_TRUE(flag);
    EXPECT_THROW(reader.value(flag), std::runtime_error);
    const std::vector<u8> invalid{2};
    hikaru::StateReader invalidReader(invalid);
    EXPECT_THROW(invalidReader.value(flag), std::runtime_error);
    EXPECT_THROW(measured.append(hikaru::StateWriter::MaxSize), std::runtime_error);
}
