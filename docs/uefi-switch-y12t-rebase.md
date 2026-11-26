# UEFI OS switcher on `lead/v131y12t`

`main-repo/switch/sel1` at `48e56066` is its own merge base with `lead/v131y12t` at `3fcf74f4`. The seven switcher commits after `0663a6d2` entered the lead line in `d801f66e` on y12e. There were zero switcher-only commits to replay and no conflicts to resolve. The audit on `codex/switch-sel1-audit` used the T-16 switcher design and followed the existing slot-A rules in [Persistent boot](persistent-boot.md).

## Compatibility

- The selector still defaults to macOS on a normal boot with no request. With a foreign or unreadable flag, it boots macOS or honors a vol-up request for Android without writing the flag. A failed one-shot consume falls back to macOS. Recovery and off-mode charging retain ABL's stock handoff. ABL's power + vol-down path runs before the selector and is independent of the image. The selector and UniXNULib menu actions persist their requests through SDAM without partition writes.
- UniXNULib handles the switcher menu actions separately from the macOS entry's DarwinBoot request. DarwinBoot patches `0001`–`0003`, both host test files, and the UniXNU patch series match y12t byte for byte. DarwinBoot's 1024-byte command-line field and the k27 ADT tail keep their existing ABI. The recorded y12t qualification and y12q daily ESP both carry `piano-usb-net-txflow=1` in the merged tail and omit `piano-bootflag-write=1`. Both knobs use the post-merge PE parser. The kext write gate still defaults to `0`, and link loss and disable still flush queued RX. [Boot argument length](boot-args-length.md)
- The packer uses a persistent v4 UEFI wrapper. It keeps the stock Android ramdisk and copies BootShim, FD and DTB from the UEFI kernel field. The audit added refusals for incomplete v4 sections and a v4 UEFI input with a ramdisk that would otherwise be dropped. For an input with an AVB footer, section lengths are bounded by the original image size. Signing with `--props-from` uses the stock AVB properties, rollback index and fixed boot partition size. The audit exercised signing parameters in dry mode.

## Host results

| Check | Result |
| --- | --- |
| `tooling/switch/run-tests.sh` | PASS. 19,705 selector codec and decision checks, selector binary validation, packer regressions, shell codec, dry signing and FD menu patch checks. Real inputs passed with the stock Android image and both the macOS v5a wrapper and a persistent v4 wrapper built from the qualification record's exact y12t kernel field. The real y12t v2 RAM wrapper was refused before output. |
| Firmware and DarwinBoot | UniXNU `0001`–`0003`, Mu-Silicium and piano-uefi patches applied to their pinned sources in temporary copies. `test-darwinboot-extra-ram.sh` ran both DarwinBoot suites on the pin with `0001`–`0003` and passed, including EFI-map publication across an EBS retry. The y12t qualification RAM images passed `verify-ram-images.py` against their FD, DTB and build record. |
| PianoDwc3 | All C++ host tests and `test_piano_usbnet_tx.py` passed. TX flow delivered 64 MiB with zero drops and 1,477 stalls and restarts. The RX flush checks and all 1,558 boot-flag SDAM checks passed. The 27.3 native build passed import validation and found zero `PianoUsbNet` vtable problem slots. |
| Boot arguments | The k27 merge tests, ADT tests and boot-args-length boundary checks passed. The y12s qualification replay covered the 1,013-byte refusal and the long-layout fallback. The daily ESP replay covered the macOS, diagnostic and fastboot entries. |
| Persistence | The switch-pair test passed its slot-pair and refusal checks with the local fixtures. The sandbox denied loopback sockets during the positive backup and write simulation. All assertions passed on a rerun using file-backed transfers against the same fake tablet. |
| Branch diff from `3fcf74f4` | Privacy scan reported 0 hits and 0 refused files. `git diff --check` passed. |

## Owner gates

- **A0** remains a read-only cross-reboot survey. Confirm SDAM candidate `0x49` reads `0x00` in macOS and Android across the planned reboot, ABL fastboot, off-mode charge and cold-boot cases. Keep `piano-bootflag-write` at `0` for the survey and record the default-off refusal.
- **SW-B** requires owner authorization after the A0 survey and SW-A default and fastboot evidence, verified sets and a known ABL restore path. Verify the candidate's FD includes the switcher actions and the intended DarwinBoot patches before signing and RAM qualification. The later persistent step is a backed-up, read-back-verified `boot_a`-only write. Preserve `vendor_boot_a`, `init_boot_a`, Android data partitions, all `_b` partitions and the active slot.

All writes in this audit were to host files. The A0 and SW-B gates remain open.
