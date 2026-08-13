# Native Qt (haiku QPA) test helpers

Work in progress for the native Qt backend discussed in
[issue #214](https://github.com/VitruvianOS/Vitruvian/issues/214).

Vitruvian runs Debian's stock Qt 6; the platform integration comes from the
`haiku` QPA plugin built against libbe. These helpers drive a running debug
VM to build and exercise it.

| File | Purpose |
|---|---|
| `vm.py` | SSH orchestration for the debug VM: `run` / `capture` / `get` / `put` (root:live on localhost:2222, `VM_TIMEOUT` env for the timeout) |
| `inject.py` | evdev input injector, run inside the VM — events traverse the real input_server → app_server path instead of being synthesised |
| `qtbuild.sh` | sync the plugin checkout into the VM and build `src/platform` there (`QHP_SRC` overrides the checkout path) |
| `testbed.cpp` | Qt application built for testing: a known colour pattern, event logging on stdout, and a command channel on stdin |
| `run_tests.py` | the suite, T1–T7, driven from the host |

## Running the suite

Boot a debug ISO and pick the **Vitruvian Live (Debug)** GRUB entry — the plain
entry leaves `vos-sshdebug.service` inert and there is no ssh to drive:

```sh
bash build/host/boot.sh
```

The VM is a live ISO, so a reboot loses everything. Provision it once per boot:

```sh
python3 tests/qt/run_tests.py --provision
```

`--provision` installs the Qt build dependencies and rebuilds the plugin from
the checkout. Without it the suite assumes both are already in place and only
rebuilds the testbed, which is quick. Useful flags: `-k t4` to run one test,
`--keep` to leave the testbed running for poking at by hand.

Screenshots and logs land in `tests/qt/artifacts/`.

The suite also expects `/tmp/gui.sh` in the VM — a two-line helper that
re-exports app_server's environment so a child process joins the running
session. `--provision` writes it; see the README history if you need it
standalone.

## The tests

| | What it proves |
|---|---|
| T1 build | the plugin is deployed and every library it needs resolves |
| T2 load | Qt selects it — `qtdiag6` reports platform `haiku` |
| T3 window | the testbed's colour pattern reaches the screen, inside a window carrying a BeOS tab |
| T4 input | injected evdev events arrive as `QKeyEvent`/`QMouseEvent` — the whole input_server → app_server → QPA chain |
| T5 clipboard | text crosses between `QClipboard` and `BClipboard` in both directions |
| T6 lifecycle | three start/quit rounds finish without wedging the looper |
| T7 dialog | `QFileDialog` opens as a native `BFilePanel` window |

## Why the assertions look the way they do

They are **structural**, not golden images: "these four saturated colours each
cover at least 2000 pixels", "a horizontal run of at least 60 tab-coloured
pixels exists", "the new window covers at least 20000 desktop-coloured pixels".
A golden-image comparison would fail on every theme, font or icon change while
telling us nothing about the plugin — which is the only thing under test here.

The colour constants are sampled from a real session rather than assumed: the
active window tab is an orange gradient around `(237,167,51)` and the desktop
behind it is `(51,102,152)`. The testbed paints saturated red, green, blue and
magenta, none of which occurs in the theme or the icon set, so finding them is
unambiguous.

## Notes

- Qt6 SVG icons need `qt6-svg-plugins` (it ships `iconengines/libqsvgicon.so`
  and `imageformats/libqsvg.so`). Debian only Recommends it from `libqt6gui6`,
  so `--no-install-recommends` drops it and themed toolbars render text-only;
  `--provision` installs it.
- FeatherPad has a singleton: `pkill -9 -x featherpad` before a test run.
- The testbed answers every command on stdout (`READY`, `DIALOG opened`,
  `CLIP get …`), so the driver never has to sleep and hope.
- `pkill -f <pattern>` over ssh will match the shell running that very command
  and kill it. Use `-x` and an exact name.