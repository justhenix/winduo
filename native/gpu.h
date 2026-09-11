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
    std::vector<uint32_t> soft_cache;
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
    void clear(){gaussian.Reset();input.Reset();bitmap.Reset();staging.Reset();target.Reset();draw.Reset();d2d.Reset();factory.Reset();context.Reset();device.Reset();soft_cache.clear();soft_cache.shrink_to_fit();width=height=0;}
    Frame render(const Frame& source,double p,int strength){
        p = std::clamp(p, 0.0, 1.0);
        if (p <= 0.0 || source.pixels.empty() || source.width <= 0 || source.height <= 0) return source;
        if(failed)return blur(source,p,strength);
        try {
            if(width!=source.width||height!=source.height){clear();initialize(source.width,source.height);}
            if(soft_cache.size()<size_t(width)*height) soft_cache.resize(size_t(width)*height);
            winrt::check_hresult(input->CopyFromMemory(nullptr,source.pixels.data(),source.width*4));
            float sigma = float(blurSigma(strength) * height / 1080.0 * p);
            winrt::check_hresult(gaussian->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,std::max(0.1f, sigma)));
            draw->BeginDraw();draw->Clear(D2D1::ColorF(0,0,0,1));draw->DrawImage(gaussian.Get());winrt::check_hresult(draw->EndDraw());
            context->CopyResource(staging.Get(),target.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            winrt::check_hresult(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
            for(int y=0; y<height; ++y){
                memcpy(soft_cache.data() + size_t(y) * width,
                       static_cast<const BYTE*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                       size_t(width) * 4);
            }
            context->Unmap(staging.Get(),0);
            Frame result(width,height);
            projectFold(source, soft_cache.data(), width, height, p, result);
            return result;
        }catch(...){clear();failed=true;return blur(source,p,strength);}
    }
};
}
