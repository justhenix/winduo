#pragma once
#include "core.h"
namespace wd {
template<class Function> void rows(int count,Function function){
    std::exception_ptr error;std::mutex errorMutex;
    auto run=[&](int part){try{for(int y=count*part/4;y<count*(part+1)/4;++y)function(y);}catch(...){std::lock_guard lock(errorMutex);error=std::current_exception();}};
    std::thread a([&]{run(0);}),b([&]{run(1);}),c([&]{run(2);});run(3);a.join();b.join();c.join();if(error)std::rethrow_exception(error);
}
struct Dib {
    HDC dc{};HBITMAP bitmap{};HGDIOBJ previous{};uint32_t* bits{};int width{},height{};
    Dib(int w,int h):width(w),height(h){
        dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),w,-h,1,32,BI_RGB};
        bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&bits),nullptr,0);
        if(!bitmap){DeleteDC(dc);throw std::runtime_error("Cannot allocate display buffer.");}previous=SelectObject(dc,bitmap);
    }
    ~Dib(){SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);}
    Dib(const Dib&)=delete;Dib& operator=(const Dib&)=delete;
};
inline Frame resize(const Frame& f,int width){
    width=std::min(width,f.width);int height=std::max(1,f.height*width/f.width);Frame result(width,height);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){
        double sx=(x+.5)*f.width/width-.5,sy=(y+.5)*f.height/height-.5;
        int x0=int(sx),y0=int(sy),x1=std::min(x0+1,f.width-1),y1=std::min(y0+1,f.height-1),fx=int((sx-x0)*256),fy=int((sy-y0)*256);
        uint32_t pixel=0xff000000,a=f.pixels[size_t(y0)*f.width+x0],b=f.pixels[size_t(y0)*f.width+x1],c=f.pixels[size_t(y1)*f.width+x0],d=f.pixels[size_t(y1)*f.width+x1];
        for(int channel=0;channel<3;++channel){int shift=channel*8;int upper=int((a>>shift)&255)*(256-fx)+int((b>>shift)&255)*fx;int lower=int((c>>shift)&255)*(256-fx)+int((d>>shift)&255)*fx;pixel|=uint32_t((upper*(256-fy)+lower*fy)>>16)<<shift;}
        result.pixels[size_t(y)*width+x]=pixel;
    }
    return result;
}
inline Frame blur(const Frame& source,double progress,int strength){
    progress=std::clamp(progress,0.0,1.0);if(progress==0)return source;
    auto small=resize(source,320);auto horizontal=small;auto soft=small;
    double sigma=blurSigma(strength)*source.height/1080.0*small.width/source.width*progress;
    rows(small.height,[&](int y){
        double spread=std::max(.25,sigma*(1-.35*y/std::max(1,small.height-1)));int radius=int(std::ceil(3*spread));
        std::vector<double> kernel(2*radius+1);double sum=0;
        for(int k=-radius;k<=radius;++k)sum+=(kernel[k+radius]=std::exp(-k*k/(2*spread*spread)));
        for(auto& v:kernel)v/=sum;
        for(int x=0;x<small.width;++x){uint32_t pixel=0xff000000;
            for(int c=0;c<3;++c){double value=0;for(int k=-radius;k<=radius;++k)value+=((small.pixels[size_t(y)*small.width+std::clamp(x+k,0,small.width-1)]>>(c*8))&255)*kernel[k+radius];pixel|=uint32_t(std::round(value))<<(c*8);}
            horizontal.pixels[size_t(y)*small.width+x]=pixel;
        }
    });
    rows(small.height,[&](int y){
        double spread=std::max(.25,sigma*(1-.35*y/std::max(1,small.height-1)));int radius=int(std::ceil(3*spread));std::vector<double> kernel(2*radius+1);double sum=0;
        for(int k=-radius;k<=radius;++k)sum+=(kernel[k+radius]=std::exp(-k*k/(2*spread*spread)));for(auto& v:kernel)v/=sum;
        for(int x=0;x<small.width;++x){uint32_t pixel=0xff000000;for(int c=0;c<3;++c){double value=0;for(int k=-radius;k<=radius;++k)value+=((horizontal.pixels[size_t(std::clamp(y+k,0,small.height-1))*small.width+x]>>(c*8))&255)*kernel[k+radius];pixel|=uint32_t(std::round(value))<<(c*8);}soft.pixels[size_t(y)*small.width+x]=pixel;}
    });
    Frame result(source.width,source.height);int weight=int(256*progress),brightness=int(256*(1-blurDim(strength)*progress));
    std::vector<int> columns(source.width),fractions(source.width);
    for(int x=0;x<source.width;++x){double sx=std::clamp((x+.5)*soft.width/source.width-.5,0.0,double(soft.width-1));columns[x]=int(sx);fractions[x]=int((sx-columns[x])*256);}
    rows(source.height,[&](int y){
        double sy=std::clamp((y+.5)*soft.height/source.height-.5,0.0,double(soft.height-1));int iy=int(sy),fy=int((sy-iy)*256);
        for(int x=0;x<source.width;++x){int left=columns[x],right=std::min(left+1,soft.width-1),fx=fractions[x];uint32_t pixel=0xff000000;
            auto a=soft.pixels[size_t(iy)*soft.width+left],b=soft.pixels[size_t(iy)*soft.width+right],c=soft.pixels[size_t(std::min(iy+1,soft.height-1))*soft.width+left],d=soft.pixels[size_t(std::min(iy+1,soft.height-1))*soft.width+right],original=source.pixels[size_t(y)*source.width+x];
            for(int channel=0;channel<3;++channel){int shift=channel*8;int top=int((a>>shift)&255)*(256-fx)+int((b>>shift)&255)*fx;int bottom=int((c>>shift)&255)*(256-fx)+int((d>>shift)&255)*fx;int sample=(top*(256-fy)+bottom*fy)>>16;pixel|=uint32_t(((int((original>>shift)&255)*(256-weight)+sample*weight)*brightness)>>16)<<shift;}
            result.pixels[size_t(y)*source.width+x]=pixel;
        }
    });return result;
}
class Capture {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGIOutputDuplication> duplicate;ComPtr<ID3D11Texture2D> staging;
    Frame last;RECT area{};double retry=0;
    void initialize(RECT r){
        ComPtr<IDXGIFactory1> factory;winrt::check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        for(UINT a=0;;++a){ComPtr<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(a,&adapter)==DXGI_ERROR_NOT_FOUND)break;
            for(UINT o=0;;++o){ComPtr<IDXGIOutput> output;if(adapter->EnumOutputs(o,&output)==DXGI_ERROR_NOT_FOUND)break;DXGI_OUTPUT_DESC desc{};output->GetDesc(&desc);if(!sameRect(desc.DesktopCoordinates,r))continue;
                if(desc.Rotation!=DXGI_MODE_ROTATION_IDENTITY&&desc.Rotation!=DXGI_MODE_ROTATION_UNSPECIFIED)throw std::runtime_error("Rotated capture uses GDI.");
                winrt::check_hresult(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
                ComPtr<IDXGIOutput1> modern;winrt::check_hresult(output.As(&modern));winrt::check_hresult(modern->DuplicateOutput(device.Get(),&duplicate));
                D3D11_TEXTURE2D_DESC texture{};texture.Width=r.right-r.left;texture.Height=r.bottom-r.top;texture.MipLevels=texture.ArraySize=1;texture.Format=DXGI_FORMAT_B8G8R8A8_UNORM;texture.SampleDesc.Count=1;texture.Usage=D3D11_USAGE_STAGING;texture.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                winrt::check_hresult(device->CreateTexture2D(&texture,nullptr,&staging));return;
            }
        }throw std::runtime_error("No duplication output.");
    }
public:
    void clear(){staging.Reset();duplicate.Reset();context.Reset();device.Reset();last={};}
    Frame get(RECT r){
        if(!sameRect(r,area)){clear();area=r;retry=0;}
        try{
            if(!duplicate&&now()>=retry)initialize(r);
            if(duplicate){DXGI_OUTDUPL_FRAME_INFO info{};ComPtr<IDXGIResource> resource;HRESULT hr=duplicate->AcquireNextFrame(0,&info,&resource);
                if(hr==DXGI_ERROR_WAIT_TIMEOUT){if(!last.pixels.empty())return last;}
                else{winrt::check_hresult(hr);try{ComPtr<ID3D11Texture2D> source;winrt::check_hresult(resource.As(&source));context->CopyResource(staging.Get(),source.Get());D3D11_MAPPED_SUBRESOURCE mapped{};winrt::check_hresult(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
                    try {Frame result(r.right-r.left,r.bottom-r.top);for(int y=0;y<result.height;++y)memcpy(result.pixels.data()+size_t(y)*result.width,static_cast<BYTE*>(mapped.pData)+size_t(y)*mapped.RowPitch,size_t(result.width)*4);last=std::move(result);}
                    catch(...){context->Unmap(staging.Get(),0);throw;}context->Unmap(staging.Get(),0);
                }catch(...){duplicate->ReleaseFrame();throw;}duplicate->ReleaseFrame();return last;}
            }
        }catch(...){clear();retry=now()+5;}
        Dib dib(r.right-r.left,r.bottom-r.top);auto dc=GetDC(nullptr);BOOL ok=BitBlt(dib.dc,0,0,dib.width,dib.height,dc,r.left,r.top,SRCCOPY);ReleaseDC(nullptr,dc);check(ok,L"Desktop capture failed.");Frame frame(dib.width,dib.height);memcpy(frame.pixels.data(),dib.bits,frame.pixels.size()*4);return frame;
    }
};
}
