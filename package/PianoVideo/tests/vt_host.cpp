// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoVideoVTWire.h"
#include <cstdio>
#include <cstring>
#include <cstdint>

extern "C" {
unsigned droidvm_media_devices(void);
uint32_t droidvm_media_caps(unsigned);
const char *droidvm_media_card(unsigned);
bool droidvm_media_is_up(unsigned);
int droidvm_media_open(unsigned, uint32_t *);
int droidvm_media_ioctl(unsigned, uint32_t, uint32_t, void *, uint32_t, uint32_t);
}

static int failures;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL:%s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

int main()
{
    using namespace vtwire;
    uint8_t b[256] = {};
    Writer w(b, sizeof b);
    uint8_t uuid[16] = {};
    w.header(cc("decs"), FlagRequest, uuid, 9);
    w.s32(cc(",cod"), int32_t(cc("avc1")));
    CHECK(w.ok());
    Header h;
    CHECK(header(b, w.size(), h) && h.type == cc("decs") && h.flags == FlagRequest && h.request == 9);
    int32_t codec = 0;
    CHECK(paramS32(b, w.size(), cc(",cod"), codec));
    CHECK(uint32_t(codec) == cc("avc1"));
    CHECK(droidvm_media_devices() == 1);
    CHECK(droidvm_media_is_up(0));
    CHECK(droidvm_media_caps(0) == 3);
    CHECK(std::strcmp(droidvm_media_card(0), "PianoVideo decoder") == 0);
    uint32_t session = 0;
    CHECK(droidvm_media_open(0, &session) < 0);
    CHECK(droidvm_media_ioctl(0, 0, 0, nullptr, 0, 0) < 0);
    if (failures) return 1;
    std::puts("PianoVideo VT V0b host tests: wire and refusing stub pass");
    return 0;
}
