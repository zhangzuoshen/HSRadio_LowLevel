# HSRadio - Hardware Layer (Phase 1)

Qt 5.12 / C++17 ALSA backend for the HSRadio service, targeting the
SAF775X radio chip on the i.MX9 platform.

## Compatibility

- Qt version: Qt 5.12.x (Core module).
- Language standard: C++17.
- Build systems: qmake (`HSRadioHardware.pro`) and CMake (`CMakeLists.txt`).
- No Qt 6-only classes or APIs are used by this hardware layer.
- ALSA development headers and library are required on the build host / target sysroot.

## Included

- `HSRadioTypes`: common band, result, signal and hardware-state types.
- `IRadioHardware`: chip-independent synchronous hardware contract.
- `HSRadioSAF775XBackend`: ALSA control implementation for the SAF775X driver,
  speaking the driver's binary ("BYTES blob") protocol directly.

## Build

All build commands are **out-of-source** and run inside `build/`. Build artifacts (`.o`, `moc_*`, `Makefile`, the binary, and runtime `radio_*.json`) stay under `build/` and never pollute the source tree. The `build/` directory is git-ignored.

### qmake

Use the qmake executable from the Qt 5.12 kit configured for the target platform:

```sh
mkdir build && cd build
/path/to/Qt5.12/bin/qmake ../HSRadioHardware.pro
make -j$(nproc)
```

### CMake

```sh
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/path/to/Qt5.12
cmake --build .
```

### Host build without ALSA (mock backend)

On a developer machine without ALSA headers (e.g. Ubuntu) build with
`HSRADIO_ENABLE_ALSA=OFF` (CMake) or `CONFIG+=hsradio_no_alsa` (qmake).
Only the mock backend and the business layer are compiled; the real
`HSRadioSAF775XBackend` is excluded. This is meant for compiling and
debugging the manager / scheduler / storage layers without a radio chip.

```sh
# CMake
cmake .. -DCMAKE_PREFIX_PATH=/path/to/Qt5.12 -DHSRADIO_ENABLE_ALSA=OFF

# qmake
/path/to/Qt5.12/bin/qmake CONFIG+=hsradio_no_alsa ../HSRadioHardware.pro
```

For an i.MX9 cross-build keep the default (`HSRADIO_ENABLE_ALSA` ON); the
target sysroot must provide `alsa/asoundlib.h` and `libasound`.

### Scheduler debug tool (host only)

`HSRadioManagerTest.pro` builds `hsradio_manager_test`, a
parameter-driven debug tool for the scheduler / business layer. It uses
`MockRadioHardware`, so it runs on any developer machine without ALSA headers
or the radio chip. Every action is selected on the command line and prints
`[PASS]` / `[FAIL]` plus the read-back values; there is no fixed
test suite and no assertion count.

```sh
mkdir -p build/manager-test && cd build/manager-test
/path/to/Qt5.12/bin/qmake ../../HSRadioManagerTest.pro
make -j4
./hsradio_manager_test --help
```

Manager commands (every operation goes through `HSRadioManager`):

| Option | Maps to |
|---|---|
| `--state-file path` | state JSON (default `/data/hsradio/radio_state.json`) |
| `--config-file path` | config JSON (default `/data/hsradio/radio_config.json`) |
| `--set-band FM / AM` | `manager.setBand` |
| `--set-frequency kHz` | `manager.setFrequency`; repeatable, executed in order with state/signal read-back |
| `--step-up` / `--step-down` | `manager.stepUp` / `manager.stepDown` |
| `--set-mute on/off` | `manager.setMute` |
| `--refresh` | `manager.refreshSignal` |
| `--seek` | `manager.startSeek` (band range and thresholds come from the config) |
| `--seek-down`, `--seek-mode full\|next`, `--seek-cancel-after ms` | seek options |
| `--add-station kHz[:name]`, `--add-fav kHz[:name]` | add station / favorite, repeatable |
| `--remove-station id`, `--remove-fav id` | remove by id |
| `--list-stations`, `--list-favs` | list current band |
| `--restore`, `--factory-reset` | `restoreLastState` (async, waits for `restoreFinished`) / `factoryReset` |
| `--print-state` | print region/band/frequency and station counts |
| `--no-progress` | suppress per-step seek progress |

After a complete wrapped `FullBand` seek, `seekFinished` reports the scan's start
frequency (no extra scan point is added), so the scan end point does not become
the remembered `lastFrequency`. Cancelled or failed scans still report the
frequency reached when stopping.

Both modes update `lastFrequency` and save state, so the current listening
frequency is remembered. Only `FullBand` rebuilds the band's station list;
`NextStation` is a transient jump that does not add stations or emit
`stationsChanged`.

The backend is selected at build time: an ALSA build uses the real SAF775X
backend, a `hsradio_no_alsa` build falls back to `MockRadioHardware`. Tool output
uses the `hsradio.managertest` logging category.

On startup the scheduler forces `mute=on` before any tune, and
`HSRadioManager::initialize()` only loads the remembered band/frequency into the
manager cache, without sending them to the hardware. Use `isHardwareTuned()` to
tell whether the cached value has actually been sent. Resuming playback
(including unmute) belongs to the HMI audio source manager, not to
`HSRadioManager`.

If a remembered `lastFrequency` falls outside the band range in the active
config, the manager falls back to that band's `defaultKHz`. The correction is
persisted only when it actually changes the value, so a normal startup performs
no state write. An explicit `setFrequency()` outside the range is still clamped
to the band limit.

Examples:

```sh
# Tune three frequencies in order and print the state
./hsradio_manager_test --set-frequency 94900 --set-frequency 96700 \
  --set-frequency 101100 --print-state

# Full-band seek through Manager, with elapsed time
./hsradio_manager_test --seek --seek-mode full --no-progress
```

> This tool only exercises `HSRadioManager`. Scheduler / hardware-level
> interface debugging belongs to `HSRadioHardwareTest`.

The same target is available through CMake:

```sh
cmake .. -DCMAKE_PREFIX_PATH=/path/to/Qt5.12 \
         -DHSRADIO_ENABLE_ALSA=OFF -DHSRADIO_BUILD_TESTS=ON
cmake --build . --target hsradio_manager_test
```

This project builds a static library named `libhsradio_hardware.a` on Linux.

For an i.MX9 cross-build, use the qmake/CMake configured for the i.MX9 Qt 5.12
kit and a sysroot that provides ALSA headers (`alsa/asoundlib.h`) and
`libasound`. Do not hard-code host paths in the project files.

## Thread ownership

`HSRadioSAF775XBackend` is a synchronous QObject. Construct it without a parent, move
it to the dedicated hardware `QThread`, and execute **all** its methods in that
thread. Do not call the methods directly from the service main thread. The
backend checks its owning thread and returns an error for cross-thread calls.

The object must also be shut down and destroyed in its owning thread. A queued
invocation is the intended integration mechanism; the service-level
worker/scheduler wrapper will be added in the next layer.

## SAF775X ALSA control mapping

The SAF775X driver exposes three mixer controls. They are board/driver specific
and supplied via `HSRadioSAF775XConfig` (names default to the reference board).

- `tuneControl` (`Radiocontrol`, **BYTES**): carries the tune request struct
  (`HSRadioSAF775XTune`) and returns signal information on read-back. **Band, frequency
  and signal status are NOT separate controls** — they are fields of this single
  BYTES blob.
- `muteControl` (`Mute_radio`, **INTEGER/BOOLEAN**): 0 = unmute, 1 = mute.
- `regControl` (`Radioregvalue`, **BYTES**, optional): register read/write
  (Phase 1 out of scope; the control exists, no API yet).

### `HSRadioSAF775XTune` layout (must match the kernel driver byte-for-byte)

| field       | type            | meaning                                              |
|-------------|-----------------|------------------------------------------------------|
| `mode`      | `unsigned char` | 0=standby, 0x10=preset, 0x20=search, 0x70=read-back  |
| `band`      | `unsigned char` | 0 = FM, 1 = AM                                       |
| `freq`      | `unsigned int`  | 10 kHz units: 8700 = 87.00 MHz, 10800 = 108.00 MHz                                  |
| `usn`       | `unsigned int`  | signal quality                                       |
| `wam`       | `unsigned char` | signal quality                                       |
| `offset`    | `unsigned int`  | signal quality                                       |
| `level`     | `unsigned int`  | received signal strength (RSSI)                      |
| `modulation`| `unsigned char` | (AM) modulation                                      |
| `stereo`    | `unsigned char` | stereo flag                                          |

The layout uses natural alignment (28 bytes) on both x86-64 and aarch64; this is
guaranteed by a `static_assert` in `HSRadioSAF775XBackend.cpp`. **If the target
driver uses a different packing, the static_assert will fail to compile** — fix
the struct to match the driver before building.

## Safety notes

- ALSA handle/id/info/value objects use RAII-managed heap allocations; no
  ALSA pointer escapes its function scope.
- Writes validate the element type before calling ALSA; the mute control's
  value range is checked for INTEGER controls.
- The tune struct layout is verified at compile time (see above).
- Frequency-region validation (FM/AM band limits and step) belongs to the
  manager/configuration layer, not this low-level ALSA adapter.
- The backend does not implement seek loops, signal qualification, chip
  initialization sequences or audio-source routing.

## Not implemented in this phase

- SAF775X register access (`regControl`): the control exists but no API is
  exposed by `IRadioHardware` yet.
- Seek loop and signal qualification policy (belongs in manager/scheduler).
- D-Bus and RadioClient.
- Station persistence.
- Device-loss recovery/reopen policy.

## Standalone hardware test executable

The test executable is built separately from the static-library project:

```sh
mkdir -p build/hardware-test && cd build/hardware-test
/path/to/Qt5.12/bin/qmake ../../HSRadioHardwareTest.pro
make -j$(nproc)
```

Run `./hsradio_hardware_test --help` for all options. The required controls
must be taken from the target's actual `amixer -D default controls` output:

```sh
./hsradio_hardware_test \
  --device default \
  --tune-control "Radiocontrol" \
  --mute-control "Mute_radio"
```

Optional: `--reg-control "Radioregvalue"`, `--mute-off-value 0`,
`--mute-on-value 1`. Add `--search` together with `--set-frequency` to tune
in Search (stay-muted) mode, used while scanning for stations.

By default the program only initializes the backend and reads current state and
signal information; it does not change band, frequency or mute. Write operations
are opt-in:

```sh
# These examples perform real hardware writes:
./hsradio_hardware_test ... --set-band FM --set-frequency 99500 --set-mute off
```

```sh
# Scan in muted Search mode (station seek) instead of normal Preset listening:
./hsradio_hardware_test ... --set-band FM --set-frequency 99500 --search
```

The test tool also ships an automatic station-search business flow (`--seek`).
It scans the band from `--seek-from` to `--seek-to` in `--seek-step` kHz
increments, tunes each candidate in Search (stay-muted) mode, and accepts a
candidate as a station when its signal meets the quality thresholds. The
thresholds default to FM-tuned values and can be overridden with
`--seek-min-rssi`, `--seek-max-usn`, `--seek-max-wam`, `--seek-max-offset`.

```sh
# Automatic station search (FM 87.5-108.0 MHz, 100 kHz step):
./hsradio_hardware_test ... --seek --seek-band FM \
  --seek-from 87500 --seek-to 108000 --seek-step 100
```

Use write options only after confirming the control mapping and safe values on
the target device. `--set-frequency` accepts kHz. The test application invokes
the backend synchronously on its own main thread for simplicity; the production
service must keep the backend on its dedicated hardware thread as described
above.
