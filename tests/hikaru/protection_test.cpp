// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 The Hollycast Authors
#include "hw/hikaru/protection.h"
#include <array>
#include <iostream>
#include <stdexcept>

int main()
{
    unsigned failures=0;
    auto check=[&](bool ok,const char* label){if(!ok){++failures;std::cerr<<label<<'\n';}};
    auto throws=[&](auto action,const char* label){try{action();check(false,label);}catch(const std::exception&) {}};
    hikaru::Protection first, second;
    first.write(0x0a000000,0x1234,2);
    first.write(0x0a000002,0x2468,2);
    check(first.read(0x0a000000,4)==0x24681234,"firmware protection RAM presence probe");
    check(second.read(0x0a000000,4)==0,"RAM instances are isolated");
    first.write(0x0a00fff8,0xfedcba9876543210ULL,8);
    check(first.read(0x0a00fff8,8)==0xfedcba9876543210ULL,"64-bit RAM boundary preserves every byte");
    check(first.read(0x0a00ffff,1)==0xfe,"RAM little endian lanes");
    throws([&]{first.read(0x0a000001,2);},"unaligned RAM access rejected");
    throws([&]{first.read(0x0a01000e,8);},"wide crypto read rejected without side effect");
    throws([&]{first.write(0x0a01000a,1,2);},"unsupported high crypto address rejected");
    throws([&]{first.read(0x0a020000,2);},"unmapped board read rejected");
    throws([&]{first.write(0x0a010002,0,2);},"unmapped register write rejected");

    // The base ROM-board map installs neither protection RAM nor crypto.
    hikaru::Protection absent(std::nullopt);
    constexpr std::array<unsigned,4> widths{{1,2,4,8}};
    constexpr std::array<uint64_t,4> openBus{{0xff,0xffff,0xffffffffULL,UINT64_MAX}};
    for(unsigned i=0;i<widths.size();++i) {
        for(uint32_t address : {0x0a000000u,0x0a00fff8u,0x0a010008u}) {
            absent.write(address,0,widths[i]);
            check(absent.read(address,widths[i])==openBus[i],"absent protection window reads open bus");
        }
    }
    absent.write(0x0a01000c,0x1234,2);
    check(absent.read(0x0a01000e,2)==0xffff,"absent chip ignores register writes");
    absent.reset();
    check(absent.read(0x0a00fff8,8)==UINT64_MAX,"absent RAM remains open bus after reset");
    check(absent.read(0x0a010000,2)==0xffff,"absent chip stays absent after reset");
    throws([&]{absent.read(0x0a010001,2);},"absent protection retains bus alignment checks");
    throws([&]{absent.read(0x0a020000,2);},"absent protection does not expand mapping");

    // Generated ciphertext, NOT game data. LCG seed 2; golden values obtained
    // independently from Hollycast's existing Naomi 315-5881 implementation.
    // Key 291b02c7, sequence key 1234, address 0; decoded header 1e00c (raw stream).
    constexpr std::array<uint16_t,16> golden{{0x58ba,0x2be7,0xe346,0xb7a8,
        0x35ce,0x48ce,0xffb2,0x645b,0x545d,0xcda6,0xc350,0xb61e,
        0xcb80,0x3661,0xf1cd,0x9d42}};
    auto fill=[](hikaru::Protection& board){
        uint32_t state=2;
        for(uint32_t i=0;i<65536;++i){state=state*1664525u+1013904223u;board.write(0x0a000000+i,state>>24,1);}
        board.write(0x0a01000a,0,2);
        board.write(0x0a010008,0,2);
        board.write(0x0a01000c,0x1234,2);
    };
    fill(first);fill(second);
    check(first.read(0x0a010000,2)==0,"synchronous ready status");
    check(first.read(0x0a01000e,2)==0,"first read is dummy after address write");
    check(second.read(0x0a01000e,2)==0,"dummy state is per instance");
    for(auto expected:golden){
        check(first.read(0x0a01000e,2)==expected,"generated uncompressed stream golden");
        check(second.read(0x0a01000e,2)==expected,"interleaved instances do not share cipher cursor");
    }
    first.reset();
    check(first.read(0x0a00fff8,8)==0,"hard reset clears protection RAM");
    fill(first);check(first.read(0x0a01000e,2)==0,"reset restores dummy-read sequence");
    for(auto expected:golden)check(first.read(0x0a01000e,2)==expected,"reset preserves key and clears all cipher history");
    // Generated valid compressed stream: one 16-byte line containing two
    // eight-byte literal runs of 0x5a. Inverted with the independent Naomi
    // block cipher, then verified through its full stream decoder.
    first.reset();
    constexpr std::array<uint16_t,4> compressed{{0xbaf7,0x968c,0x8b05,0x5e9f}};
    for(unsigned i=0;i<compressed.size();++i){
        first.write(0x0a000000+2*i,compressed[i]>>8,1);
        first.write(0x0a000001+2*i,compressed[i]&255,1);
    }
    first.write(0x0a01000a,0,2);first.write(0x0a010008,0,2);first.write(0x0a01000c,0x1234,2);
    check(first.read(0x0a01000e,2)==0,"compressed stream initial dummy");
    for(unsigned i=0;i<8;++i)check(first.read(0x0a01000e,2)==0x5a5a,"generated compressed literal runs");
    // Same generated literal run, but an encrypted header declaring a
    // two-byte line. Reject the eight-byte run before writing beyond it.
    first.write(0x0a000000,0x07,1);first.write(0x0a000001,0x64,1);
    first.write(0x0a01000a,0,2);first.write(0x0a010008,0,2);first.write(0x0a01000c,0x1234,2);
    check(first.read(0x0a01000e,2)==0,"malformed stream dummy");
    throws([&]{first.read(0x0a01000e,2);},"compressed run exceeding declared line fails safely");
    first.reset();fill(first);first.read(0x0a01000e,2);
    check(first.read(0x0a01000e,2)==golden[0],"reset recovers after invalid compressed data");
    if(failures)return 1;
    std::cout<<"Hikaru protection tests passed\n";
}
