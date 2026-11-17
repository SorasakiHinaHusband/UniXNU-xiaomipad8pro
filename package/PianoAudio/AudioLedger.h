/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Speaker-only diagnostics. One owner writes; readers copy atomic words and
 * validate the slot's publication sequence. No allocation or logging on refill. */
#ifndef PIANO_AUDIO_LEDGER_H
#define PIANO_AUDIO_LEDGER_H
#include <stdint.h>
#define AUDIO_LEDGER_CAP 128u
#define AUDIO_LEDGER_WORDS 32u
enum {AlRefill=1, AlStart=2, AlStop=3, AlPark=4, AlRearm=5};
typedef struct {uint64_t seq, word[AUDIO_LEDGER_WORDS];} AudioLedgerSlot;
typedef struct {uint64_t written; AudioLedgerSlot slot[AUDIO_LEDGER_CAP];} AudioLedger;
static inline void audio_ledger_put(AudioLedger *l, const uint64_t *w)
{
    const uint64_t n = __atomic_load_n(&l->written, __ATOMIC_RELAXED);
    AudioLedgerSlot *s = &l->slot[n % AUDIO_LEDGER_CAP];
    __atomic_store_n(&s->seq, 0, __ATOMIC_SEQ_CST);
    for (unsigned i=0; i<AUDIO_LEDGER_WORDS; ++i) __atomic_store_n(&s->word[i], w[i], __ATOMIC_SEQ_CST);
    __atomic_store_n(&s->seq, n+1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&l->written, n+1, __ATOMIC_RELEASE);
}
static inline int audio_ledger_get(const AudioLedger *l, uint64_t n, uint64_t *w)
{
    const AudioLedgerSlot *s = &l->slot[n % AUDIO_LEDGER_CAP];
    if (__atomic_load_n(&s->seq, __ATOMIC_SEQ_CST) != n+1) return 0;
    for (unsigned i=0; i<AUDIO_LEDGER_WORDS; ++i) w[i]=__atomic_load_n(&s->word[i], __ATOMIC_SEQ_CST);
    return __atomic_load_n(&s->seq, __ATOMIC_SEQ_CST)==n+1;
}
#endif
