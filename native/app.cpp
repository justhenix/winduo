#include "wallpaper.h"
#include <sstream>
using namespace wd;
namespace {
constexpr wchar_t ClassName[]=L"WinDuo.Native.Accessory";
constexpr UINT TrayMessage=WM_APP+1,FrameMessage=WM_APP+2,ActivateMessage=WM_APP+4;
constexpr int Enable=101,SettingsId=102,Quit=103,Preview=104,Strength=105,LockBlur=106,Startup=107,ShowPreview=108,Status=109;
const GUID LidGuid={0xBA3E0F4D,0xB817,0x4094,{0xA2,0xD1,0xD5,0x63,0x79,0xE6,0xA0,0xF3}};
struct RenderResult{Frame image,snapshot;unsigned epoch{};double captureMs{},renderMs{};std::wstring error;};
class App {
public:
    HWND window{},settingsWindow{},overlay{};HPOWERNOTIFY power{};NOTIFYICONDATAW tray{};HFONT font{};
    Settings prefs;Curve curve;std::optional<RECT> bounds;std::unique_ptr<Dib> dib;std::unique_ptr<Wallpaper> wallpaper;
    std::mutex mutex;std::condition_variable wake;std::thread renderer;bool stop=false,job=false,clearCapture=false;RECT jobRect{};double jobProgress{};bool jobNormal{},jobSnapshot{};unsigned jobEpoch{};
    std::unique_ptr<RenderResult> result;bool busy=false,locked=false,suspended=false,manual=false,armed=false;unsigned epoch=0;LidInput lid;double demo=-1,hold=0;Frame cached;
    bool smoke=false,testPreview=false;double firstFrame=0,lastFrame=0;double started=now();int captures=0,presents=0,activations=0;std::wstring status=L"Ready";UINT taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
    double captureTotal=0,renderTotal=0,presentTotal=0;
    void report(std::wstring text){status=std::move(text);if(settingsWindow)SetWindowTextW(GetDlgItem(settingsWindow,Status),status.c_str());}
    void save(){if(!smoke)prefs.save();}
    void restore(){armed=false;if(wallpaper)wallpaper->requestRestore();}
    void hide(){++epoch;if(overlay)ShowWindow(overlay,SW_HIDE);}
    void reset(){curve.clear();lid.pending=lid.expires=0;manual=false;demo=-1;hide();hold=0;SetThreadExecutionState(ES_CONTINUOUS);restore();}
    void detect(){reset();if(overlay){DestroyWindow(overlay);overlay=nullptr;}dib.reset();cached={};bounds=panel();report(bounds?L"Lid input: open/closed only, not hinge angle.":L"No separate internal panel. Effect paused.");}
    void renderLoop(){Capture capture;
        for(;;){RECT area;double progress;bool strong,snapshot;unsigned version;
            {std::unique_lock lock(mutex);wake.wait(lock,[&]{return stop||job||clearCapture;});if(stop)break;if(clearCapture){capture.clear();clearCapture=false;}if(!job)continue;area=jobRect;progress=jobProgress;strong=jobNormal;snapshot=jobSnapshot;version=jobEpoch;job=false;}
            auto output=std::make_unique<RenderResult>();output->epoch=version;
            try{double start=now();auto source=capture.get(area);output->captureMs=(now()-start)*1000;start=now();if(snapshot)output->snapshot=resize(source,960);output->image=blur(source,progress,strong);output->renderMs=(now()-start)*1000;}catch(...){output->error=L"Capture unavailable. WinDuo paused.";}
            {std::lock_guard lock(mutex);result=std::move(output);}PostMessageW(window,FrameMessage,0,0);
        }
    }
    void addTray(){tray={sizeof(tray)};tray.hWnd=window;tray.uID=1;tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;tray.uCallbackMessage=TrayMessage;tray.hIcon=LoadIconW(nullptr,IDI_APPLICATION);wcscpy_s(tray.szTip,L"WinDuo");Shell_NotifyIconW(NIM_ADD,&tray);}
    void initialize(HWND hwnd){window=hwnd;if(!smoke){prefs.load();startup(prefs.startup);save();}font=CreateFontW(-MulDiv(10,GetDpiForWindow(hwnd),72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        power=RegisterPowerSettingNotification(window,&LidGuid,DEVICE_NOTIFY_WINDOW_HANDLE);check(power!=nullptr,L"Lid notifications unavailable.");check(WTSRegisterSessionNotification(window,NOTIFY_FOR_THIS_SESSION),L"Session notifications unavailable.");
        if(!smoke)wallpaper=std::make_unique<Wallpaper>(window);detect();addTray();renderer=std::thread([this]{renderLoop();});SetTimer(window,1,1000,nullptr);
    }
    void preview(){if(!prefs.preview||!prefs.enabled||locked||!bounds)return;reset();manual=true;demo=now()+2;if(smoke){testPreview=true;started=now();}SetTimer(window,1,16,nullptr);}
    void progress(double value){curve.set(value,now(),value>curve.value(now())?.6:.4);SetTimer(window,1,16,nullptr);if(value>0){SetThreadExecutionState(ES_CONTINUOUS|ES_DISPLAY_REQUIRED|ES_SYSTEM_REQUIRED);hold=now()+1.5;}}
    void tick(){double t=now();if(smoke&&t-started>5){DestroyWindow(window);return;}if(hold&&t>=hold){hold=0;SetThreadExecutionState(ES_CONTINUOUS);}
        if(!manual&&!locked&&!suspended&&prefs.enabled){if(lid.closeReady(t)){report(L"Lid-close signal received from Windows");progress(1);}if(lid.expired(t)){progress(0);report(L"Ready");}}
        if((curve.value(t)>.001||demo>=0)&&(GetAsyncKeyState(VK_ESCAPE)&0x8000)){reset();prefs.enabled=false;save();sync();return;}
        if(demo>=0&&t>=demo){if(t-demo<2.6&&curve.target==0)progress(1);else if(t-demo>=2.6){progress(0);demo=-1;}}
        double p=curve.value(t);if(p<=.001&&curve.target==0){if(overlay&&IsWindowVisible(overlay))hide();if(armed)restore();if(demo<0)manual=false;{std::lock_guard lock(mutex);clearCapture=true;wake.notify_one();}SetTimer(window,1,demo>=0||lid.pending?16:1000,nullptr);return;}
        if(!prefs.enabled||locked||suspended||!bounds||fullscreen(*bounds)){hide();SetTimer(window,1,250,nullptr);return;}
        SetTimer(window,1,16,nullptr);if(busy||p<=.001)return;
        if(!overlay){overlay=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"",WS_POPUP,bounds->left,bounds->top,bounds->right-bounds->left,bounds->bottom-bounds->top,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);check(overlay&&SetWindowDisplayAffinity(overlay,WDA_EXCLUDEFROMCAPTURE),L"Overlay capture exclusion failed.");dib=std::make_unique<Dib>(bounds->right-bounds->left,bounds->bottom-bounds->top);}
        {std::lock_guard lock(mutex);jobRect=*bounds;jobProgress=p;jobNormal=prefs.normal;jobSnapshot=prefs.lockBlur;jobEpoch=epoch;job=true;busy=true;++captures;wake.notify_one();}
    }
    void present(){std::unique_ptr<RenderResult> output;{std::lock_guard lock(mutex);output=std::move(result);}busy=false;if(!output||output->epoch!=epoch||locked||!prefs.enabled)return;
        if(!output->error.empty()){reset();prefs.enabled=false;report(output->error);sync();return;}if(!dib||!bounds)return;
        captureTotal+=output->captureMs;renderTotal+=output->renderMs;double presentStart=now();cached=std::move(output->snapshot);memcpy(dib->bits,output->image.pixels.data(),output->image.pixels.size()*4);POINT origin{},position{bounds->left,bounds->top};SIZE size{dib->width,dib->height};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        check(UpdateLayeredWindow(overlay,nullptr,&position,&size,dib->dc,&origin,0,&blend,ULW_ALPHA),L"Overlay presentation failed.");ShowWindow(overlay,SW_SHOWNOACTIVATE);if(presents++==0)firstFrame=now();lastFrame=now();
        presentTotal+=(now()-presentStart)*1000;if(prefs.lockBlur&&!armed&&curve.value(now())>=.7&&wallpaper){armed=true;wallpaper->requestApply(cached,prefs.normal);}tick();
    }
    void sync(){if(!settingsWindow)return;auto set=[&](int id,bool checked){SendDlgItemMessageW(settingsWindow,id,BM_SETCHECK,checked?BST_CHECKED:BST_UNCHECKED,0);};set(Enable,prefs.enabled);set(LockBlur,prefs.lockBlur);set(Startup,prefs.startup);set(ShowPreview,prefs.preview);SendDlgItemMessageW(settingsWindow,Strength,CB_SETCURSEL,prefs.normal?1:0,0);ShowWindow(GetDlgItem(settingsWindow,Preview),prefs.preview?SW_SHOW:SW_HIDE);EnableWindow(GetDlgItem(settingsWindow,Preview),prefs.enabled);SetWindowTextW(GetDlgItem(settingsWindow,Status),status.c_str());}
    void menu(){HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING|(prefs.enabled?MF_CHECKED:0),Enable,L"Enable");AppendMenuW(menu,MF_STRING,SettingsId,L"Settings");if(prefs.preview)AppendMenuW(menu,MF_STRING,Preview,L"Preview");AppendMenuW(menu,MF_STRING,Quit,L"Quit");POINT point;GetCursorPos(&point);SetForegroundWindow(window);int id=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,window,nullptr);DestroyMenu(menu);PostMessageW(window,WM_NULL,0,0);if(id)command(id,0);}
    void openSettings();
    void command(int id,int notification){
        // Opening/closing the combo must not reset its current selection or dismiss its list.
        if(id==Strength&&notification!=CBN_SELCHANGE)return;
        switch(id){case Enable:prefs.enabled=!prefs.enabled;if(!prefs.enabled)reset();save();break;
        case SettingsId:openSettings();break;case Quit:DestroyWindow(window);break;case Preview:preview();break;
        case Strength:prefs.normal=SendDlgItemMessageW(settingsWindow,Strength,CB_GETCURSEL,0,0)==1;save();break;
        case LockBlur:prefs.lockBlur=!prefs.lockBlur;if(!prefs.lockBlur){restore();cached={};}save();break;
        case Startup:if(!smoke)startup(!prefs.startup);prefs.startup=!prefs.startup;save();break;
        case ShowPreview:prefs.preview=!prefs.preview;if(!prefs.preview&&manual)reset();save();break;}
        sync();
    }
    void shutdown(){KillTimer(window,1);hide();SetThreadExecutionState(ES_CONTINUOUS);{std::lock_guard lock(mutex);stop=true;wake.notify_one();}if(renderer.joinable())renderer.join();wallpaper.reset();if(power)UnregisterPowerSettingNotification(power);WTSUnRegisterSessionNotification(window);Shell_NotifyIconW(NIM_DELETE,&tray);if(settingsWindow)DestroyWindow(settingsWindow);if(overlay)DestroyWindow(overlay);DeleteObject(font);
        if(smoke){std::filesystem::create_directories(L"artifacts");std::ofstream log(L"artifacts/smoke-test.txt");log<<((testPreview?presents>0:captures==0&&presents==0)?"PASS":"FAIL")<<" captures="<<captures<<" presented="<<presents<<"; preview explicitly requested="<<testPreview<<"\n";if(testPreview)log<<"Live FPS="<<(presents-1)/std::max(.001,lastFrame-firstFrame)<<"; capture/render/present ms="<<captureTotal/std::max(1,presents)<<"/"<<renderTotal/std::max(1,presents)<<"/"<<presentTotal/std::max(1,presents)<<"\n";log<<"Settings activations="<<activations<<"\n";log<<"No settings, startup, wallpaper or lock side effects in smoke mode.\n";}
        PostQuitMessage(0);
    }
};
LRESULT CALLBACK SettingsProc(HWND hwnd,UINT message,WPARAM w,LPARAM l){auto app=reinterpret_cast<App*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));if(message==WM_NCCREATE){app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}
    if(app)try{if(message==WM_COMMAND){app->command(LOWORD(w),HIWORD(w));return 0;}if(message==WM_CLOSE){ShowWindow(hwnd,SW_HIDE);return 0;}if(message==WM_DPICHANGED){auto r=reinterpret_cast<RECT*>(l);SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER);return 0;}}catch(...){app->report(L"Could not apply setting.");app->sync();}return DefWindowProcW(hwnd,message,w,l);
}
void App::openSettings(){++activations;if(!settingsWindow){WNDCLASSW cls{};cls.lpfnWndProc=SettingsProc;cls.hInstance=GetModuleHandleW(nullptr);cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);cls.lpszClassName=L"WinDuo.Native.Settings";RegisterClassW(&cls);
        int dpi=GetDpiForWindow(window);auto scale=[&](int n){return MulDiv(n,dpi,96);};RECT rect{0,0,scale(410),scale(355)};AdjustWindowRectExForDpi(&rect,WS_CAPTION|WS_SYSMENU,FALSE,WS_EX_TOOLWINDOW,dpi);
        settingsWindow=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_CONTROLPARENT,cls.lpszClassName,L"WinDuo Settings",WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,window,nullptr,cls.hInstance,this);
        if(!smoke)SetWindowDisplayAffinity(settingsWindow,WDA_EXCLUDEFROMCAPTURE);
        auto control=[&](const wchar_t* kind,const wchar_t* text,int id,int x,int y,int width,int height,DWORD style){auto c=CreateWindowExW(0,kind,text,WS_CHILD|WS_VISIBLE|style,scale(x),scale(y),scale(width),scale(height),settingsWindow,reinterpret_cast<HMENU>(INT_PTR(id)),cls.hInstance,nullptr);SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return c;};
        control(L"BUTTON",L"Enable WinDuo",Enable,20,18,300,26,BS_AUTOCHECKBOX|WS_TABSTOP);
        control(L"STATIC",L"Strength",0,20,58,100,24,0);auto combo=control(L"COMBOBOX",L"",Strength,135,54,140,120,CBS_DROPDOWNLIST|WS_TABSTOP);SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Slight"));SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Normal"));
        control(L"BUTTON",L"Lock screen blur",LockBlur,20,96,340,26,BS_AUTOCHECKBOX|WS_TABSTOP);
        control(L"STATIC",L"Local image with backup and restore.\nWin+L needs an existing cached frame.",0,40,125,350,44,0);
        control(L"BUTTON",L"Start with Windows",Startup,20,179,340,26,BS_AUTOCHECKBOX|WS_TABSTOP);
        control(L"BUTTON",L"Show preview menu",ShowPreview,20,217,340,26,BS_AUTOCHECKBOX|WS_TABSTOP);
        control(L"BUTTON",L"Quit",Quit,20,260,88,32,WS_TABSTOP);control(L"BUTTON",L"Preview",Preview,120,260,88,32,WS_TABSTOP);
        control(L"STATIC",L"",Status,20,308,370,40,0);
    }sync();ShowWindow(settingsWindow,SW_SHOW);SetForegroundWindow(settingsWindow);SetFocus(GetDlgItem(settingsWindow,Enable));}
LRESULT CALLBACK AppProc(HWND hwnd,UINT message,WPARAM w,LPARAM l){auto app=reinterpret_cast<App*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));if(message==WM_NCCREATE){app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}if(!app)return DefWindowProcW(hwnd,message,w,l);
    try{
        if(message==app->taskbarCreated){app->addTray();return 0;}
        switch(message){case WM_TIMER:app->tick();return 0;case FrameMessage:app->present();return 0;case ActivateMessage:app->openSettings();return 0;
        case WM_COMMAND:app->command(LOWORD(w),HIWORD(w));return 0;
        case WM_APP+3:app->report(L"Lock image unavailable; recovery retained if needed.");return 0;
        case TrayMessage:if(l==WM_LBUTTONDBLCLK)app->openSettings();else if(l==WM_RBUTTONUP||l==WM_CONTEXTMENU)app->menu();return 0;
        case WM_DISPLAYCHANGE:app->detect();return 0;
        case WM_WTSSESSION_CHANGE:if(w==WTS_SESSION_LOCK){app->locked=true;app->curve.clear();app->lid.reset();app->manual=false;app->demo=-1;app->hide();SetThreadExecutionState(ES_CONTINUOUS);if(app->prefs.enabled&&app->prefs.lockBlur&&!app->cached.pixels.empty()&&app->wallpaper)app->wallpaper->requestApply(app->cached,app->prefs.normal);}else if(w==WTS_SESSION_UNLOCK){app->locked=false;app->reset();}return 0;
        case WM_POWERBROADCAST:
            if(w==PBT_POWERSETTINGCHANGE){auto setting=reinterpret_cast<POWERBROADCAST_SETTING*>(l);if(setting&&setting->PowerSetting==LidGuid&&setting->DataLength==sizeof(DWORD)){DWORD open;memcpy(&open,setting->Data,sizeof(open));if(open<=1){bool opened=app->lid.receive(open,now());app->report(open?L"Windows lid state: open (no angle available).":L"Windows lid state: closed.");if(app->manual||app->locked||app->suspended||!app->prefs.enabled){app->lid.pending=0;}else{if(opened){app->hide();app->progress(0);app->restore();}if(app->lid.closeReady(now()))app->progress(1);}}}}
            else if(w==PBT_APMSUSPEND){app->suspended=true;app->reset();}else if(w==PBT_APMRESUMEAUTOMATIC||w==PBT_APMRESUMESUSPEND){app->suspended=false;app->detect();}return TRUE;
        case WM_CLOSE:DestroyWindow(hwnd);return 0;case WM_DESTROY:app->shutdown();return 0;
        }
    }catch(...){app->reset();app->prefs.enabled=false;app->report(L"WinDuo paused after an operating system error.");app->sync();}
    return DefWindowProcW(hwnd,message,w,l);
}
int selfTest(){std::filesystem::create_directories(L"artifacts");std::ofstream log(L"artifacts/self-test.txt");int failed=0;auto test=[&](bool ok,const char* text){log<<(ok?"PASS ":"FAIL ")<<text<<"\n";if(!ok)++failed;};
    LidInput input;input.receive(0,1);test(!input.closeReady(2),"Initial closed notification never triggers blur");input.receive(1,3);input.receive(0,4);input.receive(1,4.05);test(!input.closeReady(5),"Open cancels an unconsumed close event");input.receive(0,6);test(input.closeReady(6),"Close starts immediately without extra debounce delay");test(input.expired(8),"Missing open event cannot leave blur stuck");input.reset();input.receive(0,9);test(!input.closeReady(10),"Unknown initial state never triggers close");input.receive(1,11);input.receive(999,12);test(!input.closeReady(13),"Invalid lid payload is ignored");
    App app;app.smoke=true;app.lid.receive(1,1);app.reset();app.lid.receive(0,2);test(app.lid.closeReady(2),"Pause/preview reset preserves open baseline for next real close");
    test(Curve::smooth(-1)==0&&Curve::smooth(2)==1,"Smoothstep bounds");Curve c;c.set(1,0,.6);test(std::abs(c.value(.3)-.5)<1e-6,"Close midpoint");c.set(0,.3,.4);test(std::abs(c.value(.3)-.5)<1e-6&&c.value(.71)==0,"Continuous reversal and open");c.clear();test(c.value(1)==0,"Immediate reset");Settings settings;test(settings.enabled&&settings.startup&&!settings.preview&&!settings.lockBlur&&!settings.normal,"Accessory defaults");
    Frame frame(960,540);for(int y=0;y<frame.height;++y)for(int x=0;x<frame.width;++x)frame.pixels[size_t(y)*frame.width+x]=0xff000000|((x%20<6)?0:0xeeeeee);
    test(blur(frame,0,false).pixels==frame.pixels,"Zero strength preserves pixels");auto start=now();auto output=blur(frame,1,false);test(output.pixels[20*960+2]!=frame.pixels[20*960+2],"Gaussian softens stripes");test((output.pixels[20*960+2]&255)>(output.pixels[500*960+2]&255),"Blur tapers toward hinge");test(resize(frame,480).height==270,"Snapshot aspect ratio");test(hash({1,2,3})=="039058C6F2C0CB492C533B0A4D14EF77CC0F78ABCCCED5287D84A1A2011CFB81", "SHA256 recovery fingerprint");
    log<<"Native 960x540 blur ms: "<<(now()-start)*1000<<"\n";return failed?1:0;
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR command,int){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);winrt::init_apartment(winrt::apartment_type::single_threaded);
    if(wcsstr(command,L"--self-test"))return selfTest();
    HANDLE mutex=CreateMutexW(nullptr,FALSE,L"Local\\WinDuo.Native.Tray");if(GetLastError()==ERROR_ALREADY_EXISTS){for(int i=0;i<30;++i){if(auto window=FindWindowW(ClassName,nullptr)){DWORD pid;GetWindowThreadProcessId(window,&pid);AllowSetForegroundWindow(pid);PostMessageW(window,ActivateMessage,0,0);break;}Sleep(100);}CloseHandle(mutex);return 0;}
    // Hand off to v0.1 if it is still running; avoid two overlays during an upgrade.
    if(HANDLE legacy=OpenMutexW(SYNCHRONIZE,FALSE,L"Local\\WinDuo.Tray")){CloseHandle(legacy);if(HANDLE signal=OpenEventW(EVENT_MODIFY_STATE,FALSE,L"Local\\WinDuo.OpenSettings")){SetEvent(signal);CloseHandle(signal);}MessageBoxW(nullptr,L"Quit the previous WinDuo version from its tray, then launch this native build.",L"WinDuo",MB_OK);CloseHandle(mutex);return 0;}
    App app;app.smoke=wcsstr(command,L"--smoke-test")!=nullptr;WNDCLASSW cls{};cls.lpfnWndProc=AppProc;cls.hInstance=instance;cls.lpszClassName=ClassName;RegisterClassW(&cls);
    HWND window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,ClassName,L"WinDuo",WS_POPUP,0,0,0,0,nullptr,nullptr,instance,&app);
    try{check(window!=nullptr,L"Cannot create message window.");app.initialize(window);MSG message;while(GetMessageW(&message,nullptr,0,0)>0){if(app.settingsWindow&&IsWindowVisible(app.settingsWindow)){if(message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE){ShowWindow(app.settingsWindow,SW_HIDE);continue;}if(IsDialogMessageW(app.settingsWindow,&message))continue;}TranslateMessage(&message);DispatchMessageW(&message);}}
    catch(...){MessageBoxW(nullptr,L"WinDuo could not start. Check Windows session and startup permissions.",L"WinDuo",MB_ICONERROR);if(window)DestroyWindow(window);CloseHandle(mutex);return 1;}
    CloseHandle(mutex);return 0;
}
