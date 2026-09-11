#pragma once
#include "engine.h"
#include <d2d1_1.h>
#include <d2d1effects.h>
namespace wd {
class GpuBlur {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID2D1Factory1> factory;ComPtr<ID2D1Device> d2d;ComPtr<ID2D1DeviceContext> draw;
    ComPtr<ID3D11Texture2D> target,staging;ComPtr<ID2D1Bitmap1> bitmap,input;ComPtr<ID2D1Effect> gaussian;
    int width=0,height=0;bool failed=false;
    void initialize(int w,int h){
        winrt::check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        D2D1_FACTORY_OPTIONS options{};winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,__uuidof(ID2D1Factory1),&options,reinterpret_cast<void**>(factory.GetAddressOf())));
        ComPtr<IDXGIDevice> dxgi;winrt::check_hresult(device.As(&dxgi));winrt::check_hresult(factory->CreateDevice(dxgi.Get(),&d2d));winrt::check_hresult(d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&draw));
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        winrt::check_hresult(device->CreateTexture2D(&desc,nullptr,&target));desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;winrt::check_hresult(device->CreateTexture2D(&desc,nullptr,&staging));
        ComPtr<IDXGISurface> surface;winrt::check_hresult(target.As(&surface));
        auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,D2D1::PixelFormat(desc.Format,D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::check_hresult(draw->CreateBitmapFromDxgiSurface(surface.Get(),&properties,&bitmap));draw->SetTarget(bitmap.Get());
        properties.bitmapOptions=D2D1_BITMAP_OPTIONS_NONE;properties.pixelFormat.alphaMode=D2D1_ALPHA_MODE_IGNORE;winrt::check_hresult(draw->CreateBitmap(D2D1::SizeU(w,h),nullptr,0,&properties,&input));
        winrt::check_hresult(draw->CreateEffect(CLSID_D2D1GaussianBlur,&gaussian));gaussian->SetInput(0,input.Get());
        gaussian->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,D2D1_BORDER_MODE_HARD);width=w;height=h;
    }
public:
    bool usingGpu() const {return device!=nullptr&&!failed;}
    void clear(){gaussian.Reset();input.Reset();bitmap.Reset();staging.Reset();target.Reset();draw.Reset();d2d.Reset();factory.Reset();context.Reset();device.Reset();width=height=0;}
    Frame render(const Frame& source,double p,int strength){
        if(failed)return blur(source,p,strength);
        try {
            if(width!=source.width||height!=source.height){clear();initialize(source.width,source.height);}
            winrt::check_hresult(input->CopyFromMemory(nullptr,source.pixels.data(),source.width*4));
            winrt::check_hresult(gaussian->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,float(blurSigma(strength)*height/1080.0*p)));
            draw->BeginDraw();draw->Clear(D2D1::ColorF(0,0,0,1));draw->DrawImage(gaussian.Get());winrt::check_hresult(draw->EndDraw());
            context->CopyResource(staging.Get(),target.Get());Frame result(width,height);D3D11_MAPPED_SUBRESOURCE mapped{};winrt::check_hresult(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
            for(int y=0;y<height;++y){auto pixels=reinterpret_cast<const uint32_t*>(static_cast<const BYTE*>(mapped.pData)+size_t(y)*mapped.RowPitch);
                double weight=p*(1-.35*y/std::max(1,height-1)),dim=1-blurDim(strength)*p;
                for(int x=0;x<width;++x){auto original=source.pixels[size_t(y)*width+x],soft=pixels[x];uint32_t out=0xff000000;
                    for(int c=0;c<3;++c){int shift=c*8;out|=uint32_t((((original>>shift)&255)*(1-weight)+((soft>>shift)&255)*weight)*dim)<<shift;}result.pixels[size_t(y)*width+x]=out;
                }
            }context->Unmap(staging.Get(),0);return result;
        }catch(...){clear();failed=true;return blur(source,p,strength);}
    }
};
}
