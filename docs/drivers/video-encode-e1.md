# PianoVideo E1 integration

E1 is host-tested and built for arm64e on `main` base `3d1548df2`. Tablet qualification remains pending.

- E1 starts only with one literal `piano-video-enc=1` token, level 6 and a successful boot reservation. It waits for the decoder's exclusive lease and reuses the arena with its current backing. On firmware system error, watchdog or SMMU fault, the worker stops issuing HFI commands and quarantines the core with its DMA backing held for the rest of the boot.
- AVC and HEVC continue to use the existing decode path, VT inventory and borrow policy. The RAM16G reservation and XNU fallback still serve decode. See the [driver guide](video.md#18-e1-bounded-native-h264-encode-self-test).

## Host results

- `run-bringup-host.sh` runs E1 scripted HFI alongside the existing AVC, HEVC and VT suites under ASan/UBSan. The VT cancellation regression covers the deferred `dinv` reply. The E1 arena regression switches AVC → E1 → Main10 → AVC over both XNU and region0 backing and restores the original AVC page tables. The FFmpeg check grades a synthetic reference stream and rejects truncated and wrong-size samples.
- The native build completed the firmware loader, shared extra-RAM and PianoVideo host tests and compiled all 12 arm64e objects
  with zero warnings. The import check against `kernel-k273-0070-s0001-native.macho` reported 431 kernel imports, six declared
  library imports and zero unresolved imports.

## RAM batch E qualification

Use the same 27.3 RAM payload, pinned Iris firmware addon, fixed display profile and volatile Data across the four boots below. Use the RAM16G qualification FD with DarwinBoot patch 0002 and stage the ADT placeholder with `PIANO_EXTRA_RAM=1`. Keep `piano-video=6 piano-video-vt=1 piano-extra-ram=1` in every boot and keep the normal VT grant and borrow defaults. Save the commit, FD/KC identities, boot UUID, `kern.boottime`, merged `kern.piano_bootargs` and a fresh serial capture per boot.

| Order | `piano-video-extra-ram` | `piano-video-enc` | Required observation |
| --- | --- | --- | --- |
| 1 | `0` | Omitted | XNU-backed 256 MiB V9h reservation, zero video slabs allocated. |
| 2 | `0` | `0` | Same reservation, decode results and E1 silence as boot 1. |
| 3 | `1` | `0` | Region0-backed V9h reservation with the same decode results. |
| 4 | `1` | `1` | E1 only, after all decode and RAM16G evidence is saved. |

1. On boots 1–3, save `sudo sysctl machdep.piano_extra_ram_video`, the `/chosen/piano-extra-ram` property and the V9h owner/ready lines. For boot 3 require `state=ours`, `self-test=pass pages=70144 bad=0`, `used=128 free=9` and the console's 128 write-combined slabs starting at `0x89e00000`. On each boot, check serial and the PianoVideo IORegistry after the 120-second default E1 delay for zero E1 lines and absent `piano-video-enc-result` and `piano-video-enc-stream`.
2. On each control boot, run `video-v6 --qualify package/PianoVideo/tests/fixtures/v6` and save the per-frame NV12/PTS results and native session receipts. In stock QuickTime play the owned `fixtures/v8/high1080.mp4` AVC clip, then `fixtures/v9/main8-1080.mp4` and `fixtures/v9/main10-1080.mp4`. Capture the opening transition and playback, including V9j input-ready before the start reply, V9j3 first real input, matching native/client frame counts, NV12 or P010 as appropriate, and a healthy STOP/CLOSE for each. Require zero flicker, transparent frames, watchdog, SMMU faults or quarantine. Verify Safari still selects native AVC, WebKit's HEVC goes to the existing fallback, and VT `encs` still lists zero encoders. Stop the batch on a failed control and save its evidence.
3. **Run E1 last.** On boot 4 set `piano-video-enc-delay=120 piano-video-enc-res=0 piano-video-enc-frames=10 piano-video-enc-kbps=2000` with the table's flags. Confirm the extra-RAM reservation and leave media clients idle so E1 takes the lease. Save serial from the arm line through OPEN, ten input returns and ten nonempty bitstream completions, DRAIN ACK/LAST, both STOP acknowledgments and CLOSE. Require `E1 encode end PASS` with status 0, `quarantined 0`, zero watchdog or SMMU faults and `truncated 0` in the result log. Capture both properties using `ioreg -r -c PianoVideo -l -w0 > enc-ioreg.txt`.
4. On the host, run `python3 package/PianoVideo/tests/enc_stream_check.py --ioreg enc-ioreg.txt --size 640x480 --frames 10`. Require the IORegistry PASS, a complete Annex B stream within 256 KiB with SPS, PPS and IDR, ten software-decoded frames and minimum luma PSNR of 30 dB. If E1 stalls, fails, or never starts, save serial and any available IORegistry data and end the boot. E1 can lock the video core, so finish the batch without starting another video session on that boot.
