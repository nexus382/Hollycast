// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "types.h"
#include "network/net_platform.h"

#include <vector>

extern "C" {
#include <pico_ipv4.h>
#include <pico_dns_common.h>
}

pico_ip4 parseDnsResponsePacket(const void *buf, size_t len);

namespace
{
void appendBytes(std::vector<u8>& packet, const void *data, size_t length)
{
	const u8 *bytes = static_cast<const u8 *>(data);
	packet.insert(packet.end(), bytes, bytes + length);
}

std::vector<u8> questionPacket(u16 answerCount = 0)
{
	pico_dns_packet header{};
	header.qdcount = htons(1);
	header.ancount = htons(answerCount);
	std::vector<u8> packet;
	appendBytes(packet, &header, sizeof(header));
	packet.push_back(0); // root question name
	pico_dns_question_suffix question{};
	question.qtype = htons(PICO_DNS_TYPE_A);
	question.qclass = htons(PICO_DNS_CLASS_IN);
	appendBytes(packet, &question, sizeof(question));
	return packet;
}

void appendAnswer(std::vector<u8>& packet, u16 type, const std::vector<u8>& data)
{
	const u8 compressedRootName[] = { 0xc0, 0x0c };
	appendBytes(packet, compressedRootName, sizeof(compressedRootName));
	pico_dns_record_suffix record{};
	record.rtype = htons(type);
	record.rclass = htons(PICO_DNS_CLASS_IN);
	record.rdlength = htons(static_cast<u16>(data.size()));
	appendBytes(packet, &record, sizeof(record));
	if (!data.empty())
		appendBytes(packet, data.data(), data.size());
}

pico_ip4 parse(const std::vector<u8>& packet)
{
	return parseDnsResponsePacket(packet.data(), packet.size());
}
}

TEST(DnsParserTest, ParsesARecord)
{
	auto packet = questionPacket(1);
	appendAnswer(packet, PICO_DNS_TYPE_A, { 192, 0, 2, 44 });
	EXPECT_EQ(parse(packet).addr, htonl(0xc000022c));
}

TEST(DnsParserTest, SkipsNonARecordBeforeARecord)
{
	auto packet = questionPacket(2);
	appendAnswer(packet, PICO_DNS_TYPE_AAAA, std::vector<u8>(16, 0));
	appendAnswer(packet, PICO_DNS_TYPE_A, { 198, 51, 100, 7 });
	EXPECT_EQ(parse(packet).addr, htonl(0xc6336407));
}
