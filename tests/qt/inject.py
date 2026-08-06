#!/usr/bin/env python3
#
# Author: Vláďa Janeček <vlada@janecek.cloud>
"""evdev injector for the Vitruvian VM (run as root inside the VM).

Commands (sequenced from argv):
  pin              drive the cursor to the top-left corner (0,0)
  move DX DY       walk the cursor by DX,DY in 1px steps (below accel threshold)
  click            left click
  esc              press Escape
  type TEXT        type ascii text ('_' = space)
  enter            press Enter
  sleep SECS       pause

Keyboard = event0, relative mouse = event3 (event2 is the absolute tablet,
which input_server ignores). struct input_event = llHHi; EV_SYN after every
event; ~30 ms key pacing per the F2 recipe.
"""
import struct
import sys
import time

KBD = '/dev/input/event0'
MOUSE = '/dev/input/event3'
EV_SYN, EV_KEY, EV_REL = 0, 1, 2
REL_X, REL_Y = 0, 1
BTN_LEFT = 0x110
KEY_ENTER, KEY_ESC = 28, 1
KEYS = {
    'a': 30, 'b': 48, 'c': 46, 'd': 32, 'e': 18, 'f': 33, 'g': 34, 'h': 35,
    'i': 23, 'j': 36, 'k': 37, 'l': 38, 'm': 50, 'n': 49, 'o': 24, 'p': 25,
    'q': 16, 'r': 19, 's': 31, 't': 20, 'u': 22, 'v': 47, 'w': 17, 'x': 45,
    'y': 21, 'z': 44, '1': 2, '2': 3, '3': 4, '4': 5, '5': 6, '6': 7,
    '7': 8, '8': 9, '9': 10, '0': 11, '-': 12, '.': 52, '/': 53, '_': 57,
}


def emit(f, etype, code, value):
    f.write(struct.pack('llHHi', 0, 0, etype, code, value))
    f.flush()


def syn(f):
    emit(f, EV_SYN, 0, 0)


def keypress(f, code):
    emit(f, EV_KEY, code, 1)
    syn(f)
    time.sleep(0.03)
    emit(f, EV_KEY, code, 0)
    syn(f)
    time.sleep(0.03)


def rel(f, dx, dy):
    if dx:
        emit(f, EV_REL, REL_X, dx)
    if dy:
        emit(f, EV_REL, REL_Y, dy)
    syn(f)


_tablet = None


def tablet():
    global _tablet
    if _tablet is None:
        _tablet = open('/dev/input/event2', 'wb')
    return _tablet


_tablet_max = {}


def tablet_max(axis):
    if axis not in _tablet_max:
        import fcntl
        buf = bytearray(24)
        fcntl.ioctl(tablet(), 0x80184540 + axis, buf)
        _v, _mn, mx = struct.unpack('3i', bytes(buf[:12]))
        _tablet_max[axis] = mx if mx > 0 else 32767
    return _tablet_max[axis]


def main():
    mouse = open(MOUSE, 'wb')
    kbd = open(KBD, 'wb')
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        cmd = args[i]
        if cmd == 'pin':
            for _ in range(60):
                rel(mouse, -100, -100)
                time.sleep(0.004)
        elif cmd == 'move':
            dx, dy = int(args[i + 1]), int(args[i + 2])
            i += 2
            sx, sy = (1 if dx > 0 else -1), (1 if dy > 0 else -1)
            ax, ay = abs(dx), abs(dy)
            while ax > 0 or ay > 0:
                stepx = sx if ax > 0 else 0
                stepy = sy if ay > 0 else 0
                rel(mouse, stepx, stepy)
                ax -= abs(stepx)
                ay -= abs(stepy)
                time.sleep(0.006)
        elif cmd == 'click':
            emit(mouse, EV_KEY, BTN_LEFT, 1)
            syn(mouse)
            time.sleep(0.09)
            emit(mouse, EV_KEY, BTN_LEFT, 0)
            syn(mouse)
            time.sleep(0.05)
        elif cmd == 'down':
            emit(mouse, EV_KEY, BTN_LEFT, 1)
            syn(mouse)
            time.sleep(0.09)
        elif cmd == 'up':
            emit(mouse, EV_KEY, BTN_LEFT, 0)
            syn(mouse)
            time.sleep(0.05)
        elif cmd == 'mclick':
            # BTN_MIDDLE via the ABSOLUTE tablet (event2): the relative
            # mouse (event3) does NOT declare BTN_MIDDLE, so the kernel
            # silently drops it there (verified via EVIOCGBIT). Position
            # the tablet cursor first with 'mmove'.
            emit(tablet(), EV_KEY, 0x112, 1)
            syn(tablet())
            time.sleep(0.09)
            emit(tablet(), EV_KEY, 0x112, 0)
            syn(tablet())
            time.sleep(0.05)
        elif cmd == 'mmove':
            # Absolute position via the tablet, scaled to its axis range.
            x, y = int(args[i + 1]), int(args[i + 2])
            i += 2
            t = tablet()
            emit(t, 3, 0, x * tablet_max(0) // 1280)  # EV_ABS ABS_X
            emit(t, 3, 1, y * tablet_max(1) // 800)   # EV_ABS ABS_Y
            syn(t)
            time.sleep(0.1)
        elif cmd == 'wheel':
            n = int(args[i + 1])
            i += 1
            step = 1 if n > 0 else -1
            for _ in range(abs(n)):
                emit(mouse, EV_REL, 8, step)  # REL_WHEEL
                syn(mouse)
                time.sleep(0.03)
        elif cmd == 'rdown':
            emit(mouse, EV_KEY, 0x111, 1)
            syn(mouse)
            time.sleep(0.09)
        elif cmd == 'rup':
            emit(mouse, EV_KEY, 0x111, 0)
            syn(mouse)
            time.sleep(0.05)
        elif cmd == 'esc':
            keypress(kbd, KEY_ESC)
        elif cmd == 'key':
            keypress(kbd, int(args[i + 1]))
            i += 1
        elif cmd == 'keydown':
            emit(kbd, EV_KEY, int(args[i + 1]), 1)
            syn(kbd)
            time.sleep(0.03)
            i += 1
        elif cmd == 'keyup':
            emit(kbd, EV_KEY, int(args[i + 1]), 0)
            syn(kbd)
            time.sleep(0.03)
            i += 1
        elif cmd == 'enter':
            keypress(kbd, KEY_ENTER)
        elif cmd == 'type':
            for ch in args[i + 1]:
                keypress(kbd, KEYS[ch])
            i += 1
        elif cmd == 'sleep':
            time.sleep(float(args[i + 1]))
            i += 1
        else:
            print(f'unknown command: {cmd}', file=sys.stderr)
            return 2
        i += 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
