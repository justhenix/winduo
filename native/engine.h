#pragma once
#include "core.h"
namespace wd {
class WorkerPool {
    std::vector<std::thread> workers;
    std::mutex run_mtx;
    std::mutex mtx;
    std::condition_variable cv_work, cv_done;
    void* task_ctx = nullptr;
    void (*task_fn)(void*, int, int) = nullptr;
    uint64_t generation = 0;
    int remaining = 0;
    bool quit = false;
public:
    WorkerPool() {
        unsigned int count = std::clamp(std::thread::hardware_concurrency(), 2u, 16u);
        for (unsigned int i = 1; i < count; ++i) {
            workers.emplace_back([this, i, count] {
                uint64_t my_gen = 0;
                std::unique_lock lock(mtx);
                while (!quit) {
                    cv_work.wait(lock, [this, my_gen] { return quit || generation > my_gen; });
                    if (quit) break;
                    my_gen = generation;
                    auto fn = task_fn;
                    auto ctx = task_ctx;
                    lock.unlock();
                    try {
                        fn(ctx, int(i), int(count));
                    } catch (...) {}
                    lock.lock();
                    if (--remaining == 0) cv_done.notify_one();
                }
            });
        }
    }
    ~WorkerPool() {
        { std::lock_guard lock(mtx); quit = true; }
        cv_work.notify_all();
        for (auto& w : workers) if (w.joinable()) w.join();
    }
    template<class Func>
    void run(int count, Func func) {
        if (count <= 0) return;
        std::unique_lock run_lock(run_mtx);
        int total = int(workers.size()) + 1;
        struct Runner {
            int count;
            Func& func;
            static void execute(void* ptr, int part, int parts) {
                auto* self = static_cast<Runner*>(ptr);
                int start = self->count * part / parts;
                int end = self->count * (part + 1) / parts;
                for (int y = start; y < end; ++y) self->func(y);
            }
        } runner{ count, func };
        {
            std::lock_guard lock(mtx);
            task_ctx = &runner;
            task_fn = &Runner::execute;
            remaining = total - 1;
            ++generation;
        }
        cv_work.notify_all();
        Runner::execute(&runner, 0, total);
        {
            std::unique_lock lock(mtx);
            cv_done.wait(lock, [this] { return remaining == 0; });
        }
    }
};
inline WorkerPool& workerPool() { static WorkerPool pool; return pool; }
template<class Function> void rows(int count,Function function){
    workerPool().run(count, function);
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
inline uint32_t bilerp(uint32_t c00, uint32_t c01, uint32_t c10, uint32_t c11, int fx, int fy) {
    uint64_t rb0 = ((uint64_t(c00 & 0x00FF00FF) * (256 - fx) + uint64_t(c01 & 0x00FF00FF) * fx) >> 8) & 0x00FF00FF;
    uint64_t g0  = ((uint64_t(c00 & 0x0000FF00) * (256 - fx) + uint64_t(c01 & 0x0000FF00) * fx) >> 8) & 0x0000FF00;
    uint64_t rb1 = ((uint64_t(c10 & 0x00FF00FF) * (256 - fx) + uint64_t(c11 & 0x00FF00FF) * fx) >> 8) & 0x00FF00FF;
    uint64_t g1  = ((uint64_t(c10 & 0x0000FF00) * (256 - fx) + uint64_t(c11 & 0x0000FF00) * fx) >> 8) & 0x0000FF00;
    uint32_t rb = uint32_t(((rb0 * (256 - fy) + rb1 * fy) >> 8) & 0x00FF00FF);
    uint32_t g  = uint32_t(((g0 * (256 - fy) + g1 * fy) >> 8) & 0x0000FF00);
    return rb | g;
}
inline uint32_t lerp1d(uint32_t c0, uint32_t c1, int f) {
    uint64_t rb = ((uint64_t(c0 & 0x00FF00FF) * (256 - f) + uint64_t(c1 & 0x00FF00FF) * f) >> 8) & 0x00FF00FF;
    uint64_t g  = ((uint64_t(c0 & 0x0000FF00) * (256 - f) + uint64_t(c1 & 0x0000FF00) * f) >> 8) & 0x0000FF00;
    return uint32_t(rb | g);
}
inline Frame resize(const Frame& f,int width){
    if (f.pixels.empty() || f.width <= 0 || f.height <= 0 || width <= 0) return Frame{};
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
inline void projectFold(const Frame& source, const uint32_t* soft_pixels, int soft_w, int soft_h, double progress, Frame& result) {
    int W = source.width, H = source.height;
    if (W <= 0 || H <= 0 || progress <= 0.0) {
        result = source;
        return;
    }
    double p = std::clamp(progress, 0.0, 1.0);
    double theta = p * 0.907; // ~52 degrees physical tilt
    double cos_t = std::cos(theta);
    double sin_t = std::sin(theta);
    double Ye = 0.55, Ze = 1.80;
    
    rows(H, [&](int y) {
        uint32_t* out_row = result.pixels.data() + size_t(y) * W;
        double y_proj = double(H - 1 - y) / std::max(1, H - 1);
        double denom = Ze * cos_t + (Ye - y_proj) * sin_t;
        if (denom <= 1e-5) {
            std::fill_n(out_row, W, 0xff000000);
            return;
        }
        double h = (y_proj * Ze) / denom;
        if (h < 0.0 || h > 1.0) {
            std::fill_n(out_row, W, 0xff000000);
            return;
        }
        
        double scale_x = 1.0 + (h * sin_t) / Ze;
        double sample_y = std::clamp((1.0 - h) * double(H - 1), 0.0, double(H - 1));
        int src_y0 = int(sample_y), src_y1 = std::min(src_y0 + 1, H - 1);
        int src_fy = int((sample_y - src_y0) * 256.0);
        
        double soft_sample_y = std::clamp((1.0 - h) * double(soft_h - 1), 0.0, double(soft_h - 1));
        int soft_y0 = int(soft_sample_y), soft_y1 = std::min(soft_y0 + 1, soft_h - 1);
        int soft_fy = int((soft_sample_y - soft_y0) * 256.0);
        
        const uint32_t* src_r0 = source.pixels.data() + size_t(src_y0) * W;
        const uint32_t* src_r1 = source.pixels.data() + size_t(src_y1) * W;
        const uint32_t* soft_r0 = soft_pixels + size_t(soft_y0) * soft_w;
        const uint32_t* soft_r1 = soft_pixels + size_t(soft_y1) * soft_w;
        
        // Natural depth-of-field transition:
        // Anchored lower ~18% stays sharp and legible, mid-screen gracefully defocuses,
        // upper 35% transitions to 100% heavy frosted bokeh.
        double blur_weight = smoothstep(0.18, 0.70, h);
        int w = std::clamp(int(blur_weight * 256.0), 0, 256);
        
        double frost = p * smoothstep(0.20, 0.72, h);
        double shade = 1.0 - p * 0.06 * h;
        double feather = std::max(0.002, 0.008 * p);
        
        int x_left = std::clamp(int((0.5 - 0.5 / scale_x) * double(W - 1)), 0, W - 1);
        int x_right = std::clamp(int((0.5 + 0.5 / scale_x) * double(W - 1) + 1.0), 0, W - 1);
        
        if (x_left > 0) std::fill_n(out_row, x_left, 0xff000000);
        if (x_right < W - 1) std::fill_n(out_row + x_right + 1, (W - 1) - x_right, 0xff000000);
        
        double inv_w = 1.0 / std::max(1, W - 1);
        for (int x = x_left; x <= x_right; ++x) {
            double x_norm = double(x) * inv_w;
            double u = 0.5 + (x_norm - 0.5) * scale_x;
            if (u < 0.0 || u > 1.0) {
                out_row[x] = 0xff000000;
                continue;
            }
            
            double edge_dist = std::min({u, 1.0 - u, 1.0 - h});
            double corner = (1.0 - smoothstep(0.0, 0.22, std::min(u, 1.0 - u))) * h;
            double lighting = shade * (1.0 - p * 0.16 * corner);
            int lit = std::clamp(int(lighting * 256.0), 0, 256);
            
            double sx = u * double(W - 1);
            int sx0 = int(sx), sx1 = std::min(sx0 + 1, W - 1), sfx = int((sx - sx0) * 256.0);
            uint32_t src_col = bilerp(src_r0[sx0], src_r0[sx1], src_r1[sx0], src_r1[sx1], sfx, src_fy);
            
            double sfx_x = u * double(soft_w - 1);
            int so_x0 = int(sfx_x), so_x1 = std::min(so_x0 + 1, soft_w - 1), so_fx = int((sfx_x - so_x0) * 256.0);
            uint32_t soft_col = bilerp(soft_r0[so_x0], soft_r0[so_x1], soft_r1[so_x0], soft_r1[so_x1], so_fx, soft_fy);
            
            uint32_t blended = (w <= 0) ? src_col : (w >= 256) ? soft_col : lerp1d(src_col, soft_col, w);
            
            int b = blended & 0xFF;
            int g = (blended >> 8) & 0xFF;
            int r = (blended >> 16) & 0xFF;
            
            if (frost > 0.001) {
                int lift = int(frost * 14.0);
                int scatter_r = (255 - r) * int(frost * 20.0) / 256;
                int scatter_g = (255 - g) * int(frost * 20.0) / 256;
                int scatter_b = (255 - b) * int(frost * 22.0) / 256;
                r = std::clamp(r + lift + scatter_r, 0, 255);
                g = std::clamp(g + lift + scatter_g, 0, 255);
                b = std::clamp(b + lift + scatter_b, 0, 255);
            }
            
            int r_lit = (r * lit) >> 8;
            int g_lit = (g * lit) >> 8;
            int b_lit = (b * lit) >> 8;
            uint32_t lit_color = 0xff000000 | (r_lit << 16) | (g_lit << 8) | b_lit;
            
            if (edge_dist < feather) {
                double f_val = smoothstep(0.0, feather, edge_dist);
                int f = int(f_val * 256.0);
                out_row[x] = lerp1d(0xff000000, lit_color, f) | 0xff000000;
            } else {
                out_row[x] = lit_color;
            }
        }
    });
}
inline Frame blur(const Frame& source,double progress,int strength){
    progress=std::clamp(progress,0.0,1.0);
    if(progress<=0.0 || source.pixels.empty() || source.width <= 0 || source.height <= 0)return source;
    auto small=resize(source,320);if(small.pixels.empty()||small.width<=0||small.height<=0)return source;
    auto horizontal=small;auto soft=small;
    double sigma=blurSigma(strength)*source.height/1080.0*small.width/source.width*progress;
    double spread=std::max(.25,sigma);int radius=std::min(int(std::ceil(3*spread)), 64);
    std::vector<double> kernel(2*radius+1);double sum=0;
    for(int k=-radius;k<=radius;++k)sum+=(kernel[k+radius]=std::exp(-k*k/(2*spread*spread)));
    for(auto& v:kernel)v/=sum;
    rows(small.height,[&](int y){
        for(int x=0;x<small.width;++x){uint32_t pixel=0xff000000;
            for(int c=0;c<3;++c){double value=0;for(int k=-radius;k<=radius;++k)value+=((small.pixels[size_t(y)*small.width+std::clamp(x+k,0,small.width-1)]>>(c*8))&255)*kernel[k+radius];pixel|=uint32_t(std::round(value))<<(c*8);}
            horizontal.pixels[size_t(y)*small.width+x]=pixel;
        }
    });
    rows(small.height,[&](int y){
        for(int x=0;x<small.width;++x){uint32_t pixel=0xff000000;for(int c=0;c<3;++c){double value=0;for(int k=-radius;k<=radius;++k)value+=((horizontal.pixels[size_t(std::clamp(y+k,0,small.height-1))*small.width+x]>>(c*8))&255)*kernel[k+radius];pixel|=uint32_t(std::round(value))<<(c*8);}soft.pixels[size_t(y)*small.width+x]=pixel;}
    });
    Frame result(source.width,source.height);
    projectFold(source, soft.pixels.data(), soft.width, soft.height, progress, result);
    return result;
}
inline Frame blurFlat(const Frame& source,int strength){
    if(source.pixels.empty()||source.width<=0||source.height<=0)return source;
    int targetW=std::min(source.width,640);
    auto small=resize(source,targetW);
    if(small.pixels.empty()||small.width<=0||small.height<=0)return source;
    auto horizontal=small;auto soft=small;
    double sigma=blurSigma(strength)*double(small.width)/960.0;
    double spread=std::max(0.5,sigma);int radius=std::min(int(std::ceil(3*spread)), 64);
    std::vector<double> kernel(2*radius+1);double sum=0;
    for(int k=-radius;k<=radius;++k)sum+=(kernel[k+radius]=std::exp(-k*k/(2*spread*spread)));
    for(auto& v:kernel)v/=sum;
    rows(small.height,[&](int y){
        for(int x=0;x<small.width;++x){
            double r=0,g=0,b=0;
            for(int k=-radius;k<=radius;++k){
                int sx=std::clamp(x+k,0,small.width-1);uint32_t px=small.pixels[size_t(y)*small.width+sx];
                double w=kernel[k+radius];b+=(px&0xFF)*w;g+=((px>>8)&0xFF)*w;r+=((px>>16)&0xFF)*w;
            }
            horizontal.pixels[size_t(y)*small.width+x]=0xFF000000|(uint32_t(std::clamp(int(std::round(r)),0,255))<<16)|(uint32_t(std::clamp(int(std::round(g)),0,255))<<8)|uint32_t(std::clamp(int(std::round(b)),0,255));
        }
    });
    rows(small.height,[&](int y){
        for(int x=0;x<small.width;++x){
            double r=0,g=0,b=0;
            for(int k=-radius;k<=radius;++k){
                int sy=std::clamp(y+k,0,small.height-1);uint32_t px=horizontal.pixels[size_t(sy)*small.width+x];
                double w=kernel[k+radius];b+=(px&0xFF)*w;g+=((px>>8)&0xFF)*w;r+=((px>>16)&0xFF)*w;
            }
            soft.pixels[size_t(y)*small.width+x]=0xFF000000|(uint32_t(std::clamp(int(std::round(r)),0,255))<<16)|(uint32_t(std::clamp(int(std::round(g)),0,255))<<8)|uint32_t(std::clamp(int(std::round(b)),0,255));
        }
    });
    double dim=1.0-blurDim(strength);int dimFactor=int(dim*256);Frame result(source.width,source.height);
    rows(source.height,[&](int y){
        double sy=(y+0.5)*double(soft.height)/double(source.height)-0.5;double clamped_sy=std::clamp(sy,0.0,double(soft.height-1));
        int y0=int(clamped_sy),y1=std::min(y0+1,soft.height-1),fy=int((clamped_sy-y0)*256);
        const uint32_t* r0=soft.pixels.data()+size_t(y0)*soft.width;const uint32_t* r1=soft.pixels.data()+size_t(y1)*soft.width;
        uint32_t* out_row=result.pixels.data()+size_t(y)*source.width;
        for(int x=0;x<source.width;++x){
            double sx=(x+0.5)*double(soft.width)/double(source.width)-0.5;double clamped_sx=std::clamp(sx,0.0,double(soft.width-1));
            int x0=int(clamped_sx),x1=std::min(x0+1,soft.width-1),fx=int((clamped_sx-x0)*256);
            uint32_t color=bilerp(r0[x0],r0[x1],r1[x0],r1[x1],fx,fy);
            uint32_t rb=uint32_t(((uint64_t(color&0x00FF00FF)*dimFactor)>>8)&0x00FF00FF);
            uint32_t g=uint32_t(((uint64_t(color&0x0000FF00)*dimFactor)>>8)&0x0000FF00);
            out_row[x]=0xFF000000|rb|g;
        }
    });
    return result;
}
class Capture {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGIOutputDuplication> duplicate;ComPtr<ID3D11Texture2D> staging;
    Frame last;RECT area{};double retry=0;
    void syncDesktop(){
        if(HDESK input=OpenInputDesktop(0,FALSE,GENERIC_ALL)){SetThreadDesktop(input);CloseDesktop(input);}
        else if(HDESK inputFallback=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS|DESKTOP_WRITEOBJECTS|DESKTOP_SWITCHDESKTOP)){SetThreadDesktop(inputFallback);CloseDesktop(inputFallback);}
    }
    void initialize(RECT r){
        syncDesktop();
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
        syncDesktop();
        try{
            if(!duplicate&&now()>=retry)initialize(r);
            if(duplicate){DXGI_OUTDUPL_FRAME_INFO info{};ComPtr<IDXGIResource> resource;
                UINT waitMs = last.pixels.empty() ? 100 : 0;
                HRESULT hr=duplicate->AcquireNextFrame(waitMs,&info,&resource);
                if(hr==DXGI_ERROR_WAIT_TIMEOUT){if(!last.pixels.empty())return last;}
                else{winrt::check_hresult(hr);try{ComPtr<ID3D11Texture2D> source;winrt::check_hresult(resource.As(&source));context->CopyResource(staging.Get(),source.Get());D3D11_MAPPED_SUBRESOURCE mapped{};winrt::check_hresult(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
                    try {Frame result(r.right-r.left,r.bottom-r.top);
                        for(int y=0;y<result.height;++y){
                            auto* dst = result.pixels.data()+size_t(y)*result.width;
                            auto* src = reinterpret_cast<const uint32_t*>(static_cast<const BYTE*>(mapped.pData)+size_t(y)*mapped.RowPitch);
                            for(int x=0;x<result.width;++x) dst[x] = src[x] | 0xff000000;
                        }
                        last=std::move(result);
                    }
                    catch(...){context->Unmap(staging.Get(),0);throw;}context->Unmap(staging.Get(),0);
                }catch(...){duplicate->ReleaseFrame();throw;}duplicate->ReleaseFrame();return last;}
            }
        }catch(...){clear();retry=now()+5;}
        Dib dib(r.right-r.left,r.bottom-r.top);auto dc=GetDC(nullptr);BOOL ok=BitBlt(dib.dc,0,0,dib.width,dib.height,dc,r.left,r.top,SRCCOPY);ReleaseDC(nullptr,dc);
        if(!ok){syncDesktop();dc=GetDC(nullptr);ok=BitBlt(dib.dc,0,0,dib.width,dib.height,dc,r.left,r.top,SRCCOPY);ReleaseDC(nullptr,dc);}
        check(ok,L"Desktop capture failed.");Frame frame(dib.width,dib.height);
        for(size_t i=0;i<size_t(dib.width)*dib.height;++i)frame.pixels[i]=dib.bits[i]|0xff000000;
        last=frame;return frame;
    }
};
}
