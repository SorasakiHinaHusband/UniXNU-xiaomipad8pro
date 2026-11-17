# V6 owned AVC test data

These two clips are Leo Zhang's encodes of FFmpeg's generated `testsrc2` pattern,
not downloaded media. The manifest records the complete encoding arguments,
compressed packet SHA256/PTS/DTS, and every active NV12 frame's independent
software-decoder SHA256. Both clips are 60 frames, 320×240 at 30 fps. The IP clip
uses Baseline and the IBP clip Main; neither contains audio or personal metadata.

The manifest's paths are relative for reproducibility. The software truth is
fixed before hardware testing and must never be replaced with tablet output.
`video-v6 --self-test FIXTURE_DIR` verifies the clip, demux, parameter and serializer
contracts without opening a hardware decoder. `--qualify` is tablet-only.
