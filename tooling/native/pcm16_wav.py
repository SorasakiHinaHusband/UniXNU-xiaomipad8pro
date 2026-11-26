# SPDX-License-Identifier: MIT
"""Read bounded RIFF/WAVE and 16-bit PCM on Python 3.9 and later."""
import array
import struct
import sys

PCM_SUBFORMAT_GUID = bytes.fromhex("01000000 0000 1000 8000 00aa00389b71")
FLOAT_SUBFORMAT_GUID = bytes.fromhex("03000000 0000 1000 8000 00aa00389b71")


def riff_chunks(wav):
    header = wav.read(12)
    if len(header) != 12 or header[:4] != b"RIFF" or header[8:] != b"WAVE":
        raise ValueError("expected a RIFF/WAVE file")
    riff_end = 8 + struct.unpack_from("<I", header, 4)[0]
    if not 12 <= riff_end <= wav.seek(0, 2):
        raise ValueError("invalid or truncated RIFF size")
    wav.seek(12)
    while wav.tell() < riff_end:
        header = wav.read(min(8, riff_end - wav.tell()))
        if len(header) != 8:
            raise ValueError("truncated WAV chunk header")
        chunk, size = struct.unpack("<4sI", header)
        chunk_end = wav.tell() + size + (size & 1)
        if chunk_end > riff_end:
            raise ValueError("WAV chunk extends past the RIFF boundary")
        yield chunk, size
        wav.seek(chunk_end)


def read_header(wav, pcm16_only):
    fmt = None
    for chunk, size in riff_chunks(wav):
        if chunk == b"fmt ":
            if fmt is not None:
                raise ValueError("duplicate WAV format chunk")
            data = wav.read(min(size, 40))
            if len(data) < 16:
                raise ValueError("truncated WAV format chunk")
            tag, channels, rate, byte_rate, align, bits = struct.unpack_from("<HHIIHH", data)
            if tag == 0xfffe:
                # afconvert can write WAVE_FORMAT_EXTENSIBLE, which wave in Python 3.9 rejects.
                if len(data) < 40:
                    raise ValueError("truncated extensible WAV format")
                extra_size, valid_bits = struct.unpack_from("<HH", data, 16)
                if not 22 <= extra_size <= size - 18 or valid_bits > bits:
                    raise ValueError("invalid extensible WAV format")
                tag = {PCM_SUBFORMAT_GUID: 1, FLOAT_SUBFORMAT_GUID: 3}.get(data[24:40], tag)
            if pcm16_only and (tag != 1 or bits != 16):
                raise ValueError("expected 16-bit PCM WAV")
            if not channels or not rate or not align:
                raise ValueError("invalid WAV format")
            if tag in (1, 3) and (not bits or align != channels * ((bits + 7) // 8) or byte_rate != rate * align):
                raise ValueError("invalid PCM WAV format")
            fmt = rate, channels, align, tag
        elif chunk == b"data":
            if fmt is None:
                raise ValueError("missing WAV format before data")
            rate, channels, align, tag = fmt
            if tag in (1, 3) and size % align:
                raise ValueError("partial PCM frame in WAV data")
            if not size:
                raise ValueError("recording contains no frames")
            return rate, channels, size // align
    raise ValueError("missing WAV data chunk")


def read_pcm16_header(wav):
    """Leave the stream positioned at the start of the PCM frames."""
    return read_header(wav, pcm16_only=True)


def validate_wav(wav):
    """Validate the container before handing it to a format converter."""
    read_header(wav, pcm16_only=False)


def read_samples(wav, frames, channels):
    size = frames * channels * 2
    data = wav.read(size)
    if len(data) != size:
        raise ValueError("truncated PCM data")
    samples = array.array("h", data)
    if sys.byteorder == "big":
        samples.byteswap()
    return samples
