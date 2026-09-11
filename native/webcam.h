#pragma once
#include "core.h"
#include <mfapi.h>
#include <wrl/implements.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <winrt/Windows.Devices.Enumeration.h>
namespace wd {
struct CameraSignal {double mean=0,delta=0;};
inline double cameraProgress(CameraSignal v,CameraSignal a,CameraSignal b){
    double x=b.mean-a.mean,y=b.delta-a.delta,den=x*x+y*y;
    return den<.0004?0:std::clamp(((v.mean-a.mean)*x+(v.delta-a.delta)*y)/den,0.0,1.0);
}
struct CameraFilter {
    CameraSignal history[3]{},value{};int count=0;double time=0;
    CameraSignal push(CameraSignal s,double t){history[count++%3]=s;if(count<3){value=s;time=t;return value;}
        auto median=[](double a,double b,double c){return a+b+c-std::min({a,b,c})-std::max({a,b,c});};
        CameraSignal m{median(history[0].mean,history[1].mean,history[2].mean),median(history[0].delta,history[1].delta,history[2].delta)};
        double alpha=1-std::exp(-std::clamp(t-time,0.0,1.0)/.25);time=t;
        value.mean+=(m.mean-value.mean)*alpha;value.delta+=(m.delta-value.delta)*alpha;return value;
    }
};
struct CameraDevice {std::wstring id,name;bool front=false;};
class CameraCallback final: public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IMFSourceReaderCallback> {
public:
    std::mutex mutex;std::condition_variable wake;ComPtr<IMFSample> sample;HRESULT result=S_OK;bool ready=false;
    STDMETHODIMP OnReadSample(HRESULT hr,DWORD,DWORD flags,LONGLONG,IMFSample* s) override {
        std::lock_guard lock(mutex);result=(flags&(MF_SOURCE_READERF_ERROR|MF_SOURCE_READERF_ENDOFSTREAM|MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED))?E_FAIL:hr;sample=s;ready=true;wake.notify_one();return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD) override {return S_OK;}
    STDMETHODIMP OnEvent(DWORD,IMFMediaEvent*) override {return S_OK;}
};
class Webcam {
    std::mutex mutex;std::condition_variable wake;std::thread worker;
    bool exit=false,wanted=false;unsigned generation=0;Settings config;
    CameraSignal latest{};double stamp=0;std::vector<CameraDevice> devices;std::wstring state=L"Camera off";
    void loop(){
        winrt::init_apartment();
        // Windows N may lack Media Foundation; keep the virtual hinge available.
        HMODULE modules[3]{LoadLibraryExW(L"mf.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32),LoadLibraryExW(L"mfplat.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32),LoadLibraryExW(L"mfreadwrite.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32)};
        HRESULT mf=modules[0]&&modules[1]&&modules[2]?MFStartup(MF_VERSION,MFSTARTUP_LITE):HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        for(;;){Settings selected;unsigned version;
            {std::unique_lock lock(mutex);wake.wait(lock,[&]{return exit||wanted;});if(exit)break;selected=config;version=generation;state=L"Opening camera...";}
            ComPtr<IMFMediaSource> source;
            try {
                winrt::check_hresult(mf);
                using namespace winrt::Windows::Devices::Enumeration;
                auto found=DeviceInformation::FindAllAsync(DeviceClass::VideoCapture).get();std::vector<CameraDevice> list;
                for(auto const& d:found){auto location=d.EnclosureLocation();list.push_back({d.Id().c_str(),d.Name().c_str(),location&&location.Panel()==Panel::Front});}
                std::wstring id=selected.camera;
                {std::lock_guard lock(mutex);devices=list;}
                if(id.empty())for(auto const& d:list)if(d.front){id=d.id;break;}
                if(id.empty())throw std::runtime_error("Select a camera in Settings; no front camera identified.");
                ComPtr<IMFAttributes> attributes;winrt::check_hresult(MFCreateAttributes(&attributes,2));
                winrt::check_hresult(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));
                winrt::check_hresult(attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,id.c_str()));
                winrt::check_hresult(MFCreateDeviceSource(attributes.Get(),&source));
                auto callback=Microsoft::WRL::Make<CameraCallback>();ComPtr<IMFAttributes> options;winrt::check_hresult(MFCreateAttributes(&options,3));
                winrt::check_hresult(options->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK,callback.Get()));
                winrt::check_hresult(options->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,TRUE));
                winrt::check_hresult(options->SetUINT32(MF_LOW_LATENCY,TRUE));
                ComPtr<IMFSourceReader> reader;winrt::check_hresult(MFCreateSourceReaderFromMediaSource(source.Get(),options.Get(),&reader));
                winrt::check_hresult(reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS),FALSE));winrt::check_hresult(reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),TRUE));
                ComPtr<IMFMediaType> type;winrt::check_hresult(MFCreateMediaType(&type));type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12);
                MFSetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,320,180);MFSetAttributeRatio(type.Get(),MF_MT_FRAME_RATE,15,1);type->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);
                winrt::check_hresult(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),nullptr,type.Get()));
                ComPtr<IMFMediaType> actual;winrt::check_hresult(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),&actual));UINT32 width=0,height=0;
                winrt::check_hresult(MFGetAttributeSize(actual.Get(),MF_MT_FRAME_SIZE,&width,&height));check(width<=320&&height<=180&&width>0&&height>2,L"Camera format unavailable.");UINT32 rate=0,denominator=0;winrt::check_hresult(MFGetAttributeRatio(actual.Get(),MF_MT_FRAME_RATE,&rate,&denominator));check(denominator>0&&double(rate)/denominator<=15.01,L"Camera frame rate unavailable.");
                CameraFilter filter;double next=now();
                for(;;){
                    {std::unique_lock lock(mutex);if(wake.wait_for(lock,std::chrono::duration<double>(std::max(0.0,next-now())),[&]{return exit||!wanted||generation!=version;}))break;}
                    double deadline=now()+3;winrt::check_hresult(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,nullptr,nullptr,nullptr,nullptr));
                    ComPtr<IMFSample> sample;bool cancelled=false;
                    for(;;){
                        {std::unique_lock lock(callback->mutex);if(callback->wake.wait_for(lock,std::chrono::milliseconds(50),[&]{return callback->ready;})){callback->ready=false;winrt::check_hresult(callback->result);sample=std::move(callback->sample);break;}}
                        {std::lock_guard lock(mutex);cancelled=exit||!wanted||generation!=version;}if(cancelled)break;if(now()>deadline)throw std::runtime_error("Camera did not deliver a frame.");
                    }
                    if(cancelled)break;next=now()+1.0/15;if(!sample)continue;
                    ComPtr<IMFMediaBuffer> buffer;winrt::check_hresult(sample->ConvertToContiguousBuffer(&buffer));
                    BYTE* data=nullptr;DWORD length=0;winrt::check_hresult(buffer->Lock(&data,nullptr,&length));
                    UINT32 rawStride=width;actual->GetUINT32(MF_MT_DEFAULT_STRIDE,&rawStride);LONG stride=static_cast<LONG>(rawStride);
                    if(stride<LONG(width)||size_t(stride)*height>length){buffer->Unlock();throw std::runtime_error("Unsupported camera stride.");}
                    double total=0,top=0,bottom=0;unsigned third=height/3;
                    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){double v=data[size_t(y)*stride+x]/255.0;total+=v;if(y<third)top+=v;if(y>=height-third)bottom+=v;}
                    buffer->Unlock();auto value=filter.push({total/(width*height),(bottom-top)/(width*third)},now());
                    {std::lock_guard lock(mutex);latest=value;stamp=now();state=L"Camera on - LED on; frames stay in RAM";}
                }
                reader->Flush(DWORD(MF_SOURCE_READER_ALL_STREAMS));
            }catch(...){std::lock_guard lock(mutex);if(generation==version&&wanted){wanted=false;state=L"Camera unavailable. Select a camera or use the hotkey.";}}
            if(source)source->Shutdown();
            {std::lock_guard lock(mutex);if(!wanted&&state.find(L"unavailable")==std::wstring::npos)state=L"Camera stopped";}
        }
        if(SUCCEEDED(mf))MFShutdown();for(auto module:modules)if(module)FreeLibrary(module);winrt::uninit_apartment();
    }
public:
    Webcam(){worker=std::thread([this]{loop();});}
    ~Webcam(){{std::lock_guard lock(mutex);exit=true;wanted=false;wake.notify_all();}worker.join();}
    void start(const Settings& settings){std::lock_guard lock(mutex);config=settings;wanted=true;stamp=0;++generation;wake.notify_all();}
    void stop(){std::lock_guard lock(mutex);wanted=false;++generation;wake.notify_all();}
    bool read(CameraSignal& signal,double& timestamp,std::wstring& text,std::vector<CameraDevice>& list){std::lock_guard lock(mutex);signal=latest;timestamp=stamp;text=state;list=devices;return wanted;}
};
}
