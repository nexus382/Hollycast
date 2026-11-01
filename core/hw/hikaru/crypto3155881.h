// SPDX-License-Identifier: BSD-3-Clause
// Derived from MAME 315-5881_crypt.h; copyright David Haywood.
// See crypto3155881.cpp for full license and pinned source provenance.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
namespace hikaru {
class Crypto3155881 {
public:
    using ReadWord = std::function<uint16_t(uint32_t)>;
    Crypto3155881(uint32_t key, ReadWord read);
    void reset();
    void writeAddressLow(uint16_t value);
    void writeAddressHigh(uint16_t value);
    void writeSubkey(uint16_t value);
    uint16_t read();
private:
    ReadWord m_read;
    uint16_t do_decrypt(uint8_t *&base);
    void set_addr_low(uint16_t data);
    void set_addr_high(uint16_t data);
    void set_subkey(uint16_t data);
	bool first_read = false;

	enum {
//        BUFFER_SIZE = 32768, LINE_SIZE = 512,
		BUFFER_SIZE = 2, LINE_SIZE = 512,  // this should be a stream, without any 'BUFFER_SIZE' ? I guess the SH4 DMA implementation isn't on a timer tho?
		FLAG_COMPRESSED = 0x20000
	};

	uint32_t key = 0;

	std::unique_ptr<uint8_t[]> buffer;
	std::unique_ptr<uint8_t[]> line_buffer;
	std::unique_ptr<uint8_t[]> line_buffer_prev;
	uint32_t prot_cur_address = 0;
	uint16_t subkey = 0, dec_hist = 0;
	uint32_t dec_header = 0;

	bool enc_ready = 0;

	int buffer_pos = 0, line_buffer_pos = 0, line_buffer_size = 0, buffer_bit = 0, buffer_bit2 = 0;
	uint8_t buffer2[2]{};
	uint16_t buffer2a = 0;

	int block_size = 0;
	int block_pos = 0;
	int block_numlines = 0;
	int done_compression = 0;

	struct sbox {
		uint8_t table[64];
		int inputs[6];      // positions of the inputs bits, -1 means no input except from key
		int outputs[2];     // positions of the output bits
	};

	static const sbox fn1_sboxes[4][4];
	static const sbox fn2_sboxes[4][4];

	static const int FN1GK = 38;
	static const int FN2GK = 32;
	static const int fn1_game_key_scheduling[FN1GK][2];
	static const int fn2_game_key_scheduling[FN2GK][2];
	static const int fn1_sequence_key_scheduling[20][2];
	static const int fn2_sequence_key_scheduling[16];
	static const int fn2_middle_result_scheduling[16];

	static const uint8_t trees[9][2][32];

	int feistel_function(int input, const struct sbox *sboxes, uint32_t subkeys);
	uint16_t block_decrypt(uint32_t game_key, uint16_t sequence_key, uint16_t counter, uint16_t data);

	uint16_t get_decrypted_16();
	int get_compressed_bit();

	void enc_start();
	void enc_fill();
	void line_fill();

};
} // namespace hikaru
