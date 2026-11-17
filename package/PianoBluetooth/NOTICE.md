# Source attribution

`QbtTty.c` and `qbt_transport.h` are copied from UniXNU's QcaBluetooth,
commit `effe4ec5c57cd400a5f818b21c9903fef237edbc`, files
`src/qbt_tty.c` and `include/qbt_transport.h`:
<https://github.com/UniXNU/QcaBluetooth>.
They retain their BSD-2-Clause-Patent SPDX notices. Piano's changes adapt
power coordination and observability to its existing controller driver.

The address publication and domain-separated stable local address follow
UniXNU-sheng, commit `0eea3a2`, `package/BluetoothProbe.cpp`. The controller,
firmware reader and hardware programming remain Piano's existing code.
