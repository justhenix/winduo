#include "wallpaper.h"
#include <sstream>
#include "webcam.h"
#include "gpu.h"
#include <dwmapi.h>
#include <uxtheme.h>
using namespace wd;
namespace {
constexpr wchar_t ClassName[]=L"WinDuo.Native.Accessory";
constexpr UINT TrayMessage=WM_APP+1,FrameMessage=WM_APP+2,ActivateMessage=WM_APP+4;
constexpr int Enable=101,SettingsId=102,Quit=103,ToggleHinge=104,Strength=105,LockBlur=106,Startup=107,HoldAwake=108,Status=109,WebcamId=110,CalibrateOpen=111,CalibrateClosed=112,CameraId=113;
const GUID LidGuid={0xBA3E0F4D,0xB817,0x4094,{0xA2,0xD1,0xD5,0x63,0x79,0xE6,0xA0,0xF3}};
struct RenderResult{Frame image,snapshot;unsigned epoch{};double captureMs{},renderMs{};std::wstring error;};
class App {
public:
    HWND window{},settingsWindow{},overlay{};HPOWERNOTIFY power{};NOTIFYICONDATAW tray{};HFONT font{};
    Settings prefs;Curve curve;std::optional<RECT> bounds;std::unique_ptr<Dib> dib;std::unique_ptr<Wallpaper> wallpaper;
    std::mutex mutex;std::condition_variable wake;std::thread renderer;bool stop=false,job=false,clearCapture=false;RECT jobRect{};double jobProgress{};bool jobNormal{},jobSnapshot{};unsigned jobEpoch{};
    std::unique_ptr<RenderResult> result;bool busy=false,locked=false,suspended=false,manual=false,armed=false;unsigned epoch=0;LidInput lid;double hold=0;Frame cached;
    bool smoke=false,testEffect=false;double firstFrame=0,lastFrame=0;double started=now();int captures=0,presents=0,activations=0;std::wstring status=L"Ready";UINT taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
    HBRUSH background=CreateSolidBrush(RGB(24,24,26)),surface=CreateSolidBrush(RGB(38,38,42));
    std::unique_ptr<Webcam> camera;std::vector<CameraDevice> cameraDevices;
    bool cameraActive=false,cameraWarned=false;double cameraSince=0,cameraIdle=0,cameraStamp=0;
    CameraSignal cameraValue{},baseline{};int baselineCount=0,pendingCalibration=0;
    void startCamera(){if(smoke||!prefs.webcam||!prefs.enabled||locked||suspended||lid.state==0||!bounds)return;if(!camera)camera=std::make_unique<Webcam>();camera->start(prefs);cameraActive=true;cameraSince=now();pendingCalibration=0;cameraIdle=0;cameraStamp=0;baselineCount=0;baseline={};SetTimer(window,1,67,nullptr);}
    void stopCamera(){if(camera)camera->stop();cameraActive=false;pendingCalibration=0;}
    void cameraTick(double t){if(!cameraActive||!camera)return;
        if(!prefs.enabled||!prefs.webcam||locked||suspended||lid.state==0||!bounds||fullscreen(*bounds)){stopCamera();if(!manual)progress(0);report(L"Camera paused. Re-enable or calibrate to resume.");return;}
        CameraSignal value;double stamp;std::wstring message;std::vector<CameraDevice> list;bool running=camera->read(value,stamp,message,list);
        if(list.size()!=cameraDevices.size()||!std::equal(list.begin(),list.end(),cameraDevices.begin(),[](const CameraDevice& a,const CameraDevice& b){return a.id==b.id&&a.name==b.name;})){cameraDevices=std::move(list);fillCameras();}
        if(!running){cameraActive=false;pendingCalibration=0;if(!manual)progress(0);report(message);if(!cameraWarned){cameraWarned=true;tray.uFlags=NIF_INFO;wcscpy_s(tray.szInfoTitle,L"WinDuo camera unavailable");wcscpy_s(tray.szInfo,L"Use Ctrl+Alt+Space or select a camera in Settings.");Shell_NotifyIconW(NIM_MODIFY,&tray);}return;}
        if(stamp>cameraStamp){if(!cameraStamp)cameraSince=t;cameraStamp=stamp;cameraValue=value;
            if(t-cameraSince<1){baseline.mean+=value.mean;baseline.delta+=value.delta;++baselineCount;return;}
            if(pendingCalibration){bool open=pendingCalibration==1;pendingCalibration=0;if(open){prefs.openMean=value.mean;prefs.openDelta=value.delta;prefs.calibratedOpen=true;}else{prefs.closedMean=value.mean;prefs.closedDelta=value.delta;prefs.calibratedClosed=true;}save();report(open?L"Open sample saved.":L"Almost-closed sample saved.");}
            CameraSignal a=prefs.calibratedOpen?CameraSignal{prefs.openMean,prefs.openDelta}:CameraSignal{baseline.mean/std::max(1,baselineCount),baseline.delta/std::max(1,baselineCount)};
            CameraSignal b=prefs.calibratedClosed?CameraSignal{prefs.closedMean,prefs.closedDelta}:CameraSignal{std::max(0.0,a.mean-.35),a.delta-.15};
            if(!manual){double p=cameraProgress(value,a,b);if(p<.04)p=0;curve.set(p,t,.1);}
            if(curve.target<.01&&curve.value(t)<.01){if(!cameraIdle)cameraIdle=t;if(t-cameraIdle>=3){stopCamera();report(L"Camera stopped after idle. Calibrate or re-enable to resume.");}}else cameraIdle=0;
        }
    }
    void fillCameras(){if(!settingsWindow)return;auto box=GetDlgItem(settingsWindow,CameraId);SendMessageW(box,CB_RESETCONTENT,0,0);SendMessageW(box,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Automatic (front camera only)"));int selected=0;for(size_t i=0;i<cameraDevices.size();++i){SendMessageW(box,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(cameraDevices[i].name.c_str()));if(cameraDevices[i].id==prefs.camera)selected=int(i+1);}SendMessageW(box,CB_SETCURSEL,selected,0);}
    double captureTotal=0,renderTotal=0,presentTotal=0;
    void report(std::wstring text){status=std::move(text);if(settingsWindow){SetWindowTextW(GetDlgItem(settingsWindow,Status),status.c_str());InvalidateRect(settingsWindow,nullptr,TRUE);}}
    void save(){if(!smoke)prefs.save();}
    void restore(){armed=false;if(wallpaper)wallpaper->requestRestore();}
    void hide(){++epoch;if(overlay)ShowWindow(overlay,SW_HIDE);}
    void reset(){curve.clear();lid.pending=0;manual=false;stopCamera();hide();hold=0;SetThreadExecutionState(ES_CONTINUOUS);restore();}
    void detect(){reset();if(overlay){DestroyWindow(overlay);overlay=nullptr;}dib.reset();cached={};bounds=panel();report(bounds?L"Lid input: open/closed only, not hinge angle.":L"No separate internal panel. Effect paused.");}
    void renderLoop(){Capture capture;GpuBlur gpu;
        for(;;){RECT area;double progress;bool strong,snapshot;unsigned version;
            {std::unique_lock lock(mutex);wake.wait(lock,[&]{return stop||job||clearCapture;});if(stop)break;if(clearCapture){capture.clear();gpu.clear();clearCapture=false;}if(!job)continue;area=jobRect;progress=jobProgress;strong=jobNormal;snapshot=jobSnapshot;version=jobEpoch;job=false;}
            auto output=std::make_unique<RenderResult>();output->epoch=version;
            try{double start=now();auto source=capture.get(area);output->captureMs=(now()-start)*1000;start=now();if(snapshot)output->snapshot=resize(source,960);output->image=gpu.render(source,progress,strong);output->renderMs=(now()-start)*1000;}catch(...){output->error=L"Capture unavailable. WinDuo paused.";}
            {std::lock_guard lock(mutex);result=std::move(output);}PostMessageW(window,FrameMessage,0,0);
        }
    }
    void addTray(){tray={sizeof(tray)};tray.hWnd=window;tray.uID=1;tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;tray.uCallbackMessage=TrayMessage;tray.hIcon=LoadIconW(nullptr,IDI_APPLICATION);wcscpy_s(tray.szTip,L"WinDuo");Shell_NotifyIconW(NIM_ADD,&tray);}
    void initialize(HWND hwnd){window=hwnd;if(!smoke){prefs.load();startup(prefs.startup);save();}font=CreateFontW(-MulDiv(10,GetDpiForWindow(hwnd),72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        power=RegisterPowerSettingNotification(window,&LidGuid,DEVICE_NOTIFY_WINDOW_HANDLE);check(power!=nullptr,L"Lid notifications unavailable.");check(WTSRegisterSessionNotification(window,NOTIFY_FOR_THIS_SESSION),L"Session notifications unavailable.");
        if(!smoke)wallpaper=std::make_unique<Wallpaper>(window);detect();addTray();renderer=std::thread([this]{renderLoop();});SetTimer(window,1,1000,nullptr);if(!RegisterHotKey(window,1,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_SPACE))report(L"Hotkey unavailable; use tray click.");if(prefs.webcam)startCamera();
    }
    void toggleHinge(){if(!prefs.enabled||locked||suspended||!bounds)return;manual=true;lid.pending=0;progress(curve.target>.5?0:1);if(smoke)testEffect=true;}
    void progress(double value,bool lidClose=false){curve.set(value,now(),value>curve.value(now())?.5:.4);SetTimer(window,1,16,nullptr);if(lidClose&&prefs.holdAwake){SetThreadExecutionState(ES_CONTINUOUS|ES_DISPLAY_REQUIRED|ES_SYSTEM_REQUIRED);hold=now()+1.5;}}
    void tick(){double t=now();if(smoke&&t-started>5){DestroyWindow(window);return;}if(hold&&t>=hold){hold=0;SetThreadExecutionState(ES_CONTINUOUS);}
        cameraTick(t);
        if((curve.value(t)>.001)&&(GetAsyncKeyState(VK_ESCAPE)&0x8000)){reset();prefs.enabled=false;save();sync();return;}
        double p=curve.value(t);if(p<=.001&&curve.target==0){hide();if(armed)restore();if(!cameraActive)manual=false;{std::lock_guard lock(mutex);clearCapture=true;wake.notify_one();}SetTimer(window,1,cameraActive?67:1000,nullptr);return;}
        if(!prefs.enabled||locked||suspended||!bounds||fullscreen(*bounds)){hide();SetTimer(window,1,250,nullptr);return;}
        SetTimer(window,1,16,nullptr);if(busy||p<=.001)return;
        if(!overlay){overlay=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"",WS_POPUP,bounds->left,bounds->top,bounds->right-bounds->left,bounds->bottom-bounds->top,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);check(overlay&&SetWindowDisplayAffinity(overlay,WDA_EXCLUDEFROMCAPTURE),L"Overlay capture exclusion failed.");dib=std::make_unique<Dib>(bounds->right-bounds->left,bounds->bottom-bounds->top);}
        {std::lock_guard lock(mutex);jobRect=*bounds;jobProgress=p;jobNormal=prefs.normal;jobSnapshot=prefs.lockBlur;jobEpoch=epoch;job=true;busy=true;++captures;wake.notify_one();}
    }
    void present(){std::unique_ptr<RenderResult> output;{std::lock_guard lock(mutex);output=std::move(result);}busy=false;if(!output||output->epoch!=epoch||locked||!prefs.enabled)return;
        if(!output->error.empty()){reset();prefs.enabled=false;report(output->error);sync();return;}if(!dib||!bounds)return;if(suspended||fullscreen(*bounds)){hide();return;}
        captureTotal+=output->captureMs;renderTotal+=output->renderMs;double presentStart=now();cached=std::move(output->snapshot);memcpy(dib->bits,output->image.pixels.data(),output->image.pixels.size()*4);POINT origin{},position{bounds->left,bounds->top};SIZE size{dib->width,dib->height};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        check(UpdateLayeredWindow(overlay,nullptr,&position,&size,dib->dc,&origin,0,&blend,ULW_ALPHA),L"Overlay presentation failed.");ShowWindow(overlay,SW_SHOWNOACTIVATE);if(presents++==0)firstFrame=now();lastFrame=now();
        presentTotal+=(now()-presentStart)*1000;if(prefs.lockBlur&&!armed&&curve.value(now())>=.7&&wallpaper){armed=true;wallpaper->requestApply(cached,prefs.normal);}tick();
    }
    void sync(){if(!settingsWindow)return;auto set=[&](int id,bool checked){SendDlgItemMessageW(settingsWindow,id,BM_SETCHECK,checked?BST_CHECKED:BST_UNCHECKED,0);};set(Enable,prefs.enabled);set(LockBlur,prefs.lockBlur);set(Startup,prefs.startup);set(HoldAwake,prefs.holdAwake);set(WebcamId,prefs.webcam);SendDlgItemMessageW(settingsWindow,Strength,CB_SETCURSEL,prefs.normal?1:0,0);for(int id:{CalibrateOpen,CalibrateClosed,CameraId})EnableWindow(GetDlgItem(settingsWindow,id),prefs.webcam);SetWindowTextW(GetDlgItem(settingsWindow,Status),status.c_str());InvalidateRect(settingsWindow,nullptr,TRUE);}
    void menu(){HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING|(prefs.enabled?MF_CHECKED:0),Enable,L"Enable");AppendMenuW(menu,MF_STRING,SettingsId,L"Settings");AppendMenuW(menu,MF_STRING,Quit,L"Quit");POINT point;GetCursorPos(&point);SetForegroundWindow(window);int id=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,window,nullptr);DestroyMenu(menu);PostMessageW(window,WM_NULL,0,0);if(id)command(id,0);}
    void openSettings();
    void command(int id,int notification){
        // Opening/closing the combo must not reset its current selection or dismiss its list.
        if((id==Strength||id==CameraId)&&notification!=CBN_SELCHANGE)return;
        switch(id){case Enable:prefs.enabled=!prefs.enabled;if(!prefs.enabled)reset();save();break;
        case SettingsId:openSettings();break;case Quit:DestroyWindow(window);break;case ToggleHinge:toggleHinge();break;
        case Strength:prefs.normal=SendDlgItemMessageW(settingsWindow,Strength,CB_GETCURSEL,0,0)==1;save();break;
        case LockBlur:prefs.lockBlur=!prefs.lockBlur;if(!prefs.lockBlur){restore();cached={};}save();break;
        case Startup:if(!smoke)startup(!prefs.startup);prefs.startup=!prefs.startup;save();break;
        case HoldAwake:prefs.holdAwake=!prefs.holdAwake;if(!prefs.holdAwake){hold=0;SetThreadExecutionState(ES_CONTINUOUS);}save();break;
        case WebcamId:prefs.webcam=!prefs.webcam;if(prefs.webcam){manual=false;startCamera();}else{stopCamera();if(!manual)progress(0);}save();break;
        case CameraId:{int i=int(SendDlgItemMessageW(settingsWindow,CameraId,CB_GETCURSEL,0,0));prefs.camera=i>0&&size_t(i)<=cameraDevices.size()?cameraDevices[i-1].id:L"";prefs.calibratedOpen=prefs.calibratedClosed=false;save();if(prefs.webcam)startCamera();break;}
        case CalibrateOpen:case CalibrateClosed:if(prefs.webcam){manual=false;if(!cameraActive)startCamera();cameraIdle=0;pendingCalibration=id==CalibrateOpen?1:2;report(L"Hold position while a camera sample is collected.");}break;}
        sync();
    }
    void shutdown(){KillTimer(window,1);KillTimer(window,2);UnregisterHotKey(window,1);stopCamera();camera.reset();hide();SetThreadExecutionState(ES_CONTINUOUS);{std::lock_guard lock(mutex);stop=true;wake.notify_one();}if(renderer.joinable())renderer.join();wallpaper.reset();if(power)UnregisterPowerSettingNotification(power);WTSUnRegisterSessionNotification(window);Shell_NotifyIconW(NIM_DELETE,&tray);if(settingsWindow)DestroyWindow(settingsWindow);if(overlay)DestroyWindow(overlay);DeleteObject(font);DeleteObject(background);DeleteObject(surface);
        if(smoke){std::filesystem::create_directories(L"artifacts");std::ofstream log(L"artifacts/smoke-test.txt");log<<((testEffect?presents>0:captures==0&&presents==0)?"PASS":"FAIL")<<" captures="<<captures<<" presented="<<presents<<"; virtual hinge explicitly requested="<<testEffect<<"\n";if(testEffect)log<<"Live FPS="<<(presents-1)/std::max(.001,lastFrame-firstFrame)<<"; capture/render/present ms="<<captureTotal/std::max(1,presents)<<"/"<<renderTotal/std::max(1,presents)<<"/"<<presentTotal/std::max(1,presents)<<"\n";log<<"Settings activations="<<activations<<"\n";log<<"No settings, startup, wallpaper or lock side effects in smoke mode.\n";}
        PostQuitMessage(0);
    }
};
LRESULT CALLBACK SettingsProc(HWND hwnd,UINT message,WPARAM w,LPARAM l){auto app=reinterpret_cast<App*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));if(message==WM_NCCREATE){app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}
    if(app)try{
        if(message==WM_MEASUREITEM){auto item=reinterpret_cast<MEASUREITEMSTRUCT*>(l);item->itemHeight=MulDiv(22,GetDpiForWindow(hwnd),96);return TRUE;}
        if(message==WM_CTLCOLORDLG||message==WM_CTLCOLORSTATIC||message==WM_CTLCOLORBTN||message==WM_CTLCOLORLISTBOX){auto dc=reinterpret_cast<HDC>(w);SetTextColor(dc,RGB(235,235,238));SetBkColor(dc,RGB(24,24,26));return reinterpret_cast<LRESULT>(app->background);}
        if(message==WM_DRAWITEM){auto item=reinterpret_cast<DRAWITEMSTRUCT*>(l);
            if(item->CtlType==ODT_COMBOBOX){FillRect(item->hDC,&item->rcItem,app->surface);SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,(item->itemState&ODS_DISABLED)?RGB(145,145,152):RGB(235,235,238));if(item->itemID!=UINT(-1)){auto len=SendMessageW(item->hwndItem,CB_GETLBTEXTLEN,item->itemID,0);if(len>=0&&len<4096){std::wstring text(size_t(len)+1,L' ');SendMessageW(item->hwndItem,CB_GETLBTEXT,item->itemID,reinterpret_cast<LPARAM>(text.data()));auto rect=item->rcItem;rect.left+=5;DrawTextW(item->hDC,text.c_str(),-1,&rect,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);}}if(item->itemState&ODS_FOCUS)DrawFocusRect(item->hDC,&item->rcItem);return TRUE;}
            if(item->CtlType==ODT_BUTTON){bool checkbox=item->CtlID!=Quit&&item->CtlID!=CalibrateOpen&&item->CtlID!=CalibrateClosed;FillRect(item->hDC,&item->rcItem,checkbox?app->background:app->surface);SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,(item->itemState&ODS_DISABLED)?RGB(125,125,132):RGB(235,235,238));RECT text=item->rcItem;
            if(checkbox){RECT box{text.left+2,text.top+6,text.left+16,text.top+20};FrameRect(item->hDC,&box,reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));bool checked=item->CtlID==Enable?app->prefs.enabled:item->CtlID==LockBlur?app->prefs.lockBlur:item->CtlID==Startup?app->prefs.startup:item->CtlID==HoldAwake?app->prefs.holdAwake:app->prefs.webcam;if(checked){InflateRect(&box,-3,-3);FillRect(item->hDC,&box,reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));}text.left+=26;}
            wchar_t label[180]{};GetWindowTextW(item->hwndItem,label,180);DrawTextW(item->hDC,label,-1,&text,DT_SINGLELINE|DT_VCENTER|(checkbox?DT_LEFT:DT_CENTER));if(item->itemState&ODS_FOCUS){RECT focus=item->rcItem;InflateRect(&focus,-2,-2);DrawFocusRect(item->hDC,&focus);}return TRUE;}}
        if(message==WM_COMMAND){app->command(LOWORD(w),HIWORD(w));return 0;}if(message==WM_CLOSE){ShowWindow(hwnd,SW_HIDE);return 0;}if(message==WM_DPICHANGED){auto r=reinterpret_cast<RECT*>(l);SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER);return 0;}}catch(...){app->report(L"Could not apply setting.");app->sync();}return DefWindowProcW(hwnd,message,w,l);
}
void App::openSettings(){++activations;if(!settingsWindow){WNDCLASSW cls{};cls.lpfnWndProc=SettingsProc;cls.hInstance=GetModuleHandleW(nullptr);cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=background;cls.lpszClassName=L"WinDuo.Native.Settings";RegisterClassW(&cls);
        int dpi=GetDpiForWindow(window);auto scale=[&](int n){return MulDiv(n,dpi,96);};RECT rect{0,0,scale(430),scale(470)};AdjustWindowRectExForDpi(&rect,WS_CAPTION|WS_SYSMENU,FALSE,WS_EX_TOOLWINDOW,dpi);
        settingsWindow=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_CONTROLPARENT,cls.lpszClassName,L"WinDuo Settings",WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,window,nullptr,cls.hInstance,this);
        BOOL dark=TRUE;DwmSetWindowAttribute(settingsWindow,20,&dark,sizeof(dark));if(!smoke)SetWindowDisplayAffinity(settingsWindow,WDA_EXCLUDEFROMCAPTURE);
        auto control=[&](const wchar_t* kind,const wchar_t* text,int id,int x,int y,int width,int height,DWORD style){auto c=CreateWindowExW(0,kind,text,WS_CHILD|WS_VISIBLE|style,scale(x),scale(y),scale(width),scale(height),settingsWindow,reinterpret_cast<HMENU>(INT_PTR(id)),cls.hInstance,nullptr);SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);SetWindowTheme(c,L"",L"");return c;};
        control(L"BUTTON",L"Enable WinDuo",Enable,20,16,390,28,BS_AUTOCHECKBOX|WS_TABSTOP);
        control(L"STATIC",L"Strength",0,20,56,100,24,0);auto combo=control(L"COMBOBOX",L"",Strength,140,52,160,120,CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_TABSTOP);SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Slight"));SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Normal"));
        control(L"BUTTON",L"Lock screen blur",LockBlur,20,90,390,28,BS_AUTOCHECKBOX|WS_TABSTOP);
        control(L"BUTTON",L"Hold awake ~1.5s on lid close",HoldAwake,20,124,390,28,BS_AUTOCHECKBOX|WS_TABSTOP);
        control(L"BUTTON",L"Start with Windows",Startup,20,158,390,28,BS_AUTOCHECKBOX|WS_TABSTOP);
        control(L"BUTTON",L"Experimental (cursed) webcam hinge",WebcamId,20,196,390,28,BS_AUTOCHECKBOX|WS_TABSTOP);
        control(L"COMBOBOX",L"",CameraId,44,232,366,180,CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_TABSTOP);
        control(L"BUTTON",L"Calibrate open",CalibrateOpen,44,270,146,30,BS_OWNERDRAW|WS_TABSTOP);
        control(L"BUTTON",L"Calibrate almost closed",CalibrateClosed,202,270,208,30,BS_OWNERDRAW|WS_TABSTOP);
        control(L"STATIC",L"LED on = camera on. Frames stay in RAM.\nCamera stops after 3s idle; calibrate to restart.",0,44,311,370,38,0);
        control(L"STATIC",L"Ctrl+Alt+Space or tray click: close / open",0,20,360,390,24,0);
        control(L"BUTTON",L"Quit",Quit,20,398,88,30,BS_OWNERDRAW|WS_TABSTOP);
        control(L"STATIC",L"",Status,122,398,288,60,0);fillCameras();
    }sync();ShowWindow(settingsWindow,SW_SHOW);SetForegroundWindow(settingsWindow);SetFocus(GetDlgItem(settingsWindow,Enable));}
LRESULT CALLBACK AppProc(HWND hwnd,UINT message,WPARAM w,LPARAM l){auto app=reinterpret_cast<App*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));if(message==WM_NCCREATE){app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}if(!app)return DefWindowProcW(hwnd,message,w,l);
    try{
        if(message==app->taskbarCreated){app->addTray();return 0;}
        switch(message){case WM_HOTKEY:if(w==1)app->toggleHinge();return 0;case WM_TIMER:if(w==2){KillTimer(hwnd,2);app->toggleHinge();}else app->tick();return 0;case FrameMessage:app->present();return 0;case ActivateMessage:app->openSettings();return 0;
        case WM_COMMAND:app->command(LOWORD(w),HIWORD(w));return 0;
        case WM_APP+3:app->report(L"Lock image unavailable; recovery retained if needed.");return 0;
        case TrayMessage:if(l==WM_LBUTTONDOWN)SetTimer(hwnd,2,GetDoubleClickTime(),nullptr);else if(l==WM_LBUTTONDBLCLK){KillTimer(hwnd,2);app->openSettings();}else if(l==WM_RBUTTONUP||l==WM_CONTEXTMENU){KillTimer(hwnd,2);app->menu();}return 0;
        case WM_DISPLAYCHANGE:app->detect();return 0;
        case WM_WTSSESSION_CHANGE:if(w==WTS_SESSION_LOCK){app->locked=true;app->curve.clear();app->lid.reset();app->manual=false;app->stopCamera();app->hide();SetThreadExecutionState(ES_CONTINUOUS);if(app->prefs.enabled&&app->prefs.lockBlur&&!app->cached.pixels.empty()&&app->wallpaper)app->wallpaper->requestApply(app->cached,app->prefs.normal);}else if(w==WTS_SESSION_UNLOCK){app->locked=false;app->reset();}return 0;
        case WM_POWERBROADCAST:
            if(w==PBT_POWERSETTINGCHANGE){auto setting=reinterpret_cast<POWERBROADCAST_SETTING*>(l);if(setting&&setting->PowerSetting==LidGuid&&setting->DataLength==sizeof(DWORD)){DWORD open;memcpy(&open,setting->Data,sizeof(open));if(open<=1){bool opened=app->lid.receive(open,now());app->report(open?L"Windows lid state: open (no angle available).":L"Windows lid state: closed.");if(!open)app->stopCamera();if(app->locked||app->suspended||!app->prefs.enabled){app->lid.pending=0;}else{if(opened){app->manual=false;++app->epoch;app->progress(0);app->restore();}if(app->lid.closeReady(now())){app->manual=false;app->progress(1,true);}}}}}
            else if(w==PBT_APMSUSPEND){app->suspended=true;app->reset();}else if(w==PBT_APMRESUMEAUTOMATIC||w==PBT_APMRESUMESUSPEND){app->suspended=false;app->detect();}return TRUE;
        case WM_CLOSE:DestroyWindow(hwnd);return 0;case WM_DESTROY:app->shutdown();return 0;
        }
    }catch(...){app->reset();app->prefs.enabled=false;app->report(L"WinDuo paused after an operating system error.");app->sync();}
    return DefWindowProcW(hwnd,message,w,l);
}
int selfTest(){std::filesystem::create_directories(L"artifacts");std::ofstream log(L"artifacts/self-test.txt");int failed=0;auto test=[&](bool ok,const char* text){log<<(ok?"PASS ":"FAIL ")<<text<<"\n";if(!ok)++failed;};
    LidInput input;input.receive(0,1);test(!input.closeReady(2),"Initial closed notification never triggers blur");input.receive(1,3);input.receive(0,4);input.receive(1,4.05);test(!input.closeReady(5),"Open cancels an unconsumed close event");input.receive(0,6);test(input.closeReady(6),"Close starts immediately without extra debounce delay");test(input.state==0,"Closed baseline persists until lid opens");input.reset();input.receive(0,9);test(!input.closeReady(10),"Unknown initial state never triggers close");input.receive(1,11);input.receive(999,12);test(!input.closeReady(13),"Invalid lid payload is ignored");
    App app;app.smoke=true;app.lid.receive(1,1);app.reset();app.lid.receive(0,2);test(app.lid.closeReady(2),"Pause/reset preserves open baseline for next real close");
    test(Curve::smooth(-1)==0&&Curve::smooth(2)==1,"Smoothstep bounds");Curve c;c.set(1,0,.6);test(std::abs(c.value(.3)-.5)<1e-6,"Close midpoint");c.set(0,.3,.4);test(std::abs(c.value(.3)-.5)<1e-6&&c.value(.71)==0,"Continuous reversal and open");c.clear();test(c.value(1)==0,"Immediate reset");Settings settings;test(settings.enabled&&settings.startup&&!settings.webcam&&!settings.lockBlur&&!settings.normal,"Accessory defaults");
    Frame frame(960,540);for(int y=0;y<frame.height;++y)for(int x=0;x<frame.width;++x)frame.pixels[size_t(y)*frame.width+x]=0xff000000|((x%20<6)?0:0xeeeeee);
    test(blur(frame,0,false).pixels==frame.pixels,"Zero strength preserves pixels");auto start=now();auto output=blur(frame,1,false);test(output.pixels[20*960+2]!=frame.pixels[20*960+2],"Gaussian softens stripes");test((output.pixels[20*960+2]&255)>(output.pixels[500*960+2]&255),"Blur tapers toward hinge");test(resize(frame,480).height==270,"Snapshot aspect ratio");test(hash({1,2,3})=="039058C6F2C0CB492C533B0A4D14EF77CC0F78ABCCCED5287D84A1A2011CFB81", "SHA256 recovery fingerprint");
    CameraSignal open{.7,.1},closed{.2,-.2};
    test(cameraProgress(open,open,closed)==0&&cameraProgress(closed,open,closed)==1,"Camera calibration endpoints");
    test(std::abs(cameraProgress({.45,-.05},open,closed)-.5)<1e-6,"Camera calibration midpoint");
    test(cameraProgress(open,open,open)==0,"Identical camera calibration cannot amplify noise");
    CameraFilter filter;filter.push(open,0);filter.push(open,.067);auto stable=filter.push(closed,.134);test(std::abs(stable.mean-open.mean)<1e-6,"Single camera spike rejected");
    GpuBlur gpu;auto gpuFrame=gpu.render(frame,1,false);test(gpuFrame.pixels[20*960+2]!=frame.pixels[20*960+2],"GPU/fallback Gaussian softens stripes");
    log<<"Renderer: "<<(gpu.usingGpu()?"Direct2D / D3D11":"CPU fallback")<<"\n";
    Frame opaque(64,64);std::fill(opaque.pixels.begin(),opaque.pixels.end(),0x00ffffff);auto white=gpu.render(opaque,1,false);test((white.pixels[32*64+32]&255)>220,"GDI zero-alpha input remains visible");
    gpu.clear();test(!gpu.usingGpu(),"GPU resources released at idle");
    HWND testWindow=CreateWindowExW(0,L"STATIC",L"",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);app.window=testWindow;app.bounds=RECT{0,0,960,540};app.toggleHinge();test(app.curve.target==1,"Manual hinge closes");app.toggleHinge();test(app.curve.target==0,"Manual hinge reverses immediately");app.prefs.enabled=false;app.toggleHinge();test(app.curve.target==0,"Disabled hinge ignores input");test(app.hold==0,"Virtual hinge does not request a lid sleep hold");app.prefs.enabled=true;app.prefs.holdAwake=false;SetWindowLongPtrW(testWindow,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(&app));
    AppProc(testWindow,WM_HOTKEY,1,0);test(app.curve.target==1,"Registered hotkey message routes to virtual hinge");
    struct {GUID id;DWORD size;DWORD value;} powerEvent{LidGuid,sizeof(DWORD),1};
    AppProc(testWindow,WM_POWERBROADCAST,PBT_POWERSETTINGCHANGE,reinterpret_cast<LPARAM>(&powerEvent));powerEvent.value=0;
    AppProc(testWindow,WM_POWERBROADCAST,PBT_POWERSETTINGCHANGE,reinterpret_cast<LPARAM>(&powerEvent));test(app.curve.target==1&&app.curve.duration==.5&&!app.manual&&app.hold==0,"ACPI close uses 500 ms easing and respects hold-off");powerEvent.value=1;
    AppProc(testWindow,WM_POWERBROADCAST,PBT_POWERSETTINGCHANGE,reinterpret_cast<LPARAM>(&powerEvent));test(app.curve.target==0&&app.curve.duration==.4,"ACPI open uses 400 ms easing");
    AppProc(testWindow,WM_TIMER,2,0);test(app.curve.target==1,"Tray single-click timer toggles virtual hinge");
    KillTimer(testWindow,1);KillTimer(testWindow,2);DestroyWindow(testWindow);DeleteObject(app.background);DeleteObject(app.surface);
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
