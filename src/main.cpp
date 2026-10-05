#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <batclass.h>
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

int wmain(int argc, wchar_t** argv) {
    bool nopause=false;
    for(int i=1;i<argc;i++) if(std::wstring(argv[i])==L"--no-pause") nopause=true;

    std::ostringstream out;
    out<<"SurfaceBatteryDiag v0.1.0\n";
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
