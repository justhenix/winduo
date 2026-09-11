#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wtsapi32.h>
#include <powrprof.h>
#include <commctrl.h>
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.UserProfile.h>
#include <winrt/Windows.Data.Json.h>
#undef small
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace wd {
using Microsoft::WRL::ComPtr;
inline double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline void check(bool ok, const wchar_t* message) { if (!ok) throw std::runtime_error(winrt::to_string(message)); }
inline std::filesystem::path folder() {
    PWSTR raw{}; winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw));
    auto p = std::filesystem::path(raw) / L"WinDuo"; CoTaskMemFree(raw); std::filesystem::create_directories(p); return p;
}
struct Settings {
    bool enabled=true, normal=false, lockBlur=false, startup=true, holdAwake=true, webcam=false;
    std::wstring camera;
    double openMean=0,openDelta=0,closedMean=0,closedDelta=0;
    bool calibratedOpen=false,calibratedClosed=false;
    void load() {
        auto ini=folder()/L"settings.ini";
        auto read=[&](const wchar_t* k,int d){return GetPrivateProfileIntW(L"WinDuo",k,d,ini.c_str())!=0;};
        enabled=read(L"Enabled",1);normal=read(L"Normal",0);lockBlur=read(L"LockBlur",0);startup=read(L"Startup",1);
        holdAwake=read(L"HoldAwake",1);webcam=read(L"Webcam",0);calibratedOpen=read(L"CalibratedOpen",0);calibratedClosed=read(L"CalibratedClosed",0);
        wchar_t value[4096]{};GetPrivateProfileStringW(L"WinDuo",L"Camera",L"",value,4096,ini.c_str());camera=value;
        auto number=[&](const wchar_t* k){GetPrivateProfileStringW(L"WinDuo",k,L"0",value,4096,ini.c_str());double n=wcstod(value,nullptr);return std::isfinite(n)?n:0;};
        openMean=number(L"OpenMean");openDelta=number(L"OpenDelta");closedMean=number(L"ClosedMean");closedDelta=number(L"ClosedDelta");
    }
    void save() const {
        auto ini=folder()/L"settings.ini";
        auto text=[&](const wchar_t* k,const std::wstring& v){check(WritePrivateProfileStringW(L"WinDuo",k,v.c_str(),ini.c_str()),L"Could not save settings.");};
        auto bit=[&](const wchar_t* k,bool v){text(k,v?L"1":L"0");};
        bit(L"Enabled",enabled);bit(L"Normal",normal);bit(L"LockBlur",lockBlur);bit(L"Startup",startup);bit(L"HoldAwake",holdAwake);bit(L"Webcam",webcam);
        bit(L"CalibratedOpen",calibratedOpen);bit(L"CalibratedClosed",calibratedClosed);text(L"Camera",camera);
        text(L"OpenMean",std::to_wstring(openMean));text(L"OpenDelta",std::to_wstring(openDelta));text(L"ClosedMean",std::to_wstring(closedMean));text(L"ClosedDelta",std::to_wstring(closedDelta));
    }
};
inline void startup(bool enable) {
    HKEY key{}; check(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)==ERROR_SUCCESS,L"Startup registry unavailable.");
    LSTATUS result{};
    if(enable) { wchar_t path[32768]{}; GetModuleFileNameW(nullptr,path,32768); std::wstring command=L"\""+std::wstring(path)+L"\""; result=RegSetValueExW(key,L"WinDuo",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),static_cast<DWORD>((command.size()+1)*2)); }
    else {result=RegDeleteValueW(key,L"WinDuo"); if(result==ERROR_FILE_NOT_FOUND) result=ERROR_SUCCESS;}
    RegCloseKey(key); check(result==ERROR_SUCCESS,L"Could not update startup.");
}
struct Curve {
    double from=0,target=0,start=0,duration=0;
    static double smooth(double x){x=std::clamp(x,0.0,1.0);return x*x*(3-2*x);}
    double value(double t)const{return duration<=0?target:from+(target-from)*smooth((t-start)/duration);}
    void set(double p,double t,double d){from=value(t);target=std::clamp(p,0.0,1.0);start=t;duration=d;}
    void clear(){from=target=duration=0;}
};
struct LidInput {
    int state=-1;
    double pending=0;
    void reset(){state=-1;pending=0;}
    // Initial notifications establish a baseline, not a gesture.
    bool receive(DWORD value,double time){
        if(value>1)return false;
        if(value==1){bool changed=state==0;state=1;pending=0;return changed;}
        if(state==1)pending=time;
        state=0;return false;
    }
    bool closeReady(double time){if(pending&&time>=pending){pending=0;return true;}return false;}

};
struct Frame {int width=0,height=0;std::vector<uint32_t> pixels; Frame()=default;Frame(int w,int h):width(w),height(h),pixels(size_t(w)*h){};};
inline bool sameRect(RECT a,RECT b){return EqualRect(&a,&b)!=0;}
inline std::optional<RECT> panel() {
    for(int attempt=0;attempt<3;++attempt){
        UINT32 pc{},mc{}; if(GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&pc,&mc))break;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pc);std::vector<DISPLAYCONFIG_MODE_INFO> modes(mc);
        auto error=QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&pc,paths.data(),&mc,modes.data(),nullptr);
        if(error==ERROR_INSUFFICIENT_BUFFER)continue;if(error)break; paths.resize(pc);
        for(auto& p:paths){
            auto tech=p.targetInfo.outputTechnology;
            if(tech!=DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL&&tech!=DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED&&tech!=DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED)continue;
            int shared=0; for(auto& q:paths)if(q.sourceInfo.id==p.sourceInfo.id&&q.sourceInfo.adapterId.HighPart==p.sourceInfo.adapterId.HighPart&&q.sourceInfo.adapterId.LowPart==p.sourceInfo.adapterId.LowPart)++shared;
            if(shared>1)return {};
            DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};name.header={DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,sizeof(name),p.sourceInfo.adapterId,p.sourceInfo.id};
            if(DisplayConfigGetDeviceInfo(&name.header))continue;
            DEVMODEW mode{};mode.dmSize=sizeof(mode);
            if(EnumDisplaySettingsW(name.viewGdiDeviceName,ENUM_CURRENT_SETTINGS,&mode))return RECT{mode.dmPosition.x,mode.dmPosition.y,mode.dmPosition.x+LONG(mode.dmPelsWidth),mode.dmPosition.y+LONG(mode.dmPelsHeight)};
        }
    }return {};
}
inline bool fullscreen(RECT area){
    QUERY_USER_NOTIFICATION_STATE state{};if(SUCCEEDED(SHQueryUserNotificationState(&state))&&(state==QUNS_RUNNING_D3D_FULL_SCREEN||state==QUNS_NOT_PRESENT))return true;
    auto window=GetForegroundWindow();wchar_t name[128]{};GetClassNameW(window,name,128);
    if(!wcscmp(name,L"Progman")||!wcscmp(name,L"WorkerW")||!wcscmp(name,L"Shell_TrayWnd"))return false;
    RECT r{};return GetWindowRect(window,&r)&&r.left<=area.left&&r.top<=area.top&&r.right>=area.right&&r.bottom>=area.bottom;
}
}
