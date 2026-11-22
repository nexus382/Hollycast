# Portions Copyright 2026 The Hollycast Authors
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
osd = (root / 'core/rend/osd.cpp').read_text(encoding='utf-8')
header = (root / 'core/rend/osd.h').read_text(encoding='utf-8')
gui = (root / 'core/ui/gui.cpp').read_text(encoding='utf-8')

def between(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]

# Deliberately compile without _WIN32: publication must use the shared capability,
# including explicit blanking, rather than accidentally retaining a Windows guard.
source = r'''
#include <cstdint>
#include <cstring>
#include <iterator>
#include <cstdlib>
#include <iostream>
#define HAVE_MOVABLE_VMUS
#define HAVE_PHONE_VMU
using u8=std::uint8_t; using u32=std::uint32_t; using u64=std::uint64_t;
u64 getTimeMs() { return 123; }
u32 vmu_lcd_data[8][48*32] {};
bool vmu_lcd_status[8] {};
u64 vmuLastChanged[8] {};
int publications=0, lastSlot=-1; bool lastActive=false; u32 firstPixel=0;
namespace vmu_window {
void publishScreen(int slot, const u32* pixels, bool active) {
 ++publications; lastSlot=slot; lastActive=active; firstPixel=pixels ? pixels[0] : 0;
}
}
int phonePublications=0, phoneSlot=-1; bool phoneActive=false; u32 phonePixel=0;
namespace phone_vmu {
void publishScreen(int slot, const u32* pixels, bool active) {
 ++phonePublications; phoneSlot=slot; phoneActive=active; phonePixel=pixels ? pixels[0] : 0;
}
}
'''
source += between(osd, 'void push_vmu_screen(', 'int getScaledTopInset()')
source += header[header.index('static inline void blankVmus()'):]
source += between(gui, 'void reset_vmus()', 'void gui_error(')
source += r'''
int checks=0;
void require(bool condition,const char* message) {
 ++checks; if(!condition){std::cerr<<message<<'\n';std::exit(1);}
}
int main() {
 u8 pixels[48*32] {}; pixels[0]=1;
 push_vmu_screen(3,1,pixels);
 require(publications==1 && lastSlot==7 && lastActive && firstPixel==0xffffffff,
   "producer publishes LCD pixels to portable backends");
 require(phonePublications==1 && phoneSlot==7 && phoneActive && phonePixel==0xffffffff,
   "producer publishes LCD pixels to phones");
 push_vmu_screen(-1,0,pixels); push_vmu_screen(4,0,pixels);
 require(publications==1,"invalid slots do not publish");
 blankVmus();
 require(publications==9 && lastSlot==7 && lastActive && firstPixel==0,
   "explicit framebuffer blank reaches every portable screen without disconnecting it");
 require(phonePublications==9 && phoneSlot==7 && phoneActive && phonePixel==0,
   "explicit framebuffer blank reaches phones without disconnecting them");
 reset_vmu_screen(3,1);
 require(publications==10 && !lastActive,"disconnect publishes inactivity");
 require(phonePublications==10 && !phoneActive,"disconnect publishes inactivity to phones");
 reset_vmus();
 require(publications==18 && !lastActive,"game reset clears every portable screen");
 require(phonePublications==18 && !phoneActive,"game reset clears every phone screen");
 std::cout<<checks<<" checks passed\n";
}
'''
Path(sys.argv[1]).write_text('// Portions Copyright 2026 The Hollycast Authors\n' + source, encoding='utf-8')
