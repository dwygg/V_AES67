// AES67 Virtual ASIO — Step 1: Audio thread only (no network)
// Verifies audio thread works before adding network.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <atomic>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ws2_32.lib")

// ===== TX Ring Buffer (SPSC lock-free) =====
class RingBuffer {
    static constexpr size_t N=65536, M=N-1;
    BYTE m_buf[N]={}; size_t m_head=0,m_tail=0;
public:
    size_t Write(const BYTE* s,size_t n){size_t u=(m_head-m_tail)&M,f=N-u-1;if(n>f)n=f;if(!n)return 0;size_t c=N-m_head;if(n<=c)memcpy(m_buf+m_head,s,n);else{memcpy(m_buf+m_head,s,c);memcpy(m_buf,s+c,n-c);}m_head=(m_head+n)&M;return n;}
    size_t Read(BYTE* d,size_t n){size_t a=(m_head-m_tail)&M;if(!a)return 0;if(n>a)n=a;size_t c=N-m_tail;if(n<=c)memcpy(d,m_buf+m_tail,n);else{memcpy(d,m_buf+m_tail,c);memcpy(d+c,m_buf,n-c);}m_tail=(m_tail+n)&M;return n;}
    void Reset(){m_head=m_tail=0;}
};

// ===== Network State =====
static constexpr char kMcast[]="239.69.1.128";
static constexpr uint16_t kPort=5004;
static SOCKET g_sockTx=INVALID_SOCKET;
static sockaddr_in g_mcast={};
static uint32_t g_ssrc=0;
static std::atomic<uint32_t> g_rtpSeq{0},g_rtpTs{0};
static RingBuffer g_txRing;
static HANDLE g_hTxThread=nullptr;

static bool InitTxSocket(){
    WSADATA wsa;WSAStartup(MAKEWORD(2,2),&wsa);
    g_sockTx=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(g_sockTx==INVALID_SOCKET)return false;
    int tos=0xB8;setsockopt(g_sockTx,IPPROTO_IP,IP_TOS,(char*)&tos,sizeof(tos));
    DWORD ttl=32;setsockopt(g_sockTx,IPPROTO_IP,IP_MULTICAST_TTL,(char*)&ttl,sizeof(ttl));
    g_mcast.sin_family=AF_INET;g_mcast.sin_port=htons(kPort);
    inet_pton(AF_INET,kMcast,&g_mcast.sin_addr);
    srand((unsigned)GetTickCount());
    g_ssrc=(uint32_t)(((uint64_t)rand()<<16)^rand());
    g_rtpSeq.store((uint16_t)rand());g_rtpTs.store((uint32_t)rand());
    return true;
}

static void RtpHeader(BYTE*p,uint16_t seq,uint32_t ts,uint32_t ss){
    p[0]=0x80;p[1]=0x61;p[2]=seq>>8;p[3]=seq&0xFF;
    p[4]=ts>>24;p[5]=ts>>16;p[6]=ts>>8;p[7]=ts&0xFF;
    p[8]=ss>>24;p[9]=ss>>16;p[10]=ss>>8;p[11]=ss&0xFF;
}

static DWORD WINAPI TxThread(LPVOID){
    BYTE pkt[1500];
    while(g_running.load()){
        size_t r=g_txRing.Read(pkt+12,288);
        if(r<288){if(WaitForSingleObject(g_hStopEvent,1)==WAIT_OBJECT_0)break;continue;}
        uint16_t seq=(uint16_t)g_rtpSeq.fetch_add(1);
        uint32_t ts=g_rtpTs.fetch_add(48);
        RtpHeader(pkt,seq,ts,g_ssrc);
        sendto(g_sockTx,(char*)pkt,300,0,(sockaddr*)&g_mcast,sizeof(g_mcast));
    }
    return 0;
}

typedef long ASIOError; typedef long ASIOBool; typedef double ASIOSampleRate;
struct ASIOSamples{unsigned long hi,lo;}; struct ASIOTimeStamp{unsigned long hi,lo;};
enum{ASIOFalse=0,ASIOTrue=1};
enum{ASE_OK=0,ASE_NotPresent=-1000,ASE_HWMalfunction=-999,ASE_InvalidParameter=-998,ASE_InvalidMode=-997,ASE_NoClock=-995,ASE_NoMemory=-994};
enum{ASIOSTInt32LSB=18};
#pragma pack(push,4)
struct ASIODriverInfo{long asioVersion,driverVersion;char name[32],errorMessage[124];void*sysRef;};
struct ASIOBufferInfo{ASIOBool isInput;long channelNum;void*buffers[2];};
struct ASIOChannelInfo{long channel,isInput,isActive,channelGroup,type;char name[32];};
struct ASIOClockSource{long index,associatedChannel,associatedGroup;ASIOBool isCurrentSource;char name[32];};
struct ASIOCallbacks{void(*bufferSwitch)(long,ASIOBool);void(*sampleRateDidChange)(ASIOSampleRate);long(*asioMessage)(long,long,void*,double*);void(*bufferSwitchTimeInfo)(void*,long,ASIOBool);};
#pragma pack(pop)

static const GUID CLSID_AES67={0xA1B2C3D4,0xE5F6,0x7890,{0xAB,0xCD,0xEF,0x12,0x34,0x56,0x78,0x90}};
struct IASIO:IUnknown{
    virtual ASIOBool init(void*)=0;
    virtual void getDriverName(char*)=0;
    virtual long getDriverVersion()=0;
    virtual void getErrorMessage(char*)=0;
    virtual ASIOError start()=0;
    virtual ASIOError stop()=0;
    virtual ASIOError getChannels(long*,long*)=0;
    virtual ASIOError getLatencies(long*,long*)=0;
    virtual ASIOError getBufferSize(long*,long*,long*,long*)=0;
    virtual ASIOError canSampleRate(ASIOSampleRate)=0;
    virtual ASIOError getSampleRate(ASIOSampleRate*)=0;
    virtual ASIOError setSampleRate(ASIOSampleRate)=0;
    virtual ASIOError getClockSources(ASIOClockSource*,long*)=0;
    virtual ASIOError setClockSource(long)=0;
    virtual ASIOError getSamplePosition(ASIOSamples*,ASIOTimeStamp*)=0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo*)=0;
    virtual ASIOError createBuffers(ASIOBufferInfo*,long,long,ASIOCallbacks*)=0;
    virtual ASIOError disposeBuffers()=0;
    virtual ASIOError controlPanel()=0;
    virtual ASIOError future(long,void*)=0;
    virtual ASIOError outputReady()=0;
};

static constexpr long kNumIn=2,kNumOut=2,kBufSize=64;
static constexpr double kSampleRate=48000.0;
static ASIOCallbacks g_cb={};
static ASIOBufferInfo*g_bi=nullptr;
static long g_doubleIdx=0;
static std::atomic<bool> g_running{false},g_initialized{false};
static HANDLE g_hAudioThread=nullptr,g_hStopEvent=nullptr;

static DWORD WINAPI AudioThread(LPVOID){
    timeBeginPeriod(1);
    HANDLE hTimer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    if(!hTimer) hTimer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
    LONGLONG periodHns=(LONGLONG)((double)kBufSize/kSampleRate*1e7);
    if(periodHns<5000) periodHns=5000;
    LARGE_INTEGER due;due.QuadPart=-periodHns;
    SetWaitableTimer(hTimer,&due,0,nullptr,nullptr,FALSE);
    while(g_running.load()){
        HANDLE wh[2]={hTimer,g_hStopEvent};
        if(WaitForMultipleObjects(2,wh,FALSE,INFINITE)!=WAIT_OBJECT_0) break;
        long idx=g_doubleIdx;
        // Fill inputs with silence
        for(long ch=0;ch<kNumIn;ch++){if(g_bi&&g_bi[ch].buffers[idx])memset(g_bi[ch].buffers[idx],0,kBufSize*sizeof(int32_t));}
        // Call DAW
        if(g_cb.bufferSwitch) g_cb.bufferSwitch(idx,ASIOFalse);
        // Read DAW outputs → TX ring buffer (int32 → L24)
        for(long ch=0;ch<kNumOut;ch++){
            long bi=kNumIn+ch;
            if(!g_bi||!g_bi[bi].buffers[idx])continue;
            int32_t*src=(int32_t*)g_bi[bi].buffers[idx];
            BYTE pcm[288];memset(pcm,0,288);
            for(long f=0;f<kBufSize&&f<48;f++){
                int32_t v=src[f*kNumOut+ch];
                if(v>8388607)v=8388607;if(v<-8388608)v=-8388608;
                pcm[f*3]=v&0xFF;pcm[f*3+1]=(v>>8)&0xFF;pcm[f*3+2]=(v>>16)&0xFF;
            }
            g_txRing.Write(pcm,288);
        }
        g_doubleIdx^=1;
        due.QuadPart=-periodHns;SetWaitableTimer(hTimer,&due,0,nullptr,nullptr,FALSE);
    }
    CancelWaitableTimer(hTimer);CloseHandle(hTimer);
    timeEndPeriod(1);
    return 0;
}

class AES67Asio:public IASIO{ULONG m_ref=1;
public:
    STDMETHOD(QueryInterface)(REFIID riid,void**ppv)override{if(!ppv)return E_POINTER;if(riid==IID_IUnknown||riid==CLSID_AES67){*ppv=this;AddRef();return S_OK;}*ppv=nullptr;return E_NOINTERFACE;}
    STDMETHOD_(ULONG,AddRef)()override{return++m_ref;}
    STDMETHOD_(ULONG,Release)()override{if(--m_ref==0){delete this;return 0;}return m_ref;}
    ASIOBool init(void*)override{g_initialized.store(true);return ASIOTrue;}
    void getDriverName(char*n)override{strcpy_s(n,32,"AES67 Virtual ASIO");}
    long getDriverVersion()override{return 1;}
    void getErrorMessage(char*s)override{s[0]=0;}
    ASIOError start()override;
    ASIOError stop()override;
    ASIOError getChannels(long*i,long*o)override{if(i)*i=kNumIn;if(o)*o=kNumOut;return ASE_OK;}
    ASIOError getLatencies(long*il,long*ol)override{if(il)*il=96;if(ol)*ol=96;return ASE_OK;}
    ASIOError getBufferSize(long*a,long*b,long*c,long*d)override{if(a)*a=16;if(b)*b=2048;if(c)*c=kBufSize;if(d)*d=16;return ASE_OK;}
    ASIOError canSampleRate(ASIOSampleRate sr)override{if(sr>=44099.0&&sr<=44101.0)return ASE_OK;if(sr>=47999.0&&sr<=48001.0)return ASE_OK;if(sr>=95999.0&&sr<=96001.0)return ASE_OK;return ASE_NoClock;}
    ASIOError getSampleRate(ASIOSampleRate*s)override{if(s)*s=kSampleRate;return ASE_OK;}
    ASIOError setSampleRate(ASIOSampleRate s)override{return canSampleRate(s);}
    ASIOError getClockSources(ASIOClockSource*c,long*n)override{if(n)*n=1;if(c){memset(c,0,sizeof(*c));c->isCurrentSource=ASIOTrue;strcpy_s(c->name,"Internal");}return ASE_OK;}
    ASIOError setClockSource(long r)override{return r==0?ASE_OK:ASE_InvalidParameter;}
    ASIOError getSamplePosition(ASIOSamples*s,ASIOTimeStamp*t)override{if(s)memset(s,0,sizeof(*s));if(t)memset(t,0,sizeof(*t));return ASE_OK;}
    ASIOError getChannelInfo(ASIOChannelInfo*info)override{if(!info)return ASE_InvalidParameter;long m=info->isInput?kNumIn:kNumOut;if(info->channel<0||info->channel>=m)return ASE_InvalidParameter;info->channelGroup=0;info->isActive=ASIOTrue;info->type=ASIOSTInt32LSB;sprintf_s(info->name,"%s %ld",info->isInput?"In":"Out",info->channel+1);return ASE_OK;}
    ASIOError createBuffers(ASIOBufferInfo*infos,long nCh,long bufSize,ASIOCallbacks*cb)override{if(!infos||!cb||nCh<1||bufSize<1)return ASE_InvalidParameter;for(long i=0;i<nCh;i++){SIZE_T sz=(SIZE_T)bufSize*4;infos[i].buffers[0]=GlobalAlloc(GPTR,sz);infos[i].buffers[1]=GlobalAlloc(GPTR,sz);if(!infos[i].buffers[0]||!infos[i].buffers[1])return ASE_NoMemory;}g_bi=infos;g_cb=*cb;return ASE_OK;}
    ASIOError disposeBuffers()override{g_bi=nullptr;return ASE_OK;}
    ASIOError controlPanel()override{return ASE_NotPresent;}
    ASIOError future(long,void*)override{return ASE_NotPresent;}
    ASIOError outputReady()override{return ASE_OK;}
};
static AES67Asio*g_drv=new AES67Asio();

ASIOError AES67Asio::start(){if(!g_initialized.load()||g_running.load())return ASE_NotPresent;g_txRing.Reset();InitTxSocket();g_running.store(true);g_doubleIdx=0;ResetEvent(g_hStopEvent);g_hAudioThread=CreateThread(nullptr,0,AudioThread,nullptr,0,nullptr);g_hTxThread=CreateThread(nullptr,0,TxThread,nullptr,0,nullptr);return ASE_OK;}
ASIOError AES67Asio::stop(){g_running.store(false);SetEvent(g_hStopEvent);if(g_hTxThread){WaitForSingleObject(g_hTxThread,3000);CloseHandle(g_hTxThread);g_hTxThread=nullptr;}if(g_hAudioThread){WaitForSingleObject(g_hAudioThread,3000);CloseHandle(g_hAudioThread);g_hAudioThread=nullptr;}if(g_sockTx!=INVALID_SOCKET){closesocket(g_sockTx);g_sockTx=INVALID_SOCKET;}WSACleanup();return ASE_OK;}

struct AES67Factory:IClassFactory{
    STDMETHOD(QueryInterface)(REFIID riid,void**ppv)override{if(!ppv)return E_POINTER;if(riid==IID_IUnknown||riid==IID_IClassFactory){*ppv=this;return S_OK;}*ppv=nullptr;return E_NOINTERFACE;}
    STDMETHOD_(ULONG,AddRef)()override{return 1;}STDMETHOD_(ULONG,Release)()override{return 1;}
    STDMETHOD(CreateInstance)(IUnknown*o,REFIID riid,void**ppv)override{if(o)return CLASS_E_NOAGGREGATION;return g_drv->QueryInterface(riid,ppv);}
    STDMETHOD(LockServer)(BOOL)override{return S_OK;}
};
extern"C" HRESULT MyDllGetClassObject(REFCLSID clsid,REFIID iid,void**ppv){if(!ppv)return E_POINTER;if(clsid==CLSID_AES67){static AES67Factory f;return f.QueryInterface(iid,ppv);}*ppv=nullptr;return CLASS_E_CLASSNOTAVAILABLE;}
extern"C" HRESULT MyDllCanUnloadNow(){return S_FALSE;}

BOOL WINAPI DllMain(HINSTANCE hDll,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){g_hStopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);DisableThreadLibraryCalls((HMODULE)hDll);}else if(reason==DLL_PROCESS_DETACH){if(g_running.load()){g_running.store(false);SetEvent(g_hStopEvent);Sleep(100);}CloseHandle(g_hStopEvent);}return TRUE;}

extern"C"{
__declspec(dllexport) ASIOError ASIOInit(ASIODriverInfo*i){if(!i)return ASE_InvalidParameter;i->asioVersion=2;i->driverVersion=1;i->errorMessage[0]=0;strcpy_s(i->name,"AES67 Virtual ASIO");i->sysRef=nullptr;g_initialized.store(true);return ASE_OK;}
__declspec(dllexport) ASIOError ASIOExit(){if(g_running.load())g_drv->stop();g_initialized.store(false);return ASE_OK;}
__declspec(dllexport) ASIOError ASIOStart(){return g_drv->start();}
__declspec(dllexport) ASIOError ASIOStop(){return g_drv->stop();}
__declspec(dllexport) ASIOError ASIOGetChannels(long*a,long*b){return g_drv->getChannels(a,b);}
__declspec(dllexport) ASIOError ASIOGetBufferSize(long*a,long*b,long*c,long*d){return g_drv->getBufferSize(a,b,c,d);}
__declspec(dllexport) ASIOError ASIOCanSampleRate(ASIOSampleRate r){return g_drv->canSampleRate(r);}
__declspec(dllexport) ASIOError ASIOGetSampleRate(ASIOSampleRate*r){return g_drv->getSampleRate(r);}
__declspec(dllexport) ASIOError ASIOSetSampleRate(ASIOSampleRate r){return g_drv->setSampleRate(r);}
__declspec(dllexport) ASIOError ASIOGetLatencies(long*a,long*b){return g_drv->getLatencies(a,b);}
__declspec(dllexport) ASIOError ASIOGetClockSources(ASIOClockSource*c,long*n){return g_drv->getClockSources(c,n);}
__declspec(dllexport) ASIOError ASIOSetClockSource(long r){return g_drv->setClockSource(r);}
__declspec(dllexport) ASIOError ASIOGetSamplePosition(ASIOSamples*s,ASIOTimeStamp*t){return g_drv->getSamplePosition(s,t);}
__declspec(dllexport) ASIOError ASIOGetChannelInfo(ASIOChannelInfo*i){return g_drv->getChannelInfo(i);}
__declspec(dllexport) ASIOError ASIOCreateBuffers(ASIOBufferInfo*bi,long nc,long bs,ASIOCallbacks*cb){return g_drv->createBuffers(bi,nc,bs,cb);}
__declspec(dllexport) ASIOError ASIODisposeBuffers(){return g_drv->disposeBuffers();}
__declspec(dllexport) ASIOError ASIOControlPanel(){return g_drv->controlPanel();}
__declspec(dllexport) void* ASIOFuture(long s,void*o){static long r;r=(long)g_drv->future(s,o);return&r;}
__declspec(dllexport) ASIOError ASIOOutputReady(){return g_drv->outputReady();}
}
