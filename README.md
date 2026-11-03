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

### Scheduler smoke test (host only)

`HSRadioManagerTest.pro` builds a console program that drives
`HSRadioOperationScheduler` with `MockRadioHardware`, so it runs on
any developer machine without ALSA headers or the radio chip. It covers the
serial FIFO order of atomic commands, FullBand and NextStation seeks, seek
cancellation, the forced mute/restore around a seek, and hardware fault
propagation.

```sh
mkdir build-test && cd build-test
/path/to/Qt5.12/bin/qmake ../HSRadioManagerTest.pro
make -j$(nproc)
./hsradio_manager_test
```

The program prints a per-case trace, ends with `全部通过：N 项检查`
(all checks passed) and exits with code 0; on failure it prints the failing
line numbers and exits with code 1.

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
mkdir build-test && cd build-test
/path/to/Qt5.12/bin/qmake ../HSRadioHardwareTest.pro
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
