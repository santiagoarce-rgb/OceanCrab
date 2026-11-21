#!/usr/bin/env python3
"""Correlate button presses with CSpongeBobEnt fields (phase 2).

Reads a PSPRECOMP_PLAYER_RECORD file and, for every PSP button the player
pressed, lists the object words that react within a short window after the
press, ranked by how consistently they react. That is how jump height, attack
state, timers and flags show up.

usage: player_record.py <record.bin> [--window N] [--top N]
"""
import argparse
import struct
import sys
from collections import defaultdict

OBJECT_SIZE = 0x928
RECORD = 16 + OBJECT_SIZE
BUTTONS = {
    0x0001: 'SELECT', 0x0008: 'START', 0x0010: 'UP', 0x0020: 'RIGHT', 0x0040: 'DOWN',
    0x0080: 'LEFT', 0x0100: 'L', 0x0200: 'R', 0x1000: 'TRIANGLE', 0x2000: 'CIRCLE',
    0x4000: 'CROSS', 0x8000: 'SQUARE',
}


def load(path):
    data = open(path, 'rb').read()
    if data[:4] != b'SBR1':
        sys.exit('not a PSPRECOMP_PLAYER_RECORD file')
    frames = []
    for off in range(4, len(data) - RECORD + 1, RECORD):
        frame, buttons, stick, entity = struct.unpack_from('<4I', data, off)
        words = struct.unpack_from(f'<{OBJECT_SIZE // 4}I', data, off + 16)
        frames.append((frame, buttons, stick & 0xFF, (stick >> 8) & 0xFF, entity, words))
    return frames


def as_float(word):
    return struct.unpack('<f', struct.pack('<I', word))[0]


def describe(values):
    floats = [as_float(v) for v in values]
    if all(abs(f) < 1e5 and (f == 0.0 or abs(f) > 1e-5) for f in floats):
        return 'float ' + ' '.join(f'{f:.2f}' for f in floats)
    return 'int   ' + ' '.join(f'{v:X}' for v in values)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('record')
    ap.add_argument('--window', type=int, default=20, help='frames after a press to watch')
    ap.add_argument('--top', type=int, default=15)
    args = ap.parse_args()
    frames = load(args.record)
    if len(frames) < 2:
        sys.exit('record is empty (was a level loaded?)')
    print(f'{len(frames)} frames, {frames[0][0]}..{frames[-1][0]}, entity 0x{frames[0][4]:08X}')
    nwords = OBJECT_SIZE // 4

    # How often each word changes from one frame to the next, in general.
    base_change = [0] * nwords
    for a, b in zip(frames, frames[1:]):
        for i in range(nwords):
            if a[5][i] != b[5][i]:
                base_change[i] += 1
    base_rate = [c / (len(frames) - 1) for c in base_change]

    for mask, name in BUTTONS.items():
        presses = [k for k in range(1, len(frames))
                   if frames[k][1] & mask and not frames[k - 1][1] & mask]
        if not presses:
            continue
        reacts = defaultdict(int)
        for k in presses:
            before = frames[k - 1][5]
            window = frames[k:k + args.window]
            for i in range(nwords):
                if any(f[5][i] != before[i] for f in window):
                    reacts[i] += 1
        # Score: reacts on most presses, but rarely changes otherwise.
        scored = sorted(((reacts[i] / len(presses)) - base_rate[i], i) for i in reacts)
        scored = [s for s in scored[::-1] if s[0] > 0.3][:args.top]
        print(f'\n== {name}: {len(presses)} presses (frames {", ".join(str(frames[k][0]) for k in presses[:8])}'
              f'{"…" if len(presses) > 8 else ""})')
        sample = presses[0]
        for score, i in scored:
            values = [f[5][i] for f in frames[max(0, sample - 1):sample + args.window:4]]
            print(f'  +0x{i * 4:03X} score={score:.2f} base={base_rate[i]:.2f}  {describe(values)}')


if __name__ == '__main__':
    main()
