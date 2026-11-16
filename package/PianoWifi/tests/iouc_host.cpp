// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host contract test for the IOUserClient OOL structure path. The kernel
// receives an IOExternalMethodArguments block whose large input/output sides
// are descriptors; this fake keeps the same readBytes/writeBytes contract.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. iouc_host.cpp -o /tmp/iouc_host && /tmp/iouc_host
#include "PianoWifiUcTransport.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

struct FakeDescriptor {
  std::vector<uint8_t> bytes;
  uint64_t readBytes(uint64_t offset, void *out, uint64_t length)
  {
    if (offset > bytes.size() || length > bytes.size() - offset) return 0;
    for (uint64_t i = 0; i < length; ++i) static_cast<uint8_t *>(out)[i] = bytes[size_t(offset + i)];
    return length;
  }
  uint64_t writeBytes(uint64_t offset, const void *in, uint64_t length)
  {
    if (offset > bytes.size() || length > bytes.size() - offset) return 0;
    for (uint64_t i = 0; i < length; ++i) bytes[size_t(offset + i)] = static_cast<const uint8_t *>(in)[i];
    return length;
  }
};

struct FakeArguments {
  const void *structureInput = nullptr;
  uint32_t structureInputSize = 0;
  FakeDescriptor *structureInputDescriptor = nullptr;
  void *structureOutput = nullptr;
  uint32_t structureOutputSize = 0;
  FakeDescriptor *structureOutputDescriptor = nullptr;
  uint32_t structureOutputDescriptorSize = 0;
};

int main()
{
  using pianowifi::a80211::readDescriptor;
  using pianowifi::a80211::writeDescriptor;
  constexpr uint32_t requestBytes = 0x3c30;
  constexpr uint32_t replyBytes = 0x2720;
  FakeDescriptor input{std::vector<uint8_t>(requestBytes, 0x5a)};
  FakeArguments args;
  args.structureInputDescriptor = &input;
  assert(args.structureInput == nullptr && args.structureInputSize == 0);
  std::vector<uint8_t> staged(requestBytes, 0);
  assert(readDescriptor(args.structureInputDescriptor, input.bytes.size(), staged.data(), requestBytes));
  assert(staged[0] == 0x5a && staged[requestBytes - 1] == 0x5a);

  FakeDescriptor shortInput{std::vector<uint8_t>(requestBytes - 1, 0)};
  assert(!readDescriptor(&shortInput, shortInput.bytes.size(), staged.data(), requestBytes));

  FakeDescriptor output{std::vector<uint8_t>(replyBytes, 0)};
  args.structureOutputDescriptor = &output;
  args.structureOutputDescriptorSize = replyBytes;
  const uint8_t marker[4] = {0x22, 0x11, 0xaa, 0x55};
  assert(writeDescriptor(args.structureOutputDescriptor, args.structureOutputDescriptorSize, 0x271c, marker, sizeof marker));
  assert(output.bytes[0x271c] == 0x22 && output.bytes[0x271f] == 0x55);
  assert(!writeDescriptor(&output, args.structureOutputDescriptorSize, 0x271f, marker, sizeof marker));
  std::puts("iouc_host: all checks passed");
  return 0;
}
