# HSRadio - Hardware Layer (Phase 1)

Qt 5.12 / C++17 ALSA Mixer Control backend for the HSRadio service.

## Compatibility

- Qt version: Qt 5.12.x (Core module).
- Language standard: C++17; the target compiler must support C++17.
- Build system: qmake (`HSRadioHardware.pro`).
- No Qt 6-only classes or APIs are used by this hardware layer.
- ALSA development headers and library are required on the target sysroot.

## Included

- `HSRadioTypes`: common band, result, signal and hardware-state types.
- `IRadioHardware`: chip-independent synchronous hardware contract.
- `AlsaRadioBackend`: ALSA control implementation using `snd_ctl_elem_read()` and `snd_ctl_elem_write()`.

## Build with qmake

Use the qmake executable from the Qt 5.12 kit configured for the target platform. For example:

```sh
mkdir build && cd build
/path/to/Qt5.12/bin/qmake ../HSRadioHardware.pro
make -j$(nproc)
```

This project builds a static library named `libhsradio_hardware.a` on Linux.

For an i.MX9 cross-build, invoke the qmake executable and compiler/toolchain configured for the i.MX9 Qt 5.12 kit. The target sysroot must provide ALSA headers (`alsa/asoundlib.h`) and `libasound`. If ALSA is installed in a non-standard sysroot location, configure its include and library search paths in the kit/toolchain rather than hard-coding host paths in this project file.

## Thread ownership

`AlsaRadioBackend` is a synchronous QObject. Construct it without a parent, move it to the dedicated hardware `QThread`, and execute **all** its methods in that thread. Do not call the methods directly from the service main thread. The backend checks its owning thread and returns an error for cross-thread calls.

The object must also be shut down and destroyed in its owning thread. A queued invocation is the intended integration mechanism; the service-level worker/scheduler wrapper will be added in the next layer.

## ALSA control mapping

The actual mixer control names and ranges for the target i.MX9 / SAF775D image have not been supplied. They are deliberately not hard-coded. Populate `AlsaRadioConfig` in platform integration code after inspecting the target device:

```sh
amixer -D default controls
amixer -D default contents
```

Required controls:

- `band`: integer/boolean control; values configured by `fmBandValue` and `amBandValue`.
- `frequencyKHz`: integer or integer64 control. The value passed to the backend is in kHz.
- `mute`: integer/boolean control; values configured by `muteOnValue` and `muteOffValue`.

Optional read-only controls:

- `rssi`, `usn`, `wam`, `offset`, `stereo`.

Each control is addressed by ALSA Mixer interface + element name + index. Supported ALSA element types are INTEGER, INTEGER64 and BOOLEAN. ENUMERATED controls are intentionally not guessed or silently mapped. Optional signal controls that are not present or have unsupported types are marked unavailable; they do not prevent basic radio initialization.

Example mapping (names below are placeholders, **not** SAF775D control names):

```cpp
HSRadio::AlsaRadioConfig config;
config.device = QStringLiteral("default");
config.band.name = QStringLiteral("<actual-band-control>");
config.frequencyKHz.name = QStringLiteral("<actual-frequency-control>");
config.mute.name = QStringLiteral("<actual-mute-control>");
config.rssi.name = QStringLiteral("<actual-rssi-control>"); // optional
config.fmBandValue = 0;
config.amBandValue = 1;
config.muteOffValue = 0;
config.muteOnValue = 1;
```

## Safety notes

- ALSA element IDs, info and value objects use RAII-managed heap allocations. No ALSA stack-allocation pointer escapes its function scope.
- Writes validate the element type and its supported value range before calling ALSA.
- Frequency-region validation (FM/AM band limits and step) belongs to the manager/configuration layer, not this low-level ALSA adapter.
- The backend does not implement seek loops, signal qualification, chip initialization sequences or audio-source routing.

## Not implemented in this phase

- SAF775D-specific control mapping and chip initialization sequence.
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

Run `./hsradio_hardware_test --help` for all options. The three required control names
must be taken from the target's actual `amixer -D default controls` output:

```sh
./hsradio_hardware_test \
  --device default \
  --band-control "<actual-band-control>" \
  --frequency-control "<actual-frequency-control>" \
  --mute-control "<actual-mute-control>"
```

Optional signal controls can be added with `--rssi-control`, `--usn-control`,
`--wam-control`, `--offset-control`, and `--stereo-control`. Their names are
board/driver-specific and must not be guessed.

By default, the program only initializes the backend and reads current state and
signal information. It does not change band, frequency or mute. Write operations
are opt-in:

```sh
# These examples perform real hardware writes:
./hsradio_hardware_test ... --set-band FM --set-frequency 99500 --set-mute off
```

Use write options only after confirming the control mapping and safe values on the
target device. `--set-frequency` accepts kHz. The test application invokes the
backend synchronously on its own main thread for simplicity; the production service
must keep the backend on its dedicated hardware thread as described above.
