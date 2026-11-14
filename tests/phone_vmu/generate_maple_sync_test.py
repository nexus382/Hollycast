# Portions Copyright 2026 The Hollycast Authors
"""Exercise production Maple ownership and disk persistence with the real HTTP service."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
source = (root / "core/hw/maple/maple_devs.cpp").read_text(encoding="utf-8")
header = (root / "core/hw/maple/maple_devs.h").read_text(encoding="utf-8")
link = source[source.index("bool maple_sega_vmu::linkStatus()"):source.index("u32 maple_sega_vmu::dma(u32 cmd)")]
save = source[source.index("bool maple_sega_vmu::fullSave()"):source.index("void maple_sega_vmu::initializeVmu()")]
base_link = header[header.index("\tvirtual bool linkStatus()"):header.index("\tvirtual void requestReconnect()")]

prefix = r'''
// Portions Copyright 2026 The Hollycast Authors
// The transport, ownership checks, VMU linkStatus and fullSave bodies are production code.
#define ERROR_LOG(...) ((void)0)
using u64 = std::uint64_t;
constexpr u64 SH4_MAIN_CLOCK = 200000000;
u64 testTicks = 1;
u64 sh4_sched_now64() { return testTicks; }
struct { struct { bool online = false; } network; } settings;
std::string testCardPath;
namespace hostfs { std::string getVmuPath(const char *, bool) { return testCardPath; } }
namespace nowide { FILE *fopen(const char *path, const char *mode) { return std::fopen(path, mode); } }
struct maple_base {
 bool reconnect_requested = false;
 u64 reconnect_time = 0;
'''
middle = r'''
};
struct maple_sega_vmu : maple_base {
 FILE *file = nullptr;
 std::uint8_t flash_data[131072] {};
 std::uint8_t accessed_blocks[256] {};
 int bus_id = 1, bus_port = 0;
 char logical_port[3] = "B1";
 bool fullSaveNeeded = false, accessed_blocks_valid = false;
 u64 last_write_tick = 0;
 bool fullSave();
 bool linkStatus() override;
};
'''
tests = r'''
void testMapleSync(Address address)
{
 maple_sega_vmu vmu;
 testCardPath = (std::filesystem::current_path() / "phone-vmu-sync-regression.bin").string();
 const std::string diskPath = testCardPath;
 struct Cleanup {
  maple_sega_vmu& vmu; std::string path;
  ~Cleanup() { if(vmu.file)std::fclose(vmu.file); std::filesystem::remove(path); std::filesystem::remove(path + ".phone-backup"); }
 } cleanup {vmu, diskPath};
 vmu.file = std::fopen(diskPath.c_str(), "wb+");
 check(vmu.file != nullptr, "Could not create test VMU");
 std::fill_n(vmu.flash_data, sizeof(vmu.flash_data), 0x55);
 check(vmu.fullSave() && std::fflush(vmu.file) == 0, "Initial test card save failed");
 phone_vmu::publishCard(2, vmu.flash_data, sizeof(vmu.flash_data));
 const std::string lease(32, 'a'), otherLease(32, 'b');
 const std::string auth = "Content-Type: application/octet-stream\r\nX-VMU-Lease: " + lease + "\r\n";
 const auto revision = header(request(address, address.path + "card/2"), "ETag");
 auto checkout = [&](const std::string& extra) { return request(address, address.path + "checkout/2", "POST", "x", auth + extra); };
 check(checkout("If-Match: wrong\r\n").find("HTTP/1.1 409") == 0, "Stale checkout was accepted");
 check(checkout("If-Match: " + revision + "\r\nOrigin: https://untrusted.example\r\n").find("HTTP/1.1 403") == 0, "Cross-origin write accepted");
 check(request(address, address.path + "checkout/2", "POST").find("HTTP/1.1 403") == 0, "Missing lease was accepted");
 check(checkout("If-Match: " + revision + "\r\n").find("HTTP/1.1 200") == 0, "Checkout failed");
 check(checkout("If-Match: " + revision + "\r\n").find("HTTP/1.1 200") == 0, "Checkout retry was not idempotent");
 check(request(address, address.path + "checkout/2", "POST", "x", "Content-Type: application/octet-stream\r\nX-VMU-Lease: " + otherLease + "\r\nIf-Match: " + revision + "\r\n").find("HTTP/1.1 409") == 0, "Second phone took an owned VMU");
 check(!vmu.linkStatus(), "Checked-out VMU must be absent from the guest bus");

 std::string progress(131072, '\x55'); progress[123] = 42;
 auto queue = [&](char operation, const std::string& bytes, bool returning = false, std::size_t declaredLength = std::string::npos) {
  return request(address, address.path + "sync/2", "POST", bytes, auth + "X-VMU-Operation: " + std::string(32, operation) + "\r\nX-VMU-Return: " + (returning ? "1" : "0") + "\r\n", declaredLength);
 };
 auto state = [&](char operation) {
  return request(address, address.path + "sync/2", "GET", "", auth + "X-VMU-Operation: " + std::string(32, operation) + "\r\n");
 };
 check(queue('1', "short").find("HTTP/1.1 400") == 0, "Truncated card was accepted");
 check(queue('1', progress).find("HTTP/1.1 202") == 0, "Progress was not queued");
 check(state('1').find("HTTP/1.1 202") == 0 && vmu.flash_data[123] == 0x55, "HTTP thread changed live flash or falsely acknowledged persistence");
 check(!vmu.linkStatus(), "Checkpoint reinserted VMU before return");
 check(state('1').find("HTTP/1.1 200") == 0 && vmu.flash_data[123] == 42, "Emulation thread failed to commit progress");
 check(queue('1', progress).find("HTTP/1.1 200") == 0, "Saved operation was reapplied");
 auto readDisk = [](const std::string& path) { std::ifstream input(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(input), {}); };
 check(readDisk(diskPath) == progress, "Progress was acknowledged before the VMU file was flushed");
 check(readDisk(diskPath + ".phone-backup") == std::string(131072, '\x55'), "Original card backup is missing or altered");

 // Send only the oversized declaration: the server rejects it before reading
 // a body, so writing a large body here would race its intentional close.
 check(queue('2', "", false, 131073).find("HTTP/1.1 400") == 0, "Oversized body bypassed the parser limit");
 progress[123] = 43;
 check(queue('2', progress, true).find("HTTP/1.1 202") == 0, "Return was not queued");
 check(!vmu.linkStatus(), "VMU must visibly unplug before reconnecting");
 check(state('2').find("HTTP/1.1 200") == 0, "Return not persisted");
 testTicks += SH4_MAIN_CLOCK / 10;
 check(vmu.linkStatus(), "Returned VMU did not reconnect");
 check(readDisk(diskPath) == progress && vmu.flash_data[123] == 43, "Returned progress missing");
 check(queue('1', std::string(131072, 'x')).find("HTTP/1.1 409") == 0, "Old checkpoint overwrote a returned card");

 // A new owner must observe the current revision. I/O failures retain ownership
 // and live bytes so retrying can recover without replacing the original backup.
 const auto current = header(request(address, address.path + "card/2"), "ETag");
 check(checkout("If-Match: " + current + "\r\n").find("HTTP/1.1 200") == 0, "Second checkout failed");
 check(!vmu.linkStatus(), "Second checkout was not detached");
 testCardPath = diskPath + "/missing/card";
 progress[123] = 44;
 check(queue('3', progress).find("HTTP/1.1 202") == 0, "Failed-write fixture not queued");
 check(!vmu.linkStatus() && state('3').find("HTTP/1.1 500") == 0, "Disk error was reported as saved");
 check(vmu.flash_data[123] == 43 && readDisk(diskPath)[123] == 43, "Disk failure changed live data");
 testCardPath = diskPath;
 check(queue('4', progress).find("HTTP/1.1 202") == 0, "Disk retry failed to queue");
 check(!vmu.linkStatus() && state('4').find("HTTP/1.1 200") == 0, "Disk retry failed");
 check(vmu.flash_data[123] == 44 && readDisk(diskPath)[123] == 44, "Disk retry lost progress");

 check(queue('3', progress).find("HTTP/1.1 409") == 0, "Delayed checkpoint was accepted while detached");
 progress[123] = 45; check(queue('5', progress).find("HTTP/1.1 202") == 0, "State-load fixture not queued");
 phone_vmu::publishCard(2, vmu.flash_data, sizeof(vmu.flash_data));
 check(state('5').find("HTTP/1.1 409") == 0, "State load failed to invalidate ownership");
 vmu.linkStatus();check(vmu.flash_data[123] == 44, "Queued phone data overwrote a loaded state");
 phone_vmu::unpublishCard(2, vmu.flash_data);
 check(request(address, address.path + "card/2").find("HTTP/1.1 404") == 0, "Removed card remained published");
 phone_vmu::publishCard(2, vmu.flash_data, sizeof(vmu.flash_data), false);
 check(checkout("If-Match: " + current + "\r\n").find("HTTP/1.1 403") == 0, "Read-only/netplay card allowed write ownership");
 std::vector<std::uint8_t> replacement(131072, 0x66);
 phone_vmu::publishCard(2, replacement.data(), replacement.size());
 phone_vmu::unpublishCard(2, vmu.flash_data);
 check(body(request(address, address.path + "card/2"))[0] == 0x66, "Old device removed its replacement's card");
 phone_vmu::unpublishCard(2, replacement.data());
}
'''
Path(sys.argv[1]).write_text(prefix + base_link + middle + save + link + tests, encoding="utf-8")
