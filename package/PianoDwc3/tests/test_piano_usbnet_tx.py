#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the RX thread's calls into the real IOBasicOutputQueue boundary."""
import re
from pathlib import Path

source = (Path(__file__).resolve().parents[1] / 'PianoUsbNet.cpp').read_text()

# A synchronous restart can drain indefinitely on the RX thread. Reopening
# after a flush must also start at zero capacity so start() has no work to drain.
assert re.search(r'oqService\(net->fQueue,\s*IOBasicOutputQueue::kServiceAsync\);', source)
start = source.split('void PianoUsbNet::startTxQueue()', 1)[1].split('}', 1)[0]
assert re.search(r'oqStart\(fQueue\);\s*oqSetCapacity\(fQueue, kOutputPackets\);', start)
print('PianoUsbNet TX: asynchronous service and start before admitting packets')
