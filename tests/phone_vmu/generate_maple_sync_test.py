# Portions Copyright 2026 The Hollycast Authors
"""Exercise production Maple ownership and disk persistence with the real HTTP service."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
source = (root / "core/hw/maple/maple_devs.cpp").read_text(encoding="utf-8")
header = (root / "core/hw/maple/maple_devs.h").read_text(encoding="utf-8")


def extract_definition(text, marker, description):
    start = text.find(marker)
    if start == -1:
        raise SystemExit(f"Could not generate Maple sync test: missing {description} marker {marker!r}")

    opening = text.find("{", start + len(marker))
    if opening == -1:
        raise SystemExit(f"Could not generate Maple sync test: missing body for {description} marker {marker!r}")

    depth = 0
    state = "code"
    escaped = False
    index = opening
    while index < len(text):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""

        if state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if char == "*" and next_char == "/":
                state = "code"
                index += 1
        elif state in ("string", "character"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (state == "character" and char == "'"):
                state = "code"
        elif char == "/" and next_char == "/":
            state = "line_comment"
            index += 1
        elif char == "/" and next_char == "*":
            state = "block_comment"
            index += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "character"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1] + "\n"

        index += 1

    raise SystemExit(f"Could not generate Maple sync test: unterminated {description} definition")


link = extract_definition(source, "bool maple_sega_vmu::linkStatus()", "maple_sega_vmu::linkStatus()")
save = extract_definition(source, "bool maple_sega_vmu::fullSave()", "maple_sega_vmu::fullSave()")
base_link_start = header.find("\tvirtual bool linkStatus()")
if base_link_start == -1:
    raise SystemExit("Could not generate Maple sync test: missing maple_base::linkStatus() declaration")
base_link_end = header.find("\tvirtual void requestReconnect()", base_link_start)
if base_link_end == -1:
    raise SystemExit("Could not generate Maple sync test: missing maple_base::requestReconnect() declaration")
base_link = header[base_link_start:base_link_end]

prefix = r'''
// Portions Copyright 2026 The Hollycast Authors
// The transport, ownership checks, VMU linkStatus and fullSave bodies are production code.
#include <ctime>
#define ERROR_LOG(...) ((void)0)
using u64 = std::uint64_t;
constexpr u64 SH4_MAIN_CLOCK = 200000000;
u64 testTicks = 1;
u64 sh4_sched_now64() { return testTicks; }
unsigned rewindInvalidationCount = 0;
namespace rewind_state { void invalidate() { ++rewindInvalidationCount; } }
struct { struct { bool online = false; } network; } settings;
std::string testCardPath;
namespace hostfs { std::string getVmuPath(const char *, bool) { return testCardPath; } }
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
 bool memoryOnly = false;
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
  ~Cleanup() { if(vmu.file)std::fclose(vmu.file); std::filesystem::remove(path); std::error_code ec; for(const auto& e:std::filesystem::directory_iterator(std::filesystem::current_path(),ec)) if(e.path().filename().string().rfind("phone-vmu-sync-regression.bin.phone-backup-",0)==0) std::filesystem::remove(e.path(),ec); }
 } cleanup {vmu, diskPath};
 const std::string backupPrefix = "phone-vmu-sync-regression.bin.phone-backup-";
 for(unsigned i=0;i<6;++i){const auto old=std::filesystem::current_path()/(backupPrefix+"20200101-00000"+std::to_string(i));std::ofstream output(old,std::ios::binary);output<<"old";check(output.good(),"Could not seed old backup fixtures");}
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
 const unsigned invalidationsBeforeProgressSave = rewindInvalidationCount;
 check(!vmu.linkStatus(), "Checkpoint reinserted VMU before return");
 const auto progressSave = state('1');
 check(progressSave.find("HTTP/1.1 200") == 0 && vmu.flash_data[123] == 42, "Emulation thread failed to commit progress");
 check(rewindInvalidationCount == invalidationsBeforeProgressSave + 1, "Successful phone VMU persistence did not invalidate rewind history");
 check(queue('1', progress).find("HTTP/1.1 200") == 0, "Saved operation was reapplied");
 auto readDisk = [](const std::string& path) { std::ifstream input(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(input), {}); };
 check(readDisk(diskPath) == progress, "Progress was acknowledged before the VMU file was flushed");
 auto backupPaths = [&]() { std::vector<std::filesystem::path> paths; std::error_code ec; std::filesystem::directory_iterator it(std::filesystem::current_path(),ec),end; while(!ec&&it!=end){if(it->path().filename().string().rfind(backupPrefix,0)==0)paths.push_back(it->path());it.increment(ec);} check(!ec,"Could not enumerate VMU backups");std::sort(paths.begin(),paths.end());return paths; };
 // Backups are timestamped snapshots beside the VMU file; the newest must hold the pre-phone card.
 const auto firstSnapshots = backupPaths();
 const std::string backupPath = firstSnapshots.empty() ? std::string() : firstSnapshots.back().string();
 check(!backupPath.empty() && readDisk(backupPath) == std::string(131072, '\x55'), "Original card backup is missing or altered");
 const std::string firstBackupPath=backupPath;
 check(backupPaths().size()==5,"Backup pruning must keep five snapshots after a checkout");

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
 const std::string beforeSecondPhoneWrite=readDisk(diskPath);
 testCardPath = diskPath + "/missing/card";
 progress[123] = 44;
 check(queue('3', progress).find("HTTP/1.1 202") == 0, "Failed-write fixture not queued");
 const unsigned invalidationsBeforeFailedSave = rewindInvalidationCount;
 check(!vmu.linkStatus(), "VMU reconnected before failed phone persistence");
 const auto failedSave = state('3');
 check(failedSave.find("HTTP/1.1 500") == 0, "Disk error was reported as saved");
 check(rewindInvalidationCount == invalidationsBeforeFailedSave, "Failed phone VMU persistence invalidated rewind history");
 check(vmu.flash_data[123] == 43 && readDisk(diskPath)[123] == 43, "Disk failure changed live data");
 testCardPath = diskPath;
 check(queue('4', progress).find("HTTP/1.1 202") == 0, "Disk retry failed to queue");
 check(!vmu.linkStatus() && state('4').find("HTTP/1.1 200") == 0, "Disk retry failed");
 check(vmu.flash_data[123] == 44 && readDisk(diskPath)[123] == 44, "Disk retry lost progress");
 check(backupPaths().size()==5,"Backup rotation must retain at most five snapshots");
 check(readDisk(firstBackupPath)==std::string(131072,'\x55'),"A later backup overwrote the first snapshot");
 check(readDisk(backupPaths().back().string())==beforeSecondPhoneWrite,"A second checkout did not preserve its own snapshot");

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
 phone_vmu::publishCard(2, vmu.flash_data, sizeof(vmu.flash_data));
 std::vector<std::string> expectedSnapshots { std::string(131072, '\x55'), beforeSecondPhoneWrite };
 for(unsigned session=0;session<7;++session){
  const std::string before=readDisk(diskPath),revision=header(request(address,address.path+"card/2"),"ETag");
  check(checkout("If-Match: "+revision+"\r\n").find("HTTP/1.1 200")==0,"Repeated backup checkout failed");
  check(!vmu.linkStatus(),"Repeated checkout did not detach the card");
  std::string data(131072,static_cast<char>(0x60+session));data[123]=static_cast<char>(20+session);
  const char operation=static_cast<char>('1'+session);
  check(queue(operation,data).find("HTTP/1.1 202")==0,"Repeated checkpoint was not queued");
  check(!vmu.linkStatus()&&state(operation).find("HTTP/1.1 200")==0,"Repeated checkpoint did not save while detached");
  expectedSnapshots.push_back(before);
  check(queue('f',data,true).find("HTTP/1.1 202")==0&&!vmu.linkStatus(),"Repeated return must visibly unplug the card");
  testTicks += SH4_MAIN_CLOCK / 10;
  check(vmu.linkStatus(),"Repeated return did not reconnect the card after the bus delay");
  check(state('f').find("HTTP/1.1 200")==0&&readDisk(diskPath)==data,"Repeated return lost card contents");
 }
 const auto retained=backupPaths();
 check(retained.size()==5,"More than five checkouts must retain only the newest five snapshots");
 for(std::size_t i=0;i<retained.size();++i)
  check(readDisk(retained[i].string())==expectedSnapshots[expectedSnapshots.size()-retained.size()+i],"Backup rotation must retain the newest snapshots in timestamp order");
 phone_vmu::unpublishCard(2,vmu.flash_data);
}
'''
Path(sys.argv[1]).write_text(prefix + base_link + middle + save + link + tests, encoding="utf-8")
