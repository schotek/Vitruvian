#!/usr/bin/env python3
#
# Author: Vláďa Janeček <vlada@janecek.cloud>
"""Test suite for the native haiku QPA plugin, driven from the host.

Expects a debug ISO already booted (`bash build/host/boot.sh`, GRUB entry
"Vitruvian Live (Debug)") with the plugin deployed and the Qt build
dependencies present — see README.md for the provisioning one-liner, or run
with --provision to have it done here.

    python3 tests/qt/run_tests.py [--provision] [--keep] [-k PATTERN]

Screenshots and logs of every test land in tests/qt/artifacts/ so a failure
can be looked at rather than guessed at.

The image assertions are deliberately structural — "these saturated colours
cover at least this many pixels", "a window tab of the BeOS colour exists" —
rather than golden-image comparisons, which would break on every theme or
font change without telling us anything about the plugin.
"""
import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vm  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.join(HERE, "artifacts")

# Sampled from a real session rather than assumed: the active window tab
# is an orange gradient around (237,167,51); the desktop behind it is
# (51,102,152). See README.
TAB_RGB = (237, 167, 51)
TAB_TOLERANCE = 28

QUADRANTS = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 0, 255)]

PLUGIN = "/usr/lib/x86_64-linux-gnu/qt6/plugins/platforms/libqhaiku.so"
# Not under /root: that is mode 700, so vos-live cannot even traverse it.
TESTBED = "/usr/local/bin/testbed"
# Every GUI command goes through this: it re-exports app_server's environment
# so the child lands in the running session, and pins HOME and the runtime
# directory, which the root ssh session would otherwise supply as root's.
GUI = ("cd /home/vos-live && runuser -u vos-live -- /tmp/gui.sh "
       "env HOME=/home/vos-live XDG_RUNTIME_DIR=/run/user/%s "
       "QT_QPA_PLATFORM=haiku ")

results = []


def record(name, ok, detail=""):
    results.append((name, ok, detail))
    print(("  PASS  " if ok else "  FAIL  ") + name + (" — " + detail if detail else ""))
    return ok


def sh(cmd, timeout=120):
    status, out = vm.capture(cmd, timeout=timeout)
    return out


def uid():
    return sh("id -u vos-live").strip().splitlines()[-1].strip()


def gui(cmd):
    return GUI % uid() + cmd


def grab(name):
    """Screenshot the VM desktop and fetch it. Returns the local path."""
    remote = "/tmp/shot-%s.png" % name
    sh("rm -f %s; %s screenshot -s %s >/dev/null 2>&1; echo done"
       % (remote, gui(""), remote))
    local = os.path.join(ART, name + ".png")
    vm.scp("root@localhost:" + remote, local)
    return local


def _mask(img, rgb, tolerance):
    """Boolean array of pixels within `tolerance` of `rgb`, per channel."""
    import numpy as np
    a = np.asarray(img.convert("RGB"), dtype=np.int16)
    return (np.abs(a - np.array(rgb, dtype=np.int16)) <= tolerance).all(axis=2)


def colour_hits(img, rgb, tolerance=20):
    return int(_mask(img, rgb, tolerance).sum())


def has_window_tab(img):
    """A BeOS window tab is a horizontal run of the tab colour. Counting
    pixels alone would also accept scattered orange in an icon, so require
    a contiguous run wider than any icon."""
    import numpy as np
    m = _mask(img, TAB_RGB, TAB_TOLERANCE)
    # Longest run of True per row: reset the running count on every False.
    for row in m:
        # The gaps between the non-matching pixels are the matching runs.
        idx = np.flatnonzero(~row)
        if idx.size == 0:
            if row.size >= 60:
                return True
            continue
        gaps = np.diff(np.r_[-1, idx, row.size]) - 1
        if gaps.max() >= 60:
            return True
    return False


def testbed_start():
    """Start the testbed with a fifo it opens itself, and its output going to
    a file we can read incrementally."""
    sh("rm -f /tmp/tb.in; mkfifo /tmp/tb.in; chmod 666 /tmp/tb.in; "
       ": > /tmp/tb.out; chmod 666 /tmp/tb.out")
    sh("%s %s --fifo /tmp/tb.in </dev/null >/tmp/tb.out 2>&1 & sleep 12; echo started"
       % (gui(""), TESTBED), timeout=90)


def tb_size():
    out = sh("stat -c %s /tmp/tb.out 2>/dev/null || echo 0").strip().splitlines()
    return int(out[-1].strip() or 0) if out else 0


def tb_since(offset):
    """Read only what the testbed printed after `offset`. Truncating the file
    instead would leave a hole: the testbed holds it open and keeps writing
    at its own offset."""
    return sh("tail -c +%d /tmp/tb.out 2>/dev/null || true" % (offset + 1))


def testbed_cmd(cmd, wait=2.0):
    before = tb_size()
    sh("printf '%%s\\n' %s > /tmp/tb.in" % quote(cmd))
    time.sleep(wait)
    return tb_since(before)


def quote(s):
    return "'" + s.replace("'", "'\\''") + "'"


def ensure_testbed():
    """Bring the shared testbed up if it is not running. T6 deliberately
    kills it, so later tests cannot assume the one started at the beginning
    is still there."""
    if running("testbed"):
        return True
    testbed_start()
    return running("testbed")


def running(name):
    out = sh("pgrep -c -u vos-live -x %s || true" % name).strip().splitlines()
    return out and out[-1].strip().isdigit() and int(out[-1].strip()) > 0


# --------------------------------------------------------------------------


def t1_build():
    """T1 — the plugin is present and fully linked."""
    out = sh("ls -l %s 2>&1; ldd %s 2>&1 | grep -c 'not found' || true"
             % (PLUGIN, PLUGIN))
    if "No such file" in out:
        return record("T1 build", False, "plugin not deployed at " + PLUGIN)
    missing = out.strip().splitlines()[-1].strip()
    return record("T1 build", missing == "0",
                  "unresolved libraries: " + missing)


def t2_load():
    """T2 — Qt selects the plugin and reports platform "haiku"."""
    # Grep the whole output rather than its first lines: Qt prints a
    # four-line locale warning before the version banner whenever the
    # session locale is not UTF-8, which pushes the interesting line out
    # of any fixed head.
    if "not found" in sh("command -v qtdiag6 || echo not found"):
        return record("T2 load", False,
                      "qtdiag6 is missing — install qt6-base-dev-tools")
    out = sh(gui("qtdiag6") + " 2>/dev/null")
    line = next((l for l in out.splitlines() if l.startswith("Qt ")), "")
    return record("T2 load", 'on "haiku"' in line,
                  line.strip() or "qtdiag6 printed no version banner")


def t3_window():
    """T3 — the testbed's pattern reaches the screen inside a BeOS window."""
    if not ensure_testbed():
        return record("T3 window", False, "testbed did not start")
    from PIL import Image
    img = Image.open(grab("t3-window"))
    missing = [c for c in QUADRANTS if colour_hits(img, c) < 2000]
    if missing:
        return record("T3 window", False,
                      "quadrant colours missing: %s" % missing)
    tab = has_window_tab(img)
    return record("T3 window", tab,
                  "pattern drawn, BeOS tab %s"
                  % ("present" if tab else "NOT found"))


def t4_input():
    """T4 — injected evdev events arrive as Qt events."""
    if not ensure_testbed():
        return record("T4 input", False, "testbed is not running")
    before = tb_size()
    # Click into the text edit first: keystrokes go to the focused widget,
    # and the pattern widget above it accepts no text.
    sh("python3 /root/inject.py pin move 640 520 click sleep 1 type vitruvian",
       timeout=120)
    time.sleep(2)
    out = tb_since(before)
    keys = len(re.findall(r"^EV key press", out, re.M))
    clicks = len(re.findall(r"^EV mouse press", out, re.M))
    return record("T4 input", keys >= 8 and clicks >= 1,
                  "%d key events, %d mouse events" % (keys, clicks))


def t5_clipboard():
    """T5 — the clipboard bridges both ways between Qt and BeOS."""
    if not ensure_testbed():
        return record("T5 clipboard", False, "testbed is not running")
    marker = "vos-clip-%d" % int(time.time())
    testbed_cmd("clip-set " + marker)
    # The BeOS side is /usr/bin/clipboard: -p prints, -i reads stdin.
    out = sh(gui("clipboard -p") + " 2>&1 || true")
    to_beos = marker in out
    # Via a file, not a pipe: gui() begins with "cd … &&", so piping into it
    # feeds the cd rather than the command.
    sh("printf '%%s' %s > /tmp/clip.txt; chmod 666 /tmp/clip.txt"
       % quote(marker + "-back"))
    sh(gui("sh -c 'clipboard -i < /tmp/clip.txt'") + " >/dev/null 2>&1 || true")
    back = testbed_cmd("clip-get")
    from_beos = (marker + "-back") in back
    return record("T5 clipboard", to_beos and from_beos,
                  "Qt->BeOS %s, BeOS->Qt %s"
                  % ("ok" if to_beos else "failed",
                     "ok" if from_beos else "failed"))


def t6_lifecycle():
    """T6 — repeated start/quit finishes without wedging the looper."""
    for i in range(3):
        sh("rm -f /tmp/life.out; %s %s </dev/null >/tmp/life.out 2>&1 & sleep 8; echo up"
           % (gui(""), TESTBED), timeout=90)
        if not running("testbed"):
            return record("T6 lifecycle", False, "round %d: did not start" % (i + 1))
        sh("pkill -x testbed; sleep 3; echo killed")
        if running("testbed"):
            return record("T6 lifecycle", False,
                          "round %d: still alive after SIGTERM" % (i + 1))
    # Leave the shared testbed running: the tests after this one need it.
    ensure_testbed()
    return record("T6 lifecycle", True, "3 rounds")


def t7_dialog():
    """T7 — the file dialog opens as a native BFilePanel window."""
    from PIL import Image
    if not ensure_testbed():
        return record("T7 dialog", False, "testbed is not running")
    before = Image.open(grab("t7-before"))
    reply = testbed_cmd("dialog", wait=4.0)
    after = Image.open(grab("t7-after"))
    if "DIALOG opened" not in reply:
        return record("T7 dialog", False, "testbed did not acknowledge: " + reply.strip()[-80:])
    # A second window means more of the desktop is covered; comparing the
    # count of desktop-coloured pixels is far more robust than looking for
    # a title, which is localised.
    desk = (51, 102, 152)
    freed = colour_hits(before, desk) - colour_hits(after, desk)
    return record("T7 dialog", freed > 20000 and has_window_tab(after),
                  "desktop pixels covered by the new window: %d" % freed)


TESTS = [t1_build, t2_load, t3_window, t4_input, t5_clipboard, t6_lifecycle,
         t7_dialog]


def install_gui_helper():
    """Every GUI command in the suite goes through this; a live ISO loses it
    on every reboot, so put it there rather than assuming it."""
    vm.scp(os.path.join(HERE, "gui.sh"), "root@localhost:/tmp/gui.sh")
    sh("chmod 755 /tmp/gui.sh; echo ok")


def provision(plugin=None):
    print("== provisioning ==")
    # qt6-svg-plugins carries the SVG icon-engine and image-format plugins
    # (libqsvgicon.so / libqsvg.so). Debian only Recommends it from libqt6gui6,
    # so --no-install-recommends drops it and SVG icon themes (breeze) render as
    # text-only toolbars. qt6-image-formats-plugins covers webp/tiff/icns icons.
    sh("DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "
       "build-essential qt6-base-dev qt6-base-private-dev qt6-base-dev-tools "
       "qt6-svg-plugins qt6-image-formats-plugins "
       "pkg-config libfreetype-dev 2>&1 | tail -1", timeout=2400)
    if plugin:
        # A plugin built during an earlier boot: copying beats rebuilding,
        # and the .so does not depend on anything the live ISO forgets.
        vm.scp(plugin, "root@localhost:/tmp/libqhaiku.so")
        print(sh("install -m 644 /tmp/libqhaiku.so %s && ls -l %s | tail -1"
                 % (PLUGIN, PLUGIN)).strip().splitlines()[-1])
    else:
        # qtbuild.sh is bash, not python — it tars the checkout up and builds
        # it inside the VM.
        subprocess.run(["bash", os.path.join(HERE, "qtbuild.sh")], check=False)


def build_testbed():
    print("== building testbed in the VM ==")
    vm.scp(os.path.join(HERE, "testbed.cpp"), "root@localhost:/tmp/testbed.cpp")
    vm.scp(os.path.join(HERE, "inject.py"), "root@localhost:/root/inject.py")
    out = sh("cd /tmp && g++ -fPIC testbed.cpp -o /tmp/testbed.bin "
             "$(pkg-config --cflags --libs Qt6Widgets) 2>&1 | tail -5; "
             "install -m 755 /tmp/testbed.bin %s 2>&1; "
             "ls -l %s 2>&1 | tail -1" % (TESTBED, TESTBED), timeout=600)
    print(out.strip().splitlines()[-1] if out.strip() else "(no output)")
    return TESTBED in out and "No such file" not in out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--provision", action="store_true",
                    help="install build dependencies and rebuild the plugin first")
    ap.add_argument("--keep", action="store_true",
                    help="leave the testbed running afterwards")
    ap.add_argument("--plugin", metavar="SO",
                    help="deploy this prebuilt libqhaiku.so instead of rebuilding it")
    ap.add_argument("-k", metavar="PATTERN", default="",
                    help="only run tests whose name contains PATTERN")
    args = ap.parse_args()

    os.makedirs(ART, exist_ok=True)

    status, _ = vm.capture("true", timeout=25)
    if status != 0:
        print("!! no VM on localhost:2222 — boot the Debug ISO first", file=sys.stderr)
        return 2

    install_gui_helper()
    if args.provision or args.plugin:
        provision(args.plugin)
    if not build_testbed():
        print("!! testbed did not build", file=sys.stderr)
        return 2

    print("== tests ==")
    sh("pkill -x testbed; echo cleared")
    testbed_start()

    for t in TESTS:
        if args.k and args.k not in t.__name__:
            continue
        try:
            t()
        except Exception as exc:  # a broken test must not hide the others
            record(t.__name__, False, "exception: %r" % (exc,))

    if not args.keep:
        sh("pkill -x testbed; echo cleared")

    failed = [n for n, ok, _ in results if not ok]
    print("\n%d/%d passed, artefacts in %s"
          % (len(results) - len(failed), len(results), ART))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())