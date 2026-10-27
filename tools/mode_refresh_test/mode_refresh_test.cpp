// Executes production lifecycle/command bodies with Windows API test doubles.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <cstdint>
#include <climits>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
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
static vector<string> messages;
#define VDD_LOG_INFO_STREAM(value) do { ostringstream stream; stream << value; messages.push_back(stream.str()); } while(false)
#define VDD_LOG_WARNING_STREAM(value) VDD_LOG_INFO_STREAM(value)
#define VDD_LOG_ERROR_STREAM(value) VDD_LOG_INFO_STREAM(value)
#define VDD_LOG_DEBUG_STREAM(value) VDD_LOG_INFO_STREAM(value)
#define VDD_LOG_INFO(value) messages.push_back(value)
#define VDD_LOG_WARNING(value) messages.push_back(value)
#define VDD_LOG_ERROR(value) messages.push_back(value)
#define VDD_LOG_DEBUG(value) messages.push_back(value)

mutex g_DataMutex, g_Mutex;
Modes monitorModes;
static bool apiAvailable = true;
static map<unsigned, LONG> updateStatus;
static set<unsigned> failCreate, failDestroy;
static vector<unsigned> updateCalls, createCalls, destroyCalls;
static map<unsigned, Modes> submittedModes;
static uintptr_t nextHandle = 100;
static map<IDDCX_MONITOR, unsigned> handleIndex;
static set<unsigned> failArrival;
static set<unsigned> throwOnCreate;
static vector<unsigned> arrivalCalls, deletedHandles;
static vector<DWORD> sleeps;
#define Sleep(ms) sleeps.push_back(ms)
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
    output->MonitorObject=reinterpret_cast<IDDCX_MONITOR>(++nextHandle);
    handleIndex[output->MonitorObject]=index;
    return STATUS_SUCCESS;
}
static LONG IddCxMonitorArrival(IDDCX_MONITOR handle,IDARG_OUT_MONITORARRIVAL*) {
    const auto index=handleIndex.at(handle);arrivalCalls.push_back(index);
    return failArrival.count(index)?STATUS_UNSUCCESSFUL:STATUS_SUCCESS;
}
static LONG IddCxMonitorDeparture(IDDCX_MONITOR handle) {
    const auto index=handleIndex.at(handle);destroyCalls.push_back(index);
    return failDestroy.count(index)?STATUS_UNSUCCESSFUL:STATUS_SUCCESS;
}
static void WdfObjectDelete(IDDCX_MONITOR handle) { deletedHandles.push_back(handleIndex.at(handle)); }
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
#if PR45
    int RefreshMonitorModes(bool refreshMonitorDescription, bool reuseLiveDescriptions = false);
    bool UpdateTargetModesWithinDescription(unsigned index, const Modes& modes);
#else
    int RefreshMonitorModes(bool refreshMonitorDescription);
#endif
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
#include "lifecycle.inc"
#include "commands.inc"

static const Mode A{1280,720,60,1}, B{1920,1080,60,1}, C{2560,1440,60,1}, D{1600,900,60,1};
static const Modes ModesABC{A,B,C}, AB{A,B};
static void Require(bool ok,const string& message) { if(!ok) throw runtime_error(message); }
static void Reset() {
    monitorModes=ModesABC; messages.clear(); apiAvailable=true; updateStatus.clear(); failCreate.clear(); failDestroy.clear();
    updateCalls.clear(); createCalls.clear(); destroyCalls.clear(); submittedModes.clear(); handleIndex.clear(); sleeps.clear();
    failArrival.clear();throwOnCreate.clear();arrivalCalls.clear();deletedHandles.clear();s_ClientGuidEdidMap.clear();s_MonitorHdrSettingsMap.clear();
}
static int Publish(IndirectDeviceContext& context,const Modes& modes,bool force=false) {
    monitorModes=modes;
#if PR45
    return context.RefreshMonitorModes(true,!force);
#else
    (void)force;
    return context.RefreshMonitorModes(true);
#endif
}
static void Command(IndirectDeviceContext& context,const Modes& modes) {
    wstring param;
    for (const auto& mode:modes) {
        if(!param.empty()) param+=L',';
        param+=to_wstring(get<0>(mode))+L'x'+to_wstring(get<1>(mode))+L'x'+to_wstring(get<2>(mode)/get<3>(mode));
    }
    wrapper.pContext=&context;
    HandleSetModesCommand(param.data());
}
static bool HasMessage(const string& text) {
    return any_of(messages.begin(),messages.end(),[&](const auto& value){return value.find(text)!=string::npos;});
}

int main() {
    int passed=0, failed=0;
    auto test=[&](const char* name,auto body) {
        Reset();
        try { body(); ++passed; cout<<"PASS "<<name<<'\n'; }
        catch(const exception& ex) {
            ++failed; cout<<"FAIL "<<name<<": "<<ex.what()<<'\n';
            for (const auto& message:messages) cout<<"  driver-log: "<<message<<'\n';
            cout<<"  boundary-calls: update="<<updateCalls.size()<<" create="<<createCalls.size()<<" destroy="<<destroyCalls.size()<<'\n';
        }
    };
    test("remove_mode",[]{
        IndirectDeviceContext ctx; ctx.Seed(0,ModesABC); auto before=ctx.m_Monitors.at(0);
        Require(Publish(ctx,AB)==1,"publication failed");
#if PR45
        Require(ctx.m_Monitors.at(0)==before && updateCalls.size()==1 && createCalls.empty(),"did not reuse monitor");
        Require(ctx.m_MonitorDescriptionModes.at(0)==ModesABC && submittedModes.at(0)==AB,"cached description/payload incorrect");
#else
        Require(ctx.m_Monitors.at(0)!=before && createCalls.size()==1,"baseline did not rebuild");
#endif
    });
    test("reorder_nonpreferred_modes",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);auto before=ctx.m_Monitors.at(0);
        Require(Publish(ctx,{A,C,B})==1,"publication failed");
#if PR45
        Require(ctx.m_Monitors.at(0)==before && submittedModes.at(0)==Modes{A,C,B},"reorder did not reuse correctly");
#else
        Require(ctx.m_Monitors.at(0)!=before,"baseline did not rebuild");
#endif
    });
    test("remove_then_restore_creation_mode",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);auto before=ctx.m_Monitors.at(0);
        Require(Publish(ctx,AB)==1 && Publish(ctx,ModesABC)==1,"publication failed");
#if PR45
        Require(ctx.m_Monitors.at(0)==before && updateCalls.size()==2 && createCalls.empty(),"restoration lost creation-time description");
#else
        Require(createCalls.size()==2,"baseline rebuild count incorrect");
#endif
    });
    test("new_mode_rebuilds",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);auto before=ctx.m_Monitors.at(0);
        Require(Publish(ctx,{A,B,D})==1,"publication failed");
        Require(ctx.m_Monitors.at(0)!=before && updateCalls.empty() && createCalls.size()==1,"new mode did not rebuild");
    });
    test("preferred_mode_change_rebuilds",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);auto before=ctx.m_Monitors.at(0);
        Require(Publish(ctx,{B,A,C})==1,"publication failed");
        Require(ctx.m_Monitors.at(0)!=before && updateCalls.empty(),"preferred mode change reused stale description");
    });
    test("update_error_falls_back_to_rebuild",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);auto before=ctx.m_Monitors.at(0);updateStatus[0]=STATUS_UNSUCCESSFUL;
        Require(Publish(ctx,AB)==1,"fallback failed");
        Require(ctx.m_Monitors.at(0)!=before && destroyCalls.size()==1 && createCalls.size()==1,"fallback sequence incorrect");
#if PR45
        Require(updateCalls.size()==1 && HasMessage("in-place target-mode update failed"),"failure branch not exercised");
#endif
    });
    test("unavailable_api_falls_back",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);apiAvailable=false;
        Require(Publish(ctx,AB)==1 && updateCalls.empty() && createCalls.size()==1,"unavailable API did not rebuild");
    });
    test("missing_description_rebuilds",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);ctx.m_MonitorDescriptionModes.clear();
        Require(Publish(ctx,AB)==1 && updateCalls.empty() && createCalls.size()==1,"missing cache did not rebuild");
    });
    test("mixed_monitors_evaluated_individually",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);ctx.Seed(1,{A,C});auto zero=ctx.m_Monitors.at(0),one=ctx.m_Monitors.at(1);
        Require(Publish(ctx,AB)==2,"not all monitors published");
#if PR45
        Require(ctx.m_Monitors.at(0)==zero && ctx.m_Monitors.at(1)!=one && updateCalls==vector<unsigned>{0} && createCalls==vector<unsigned>{1},"per-monitor decisions incorrect");
#else
        Require(createCalls.size()==2,"baseline did not rebuild both");
#endif
    });
    test("forced_refresh_rebuilds",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);
        Require(Publish(ctx,AB,true)==1 && updateCalls.empty() && createCalls.size()==1,"forced refresh reused monitor");
    });
    test("setmodes_command_routes_correctly",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);auto before=ctx.m_Monitors.at(0);Command(ctx,AB);
        Require(monitorModes==AB,"command parser changed modes");
#if PR45
        Require(ctx.m_Monitors.at(0)==before && updateCalls.size()==1,"command did not enable reuse");
#else
        Require(createCalls.size()==1,"baseline command did not rebuild");
#endif
    });
    test("fallback_creation_failure_preserves_retry_parameters",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);updateStatus[0]=STATUS_UNSUCCESSFUL;failCreate.insert(0);
        Require(Publish(ctx,AB)==0 && ctx.m_Monitors.empty() && ctx.m_MonitorCreationParams.count(0)==1,"failure bookkeeping incorrect");
        failCreate.clear();Require(Publish(ctx,AB,true)==1 && ctx.m_Monitors.count(0)==1,"forced retry did not recover");
    });
    test("fallback_departure_failure_retains_monitor",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);auto before=ctx.m_Monitors.at(0);updateStatus[0]=STATUS_UNSUCCESSFUL;failDestroy.insert(0);
        Require(Publish(ctx,AB)==0 && ctx.m_Monitors.at(0)==before && createCalls.empty(),"departure failure discarded monitor");
    });
    test("second_monitor_update_error_is_isolated",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);ctx.Seed(1,ModesABC);updateStatus[1]=STATUS_UNSUCCESSFUL;
        Require(Publish(ctx,AB)==2,"mixed update/fallback failed");
#if PR45
        Require(updateCalls==vector<unsigned>{0,1} && createCalls==vector<unsigned>{1},"fallback affected healthy monitor");
#else
        Require(createCalls.size()==2,"baseline did not rebuild both");
#endif
    });
    test("rebuild_preserves_creation_parameters",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);
        auto& params=ctx.m_MonitorCreationParams.at(0);
        params.hasClientGuid=true;params.clientGuid.Data1=0x12345678;
        params.maxNits=777;params.minNits=0.02f;params.maxFALL=333;params.widthCm=52;params.heightCm=29;
        updateStatus[0]=STATUS_UNSUCCESSFUL;Require(Publish(ctx,AB)==1,"fallback failed");
        const auto& actual=ctx.m_MonitorCreationParams.at(0);
        Require(actual.hasClientGuid && actual.clientGuid.Data1==0x12345678 && actual.maxNits==777 && actual.minNits==0.02f && actual.maxFALL==333 && actual.widthCm==52 && actual.heightCm==29,"fallback changed identity/HDR/dimensions");
    });
    test("one_failed_recreation_does_not_stop_other_monitors",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);ctx.Seed(1,ModesABC);
        updateStatus[0]=STATUS_UNSUCCESSFUL;failCreate.insert(0);
        Require(Publish(ctx,AB)==1 && ctx.m_Monitors.count(0)==0 && ctx.m_Monitors.count(1)==1,"one failure prevented publication to other monitor");
        Require(ctx.m_MonitorCreationParams.count(0)==1,"failed monitor retry data lost");
    });
    // Desired retry contract, deliberately checked against PR and parent to attribute failures.
    test("same_setmodes_recovers_after_transient_create_failure",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);updateStatus[0]=STATUS_UNSUCCESSFUL;failCreate.insert(0);
        Command(ctx,AB);Require(ctx.m_Monitors.empty(),"failure injection did not fire");
        failCreate.clear();updateStatus.clear();createCalls.clear();Command(ctx,AB);
        Require(ctx.m_Monitors.count(0)==1,"same SETMODES never retried missing monitor; only REFRESHMODES/different list recovers");
    });
    test("same_setmodes_retries_after_transient_departure_failure",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);auto before=ctx.m_Monitors.at(0);updateStatus[0]=STATUS_UNSUCCESSFUL;failDestroy.insert(0);
        Command(ctx,{A,D});Require(ctx.m_Monitors.at(0)==before,"failure injection did not fire");
        failDestroy.clear();updateStatus.clear();destroyCalls.clear();Command(ctx,{A,D});
        Require(ctx.m_Monitors.at(0)!=before,"same SETMODES updates target modes without republishing a description missing D");
    });
    test("pending_retry_recreates_quiesced_monitor_even_if_description_covers_modes",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);auto before=ctx.m_Monitors.at(0);
        updateStatus[0]=STATUS_UNSUCCESSFUL;failDestroy.insert(0);Command(ctx,AB);
        failDestroy.clear();updateStatus.clear();updateCalls.clear();Command(ctx,AB);
        Require(ctx.m_Monitors.at(0)!=before && updateCalls.empty(),"retry used in-place update on a quiesced monitor");
        Require(ctx.m_PendingMonitorModeRecreation.empty(),"successful recreation left retry flag");
    });
    test("repeated_failures_keep_retry_parameters",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);failCreate.insert(0);
        for(int i=0;i<3;++i) { Command(ctx,{A,D});Require(ctx.m_MonitorCreationParams.count(0)==1,"retry parameters lost"); }
        failCreate.clear();Command(ctx,{A,D});
        Require(createCalls.size()==4 && ctx.m_Monitors.count(0)==1,"same list did not recover after repeated failures");
    });
    test("failed_arrival_is_retried",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);failArrival.insert(0);Command(ctx,{A,D});
        Require(ctx.m_Monitors.empty() && deletedHandles==vector<unsigned>{0},"failed arrival was not cleaned up");
        failArrival.clear();Command(ctx,{A,D});
        Require(ctx.m_Monitors.count(0)==1 && ctx.m_MonitorDescriptionModes.at(0)==Modes{A,D},"arrival failure was not retried");
    });
    test("partial_retry_only_recreates_failed_monitor",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);ctx.Seed(1,ModesABC);
        failCreate.insert(1);Command(ctx,{A,D});auto healthy=ctx.m_Monitors.at(0);
        failCreate.clear();createCalls.clear();destroyCalls.clear();Command(ctx,{A,D});
        Require(createCalls==vector<unsigned>{1} && ctx.m_Monitors.at(0)==healthy,"retry recreated a healthy monitor");
        Require(ctx.m_Monitors.count(1)==1 && ctx.m_PendingMonitorModeRecreation.empty(),"failed monitor did not recover");
    });
    test("old_os_partial_retry_preserves_healthy_monitor",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);ctx.Seed(1,ModesABC);apiAvailable=false;
        failCreate.insert(1);Command(ctx,{A,D});auto healthy=ctx.m_Monitors.at(0);
        failCreate.clear();createCalls.clear();Command(ctx,{A,D});
        Require(createCalls==vector<unsigned>{1} && ctx.m_Monitors.at(0)==healthy && ctx.m_Monitors.count(1)==1,"old OS retry rebuilt healthy monitor or missed recovery");
    });
    test("explicit_destroy_cancels_missing_monitor_retry",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);failCreate.insert(0);Command(ctx,{A,D});
        Require(ctx.DestroyMonitor(0),"cancel failed");failCreate.clear();createCalls.clear();Command(ctx,{A,D});
        Require(createCalls.empty() && ctx.m_MonitorCreationParams.empty() && ctx.m_PendingMonitorModeRecreation.empty(),"explicitly destroyed monitor was resurrected");
    });
    test("destroy_all_cancels_pending_missing_monitors",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);ctx.Seed(1,ModesABC);failCreate.insert(1);Command(ctx,{A,D});
        ctx.DestroyAllMonitors();failCreate.clear();createCalls.clear();Command(ctx,{A,D});
        Require(ctx.m_Monitors.empty() && ctx.m_MonitorCreationParams.empty() && ctx.m_PendingMonitorModeRecreation.empty() && createCalls.empty(),"destroy-all left a retry that resurrects a monitor");
    });
    test("successful_retry_returns_to_in_place_updates",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);updateStatus[0]=STATUS_UNSUCCESSFUL;failCreate.insert(0);Command(ctx,AB);
        updateStatus.clear();failCreate.clear();Command(ctx,AB);auto recovered=ctx.m_Monitors.at(0);createCalls.clear();
        Command(ctx,{A});Command(ctx,AB);
        Require(createCalls.empty() && ctx.m_Monitors.at(0)==recovered && ctx.m_PendingMonitorModeRecreation.empty(),"successful retry disabled later reuse");
    });
    test("new_request_supersedes_pending_modes",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);failCreate.insert(0);Command(ctx,{A,D});
        failCreate.clear();Command(ctx,{A,C});
        Require(ctx.m_MonitorDescriptionModes.at(0)==Modes{A,C} && ctx.m_PendingMonitorModeRecreation.empty(),"retry published obsolete modes");
    });
    test("unchanged_list_update_failure_gets_retried",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);auto before=ctx.m_Monitors.at(0);updateStatus[0]=STATUS_UNSUCCESSFUL;
        Command(ctx,ModesABC);updateStatus.clear();Command(ctx,ModesABC);
        Require(ctx.m_Monitors.at(0)!=before && ctx.m_PendingMonitorModeRecreation.empty(),"unchanged-list update failure did not schedule recreation");
    });
    test("exception_during_creation_keeps_retry_state",[]{
        IndirectDeviceContext ctx;ctx.Seed(0,ModesABC);throwOnCreate.insert(0);bool thrown=false;
        try { Command(ctx,{A,D}); } catch(const runtime_error&) { thrown=true; }
        Require(thrown,"exception injection did not fire");throwOnCreate.clear();Command(ctx,{A,D});
        Require(ctx.m_Monitors.count(0)==1 && ctx.m_PendingMonitorModeRecreation.empty(),"exception lost retry state");
    });
    cout<<"RESULT variant="<<(PR45?"PR45":"BASE")<<" passed="<<passed<<" failed="<<failed<<'\n';
    return failed?1:0;
}
