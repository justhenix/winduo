#pragma once
#include "engine.h"
#include <bcrypt.h>
namespace wd {
inline std::vector<uint8_t> readBytes(std::filesystem::path path){std::ifstream file(path,std::ios::binary);check(bool(file),L"Cannot read recovery file.");return {std::istreambuf_iterator<char>(file),{}};}
inline void writeBytes(std::filesystem::path path,const std::vector<uint8_t>& bytes){std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());file.flush();check(bool(file),L"Cannot save recovery file.");}
inline std::string hash(const std::vector<uint8_t>& bytes){
    BCRYPT_ALG_HANDLE alg{};BCRYPT_HASH_HANDLE state{};check(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,L"Hash provider unavailable.");
    DWORD length{},size{};BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,reinterpret_cast<BYTE*>(&length),sizeof(length),&size,0);std::vector<BYTE> object(length);BYTE digest[32]{};
    auto status=BCryptCreateHash(alg,&state,object.data(),length,nullptr,0,0);if(status>=0)status=BCryptHashData(state,const_cast<BYTE*>(bytes.data()),static_cast<ULONG>(bytes.size()),0);if(status>=0)status=BCryptFinishHash(state,digest,32,0);if(state)BCryptDestroyHash(state);BCryptCloseAlgorithmProvider(alg,0);check(status>=0,L"Hash operation failed.");
    const char* hex="0123456789ABCDEF";std::string result;for(auto byte:digest){result+=hex[byte>>4];result+=hex[byte&15];}return result;
}
inline std::vector<uint8_t> currentWallpaper(){
    using namespace winrt::Windows::Storage::Streams;
    auto stream=winrt::Windows::System::UserProfile::LockScreen::GetImageStream();
    check(stream.Size()<=64*1024*1024,L"Lock wallpaper exceeds backup limit.");
    DataReader reader(stream.GetInputStreamAt(0));auto size=static_cast<uint32_t>(stream.Size());reader.LoadAsync(size).get();std::vector<uint8_t> data(size);reader.ReadBytes(data);return data;
}
inline void setWallpaper(std::filesystem::path p){auto file=winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(p.wstring()).get();winrt::Windows::System::UserProfile::LockScreen::SetImageFileAsync(file).get();}
inline void saveBmp(const Frame& frame,std::filesystem::path path){
    BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);file.bfSize=file.bfOffBits+DWORD(frame.pixels.size()*4);
    BITMAPINFOHEADER info{sizeof(info),frame.width,-frame.height,1,32,BI_RGB};
    std::vector<uint8_t> data(file.bfSize);memcpy(data.data(),&file,sizeof(file));memcpy(data.data()+sizeof(file),&info,sizeof(info));memcpy(data.data()+file.bfOffBits,frame.pixels.data(),frame.pixels.size()*4);writeBytes(path,data);
}
class Wallpaper {
    std::mutex mutex;std::condition_variable changed;bool stop=false;int command=0;Frame frame;int strength=1;
    HWND notify;std::thread worker;
    void journal(const std::string& value){auto p=folder()/L"native-lock-restore.txt";auto temp=p;temp+=L".tmp";writeBytes(temp,{value.begin(),value.end()});check(MoveFileExW(temp.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH),L"Cannot commit recovery record.");}
    void restore(){
        auto root=folder();auto record=root/L"native-lock-restore.txt";
        if(std::filesystem::exists(record)){auto bytes=readBytes(record);if(hash(currentWallpaper())==std::string(bytes.begin(),bytes.end()))setWallpaper(root/L"native-previous-lock.img");std::filesystem::remove(record);}
        // Complete a pending v0.1 managed restoration before applying a native image.
        auto legacy=root/L"lock-restore.json";
        if(std::filesystem::exists(legacy)){auto bytes=readBytes(legacy);auto json=winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(std::string(bytes.begin(),bytes.end())));if(winrt::to_string(json.GetNamedString(L"AppliedHash"))==hash(currentWallpaper()))setWallpaper(root/L"previous-lock.jpg");std::filesystem::remove(legacy);std::filesystem::remove(root/L"previous-lock.jpg");std::filesystem::remove(root/L"blurred-lock.jpg");}
        std::filesystem::remove(root/L"native-previous-lock.img");std::filesystem::remove(root/L"native-blurred-lock.bmp");
    }
    void apply(const Frame& source,int strong){
        if(std::filesystem::exists(folder()/L"native-lock-restore.txt"))return;
        restore();auto previous=currentWallpaper();writeBytes(folder()/L"native-previous-lock.img",previous);
        auto output=blur(source,1,strong);auto path=folder()/L"native-blurred-lock.bmp";saveBmp(output,path);
        journal(hash(readBytes(path)));setWallpaper(path);journal(hash(currentWallpaper()));
    }
    void run(){winrt::init_apartment(winrt::apartment_type::multi_threaded);
        for(;;){int work;Frame input;int strong;bool ending;{std::unique_lock lock(mutex);changed.wait(lock,[&]{return stop||command;});ending=stop;work=ending?2:command;command=0;input=std::move(frame);strong=strength;}
            try{if(work==2)restore();else if(work==1&&!input.pixels.empty())apply(input,strong);}
            catch(...){PostMessageW(notify,WM_APP+3,0,0);}
            if(ending)break;
        }winrt::uninit_apartment();
    }
public:
    explicit Wallpaper(HWND hwnd):notify(hwnd),worker([this]{run();}){requestRestore();}
    void requestApply(Frame source,int strong){std::lock_guard lock(mutex);frame=std::move(source);strength=strong;command=1;changed.notify_one();}
    void requestRestore(){std::lock_guard lock(mutex);command=2;changed.notify_one();}
    ~Wallpaper(){{std::lock_guard lock(mutex);stop=true;changed.notify_one();}worker.join();}
};
}
