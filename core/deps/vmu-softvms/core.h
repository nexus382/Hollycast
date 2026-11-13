// VMU JavaScript engine from bucanero/dcvmu-tool, GPL-3.0-only.
// See README.md and COPYING. The engine below is unmodified.
#pragma once
namespace phone_vmu {
inline constexpr char standaloneCore[] = R"VMUJS(
/* =====================================================================
 * VMU emulator  (JS port of SoftVMS 1.10's cpu.c, by Marcus Comstedt)
 *
 * Runs VMU mini-games: the LC86K CPU, its timers, the LCD and the one-tone
 * buzzer. Without a BIOS dump the firmware calls games make (flash read,
 * write and verify, and the clock tick) are emulated, as SoftVMS does.
 *
 * Kept as close to cpu.c as JavaScript allows, statement for statement, so
 * it can be checked against it: web/test/dctest.js builds cpu.c with a
 * headless front end (web/test/softvms/) and compares the LCD, the flash
 * and the tones played, slice by slice.
 *
 * SoftVMS runs forever and sleeps between 10 ms slices; here run(n) runs n
 * slices and returns, and the page calls it from requestAnimationFrame.
 * onSlice is called where cpu.c polls for input, once per slice.
 * ===================================================================== */
const VMS = (() => {
  const flash = new Uint8Array(0x20000);
  /* 64 KB, padded like flash so a read just past it is defined */
  const bios = new Uint8Array(0x20000);
  const ram = [new Uint8Array(0x100), new Uint8Array(0x100)];
  const sfr = new Uint8Array(0x100);
  const xram = [new Uint8Array(0x80), new Uint8Array(0x80), new Uint8Array(0x80)];
  const wram = new Uint8Array(0x200);
  const parity = new Uint8Array(0x100);
  /* 48x32 dot matrix, then the four status icons on rows 33-39 */
  const W = 48, H = 40;
  const lcd = new Uint8Array(W * H);

  let rom = flash;
  let pc = 0, lcdUpdated = 0, lcdon = 0, imask = 0, intreq = 0, hasbios = 0;
  let spd = 0, t0h = 0, t0l = 0, t0base = 0, t0scale = 0, t1h = 0, t1l = 0;
  let gamesize = 0, mcy = 0, tick = 0;
  let halted = null;
  const hooks = { sound: null, redraw: null, error: null };

  /* ++ as on an unsigned char: the value stored, so 255 wraps to 0 */
  const inc = (a, o) => { a[o]++; return a[o]; };
  const SGNEXT = n => (n & 0x80 ? n - 0x100 : n);
  const tobcd = n => ((Math.floor(n / 10)) << 4) | (n % 10);
  const error = msg => { if (hooks.error) hooks.error(msg); };
  const sound = freq => { if (hooks.sound) hooks.sound(freq); };

  function keypress(i) {
    sfr[0x4c] &= ~(1 << i);
    if (sfr[0x4e] & 4) sfr[0x4e] |= 2;
  }
  function keyrelease(i) {
    sfr[0x4c] |= (1 << i);
    if (sfr[0x4e] & 4) sfr[0x4e] |= 2;
  }

  /* date: { year, mon (1-12), mday, hour, min, sec, wday (0 = Sunday) } */
  function dateNow() {
    const d = new Date();
    return { year: d.getFullYear(), mon: d.getMonth() + 1, mday: d.getDate(),
             hour: d.getHours(), min: d.getMinutes(), sec: d.getSeconds(), wday: d.getDay() };
  }

  /* A lone game file gets a directory built around it, as on a VMU */
  function fakeflash(filename, sz, tm) {
    const root = 255 * 512, fat = 254 * 512, dir = 253 * 512;
    let i;
    filename = filename.replace(/^.*[\\/]/, "");
    flash.fill(0, 241 * 512);
    sz = (sz + 511) >> 9;
    for (i = 0; i < 256 * 2; i += 2) {
      flash[fat + i] = 0xfc;
      flash[fat + i + 1] = 0xff;
    }
    for (i = 0; i < sz; i++) {
      flash[fat + 2 * i] = i + 1;
      flash[fat + 2 * i + 1] = 0;
    }
    if ((--i) >= 0) {
      flash[fat + 2 * i] = 0xfa;
      flash[fat + 2 * i + 1] = 0xff;
    }
    flash[fat + 254 * 2] = 0xfa;
    flash[fat + 254 * 2 + 1] = 0xff;
    flash[fat + 255 * 2] = 0xfa;
    flash[fat + 255 * 2 + 1] = 0xff;
    for (i = 253; i > 241; --i) {
      flash[fat + 2 * i] = i - 1;
      flash[fat + 2 * i + 1] = 0;
    }
    flash[fat + 241 * 2] = 0xfa;
    flash[fat + 241 * 2 + 1] = 0xff;
    flash[dir] = 0xcc;
    for (i = 0; i < 12; i++)
      flash[dir + 4 + i] = i < filename.length ? filename.charCodeAt(i) : 0x20;
    const tmYear = tm.year - 1900;
    flash[dir + 0x10] = tobcd(Math.floor(tmYear / 100) + 19);
    flash[dir + 0x11] = tobcd(tmYear % 100);
    flash[dir + 0x12] = tobcd(tm.mon);
    flash[dir + 0x13] = tobcd(tm.mday);
    flash[dir + 0x14] = tobcd(tm.hour);
    flash[dir + 0x15] = tobcd(tm.min);
    flash[dir + 0x16] = tobcd(tm.sec);
    flash[dir + 0x17] = tobcd(tm.wday);
    flash[dir + 0x18] = sz & 0xff;
    flash[dir + 0x19] = sz >> 8;
    flash[dir + 0x1a] = 1;
    flash.fill(0x55, root, root + 16);
    flash[root + 0x10] = 1;
    flash.copyWithin(root + 0x30, dir + 0x10, dir + 0x18);
    flash[root + 0x44] = 255;
    flash[root + 0x46] = 254;
    flash[root + 0x48] = 1;
    flash[root + 0x4a] = 253;
    flash[root + 0x4c] = 13;
    flash[root + 0x50] = 200;
  }

  /* Flash writes are allowed within the game file only */
  function check_gamesize() {
    const root = 255 * 512;
    const fatblk = (flash[root + 0x47] << 8) | flash[root + 0x46];
    let dirblk = (flash[root + 0x4b] << 8) | flash[root + 0x4a];
    let dircnt = (flash[root + 0x4d] << 8) | flash[root + 0x4c];
    if (fatblk >= 256 || dircnt >= 256 || !dircnt)
      return;
    const fat = fatblk * 512;
    while (dircnt-- && dirblk < 256) {
      const dir = dirblk * 512;
      dirblk = (flash[fat + 2 * dirblk + 1] << 8) | flash[fat + 2 * dirblk];
      for (let i = 0; i < 16; i++)
        if (flash[dir + i * 32] === 0xcc) {
          const sz = (flash[dir + i * 32 + 0x19] << 8) | flash[dir + i * 32 + 0x18];
          if (sz > 1 && sz <= 200) {
            gamesize = sz * 512;
            return;
          }
        }
    }
  }

  /* A whole card, or a single game file. false if it is too short. */
  function load(bytes, filename, tm) {
    if (bytes.length < 0x480) return false;
    flash.fill(0);
    const r = Math.min(bytes.length, flash.length);
    flash.set(bytes.subarray(0, r));
    gamesize = r;
    if (r < 241 * 512)
      fakeflash(filename || "", r, tm || dateNow());
    check_gamesize();
    return true;
  }

  /* A dump of the VMU's own firmware, or null to emulate its calls */
  function setBios(bytes) {
    bios.fill(0);
    if (!bytes) { hasbios = 0; return true; }
    if (bytes.length < 0x200) return false;
    bios.set(bytes.subarray(0, Math.min(bytes.length, 0x10000)));
    hasbios = 1;
    return true;
  }

  function vmputpixel(x, y, p) {
    lcd[y * W + x] = p & 1;
  }

  const icon = [
    [0x2a, 0x23, 0x47, 0xfb, 0x23, 0x23, 0x3f],
    [0xff, 0x83, 0x93, 0xbb, 0x93, 0xbb, 0xff],
    [0x7c, 0x82, 0x92, 0x9b, 0x82, 0x82, 0x7c],
    [0x10, 0x28, 0xef, 0x6e, 0x6c, 0x7e, 0xc3]
  ];

  function lcdrefresh() {
    let y, x, i, a, b = 0, p = 0;

    p = sfr[0x22];
    if (p >= 0x83)
      p -= 0x83;
    b = (p >> 6);
    p = (p & 0x3f) * 2;
    for (y = 0; y < 32; y++) {
      for (x = 0; x < 48; ) {
        let value = xram[b][p++];
        if (!lcdon)
          value = 0;
        vmputpixel(x++, y, value >> 7);
        vmputpixel(x++, y, value >> 6);
        vmputpixel(x++, y, value >> 5);
        vmputpixel(x++, y, value >> 4);
        vmputpixel(x++, y, value >> 3);
        vmputpixel(x++, y, value >> 2);
        vmputpixel(x++, y, value >> 1);
        vmputpixel(x++, y, value);
        if ((p & 0xf) >= 12)
          p += 4;
        if (p >= 128) {
          b++;
          p -= 128;
        }
        if (b === 2 && p >= 6) {
          b = 0;
          p -= 6;
        }
      }
    }
    p++;
    if ((p & 0xf) >= 12)
      p += 4;
    if (p >= 128) {
      b++;
      p -= 128;
    }
    if (b === 2 && p >= 6) {
      b = 0;
      p -= 6;
    }
    for (i = 0; i < 4; i++) {
      a = xram[b][p++] >> (2 * (3 - i));
      if (!lcdon)
        a = 0;
      for (y = 0; y < 7; y++) {
        const value = icon[i][y];
        for (x = 0; x < 7; x++)
          vmputpixel(2 + i * 12 + x, y + 33, (value >> (7 - x)) & a);
      }
      if ((p & 0xf) >= 12)
        p += 4;
      if (p >= 128) {
        b++;
        p -= 128;
      }
      if (b === 2 && p >= 6) {
        b = 0;
        p -= 6;
      }
    }
    if (hooks.redraw) hooks.redraw();
    lcdUpdated = 0;
  }

  function writemem(addr, value) {
    value &= 0xff;
    if (addr < 0x100) {
      ram[(sfr[0x01] & 2) >> 1][addr] = value;
      return;
    }
    if (addr >= 0x180) {
      const b = sfr[0x25];
      if (b > 2 || (b === 2 && addr >= 0x186))
        return;
      xram[b][addr - 0x180] = value;
      if (lcdon)
        lcdUpdated = 1;
    } else switch (addr) {
      case 0x100:
        sfr[0x01] = (sfr[0x01] & 0xfe) | parity[value];
        break;
      case 0x10d:
        if ((value & 1) !== (sfr[0x0d] & 1)) {
          if (pc > 0xfffd || rom[pc] !== 0x21)
            error("EXT 0 changed without following JMPF.  pc = " + (pc & 0xffff).toString(16).padStart(4, "0"));
          else
            pc = (rom[pc + 1] << 8) | rom[pc + 2];
          if (hasbios)
            rom = ((value & 1) ? flash : bios);
        }
        break;
      case 0x10e:
        switch (value & 0xa0) {
          case 0x00: spd = 3000; break;
          case 0x20: spd = 164; break;
          case 0x80: spd = 6000; break;
          case 0xa0: spd = 328; break;
        }
        break;
      case 0x110:
        if (!(value & 0x40))
          t0l = sfr[0x13];
        if (!(value & 0x80))
          t0h = sfr[0x15];
        break;
      case 0x111:
        t0scale = 256 - value;
        t0base = 0;
        break;
      case 0x113:
        if (!(sfr[0x10] & 0x40))
          t0l = value;
        break;
      case 0x115:
        if (!(sfr[0x10] & 0x80))
          t0h = value;
        break;
      case 0x118:
        if (!(value & 0x40))
          t1l = sfr[0x1b];
        if (!(value & 0x80))
          t1h = sfr[0x1d];
        break;
      case 0x11b:
        if (!(sfr[0x18] & 0x40))
          t1l = value;
        break;
      case 0x11d:
        if (!(sfr[0x18] & 0x80))
          t1h = value;
        break;
      case 0x122:
        if (lcdon)
          lcdUpdated = 1;
        break;
      case 0x127:
        if ((+!!(value & 0x80)) !== lcdon) {
          lcdon = +!!(value & 0x80);
          lcdrefresh();
        }
        break;
      case 0x166:
        wram[0x1ff & ((sfr[0x65] << 8) | sfr[0x64])] = value;
        /* not !++sfr[0x64]: on a typed array that yields 256, not the 0 stored */
        if (sfr[0x63] & 0x10) {
          sfr[0x64]++;
          if (!sfr[0x64])
            sfr[0x65] ^= 1;
        }
        return;
    }
    sfr[addr & 0xff] = value;
    if (addr === 0x118 || addr === 0x11b) {
      /* the buzzer: timer 1's reload value sets the pitch */
      if (sfr[0x18] & 0x40)
        sound(Math.trunc(32768 / ((256 - sfr[0x1b]) * 6)));
      else
        sound(-1);
    }
  }

  function readmem(addr) {
    let r;
    if (addr < 0x100)
      return ram[(sfr[0x01] & 2) >> 1][addr];
    if (addr >= 0x180) {
      const b = sfr[0x25];
      if (b > 2)
        return 0xff;
      return xram[b][addr - 0x180];
    } else switch (addr) {
      case 0x112:
        return t0l;
      case 0x114:
        return t0h;
      case 0x11b:
        return t1l;
      case 0x11d:
        return t1h;
      case 0x15c:
        return 2;
      case 0x165:
        return 0xfe | (sfr[0x65] & 1);
      case 0x166:
        r = wram[0x1ff & ((sfr[0x65] << 8) | sfr[0x64])];
        /* not !++sfr[0x64]: on a typed array that yields 256, not the 0 stored */
        if (sfr[0x63] & 0x10) {
          sfr[0x64]++;
          if (!sfr[0x64])
            sfr[0x65] ^= 1;
        }
        return r;
    }
    return sfr[addr & 0xff];
  }

  function readlatch(addr) {
    switch (addr) {
      case 0x11b:
      case 0x11d:
        return 0xff;
      default:
        return readmem(addr);
    }
  }

  function push(n) {
    writemem(0x106, readmem(0x106) + 1);
    ram[0][readmem(0x106)] = n;
  }

  function pop() {
    const r = ram[0][readmem(0x106)];
    writemem(0x106, readmem(0x106) - 1);
    return r;
  }

  function resetcpu(tm) {
    let i;
    tm = tm || dateNow();
    for (const b of ram) b.fill(0);
    for (const b of xram) b.fill(0);
    wram.fill(0);
    sfr.fill(0);
    parity[0] = 0;
    parity[1] = 1;
    parity[2] = 1;
    parity[3] = 0;
    for (i = 4; i < 16; i++)
      parity[i] = parity[i & 3] ^ parity[i >> 2];
    for (i = 16; i < 255; i++)
      parity[i] = parity[i & 15] ^ parity[i >> 4];
    sfr[0x06] = 0x7f;
    sfr[0x4c] = 0xff;
    sfr[0x01] = 0x02;
    t0h = t0l = 0;
    t1h = t1l = 0;
    t0base = 0;
    t0scale = 256;
    const tmYear = tm.year - 1900;
    ram[0][0x10] = tobcd(Math.floor(tmYear / 100) + 19);
    ram[0][0x11] = tobcd(tmYear % 100);
    ram[0][0x12] = tobcd(tm.mon);
    ram[0][0x13] = tobcd(tm.mday);
    ram[0][0x14] = tobcd(tm.hour);
    ram[0][0x15] = tobcd(tm.min);
    ram[0][0x17] = tm.year >> 8;
    ram[0][0x18] = tm.year & 0xff;
    ram[0][0x19] = tm.mon;
    ram[0][0x1a] = tm.mday;
    ram[0][0x1b] = tm.hour;
    ram[0][0x1c] = tm.min;
    ram[0][0x1d] = tm.sec;
    ram[0][0x31] = 0xff;
    sfr[0x08] = 0x80;
    writemem(0x10e, 0x81);
    lcdUpdated = 0;
    lcdon = 0;
    imask = 0;
    intreq = 0;
    mcy = 0;
    tick = 0;
    halted = null;
    if (hasbios) {
      sfr[0x0d] = 0;
      rom = bios;
      pc = 0x1f0;
    } else {
      sfr[0x0d] = 1;
      rom = flash;
      pc = 0;
      writemem(0x125, 2);
      writemem(0x182, 0x10);
      writemem(0x125, 0);
      writemem(0x127, 0x80);
    }
    sound(-1);
  }

  function month_days() {
    const m = ram[0][0x19];
    if (m === 2) {
      const y = ram[0][0x18] | (ram[0][0x17] << 8);
      if (y & 3)
        return 28;
      if (!(y % 4000))
        return 29;
      if (!(y % 1000))
        return 28;
      if (!(y % 400))
        return 29;
      if (!(y % 100))
        return 28;
      return 29;
    } else return (m > 7 ? ((m & 1) ? 30 : 31) : ((m & 1) ? 31 : 30));
  }

  /* The firmware calls a game makes, with no BIOS to run them */
  function handle_fwcall(pc) {
    let i, a;
    switch (pc) {
      case 0x100:
        a = ((ram[1][0x7d] << 16) | (ram[1][0x7e] << 8) | ram[1][0x7f]) & 0x1ffff;
        if (a >= gamesize)
          writemem(0x100, 0xff);
        else {
          writemem(0x100, 0x00);
          for (i = 0; i < 0x80; i++)
            flash[(a & ~0xff) | ((a + i) & 0xff)] = ram[1][i + 0x80];
        }
        return 0x105;
      case 0x110: {
        let r = 0;
        a = ((ram[1][0x7d] << 16) | (ram[1][0x7e] << 8) | ram[1][0x7f]) & 0x1ffff;
        for (i = 0; i < 0x80; i++)
          if ((r = (flash[(a & ~0xff) | ((a + i) & 0xff)] ^ ram[1][i + 0x80])) !== 0)
            break;
        writemem(0x100, r);
        return 0x115;
      }
      case 0x120:
        a = ((ram[1][0x7d] << 16) | (ram[1][0x7e] << 8) | ram[1][0x7f]) & 0x1ffff;
        for (i = 0; i < 0x80; i++)
          ram[1][i + 0x80] = flash[(a & ~0xff) | ((a + i) & 0xff)];
        return 0x125;
      case 0x130:
        if (!((ram[0][0x1e] ^= 1) & 1))
          if (inc(ram[0], 0x1d) >= 60) {
            ram[0][0x1d] = 0;
            if (inc(ram[0], 0x1c) >= 60) {
              ram[0][0x1c] = 0;
              if (inc(ram[0], 0x1b) >= 24) {
                ram[0][0x1b] = 0;
                if (inc(ram[0], 0x1a) > month_days()) {
                  ram[0][0x1a] = 1;
                  if (inc(ram[0], 0x19) >= 13) {
                    ram[0][0x19] = 1;
                    if (ram[0][0x18] === 0xff) {
                      ram[0][0x18] = 0;
                      ram[0][0x17]++;
                    } else
                      ram[0][0x18]++;
                  }
                }
              }
            }
          }
        return 0x139;
      case 0x1f0:
        halted = "exit";
        return 0;
      default:
        error("Firmware entered at unknown vector " + pc.toString(16).padStart(4, "0") + "!");
        halted = "vector";
        return 0;
    }
  }

  /* Branch taken / not taken: the offset byte is at pc */
  const branch = () => { pc = 0xffff & (pc + 1 + SGNEXT(rom[pc])); };
  const nobranch = () => { pc++; pc &= 0xffff; };

  function add(s, c) {
    const r = readmem(0x100);
    writemem(0x100, r + s + c);
    writemem(0x101, (readmem(0x101) & 0x3b) | (r + s + c > 255 ? 0x80 : 0) |
             ((r & 15) + (s & 15) + c > 15 ? 0x40 : 0) |
             ((0x80 & (~r ^ s) & (s ^ (r + s + c))) ? 4 : 0));
  }
  /* FIXME (upstream): OV is never set */
  function sub(s, c) {
    const r = readmem(0x100);
    writemem(0x100, r - s - c);
    writemem(0x101, (readmem(0x101) & 0x3b) | (r - s - c < 0 ? 0x80 : 0) |
             ((r & 15) - (s & 15) - c < 0 ? 0x40 : 0));
  }
  const carry = () => (readmem(0x101) & 0x80) >> 7;

  /* The operations shared by direct (d9) and indirect (@Rj) addressing,
   * op being the opcode's high nibble. Returns the cycles taken. */
  function memop(op, r) {
    let s;
    switch (op) {
      case 0: writemem(0x100, readmem(r)); break;
      case 1: writemem(r, readmem(0x100)); break;
      case 5:
        s = (readlatch(r) - 1) & 0xff;
        writemem(r, s);
        if (s !== 0) branch(); else nobranch();
        return 2;
      case 6: writemem(r, readlatch(r) + 1); break;
      case 7: writemem(r, readlatch(r) - 1); break;
      case 8: add(readmem(r), 0); break;
      case 9: s = readmem(r); add(s, carry()); break;
      case 0xa: sub(readmem(r), 0); break;
      case 0xb: s = readmem(r); sub(s, carry()); break;
      case 0xc:
        s = readmem(r);
        writemem(r, readmem(0x100));
        writemem(0x100, s);
        break;
      case 0xd: writemem(0x100, readmem(0x100) | readmem(r)); break;
      case 0xe: writemem(0x100, readmem(0x100) & readmem(r)); break;
      case 0xf: writemem(0x100, readmem(0x100) ^ readmem(r)); break;
    }
    return 1;
  }

  /*
   * Run n slices, calling onSlice() at the end of each, where cpu.c polls
   * for input. Returns early if the game stops: halted then says why,
   * "exit" for a return to the (absent) BIOS menu.
   */
  function run(n, onSlice) {
    let done = 0;
    while (!halted) {
      let r, s, v, cy = 1, boundary = false;
      const i = rom[pc];
      pc++;
      pc &= 0xffff;
      switch (i & 0xf) {
        case 0:
          switch (i >> 4) {
            case 0:
              break;
            case 1:
              cy = 4;
              push((pc + 2) & 0xff);
              push(((pc + 2) & 0xff00) >> 8);
              pc = 0xffff & (pc + 1 + rom[pc] + ((rom[(pc + 1) & 0xffff]) << 8));
              break;
            case 2:
              cy = 2;
              push((pc + 2) & 0xff);
              push(((pc + 2) & 0xff00) >> 8);
              pc = (rom[pc] << 8) | rom[(pc + 1) & 0xffff];
              break;
            case 3:
              cy = 7;
              r = readmem(0x102) * ((readmem(0x100) << 8) | readmem(0x103));
              writemem(0x101, (readmem(0x101) & 0x7b) | (r > 65535 ? 4 : 0));
              writemem(0x103, r & 0xff);
              writemem(0x100, (r & 0xff00) >> 8);
              writemem(0x102, (r & 0xff0000) >> 16);
              break;
            case 4:
              cy = 7;
              r = readmem(0x102);
              if (r) {
                v = (readmem(0x100) << 8) | readmem(0x103);
                s = v % r;
                r = Math.floor(v / r);
              } else {
                r = 0xff00 | readmem(0x103);
                s = 0;
              }
              writemem(0x101, (readmem(0x101) & 0x7b) | (s ? 0 : 4));
              writemem(0x103, r & 0xff);
              writemem(0x100, (r & 0xff00) >> 8);
              writemem(0x102, s);
              break;
            case 5:
              cy = 2;
              writemem(0x100, flash[0x1ffff & (readmem(0x104) + (readmem(0x105) << 8) +
                                               (readmem(0x154) << 16))]);
              break;
            case 6:
              cy = 2;
              push(readmem(rom[pc++]));
              pc &= 0xffff;
              break;
            case 7:
              cy = 2;
              r = rom[pc++];
              writemem(r, pop());
              pc &= 0xffff;
              break;
            case 8:
              cy = 2;
              if (readmem(0x100) === 0) branch(); else nobranch();
              break;
            case 9:
              cy = 2;
              if (readmem(0x100) !== 0) branch(); else nobranch();
              break;
            case 0xa:
              cy = 2;
              r = pop() << 8;
              r |= pop();
              pc = r;
              break;
            case 0xb:
              cy = 2;
              r = pop() << 8;
              r |= pop();
              pc = r;
              --imask;
              break;
            case 0xc:
              r = readmem(0x100);
              writemem(0x100, (r >> 1) | ((r & 1) << 7));
              break;
            case 0xd:
              r = readmem(0x100);
              s = readmem(0x101);
              writemem(0x101, (s & 0x7f) | ((r & 1) << 7));
              writemem(0x100, (r >> 1) | (s & 0x80));
              break;
            case 0xe:
              r = readmem(0x100);
              writemem(0x100, (r << 1) | ((r & 0x80) >> 7));
              break;
            case 0xf:
              r = readmem(0x100);
              s = readmem(0x101);
              writemem(0x101, (s & 0x7f) | (r & 0x80));
              writemem(0x100, (r << 1) | ((s & 0x80) >> 7));
              break;
          }
          break;
        case 1:
          switch (i >> 4) {
            case 0:
              cy = 2;
              branch();
              break;
            case 1:
              cy = 4;
              pc = 0xffff & (pc + 1 + rom[pc] + ((rom[(pc + 1) & 0xffff]) << 8));
              break;
            case 2:
              cy = 2;
              pc = (rom[pc] << 8) | rom[(pc + 1) & 0xffff];
              break;
            case 3:
            case 4:
              cy = 2;
              r = readmem(0x100);
              writemem(0x101, (readmem(0x101) & 0x7f) | (r < rom[pc] ? 0x80 : 0));
              s = (i >> 4) === 3 ? (r === rom[pc++]) : (r !== rom[pc++]);
              pc &= 0xffff;
              if (s) branch(); else nobranch();
              break;
            case 5:
              cy = 2;
              if (!(readmem(0x154) & 2))
                flash[0x1ffff & (readmem(0x104) + (readmem(0x105) << 8) +
                                 (readmem(0x154) << 16))] = readmem(0x100);
              break;
            case 6:
              cy = 2;
              push(readmem(0x100 | rom[pc++]));
              pc &= 0xffff;
              break;
            case 7:
              cy = 2;
              r = 0x100 | rom[pc++];
              writemem(r, pop());
              pc &= 0xffff;
              break;
            case 8:
              s = rom[pc++];
              pc &= 0xffff;
              add(s, 0);
              break;
            case 9:
              s = rom[pc++];
              pc &= 0xffff;
              add(s, carry());
              break;
            case 0xa:
              s = rom[pc++];
              pc &= 0xffff;
              sub(s, 0);
              break;
            case 0xb:
              s = rom[pc++];
              pc &= 0xffff;
              sub(s, carry());
              break;
            case 0xc:
              cy = 2;
              writemem(0x100, rom[0xffff & (readmem(0x100) + readmem(0x104) +
                                            (readmem(0x105) << 8))]);
              break;
            case 0xd:
              writemem(0x100, readmem(0x100) | rom[pc++]);
              pc &= 0xffff;
              break;
            case 0xe:
              writemem(0x100, readmem(0x100) & rom[pc++]);
              pc &= 0xffff;
              break;
            case 0xf:
              writemem(0x100, readmem(0x100) ^ rom[pc++]);
              pc &= 0xffff;
              break;
          }
          break;
        case 2:
        case 3:
          r = ((i & 1) << 8) | rom[pc++];
          pc &= 0xffff;
          switch (i >> 4) {
            case 2:
              cy = 2;
              v = rom[pc++];
              writemem(r, v);
              pc &= 0xffff;
              break;
            case 3:
            case 4:
              cy = 2;
              s = readmem(r);
              r = readmem(0x100);
              writemem(0x101, (readmem(0x101) & 0x7f) | (r < s ? 0x80 : 0));
              if ((i >> 4) === 3 ? r === s : r !== s) branch(); else nobranch();
              break;
            default:
              cy = memop(i >> 4, r);
          }
          break;
        case 4:
        case 5:
        case 6:
        case 7:
          r = readmem((i & 3) | ((readmem(0x101) >> 1) & 0xc)) | ((i & 2) ? 0x100 : 0);
          switch (i >> 4) {
            case 2:
              v = rom[pc++];
              writemem(r, v);
              pc &= 0xffff;
              break;
            case 3:
            case 4:
              cy = 2;
              s = readmem(r);
              r = rom[pc++];
              pc &= 0xffff;
              writemem(0x101, (readmem(0x101) & 0x7f) | (s < r ? 0x80 : 0));
              if ((i >> 4) === 3 ? r === s : r !== s) branch(); else nobranch();
              break;
            default:
              cy = memop(i >> 4, r);
          }
          break;
        default:
          switch (i & 0xe0) {
            case 0x00:
              cy = 2;
              r = ((i & 7) << 8) | ((i & 0x10) << 7) | rom[pc++];
              push(pc & 0xff);
              push((pc & 0xff00) >> 8);
              pc = (pc & 0xf000) | r;
              break;
            case 0x20:
              cy = 2;
              r = ((i & 7) << 8) | ((i & 0x10) << 7) | rom[pc++];
              pc = (pc & 0xf000) | r;
              break;
            case 0x40:
              cy = 2;
              r = ((i & 0x10) << 4) | rom[pc++];
              pc &= 0xffff;
              if ((s = readmem(r)) & (1 << (i & 7))) {
                writemem(r, s & ~(1 << (i & 7)));
                branch();
              } else nobranch();
              break;
            case 0x60:
              cy = 2;
              r = ((i & 0x10) << 4) | rom[pc++];
              pc &= 0xffff;
              if (readmem(r) & (1 << (i & 7))) branch(); else nobranch();
              break;
            case 0x80:
              cy = 2;
              r = ((i & 0x10) << 4) | rom[pc++];
              pc &= 0xffff;
              if (!(readmem(r) & (1 << (i & 7)))) branch(); else nobranch();
              break;
            case 0xa0:
              r = ((i & 0x10) << 4) | rom[pc++];
              pc &= 0xffff;
              writemem(r, readlatch(r) ^ (1 << (i & 7)));
              break;
            case 0xc0:
              r = ((i & 0x10) << 4) | rom[pc++];
              pc &= 0xffff;
              writemem(r, readlatch(r) & ~(1 << (i & 7)));
              break;
            case 0xe0:
              r = ((i & 0x10) << 4) | rom[pc++];
              pc &= 0xffff;
              writemem(r, readlatch(r) | (1 << (i & 7)));
              break;
          }
          break;
      }
      mcy += cy;
      if (mcy >= spd) {
        if (lcdUpdated)
          lcdrefresh();
        if (onSlice) onSlice();
        mcy -= spd;
        ++tick;
        if (tick >= 50) {
          intreq |= 1 << 3;
          tick -= 50;
        }
        boundary = true;
      }
      /* Timer 0 */
      if (sfr[0x10] & 0xc0) {
        let c0 = 0;
        if ((t0base += cy) >= t0scale)
          do
            c0++;
          while ((t0base -= t0scale) >= t0scale);
        if (c0) {
          if ((sfr[0x10] & 0xe0) === 0xe0) {
            t0l += c0;
            if (t0l >= 256) {
              t0l -= 256;
              if (++t0h >= 256) {
                t0h -= 256;
                if ((t0l += sfr[0x13]) >= 256) {
                  t0l -= 256;
                  if ((t0h += sfr[0x15]) >= 256) {
                    t0l = sfr[0x13];
                    t0h = sfr[0x15];
                  }
                }
                sfr[0x10] |= 10;
                if (sfr[0x10] & 4)
                  intreq |= 1 << 4;
              }
            }
          } else {
            if (sfr[0x10] & 0x40) {
              t0l += c0;
              if (t0l >= 256) {
                t0l -= 256;
                if ((t0l += sfr[0x13]) >= 256)
                  t0l = sfr[0x13];
                sfr[0x10] |= 2;
                if (sfr[0x10] & 1)
                  intreq |= 1 << 2;
              }
            }
            if (sfr[0x10] & 0x80) {
              t0h += c0;
              if (t0h >= 256) {
                t0h -= 256;
                if ((t0h += sfr[0x15]) >= 256)
                  t0h = sfr[0x15];
                sfr[0x10] |= 8;
                if (sfr[0x10] & 4)
                  intreq |= 1 << 4;
              }
            }
          }
        }
      }
      /* Timer 1 */
      if (sfr[0x18] & 0xc0) {
        if ((sfr[0x18] & 0xe0) === 0xe0) {
          t1l += cy;
          if (t1l >= 256) {
            t1l -= 256;
            if (++t1h >= 256) {
              t1h -= 256;
              if ((t1l += sfr[0x1b]) >= 256) {
                t1l -= 256;
                if ((t1h += sfr[0x1d]) >= 256) {
                  t1l = sfr[0x1b];
                  t1h = sfr[0x1d];
                }
              }
              sfr[0x18] |= 10;
              if (sfr[0x18] & 4)
                intreq |= 1 << 5;
            }
          }
        } else {
          if (sfr[0x18] & 0x40) {
            t1l += cy;
            if (t1l >= 256) {
              t1l -= 256;
              if ((t1l += sfr[0x1b]) >= 256)
                t1l = sfr[0x1b];
              sfr[0x18] |= 2;
              if (sfr[0x18] & 1)
                intreq |= 1 << 5;
            }
          }
          if (sfr[0x18] & 0x80) {
            t1h += cy;
            if (t1h >= 256) {
              t1h -= 256;
              if ((t1h += sfr[0x1d]) >= 256)
                t1h = sfr[0x1d];
              sfr[0x18] |= 8;
              if (sfr[0x18] & 4)
                intreq |= 1 << 5;
            }
          }
        }
      }
      if (!(sfr[0x0d] & 1) && !hasbios) {
        if (!(pc = handle_fwcall(pc)))
          break;
        else
          sfr[0x0d] |= 1;
      }

      if ((sfr[0x4e] & 3) === 3 && !imask)
        intreq |= 1 << 9;

      if (intreq && !imask && (sfr[0x08] & 0x80)) {
        for (r = 0; r < 10; r++)
          if (intreq & (1 << r))
            break;
        intreq &= ~(1 << r);
        push(pc & 0xff);
        push((pc & 0xff00) >> 8);
        imask++;
        pc = ((r & ~1) << 3) + ((r & 1) ? 0xb : 0x3);
      }
      if (boundary && ++done >= n)
        break;
    }
    return done;
  }

  return {
    W, H, lcd, flash, hooks, load, setBios, run, dateNow,
    reset: resetcpu,
    /* 0 up, 1 down, 2 left, 3 right, 4 A, 5 B, 6 mode, 7 sleep */
    key: (i, down) => (down ? keypress(i) : keyrelease(i)),
    get halted() { return halted; },
    get gamesize() { return gamesize; },
    get hasBios() { return !!hasbios; },
    get lcdOn() { return !!lcdon; }
  };
})();
)VMUJS";
inline constexpr char standaloneLicense[] = R"VMULICENSE(                    GNU GENERAL PUBLIC LICENSE
                       Version 3, 29 June 2007

 Copyright (C) 2007 Free Software Foundation, Inc. <https://fsf.org/>
 Everyone is permitted to copy and distribute verbatim copies
 of this license document, but changing it is not allowed.

                            Preamble

  The GNU General Public License is a free, copyleft license for
software and other kinds of works.

  The licenses for most software and other practical works are designed
to take away your freedom to share and change the works.  By contrast,
the GNU General Public License is intended to guarantee your freedom to
share and change all versions of a program--to make sure it remains free
software for all its users.  We, the Free Software Foundation, use the
GNU General Public License for most of our software; it applies also to
any other work released this way by its authors.  You can apply it to
your programs, too.

  When we speak of free software, we are referring to freedom, not
price.  Our General Public Licenses are designed to make sure that you
have the freedom to distribute copies of free software (and charge for
them if you wish), that you receive source code or can get it if you
want it, that you can change the software or use pieces of it in new
free programs, and that you know you can do these things.

  To protect your rights, we need to prevent others from denying you
these rights or asking you to surrender the rights.  Therefore, you have
certain responsibilities if you distribute copies of the software, or if
you modify it: responsibilities to respect the freedom of others.

  For example, if you distribute copies of such a program, whether
gratis or for a fee, you must pass on to the recipients the same
freedoms that you received.  You must make sure that they, too, receive
or can get the source code.  And you must show them these terms so they
know their rights.

  Developers that use the GNU GPL protect your rights with two steps:
(1) assert copyright on the software, and (2) offer you this License
giving you legal permission to copy, distribute and/or modify it.

  For the developers' and authors' protection, the GPL clearly explains
that there is no warranty for this free software.  For both users' and
authors' sake, the GPL requires that modified versions be marked as
changed, so that their problems will not be attributed erroneously to
authors of previous versions.

  Some devices are designed to deny users access to install or run
modified versions of the software inside them, although the manufacturer
can do so.  This is fundamentally incompatible with the aim of
protecting users' freedom to change the software.  The systematic
pattern of such abuse occurs in the area of products for individuals to
use, which is precisely where it is most unacceptable.  Therefore, we
have designed this version of the GPL to prohibit the practice for those
products.  If such problems arise substantially in other domains, we
stand ready to extend this provision to those domains in future versions
of the GPL, as needed to protect the freedom of users.

  Finally, every program is threatened constantly by software patents.
States should not allow patents to restrict development and use of
software on general-purpose computers, but in those that do, we wish to
avoid the special danger that patents applied to a free program could
make it effectively proprietary.  To prevent this, the GPL assures that
patents cannot be used to render the program non-free.

  The precise terms and conditions for copying, distribution and
modification follow.

                       TERMS AND CONDITIONS

  0. Definitions.

  "This License" refers to version 3 of the GNU General Public License.

  "Copyright" also means copyright-like laws that apply to other kinds of
works, such as semiconductor masks.

  "The Program" refers to any copyrightable work licensed under this
License.  Each licensee is addressed as "you".  "Licensees" and
"recipients" may be individuals or organizations.

  To "modify" a work means to copy from or adapt all or part of the work
in a fashion requiring copyright permission, other than the making of an
exact copy.  The resulting work is called a "modified version" of the
earlier work or a work "based on" the earlier work.

  A "covered work" means either the unmodified Program or a work based
on the Program.

  To "propagate" a work means to do anything with it that, without
permission, would make you directly or secondarily liable for
infringement under applicable copyright law, except executing it on a
computer or modifying a private copy.  Propagation includes copying,
distribution (with or without modification), making available to the
public, and in some countries other activities as well.

  To "convey" a work means any kind of propagation that enables other
parties to make or receive copies.  Mere interaction with a user through
a computer network, with no transfer of a copy, is not conveying.

  An interactive user interface displays "Appropriate Legal Notices"
to the extent that it includes a convenient and prominently visible
feature that (1) displays an appropriate copyright notice, and (2)
tells the user that there is no warranty for the work (except to the
extent that warranties are provided), that licensees may convey the
work under this License, and how to view a copy of this License.  If
the interface presents a list of user commands or options, such as a
menu, a prominent item in the list meets this criterion.

  1. Source Code.

  The "source code" for a work means the preferred form of the work
for making modifications to it.  "Object code" means any non-source
form of a work.

  A "Standard Interface" means an interface that either is an official
standard defined by a recognized standards body, or, in the case of
interfaces specified for a particular programming language, one that
is widely used among developers working in that language.

  The "System Libraries" of an executable work include anything, other
than the work as a whole, that (a) is included in the normal form of
packaging a Major Component, but which is not part of that Major
Component, and (b) serves only to enable use of the work with that
Major Component, or to implement a Standard Interface for which an
implementation is available to the public in source code form.  A
"Major Component", in this context, means a major essential component
(kernel, window system, and so on) of the specific operating system
(if any) on which the executable work runs, or a compiler used to
produce the work, or an object code interpreter used to run it.

  The "Corresponding Source" for a work in object code form means all
the source code needed to generate, install, and (for an executable
work) run the object code and to modify the work, including scripts to
control those activities.  However, it does not include the work's
System Libraries, or general-purpose tools or generally available free
programs which are used unmodified in performing those activities but
which are not part of the work.  For example, Corresponding Source
includes interface definition files associated with source files for
the work, and the source code for shared libraries and dynamically
linked subprograms that the work is specifically designed to require,
such as by intimate data communication or control flow between those
subprograms and other parts of the work.

  The Corresponding Source need not include anything that users
can regenerate automatically from other parts of the Corresponding
Source.

  The Corresponding Source for a work in source code form is that
same work.

  2. Basic Permissions.

  All rights granted under this License are granted for the term of
copyright on the Program, and are irrevocable provided the stated
conditions are met.  This License explicitly affirms your unlimited
permission to run the unmodified Program.  The output from running a
covered work is covered by this License only if the output, given its
content, constitutes a covered work.  This License acknowledges your
rights of fair use or other equivalent, as provided by copyright law.

  You may make, run and propagate covered works that you do not
convey, without conditions so long as your license otherwise remains
in force.  You may convey covered works to others for the sole purpose
of having them make modifications exclusively for you, or provide you
with facilities for running those works, provided that you comply with
the terms of this License in conveying all material for which you do
not control copyright.  Those thus making or running the covered works
for you must do so exclusively on your behalf, under your direction
and control, on terms that prohibit them from making any copies of
your copyrighted material outside their relationship with you.

  Conveying under any other circumstances is permitted solely under
the conditions stated below.  Sublicensing is not allowed; section 10
makes it unnecessary.

  3. Protecting Users' Legal Rights From Anti-Circumvention Law.

  No covered work shall be deemed part of an effective technological
measure under any applicable law fulfilling obligations under article
11 of the WIPO copyright treaty adopted on 20 December 1996, or
similar laws prohibiting or restricting circumvention of such
measures.

  When you convey a covered work, you waive any legal power to forbid
circumvention of technological measures to the extent such circumvention
is effected by exercising rights under this License with respect to
the covered work, and you disclaim any intention to limit operation or
modification of the work as a means of enforcing, against the work's
users, your or third parties' legal rights to forbid circumvention of
technological measures.

  4. Conveying Verbatim Copies.

  You may convey verbatim copies of the Program's source code as you
receive it, in any medium, provided that you conspicuously and
appropriately publish on each copy an appropriate copyright notice;
keep intact all notices stating that this License and any
non-permissive terms added in accord with section 7 apply to the code;
keep intact all notices of the absence of any warranty; and give all
recipients a copy of this License along with the Program.

  You may charge any price or no price for each copy that you convey,
and you may offer support or warranty protection for a fee.

  5. Conveying Modified Source Versions.

  You may convey a work based on the Program, or the modifications to
produce it from the Program, in the form of source code under the
terms of section 4, provided that you also meet all of these conditions:

    a) The work must carry prominent notices stating that you modified
    it, and giving a relevant date.

    b) The work must carry prominent notices stating that it is
    released under this License and any conditions added under section
    7.  This requirement modifies the requirement in section 4 to
    "keep intact all notices".

    c) You must license the entire work, as a whole, under this
    License to anyone who comes into possession of a copy.  This
    License will therefore apply, along with any applicable section 7
    additional terms, to the whole of the work, and all its parts,
    regardless of how they are packaged.  This License gives no
    permission to license the work in any other way, but it does not
    invalidate such permission if you have separately received it.

    d) If the work has interactive user interfaces, each must display
    Appropriate Legal Notices; however, if the Program has interactive
    interfaces that do not display Appropriate Legal Notices, your
    work need not make them do so.

  A compilation of a covered work with other separate and independent
works, which are not by their nature extensions of the covered work,
and which are not combined with it such as to form a larger program,
in or on a volume of a storage or distribution medium, is called an
"aggregate" if the compilation and its resulting copyright are not
used to limit the access or legal rights of the compilation's users
beyond what the individual works permit.  Inclusion of a covered work
in an aggregate does not cause this License to apply to the other
parts of the aggregate.

  6. Conveying Non-Source Forms.

  You may convey a covered work in object code form under the terms
of sections 4 and 5, provided that you also convey the
machine-readable Corresponding Source under the terms of this License,
in one of these ways:

    a) Convey the object code in, or embodied in, a physical product
    (including a physical distribution medium), accompanied by the
    Corresponding Source fixed on a durable physical medium
    customarily used for software interchange.

    b) Convey the object code in, or embodied in, a physical product
    (including a physical distribution medium), accompanied by a
    written offer, valid for at least three years and valid for as
    long as you offer spare parts or customer support for that product
    model, to give anyone who possesses the object code either (1) a
    copy of the Corresponding Source for all the software in the
    product that is covered by this License, on a durable physical
    medium customarily used for software interchange, for a price no
    more than your reasonable cost of physically performing this
    conveying of source, or (2) access to copy the
    Corresponding Source from a network server at no charge.

    c) Convey individual copies of the object code with a copy of the
    written offer to provide the Corresponding Source.  This
    alternative is allowed only occasionally and noncommercially, and
    only if you received the object code with such an offer, in accord
    with subsection 6b.

    d) Convey the object code by offering access from a designated
    place (gratis or for a charge), and offer equivalent access to the
    Corresponding Source in the same way through the same place at no
    further charge.  You need not require recipients to copy the
    Corresponding Source along with the object code.  If the place to
    copy the object code is a network server, the Corresponding Source
    may be on a different server (operated by you or a third party)
    that supports equivalent copying facilities, provided you maintain
    clear directions next to the object code saying where to find the
    Corresponding Source.  Regardless of what server hosts the
    Corresponding Source, you remain obligated to ensure that it is
    available for as long as needed to satisfy these requirements.

    e) Convey the object code using peer-to-peer transmission, provided
    you inform other peers where the object code and Corresponding
    Source of the work are being offered to the general public at no
    charge under subsection 6d.

  A separable portion of the object code, whose source code is excluded
from the Corresponding Source as a System Library, need not be
included in conveying the object code work.

  A "User Product" is either (1) a "consumer product", which means any
tangible personal property which is normally used for personal, family,
or household purposes, or (2) anything designed or sold for incorporation
into a dwelling.  In determining whether a product is a consumer product,
doubtful cases shall be resolved in favor of coverage.  For a particular
product received by a particular user, "normally used" refers to a
typical or common use of that class of product, regardless of the status
of the particular user or of the way in which the particular user
actually uses, or expects or is expected to use, the product.  A product
is a consumer product regardless of whether the product has substantial
commercial, industrial or non-consumer uses, unless such uses represent
the only significant mode of use of the product.

  "Installation Information" for a User Product means any methods,
procedures, authorization keys, or other information required to install
and execute modified versions of a covered work in that User Product from
a modified version of its Corresponding Source.  The information must
suffice to ensure that the continued functioning of the modified object
code is in no case prevented or interfered with solely because
modification has been made.

  If you convey an object code work under this section in, or with, or
specifically for use in, a User Product, and the conveying occurs as
part of a transaction in which the right of possession and use of the
User Product is transferred to the recipient in perpetuity or for a
fixed term (regardless of how the transaction is characterized), the
Corresponding Source conveyed under this section must be accompanied
by the Installation Information.  But this requirement does not apply
if neither you nor any third party retains the ability to install
modified object code on the User Product (for example, the work has
been installed in ROM).

  The requirement to provide Installation Information does not include a
requirement to continue to provide support service, warranty, or updates
for a work that has been modified or installed by the recipient, or for
the User Product in which it has been modified or installed.  Access to a
network may be denied when the modification itself materially and
adversely affects the operation of the network or violates the rules and
protocols for communication across the network.

  Corresponding Source conveyed, and Installation Information provided,
in accord with this section must be in a format that is publicly
documented (and with an implementation available to the public in
source code form), and must require no special password or key for
unpacking, reading or copying.

  7. Additional Terms.

  "Additional permissions" are terms that supplement the terms of this
License by making exceptions from one or more of its conditions.
Additional permissions that are applicable to the entire Program shall
be treated as though they were included in this License, to the extent
that they are valid under applicable law.  If additional permissions
apply only to part of the Program, that part may be used separately
under those permissions, but the entire Program remains governed by
this License without regard to the additional permissions.

  When you convey a copy of a covered work, you may at your option
remove any additional permissions from that copy, or from any part of
it.  (Additional permissions may be written to require their own
removal in certain cases when you modify the work.)  You may place
additional permissions on material, added by you to a covered work,
for which you have or can give appropriate copyright permission.

  Notwithstanding any other provision of this License, for material you
add to a covered work, you may (if authorized by the copyright holders of
that material) supplement the terms of this License with terms:

    a) Disclaiming warranty or limiting liability differently from the
    terms of sections 15 and 16 of this License; or

    b) Requiring preservation of specified reasonable legal notices or
    author attributions in that material or in the Appropriate Legal
    Notices displayed by works containing it; or

    c) Prohibiting misrepresentation of the origin of that material, or
    requiring that modified versions of such material be marked in
    reasonable ways as different from the original version; or

    d) Limiting the use for publicity purposes of names of licensors or
    authors of the material; or

    e) Declining to grant rights under trademark law for use of some
    trade names, trademarks, or service marks; or

    f) Requiring indemnification of licensors and authors of that
    material by anyone who conveys the material (or modified versions of
    it) with contractual assumptions of liability to the recipient, for
    any liability that these contractual assumptions directly impose on
    those licensors and authors.

  All other non-permissive additional terms are considered "further
restrictions" within the meaning of section 10.  If the Program as you
received it, or any part of it, contains a notice stating that it is
governed by this License along with a term that is a further
restriction, you may remove that term.  If a license document contains
a further restriction but permits relicensing or conveying under this
License, you may add to a covered work material governed by the terms
of that license document, provided that the further restriction does
not survive such relicensing or conveying.

  If you add terms to a covered work in accord with this section, you
must place, in the relevant source files, a statement of the
additional terms that apply to those files, or a notice indicating
where to find the applicable terms.

  Additional terms, permissive or non-permissive, may be stated in the
form of a separately written license, or stated as exceptions;
the above requirements apply either way.

  8. Termination.

  You may not propagate or modify a covered work except as expressly
provided under this License.  Any attempt otherwise to propagate or
modify it is void, and will automatically terminate your rights under
this License (including any patent licenses granted under the third
paragraph of section 11).

  However, if you cease all violation of this License, then your
license from a particular copyright holder is reinstated (a)
provisionally, unless and until the copyright holder explicitly and
finally terminates your license, and (b) permanently, if the copyright
holder fails to notify you of the violation by some reasonable means
prior to 60 days after the cessation.

  Moreover, your license from a particular copyright holder is
reinstated permanently if the copyright holder notifies you of the
violation by some reasonable means, this is the first time you have
received notice of violation of this License (for any work) from that
copyright holder, and you cure the violation prior to 30 days after
your receipt of the notice.

  Termination of your rights under this section does not terminate the
licenses of parties who have received copies or rights from you under
this License.  If your rights have been terminated and not permanently
reinstated, you do not qualify to receive new licenses for the same
material under section 10.

  9. Acceptance Not Required for Having Copies.

  You are not required to accept this License in order to receive or
run a copy of the Program.  Ancillary propagation of a covered work
occurring solely as a consequence of using peer-to-peer transmission
to receive a copy likewise does not require acceptance.  However,
nothing other than this License grants you permission to propagate or
modify any covered work.  These actions infringe copyright if you do
not accept this License.  Therefore, by modifying or propagating a
covered work, you indicate your acceptance of this License to do so.

  10. Automatic Licensing of Downstream Recipients.

  Each time you convey a covered work, the recipient automatically
receives a license from the original licensors, to run, modify and
propagate that work, subject to this License.  You are not responsible
for enforcing compliance by third parties with this License.

  An "entity transaction" is a transaction transferring control of an
organization, or substantially all assets of one, or subdividing an
organization, or merging organizations.  If propagation of a covered
work results from an entity transaction, each party to that
transaction who receives a copy of the work also receives whatever
licenses to the work the party's predecessor in interest had or could
give under the previous paragraph, plus a right to possession of the
Corresponding Source of the work from the predecessor in interest, if
the predecessor has it or can get it with reasonable efforts.

  You may not impose any further restrictions on the exercise of the
rights granted or affirmed under this License.  For example, you may
not impose a license fee, royalty, or other charge for exercise of
rights granted under this License, and you may not initiate litigation
(including a cross-claim or counterclaim in a lawsuit) alleging that
any patent claim is infringed by making, using, selling, offering for
sale, or importing the Program or any portion of it.

  11. Patents.

  A "contributor" is a copyright holder who authorizes use under this
License of the Program or a work on which the Program is based.  The
work thus licensed is called the contributor's "contributor version".

  A contributor's "essential patent claims" are all patent claims
owned or controlled by the contributor, whether already acquired or
hereafter acquired, that would be infringed by some manner, permitted
by this License, of making, using, or selling its contributor version,
but do not include claims that would be infringed only as a
consequence of further modification of the contributor version.  For
purposes of this definition, "control" includes the right to grant
patent sublicenses in a manner consistent with the requirements of
this License.

  Each contributor grants you a non-exclusive, worldwide, royalty-free
patent license under the contributor's essential patent claims, to
make, use, sell, offer for sale, import and otherwise run, modify and
propagate the contents of its contributor version.

  In the following three paragraphs, a "patent license" is any express
agreement or commitment, however denominated, not to enforce a patent
(such as an express permission to practice a patent or covenant not to
sue for patent infringement).  To "grant" such a patent license to a
party means to make such an agreement or commitment not to enforce a
patent against the party.

  If you convey a covered work, knowingly relying on a patent license,
and the Corresponding Source of the work is not available for anyone
to copy, free of charge and under the terms of this License, through a
publicly available network server or other readily accessible means,
then you must either (1) cause the Corresponding Source to be so
available, or (2) arrange to deprive yourself of the benefit of the
patent license for this particular work, or (3) arrange, in a manner
consistent with the requirements of this License, to extend the patent
license to downstream recipients.  "Knowingly relying" means you have
actual knowledge that, but for the patent license, your conveying the
covered work in a country, or your recipient's use of the covered work
in a country, would infringe one or more identifiable patents in that
country that you have reason to believe are valid.

  If, pursuant to or in connection with a single transaction or
arrangement, you convey, or propagate by procuring conveyance of, a
covered work, and grant a patent license to some of the parties
receiving the covered work authorizing them to use, propagate, modify
or convey a specific copy of the covered work, then the patent license
you grant is automatically extended to all recipients of the covered
work and works based on it.

  A patent license is "discriminatory" if it does not include within
the scope of its coverage, prohibits the exercise of, or is
conditioned on the non-exercise of one or more of the rights that are
specifically granted under this License.  You may not convey a covered
work if you are a party to an arrangement with a third party that is
in the business of distributing software, under which you make payment
to the third party based on the extent of your activity of conveying
the work, and under which the third party grants, to any of the
parties who would receive the covered work from you, a discriminatory
patent license (a) in connection with copies of the covered work
conveyed by you (or copies made from those copies), or (b) primarily
for and in connection with specific products or compilations that
contain the covered work, unless you entered into that arrangement,
or that patent license was granted, prior to 28 March 2007.

  Nothing in this License shall be construed as excluding or limiting
any implied license or other defenses to infringement that may
otherwise be available to you under applicable patent law.

  12. No Surrender of Others' Freedom.

  If conditions are imposed on you (whether by court order, agreement or
otherwise) that contradict the conditions of this License, they do not
excuse you from the conditions of this License.  If you cannot convey a
covered work so as to satisfy simultaneously your obligations under this
License and any other pertinent obligations, then as a consequence you may
not convey it at all.  For example, if you agree to terms that obligate you
to collect a royalty for further conveying from those to whom you convey
the Program, the only way you could satisfy both those terms and this
License would be to refrain entirely from conveying the Program.

  13. Use with the GNU Affero General Public License.

  Notwithstanding any other provision of this License, you have
permission to link or combine any covered work with a work licensed
under version 3 of the GNU Affero General Public License into a single
combined work, and to convey the resulting work.  The terms of this
License will continue to apply to the part which is the covered work,
but the special requirements of the GNU Affero General Public License,
section 13, concerning interaction through a network will apply to the
combination as such.

  14. Revised Versions of this License.

  The Free Software Foundation may publish revised and/or new versions of
the GNU General Public License from time to time.  Such new versions will
be similar in spirit to the present version, but may differ in detail to
address new problems or concerns.

  Each version is given a distinguishing version number.  If the
Program specifies that a certain numbered version of the GNU General
Public License "or any later version" applies to it, you have the
option of following the terms and conditions either of that numbered
version or of any later version published by the Free Software
Foundation.  If the Program does not specify a version number of the
GNU General Public License, you may choose any version ever published
by the Free Software Foundation.

  If the Program specifies that a proxy can decide which future
versions of the GNU General Public License can be used, that proxy's
public statement of acceptance of a version permanently authorizes you
to choose that version for the Program.

  Later license versions may give you additional or different
permissions.  However, no additional obligations are imposed on any
author or copyright holder as a result of your choosing to follow a
later version.

  15. Disclaimer of Warranty.

  THERE IS NO WARRANTY FOR THE PROGRAM, TO THE EXTENT PERMITTED BY
APPLICABLE LAW.  EXCEPT WHEN OTHERWISE STATED IN WRITING THE COPYRIGHT
HOLDERS AND/OR OTHER PARTIES PROVIDE THE PROGRAM "AS IS" WITHOUT WARRANTY
OF ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE.  THE ENTIRE RISK AS TO THE QUALITY AND PERFORMANCE OF THE PROGRAM
IS WITH YOU.  SHOULD THE PROGRAM PROVE DEFECTIVE, YOU ASSUME THE COST OF
ALL NECESSARY SERVICING, REPAIR OR CORRECTION.

  16. Limitation of Liability.

  IN NO EVENT UNLESS REQUIRED BY APPLICABLE LAW OR AGREED TO IN WRITING
WILL ANY COPYRIGHT HOLDER, OR ANY OTHER PARTY WHO MODIFIES AND/OR CONVEYS
THE PROGRAM AS PERMITTED ABOVE, BE LIABLE TO YOU FOR DAMAGES, INCLUDING ANY
GENERAL, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES ARISING OUT OF THE
USE OR INABILITY TO USE THE PROGRAM (INCLUDING BUT NOT LIMITED TO LOSS OF
DATA OR DATA BEING RENDERED INACCURATE OR LOSSES SUSTAINED BY YOU OR THIRD
PARTIES OR A FAILURE OF THE PROGRAM TO OPERATE WITH ANY OTHER PROGRAMS),
EVEN IF SUCH HOLDER OR OTHER PARTY HAS BEEN ADVISED OF THE POSSIBILITY OF
SUCH DAMAGES.

  17. Interpretation of Sections 15 and 16.

  If the disclaimer of warranty and limitation of liability provided
above cannot be given local legal effect according to their terms,
reviewing courts shall apply local law that most closely approximates
an absolute waiver of all civil liability in connection with the
Program, unless a warranty or assumption of liability accompanies a
copy of the Program in return for a fee.

                     END OF TERMS AND CONDITIONS

            How to Apply These Terms to Your New Programs

  If you develop a new program, and you want it to be of the greatest
possible use to the public, the best way to achieve this is to make it
free software which everyone can redistribute and change under these terms.

  To do so, attach the following notices to the program.  It is safest
to attach them to the start of each source file to most effectively
state the exclusion of warranty; and each file should have at least
the "copyright" line and a pointer to where the full notice is found.

    <one line to give the program's name and a brief idea of what it does.>
    Copyright (C) <year>  <name of author>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.

Also add information on how to contact you by electronic and paper mail.

  If the program does terminal interaction, make it output a short
notice like this when it starts in an interactive mode:

    <program>  Copyright (C) <year>  <name of author>
    This program comes with ABSOLUTELY NO WARRANTY; for details type `show w'.
    This is free software, and you are welcome to redistribute it
    under certain conditions; type `show c' for details.

The hypothetical commands `show w' and `show c' should show the appropriate
parts of the General Public License.  Of course, your program's commands
might be different; for a GUI interface, you would use an "about box".

  You should also get your employer (if you work as a programmer) or school,
if any, to sign a "copyright disclaimer" for the program, if necessary.
For more information on this, and how to apply and follow the GNU GPL, see
<https://www.gnu.org/licenses/>.

  The GNU General Public License does not permit incorporating your program
into proprietary programs.  If your program is a subroutine library, you
may consider it more useful to permit linking proprietary applications with
the library.  If this is what you want to do, use the GNU Lesser General
Public License instead of this License.  But first, please read
<https://www.gnu.org/licenses/why-not-lgpl.html>.
)VMULICENSE";
}
