// SPDX-License-Identifier: BSD-2-Clause-Patent
// Small, allocation-free bounds helpers shared by the kernel user-client path
// and its host fake-descriptor test. The real descriptor supplies readBytes /
// writeBytes; the helpers deliberately check the advertised length first.
#pragma once
#include <stdint.h>

namespace pianowifi {
namespace a80211 {

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
