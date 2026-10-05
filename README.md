# SurfaceBatteryDiag

A small Windows utility for diagnosing implausibly low battery discharge-rate telemetry on Microsoft Surface devices.

It compares multiple layers of the Windows battery stack:

- direct `IOCTL_BATTERY_QUERY_STATUS` values from the battery-class device;
- `BATTERY_INFORMATION.Capabilities`, including `BATTERY_CAPACITY_RELATIVE`;
- Windows battery-driver estimated runtime;
- `root\\wmi:BatteryStatus`;
- battery PnP IDs, services, interfaces and driver stack via `pnputil`;
- a 10-second direct IOCTL trace.

For non-relative batteries, Windows documents `BATTERY_STATUS.Rate` as mW and capacity as mWh. The tool therefore prints the normal Windows interpretation and, as a diagnostic counterfactual, what the same raw numeric rate would imply if it were actually mA at the reported battery voltage.

That second number is a hypothesis test only, not a claim that Surface firmware definitely reports mA.

## Run

Download `SurfaceBatteryDiag.exe` from GitHub Actions or Releases, unplug the charger, and run it. It writes a timestamped `SurfaceBatteryDiag-YYYYMMDD-HHMMSS.txt` report next to the EXE.

No installation is required. Administrator privileges should normally not be required.

```powershell
.\\SurfaceBatteryDiag.exe --no-pause
```

## Privacy

The report can include machine/product identifiers exposed by Windows and PnP device paths. Review it before posting publicly.

## Build

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The GitHub Actions workflow builds a static-runtime Windows x64 EXE on each push and uploads it as an artifact. A `v*` tag also creates a GitHub Release.

## License

MIT.
