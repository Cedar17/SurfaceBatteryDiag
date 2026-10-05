#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <devguid.h>
#include <batclass.h>

#ifndef FILE_DEVICE_BATTERY
#define FILE_DEVICE_BATTERY 0x00000029
#endif

// Minimal public ACPI IOCTL definitions copied from the Windows WDK contract.
// We keep them local so the project builds with the ordinary Windows SDK and
// does not require the WDK-only acpiioct.h header.
#ifndef FILE_DEVICE_ACPI
#define FILE_DEVICE_ACPI 0x00000032
#endif
#ifndef IOCTL_ACPI_EVAL_METHOD
#define IOCTL_ACPI_EVAL_METHOD CTL_CODE(FILE_DEVICE_ACPI, 0x001, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif
#ifndef IOCTL_ACPI_EVAL_METHOD_V2
#define IOCTL_ACPI_EVAL_METHOD_V2 CTL_CODE(FILE_DEVICE_ACPI, 0x00f, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif

static constexpr ULONG acpi_sig(char a, char b, char c, char d) {
    return (static_cast<ULONG>(static_cast<unsigned char>(a)) << 24) |
           (static_cast<ULONG>(static_cast<unsigned char>(b)) << 16) |
           (static_cast<ULONG>(static_cast<unsigned char>(c)) << 8)  |
            static_cast<ULONG>(static_cast<unsigned char>(d));
}

static constexpr ULONG ACPI_EVAL_INPUT_BUFFER_SIGNATURE_V1_LOCAL = acpi_sig('B','i','e','A');
static constexpr ULONG ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1_LOCAL = acpi_sig('B','o','e','A');
static constexpr ULONG ACPI_EVAL_INPUT_BUFFER_SIGNATURE_V2_LOCAL = acpi_sig('K','i','e','A');
static constexpr ULONG ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V2_LOCAL = acpi_sig('K','o','e','A');

static constexpr USHORT ACPI_ARG_INTEGER = 0x0;
static constexpr USHORT ACPI_ARG_STRING  = 0x1;
static constexpr USHORT ACPI_ARG_BUFFER  = 0x2;
static constexpr USHORT ACPI_ARG_PACKAGE = 0x3;
static constexpr USHORT ACPI_ARG_PACKAGE_EX = 0x4;

struct AcpiEvalInputV1Local {
    ULONG Signature;
    union {
        UCHAR MethodName[4];
        ULONG MethodNameAsUlong;
    };
};

struct AcpiMethodArgumentV1Local {
    USHORT Type;
    USHORT DataLength;
    union {
        ULONG Argument;
        UCHAR Data[1];
    };
};

struct AcpiEvalOutputV1Local {
    ULONG Signature;
    ULONG Length;
    ULONG Count;
    AcpiMethodArgumentV1Local Argument[1];
};

static constexpr GUID GUID_SURFACE_BATTERY_IFACE_A_LOCAL =
    {0xe849804e,0xc719,0x43d8,{0xac,0x88,0x96,0xb8,0x94,0xc1,0x91,0xe2}};
static constexpr GUID GUID_SURFACE_BATTERY_IFACE_B_LOCAL =
    {0xaaa1e791,0xbc4f,0x4e5d,{0xaf,0x22,0x98,0x04,0x12,0x20,0xd2,0xad}};

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <array>

#pragma comment(lib, "setupapi.lib")

static std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);
    std::string out(n,'\0');
    WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),out.data(),n,nullptr,nullptr);
    return out;
}

static std::wstring exe_dir() {
    wchar_t b[32768];
    DWORD n=GetModuleFileNameW(nullptr,b,32768);
    return std::filesystem::path(std::wstring(b,n)).parent_path().wstring();
}

static std::string timestamp() {
    SYSTEMTIME st{}; GetLocalTime(&st);
    char b[32];
    sprintf_s(b,"%04u%02u%02u-%02u%02u%02u",st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
    return b;
}

static std::string run_capture(const std::wstring& cmdline) {
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    HANDLE rd=nullptr,wr=nullptr;
    if(!CreatePipe(&rd,&wr,&sa,0)) return "[CreatePipe failed]\n";
    SetHandleInformation(rd,HANDLE_FLAG_INHERIT,0);

    STARTUPINFOW si{}; si.cb=sizeof(si);
    si.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
    si.wShowWindow=SW_HIDE; si.hStdOutput=wr; si.hStdError=wr; si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> c(cmdline.begin(),cmdline.end()); c.push_back(L'\0');
    BOOL ok=CreateProcessW(nullptr,c.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi);
    CloseHandle(wr);
    if(!ok){ CloseHandle(rd); return "[CreateProcess failed]\n"; }

    std::string out; char buf[4096]; DWORD got=0;
    while(ReadFile(rd,buf,sizeof(buf),&got,nullptr) && got) out.append(buf,got);
    WaitForSingleObject(pi.hProcess,INFINITE);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(rd);
    return out;
}

static std::string power_state(ULONG ps){
    std::string s;
    auto add=[&](const char* x){ if(!s.empty()) s+=" | "; s+=x; };
    if(ps&BATTERY_POWER_ON_LINE) add("POWER_ON_LINE");
    if(ps&BATTERY_DISCHARGING) add("DISCHARGING");
    if(ps&BATTERY_CHARGING) add("CHARGING");
    if(ps&BATTERY_CRITICAL) add("CRITICAL");
    return s.empty() ? "0" : s;
}


static size_t acpi_arg_span_v1(const AcpiMethodArgumentV1Local* a) {
    const size_t data = std::max<size_t>(sizeof(ULONG), a->DataLength);
    return offsetof(AcpiMethodArgumentV1Local, Data) + data;
}

static void dump_acpi_arg_v1(std::ostream& out,
                             const AcpiMethodArgumentV1Local* a,
                             size_t available,
                             int depth,
                             std::vector<ULONG>& flattenedIntegers) {
    if (available < offsetof(AcpiMethodArgumentV1Local, Data) + sizeof(ULONG)) {
        out << std::string(depth * 2, ' ') << "<truncated argument>\n";
        return;
    }

    const std::string indent(depth * 2, ' ');
    out << indent << "Type=" << a->Type << " DataLength=" << a->DataLength;

    if (a->Type == ACPI_ARG_INTEGER) {
        out << " INTEGER=" << a->Argument << " (0x"
            << std::hex << a->Argument << std::dec << ")\n";
        flattenedIntegers.push_back(a->Argument);
        return;
    }

    if (a->Type == ACPI_ARG_STRING) {
        const size_t n = std::min<size_t>(a->DataLength, available - offsetof(AcpiMethodArgumentV1Local, Data));
        std::string s(reinterpret_cast<const char*>(a->Data),
                      reinterpret_cast<const char*>(a->Data) + n);
        while (!s.empty() && s.back() == '\0') s.pop_back();
        out << " STRING=\"" << s << "\"\n";
        return;
    }

    if (a->Type == ACPI_ARG_BUFFER) {
        const size_t n = std::min<size_t>(a->DataLength, available - offsetof(AcpiMethodArgumentV1Local, Data));
        out << " BUFFER[" << n << "]=";
        for (size_t i = 0; i < std::min<size_t>(n, 32); ++i)
            out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(a->Data[i]);
        out << std::dec << std::setfill(' ');
        if (n > 32) out << "...";
        out << "\n";
        return;
    }

    if (a->Type == ACPI_ARG_PACKAGE || a->Type == ACPI_ARG_PACKAGE_EX) {
        const size_t n = std::min<size_t>(a->DataLength, available - offsetof(AcpiMethodArgumentV1Local, Data));
        out << " PACKAGE bytes=" << n << "\n";
        size_t off = 0;
        while (off + offsetof(AcpiMethodArgumentV1Local, Data) + sizeof(ULONG) <= n) {
            const auto* child = reinterpret_cast<const AcpiMethodArgumentV1Local*>(a->Data + off);
            const size_t span = acpi_arg_span_v1(child);
            if (span == 0 || off + span > n) {
                out << indent << "  <package tail/truncated at +" << off << ">\n";
                break;
            }
            dump_acpi_arg_v1(out, child, n - off, depth + 1, flattenedIntegers);
            off += span;
        }
        return;
    }

    out << " UNKNOWN\n";
}

static void interpret_bst_flat(std::ostream& out, const std::vector<ULONG>& v) {
    if (v.size() < 4) {
        out << "  _BST flattened integer count=" << v.size() << " (need >=4)\n";
        return;
    }
    out << "  _BST decoded by ACPI spec:\n";
    out << "    Battery State      = " << v[0] << " (0x" << std::hex << v[0] << std::dec << ")\n";
    out << "    Present Rate       = " << v[1] << "  <-- CRITICAL RAW VALUE\n";
    out << "    Remaining Capacity = " << v[2] << "\n";
    out << "    Present Voltage    = " << v[3] << "\n";
    if (v[3] != 0 && v[3] != 0xffffffffUL && v[1] != 0xffffffffUL) {
        const double altW = static_cast<double>(v[1]) * static_cast<double>(v[3]) / 1000000.0;
        out << std::fixed << std::setprecision(4)
            << "    If Present Rate were mA at Present Voltage: " << altW << " W\n";
        out.unsetf(std::ios::floatfield);
    }
}

static std::string error_text(DWORD e) {
    LPWSTR msg = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                             reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::string s;
    if (n && msg) {
        s = utf8(std::wstring(msg, n));
        LocalFree(msg);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    }
    if (s.empty()) s = "unknown";
    return s;
}

static std::vector<std::wstring> enumerate_interface_paths(const GUID& guid) {
    std::vector<std::wstring> paths;
    HDEVINFO h = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (h == INVALID_HANDLE_VALUE) return paths;

    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA ifd{};
        ifd.cbSize = sizeof(ifd);
        if (!SetupDiEnumDeviceInterfaces(h, nullptr, &guid, i, &ifd)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            continue;
        }
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(h, &ifd, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> buf(need);
        auto detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(h, &ifd, detail, need, nullptr, nullptr))
            paths.emplace_back(detail->DevicePath);
    }
    SetupDiDestroyDeviceInfoList(h);
    return paths;
}

static HANDLE open_probe_handle(const std::wstring& path, DWORD& lastError) {
    const DWORD shares = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, shares,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { lastError = ERROR_SUCCESS; return h; }
    lastError = GetLastError();

    h = CreateFileW(path.c_str(), GENERIC_READ, shares,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { lastError = ERROR_SUCCESS; return h; }
    lastError = GetLastError();
    return INVALID_HANDLE_VALUE;
}

static bool try_acpi_eval_v1(HANDLE h, const char method[5],
                             std::ostream& out, DWORD& lastError) {
    AcpiEvalInputV1Local in{};
    in.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE_V1_LOCAL;
    std::memcpy(in.MethodName, method, 4);

    std::array<BYTE, 8192> storage{};
    DWORD returned = 0;
    SetLastError(ERROR_SUCCESS);
    BOOL ok = DeviceIoControl(h, IOCTL_ACPI_EVAL_METHOD,
                              &in, sizeof(in),
                              storage.data(), static_cast<DWORD>(storage.size()),
                              &returned, nullptr);
    if (!ok) {
        lastError = GetLastError();
        out << "  legacy " << method << ": FAILED Win32=" << lastError
            << " (" << error_text(lastError) << ")\n";
        return false;
    }

    out << "  legacy " << method << ": SUCCESS bytes=" << returned << "\n";
    if (returned < offsetof(AcpiEvalOutputV1Local, Argument)) {
        out << "    output too short\n";
        return true;
    }

    const auto* o = reinterpret_cast<const AcpiEvalOutputV1Local*>(storage.data());
    out << "    Signature=0x" << std::hex << o->Signature << std::dec
        << " Length=" << o->Length << " Count=" << o->Count << "\n";
    if (o->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1_LOCAL) {
        out << "    WARNING unexpected V1 output signature (expected 0x"
            << std::hex << ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1_LOCAL << std::dec << ")\n";
    }

    size_t off = offsetof(AcpiEvalOutputV1Local, Argument);
    std::vector<ULONG> flattened;
    for (ULONG i = 0; i < o->Count && off < returned; ++i) {
        if (off + offsetof(AcpiMethodArgumentV1Local, Data) + sizeof(ULONG) > returned) break;
        const auto* a = reinterpret_cast<const AcpiMethodArgumentV1Local*>(storage.data() + off);
        const size_t span = acpi_arg_span_v1(a);
        if (span == 0 || off + span > returned) {
            out << "    argument " << i << " truncated, span=" << span << "\n";
            break;
        }
        out << "    Arg[" << i << "]:\n";
        dump_acpi_arg_v1(out, a, returned - off, 3, flattened);
        off += span;
    }
    if (std::string(method, 4) == "_BST") interpret_bst_flat(out, flattened);
    return true;
}

static void try_acpi_eval_v2_status(HANDLE h, const char method[5],
                                    std::ostream& out) {
    AcpiEvalInputV1Local in{};
    in.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE_V2_LOCAL;
    std::memcpy(in.MethodName, method, 4);

    std::array<BYTE, 8192> storage{};
    DWORD returned = 0;
    SetLastError(ERROR_SUCCESS);
    BOOL ok = DeviceIoControl(h, IOCTL_ACPI_EVAL_METHOD_V2,
                              &in, sizeof(in),
                              storage.data(), static_cast<DWORD>(storage.size()),
                              &returned, nullptr);
    if (!ok) {
        DWORD e = GetLastError();
        out << "  v2     " << method << ": FAILED Win32=" << e
            << " (" << error_text(e) << ")\n";
        return;
    }

    out << "  v2     " << method << ": SUCCESS bytes=" << returned;
    if (returned >= sizeof(ULONG)) {
        ULONG sig = *reinterpret_cast<const ULONG*>(storage.data());
        out << " signature=0x" << std::hex << sig << std::dec;
        if (sig == ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V2_LOCAL) out << " (V2 expected)";
    }
    out << "\n";
    out << "    V2 success is recorded, but only V1 is decoded to avoid depending on WDK V2 layout.\n";
}

static void probe_acpi_on_path(std::ostream& out,
                               const std::wstring& path,
                               const char* label) {
    out << "\n[" << label << "]\n";
    out << "Path: " << utf8(path) << "\n";
    DWORD openError = 0;
    HANDLE h = open_probe_handle(path, openError);
    if (h == INVALID_HANDLE_VALUE) {
        out << "CreateFile: FAILED Win32=" << openError
            << " (" << error_text(openError) << ")\n";
        return;
    }
    out << "CreateFile: SUCCESS\n";

    DWORD e = 0;
    try_acpi_eval_v1(h, "_BST", out, e);
    try_acpi_eval_v1(h, "_BIX", out, e);
    try_acpi_eval_v1(h, "_BIF", out, e);
    try_acpi_eval_v2_status(h, "_BST", out);
    try_acpi_eval_v2_status(h, "_BIX", out);
    CloseHandle(h);
}

static void run_direct_acpi_probe(std::ostream& out) {
    out << "\n============================================================\n";
    out << "DIRECT ACPI METHOD PROBE (experimental user-mode pass-through)\n";
    out << "============================================================\n";
    out << "These IOCTLs are officially documented for drivers sending requests to an ACPI PDO.\n";
    out << "A user-mode DeviceIoControl may be blocked by SurfaceBattery or the device stack.\n";
    out << "Failure is therefore diagnostic and is recorded verbatim; it does not mean _BST/_BIX are absent.\n";

    struct Item { GUID guid; const char* name; };
    const Item items[] = {
        { GUID_DEVCLASS_BATTERY, "Battery class interface" },
        { GUID_SURFACE_BATTERY_IFACE_A_LOCAL, "SurfaceBattery custom interface e849804e" },
        { GUID_SURFACE_BATTERY_IFACE_B_LOCAL, "SurfaceBattery custom interface aaa1e791" }
    };

    size_t total = 0;
    for (const auto& item : items) {
        auto paths = enumerate_interface_paths(item.guid);
        out << "\nInterface set: " << item.name << " paths=" << paths.size() << "\n";
        for (const auto& p : paths) {
            // Focus on the actual Surface battery where possible; retain all paths for diagnostics.
            probe_acpi_on_path(out, p, item.name);
            ++total;
        }
    }
    out << "\nTotal interface paths probed: " << total << "\n";
}


int wmain(int argc, wchar_t** argv) {
    bool nopause=false;
    for(int i=1;i<argc;i++) if(std::wstring(argv[i])==L"--no-pause") nopause=true;

    std::ostringstream out;
    out<<"SurfaceBatteryDiag v0.2.0\n";
    out<<"Purpose: inspect raw Windows battery-class telemetry and compare documented mW interpretation\n";
    out<<"with the diagnostic counterfactual that the same numeric rate might actually represent mA.\n\n";

    out<<"=== SYSTEM ===\n";
    out<<run_capture(
        L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
        L"\"$ErrorActionPreference='SilentlyContinue'; "
        L"Get-CimInstance Win32_ComputerSystemProduct | Select Vendor,Name,Version,IdentifyingNumber | Format-List; "
        L"Get-CimInstance Win32_OperatingSystem | Select Caption,Version,BuildNumber,OSArchitecture | Format-List\"");

    out<<"\n=== WMI root\\wmi BatteryStatus ===\n";
    out<<run_capture(
        L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
        L"\"Get-CimInstance -Namespace root\\wmi -ClassName BatteryStatus | "
        L"Select InstanceName,PowerOnline,Charging,Discharging,Voltage,RemainingCapacity,ChargeRate,DischargeRate,Critical | Format-List\"");

    out<<"\n=== PNP BATTERY STACK ===\n";
    out<<run_capture(L"cmd.exe /d /c pnputil /enum-devices /class Battery /deviceids /stack /drivers /services /interfaces");

    HDEVINFO hdev=SetupDiGetClassDevsW(&GUID_DEVCLASS_BATTERY,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(hdev==INVALID_HANDLE_VALUE){
        out<<"\nSetupDiGetClassDevs failed.\n";
    } else {
        for(DWORD idx=0;;++idx){
            SP_DEVICE_INTERFACE_DATA ifd{}; ifd.cbSize=sizeof(ifd);
            if(!SetupDiEnumDeviceInterfaces(hdev,nullptr,&GUID_DEVCLASS_BATTERY,idx,&ifd)){
                if(GetLastError()==ERROR_NO_MORE_ITEMS) break;
                continue;
            }

            DWORD need=0;
            SetupDiGetDeviceInterfaceDetailW(hdev,&ifd,nullptr,0,&need,nullptr);
            if(!need) continue;
            std::vector<BYTE> mem(need);
            auto detail=reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(mem.data());
            detail->cbSize=sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if(!SetupDiGetDeviceInterfaceDetailW(hdev,&ifd,detail,need,nullptr,nullptr)) continue;

            out<<"\n=== BATTERY "<<idx<<" ===\n";
            out<<"DevicePath: "<<utf8(detail->DevicePath)<<"\n";

            HANDLE h=CreateFileW(detail->DevicePath,GENERIC_READ|GENERIC_WRITE,
                FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(h==INVALID_HANDLE_VALUE){
                h=CreateFileW(detail->DevicePath,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
                    nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            }
            if(h==INVALID_HANDLE_VALUE){ out<<"CreateFile failed: "<<GetLastError()<<"\n"; continue; }

            ULONG tag=0; DWORD wait=0,ret=0;
            if(!DeviceIoControl(h,IOCTL_BATTERY_QUERY_TAG,&wait,sizeof(wait),&tag,sizeof(tag),&ret,nullptr) || tag==0){
                out<<"QUERY_TAG failed: "<<GetLastError()<<"\n"; CloseHandle(h); continue;
            }

            BATTERY_QUERY_INFORMATION qi{}; qi.BatteryTag=tag; qi.InformationLevel=BatteryInformation;
            BATTERY_INFORMATION bi{};
            if(!DeviceIoControl(h,IOCTL_BATTERY_QUERY_INFORMATION,&qi,sizeof(qi),&bi,sizeof(bi),&ret,nullptr)){
                out<<"BatteryInformation failed: "<<GetLastError()<<"\n"; CloseHandle(h); continue;
            }

            bool relative=(bi.Capabilities&BATTERY_CAPACITY_RELATIVE)!=0;
            out<<"Capabilities: 0x"<<std::hex<<bi.Capabilities<<std::dec<<"\n";
            out<<"Capacity relative: "<<(relative?"YES":"NO")<<"\n";
            out<<"Designed capacity: "<<bi.DesignedCapacity<<(relative?"\n":" mWh\n");
            out<<"Full-charge capacity: "<<bi.FullChargedCapacity<<(relative?"\n":" mWh\n");
            out<<"Cycle count: "<<bi.CycleCount<<"\n";

            auto sample=[&](BATTERY_STATUS& st)->bool{
                BATTERY_WAIT_STATUS ws{}; ws.BatteryTag=tag;
                DWORD got=0;
                return !!DeviceIoControl(h,IOCTL_BATTERY_QUERY_STATUS,&ws,sizeof(ws),&st,sizeof(st),&got,nullptr);
            };

            BATTERY_STATUS st{};
            out<<"\n[Direct IOCTL_BATTERY_QUERY_STATUS]\n";
            if(sample(st)){
                out<<"PowerState: "<<power_state(st.PowerState)<<"\n";
                out<<"Capacity raw: "<<st.Capacity<<(relative?"\n":" mWh\n");
                out<<"Voltage raw: "<<st.Voltage<<" mV\n";
                out<<"Rate raw: "<<st.Rate<<(relative?"\n":" mW by Windows contract\n");

                if(!relative && st.Rate!=BATTERY_UNKNOWN_RATE && st.Voltage!=BATTERY_UNKNOWN_VOLTAGE && st.Voltage){
                    double r=std::abs((double)st.Rate);
                    double w=r/1000.0;
                    double ma=r*1000.0/st.Voltage;
                    double alt=r*st.Voltage/1000000.0;
                    out<<std::fixed<<std::setprecision(3);
                    out<<"Windows interpretation: "<<w<<" W\n";
                    out<<"Implied current: "<<ma<<" mA\n";
                    out<<"If raw were actually mA: "<<alt<<" W\n";
                    if(st.Capacity!=BATTERY_UNKNOWN_CAPACITY && r>0)
                        out<<"Capacity/Rate runtime: "<<(st.Capacity/r*60.0)<<" min\n";
                    out.unsetf(std::ios::floatfield);
                }

                qi.InformationLevel=BatteryEstimatedTime; qi.AtRate=0;
                ULONG sec=BATTERY_UNKNOWN_TIME;
                if(DeviceIoControl(h,IOCTL_BATTERY_QUERY_INFORMATION,&qi,sizeof(qi),&sec,sizeof(sec),&ret,nullptr)
                    && sec!=BATTERY_UNKNOWN_TIME){
                    out<<std::fixed<<std::setprecision(1)<<"Driver estimated time: "<<sec/60.0<<" min\n";
                    out.unsetf(std::ios::floatfield);
                } else out<<"Driver estimated time: UNKNOWN\n";
            } else out<<"QUERY_STATUS failed: "<<GetLastError()<<"\n";

            out<<"\n[10-second sampling]\n";
            out<<"sample,capacity_raw,voltage_mV,rate_raw,windows_W,if_raw_were_mA_W,power_state\n";
            for(int i=0;i<10;i++){
                BATTERY_STATUS s{};
                if(sample(s)){
                    out<<i<<","<<s.Capacity<<","<<s.Voltage<<","<<s.Rate<<",";
                    if(!relative && s.Rate!=BATTERY_UNKNOWN_RATE && s.Voltage!=BATTERY_UNKNOWN_VOLTAGE && s.Voltage){
                        double r=std::abs((double)s.Rate);
                        out<<std::fixed<<std::setprecision(4)<<r/1000.0<<","<<r*s.Voltage/1000000.0;
                        out.unsetf(std::ios::floatfield);
                    } else out<<"NA,NA";
                    out<<","<<power_state(s.PowerState)<<"\n";
                } else out<<i<<",QUERY_FAILED,"<<GetLastError()<<"\n";
                if(i<9) std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            CloseHandle(h);
        }
        SetupDiDestroyDeviceInfoList(hdev);
    }

    run_direct_acpi_probe(out);

    out<<"\n=== INTERPRETATION ===\n";
    out<<"If Capacity relative is NO, Windows documents Rate as mW and Capacity as mWh.\n";
    out<<"If direct IOCTL and WMI both report roughly the same implausibly small rate, HWiNFO/TrafficMonitor are probably not the source of the error.\n";
    out<<"If treating the exact raw rate number as mA and multiplying by battery voltage yields a realistic several-watt drain, that is evidence consistent with an mA-vs-mW unit-path bug, but not proof.\n";
    out<<"Direct ACPI _BST/_BIX evaluation is not attempted by this user-mode tool.\n";

    std::string ts=timestamp(); std::wstring wts(ts.begin(),ts.end());
    std::wstring reportPath=exe_dir()+L"\\SurfaceBatteryDiag-"+wts+L".txt";
    std::ofstream f(std::filesystem::path(reportPath),std::ios::binary);
    std::string text=out.str(); f.write(text.data(),(std::streamsize)text.size()); f.close();

    std::cout<<text;
    std::cout<<"\nReport written to: "<<utf8(reportPath)<<"\n";
    if(!nopause){ std::cout<<"Press Enter to exit..."; std::cin.get(); }
    return 0;
}
