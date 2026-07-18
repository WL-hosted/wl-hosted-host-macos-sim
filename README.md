# WL-hosted Host macOS Simulator

POSIX adapter and scenario runner for WL-hosted Host Core. It speaks the
versioned Simulator IPC framing from the nested Protocol dependency and keeps
runtime/fault sideband records outside the standard WL-hosted wire protocol.

The simulator enables the POSIX OSAL adapter from the Common dependency nested
under Host Core. It uses pthread condition variables, bounded queues, and
monotonic timers. Core work and TX run on separate tasks; the scenario runner
does not call a Core poll API.
Transport start/stop also complete asynchronously on the TX task, scenario
state changes wake condition variables, and Core waits until the nearest real
RPC/heartbeat deadline instead of using a periodic tick.
`wlh_posix_osal` CTest is the POSIX OSAL consistency gate.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure

./build/wlh-host-macos-sim --ipc connect:/path/to/host.sock --scenario connect
```

The executable role is fixed to `HOST_SIM`; direct Host/Coproc connections
automatically disable sideband traffic.

## Real USB Coprocessor transport

Besides the virtual IPC transport, the simulator can attach to a real USB
Coprocessor (profile `espressif.esp32s3.coreboard.usb-wifi`) over a
vendor-specific bulk interface. USB mode speaks raw WL-hosted frames without
the simulator IPC record layer and disables sideband reporting. It requires
libusb-1.0 (`brew install libusb` on macOS).

```sh
./build/wlh-host-macos-sim --usb 303A:8201 --scenario connect \
    --ssid MyAp --credential MyPassphrase
```

- Bulk OUT carries Host to Coprocessor frames, bulk IN the reverse; frame
  boundaries follow the 24-byte wire header, not USB packet boundaries.
- A bus disconnect raises `wlh_host_transport_lost`; the Core then restarts
  the transport (stop, bounded reopen wait, start) and renegotiates Hello.
- The Ethernet echo step is skipped in USB mode because a real device
  forwards frames to the AP instead of echoing them.
- `--scenario services` exercises the Device Information and User
  Passthrough services against the real firmware.