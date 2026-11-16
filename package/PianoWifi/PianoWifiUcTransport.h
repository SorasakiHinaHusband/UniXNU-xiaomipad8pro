// SPDX-License-Identifier: BSD-2-Clause-Patent
// Small, allocation-free bounds helpers shared by the kernel user-client path
// and its host fake-descriptor test. The real descriptor supplies readBytes /
// writeBytes; the helpers deliberately check the advertised length first.
#pragma once
#include <stdint.h>

namespace pianowifi {
namespace a80211 {

// 27.3 IO80211 builds the complete BSD ioctl word.  The get wrapper's
// immediate is 0xc02869c9 (Apple80211RawGet 0x1c4643e40-0x1c4643e48 and
// BindToInterfaceWithService 0x1c465fac8-0x1c465fad4); set is 0x802869c8.
// Comparing only the low 16 bits rejects the bind sanity request after it has
// already been decoded.
constexpr uint32_t kUcGetCmd = 0xc02869c9u;
constexpr uint32_t kUcSetCmd = 0x802869c8u;

inline bool ucCommandAccepted(uint32_t command)
{
  return command == kUcGetCmd || command == kUcSetCmd;
}

// Apple80211IOCTLSetWrapper copies a SET payload into the IOUserClient's
// kernel input structure immediately after the command/length words.  The
// selector's `data` word at offset 0x08 is therefore the first eight payload
// bytes on this path, not a second user pointer.  Keep the range check here so
// both the real descriptor path and the host fake-buffer test use the same
// fetch rule.
inline bool ucInlineSetPayload(const uint8_t *base, uint32_t bytes, uint32_t offset, uint32_t length, uint64_t &address)
{
  if (!base || offset > bytes || uint64_t(length) > uint64_t(bytes - offset)) return false;
  address = reinterpret_cast<uint64_t>(base + offset);
  return true;
}

// IOUserClient's OOL descriptors refer to pageable memory in the calling task.
// The caller must wire them before readBytes/writeBytes and complete them on
// every exit path.  This small guard is also used by the host fake descriptor,
// which rejects an access made outside the prepared interval.
template <typename Descriptor>
class DescriptorPreparation {
  Descriptor *descriptor_ = nullptr;
  uint32_t direction_ = 0;
  bool prepared_ = false;

 public:
  DescriptorPreparation(Descriptor *descriptor, uint32_t direction) : descriptor_(descriptor), direction_(direction)
  {
    if (descriptor_) prepared_ = descriptor_->prepare(direction_) == 0;
  }
  DescriptorPreparation(const DescriptorPreparation &) = delete;
  DescriptorPreparation &operator=(const DescriptorPreparation &) = delete;
  ~DescriptorPreparation()
  {
    if (prepared_) (void)descriptor_->complete(direction_);
  }
  bool ok() const { return !descriptor_ || prepared_; }
};

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
