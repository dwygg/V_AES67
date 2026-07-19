// AES67 Virtual ASIO — minimal working baseline
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <cstdlib>

typedef long ASIOError; typedef long ASIOBool; typedef double ASIOSampleRate;
struct ASIOSamples{unsigned long hi,lo;}; struct ASIOTimeStamp{unsigned long hi,lo;};
enum{ASIOFalse=0,ASIOTrue=1};
enum{ASE_OK=0,ASE_NotPresent=-1000,ASE_InvalidParameter=-998,ASE_NoClock=-995,ASE_NoMemory=-994};
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
    virtual ASIOBool init(void*)=0;virtual void getDriverName(char*)=0;virtual long getDriverVersion()=0;virtual void getErrorMessage(char*)=0;
    virtual ASIOError start()=0;virtual ASIOError stop()=0;virtual ASIOError getChannels(long*,long*)=0;virtual ASIOError getLatencies(long*,long*)=0;
    virtual ASIOError getBufferSize(long*,long*,long*,long*)=0;virtual ASIOError canSampleRate(ASIOSampleRate)=0;virtual ASIOError getSampleRate(ASIOSampleRate*)=0;
    virtual ASIOError setSampleRate(ASIOSampleRate)=0;virtual ASIOError getClockSources(ASIOClockSource*,long*)=0;virtual ASIOError setClockSource(long)=0;
    virtual ASIOError getSamplePosition(ASIOSamples*,ASIOTimeStamp*)=0;virtual ASIOError getChannelInfo(ASIOChannelInfo*)=0;
    virtual ASIOError createBuffers(ASIOBufferInfo*,long,long,ASIOCallbacks*)=0;virtual ASIOError disposeBuffers()=0;virtual ASIOError controlPanel()=0;
    virtual ASIOError future(long,void*)=0;virtual ASIOError outputReady()=0;
};

static constexpr long kNumIn=2,kNumOut=2,kBufSize=48;  // 48 = 1 AES67 packet (1ms @ 48kHz)
static constexpr double kSampleRate=48000.0;
static std::atomic<bool> g_running{false};
static HANDLE g_hAudio=nullptr,g_hTx=nullptr,g_hRx=nullptr,g_hStop=nullptr;
static long g_doubleIdx=0;
static ASIOCallbacks g_cb={};static ASIOBufferInfo*g_bi=nullptr;

// --- Network ---
static SOCKET g_sockTx=INVALID_SOCKET,g_sockRx=INVALID_SOCKET;
static sockaddr_in g_mcast={};static uint32_t g_ssrc=0;
static std::atomic<uint32_t> g_rtpSeq{0},g_rtpTs{0};

// --- Ring Buffer ---
static constexpr size_t RB_N=65536,RB_M=RB_N-1;
static BYTE g_rb[RB_N];static std::atomic<size_t> g_rbH{0},g_rbT{0};

// --- Jitter Buffer ---
static constexpr size_t JB_S=256,JB_M=255,JB_P=288;
static BYTE g_jb_slots[JB_S][JB_P];static bool g_jb_v[JB_S]={};static uint16_t g_jb_q[JB_S]={};
static std::atomic<uint16_t> g_jb_r{0};static bool g_jb_sync=false;

// --- RTP ---
static void RTP_Hdr(BYTE*p,uint16_t s,uint32_t t,uint32_t ss){p[0]=0x80;p[1]=0x61;p[2]=s>>8;p[3]=s&0xFF;p[4]=t>>24;p[5]=t>>16;p[6]=t>>8;p[7]=t&0xFF;p[8]=ss>>24;p[9]=ss>>16;p[10]=ss>>8;p[11]=ss&0xFF;}

// --- Threads ---
static DWORD WINAPI TxThread(LPVOID){BYTE b[1500],z[288]={};while(g_running.load()){size_t head=g_rbH.load(std::memory_order_acquire),tail=g_rbT.load(std::memory_order_acquire);size_t a=(head-tail)&RB_M,n=a<288?a:288,c=RB_N-tail;if(n<=c)memcpy(b+12,g_rb+tail,n);else{memcpy(b+12,g_rb+tail,c);memcpy(b+12+c,g_rb,n-c);}g_rbT.store((tail+n)&RB_M,std::memory_order_release);if(n<288)memcpy(b+12+n,z,288-n);uint16_t s=(uint16_t)g_rtpSeq.fetch_add(1);uint32_t t=g_rtpTs.fetch_add(48);RTP_Hdr(b,s,t,g_ssrc);sendto(g_sockTx,(char*)b,300,0,(sockaddr*)&g_mcast,sizeof(g_mcast));Sleep(1);}return 0;}
static DWORD WINAPI RxThread(LPVOID){BYTE b[1500];while(g_running.load()){sockaddr_in f;int fl=sizeof(f);int n=recvfrom(g_sockRx,(char*)b,1500,0,(sockaddr*)&f,&fl);if(n==SOCKET_ERROR){if(!g_running.load())break;continue;}if(n<12||(b[0]>>6)!=2||(b[1]&0x7F)!=97)continue;int pl=n-12;if(pl<288)continue;uint16_t seq=((uint16_t)b[2]<<8)|b[3];if(!g_jb_sync){g_jb_r.store(seq,std::memory_order_release);g_jb_sync=true;}int16_t a=(int16_t)(seq-g_jb_r.load(std::memory_order_acquire));if(a<0||a>=(int16_t)JB_S)continue;size_t i=seq&JB_M;g_jb_v[i]=true;g_jb_q[i]=seq;memcpy(g_jb_slots[i],b+12,JB_P);}return 0;}
static DWORD WINAPI AudioThread(LPVOID){timeBeginPeriod(1);HANDLE t=CreateWaitableTimerExW(nullptr,nullptr,2,TIMER_ALL_ACCESS);if(!t)t=CreateWaitableTimerW(nullptr,FALSE,nullptr);LONGLONG ph=(LONGLONG)((double)kBufSize/kSampleRate*1e7);LARGE_INTEGER d;d.QuadPart=-ph;SetWaitableTimer(t,&d,0,nullptr,nullptr,FALSE);while(g_running.load()){HANDLE w[2]={t,g_hStop};if(WaitForMultipleObjects(2,w,FALSE,INFINITE)!=WAIT_OBJECT_0)break;long idx=g_doubleIdx;
// ---- RX: fill ASIO input buffers from jitter (L24→int32, per-channel non-interleaved) ----
for(long ch=0;ch<kNumIn;ch++){if(!g_bi||!g_bi[ch].buffers[idx])continue;int32_t*dst=(int32_t*)g_bi[ch].buffers[idx];BYTE jb[288];uint16_t jr=g_jb_r.load(std::memory_order_acquire);if(g_jb_v[jr&JB_M]&&g_jb_q[jr&JB_M]==jr){memcpy(jb,g_jb_slots[jr&JB_M],JB_P);g_jb_v[jr&JB_M]=false;}else{memset(jb,0,JB_P);}g_jb_r.store(jr+1,std::memory_order_release);for(long s=0;s<kBufSize;s++){long o=s*6+ch*3;dst[s]=jb[o]|(jb[o+1]<<8)|((int32_t)(int8_t)jb[o+2]<<16);}}
// ---- Call DAW ----
if(g_cb.bufferSwitch)g_cb.bufferSwitch(idx,ASIOFalse);
// ---- TX: read ASIO output buffers → L24 → ring buffer (per-channel non-interleaved) ----
BYTE pcm[288];memset(pcm,0,288);
for(long ch=0;ch<kNumOut;ch++){long bi=kNumIn+ch;if(!g_bi||!g_bi[bi].buffers[idx])continue;int32_t*src=(int32_t*)g_bi[bi].buffers[idx];for(long s=0;s<kBufSize;s++){int32_t v=src[s];if(v>8388607)v=8388607;if(v<-8388608)v=-8388608;long o=(s*kNumOut+ch)*3;pcm[o]=v&0xFF;pcm[o+1]=(v>>8)&0xFF;pcm[o+2]=(v>>16)&0xFF;}}
// Write 288B (48 frames × 2ch × 3B L24) to TX ring
size_t head=g_rbH.load(std::memory_order_acquire),tail=g_rbT.load(std::memory_order_acquire);size_t u=(head-tail)&RB_M,fr=RB_N-u-1,n=288<fr?288:fr,c=RB_N-head;if(n<=c)memcpy(g_rb+head,pcm,n);else{memcpy(g_rb+head,pcm,c);memcpy(g_rb,pcm+c,n-c);}g_rbH.store((head+n)&RB_M,std::memory_order_release);
g_doubleIdx^=1;d.QuadPart=-ph;SetWaitableTimer(t,&d,0,nullptr,nullptr,FALSE);}CancelWaitableTimer(t);CloseHandle(t);timeEndPeriod(1);return 0;}

class AES67Asio:public IASIO{ULONG m_ref=1;
public:
    STDMETHOD(QueryInterface)(REFIID riid,void**ppv)override{if(!ppv)return E_POINTER;if(riid==IID_IUnknown||riid==CLSID_AES67){*ppv=this;AddRef();return S_OK;}*ppv=nullptr;return E_NOINTERFACE;}
    STDMETHOD_(ULONG,AddRef)()override{return++m_ref;}
    STDMETHOD_(ULONG,Release)()override{return --m_ref;} // singleton — never delete
    ASIOBool init(void*)override{return ASIOTrue;}
    void getDriverName(char*n)override{strcpy_s(n,32,"AES67 Virtual ASIO");}
    long getDriverVersion()override{return 1;}
    void getErrorMessage(char*s)override{s[0]=0;}
    ASIOError start()override{
        WSADATA wsa;WSAStartup(MAKEWORD(2,2),&wsa);
        g_sockTx=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);int tos=0xB8;setsockopt(g_sockTx,IPPROTO_IP,IP_TOS,(char*)&tos,sizeof(tos));DWORD ttl=32;setsockopt(g_sockTx,IPPROTO_IP,IP_MULTICAST_TTL,(char*)&ttl,sizeof(ttl));
        g_mcast.sin_family=AF_INET;g_mcast.sin_port=htons(5004);inet_pton(AF_INET,"239.69.1.128",&g_mcast.sin_addr);
        g_sockRx=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);int reuse=1;setsockopt(g_sockRx,SOL_SOCKET,SO_REUSEADDR,(char*)&reuse,sizeof(reuse));
        sockaddr_in l={};l.sin_family=AF_INET;l.sin_port=htons(5004);bind(g_sockRx,(sockaddr*)&l,sizeof(l));
        ip_mreq mr={};inet_pton(AF_INET,"239.69.1.128",&mr.imr_multiaddr);setsockopt(g_sockRx,IPPROTO_IP,IP_ADD_MEMBERSHIP,(char*)&mr,sizeof(mr));
        srand((unsigned)GetTickCount());g_ssrc=(uint32_t)(((uint64_t)rand()<<16)^rand());g_rtpSeq.store((uint16_t)rand());g_rtpTs.store((uint32_t)rand());
        g_rbH.store(0);g_rbT.store(0);g_doubleIdx=0;g_jb_sync=false;g_jb_r.store(0);memset(g_jb_v,0,sizeof(g_jb_v));
        g_running.store(true);ResetEvent(g_hStop);
        g_hTx=CreateThread(nullptr,0,TxThread,nullptr,0,nullptr);
        g_hRx=CreateThread(nullptr,0,RxThread,nullptr,0,nullptr);
        g_hAudio=CreateThread(nullptr,0,AudioThread,nullptr,0,nullptr);
        return ASE_OK;
    }
    ASIOError stop()override{
        g_running.store(false);SetEvent(g_hStop);Sleep(100);
        if(g_hTx){WaitForSingleObject(g_hTx,3000);CloseHandle(g_hTx);g_hTx=nullptr;}
        if(g_hRx){WaitForSingleObject(g_hRx,3000);CloseHandle(g_hRx);g_hRx=nullptr;}
        if(g_hAudio){WaitForSingleObject(g_hAudio,3000);CloseHandle(g_hAudio);g_hAudio=nullptr;}
        if(g_sockTx!=INVALID_SOCKET){closesocket(g_sockTx);g_sockTx=INVALID_SOCKET;}
        if(g_sockRx!=INVALID_SOCKET){closesocket(g_sockRx);g_sockRx=INVALID_SOCKET;}
        WSACleanup();return ASE_OK;
    }
    ASIOError getChannels(long*i,long*o)override{if(i)*i=kNumIn;if(o)*o=kNumOut;return ASE_OK;}
    ASIOError getLatencies(long*il,long*ol)override{if(il)*il=96;if(ol)*ol=96;return ASE_OK;}
    ASIOError getBufferSize(long*a,long*b,long*c,long*d)override{if(a)*a=kBufSize;if(b)*b=kBufSize;if(c)*c=kBufSize;if(d)*d=0;return ASE_OK;}
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

struct AES67Factory:IClassFactory{
    STDMETHOD(QueryInterface)(REFIID riid,void**ppv)override{if(!ppv)return E_POINTER;if(riid==IID_IUnknown||riid==IID_IClassFactory){*ppv=this;return S_OK;}*ppv=nullptr;return E_NOINTERFACE;}
    STDMETHOD_(ULONG,AddRef)()override{return 1;}STDMETHOD_(ULONG,Release)()override{return 1;}
    STDMETHOD(CreateInstance)(IUnknown*o,REFIID riid,void**ppv)override{if(o)return CLASS_E_NOAGGREGATION;return g_drv->QueryInterface(riid,ppv);}
    STDMETHOD(LockServer)(BOOL)override{return S_OK;}
};
extern"C" HRESULT MyDllGetClassObject(REFCLSID clsid,REFIID iid,void**ppv){if(!ppv)return E_POINTER;if(clsid==CLSID_AES67){static AES67Factory f;return f.QueryInterface(iid,ppv);}*ppv=nullptr;return CLASS_E_CLASSNOTAVAILABLE;}
extern"C" HRESULT MyDllCanUnloadNow(){return S_FALSE;}

BOOL WINAPI DllMain(HINSTANCE h,DWORD r,LPVOID){
    if(r==DLL_PROCESS_ATTACH){g_hStop=CreateEventW(nullptr,TRUE,FALSE,nullptr);DisableThreadLibraryCalls((HMODULE)h);}
    else if(r==DLL_PROCESS_DETACH){if(g_running.load()){g_running.store(false);SetEvent(g_hStop);Sleep(100);}CloseHandle(g_hStop);}
    return TRUE;
}

extern"C"{
__declspec(dllexport) ASIOError ASIOInit(ASIODriverInfo*i){if(!i)return ASE_InvalidParameter;i->asioVersion=2;i->driverVersion=1;i->errorMessage[0]=0;strcpy_s(i->name,"AES67 Virtual ASIO");i->sysRef=nullptr;return ASE_OK;}
__declspec(dllexport) ASIOError ASIOExit(){g_drv->stop();return ASE_OK;}
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
