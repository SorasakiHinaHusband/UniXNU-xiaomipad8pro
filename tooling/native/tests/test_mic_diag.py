#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang.
"""Host-only positive/negative acoustic acceptance, using the supplied fixture.

Usage: python3 test_mic_diag.py PATH_TO_SUPPLIED_MEASURE_PY OUTPUT_DIRECTORY
No microphone, speaker, GUI, privilege or tablet operation is performed.
"""
import array
import contextlib
import importlib.util
import io
import json
import math
from pathlib import Path
import sys
import wave


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def main():
    m = module('fixture_measure', sys.argv[1])
    checker = module('diag_checker', Path(__file__).parents[1] / 'check-mic-diag.py')
    root = Path(sys.argv[2])
    root.mkdir(parents=True, exist_ok=True)
    fixture = root / 'coded.wav'
    m.fixture(fixture)
    stimulus, rate, _ = m.read(fixture)
    onset, offsets, duration, host = 12., (0., 10., 20., 30.), 49., 1000.
    rows = [dict(event='silence-start', at=host-9), dict(event='silence-end', at=host-3)]
    rows += [dict(event='play', phase=p, before=host+o, after=host+o+3.04, rc=0)
             for p, o in zip(('unity', 'lower', 'mute', 'recover'), offsets)]
    rows += [dict(event='record-stop-before', at=host+duration-onset),
             dict(event='record-stop-after', at=host+duration-onset+.2)]
    timeline = root / 'timeline.jsonl'
    timeline.write_text(''.join(json.dumps(r)+'\n' for r in rows))
    good = array.array('h', (round(4*math.sin(2*math.pi*733*i/rate))
                            for i in range(round(duration*rate))))
    # Software mute covers the full five-second window, with room on both sides.
    mute_start, mute_end = round((onset+20-2)*rate), round((onset+20+6)*rate)
    good[mute_start:mute_end] = array.array('h', [0]) * (mute_end-mute_start)
    for o, gain in zip(offsets, (1, 4125/32767, 0, 1)):
        start = round((onset+o)*rate)
        for i, value in enumerate(stimulus):
            good[start+i] += round(value*.4*gain)
    for kind in ('good', 'mute-tail-not-zero', 'source-off-tone', 'missing-five-second-tail'):
        values = good[:]
        if kind == 'mute-tail-not-zero':
            # Outside the coded burst: old tone-only checks must not suffice.
            values[round((onset+20+3.7)*rate)] = 1
        elif kind == 'source-off-tone':
            for i in range(round(3.2*rate), round(8.8*rate)):
                values[i] += round(2000*math.sin(2*math.pi*997*i/rate))
        elif kind == 'missing-five-second-tail':
            values = values[:round((onset+30+3.6)*rate)]
        path = root / (kind + '.wav')
        with wave.open(str(path), 'wb') as f:
            f.setparams((1, 2, rate, 0, 'NONE', 'not compressed'))
            f.writeframes(values.tobytes())
        # Keep the alignment envelope result from the valid full recording for
        # the truncated-window negative: the missing tail is the tested gate.
        if kind != 'missing-five-second-tail':
            output = io.StringIO()
            args = type('Args', (), dict(wav=path, timeline=timeline))()
            with contextlib.redirect_stdout(output):
                result = m.analyze(args)
            report = root / (kind + '.json')
            report.write_text(output.getvalue())
            assert not result, (kind, 'legacy fixture acceptance must pass to exercise the new gate')
        else:
            report = root / 'good.json'
        with (root / (kind + '-checks.txt')).open('w') as log, contextlib.redirect_stdout(log):
            result = checker.windows(sys.argv[1], path, timeline, report)
        assert bool(result) == (kind != 'good'), kind
        print('PASS acoustic fixture ' + kind)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
