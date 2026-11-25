# Support status (2026-11-25)

One row per feature. Every status comes from a run on the tablet; host tests, analyses and "built, not run" work do not count.

- **Works (installed)**: verified on the system installed on the tablet's disk, booted from disk.
- **RAM only**: verified on a `fastboot boot` RAM image, not yet on the installed system.
- **Partial**: works, with the gaps named in the note.
- **Not yet**: not working, or never run on the tablet.

The installed daily build is **v13.1y12k** (installed 2026-11-25; 61 installed gates PASS, the 3 FAILs are known and recorded
only: two Core Image paths fixed by the 4 GiB GPU windows below, and the Energy Mode picker check). "Verified" names the build
and date of the newest run that showed the result. Daily results: [results-2026-11-25.md](results-2026-11-25.md) and the earlier
`results-*.md` pages.

| Feature | Status | Verified (build, date) | Note |
| --- | --- | --- | --- |
| Boot from disk, persistent install | Works (installed) | v13.1y12k, 2026-11-25 | Disk boot, exact ESP profile, the owner's Data store resumed, boot guard healthy, no panic. Installs write only changed parts, after backups, with read-back checks and a layer-first rollback to the previous build. |
| Display | Works (installed), 60 Hz | v13.1y12k, 2026-11-25 | Frames reach the panel through PianoDpu flips on the boot framebuffer's pipes; the CPU still does per-frame work (a cache clean, split over 4 threads). No underrun, revert or SMMU fault over the session. A fully native DPU path (fences, refresh switching, Night Shift, panel power) is in analysis. |
| 120 Hz | RAM only | RAM gpu-hz120-2, 2026-11-21 | 120 Hz is exposed and kept across display off/on; 73-78 new frames/s are presented. The daily profile stays at 60 Hz. |
| Touch screen (native) | Works (installed) | v13.1y12k, 2026-11-25; gestures on RAM touch-native3, 2026-11-23 | A native built-in Touch Screen bound to the display: tap, long press (right click), pan and scroll in scroll views, pinch, rotate, smart magnify, palm rejection, 90 degree axes (44/0 on RAM). Installed: published from boot and acquired by the system. Long use by hand on the native path is pending. |
| Keyboard cover and trackpad | Works (installed) | owner hands-on, v12, 2026-11-19; v13.1y12k, 2026-11-25 | Typing and the trackpad work; the cover is known to the login window. |
| Wi-Fi | Works (installed), partial | v13.1y12k, 2026-11-25; v13.1x, 2026-11-21 | Joins at boot (WPA2, DHCP, DNS); radio off/on from the switch rejoins; a forced link loss rejoins within seconds; country code set. Pending: joining by clicking a network row by hand, auto-rejoin after a hotspot goes away and comes back, and a multi-access-point office network (a fix is in the build). |
| Bluetooth | Not yet | owner session on an installed v13.1 build, 2026-11-20 | Discovery works; a Bluetooth LE connection times out (HCI 0x08) before the link is ready, so keyboards and mice do not pair. Not in the daily profile. Leading candidate (inferred): Wi-Fi holds the shared antenna; a power-save test is prepared. Bluetooth audio not started. |
| Speakers | Works (installed) | v13.1u, 2026-11-21 (audible tone) | Playback, volume and mute; speaker protection at gain 243 and the stock Dolby "Movie" processing run on the DSP. Since 2026-11-23, installs are silent by request, so later installs check the output device without a tone. |
| Microphone | Partial | v13.1y12k, 2026-11-25; noise reduction on RAM mic-ns4b, 2026-11-23 | The raw "Mic" device (the default input) and a "Mic (noise reduced)" device (RNNoise) are in the audio system on the installed boot. The recording-during-playback stutter was fixed on the installed system (2026-11-21). An app recording (QuickTime) played back by hand is pending. |
| Cameras: Photo Booth | Works (installed) | v13.1y12k, 2026-11-25 | Live front preview with no prompt; the effects grid opens. |
| Cameras: QuickTime | Partial | v13.1y12k, 2026-11-25 | 10 s H.264 movies from the front and rear cameras record (AVFoundation, the QuickTime High preset). A New Movie Recording saved and played back by hand is pending. Encoding is in software. |
| Cameras: FaceTime | Not yet verified | permissions pre-granted, v13.1y12k, 2026-11-25 | Both cameras are listed and FaceTime has Camera and Microphone access; a FaceTime self-view has not been checked. |
| Charging: 5 V | Works (installed) | v13.1y12d, 2026-11-24 | Up to 3 A from a 3 A source (input limit 3000 mA; on a computer port the input settles near 1.8 A when the bus sags to about 4.72 V); net battery charge current positive. |
| Charging: PD 9 V (buck) | Works (installed) | v13.1y12d, 2026-11-24 | On by default. Fixed 9 V PD on the stock charger: 9 V / 1.6 A (about 14 W, the current the charger firmware requests, as on Ubuntu). PPS 9 V (up to 2.5 A) is built but not run. |
| Charging: 80 % hold | Works | RAM chg-buck6, 2026-11-17; in every installed profile since | Stops at 80 %, resumes at 75 %. |
| Charging: charge pump / MiPPS | Not yet | first attended try, 2026-11-24 | Off by default. The first attempt stayed at a 100 mA input limit (an unmapped vendor power object); fixed in the build, the next attended test is pending. |
| Battery percentage | Works (installed) | v13.1y12k, 2026-11-25 | Percentage, charging state and the gauge read correctly. Cycle count, health and time-to-full fields are not filled yet. |
| Sleep and wake | Partial | RAM v13.1y12d qualification, 2026-11-24 | Suspend-to-idle with a timer wake works; Wi-Fi and the USB link recover; the lock screen appears. Lid close and the power button on the installed system are not verified. |
| Brightness: manual | Partial | keys on installed v12, 2026-11-19; slider path on v13.1i | Brightness keys work. The slider path through the system's brightness control works; a hand drag in Control Center is pending. |
| Brightness: automatic | Works (installed) | v13.1y12k, 2026-11-25 | The light sensor drives automatic brightness; manual moves do not end automatic mode. A real light-change span is pending. |
| GPU: Metal (Adreno 830) | Works (installed) | v13.1y12k, 2026-11-25 | Our Metal plug-in runs WindowServer, the desktop and apps; ImageIO, Core Image on Metal, MetalKit texture loading and Quick Look thumbnails pass (28 of 30 paths; the two failing ones are fixed below). |
| GPU: paged memory | Works (installed) | v13.1y12k, 2026-11-25 | 4 KiB paged GPU memory (cap 7168 MiB, no fixed pool), no refusals, no page faults; hang recovery by replay on by default. |
| GPU: Android-level frequency curve | Works (installed) | v13.1y12k, 2026-11-25 | The Android 14-level table up to 1100 MHz with its skin and thermal steps and a KGSL-style governor; the Android CPU table. Sustained 3D bench about 50-54 fps at 734 MHz (RAM, 2026-11-23). |
| GPU: 4 GiB windows | RAM only (next daily) | RAM slots2-qual1, 2026-11-25 | 64 client windows of 4 GiB in a 256 GiB aperture, 1 GiB per object. Fixes the two Core Image 4032-pixel out-of-memory paths: 30/30 graded paths pass. |
| Safari: WebGL 2 and WebGPU | Works (installed) | v13.1y12h, 2026-11-25; again v13.1y12k | WebGL 2 on, WebGPU adapter "adreno"; Safari's GPU process opens the accelerator. |
| Safari: hardware video decode | Works (installed) | v13.1y12i, 2026-11-25 | H.264 up to 1920x1088 decodes on the Iris video core: 1080p 600 frames, 0 dropped. |
| Safari: HEVC and 4K | Works (installed), software | v13.1y12j, 2026-11-25 | HEVC 2160p 600/0 at 29.9 fps and H.264 2160p 591/5, in Apple's software decoders (by design). VP9 and AV1 are not supported. |
| Chrome | Partial | v13.1y12k, 2026-11-25 | Runs on the installed system on software rendering. The GPU plug-in now loads in Chrome's sandboxed GPU process; the last gate (a disk cache directory the sandbox forbids) has a fix built for the next daily. WebGPU also needs a larger user address space (in progress). Chrome is not used on RAM boots (it can exhaust the RAM-backed Data disk). |
| Video playback (QuickTime) | Works (installed) | v13.1x, 2026-11-21 | AVC High and HEVC Main10 decoded in hardware; about 100 s each with 0 transient or dark frames on the webcam. |
| Energy modes | Partial | v13.1y12k, 2026-11-25 | Low Power, Automatic and High Power (the daily setting) set with `pmset` reach the power and thermal policy. The picker in System Settings is not verified (its check fails, recorded only). |
| Login window idle CPU | Works (installed) | v13.1y12j, 2026-11-25 | Fixed: an audio daemon looped at the login window (about 1.3 cores); it is now held until the login. |
| App launches | Works (installed) | v13.1y12k, 2026-11-25 | Calculator, TextEdit and Notes open their first window in under 2.5 s. |
| USB debug (console, root shell, ssh) | Works (installed) | v13.1y12k, 2026-11-25 | USB-C debug console with a monitor that returns to the bootloader, root shell and ssh over the cable; no endpoint wedge over the boot. |
| OS switcher (Android / macOS) | RAM only, read-only | RAM v13.1y12e, 2026-11-24 | The boot-flag sysctls read the flag; writes are refused by design. A full switch has not been run. |
| USB host / OTG | Not yet | RAM usb-a2d, 2026-11-18 (empty port) | Host mode starts on an empty port; VBUS supply and a real device are not done. |
| External display (USB-C DP) | Not yet | none | Design only. |
| Auto-rotate and other sensors | Not yet | RAM, 2026-11-19 (accelerometer sample) | The light sensor works; auto-rotate is off in the daily profile. |
| Hardware video encode | Not yet | camera recordings, 2026-11-25 | Recordings use the software encoder. |
| IR blaster, stylus, torch | Not yet | none | Drivers built or designed, not run. |
| Using all 16 GB of RAM | Not yet | installed window 8 GiB | The installed system sees an 8 GiB window (RAM images 13 GiB); the second DRAM range is in analysis. |
| Fingerprint, NPU | Deferred | none | No Secure Enclave (no Touch ID); no Core ML backend on this SoC. |

## Known gaps

- Bluetooth pairing (LE connect timeout) and Bluetooth audio.
- Lid and power-button sleep on the installed system; the Energy Mode picker; the brightness slider by hand.
- Chrome GPU acceleration and Chrome WebGPU; Chrome on RAM boots.
- Charge pump / MiPPS fast charging; PPS 9 V.
- 120 Hz in daily use; the native DPU path; all 16 GB of RAM.
- USB host devices, external display, auto-rotate, hardware video encode, VP9/AV1 decode.
- Hand checks pending on the installed system: microphone recording in an app, QuickTime recording by hand, FaceTime self-view,
  joining a Wi-Fi network from the menu, Wi-Fi auto-rejoin after the network returns.
