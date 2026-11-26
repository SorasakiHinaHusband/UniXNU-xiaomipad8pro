#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Grade one installed-system capture offline."""
import csv
import math
import re
import sys
from pathlib import Path

SECONDS = 600
WINDOW = 10
FPS_MIN = 30.0
P99_MAX_MS = 40.0
RECOVERY_KEYS = ('recoveries', 'failures', 'reports', 'watchdog', 'jobs_failed', 'lost_cause')
FAULT = re.compile(
    r'GPU (?:LOST|RECOVER\b|restarts?\b|restarted\b|resets?\b|resetting\b|hang\b|hung\b|context is lost)'
    r'|DEVICE LOST|hang snapshot|recovery done|recovery: (?:culprit|the driver)'
    r'|MTLCommandBuffer(?:StatusError|ErrorDomain)|R3a UNSAFE|R3b3 exit|panic\(cpu', re.I)
FREQUENCY = re.compile(r'^at_us=(\d+) lvl=(\d+) mhz=(\d+)\b')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def fields(line):
    return dict(re.findall(r'(\w+)=(\S+)', line))


def snapshot(path):
    lines = path.read_text().splitlines()
    boot = next(line.removeprefix('BOOT=') for line in lines if line.startswith('BOOT='))
    state = next(FREQUENCY.match(line) for line in lines if FREQUENCY.match(line))
    counters = next(line.split() for line in lines if line.startswith('replay '))
    require(re.fullmatch(r'[\da-fA-F-]{36}', boot), 'missing boot UUID')
    recovery = {key: int(value) for key, value in zip(counters[::2], counters[1::2]) if key in RECOVERY_KEYS}
    require(set(recovery) == set(RECOVERY_KEYS), 'incomplete GPU recovery counters')
    return boot, tuple(map(int, state.groups())), recovery


def percentile(samples, p):
    return samples[math.ceil(len(samples) * p) - 1]


def number(cell):
    value = float(cell)
    require(math.isfinite(value) and value > 0, 'invalid frame timestamp')
    return value


def grade(root):
    before_boot, before_freq, before_recovery = snapshot(root / 'before.txt')
    after_boot, after_freq, after_recovery = snapshot(root / 'after.txt')
    require(before_boot == after_boot, 'boot changed during the test')
    require(before_recovery == after_recovery and before_recovery['lost_cause'] == 0,
            f'GPU recovery/fault/watchdog changed or GPU lost: {before_recovery} -> {after_recovery}')
    require((root / 'run.txt').read_text().splitlines().count('METAL_SUSTAIN_REMOTE_RC=0') == 1,
            'console-user command did not confirm exit 0')

    console = (root / 'console.txt').read_text(errors='replace')
    markers = re.findall(r'PIANO_METAL_SUSTAIN ([A-Za-z0-9]+) (BEGIN|END)\b', console)
    require(len(markers) == 2 and markers[0][0] == markers[1][0] and
            [phase for _, phase in markers] == ['BEGIN', 'END'], 'incomplete console window')
    require(re.search(r'R3b LV .*\blvl=\d+ .*\bmhz=\d+', console), 'no in-window GPU console heartbeat')
    fault = FAULT.search(console)
    require(fault is None, f'GPU hang/reset/error in console window: {fault.group() if fault else ""}')

    log = (root / 'summary.log').read_text().splitlines()
    begins = [line for line in log if line.startswith('METAL_SUSTAIN_BEGIN ')]
    ends = [fields(line) for line in log if line.startswith('METAL_SUSTAIN_END ')]
    require(len(begins) == 1 and 'device="Adreno' in begins[0], 'Adreno hardware start is missing')
    begin = fields(begins[0])
    require(all(begin.get(k) == v for k, v in dict(mode='onscreen', seconds='600', width='1920',
                                                   height='1280', draws='257').items()), 'wrong workload settings')
    require(len(ends) == 1 and all(ends[0].get(k) == v for k, v in
            dict(rc='0', elapsed_s='600.000000', windows='60', unfinished='0').items()), 'incomplete 600-second run')
    require(not any(line.startswith(('METAL_SUSTAIN_ERROR ', 'METAL_SUSTAIN_COMMAND_ERROR ')) for line in log),
            'Metal or command-buffer error')
    summaries = [fields(line) for line in log if line.startswith('WINDOW ')]
    require(len(summaries) == SECONDS // WINDOW, 'missing 10-second windows')

    frequency = [fields(line) for line in log if line.startswith('GPU_STATE ')]
    frequency_at = before_freq[0]
    low_seen = before_freq[1] <= 3
    stepped = False
    readings = [[] for _ in summaries]
    for state in frequency:
        i, at, level, mhz = (int(state[k]) for k in ('window', 'at_us', 'lvl', 'mhz'))
        elapsed = float(state['elapsed_s'])
        require(0 <= i < len(readings) and i * WINDOW <= elapsed <= (i + 1) * WINDOW and
                frequency_at <= at <= after_freq[0] and (level == 0) == (mhz == 0), 'invalid frequency sample')
        require(not readings[i] or at > readings[i][-1][0], 'repeated or out-of-order frequency sample')
        frequency_at = at
        low_seen |= level <= 3
        stepped |= low_seen and level > 3
        readings[i].append((at, level, mhz))
    require(stepped, 'no observed frequency step from level 3 or below to above level 3')

    with (root / 'frames.csv').open(newline='') as stream:
        rows = csv.DictReader(stream)
        cohorts = [[] for _ in summaries]
        sequence, last_begin = None, 0.0
        start = number(begin['start'])
        for row in rows:
            i, seq = int(row['window']), int(row['frame'])
            cpu, encode, submit, completion = (number(row[k]) for k in
                ('cpu_begin_s', 'cpu_encode_begin_s', 'cpu_submit_s', 'completion_callback_s'))
            require(0 <= i < len(cohorts) and abs(cpu - start - i * WINDOW - WINDOW / 2) <= WINDOW / 2 + 1e-6,
                    'frame outside its window')
            require(row['status'] == 'completed' and last_begin <= cpu <= encode <= submit <= completion and
                    (sequence is None or seq == sequence + 1), 'truncated or invalid frame sequence')
            sequence, last_begin = seq, cpu
            cohorts[i].append(row)

    last_completion = 0.0
    worst_p99, lowest_fps, stalls, total, presented = 0.0, math.inf, 0, 0, 0
    stall_windows, slow_windows, low_fps_windows = [], [], []
    for i, (summary, frames, samples) in enumerate(zip(summaries, cohorts, readings)):
        require(int(summary['index']) == i and int(summary['start_s']) == i * WINDOW and
                float(summary['seconds']) == WINDOW and int(summary['submitted']) == len(frames) and
                int(summary['completed']) == len(frames) and summary['errors'] == summary['pending'] == '0',
                f'window {i}: frame counts do not match')
        require(samples and int(summary['freq_samples']) == len(samples) and
                int(summary['min_level']) == min(s[1] for s in samples) and
                int(summary['max_level']) == max(s[1] for s in samples) and
                int(summary['max_mhz']) == max(s[2] for s in samples), f'window {i}: missing or invalid frequency readings')
        gpu_timed = sum(bool(f['gpu_start_s'] and f['gpu_end_s'] and
                             number(f['gpu_end_s']) > number(f['gpu_start_s'])) for f in frames)
        require(gpu_timed > 0 and gpu_timed == int(summary['gpu_timed']), f'window {i}: GPU timing missing or invalid')
        completions = sorted(number(f['completion_callback_s']) for f in frames)
        distinct = []
        for stamp in completions:
            if stamp <= last_completion:
                continue
            distinct.append((stamp, last_completion))
            last_completion = stamp
        intervals = sorted((stamp - last) * 1000 for stamp, last in distinct if last)
        fps = len(distinct) / WINDOW
        require(intervals and int(summary['render_frame_n']) == len(intervals) and
                abs(float(summary['render_fps']) - fps) < 1e-5, f'window {i}: incomplete completion cadence')
        for key, value in (('p50', percentile(intervals, .50)), ('p95', percentile(intervals, .95)),
                           ('p99', percentile(intervals, .99)), ('max', intervals[-1])):
            require(abs(float(summary[f'render_frame_{key}_ms']) - value) < .01,
                    f'window {i}: completion {key} does not match CSV')
        p99 = percentile(intervals, .99)
        window_stalls = sum(t > 50 for t in intervals)
        require(abs(int(summary['render_frame_stalls']) - window_stalls) <= sum(abs(t - 50) <= .002 for t in intervals),
                f'window {i}: completion stall count does not match CSV')
        if window_stalls:
            stall_windows.append(i)
        if fps < FPS_MIN:
            low_fps_windows.append(i)
        # The CSV rounds absolute stamps to nanoseconds before subtraction.
        if p99 > P99_MAX_MS + .000002:
            slow_windows.append(i)
        worst_p99, lowest_fps = max(worst_p99, p99), min(lowest_fps, fps)
        stalls += window_stalls
        total += len(distinct)
        presented += int(summary['presented'])

    peak_level = max(level for samples in readings for _, level, _ in samples)
    print(f'Adreno 3D completed={total} fps={total / SECONDS:.2f} min_10s_fps={lowest_fps:.2f} '
          f'worst_10s_p99_ms={worst_p99:.2f} stalls_gt_50_ms={stalls} stall_windows={stall_windows} '
          f'peak_level={peak_level} presented={presented}')
    if not presented:
        print('Drawable presentation timestamps unavailable. FPS and frame intervals use successful Metal completions.')
    require(not low_fps_windows and not slow_windows,
            f'performance floor: fps < {FPS_MIN:g} in {low_fps_windows}; p99 > {P99_MAX_MS:g} ms in {slow_windows}')
    print('PASS installed Metal sustain')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: grade.py RESULT_DIRECTORY')
    try:
        grade(Path(sys.argv[1]))
    except (KeyError, OSError, StopIteration, TypeError, ValueError, csv.Error) as error:
        sys.exit(f'FAIL installed Metal sustain: {error or "missing capture record"}')
