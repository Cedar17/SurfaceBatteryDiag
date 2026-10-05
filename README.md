# SurfaceBatteryDiag

SurfaceBatteryDiag is a small, read-only Windows diagnostic utility for investigating implausible battery discharge-rate telemetry on Microsoft Surface devices.

The project was created after a Surface Go 3 repeatedly reported whole-system battery power in the range of roughly 0.3-0.8 W while the CPU package alone could already be close to 0.9 W. Multiple monitoring applications showed the same symptom.

Search keywords: **Surface Go 3 battery power wrong**, **MSHW0146**, **SurfaceBattery**, **SurfaceBattery.sys**, **BatteryStatus DischargeRate**, **IOCTL_BATTERY_QUERY_STATUS**, **HWiNFO battery charge rate**, **TrafficMonitor battery power**, **ACPI _BST _BIX**, **mA vs mW battery telemetry**.

## What the tool collects

SurfaceBatteryDiag compares several layers of the Windows battery path:

- direct battery-class queries using `IOCTL_BATTERY_QUERY_TAG`, `IOCTL_BATTERY_QUERY_INFORMATION`, and `IOCTL_BATTERY_QUERY_STATUS`;
- `BATTERY_INFORMATION.Capabilities`, including `BATTERY_CAPACITY_RELATIVE`;
- raw `BATTERY_STATUS.Rate`, `Voltage`, `Capacity`, and power-state flags;
- the battery driver's estimated remaining time;
- `root\\wmi:BatteryStatus` values;
- battery PnP identity, service, INF, driver version, and hardware ID through SetupAPI and the registry;
- a 10-second direct battery-status trace;
- experimental user-mode attempts to evaluate ACPI `_BST`, `_BIX`, and `_BIF` on the battery and Surface-specific device interfaces.

The program does not install a driver, modify firmware, change power settings, or upload data.

## Observations from the original Surface Go 3 case

The original machine was a Microsoft Surface Go 3 using the Surface-specific battery device `ACPI\\MSHW0146` and the `SurfaceBattery` service. The installed Surface battery driver reported version `2.81.139.0`.

The important observations were:

1. `BATTERY_CAPACITY_RELATIVE` was **not** set. Windows therefore treats battery capacity as mWh and `BATTERY_STATUS.Rate` as mW.
2. WMI and direct battery-class IOCTLs reported the same order of magnitude. Example observations included WMI `DischargeRate=424` and a simultaneous direct `Rate=-424`.
3. Battery voltage was approximately 8.2-8.4 V.
4. Direct 10-second traces repeatedly produced raw discharge-rate values in the hundreds. Interpreted according to the Windows contract, this means only about 0.35-0.82 W for the entire machine.
5. Interpreting the same numeric values as mA and multiplying by the reported battery voltage yields roughly 2.9-6.8 W, which is physically plausible for a lightly loaded Surface Go 3.
6. Because WMI, HWiNFO, TrafficMonitor, and direct battery-class IOCTLs agree on the same scale, the monitoring applications are very unlikely to be the source of the error.
7. User-mode handles to the relevant Surface interfaces can be opened, but direct `IOCTL_ACPI_EVAL_METHOD` and V2 attempts for `_BST`, `_BIX`, and `_BIF` return Win32 error 50, `ERROR_NOT_SUPPORTED`, on the relevant battery/Surface interfaces. Some unrelated Surface interfaces may instead return `ERROR_ACCESS_DENIED`.

## Working conclusion

The incorrect discharge-rate value already exists below normal monitoring applications, at or below the Windows battery-class boundary.

The observed scaling is strongly consistent with a unit-path bug in which a current-like value in mA reaches a path that Windows interprets as power in mW without the expected voltage conversion. For example, a raw value near 400 at about 8.3 V is nonsensical as about 0.4 W of whole-system power, but corresponds to about 3.3 W if interpreted as 400 mA.

This is a **hypothesis supported by the observed scaling**, not proof of the exact implementation bug.

The user-mode tests cannot determine whether the incorrect value originates in the Surface firmware/EC/ACPI implementation or is introduced by the `SurfaceBattery` driver. The ACPI evaluation IOCTLs are documented for driver-to-ACPI-PDO use, and the Surface device stack does not expose them to this ordinary user-mode caller. Distinguishing those two locations would require kernel-mode instrumentation or another trusted way to observe the raw ACPI method result before the Surface battery driver translates it.

## Scope and limitations

The documented observations come from one Surface Go 3 case. They should not be assumed to apply to every Surface model, firmware version, or Windows release.

A matching symptom on another machine is especially interesting if all of the following are true:

- `Capacity relative: NO`;
- direct `BATTERY_STATUS.Rate` is implausibly small for whole-system power;
- WMI reports a matching discharge-rate scale;
- the same raw numeric value becomes physically plausible when treated as mA and multiplied by battery voltage.

That pattern would strengthen the unit-mismatch hypothesis across devices.

## Run

Download `SurfaceBatteryDiag.exe` from the latest GitHub Release.

For the cleanest sample:

1. Unplug the charger.
2. Let the machine reach a stable light-load state.
3. Run `SurfaceBatteryDiag.exe`.
4. Wait for the 10-second sampling section to finish.
5. Open the generated `SurfaceBatteryDiag-YYYYMMDD-HHMMSS.txt` file next to the executable.

To suppress the final pause:

```powershell
.\\SurfaceBatteryDiag.exe --no-pause
```

The report intentionally uses English field names and locale-independent output so results are easy to search and compare across Windows display languages.

## Privacy

The report may contain the computer product identifier, battery/device instance paths, and other hardware identifiers exposed by Windows. Review the report before posting it publicly.

## Build

Requirements: Visual Studio Build Tools or Visual Studio with Desktop C++ support and CMake.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable is produced at:

```text
build\\Release\\SurfaceBatteryDiag.exe
```

GitHub Actions builds the x64 standalone executable with the static MSVC runtime.

## Repository layout

```text
src/main.cpp                  Diagnostic utility
CMakeLists.txt                CMake build
.github/workflows/build.yml   CI and release build
LICENSE                       MIT license
```

## License

MIT.
