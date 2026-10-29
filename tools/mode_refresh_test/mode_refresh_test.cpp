// Executes production lifecycle/command bodies with Windows API test doubles.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include "../../ZakoVDD/Edid/DefaultEdid.h"
#include "../../ZakoVDD/Edid/EdidNativeResolution.h"
#include <cstdint>
#include <climits>
#include <cstring>
#include <exception>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace std;
using Mode = tuple<int,int,int,int>;
using Modes = vector<Mode>;
using IDDCX_MONITOR = void*;
using NTSTATUS = LONG;
constexpr LONG STATUS_SUCCESS = 0;
constexpr LONG STATUS_UNSUCCESSFUL = static_cast<LONG>(0xc0000001u);
#ifndef NT_SUCCESS
#define NT_SUCCESS(s) ((s) >= 0)
#endif
#define VDD_LOG_INFO_STREAM(value) do { ostringstream stream; stream << value; } while(false)
#define VDD_LOG_WARNING_STREAM(value) VDD_LOG_INFO_STREAM(value)
#define VDD_LOG_ERROR_STREAM(value) VDD_LOG_INFO_STREAM(value)
#define VDD_LOG_DEBUG_STREAM(value) VDD_LOG_INFO_STREAM(value)
#define VDD_LOG_INFO(value) ((void)(value))
#define VDD_LOG_WARNING(value) ((void)(value))
#define VDD_LOG_ERROR(value) ((void)(value))
#define VDD_LOG_DEBUG(value) ((void)(value))

mutex g_DataMutex, g_Mutex;
Modes monitorModes;
static bool apiAvailable = true;
static map<unsigned, LONG> updateStatus;
static set<unsigned> failCreate, failDestroy;
static vector<unsigned> updateCalls, createCalls;
static map<unsigned, Modes> submittedModes;
static uintptr_t nextHandle = 100;
static map<IDDCX_MONITOR, unsigned> handleIndex;
static atomic<bool> gKnownEdidIsBuiltIn{true};
static wstring requestedEdidResolution;
static wstring GetStringSetting(const wstring&) { return requestedEdidResolution; }
static set<unsigned> failArrival;
static set<unsigned> throwOnCreate;
static set<unsigned> nullCreateHandle;
#define Sleep(ms) ((void)(ms))
#define IDD_IS_FUNCTION_AVAILABLE(name) apiAvailable
struct IDDCX_TARGET_MODE2 { Mode payload; };
struct IDARG_IN_UPDATEMODES2 { int Reason; UINT TargetModeCount; const IDDCX_TARGET_MODE2* pTargetModes; };
constexpr int IDDCX_UPDATE_REASON_OTHER = 1;
constexpr int IDDCX_MONITOR_DESCRIPTION_TYPE_EDID = 1;
#define VDD_LOG_DEBUG_LAZY(value) do {} while(false)
struct WDF_OBJECT_ATTRIBUTES { int unused; };
#define WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(attr, type) (*(attr) = {})
struct MonitorDescription { UINT Size; int Type; UINT DataSize; BYTE* pData; };
struct IDDCX_MONITOR_INFO {
    UINT Size; int MonitorType; unsigned ConnectorIndex;
    ::MonitorDescription MonitorDescription; GUID MonitorContainerId;
};
struct IDARG_IN_MONITORCREATE { WDF_OBJECT_ATTRIBUTES* ObjectAttributes; IDDCX_MONITOR_INFO* pMonitorInfo; };
struct IDARG_OUT_MONITORCREATE { IDDCX_MONITOR MonitorObject; };
struct IDARG_OUT_MONITORARRIVAL { int unused; };
struct HdrSettings { float maxNits=0,minNits=0,maxFALL=0; };
using MonitorHdrSettings = HdrSettings;
struct GuidLess { bool operator()(const GUID& a,const GUID& b) const { return memcmp(&a,&b,sizeof(GUID))<0; } };
static mutex s_EdidMapMutex, s_HdrSettingsMutex;
static map<GUID,vector<BYTE>,GuidLess> s_ClientGuidEdidMap;
static map<IDDCX_MONITOR,HdrSettings> s_MonitorHdrSettingsMap;
namespace VddEdid { static bool UseDefaultLuminanceIfInvalid(float&,float&,float&) { return false; } }
static void ModifyEdidSerialNumber(vector<BYTE>&,const GUID&) {}
static void UpdateEdidHdrMetadata(vector<BYTE>&,float,float,float) {}
static void UpdateEdidFreeSyncRange(vector<BYTE>&,BYTE,BYTE) {}
static void UpdateEdidPhysicalSize(vector<BYTE>&,float,float) {}
static BYTE CalculateEdidChecksum(const vector<BYTE>&) { return 0; }
static LONG IddCxMonitorCreate(void*,const IDARG_IN_MONITORCREATE* args,IDARG_OUT_MONITORCREATE* output) {
    const auto index=args->pMonitorInfo->ConnectorIndex;
    createCalls.push_back(index);
    if(throwOnCreate.count(index)) throw runtime_error("injected create exception");
    if(failCreate.count(index)) return STATUS_UNSUCCESSFUL;
    if(nullCreateHandle.count(index)) { output->MonitorObject=nullptr; return STATUS_SUCCESS; }
    output->MonitorObject=reinterpret_cast<IDDCX_MONITOR>(++nextHandle);
    handleIndex[output->MonitorObject]=index;
    return STATUS_SUCCESS;
}
static LONG IddCxMonitorArrival(IDDCX_MONITOR handle,IDARG_OUT_MONITORARRIVAL*) {
    const auto index=handleIndex.at(handle);
    return failArrival.count(index)?STATUS_UNSUCCESSFUL:STATUS_SUCCESS;
}
static LONG IddCxMonitorDeparture(IDDCX_MONITOR handle) {
    const auto index=handleIndex.at(handle);
    return failDestroy.count(index)?STATUS_UNSUCCESSFUL:STATUS_SUCCESS;
}
static void WdfObjectDelete(IDDCX_MONITOR) {}
static LONG IddCxMonitorUpdateModes2(IDDCX_MONITOR monitor, const IDARG_IN_UPDATEMODES2* args) {
    const auto index = handleIndex.at(monitor);
    updateCalls.push_back(index);
    Modes payload;
    for (UINT i=0;i<args->TargetModeCount;++i) payload.push_back(args->pTargetModes[i].payload);
    submittedModes[index] = payload;
    return updateStatus.count(index) ? updateStatus.at(index) : STATUS_SUCCESS;
}
static void CreateTargetMode2(IDDCX_TARGET_MODE2& result, UINT w, UINT h, UINT num, UINT den) {
    result.payload = Mode{w,h,num,den};
}
static string WStringToString(const wstring& value) {
    string result;
    for (wchar_t c : value) result.push_back(c < 128 ? static_cast<char>(c) : '?');
    return result;
}

class IndirectDeviceContext {
public:
#include "creation_params.inc"
    struct MonitorRecreationResult { size_t expected=0,recreated=0; };
    struct Worker {};
    void* m_Adapter=nullptr;
    recursive_mutex m_monitorsMutex;
    map<unsigned,IDDCX_MONITOR> m_Monitors;
    map<unsigned,MonitorCreationParams> m_MonitorCreationParams;
    map<unsigned,Modes> m_MonitorDescriptionModes;
    set<unsigned> m_PendingMonitorModeRecreation;
    map<unsigned,GUID> m_MonitorGuids;
    map<IDDCX_MONITOR,int> m_CommittedTargetModes;
    map<IDDCX_MONITOR,bool> m_CommittedTargetHdrStates;
    map<IDDCX_MONITOR,unique_ptr<Worker>> m_ProcessingThreads;
    map<IDDCX_MONITOR,HANDLE> m_MouseEvents;
    static vector<BYTE> s_KnownMonitorEdid;
    int RefreshMonitorModes(bool refreshMonitorDescription, bool reuseLiveDescriptions = false);
    bool UpdateTargetModesWithinDescription(unsigned index, const Modes& modes);
    bool RecreateMonitor(unsigned index);
    bool DestroyMonitor(unsigned index);
    void DestroyAllMonitors();
    MonitorRecreationResult RecreateAllMonitors();
    void CreateMonitor(unsigned index,const GUID* guid=nullptr,float maxNits=1000,float minNits=0.0001f,float maxFALL=0,float widthCm=0,float heightCm=0);
    void Seed(unsigned index,const Modes& modes) {
        auto handle = reinterpret_cast<IDDCX_MONITOR>(++nextHandle);
        m_Monitors[index]=handle;
        handleIndex[handle]=index;
        m_MonitorCreationParams[index] = {};
        m_MonitorDescriptionModes[index] = modes;
    }
};
vector<BYTE> IndirectDeviceContext::s_KnownMonitorEdid(256,0);
struct Wrapper { IndirectDeviceContext* pContext; };
static Wrapper wrapper{};
static void* g_GlobalDevice = &wrapper;
static Wrapper* WdfObjectGet_IndirectDeviceContextWrapper(void*) { return &wrapper; }
#include "refresh.inc"
#include "edid_resolution.inc"
#include "lifecycle.inc"
#include "commands.inc"

static const Mode A{1280,720,60,1}, B{1920,1080,60,1}, C{2560,1440,60,1}, D{1600,900,60,1};
static const Modes ABCModes{A,B,C}, AB{A,B};
static void Require(bool ok,const char* message) { if(!ok) throw runtime_error(message); }
static void Reset() {
    monitorModes=ABCModes;apiAvailable=true;updateStatus.clear();submittedModes.clear();handleIndex.clear();
    failCreate.clear();failDestroy.clear();failArrival.clear();throwOnCreate.clear();nullCreateHandle.clear();
    updateCalls.clear();createCalls.clear();s_ClientGuidEdidMap.clear();s_MonitorHdrSettingsMap.clear();
    requestedEdidResolution.clear();gKnownEdidIsBuiltIn=true;
    IndirectDeviceContext::s_KnownMonitorEdid=VddEdid::GetDefaultEdid(VddEdid::Profile::Legacy);
}
static void Command(IndirectDeviceContext& context,const Modes& modes) {
    wstring param;
    for(const auto& mode:modes) {
        if(!param.empty()) param+=L',';
        param+=to_wstring(get<0>(mode))+L'x'+to_wstring(get<1>(mode))+L'x'+to_wstring(get<2>(mode)/get<3>(mode));
    }
    wrapper.pContext=&context;
    HandleSetModesCommand(param.data());
}

int main() {
    int passed=0,failed=0;
    auto test=[&](const char* name,auto body) {
        Reset();
        try {body();++passed;cout<<"PASS "<<name<<'\n';}
        catch(const exception& ex) {++failed;cout<<"FAIL "<<name<<": "<<ex.what()<<'\n';}
    };
    test("reuse_remove_reorder_restore",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ABCModes);auto before=ctx.m_Monitors.at(0);
        Command(ctx,AB);Command(ctx,{A,C,B});Command(ctx,ABCModes);
        Require(ctx.m_Monitors.at(0)==before && createCalls.empty() && updateCalls.size()==3,"existing modes recreated monitor");
        Require(ctx.m_MonitorDescriptionModes.at(0)==ABCModes && submittedModes.at(0)==ABCModes,"creation description or payload changed");
    });
    test("new_or_preferred_mode_rebuilds",[]{
        for(const Modes modes:{Modes{A,D},Modes{B,A,C}}) {
            Reset();IndirectDeviceContext ctx;ctx.Seed(0,ABCModes);auto before=ctx.m_Monitors.at(0);
            Command(ctx,modes);
            Require(ctx.m_Monitors.at(0)!=before && updateCalls.empty() && createCalls.size()==1,"description change did not rebuild");
        }
    });
    test("unavailable_or_failed_update_rebuilds",[]{
        for(bool unavailable:{false,true}) {
            Reset();IndirectDeviceContext ctx;ctx.Seed(0,ABCModes);auto before=ctx.m_Monitors.at(0);
            apiAvailable=!unavailable;updateStatus[0]=STATUS_UNSUCCESSFUL;Command(ctx,AB);
            Require(ctx.m_Monitors.at(0)!=before && createCalls.size()==1,"fallback failed");
        }
    });
    test("same_command_retries_failed_recreation",[]{
        for(int fault=0;fault<4;++fault) {
            Reset();IndirectDeviceContext ctx;ctx.Seed(0,ABCModes);auto before=ctx.m_Monitors.at(0);
            updateStatus[0]=STATUS_UNSUCCESSFUL;
            (fault==0?failCreate:fault==1?failArrival:fault==2?failDestroy:throwOnCreate).insert(0);
            try {Command(ctx,AB);} catch(const runtime_error&) {if(fault!=3) throw;}
            Require(ctx.m_PendingMonitorModeRecreation.count(0)==1 && ctx.m_MonitorCreationParams.count(0)==1,"failure lost retry state");
            failCreate.clear();failArrival.clear();failDestroy.clear();throwOnCreate.clear();updateStatus.clear();
            createCalls.clear();updateCalls.clear();Command(ctx,AB);
            Require(ctx.m_Monitors.at(0)!=before && createCalls==vector<unsigned>{0} && updateCalls.empty(),"same command did not finish recreation");
            Require(ctx.m_PendingMonitorModeRecreation.empty(),"success left retry flag");
            before=ctx.m_Monitors.at(0);createCalls.clear();Command(ctx,{A});Command(ctx,AB);
            Require(ctx.m_Monitors.at(0)==before && createCalls.empty(),"recovery disabled later in-place updates");
        }
    });
    test("partial_retry_preserves_healthy_monitor",[]{
        for(bool available:{false,true}) {
            Reset();IndirectDeviceContext ctx;ctx.Seed(0,ABCModes);ctx.Seed(1,ABCModes);apiAvailable=available;
            failCreate.insert(1);Command(ctx,{A,D});auto healthy=ctx.m_Monitors.at(0);
            failCreate.clear();createCalls.clear();Command(ctx,{A,D});
            Require(createCalls==vector<unsigned>{1} && ctx.m_Monitors.at(0)==healthy && ctx.m_Monitors.count(1)==1,"retry disturbed healthy monitor or missed failed one");
        }
    });
    test("explicit_delete_cancels_retry",[]{
        for(bool all:{false,true}) {
            Reset();IndirectDeviceContext ctx;ctx.Seed(0,ABCModes);ctx.Seed(1,ABCModes);
            failCreate.insert(1);Command(ctx,{A,D});
            if(all) ctx.DestroyAllMonitors();else Require(ctx.DestroyMonitor(1),"delete failed");
            failCreate.clear();createCalls.clear();Command(ctx,{A,D});
            Require(createCalls.empty() && ctx.m_MonitorCreationParams.count(1)==0 && ctx.m_PendingMonitorModeRecreation.empty(),"deleted monitor was resurrected");
        }
    });
    test("repeated_failure_uses_latest_request",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ABCModes);failCreate.insert(0);
        for(int i=0;i<3;++i) {Command(ctx,{A,D});Require(ctx.m_MonitorCreationParams.count(0)==1,"retry parameters lost");}
        failCreate.clear();Command(ctx,{A,C});
        Require(createCalls.size()==4 && ctx.m_MonitorDescriptionModes.at(0)==Modes{A,C} && ctx.m_PendingMonitorModeRecreation.empty(),"retry lost state or published obsolete modes");
    });
    test("unchanged_update_failure_retries",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ABCModes);auto before=ctx.m_Monitors.at(0);updateStatus[0]=STATUS_UNSUCCESSFUL;
        Command(ctx,ABCModes);updateStatus.clear();Command(ctx,ABCModes);
        Require(ctx.m_Monitors.at(0)!=before && ctx.m_PendingMonitorModeRecreation.empty(),"unchanged-list failure did not retry");
    });
    test("forced_refresh_rebuilds",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ABCModes);wrapper.pContext=&ctx;
        HandleRefreshModesCommand(nullptr);
        Require(updateCalls.empty() && createCalls.size()==1,"forced refresh reused monitor");
    });
    test("cached_client_edid_uses_current_native_preference",[]{
        IndirectDeviceContext ctx;
        const GUID guid{1,2,3,{4,5,6,7,8,9,10,11}};
        const auto original=IndirectDeviceContext::s_KnownMonitorEdid;
        auto cached=original;
        VddEdid::ApplyNativeResolution(cached,true,L"3840x2160",{{3840,2160,60,1}});
        s_ClientGuidEdidMap[guid]=cached;
        requestedEdidResolution=L"default";
        ctx.CreateMonitor(0,&guid);
        Require(s_ClientGuidEdidMap.at(guid)[56]==original[56],"cached 4K timing survived default selection");
        Require(ctx.DestroyMonitor(0),"could not remove monitor");
        requestedEdidResolution=L"3840x2160";
        monitorModes={{1920,1080,60,1},{3840,2160,60,1}};
        ctx.CreateMonitor(0,&guid);
        Require(s_ClientGuidEdidMap.at(guid)[56]==0 && s_ClientGuidEdidMap.at(guid)[58]==0xf2,
                "recreated client did not get 4K timing");
        Require(ctx.DestroyMonitor(0),"could not remove 4K monitor");
        requestedEdidResolution=L"default";
        ctx.CreateMonitor(0,&guid);
        Require(s_ClientGuidEdidMap.at(guid)[56]==original[56],"return to default failed");
    });
    test("failed_creation_does_not_cache_old_edid_source",[]{
        for(bool custom:{false,true}) for(int fault=0;fault<4;++fault) {
            Reset();IndirectDeviceContext ctx;
            const GUID guid{3,2,3,{4,5,6,7,8,9,10,11}};
            gKnownEdidIsBuiltIn=!custom;
            // Distinguish the old template from the next reload's template.
            IndirectDeviceContext::s_KnownMonitorEdid[10]=42;
            (fault==0?failCreate:fault==1?failArrival:fault==2?throwOnCreate:nullCreateHandle).insert(0);
            try {ctx.CreateMonitor(0,&guid);} catch(const runtime_error&) {if(fault!=2) throw;}
            Require(s_ClientGuidEdidMap.count(guid)==0,"failed creation retained an EDID without a registered monitor");
            failCreate.clear();failArrival.clear();throwOnCreate.clear();nullCreateHandle.clear();
            // Simulate reloading a different EDID source before retrying.
            const auto reloaded=VddEdid::GetDefaultEdid(VddEdid::Profile::Legacy);
            IndirectDeviceContext::s_KnownMonitorEdid=reloaded;
            gKnownEdidIsBuiltIn=custom;
            requestedEdidResolution=L"3840x2160";
            monitorModes={{3840,2160,60,1}};
            ctx.CreateMonitor(0,&guid);
            const auto& cached=s_ClientGuidEdidMap.at(guid);
            Require(cached[10]==reloaded[10],"retry used the old source's cached EDID");
            if(custom)
                Require(cached[56]==0 && cached[58]==0xf2,"built-in retry did not apply selected timing");
            else
                Require(equal(reloaded.begin()+54,reloaded.begin()+72,cached.begin()+54),"custom retry changed its timing");
            Require(ctx.DestroyMonitor(0),"retry monitor could not be destroyed");
            Require(s_ClientGuidEdidMap.count(guid)==0,"successful monitor did not own cache cleanup");
        }
    });
    test("failed_creation_preserves_preexisting_client_cache",[]{
        IndirectDeviceContext ctx;
        const GUID guid{4,2,3,{4,5,6,7,8,9,10,11}};
        s_ClientGuidEdidMap[guid]=IndirectDeviceContext::s_KnownMonitorEdid;
        failCreate.insert(0);
        ctx.CreateMonitor(0,&guid);
        Require(s_ClientGuidEdidMap.count(guid)==1,"failure removed an existing cache entry");
    });
    test("valid_custom_edid_is_preserved_at_monitor_creation",[]{
        IndirectDeviceContext ctx;
        gKnownEdidIsBuiltIn=false;requestedEdidResolution=L"3840x2160";
        monitorModes={{3840,2160,60,1}};
        const auto original=IndirectDeviceContext::s_KnownMonitorEdid;
        ctx.CreateMonitor(0);
        Require(equal(original.begin()+54,original.begin()+72,
                      IndirectDeviceContext::s_KnownMonitorEdid.begin()+54),"custom timing changed");
    });
    test("explicit_custom_size_is_not_replaced_by_largest_mode",[]{
        IndirectDeviceContext ctx;
        const GUID guid{2,2,3,{4,5,6,7,8,9,10,11}};
        requestedEdidResolution=L"3440x1440";
        monitorModes={{1920,1080,60,1},{3440,1440,60,1},{3840,2160,60,1}};
        ctx.CreateMonitor(0,&guid);
        const auto& edid=s_ClientGuidEdidMap.at(guid);
        Require(edid[56]+((edid[58]&240)<<4)==3440 && edid[59]+((edid[61]&240)<<4)==1440,
                "selected ultrawide mode was replaced by an automatic maximum");
    });
    cout<<"RESULT passed="<<passed<<" failed="<<failed<<'\n';
    return failed?1:0;
}
