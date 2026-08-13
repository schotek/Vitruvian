# GLTeapot freeze on click — root cause analysis

Author: Vláďa Janeček <vlada@janecek.cloud>

## Symptom

While GLTeapot is running, a left click on the teapot stops the rotation
permanently. Afterwards nothing revives it: dragging does not rotate the
model, a flick-release does not re-spin it, menu toggles and the FPS
switch have no visible effect. The application appears frozen, although
the window itself stays responsive (cursor changes, menus open).

## Root cause

Nexus semaphores do not implement the BeOS negative-count convention,
and GLTeapot's draw-thread wakeup is written exactly against it.

On BeOS/Haiku, `acquire_sem()` on an empty semaphore drives the count
*below zero*; `get_sem_count()` then returns a negative value whose
absolute value is the number of waiting threads. GLTeapot's `setEvent()`
(`src/apps/glteapot/ObjectView.cpp`) relies on that contract:

```c
get_sem_count(event, &c);
if (c < 0)
    release_sem_etc(event, -c, 0);   // wake the waiters
```

Nexus keeps `sem->count >= 0` at all times: a blocking acquirer is put
on the `waiters` list and the count stays at 0
(`nexus/sem.c`, `nexus_acquire_sem()`); `nexus_get_sem_count()` returns
the plain `sem->count`, so waiters are invisible. libroot2 passes the
value through unchanged.

The freeze chain:

1. A click on the teapot is a "grab" in the original Be sample code:
   `MouseDown()` → `Spin(0, 0)` — the spin velocity is zeroed
   (same behaviour as on Haiku; a drag-and-release flick re-spins it).
2. The draw thread ("Simon") finds nothing to redraw (`SpinIt()` ==
   false) and parks in `waitEvent(drawEvent)` = `acquire_sem()`.
3. Every wake path — `MouseMoved` drag, `MouseUp` flick, menu toggles,
   `FrameResized` — goes through `setEvent()`. With nexus the count
   reads 0, never negative, so `release_sem_etc()` is never called.
4. The draw thread sleeps forever. Input events are still delivered and
   fully processed (the model's orientation quaternion keeps changing
   during drags); only the redraw never happens.

## Evidence (captured in a live VM session)

- Frozen state: draw thread blocked in `nexus_sem_ioctl` (D state);
  `listsem` shows `quitting sem` = 1 (thread is outside its loop,
  parked on the event), `draw event` = 0; the window thread sits in
  a normal port receive; the view cursor was correctly switched to the
  open-hand cursor by `MouseUp` — no deadlock anywhere.
- Releasing the `draw event` semaphore once from a helper process
  (`release_sem(id)`) instantly revived the app: the teapot jumped to
  the orientation accumulated from the earlier "dead" drags and started
  spinning with the velocity from the earlier flick-release. All events
  had been processed; only the wakeup was lost.

## Impact beyond GLTeapot

- In-tree, only GLTeapot depends on the negative-count convention.
  Mandelbrot (`FractalEngine.cpp`) and Terminal (`TermParse.cpp`) use
  `get_sem_count()` with positive counts only, which nexus handles
  correctly.
- The VOS test harness (`src/tests/vos/testharness/testsem.cpp`) does
  not cover the "blocked waiter → negative count" case, so nothing
  flags the divergence.
- Any ported BeOS-era code using the same well-documented pattern (the
  Be Book explicitly describes negative counts) will misbehave the same
  way.

## Fix options

**A) Fix nexus (root cause).** `nexus_get_sem_count()` — and, for
consistency, the `sem_info.count` paths — report
`sem->count − Σ(pending waiter requests)`, i.e. the BeOS semantics
(`-N` with N typical waiters). Small read-only change under the
existing semaphore lock. Restores the API contract for all current and
future BeOS-pattern code.

**B) Patch GLTeapot.** Make `setEvent()`/`signalEvent()` an
unconditional `release_sem(event)`; `waitEvent()` already drains any
surplus. Works under both semantics, but leaves the kernel divergence
in place for other code to trip over.

**C) Both.** A restores the contract, B makes the app robust
regardless of kernel.
