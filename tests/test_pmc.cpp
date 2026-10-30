// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
//
// The PTP management messages Inlet sends linuxptp's ptp4l, against what
// linuxptp's own pmc sends, and the answers it reads back.

#include "check.h"

#include "../src/ptp/src/Pmc.h"

#include <cstring>
#include <string>
#include <vector>

using namespace inlet::pmc;

static std::vector<uint8_t> hex(const std::string& h)
{
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        out.push_back((uint8_t) std::stoi(h.substr(i, 2), nullptr, 16));
    return out;
}

static void requests()
{
    // `pmc -u -b 0 'GET TIME_STATUS_NP'` from process 261, captured.
    const auto pmc = hex("0d02006800000000000000000000000000000000000000000000000001050000047fffffffffffffffffffff"
                         "0000000000010034c000" + std::string(100, '0'));
    const auto ours = encodeGet(kTimeStatusNp, 0, 0, 261);
    CHECK_EQ(ours.size(), pmc.size());
    CHECK(ours == pmc);
    // The data a GET carries is the size of the answer, as pmc sends it.
    CHECK_EQ(encodeGet(kDefaultDataSet, 0, 1, 1).size(), 74u);
    CHECK_EQ(encodeGet(kCurrentDataSet, 0, 1, 1).size(), 72u);
    CHECK_EQ(encodeGet(kParentDataSet, 0, 1, 1).size(), 86u);
    CHECK_EQ(encodeGet(kTimePropertiesDataSet, 0, 1, 1).size(), 58u);
    CHECK_EQ(encodeGet(kPortDataSet, 0, 1, 1).size(), 80u);
    CHECK_EQ(encodeGet(kPortPropertiesNp, 0, 1, 1).size(), 54u);
    // The domain and sequence go where ptp4l reads them.
    const auto d = encodeGet(kPortDataSet, 3, 0x1234, 7);
    CHECK_EQ(d[4], 3);
    CHECK_EQ(d[30], 0x12);
    CHECK_EQ(d[31], 0x34);
}

static void responses()
{
    // A TIME_STATUS_NP answer: offset -1500 ns at ingress 1 000 000 000 123 ns,
    // grandmaster present.
    auto m = encodeGet(kTimeStatusNp, 0, 9, 1);
    m[46] = 2;   // RESPONSE
    uint8_t* d = &m[54];
    const int64_t offset = -1500, ingress = 1'000'000'000'123LL;
    for (int i = 0; i < 8; ++i)
    {
        d[i] = (uint8_t) ((uint64_t) offset >> (56 - 8 * i));
        d[8 + i] = (uint8_t) ((uint64_t) ingress >> (56 - 8 * i));
    }
    d[41] = 1;                                     // gmPresent
    std::memcpy(d + 42, "\x00\x1D\xC1\xFF\xFE\x12\x34\x56", 8);
    auto r = decode(m.data(), m.size());
    CHECK(r.has_value());
    if (!r)
        return;
    CHECK_EQ(r->id, kTimeStatusNp);
    CHECK_EQ(r->sequence, 9);
    CHECK(!r->error);
    auto ts = parseTimeStatus(r->data);
    CHECK(ts.has_value());
    if (ts)
    {
        CHECK_EQ(ts->masterOffsetNs, -1500);
        CHECK_EQ(ts->ingressNs, ingress);
        CHECK(ts->gmPresent);
        CHECK_EQ(ts->gmIdentity[1], 0x1D);
    }

    // PORT_PROPERTIES_NP: port state, hardware stamps, the interface's name.
    std::vector<uint8_t> pp(10, 0);
    pp.push_back(9);    // SLAVE
    pp.push_back(1);    // TS_HARDWARE
    pp.push_back(4);
    for (char c : std::string("eth0"))
        pp.push_back((uint8_t) c);
    auto props = parsePortProperties(pp);
    CHECK(props.has_value());
    if (props)
    {
        CHECK_EQ(props->state, 9);
        CHECK(props->hardwareTimestamps);
        CHECK(props->interfaceName == "eth0");
    }
    CHECK(std::string(portStateName(9)) == "SLAVE");

    // A management error status is an answer too, flagged.
    auto e = encodeGet(kTimeStatusNp, 0, 4, 1);
    e[48] = 0x00;
    e[49] = 0x02;   // MANAGEMENT_ERROR_STATUS
    e[50] = 0x00;
    e[51] = 0x08;
    e[52] = 0x00;
    e[53] = 0x02;   // NO_SUCH_ID
    e[54] = 0xC0;
    e[55] = 0x00;
    auto er = decode(e.data(), e.size());
    CHECK(er.has_value() && er->error && er->id == kTimeStatusNp);

    // Not a management message, or cut short: nothing.
    CHECK(!decode(m.data(), 40).has_value());
    m[0] = 0x00;
    CHECK(!decode(m.data(), m.size()).has_value());
}

int main()
{
    requests();
    responses();
    TEST_MAIN_END();
}
