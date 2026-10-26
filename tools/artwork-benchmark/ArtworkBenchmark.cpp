#include "gtest/gtest.h"
#include "ui/boxart/boxart.h"
#include "ui/game_scanner.h"

#include <dlfcn.h>
#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {

struct IoCounts {
    uint64_t read_bytes, write_bytes, read_calls, write_calls, cue_opens, bin_opens;
};

using ResetIo = void (*)();
using StopIo = void (*)(IoCounts *);

struct Snapshot {
    std::chrono::steady_clock::time_point wall;
    rusage cpu{};
    rusage_info_v4 disk{};
    int diskStatus = -1;
};

Snapshot snapshot()
{
    Snapshot point;
    if (getrusage(RUSAGE_SELF, &point.cpu) != 0)
        throw std::runtime_error("getrusage failed");
    point.diskStatus = proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t *>(&point.disk));
    point.wall = std::chrono::steady_clock::now();
    return point;
}

double milliseconds(const timeval& value)
{
    return value.tv_sec * 1000.0 + value.tv_usec / 1000.0;
}

std::string productId(int index)
{
    char value[20];
    std::snprintf(value, sizeof(value), "BM-%06d", index);
    return value;
}

std::vector<GameMedia> library(const std::string& root, int count)
{
    std::vector<GameMedia> games;
    for (int index = 0; index < count; ++index) {
        char value[32];
        std::snprintf(value, sizeof(value), "game-%04d", index);
        GameMedia game;
        game.name = std::string("Benchmark ") + value;
        game.fileName = std::string(value) + ".cue";
        game.path = (std::filesystem::path(root) / game.fileName).string();
        games.push_back(std::move(game));
    }
    return games;
}

TEST(ArtworkBenchmark, Library)
{
    const char *libraryRoot = std::getenv("ARTWORK_BENCH_LIBRARY");
    const char *stateRoot = std::getenv("ARTWORK_BENCH_STATE");
    const char *outputPath = std::getenv("ARTWORK_BENCH_OUTPUT");
    if (!libraryRoot || !stateRoot || !outputPath)
        GTEST_SKIP() << "Run with the benchmark driver";
    const int count = std::getenv("ARTWORK_BENCH_COUNT") ? std::atoi(std::getenv("ARTWORK_BENCH_COUNT")) : 1000;
    const int repeats = std::getenv("ARTWORK_BENCH_REPEATS") ? std::atoi(std::getenv("ARTWORK_BENCH_REPEATS")) : 10;
    ASSERT_GT(count, 0);
    ASSERT_GT(repeats, 0);
    const auto resetIo = reinterpret_cast<ResetIo>(dlsym(RTLD_DEFAULT, "artwork_io_reset"));
    const auto stopIo = reinterpret_cast<StopIo>(dlsym(RTLD_DEFAULT, "artwork_io_stop"));
    const bool instrumented = resetIo && stopIo;
    if (std::getenv("ARTWORK_BENCH_REQUIRE_IO"))
        ASSERT_TRUE(instrumented) << "I/O interposer did not load";
    std::filesystem::create_directories(stateRoot);
    set_user_data_dir(stateRoot);
    config::FetchBoxart = false;
    config::BoxartSourceMode = static_cast<int>(BoxartSourceMode::ScrapedOnly);
    config::BoxartPath = "";
    const auto games = library(libraryRoot, count);
    json result;
    result["games"] = count;
    result["repeat_passes"] = repeats;
    result["instrumented"] = instrumented;
#ifdef ARTWORK_BENCH_AFTER
    result["side"] = "after";
#else
    result["side"] = "before";
#endif
    if (instrumented) {
        const auto path = std::filesystem::path(stateRoot) / "counter-control.bin";
        resetIo();
        FILE *file = fopen(path.c_str(), "wb");
        ASSERT_NE(nullptr, file);
        const char bytes[] = "counter control\n";
        ASSERT_EQ(sizeof(bytes) - 1, fwrite(bytes, 1, sizeof(bytes) - 1, file));
        ASSERT_EQ(0, fclose(file));
        file = fopen(path.c_str(), "rb");
        ASSERT_NE(nullptr, file);
        char buffer[64]{};
        ASSERT_EQ(sizeof(bytes) - 1, fread(buffer, 1, sizeof(buffer), file));
        ASSERT_EQ(0, fclose(file));
        IoCounts control{};
        stopIo(&control);
        ASSERT_EQ(sizeof(bytes) - 1, control.read_bytes);
        ASSERT_EQ(sizeof(bytes) - 1, control.write_bytes);
        ASSERT_EQ(2u, control.bin_opens);
        result["counter_control"] = {{"read_bytes",control.read_bytes},{"write_bytes",control.write_bytes}};
        std::filesystem::remove(path);
    }
    auto boxart = std::make_unique<Boxart>();
    int errors = 0;
    auto phase = [&](const char *name, int passes, bool refresh = false) {
        if (instrumented)
            resetIo();
        const Snapshot begin = snapshot();
        if (refresh)
            boxart->refreshCache();
        for (int pass = 0; pass < passes; ++pass) {
            for (int first = 0; first < count; first += 40) {
                const int end = std::min(first + 40, count);
                for (int index = first; index < end; ++index)
                    boxart->queueBoxart(games[index]);
                boxart->startFetch();
                if (boxart->physicalFetching.valid())
                    boxart->physicalFetching.get();
                if (boxart->onlineFetching.valid())
                    boxart->onlineFetching.get();
                boxart->saveDatabase();
                for (int index = first; index < end; ++index) {
                    const GameBoxart art = boxart->getBoxart(games[index]);
                    if (art.busy || !art.boxartPath.empty() || art.scraped || art.uniqueId != productId(index)
                            || art.searchName != games[index].name
                            || art.region != (GameBoxart::JAPAN | GameBoxart::USA | GameBoxart::EUROPE))
                        ++errors;
#ifdef ARTWORK_BENCH_AFTER
                    if (art.offlineArtState != OfflineArtState::NoArtwork)
                        ++errors;
#else
                    if (!art.parsed)
                        ++errors;
#endif
                }
            }
        }
        const Snapshot finish = snapshot();
        IoCounts io{};
        if (instrumented)
            stopIo(&io);
        json measured = {
            {"passes", passes}, {"lookups", count * passes},
            {"elapsed_ms", std::chrono::duration<double, std::milli>(finish.wall - begin.wall).count()},
            {"user_cpu_ms", milliseconds(finish.cpu.ru_utime) - milliseconds(begin.cpu.ru_utime)},
            {"system_cpu_ms", milliseconds(finish.cpu.ru_stime) - milliseconds(begin.cpu.ru_stime)},
            {"disk_counter_status", {begin.diskStatus, finish.diskStatus}}, {"errors", errors}
        };
        if (instrumented) {
            measured["logical_read_bytes"] = io.read_bytes;
            measured["logical_write_bytes"] = io.write_bytes;
            measured["read_calls"] = io.read_calls;
            measured["write_calls"] = io.write_calls;
            measured["cue_opens"] = io.cue_opens;
            measured["bin_opens"] = io.bin_opens;
        }
        if (begin.diskStatus == 0 && finish.diskStatus == 0) {
            measured["physical_read_bytes"] = finish.disk.ri_diskio_bytesread - begin.disk.ri_diskio_bytesread;
            measured["physical_write_bytes"] = finish.disk.ri_diskio_byteswritten - begin.disk.ri_diskio_byteswritten;
        }
        measured["cpu_ms"] = measured["user_cpu_ms"].get<double>() + measured["system_cpu_ms"].get<double>();
        result["phases"][name] = measured;
    };
    phase("first_scan", 1);
    phase("repeat_browsing", repeats);
    boxart->term();
    boxart = std::make_unique<Boxart>();
    phase("persisted_restart", 1);
    phase("explicit_refresh", 1, true);
    boxart->term();
    std::ifstream databaseFile(std::filesystem::path(stateRoot) / "boxart" / "flycast-gamedb.json");
    json database;
    databaseFile >> database;
    result["database_records"] = database.size();
    result["database_bytes"] = std::filesystem::file_size(std::filesystem::path(stateRoot) / "boxart" / "flycast-gamedb.json");
    result["errors"] = errors;
    std::ofstream output(outputPath);
    output << result.dump(2) << '\n';
    output.close();
    EXPECT_EQ(0, errors);
    EXPECT_EQ(static_cast<size_t>(count), database.size());
}

}
