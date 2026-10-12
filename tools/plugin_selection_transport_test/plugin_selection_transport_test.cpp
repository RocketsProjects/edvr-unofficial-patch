#include "../../src/d3d11/plugin_selection_transport.h"

#include <cstdio>
#include <cstring>

namespace {
unsigned checks=0, failures=0;
void check(bool value,const char* label) {
    ++checks;
    if(!value){++failures;std::printf("FAIL: %s\n",label);}
}
using namespace edvr::plugin_selection;
using namespace edvr::plugin_selection_transport;

void transportReadCases() {
    PluginSelectionSnapshotV1 output{};
    HRESULT result=decodeStateFileReadResultV1(S_OK,true,nullptr,0,
        PluginSelectionProfileV1::Vr,&output);
    check(result==S_OK && output.recordState==static_cast<uint32_t>(PluginSelectionSnapshotStateV1::Absent),
          "missing state file is valid Absent without reading bytes");

    const char* empty="";
    output={};
    result=decodeStateFileReadResultV1(S_OK,false,empty,0,
        PluginSelectionProfileV1::Vr,&output);
    check(result==S_OK && output.recordState==static_cast<uint32_t>(PluginSelectionSnapshotStateV1::Absent),
          "empty legacy state is Absent");

    const char supported[]="[plugins]\r\nselection_schema_version = 1\r\ncatalog_schema_version = 2\r\nprofile = vr\r\nselected_ids =\r\n";
    output={};
    result=decodeStateFileReadResultV1(S_OK,false,supported,sizeof(supported)-1,
        PluginSelectionProfileV1::Vr,&output);
    check(result==S_OK && output.recordState==static_cast<uint32_t>(PluginSelectionSnapshotStateV1::Supported) &&
          output.recordedPluginMask==0 && output.validationFlags==0,
          "supported empty stays distinct from Absent");

    const char opaque[]="[plugins\r\nunknown = bytes\r\n";
    output={};
    result=decodeStateFileReadResultV1(S_OK,false,opaque,sizeof(opaque)-1,
        PluginSelectionProfileV1::Vr,&output);
    check(result==S_OK && output.recordState==static_cast<uint32_t>(PluginSelectionSnapshotStateV1::Opaque) &&
          output.recordedPluginMask==0,
          "malformed plugin-section candidate is Opaque");

    PluginSelectionSnapshotV1 sentinel;
    std::memset(&sentinel,0x5a,sizeof(sentinel));
    output=sentinel;
    result=decodeStateFileReadResultV1(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED),false,
        nullptr,0,PluginSelectionProfileV1::Vr,&output);
    check(result==HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) &&
          std::memcmp(&output,&sentinel,sizeof(output))==0,
          "read failure is separate and leaves valid output span untouched");

    output=sentinel;
    result=decodeStateFileReadResultV1(S_OK,false,nullptr,kMaxRuntimeStateBytes+1,
        PluginSelectionProfileV1::Vr,&output);
    check(FAILED(result) && std::memcmp(&output,&sentinel,sizeof(output))==0,
          "over-limit text fails before reading or writing output");

    output=sentinel;
    result=decodeStateFileReadResultV1(S_OK,true,nullptr,1,
        PluginSelectionProfileV1::Vr,&output);
    check(result==E_INVALIDARG && std::memcmp(&output,&sentinel,sizeof(output))==0,
          "missing result cannot carry a nonempty byte span");

    output=sentinel;
    result=decodeStateFileReadResultV1(S_OK,false,nullptr,1,
        PluginSelectionProfileV1::Vr,&output);
    check(result==E_INVALIDARG && std::memcmp(&output,&sentinel,sizeof(output))==0,
          "nonempty read requires a byte pointer");
}

void snapshotCodecCases() {
    const char absent[] = "[install]\r\nversion = test\r\n";
    const char empty[] = "[plugins]\r\nselection_schema_version = 1\r\ncatalog_schema_version = 2\r\nprofile = vr\r\nselected_ids =\r\n";
    const char known[] = "[plugins]\r\nselection_schema_version = 1\r\ncatalog_schema_version = 2\r\nprofile = vr\r\nselected_ids = performance\r\n";
    const char flatUnavailable[] = "[plugins]\r\nselection_schema_version = 1\r\ncatalog_schema_version = 2\r\nprofile = flat\r\nselected_ids = performance\r\n";
    const char unknown[] = "[plugins]\r\nselection_schema_version = 1\r\ncatalog_schema_version = 2\r\nprofile = vr\r\nselected_ids = not-a-plugin\r\n";
    const char malformedSchema[] = "[plugins]\r\nselection_schema_version = 9\r\ncatalog_schema_version = 2\r\nprofile = vr\r\nselected_ids =\r\n";
    PluginSelectionSnapshotV1 value{};
    HRESULT result = decodePluginSelectionSnapshotV1(absent, sizeof(absent)-1,
        PluginSelectionProfileV1::Vr, &value);
    check(result==S_OK && value.recordState==static_cast<uint32_t>(PluginSelectionSnapshotStateV1::Absent),
          "shared codec preserves Absent");
    result = decodePluginSelectionSnapshotV1(empty, sizeof(empty)-1,
        PluginSelectionProfileV1::Vr, &value);
    check(result==S_OK && value.recordState==static_cast<uint32_t>(PluginSelectionSnapshotStateV1::Supported) &&
          value.recordedPluginMask==0 && value.validationFlags==0,
          "shared codec preserves Supported-empty");
    result = decodePluginSelectionSnapshotV1(known, sizeof(known)-1,
        PluginSelectionProfileV1::Vr, &value);
    check(result==S_OK && value.recordState==static_cast<uint32_t>(PluginSelectionSnapshotStateV1::Supported) &&
          value.recordedPluginMask==(1u << 6) && value.validationFlags==0,
          "known selection-ready VR ID records its mask without an availability error");
    result = decodePluginSelectionSnapshotV1(flatUnavailable, sizeof(flatUnavailable)-1,
        PluginSelectionProfileV1::Flat, &value);
    check(result==S_OK && value.recordState==static_cast<uint32_t>(PluginSelectionSnapshotStateV1::Supported) &&
          value.recordedPluginMask==(1u << 6) && value.validationFlags==kSelectionUnavailablePlugin,
          "VR-only Performance remains unavailable for a matching Flat record");
    result = decodePluginSelectionSnapshotV1(unknown, sizeof(unknown)-1,
        PluginSelectionProfileV1::Vr, &value);
    check(result==S_OK && value.recordState==static_cast<uint32_t>(PluginSelectionSnapshotStateV1::Supported) &&
          value.recordedPluginMask==0 && (value.validationFlags & kSelectionUnknownPluginId) != 0,
          "unknown ID is diagnostic and never becomes a mask bit");
    result = decodePluginSelectionSnapshotV1(empty, sizeof(empty)-1,
        PluginSelectionProfileV1::Flat, &value);
    check(result==S_OK && (value.validationFlags & kSelectionProfileMismatch) != 0,
          "installed and recorded profiles remain distinct");
    result = decodePluginSelectionSnapshotV1(malformedSchema, sizeof(malformedSchema)-1,
        PluginSelectionProfileV1::Vr, &value);
    check(result==S_OK && value.recordState==static_cast<uint32_t>(PluginSelectionSnapshotStateV1::Opaque) &&
          value.recordedPluginMask==0,
          "unsupported installer schema remains Opaque");

    PluginSelectionSnapshotV1 good{};
    result = decodePluginSelectionSnapshotV1(empty, sizeof(empty)-1,
        PluginSelectionProfileV1::Vr, &good);
    PluginSelectionSnapshotV1 sentinel;
    std::memset(&sentinel,0x71,sizeof(sentinel));
    PluginSelectionSnapshotV1 output=sentinel;
    check(result==S_OK && copyPluginSelectionSnapshotV1(kPluginSelectionSnapshotVersionV1,
          sizeof(output),&good,&output)==S_OK &&
          std::memcmp(&output,&good,sizeof(output))==0,
          "valid shared snapshot copies by value");
    auto rejected = [&](PluginSelectionSnapshotV1 malformed,const char* label) {
        output=sentinel;
        const HRESULT status=copyPluginSelectionSnapshotV1(kPluginSelectionSnapshotVersionV1,
            sizeof(output),&malformed,&output);
        check(status==E_INVALIDARG && std::memcmp(&output,&sentinel,sizeof(output))==0,label);
    };
    PluginSelectionSnapshotV1 malformed=good;
    malformed.recordState=99; rejected(malformed,"unknown state enum rejects without output write");
    malformed=good; malformed.installedProfile=99; rejected(malformed,"unknown installed-profile enum rejects without output write");
    malformed=good; malformed.recordProfile=99; rejected(malformed,"unknown record-profile enum rejects without output write");
    malformed=good; malformed.validationFlags|=1u<<31; rejected(malformed,"unknown flags reject without output write");
    malformed=good; malformed.reserved[0]=1; rejected(malformed,"reserved words reject without output write");
    malformed=good; malformed.recordedPluginMask=1u<<31; rejected(malformed,"out-of-range mask rejects without output write");
    malformed=good; ++malformed.recordSchemaVersion; rejected(malformed,"record schema mismatch rejects without output write");
    output=sentinel;
    check(copyPluginSelectionSnapshotV1(kPluginSelectionSnapshotVersionV1+1,sizeof(output),&good,&output)==E_INVALIDARG &&
          std::memcmp(&output,&sentinel,sizeof(output))==0,
          "unsupported copy version leaves valid output span untouched");
    output=sentinel;
    check(copyPluginSelectionSnapshotV1(kPluginSelectionSnapshotVersionV1,sizeof(output)-1,&good,&output)==E_INVALIDARG &&
          std::memcmp(&output,&sentinel,sizeof(output))==0,
          "unsupported copy size leaves valid output span untouched");
    output=sentinel;
    check(decodePluginSelectionSnapshotV1(empty,sizeof(empty)-1,
          static_cast<PluginSelectionProfileV1>(99),&output)==E_INVALIDARG &&
          std::memcmp(&output,&sentinel,sizeof(output))==0,
          "invalid installed profile leaves valid output span untouched");
    PluginSelectionSnapshotV1 mismatched=good;
    check(pluginSelectionSnapshotMatchesBuildV1(good),"current snapshot matches generated catalog");
    mismatched.catalogFingerprint[0]^=0xff;
    check(!pluginSelectionSnapshotMatchesBuildV1(mismatched),"fingerprint mismatch is detected locally");
    mismatched=good; ++mismatched.compiledCatalogSchemaVersion;
    check(!pluginSelectionSnapshotMatchesBuildV1(mismatched),"compiled catalog schema mismatch is detected locally");
}

void queryBeforeCaptureCases() {
    PluginSelectionSnapshotV1 output;
    std::memset(&output,0x3c,sizeof(output));
    const PluginSelectionSnapshotV1 sentinel=output;
    check(edvrQueryPluginSelectionV1(kPluginSelectionSnapshotVersionV1,sizeof(output),&output)==E_PENDING &&
          std::memcmp(&output,&sentinel,sizeof(output))==0,
          "query before proxy capture reports pending without changing output");
    check(edvrQueryPluginSelectionV1(kPluginSelectionSnapshotVersionV1+1,sizeof(output),&output)==E_INVALIDARG &&
          std::memcmp(&output,&sentinel,sizeof(output))==0,
          "unknown query version is rejected before publication");
    check(edvrQueryPluginSelectionV1(kPluginSelectionSnapshotVersionV1,sizeof(output)-1,&output)==E_INVALIDARG &&
          std::memcmp(&output,&sentinel,sizeof(output))==0,
          "short query size is rejected before publication");
    check(edvrQueryPluginSelectionV1(kPluginSelectionSnapshotVersionV1,sizeof(output),nullptr)==E_INVALIDARG,
          "null query output is rejected");
}
}

int main(int argc,char** argv) {
    if(argc!=2)return 2;
    if(std::strcmp(argv[1],"--dry-run")==0){std::puts("plugin_selection_transport_test: dry-run (no state-file reads or writes)");return 0;}
    if(std::strcmp(argv[1],"--self-test")!=0)return 2;
    transportReadCases();
    snapshotCodecCases();
    queryBeforeCaptureCases();
    std::printf("plugin_selection_transport_test: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
