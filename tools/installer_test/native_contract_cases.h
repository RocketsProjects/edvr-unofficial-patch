#pragma once

#include <vector>
#include <cstring>
#include <cstdio>

#include "../../src/installer/probe.h"

namespace edvr::installer::test {

// Contract negatives are intentionally byte buffers: the validator must reject
// malformed, truncated, and non PE data without mapping or executing it.
template <class Check>
inline void nativeContractCases(Check check) {
    const std::vector<unsigned char> empty;
    check(!validateNativePayloadBytes(empty.data(), empty.size(), NativeImageKind::Graphics),
          "empty native graphics payload rejected");
    const unsigned char mz[] = {'M', 'Z', 0, 0, 0, 0, 0, 0};
    check(!validateNativePayloadBytes(mz, sizeof(mz), NativeImageKind::Graphics),
          "truncated native graphics payload rejected");
    check(!validateNativePayloadBytes(mz, sizeof(mz), NativeImageKind::Runtime),
          "truncated native runtime payload rejected");
    check(!validateNativePayloadBytes(mz, sizeof(mz), NativeImageKind::Loader),
          "truncated OpenXR loader payload rejected");
}

template <class Check>
inline void nativeBuiltContractCases(Check check, const std::wstring& root) {
    auto bytes=[&](const wchar_t* leaf) {
        std::vector<unsigned char> out;
        FILE* file=nullptr;
        const auto path=root+L"\\build\\"+leaf;
        if(_wfopen_s(&file,path.c_str(),L"rb") || !file) return out;
        fseek(file,0,SEEK_END);const long size=ftell(file);rewind(file);
        if(size>0 && size<(64<<20)) {out.resize(size);if(fread(out.data(),1,out.size(),file)!=out.size())out.clear();}
        fclose(file);return out;
    };
    const auto graphics=bytes(L"edvr_openxr_graphics.dll");
    const auto runtime=bytes(L"edvr_openxr_runtime.dll");
    const auto loader=bytes(L"openxr_loader.dll");
    check(validateNativePayloadBytes(graphics.data(),graphics.size(),NativeImageKind::Graphics),"built native graphics marker/providers accepted");
    const bool runtimeValid=validateNativePayloadBytes(runtime.data(),runtime.size(),NativeImageKind::Runtime);
    check(runtimeValid,"built native runtime exact export contract accepted");
    check(validateNativePayloadBytes(loader.data(),loader.size(),NativeImageKind::Loader),"bundled x64 Khronos loader accepted");
    check(probeDll(root+L"\\build\\edvr_openxr_runtime.dll").kind==DllKind::Edvr,"native runtime is identified as EDVR, never as an original to preserve");
    if(runtimeValid) {
        // Mutate the built PE in memory. Names use an alphabetic table while
        // the ordinal table establishes the ABI: the new export must be @17.
        auto old=runtime;
        auto* oldDos=reinterpret_cast<IMAGE_DOS_HEADER*>(old.data());
        auto* oldNt=reinterpret_cast<IMAGE_NT_HEADERS64*>(old.data()+oldDos->e_lfanew);
        auto* oldSections=IMAGE_FIRST_SECTION(oldNt);
        auto oldOffset=[&](DWORD rva) -> size_t {
            for(unsigned i=0;i<oldNt->FileHeader.NumberOfSections;++i) {
                const auto& section=oldSections[i];
                if(rva>=section.VirtualAddress && rva-section.VirtualAddress<section.SizeOfRawData) {
                    const size_t off=section.PointerToRawData+rva-section.VirtualAddress;
                    return off<old.size()?off:old.size();
                }
            }
            return old.size();
        };
        const size_t expOff=oldOffset(oldNt->OptionalHeader.DataDirectory[0].VirtualAddress);
        if(expOff+sizeof(IMAGE_EXPORT_DIRECTORY)>old.size()) {
            check(false,"built runtime export directory available for mutation tests");
            return;
        }
        auto* exp=reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(old.data()+expOff);
        const size_t namesOff=oldOffset(exp->AddressOfNames);
        const size_t ordsOff=oldOffset(exp->AddressOfNameOrdinals);
        if(exp->NumberOfNames!=17 || exp->NumberOfFunctions!=17 ||
           namesOff+17*sizeof(DWORD)>old.size() || ordsOff+17*sizeof(WORD)>old.size()) {
            check(false,"built runtime has 17 named ordinal exports for mutation tests");
            return;
        }
        auto* names=reinterpret_cast<DWORD*>(old.data()+namesOff);
        auto* ords=reinterpret_cast<WORD*>(old.data()+ordsOff);
        unsigned admissionIndex=17;
        size_t admissionName=old.size();
        for(unsigned i=0;i<17;++i) {
            const size_t nameOff=oldOffset(names[i]);
            if(nameOff==old.size()) continue;
            const char* name=reinterpret_cast<const char*>(old.data()+nameOff);
            if(memchr(name,0,old.size()-nameOff) &&
               strcmp(name,"edvrCommitPluginSelectionAdmissionV1")==0) {
                admissionIndex=i;admissionName=nameOff;break;
            }
        }
        if(admissionIndex==17 || ords[admissionIndex]!=16) {
            check(false,"built admission export found at ordinal 17 for mutation tests");
            return;
        }
        auto badName=old;
        badName[admissionName]='x';
        check(!validateNativePayloadBytes(badName.data(),badName.size(),NativeImageKind::Runtime),
              "renamed admission export rejected as current native payload");
        auto badOrdinal=old;
        *reinterpret_cast<WORD*>(badOrdinal.data()+ordsOff+admissionIndex*sizeof(WORD))=15;
        check(!validateNativePayloadBytes(badOrdinal.data(),badOrdinal.size(),NativeImageKind::Runtime),
              "admission export at wrong ordinal rejected as current native payload");

        // Reconstruct the prior exact 16-export ABI from this built PE. It is
        // still an installed EDVR runtime to preserve, but not a new payload.
        for(unsigned i=admissionIndex;i+1<17;++i) {names[i]=names[i+1];ords[i]=ords[i+1];}
        exp->NumberOfNames=16;
        exp->NumberOfFunctions=16;
        check(!validateNativePayloadBytes(old.data(),old.size(),NativeImageKind::Runtime),
              "prior 16-export native runtime is rejected as current payload");
        const std::wstring oldPath=root+L"\\build\\insttest_prior_native_"+
            std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(GetTickCount64())+L".dll";
        HANDLE file=CreateFileW(oldPath.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL,nullptr);
        check(file!=INVALID_HANDLE_VALUE,"create confined prior-runtime probe fixture");
        if(file!=INVALID_HANDLE_VALUE) {
            struct DeleteFixture {std::wstring path;~DeleteFixture(){DeleteFileW(path.c_str());}} cleanup{oldPath};
            DWORD written=0;
            const BOOL wrote=WriteFile(file,old.data(),static_cast<DWORD>(old.size()),&written,nullptr);
            CloseHandle(file);
            check(wrote && written==old.size(),"write prior-runtime probe fixture");
            if(wrote && written==old.size()) {
                const auto observed=probeDll(oldPath);
                check(observed.kind==DllKind::Edvr && observed.nativeRuntimeExports,
                      "prior 16-export runtime remains recognized as installed EDVR native runtime");
            }
        }
    }
    if(graphics.empty())return;
    // Mutate the actual built PE using the export ordinal table, so a marker
    // with a high/nonalphabetic ordinal is exercised too. Never execute it.
    auto bad=graphics;
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(bad.data());
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(bad.data()+dos->e_lfanew);
    auto* sections=IMAGE_FIRST_SECTION(nt);
    auto offset=[&](DWORD rva) -> size_t {
        for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i)
            if(rva>=sections[i].VirtualAddress && rva-sections[i].VirtualAddress<sections[i].SizeOfRawData)
                return sections[i].PointerToRawData+rva-sections[i].VirtualAddress;
        return bad.size();
    };
    auto* exp=reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(bad.data()+offset(nt->OptionalHeader.DataDirectory[0].VirtualAddress));
    auto* names=reinterpret_cast<DWORD*>(bad.data()+offset(exp->AddressOfNames));
    auto* funcs=reinterpret_cast<DWORD*>(bad.data()+offset(exp->AddressOfFunctions));
    auto* ords=reinterpret_cast<WORD*>(bad.data()+offset(exp->AddressOfNameOrdinals));
    size_t marker=bad.size(),provider=bad.size();DWORD markerRva=0;
    for(DWORD i=0;i<exp->NumberOfNames;++i) {
        const auto no=offset(names[i]);const char* name=reinterpret_cast<const char*>(bad.data()+no);
        if(!strcmp(name,"edvrNativeStartupRouting")) {markerRva=funcs[ords[i]];marker=offset(markerRva);}
        if(!strcmp(name,"edvrAcquireNativeGraphics"))provider=no;
    }
    if(marker+16>bad.size() || provider==bad.size()) {check(false,"built marker/provider found for mutation tests");return;}
    bad[marker+8]=0;
    check(!validateNativePayloadBytes(bad.data(),bad.size(),NativeImageKind::Graphics),"route-disabled marker rejected");
    bad[marker+8]=1;bad[provider]='x';
    check(!validateNativePayloadBytes(bad.data(),bad.size(),NativeImageKind::Graphics),"missing graphics provider rejected");
    bad[provider]='e';
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i)if(markerRva>=sections[i].VirtualAddress && markerRva-sections[i].VirtualAddress<sections[i].SizeOfRawData) {
        sections[i].Characteristics|=IMAGE_SCN_MEM_WRITE;
        check(!validateNativePayloadBytes(bad.data(),bad.size(),NativeImageKind::Graphics),"writable startup marker rejected");
    }
}

} // namespace edvr::installer::test
