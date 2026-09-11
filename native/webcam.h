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
    if(!std::isfinite(v.mean)||!std::isfinite(v.delta)||!std::isfinite(a.mean)||!std::isfinite(a.delta)||!std::isfinite(b.mean)||!std::isfinite(b.delta))return 0.0;
    double x=b.mean-a.mean,y=b.delta-a.delta,den=x*x+y*y;
    if(den<.0004)return 0.0;
    double dx=v.mean-a.mean,dy=v.delta-a.delta;
    double proj=(dx*x+dy*y)/den;
    if(proj<=0.0)return 0.0;
    double perpX=dx-proj*x,perpY=dy-proj*y;
    double perpDistSq=perpX*perpX+perpY*perpY;
    double relPerpSq=perpDistSq/den;
    double relDiff=std::max(0.0,relPerpSq-0.18);
    double conf=1.0/(1.0+8.0*relDiff+24.0*relDiff*relDiff);
    double clamped=std::min(1.0,proj);
    return std::clamp(clamped*conf,0.0,1.0);
}
struct CameraFilter {
    CameraSignal history[5]{},value{};int count=0;double time=0;
    CameraSignal push(CameraSignal s,double t){
        history[count%5]=s;++count;
        if(count<3){value=s;time=t;return value;}
        auto median3=[](double a,double b,double c){return a+b+c-std::min({a,b,c})-std::max({a,b,c});};
        auto median5=[](double a,double b,double c,double d,double e){double arr[5]={a,b,c,d,e};std::sort(arr,arr+5);return arr[2];};
        CameraSignal m;
        if(count==3)m={median3(history[0].mean,history[1].mean,history[2].mean),median3(history[0].delta,history[1].delta,history[2].delta)};
        else if(count==4){
            double arrM[4]={history[0].mean,history[1].mean,history[2].mean,history[3].mean};std::sort(arrM,arrM+4);
            double arrD[4]={history[0].delta,history[1].delta,history[2].delta,history[3].delta};std::sort(arrD,arrD+4);
            double medM=(std::abs(arrM[1]-value.mean)<=std::abs(arrM[2]-value.mean))?arrM[1]:arrM[2];
            double medD=(std::abs(arrD[1]-value.delta)<=std::abs(arrD[2]-value.delta))?arrD[1]:arrD[2];
            m={medM,medD};
        }
        else m={median5(history[0].mean,history[1].mean,history[2].mean,history[3].mean,history[4].mean),median5(history[0].delta,history[1].delta,history[2].delta,history[3].delta,history[4].delta)};
        double dt=std::clamp(t-time,0.0,1.0);time=t;
        double diff=std::abs(m.mean-value.mean)+std::abs(m.delta-value.delta);
        double tau=(diff<0.06)?0.20:0.08;
        double alpha=(dt>0.0)?(1.0-std::exp(-dt/tau)):1.0;
        value.mean+=(m.mean-value.mean)*alpha;value.delta+=(m.delta-value.delta)*alpha;return value;
    }
};
struct CameraDevice {std::wstring id,name;bool front=false;};
class CameraCallback final: public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IMFSourceReaderCallback> {
public:
    std::mutex mutex;std::condition_variable wake;ComPtr<IMFSample> sample;HRESULT result=S_OK;bool ready=false;
    STDMETHODIMP OnReadSample(HRESULT hr,DWORD,DWORD flags,LONGLONG,IMFSample* s) override {
        std::lock_guard lock(mutex);
        if(flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            sample = s; ready = true; wake.notify_one(); return S_OK;
        }
        result=(flags&(MF_SOURCE_READERF_ERROR|MF_SOURCE_READERF_ENDOFSTREAM))?E_FAIL:hr;
        sample=s;ready=true;wake.notify_one();return S_OK;
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
                if(id.empty()&&!list.empty())id=list.front().id;
                if(id.empty())throw std::runtime_error("No video capture device detected.");
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
                ComPtr<IMFMediaType> nativeType;UINT32 nw=0,nh=0;
                if(SUCCEEDED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,&nativeType))){
                    MFGetAttributeSize(nativeType.Get(),MF_MT_FRAME_SIZE,&nw,&nh);
                }
                ComPtr<IMFMediaType> type;winrt::check_hresult(MFCreateMediaType(&type));type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12);
                MFSetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,320,180);MFSetAttributeRatio(type.Get(),MF_MT_FRAME_RATE,30,1);type->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);
                if(FAILED(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),nullptr,type.Get()))){
                    if(nw>0&&nh>0)MFSetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,nw,nh);
                    if(FAILED(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),nullptr,type.Get()))){
                        type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_YUY2);
                        if(FAILED(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),nullptr,type.Get()))){
                            type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32);
                            if(FAILED(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),nullptr,type.Get()))){
                                if(nativeType)reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),nullptr,nativeType.Get());
                            }
                        }
                    }
                }
                ComPtr<IMFMediaType> actual;winrt::check_hresult(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),&actual));UINT32 width=0,height=0;
                winrt::check_hresult(MFGetAttributeSize(actual.Get(),MF_MT_FRAME_SIZE,&width,&height));check(width>0&&height>2,L"Camera format unavailable.");
                GUID subtype=GUID_NULL;actual->GetGUID(MF_MT_SUBTYPE,&subtype);
                CameraFilter filter;double next=now();uint32_t lastChecksum=0;int staticFrames=0,timeouts=0;
                for(;;){
                    {std::unique_lock lock(mutex);if(wake.wait_for(lock,std::chrono::duration<double>(std::max(0.0,next-now())),[&]{return exit||!wanted||generation!=version;}))break;}
                    double deadline=now()+3;winrt::check_hresult(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,nullptr,nullptr,nullptr,nullptr));
                    ComPtr<IMFSample> sample;bool cancelled=false,timedOut=false;
                    for(;;){
                        {std::unique_lock lock(callback->mutex);if(callback->wake.wait_for(lock,std::chrono::milliseconds(50),[&]{return callback->ready;})){callback->ready=false;winrt::check_hresult(callback->result);sample=std::move(callback->sample);break;}}
                        {std::lock_guard lock(mutex);cancelled=exit||!wanted||generation!=version;}if(cancelled)break;
                        if(now()>deadline){if(++timeouts>2)throw std::runtime_error("Camera did not deliver a frame.");timedOut=true;break;}
                    }
                    if(cancelled)break;if(timedOut)continue;timeouts=0;next=now()+1.0/30;if(!sample)continue;
                    ComPtr<IMFMediaBuffer> buffer;winrt::check_hresult(sample->ConvertToContiguousBuffer(&buffer));
                    BYTE* data=nullptr;DWORD length=0;winrt::check_hresult(buffer->Lock(&data,nullptr,&length));
                    UINT32 rawStride=width;
                    if(FAILED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE,&rawStride))){
                        if(subtype==MFVideoFormat_YUY2)rawStride=width*2;
                        else if(subtype==MFVideoFormat_RGB32)rawStride=width*4;
                        else rawStride=width;
                    }
                    LONG stride=static_cast<LONG>(rawStride);
                    LONG minStride=(subtype==MFVideoFormat_YUY2)?LONG(width*2):(subtype==MFVideoFormat_RGB32)?LONG(width*4):LONG(width);
                    size_t absStride=size_t(std::abs(stride));
                    if(absStride<size_t(minStride)||absStride*height>length){buffer->Unlock();throw std::runtime_error("Unsupported camera stride.");}
                    double total=0,totalCount=0;
                    double tl=0,tlC=0,bl=0,blC=0;
                    double tr=0,trC=0,br=0,brC=0;
                    double tc=0,tcC=0,bc=0,bcC=0;
                    unsigned third=height/3,splitL=width*35/100,splitR=width*65/100;
                    unsigned stepX=std::max(1u,width/160u),stepY=std::max(1u,height/90u);
                    uint32_t checksum=0;
                    for(unsigned y=0;y<height;y+=stepY){
                        const BYTE* row=(stride>=0)?(data+size_t(y)*absStride):(data+size_t(height-1-y)*absStride);
                        bool isTop=(y<third),isBottom=(y>=height-third);
                        for(unsigned x=0;x<width;x+=stepX){
                            double v=0;
                            if(subtype==MFVideoFormat_YUY2){v=row[x*2]/255.0;}
                            else if(subtype==MFVideoFormat_RGB32){const BYTE* px=row+x*4;v=(px[0]*29+px[1]*150+px[2]*77)/(256.0*255.0);}
                            else{v=row[x]/255.0;}
                            checksum=checksum*31+static_cast<uint32_t>(v*255.0);
                            total+=v;totalCount+=1.0;
                            if(x<splitL){
                                if(isTop){tl+=v;tlC+=1.0;}
                                if(isBottom){bl+=v;blC+=1.0;}
                            }else if(x>=splitR){
                                if(isTop){tr+=v;trC+=1.0;}
                                if(isBottom){br+=v;brC+=1.0;}
                            }else{
                                if(isTop){tc+=v;tcC+=1.0;}
                                if(isBottom){bc+=v;bcC+=1.0;}
                            }
                        }
                    }
                    buffer->Unlock();
                    if(checksum==lastChecksum){if(++staticFrames>45)state=L"Camera stream static or frozen";}
                    else{staticFrames=0;lastChecksum=checksum;}
                    double meanVal=totalCount>0?(total/totalCount):0.0;
                    double topL=tlC>0?(tl/tlC):0.0,botL=blC>0?(bl/blC):0.0;
                    double topR=trC>0?(tr/trC):0.0,botR=brC>0?(br/brC):0.0;
                    double topC=tcC>0?(tc/tcC):0.0,botC=bcC>0?(bc/bcC):0.0;
                    double deltaL=(botL-topL)/(botL+topL+0.04);
                    double deltaR=(botR-topR)/(botR+topR+0.04);
                    double asym=std::abs(deltaL-deltaR);
                    double sym=1.0/(1.0+3.0*asym);
                    double periDelta=(deltaL+deltaR)*0.5*sym;
                    double centerDelta=(botC-topC)/(botC+topC+0.04);
                    double deltaVal=periDelta*0.80+centerDelta*0.20;
                    auto value=filter.push({meanVal,deltaVal},now());
                    {std::lock_guard lock(mutex);latest=value;stamp=now();if(staticFrames<=45)state=meanVal<0.025?L"Camera blocked or privacy shutter closed":L"Camera on - LED on; frames stay in RAM";}
                }
                reader->Flush(DWORD(MF_SOURCE_READER_ALL_STREAMS));
            }catch(...){std::lock_guard lock(mutex);if(generation==version&&wanted){wanted=false;state=L"Camera unavailable. Check privacy switch or permissions.";}}
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
