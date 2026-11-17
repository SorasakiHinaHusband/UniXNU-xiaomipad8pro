/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PIANO_AUDIO_LEVEL_DICTIONARY_H
#define PIANO_AUDIO_LEVEL_DICTIONARY_H
#include <stdint.h>

/* IOAudio2's range map is an array of ranges. Raw coordinates are 32-bit
 * integers; dB coordinates are signed 32.32 numbers, decoded by GetFixed64.
 * Keep publication shared with the host's CoreFoundation dictionary decoder. */
template <class Dictionary, class Array, class SetNumber>
static bool piano_audio_level_dictionary(Dictionary *d, uint32_t value, SetNumber number)
{
    Dictionary *range = Dictionary::withCapacity(4);
    Array *map = Array::withCapacity(1);
    if (!range || !map) {
        if (range) range->release();
        if (map) map->release();
        return false;
    }
    number(range, "start int value", 0, 32);
    number(range, "start db value", uint64_t(-36LL * (1LL << 32)), 64);
    number(range, "integer steps", 100, 32);
    /* Round down to 24 fractional bits: the HAL's subsequent Float32
     * interpolation must also stay below 0 dB (within 0.000005 dB). */
    number(range, "db per step", ((36ULL << 24) / 100) << 8, 64);
    map->setObject(range);
    d->setObject("range map", map);
    range->release();
    map->release();
    number(d, "minimum value", 0, 32);
    number(d, "maximum value", 100, 32);
    number(d, "transfer function", 0, 32); // linear scalar/raw normalization
    number(d, "left value", value, 32);
    number(d, "center value", value, 32);
    number(d, "right value", value, 32);
    number(d, "left channel", 1, 32);
    number(d, "right channel", 2, 32);
    return true;
}
#endif
