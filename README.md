# WL-hosted Host macOS Simulator

POSIX adapter and scenario runner for WL-hosted Host Core. It speaks the
versioned Simulator IPC framing from the nested Protocol dependency and keeps
runtime/fault sideband records outside the standard WL-hosted wire protocol.

The simulator implements the complete Core OSAL contract with pthread,
condition variables, bounded queues, and monotonic timers. Core work and TX
run on separate tasks; the scenario runner does not call a Core poll API.
`wlh_posix_osal` CTest is the POSIX OSAL consistency gate.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure

./build/wlh-host-macos-sim --ipc connect:/path/to/host.sock --scenario connect
```

The executable role is fixed to `HOST_SIM`; direct Host/Coproc connections
automatically disable sideband traffic.
