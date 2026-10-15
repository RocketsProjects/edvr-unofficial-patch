#pragma once
#include <d3d11.h>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include "../common/guard.h"

namespace edvr { namespace eye_final_capture {
// Separate from the temporal capture: its last right-eye copy arrives AFTER
// that capture has written and reset its counters. Only explicitly scheduled
// scene frames can allocate or copy here. No render state is changed.
constexpr unsigned Count=16;
constexpr uint64_t Budget=512ull*1024*1024, BlobCap=64ull*1024*1024;
// 0: every row image is the 1400 px centre crop. N > 0: the whole submitted region at 1/(1<<N) size, made by a mip chain (CopySubresourceRegion, GenerateMips,
// then the staging copy of level N). Set by fss_tiles.cpp for the automatic eye run, which has to cover the FSS ring wherever it lies, not the centre.
inline unsigned g_reduceLevel=0;
struct Clock {
    uint32_t epoch=0;
    void boundary(bool armed){if(armed)++epoch;}
    void reset(){epoch=0;}
};
struct Image {
    ID3D11Texture2D* staging=nullptr;
    uint64_t sequence=0;
    uint32_t width=0,height=0,format=0,crop[4]{},submitRegion[4]{};
    bool composite=false,flipU=false,flipV=false;
    const char* status="not_reached";
    bool writable()const{return staging&&!strcmp(status,"copied");}
};
struct Row { uint32_t scene=0,epoch=0; bool scheduled=false; Image eye[2]; };
inline unsigned pixelBytes(DXGI_FORMAT f) {
    switch(f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: return 4;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:return 16;
    default:return 0;
    }
}
struct Run {
    Row rows[Count]{}; Image overview[2];
    ID3D11Device* captureDevice=nullptr;
    // Published pointers survive a driver SEH so the owning guarded caller can
    // release them; stack-local raw pointers would be lost during that unwind.
    ID3D11Device *pendingDevice=nullptr,*pendingOwner=nullptr;
    template<class T> static void drop(T*& p){auto* old=p;p=nullptr;if(old)old->Release();}
    void releaseTransient(){drop(pendingOwner);drop(pendingDevice);}
    bool armed=false,ready=false;
    unsigned count=0,unmatched=0,duplicates=0;
    uint64_t bytes=0;
    ID3D11Texture2D* mipTex=nullptr; ID3D11ShaderResourceView* mipSrv=nullptr; uint32_t mipW=0,mipH=0,mipFmt=0,mipLevels=0;
    static DXGI_FORMAT typedOf(DXGI_FORMAT f) {
        switch(f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return f;
        }
    }
    void reset() {
        guarded("eye capture/final mip release",[&]{drop(mipSrv);drop(mipTex);});
        for(auto& r:rows) for(auto& e:r.eye) if(e.staging)guarded("eye capture/final texture release",[&]{drop(e.staging);});
        for(auto& e:overview)if(e.staging)guarded("eye capture/final overview release",[&]{drop(e.staging);});
        if(captureDevice)guarded("eye capture/final capture device release",[&]{drop(captureDevice);});
        if(pendingOwner)guarded("eye capture/final pending owner release",[&]{drop(pendingOwner);});
        if(pendingDevice)guarded("eye capture/final pending device release",[&]{drop(pendingDevice);});
        *this=Run{};
    }
    void arm(){reset();armed=true;}
    void schedule(unsigned index,uint32_t epoch,uint32_t scene) {
        if(!armed||index>=Count)return;
        if(rows[index].scheduled){++duplicates;return;}
        rows[index].scheduled=true;rows[index].scene=scene;rows[index].epoch=epoch;
        count=(std::max)(count,index+1);
    }
    void schedule(unsigned index,uint32_t scene){schedule(index,scene,scene);}
    int find(uint32_t epoch) const {
        if(!armed)return -1;
        for(unsigned i=0;i<count;++i)if(rows[i].scheduled&&rows[i].epoch==epoch)return int(i);
        return -1;
    }
    bool complete()const {
        if(!ready||!count)return false;
        for(unsigned i=0;i<count;++i)if(rows[i].scheduled)
            for(const auto& e:rows[i].eye)if(!e.sequence)return false;
        return true;
    }
    void stage(Image& image,ID3D11DeviceContext* ctx,ID3D11Texture2D* source,
               const D3D11_TEXTURE2D_DESC& d,const uint32_t box[4]) {
        image.width=d.Width;image.height=d.Height;image.format=uint32_t(d.Format);
        for(unsigned i=0;i<4;++i)image.crop[i]=box[i];
        const unsigned bpp=pixelBytes(d.Format);
        if(!bpp){image.status="source_format";return;}
        if(d.MipLevels!=1||d.ArraySize!=1||d.SampleDesc.Count!=1||d.SampleDesc.Quality||
           box[0]>=box[2]||box[1]>=box[3]||box[2]>d.Width||box[3]>d.Height){image.status="source_shape";return;}
        const uint64_t cost=uint64_t(box[2]-box[0])*(box[3]-box[1])*bpp;
        if(cost>BlobCap||cost>Budget-bytes){image.status="budget";return;}
        const bool ran=guarded("eye capture/final stage",[&]{
        ctx->GetDevice(&pendingDevice);source->GetDevice(&pendingOwner);
        if(!pendingDevice||pendingOwner!=pendingDevice){releaseTransient();image.status="device_mismatch";return;}
        drop(pendingOwner);
        if(captureDevice&&captureDevice!=pendingDevice){drop(pendingDevice);image.status="device_changed";return;}
        D3D11_TEXTURE2D_DESC sd=d;sd.Width=box[2]-box[0];sd.Height=box[3]-box[1];
        sd.Usage=D3D11_USAGE_STAGING;sd.BindFlags=0;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;sd.MiscFlags=0;
        // Reserve before the driver can publish a resource and then fault.
        // That partially initialized artifact still consumes this run's budget.
        bytes+=cost;
        const HRESULT hr=pendingDevice->CreateTexture2D(&sd,nullptr,&image.staging);
        if(FAILED(hr)||!image.staging){drop(pendingDevice);if(image.staging){auto* partial=image.staging;image.staging=nullptr;partial->Release();}bytes-=cost;image.status="stage_failed";return;}
        if(!captureDevice){captureDevice=pendingDevice;pendingDevice=nullptr;}else drop(pendingDevice);
        D3D11_BOX copy{box[0],box[1],0,box[2],box[3],1};
        ctx->CopySubresourceRegion(image.staging,0,0,0,0,source,0,&copy);
        image.status="copied";
        });
        if(!ran)image.status="capture_fault";
        if(pendingOwner)guarded("eye capture/final stage owner release",[&]{drop(pendingOwner);});
        if(pendingDevice)guarded("eye capture/final stage device release",[&]{drop(pendingDevice);});
    }
    // The whole submitted region at 1/(1<<level) size. The mip chain does the averaging; the staging twin is the written image, and image.crop records ITS size
    // (0,0,w,h), not a source window. The same refusals as stage().
    void stageReduced(Image& image,ID3D11DeviceContext* ctx,ID3D11Texture2D* source,const D3D11_TEXTURE2D_DESC& d,const uint32_t region[4],unsigned level) {
        image.width=d.Width;image.height=d.Height;image.format=uint32_t(d.Format);
        const uint32_t rw=region[2]-region[0],rh=region[3]-region[1];
        const uint32_t ow=(std::max)(1u,rw>>level),oh=(std::max)(1u,rh>>level);
        image.crop[0]=0;image.crop[1]=0;image.crop[2]=ow;image.crop[3]=oh;
        const unsigned bpp=pixelBytes(d.Format);
        if(!bpp){image.status="source_format";return;}
        if(d.MipLevels!=1||d.ArraySize!=1||d.SampleDesc.Count!=1||d.SampleDesc.Quality||region[0]>=region[2]||region[1]>=region[3]||region[2]>d.Width||region[3]>d.Height){image.status="source_shape";return;}
        const uint64_t cost=uint64_t(ow)*oh*bpp;
        if(cost>BlobCap||cost>Budget-bytes){image.status="budget";return;}
        const bool ran=guarded("eye capture/final reduced stage",[&]{
        ctx->GetDevice(&pendingDevice);source->GetDevice(&pendingOwner);
        if(!pendingDevice||pendingOwner!=pendingDevice){releaseTransient();image.status="device_mismatch";return;}
        drop(pendingOwner);
        if(captureDevice&&captureDevice!=pendingDevice){drop(pendingDevice);image.status="device_changed";return;}
        if(!mipTex||mipW!=rw||mipH!=rh||mipFmt!=uint32_t(d.Format)||mipLevels!=level+1){
            drop(mipSrv);drop(mipTex);
            D3D11_TEXTURE2D_DESC md{};md.Width=rw;md.Height=rh;md.MipLevels=level+1;md.ArraySize=1;md.Format=typedOf(d.Format);md.SampleDesc.Count=1;md.Usage=D3D11_USAGE_DEFAULT;
            md.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;md.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if(FAILED(pendingDevice->CreateTexture2D(&md,nullptr,&mipTex))||!mipTex){drop(mipTex);drop(pendingDevice);image.status="mip_failed";return;}
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=md.Format;sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=level+1;
            if(FAILED(pendingDevice->CreateShaderResourceView(mipTex,&sv,&mipSrv))||!mipSrv){drop(mipSrv);drop(mipTex);drop(pendingDevice);image.status="mip_failed";return;}
            mipW=rw;mipH=rh;mipFmt=uint32_t(d.Format);mipLevels=level+1;
        }
        D3D11_TEXTURE2D_DESC sd=d;sd.Width=ow;sd.Height=oh;sd.MipLevels=1;sd.ArraySize=1;
        sd.Usage=D3D11_USAGE_STAGING;sd.BindFlags=0;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;sd.MiscFlags=0;
        bytes+=cost;
        const HRESULT hr=pendingDevice->CreateTexture2D(&sd,nullptr,&image.staging);
        if(FAILED(hr)||!image.staging){drop(pendingDevice);if(image.staging){auto* partial=image.staging;image.staging=nullptr;partial->Release();}bytes-=cost;image.status="stage_failed";return;}
        if(!captureDevice){captureDevice=pendingDevice;pendingDevice=nullptr;}else drop(pendingDevice);
        D3D11_BOX box{region[0],region[1],0,region[2],region[3],1};
        ctx->CopySubresourceRegion(mipTex,0,0,0,0,source,0,&box);
        ctx->GenerateMips(mipSrv);
        ctx->CopySubresourceRegion(image.staging,0,0,0,0,mipTex,level,nullptr);
        image.status="copied";
        });
        if(!ran)image.status="capture_fault";
        if(pendingOwner)guarded("eye capture/final reduced owner release",[&]{drop(pendingOwner);});
        if(pendingDevice)guarded("eye capture/final reduced device release",[&]{drop(pendingDevice);});
    }
    void capture(uint32_t epoch,uint64_t sequence,unsigned eye,ID3D11DeviceContext* ctx,
                 ID3D11Texture2D* source,const uint32_t region[4],bool composite,bool flipU,bool flipV) {
        const int index=find(epoch);
        if(index<0){if(armed)++unmatched;return;}
        if(eye>1||!sequence)return;
        Row& row=rows[index];Image& image=row.eye[eye];
        if(image.sequence){++duplicates;return;}
        image.sequence=sequence;image.composite=composite;image.flipU=flipU;image.flipV=flipV;
        if(region)for(unsigned i=0;i<4;++i)image.submitRegion[i]=region[i];
        const Image& partner=row.eye[1-eye];
        if(partner.sequence&&partner.sequence!=sequence){image.status="sequence_mismatch";return;}
        if(!ctx||!source||!region){image.status="source_missing";return;}
        D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);
        if(region[0]>=region[2]||region[1]>=region[3]||region[2]>d.Width||region[3]>d.Height){image.status="bounds_invalid";return;}
        const unsigned w=(std::min)(1400u,region[2]-region[0]),h=(std::min)(1400u,region[3]-region[1]);
        const unsigned x=region[0]+(region[2]-region[0]-w)/2,y=region[1]+(region[3]-region[1]-h)/2;
        const uint32_t crop[4]={x,y,x+w,y+h};
        if(g_reduceLevel)stageReduced(image,ctx,source,d,region,g_reduceLevel);else stage(image,ctx,source,d,crop);
        if(index==0){Image& o=overview[eye];o.sequence=sequence;o.composite=composite;o.flipU=flipU;o.flipV=flipV;for(unsigned i=0;i<4;++i)o.submitRegion[i]=region[i];stage(o,ctx,source,d,region);}
    }
};
} }
