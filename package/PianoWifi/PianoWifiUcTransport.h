// SPDX-License-Identifier: BSD-2-Clause-Patent
// Small, allocation-free bounds helpers shared by the kernel user-client path
// and its host fake-descriptor test. The real descriptor supplies readBytes /
// writeBytes; the helpers deliberately check the advertised length first.
#pragma once
#include <stdint.h>

namespace pianowifi {
namespace a80211 {

template <typename Descriptor>
inline bool readDescriptor(Descriptor *descriptor, uint64_t descriptorBytes, void *destination, uint32_t length)
{
  return descriptor && destination && length && uint64_t(length) <= descriptorBytes &&
         descriptor->readBytes(0, destination, length) == length;
}

template <typename Descriptor>
inline bool writeDescriptor(Descriptor *descriptor, uint64_t descriptorBytes, uint64_t offset, const void *source,
                            uint32_t length)
{
  return descriptor && source && offset <= descriptorBytes && uint64_t(length) <= descriptorBytes - offset &&
         descriptor->writeBytes(offset, source, length) == length;
}

}  // namespace a80211
}  // namespace pianowifi
