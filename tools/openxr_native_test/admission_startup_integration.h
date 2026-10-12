#pragma once

#include "launch_centre_cases.h"
#include "../../src/common/plugin_admission_commit.h"
#include "../../src/openxr/native_graphics_client.h"
#include "plugin_manifest.inc"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <vector>

namespace edvr::openxr::test::admission_startup {
namespace fs = std::filesystem;
using namespace edvr::plugin_selection;
using namespace edvr::plugin_selection::admission_v1;
using namespace edvr::plugin_selection::commit_v1;

inline std::uint64_t providerBase(const DecisionV1& decision) {
  return (static_cast<std::uint64_t>(decision.graphicsProviderBaseHigh)<<32)|
      decision.graphicsProviderBaseLow;
}
inline std::wstring modulePath(HMODULE module) {
  wchar_t path[32768]{};const DWORD n=GetModuleFileNameW(module,path,32768);
  return n&&n<32768?std::wstring(path,n):std::wstring{};
}

inline fs::path executableDirectory() {
  wchar_t path[32768]{};
  const DWORD n=GetModuleFileNameW(nullptr,path,static_cast<DWORD>(std::size(path)));
  return n&&n<std::size(path)?fs::canonical(fs::path(path)).parent_path():fs::path{};
}
inline fs::path checkedBuildRoot(const std::wstring& value) {
  const fs::path build=executableDirectory();
  const fs::path root=fs::canonical(fs::path(value));
  if(build.empty()||!root.is_absolute()||root.parent_path()!=build||
     !fs::is_directory(root))throw std::runtime_error("admission scratch is not a direct child of build");
  return root;
}
inline void writeText(const fs::path& path,const std::string& text) {
  std::ofstream file(path,std::ios::binary|std::ios::trunc);
  if(!file)throw std::runtime_error("could not create confined admission fixture input");
  file.write(text.data(),static_cast<std::streamsize>(text.size()));file.flush();
  if(!file)throw std::runtime_error("could not flush confined admission fixture input");
}
inline bool equalPath(const std::wstring& a,const std::wstring& b) {
  wchar_t left[32768]{},right[32768]{};
  const DWORD ln=GetFullPathNameW(a.c_str(),32768,left,nullptr);
  const DWORD rn=GetFullPathNameW(b.c_str(),32768,right,nullptr);
  return ln&&rn&&ln<32768&&rn<32768&&
      CompareStringOrdinal(left,-1,right,-1,TRUE)==CSTR_EQUAL;
}
inline std::wstring quote(const std::wstring& value) {
  if(value.find(L'"')!=std::wstring::npos)throw std::runtime_error("unexpected quote in fixture path");
  return L"\""+value+L"\"";
}
inline void reportChildLog(const fs::path& path) {
  std::printf("admission startup child output: %ls\n",path.c_str());
  std::ifstream file(path,std::ios::binary);
  if(!file)return;
  file.seekg(0,std::ios::end);
  const auto size=file.tellg();
  if(size>std::streamoff(65536))file.seekg(-65536,std::ios::end);
  else file.seekg(0,std::ios::beg);
  std::string output;
  output.assign(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());
  if(!output.empty())std::fwrite(output.data(),1,output.size(),stdout);
}
inline bool launchChild(const fs::path& root,const wchar_t* scenario) {
  wchar_t image[32768]{};const DWORD n=GetModuleFileNameW(nullptr,image,32768);
  if(!n||n>=32768)return false;
  std::wstring command=quote(image)+L" --plugin-admission-startup-child "+scenario+L" "+quote(root.wstring());
  const fs::path logPath=root/(std::wstring(L"admission_")+scenario+L".log");
  SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
  HANDLE output=CreateFileW(logPath.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,
      CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(output==INVALID_HANDLE_VALUE)return false;
  HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,
      OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(input==INVALID_HANDLE_VALUE){CloseHandle(output);return false;}
  STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
  startup.hStdInput=input;startup.hStdOutput=output;startup.hStdError=output;
  PROCESS_INFORMATION process{};
  const BOOL started=CreateProcessW(image,command.data(),nullptr,nullptr,TRUE,
      CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW,nullptr,executableDirectory().c_str(),
      &startup,&process);
  const DWORD startError=started?ERROR_SUCCESS:GetLastError();
  CloseHandle(input);CloseHandle(output);
  if(!started){std::printf("admission startup child could not start: %lu\n",startError);
    reportChildLog(logPath);return false;}
  const DWORD wait=WaitForSingleObject(process.hProcess,30000);
  DWORD exitCode=ERROR_TIMEOUT;
  const bool exited=wait==WAIT_OBJECT_0&&GetExitCodeProcess(process.hProcess,&exitCode);
  if(!exited){TerminateProcess(process.hProcess,9);WaitForSingleObject(process.hProcess,5000);}
  CloseHandle(process.hThread);CloseHandle(process.hProcess);
  if(!exited||exitCode!=0){std::printf("admission startup child %ls: wait=%lu exit=%lu\n",
      scenario,wait,exitCode);reportChildLog(logPath);return false;}
  return true;
}
inline void cleanupRoot(const fs::path& root) {
  const fs::path build=executableDirectory();
  const fs::path canonical=fs::canonical(root);
  const auto name=canonical.filename().wstring();
  if(canonical.parent_path()!=build||name.rfind(L"edvr_native_admission_",0)!=0)
    throw std::runtime_error("refusing cleanup outside owned build child");
  fs::remove_all(canonical);
}
inline fs::path makeRoot() {
  const fs::path build=executableDirectory();
  if(build.empty()||!fs::is_directory(build))throw std::runtime_error("build directory unavailable");
  const fs::path root=build/(L"edvr_native_admission_"+std::to_wstring(GetCurrentProcessId())+
      L"_"+std::to_wstring(GetTickCount64()));
  if(!fs::create_directory(root)||fs::canonical(root).parent_path()!=build)
    throw std::runtime_error("could not create confined build child");
  return root;
}
inline bool copyProductionPair(const fs::path& root,std::wstring* graphicsPath,
                              std::wstring* nativePath) {
  const fs::path build=executableDirectory();
  const fs::path graphics=build/L"d3d11.dll";
  const fs::path native=build/L"openvr_api.dll";
  const fs::path loader=build/L"openxr_loader.dll";
  const fs::path destinationGraphics=root/L"d3d11.dll";
  const fs::path destinationNative=root/L"Openvr"/L"win64"/L"openvr_api.dll";
  const fs::path destinationLoader=root/L"Openvr"/L"win64"/L"openxr_loader.dll";
  fs::create_directories(destinationNative.parent_path());
  if(!fs::is_regular_file(graphics)||!fs::is_regular_file(native)||!fs::is_regular_file(loader)||
     !CopyFileW(graphics.c_str(),destinationGraphics.c_str(),TRUE)||
     !CopyFileW(native.c_str(),destinationNative.c_str(),TRUE)||
     !CopyFileW(loader.c_str(),destinationLoader.c_str(),TRUE))return false;
  *graphicsPath=destinationGraphics.wstring();*nativePath=destinationNative.wstring();
  return true;
}
inline void writeInstallInputs(const fs::path& root,bool selected) {
  writeText(root/L"edvr_profile.ini","[install]\r\nschema=1\r\nprofile=vr\r\n");
  writeText(root/L"edvr.ini","");
  fs::create_directories(root/L"edvr_install");
  writeText(root/L"edvr_install"/L"state.ini",
      std::string("[plugins]\r\nselection_schema_version = 1\r\ncatalog_schema_version = 2\r\nprofile = vr\r\nselected_ids = ")+
      (selected?"temporal-aa":"")+"\r\n");
}
inline bool setScratchEnvironment(const fs::path& root) {
  const fs::path local=root/L"localappdata",logs=root/L"logs";
  std::error_code ec;fs::create_directories(local,ec);if(ec)return false;
  const auto build=executableDirectory().wstring();
  return SetEnvironmentVariableW(L"LOCALAPPDATA",local.c_str())&&
      SetEnvironmentVariableW(L"EDVR_LOG_DIR",logs.c_str())&&
      SetEnvironmentVariableW(L"EDVR_LOG_DIR_FOR",build.c_str());
}
using QueryAdmission=HRESULT (WINAPI *)(std::uint32_t,std::uint32_t,std::uint32_t,void*);
using QuerySelection=HRESULT (WINAPI *)(std::uint32_t,std::uint32_t,void*);
using NativeCommit=HRESULT (WINAPI *)(const AdmissionCommitRequestV1*);
using CreateDevice=HRESULT (WINAPI *)(IDXGIAdapter*,D3D_DRIVER_TYPE,HMODULE,UINT,
    const D3D_FEATURE_LEVEL*,UINT,UINT,ID3D11Device**,D3D_FEATURE_LEVEL*,ID3D11DeviceContext**);
inline bool decisionPending(HMODULE graphics) {
  const auto query=reinterpret_cast<QueryAdmission>(GetProcAddress(graphics,"edvrQueryPluginAdmissionV1"));
  DecisionV1 decision{};
  return query&&query(kQueryVersionV1,static_cast<std::uint32_t>(QueryKindV1::Decision),
      sizeof(decision),&decision)==E_PENDING;
}
inline bool queryDecision(HMODULE graphics,DecisionV1* decision,GraphicsContractV1* contract,
                          PluginSelectionSnapshotV1* snapshot) {
  const auto query=reinterpret_cast<QueryAdmission>(GetProcAddress(graphics,"edvrQueryPluginAdmissionV1"));
  const auto selection=reinterpret_cast<QuerySelection>(GetProcAddress(graphics,"edvrQueryPluginSelectionV1"));
  return query&&selection&&query(kQueryVersionV1,static_cast<std::uint32_t>(QueryKindV1::Contract),
      sizeof(*contract),contract)==S_OK&&query(kQueryVersionV1,
      static_cast<std::uint32_t>(QueryKindV1::Decision),sizeof(*decision),decision)==S_OK&&
      selection(kPluginSelectionSnapshotVersionV1,sizeof(*snapshot),snapshot)==S_OK;
}
inline AdmissionCommitRequestV1 requestFor(const DecisionV1& decision,
    const GraphicsContractV1& contract,const PluginSelectionSnapshotV1& snapshot) {
  AdmissionCommitRequestV1 request{};request.structSize=sizeof(request);request.abiVersion=commit_v1::kAbiVersion;
  request.requiredCapabilities=kRequiredCapabilities;request.activeProfile=decision.activeProfile;
  request.supportedProfileMask=contract.supportedProfileMask;request.snapshot=snapshot;request.decision=decision;
  return request;
}
inline bool validStoreProbe(HMODULE native,const AdmissionCommitRequestV1& request,
                            HRESULT expected) {
  const auto commit=reinterpret_cast<NativeCommit>(GetProcAddress(native,
      "edvrCommitPluginSelectionAdmissionV1"));
  if(!commit)return false;
  auto invalid=request;invalid.reserved=1;
  return commit(&invalid)==expected;
}
inline bool createWarp(HMODULE graphics,Microsoft::WRL::ComPtr<ID3D11Device>& device,
                       Microsoft::WRL::ComPtr<ID3D11DeviceContext>& context) {
  const auto create=reinterpret_cast<CreateDevice>(GetProcAddress(graphics,"D3D11CreateDevice"));
  if(!create)return false;
  const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL obtained{};
  HRESULT hr=create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,levels,2,D3D11_SDK_VERSION,
                   &device,&obtained,&context);
  if(hr==E_INVALIDARG)hr=create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,levels+1,1,
      D3D11_SDK_VERSION,&device,&obtained,&context);
  return hr==S_OK&&device&&context;
}
inline int child(const wchar_t* scenario,const std::wstring& rootValue) {
  unsigned checks=0,failures=0;
  const auto check=[&](bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL admission startup: %s\n",label);}};
  try {
    const fs::path root=checkedBuildRoot(rootValue);
    const bool isolated=setScratchEnvironment(root);
    check(isolated,"isolate LOCALAPPDATA and EDVR logs under build scratch");
    if(!isolated)return 2;
    const bool selected=std::wcscmp(scenario,L"selected")==0;
    const bool empty=std::wcscmp(scenario,L"empty")==0;
    const bool late=std::wcscmp(scenario,L"late-peer")==0;
    check(selected||empty||late,"known isolated scenario");
    if(!(selected||empty||late))return 2;
    const fs::path graphicsPath=root/L"d3d11.dll";
    const fs::path nativePath=root/L"Openvr"/L"win64"/L"openvr_api.dll";
    HMODULE graphics=LoadLibraryExW(graphicsPath.c_str(),nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const bool exactGraphics=graphics&&equalPath(modulePath(graphics),graphicsPath.wstring());
    check(exactGraphics,
          "load exact staged production graphics proxy before its first export");
    if(!exactGraphics)return 2;
    check(decisionPending(graphics),"graphics Decision remains unpublished before peer preload");
    HMODULE native=nullptr;
    if(!late)native=LoadLibraryExW(nativePath.c_str(),nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    check(late||native!=nullptr,"preload exact-path native peer before graphics INIT_ONCE");
    if(!late&&!native)return 2;
    if(native){
      const bool exactNative=equalPath(modulePath(native),nativePath.wstring());
      check(exactNative,"native peer maps from staged exact path");
      if(!exactNative)return 2;
    }
    if(native)check(decisionPending(graphics),"native preload does not publish the graphics Decision");
    Microsoft::WRL::ComPtr<ID3D11Device> warpDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> warpContext;
    check(createWarp(graphics,warpDevice,warpContext),"production D3D11CreateDevice export creates WARP and runs graphics INIT_ONCE");
    if(!warpDevice)return 2;
    DecisionV1 decision{};GraphicsContractV1 contract{};PluginSelectionSnapshotV1 snapshot{};
    check(queryDecision(graphics,&decision,&contract,&snapshot),"published production admission Decision, Contract and selection snapshot are queryable");
    if(!(selected||empty)) {
      check(decision.mode==static_cast<std::uint32_t>(Mode::Legacy)&&
            decision.reason==static_cast<std::uint32_t>(Reason::ProviderRejected)&&
            providerBase(decision)==0,"late native peer leaves the first graphics decision provider-rejected and provider-zero");
      const DecisionV1 frozen=decision;
      native=LoadLibraryExW(nativePath.c_str(),nullptr,
          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
      check(native!=nullptr&&equalPath(modulePath(native),nativePath.wstring()),"load native peer only after the production decision is frozen");
      if(native) {
        NativeGraphicsClient denied;
        const HRESULT status=denied.acquire(graphicsPath.wstring(),nullptr,true);
        DecisionV1 after{};GraphicsContractV1 afterContract{};PluginSelectionSnapshotV1 afterSnapshot{};
        const bool same=queryDecision(graphics,&after,&afterContract,&afterSnapshot)&&
            std::memcmp(&frozen,&after,sizeof(frozen))==0;
        check(status==E_ACCESSDENIED&&!denied.provider()&&same,
              "late peer cannot qualify or change the frozen provider-zero decision before paired graphics calls");
        const auto invalid=requestFor(decision,contract,snapshot);
        check(validStoreProbe(native,invalid,E_INVALIDARG),
              "invalid commit probe leaves a late-loaded native peer store empty");
      }
    } else {
      const std::uint32_t expectedMask=selected?(1u<<edvr::plugins::kPluginTemporalAa):0u;
      check(decision.mode==static_cast<std::uint32_t>(Mode::AdmittedSelection)&&
            decision.reason==static_cast<std::uint32_t>(Reason::None)&&
            decision.activeProfile==static_cast<std::uint32_t>(PluginSelectionProfileV1::Vr)&&
            decision.selectedPluginMask==expectedMask&&providerBase(decision)==
              static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(graphics)),
            selected?"strict VR Temporal-AA selection commits the exact graphics provider":
                     "strict VR whole-profile empty selection commits the exact graphics provider");
      check((contract.supportedProfileMask&kPluginSelectionVrProfileMaskV1)!=0,
            "generated VR profile is fully ready before strict admission");
      const auto request=requestFor(decision,contract,snapshot);
      check(validStoreProbe(native,request,E_ACCESSDENIED),
            "native commit store rejects a changed request without modifying its committed selection");
      NativeGraphicsClient binding;
      const HRESULT acquired=binding.acquire(graphicsPath.wstring(),&decision,false);
      check(acquired==S_OK&&binding.provider()==graphics&&binding.device()==warpDevice.Get(),
            "native graphics binding accepts the exact committed provider and WARP device");
      if(acquired==S_OK) {
        launch_fixture::Fixture fixture;auto& host=fixture.host;host.externalDevice=binding.device();
        const bool services=fixture.route.bind()&&fixture.owner.start();
        check(fixture.initialized&&services,"real NativeRuntimeHost fixture reaches startup treatment acquisition");
        bool accepted=false;
        const bool invoked=services&&fixture.route.invoke([&]{
          accepted=host.acquireStartupTreatmentProviders(binding.provider(),binding.device(),host.runtimeGeneration);
        });
        check(invoked&&accepted,"production NativeRuntimeHost startup treatment path completes over WARP");
        check(host.menu.acquired(),"mandatory Core menu provider remains acquired for selected and empty profiles");
        check(host.temporal.acquired()==selected&&host.sharpen.acquired()==selected,
            selected?"selected Temporal-AA acquires both actual optional providers":
                     "whole-profile empty returns exact owner absence for both optional providers");
        check(host.graphicsCalls.calls==3&&host.graphicsCalls.thread==GetCurrentThreadId()&&
              host.graphicsCalls.wrongThread==0&&host.graphicsCalls.rejected==0,
              "all three provider decisions ran through the production graphics rendezvous");
        if(services)check(fixture.owner.stop(),"startup provider owner joins cleanly");
        fixture.dispatcher.close();
        host.temporal.close();host.sharpen.close();host.menu.close();
      }
    }
    warpContext.Reset();warpDevice.Reset();
    std::printf("admission_startup_child,%ls,checks=%u,failures=%u\n",scenario,checks,failures);
    return failures?1:0;
  } catch(const std::exception& error) {
    std::printf("FAIL admission startup exception: %s\n",error.what());return 3;
  }
}
template<class Check> inline void run(Check check) {
  fs::path root;
  try {
    root=makeRoot();
    std::wstring graphics,native;
    const bool copied=copyProductionPair(root,&graphics,&native);
    check(copied,"production proxy and native runtime copy into confined build scratch");
    if(!copied){std::printf("admission startup scratch preserved: %ls\n",root.c_str());return;}
    writeInstallInputs(root,true);
    const bool selected=launchChild(root,L"selected");
    check(selected,"selected-profile production startup child passes");
    if(!selected){std::printf("admission startup scratch preserved: %ls\n",root.c_str());return;}
    writeInstallInputs(root,false);
    const bool empty=launchChild(root,L"empty");
    check(empty,"whole-profile empty production startup child passes");
    if(!empty){std::printf("admission startup scratch preserved: %ls\n",root.c_str());return;}
    writeInstallInputs(root,true);
    const bool late=launchChild(root,L"late-peer");
    check(late,"late-peer provider-rejection child passes without retry");
    if(!late){std::printf("admission startup scratch preserved: %ls\n",root.c_str());return;}
    cleanupRoot(root);
  } catch(const std::exception& error) {
    if(!root.empty()&&fs::exists(root))
      std::printf("admission startup scratch preserved: %ls\n",root.c_str());
    check(false,error.what());
  }
}
} // namespace edvr::openxr::test::admission_startup
