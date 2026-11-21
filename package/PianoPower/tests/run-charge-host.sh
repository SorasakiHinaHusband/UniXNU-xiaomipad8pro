#!/bin/bash
# Portable policy, wire/contract, gauge bus and battery regression tests.
set -euo pipefail
D=$(cd "$(dirname "$0")/../../.." && pwd)
P=$D/package/PianoPower
T=$D/out/chg-host
mkdir -p "$T"
for f in PianoMca ChargePolicy QcomQrtr; do
  "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -c "$P/$f.c" -o "$T/$f.o"
done
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/contract_host.cpp" -o "$T/contract_host"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" -I "$D/package/include" "$P/tests/contract_link_host.cpp" \
  "$T/ChargePolicy.o" "$T/PianoMca.o" -o "$T/contract_link_host"
"$T/contract_link_host"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" -I "$D/package/include" "$P/tests/pdo_firmware_host.cpp" \
  "$T/ChargePolicy.o" "$T/PianoMca.o" -o "$T/pdo_firmware_host"
"$T/pdo_firmware_host"
"$T/contract_host"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/contract_init_host.cpp" -o "$T/contract_init_host"
"$T/contract_init_host"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/policy_host.cpp" "$T/ChargePolicy.o" -o "$T/policy_host"
"$T/policy_host"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" -I "$D/package/include" "$P/tests/power_host.cpp" \
  "$T/PianoMca.o" "$T/ChargePolicy.o" "$T/QcomQrtr.o" -o "$T/power_host"
"$T/power_host"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/battery_host.cpp" -o "$T/battery_host"
"$T/battery_host"
