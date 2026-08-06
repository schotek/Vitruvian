# Native Qt (haiku QPA) test helpers

Work in progress for the native Qt backend discussed in
[issue #214](https://github.com/VitruvianOS/Vitruvian/issues/214).

Vitruvian runs Debian's stock Qt 6; the platform integration comes from the
`haiku` QPA plugin built against libbe. These helpers drive a running debug
VM to build and exercise it.

| File | Purpose |
|---|---|
| `vm.py` | SSH orchestration for the debug VM: `run` / `get` / `put` (root:live on localhost:2222, `VM_TIMEOUT` env for the timeout) |
| `inject.py` | evdev input injector, run inside the VM — events traverse the real input_server → app_server path instead of being synthesised |
| `qtbuild.sh` | sync the plugin checkout into the VM and build `src/platform` there (`QHP_SRC` overrides the checkout path) |
| `testbed.cpp` | minimal Qt widget application: themed toolbar icons, menus, text edit |

Typical loop, with a debug ISO already booted (`bash build/host/boot.sh`):

```
tests/qt/qtbuild.sh                     # build the plugin inside the VM
python3 tests/qt/vm.py run 'cp /root/qhp/build/libqhaiku.so \
    /usr/lib/x86_64-linux-gnu/qt6/plugins/platforms/'
python3 tests/qt/vm.py run 'QT_QPA_PLATFORM=haiku qtdiag6'
```

Provisioning the VM once per boot (it is a live image, so this does not
survive a reboot):

```
apt-get install -y build-essential qt6-base-dev qt6-base-private-dev \
    qt6-base-dev-tools qt6-tools-dev-tools qt6-svg-plugins pkg-config \
    libfreetype-dev
```

Still to come: `build/host/test-qt.sh` wrapping boot → provision → build →
assert → teardown, and the screenshot-based assertions themselves.
