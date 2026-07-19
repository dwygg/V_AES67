# V_AES67 代码审查报告

> 审查对象：`github.com/dwygg/V_AES67`（Windows AES67 虚拟声卡）
> 审查范围：`AES67虚拟声卡软件开发方案.md` / `开发计划.md` + `driver/` + `engine/` + `asio/` + `build.py`
> 审查方式：纯静态审查（对照开发方案），未编译运行
> 审查基线 commit：`7d480f4 feat: full AES67 virtual soundcard project — M1-M8`
> 日期：2026-07-19

---

## 目录

1. [总体结论](#1-总体结论)
2. [方案 vs 实现：架构级偏差](#2-方案-vs-实现架构级偏差)
3. [Bug 清单（按严重程度）](#3-bug-清单按严重程度)
   - [P0 阻断级](#p0-阻断级)
   - [P1 高危](#p1-高危)
   - [P2 中等](#p2-中等)
   - [P3 清理项](#p3-清理项)
4. [分模块修改建议](#4-分模块修改建议)
5. [附：合规与许可证](#5-附合规与许可证)

---

## 1. 总体结论

**开发方案是一份完整、清晰、专业的双模（WDM + ASIO）虚拟声卡架构设计。但实际代码基本没有兑现方案的核心设计**——当前仓库更像"把三份开源骨架摆在一起"，而非方案描述的集成系统：

| 组成 | 实际形态 | 与方案的关系 |
|------|----------|--------------|
| `driver/` | **Scream 驱动的重命名副本**，额外补了一个空壳 IOCTL 层 | 方案要求的"IOCTL+共享内存交换音频"未打通 |
| `engine/` | WASAPI 回环抓音频 + 独立 socket 发/收 RTP 的用户态引擎 | 全项目最实的部分，但绕开了驱动 IOCTL |
| `asio/` | 自开 socket + 自搓 RTP/jitter/ring 的独立 ASIO DLL | 完全重复造轮子，与 engine 无任何数据交换 |

**三块彼此独立、数据通路互不相连。** 方案承诺的"驱动 ↔ 引擎 ↔ ASIO 用同一 IOCTL 通道 + 共享内存串起来"这条主动脉**根本不存在**。

**里程碑真实完成度评估**（对照 `开发计划.md`）：

| 里程碑 | 方案目标 | 实际状态 |
|--------|----------|----------|
| M1 驱动骨架 | Scream 改 AES67，设备管理器可见 | ⚠️ 换皮完成，残留严重 |
| M2 IOCTL 通道 | 用户态经 IOCTL 直通驱动 buffer | ❌ 空壳，码不匹配（见 P0-4） |
| M3 共享内存 | 驱动↔引擎共享内存交换音频 | ❌ 死缓冲，从不写入（见 P0-5） |
| M4 引擎框架 | 用户态引擎骨架 | ✅ 基本可用 |
| M5 发送通路 | WDM→共享内存→RTP | ⚠️ 走 WASAPI 回环，非共享内存 |
| M6 接收通路 | RTP→jitter→共享内存→WDM | ⚠️ 走 WASAPI render，非共享内存 |
| M7 PTPv2 时钟 | 卡尔曼 + ASRC 校准 | ❌ EMA 冒充卡尔曼，ASRC 完全缺失 |
| M8 ASIO DLL | IOCTL 直通引擎 | ❌ 完全独立，重复实现 |

---

## 2. 方案 vs 实现：架构级偏差

### 偏差 A：ASIO 完全绕开引擎和驱动，重复造轮子（违反方案第 2 条）

方案：*"ASIO 与 WDM 走同一个 IOCTL 通道——不重复实现"*、*"ASIO DLL → IOCTL → 引擎共享内存（不经过音频栈）"*。

实际：`asio/asio_minimal.cpp` **零 IOCTL / 零共享内存**（`grep DeviceIoControl` 零命中），它自己：
- 开 socket（`asio_minimal.cpp:84-88`）
- 自搓 RTP 头（`asio_minimal.cpp:56`）
- 自建 ring buffer（`asio_minimal.cpp:47-48`）
- 自建 jitter buffer（`asio_minimal.cpp:51-53`）

**结果**：`asio/` 和 `engine/` 是两套完全割裂、互不通信的 RTP 实现，且常量/逻辑不一致（见 P1-3）。仓库里躺着两个不相干的程序。

### 偏差 B：驱动共享内存通路是"死"的（违反方案 M2/M3）

方案：*"驱动回调只做 memcpy，把音频搬进共享内存给引擎读"*。

实际：`g_SharedBuffer`（`driver/adapter.cpp:582`）分配后**只被 `RtlZeroMemory` 清零**（`adapter.cpp:585`），全文再无任何往里写音频的代码。真实音频仍走 Scream 原有的两条老路：
- **WSK UDP 组播**：`driver/savedata.cpp` 整份是 `WskSocket/WskBind/WskSendTo`，硬编码组播地址 `239.255.77.77:4010`（`savedata.cpp:9-10`）
- **ivshmem**：`driver/ivshmemsavedata.cpp` 对接 QEMU/KVM 虚拟机的 ivshmem PCI 设备（`IOCTL_IVSHMEM_REQUEST_MMAP`），是 Scream 的虚拟机变体，与本机引擎毫无关系

### 偏差 C：ASRC 完全缺失，PTP 降级（违反方案 M4/M7、4.1 爆音应对）

方案把 **ASRC（libsamplerate）采样率校准**列为"解决时钟漂移爆音的核心"，把**卡尔曼滤波**列为 PTP 必需。

实际：
- **ASRC 零实现**：全代码库搜索 `libsamplerate / src_process / src_new / resample` **零命中**，连函数桩都没有。接收侧 `engine/audio_render_thread.cpp:139-152` 直接整块 memcpy 进 WASAPI buffer，无任何采样率微调。
- **PTP 算出的 `m_driftPpb` 是死代码**：从未被任何模块消费（因为没有 ASRC 去用它）。
- **卡尔曼 → EMA 降级**：`engine/ptp_clock.cpp:57-60` 用的是一阶指数平滑（EMA），`ptp_clock.cpp:66` 注释自认 *"More sophisticated Kalman would be better"*。

### 偏差 D：数据通路自相矛盾

方案称 *"ASIO 走 IOCTL 不经过 Windows 音频栈"*，但：
- `engine/` 侧 TX 用 `AUDCLNT_STREAMFLAGS_LOOPBACK`（`wasapi_device.cpp:65`）从驱动端点**回环抓音频**——绕一圈走了 Windows 音频栈
- `asio/` 侧自己发 socket——没走音频栈但也没走 IOCTL

两条通路彼此独立，都不是方案设计的 IOCTL 直通。

---

## 2 bis. 作者回应：有意设计选择

以下偏差不是未实现，而是**有意为之**。标注在此避免重复审查。

### 偏差 A（ASIO 绕开 IOCTL 自建网络）—— ✅ 有意

审查意见：ASIO DLL 应走 IOCTL+共享内存，不应重复造轮子。

**回应**：
1. 零 IPC 延迟 — 数据直接从 bufferSwitch 到 socket，不经任何中间进程
2. 部署简单 — 一个 DLL 即完整驱动，无需后台 exe
3. 实际验证通过 — 48-sample buffer @ 1ms 延迟下稳定运行
4. 方案中的"ASIO 走 IOCTL"设计假设有一个引擎 exe 常驻，当前架构更轻量

### 偏差 B（引擎用 WASAPI 回环而非共享内存）—— ✅ 有意

审查意见：engine 应从驱动共享内存读音频。

**回应**：
1. 驱动端 `g_SharedBuffer` 是死代码 — 真实音频走 PortCls DMA → Scream WSK，从未写入该缓冲
2. 打通共享内存需大改驱动 minstream/savedata 路径
3. WASAPI 回环即插即用，零驱动改动，性能达标

### 偏差 C（ASRC 缺失、PTP 用 EMA）—— ✅ 有意

审查意见：应接入 libsamplerate + 卡尔曼滤波。

**回应**：
1. ASRC — 当前同机自发自收，TX/RX 共时钟域，200ms jitter buffer 足够。ASRC 保留到跨机网络场景
2. EMA vs 卡尔曼 — 一阶 EMA 在稳态下精度足够（offset < 500ns），软件时间戳下卡尔曼的额外精度不显著

### M8 buffer size 对齐 —— ✅ 已修复

审查意见：kBufSize=64 与 AES67 48 帧不对齐。

**回应**：已改为 48，一个 bufferSwitch 周期精确对齐一个 RTP 包（1ms @ 48kHz），数据路径最简。

### 其他架构项（IOCTL/共享内存空壳）

P0-4/P0-5/P0-6/P0-7 已确认，属于后续扩展。当前优先级：先稳定 WASAPI+ASIO 自包含路径。

---

## 3. Bug 清单（按严重程度）

### P0 阻断级

#### P0-1 ✅（作者已修复）ASIO 缓冲区越界写
- **位置**：`asio/asio_minimal.cpp`（历史 commit `abf6abe`）
- **问题**：把 ASIO 每通道独立的非交织缓冲当交织缓冲寻址 `dst[(f+s)*kNumIn+ch]`，稳定堆越界写。
- **状态**：commit `73a2f92` 已修复为 `dst[s]`/`src[s]`。**保留此条作记录。**

#### P0-2 ✅（作者已修复）48/64 帧节拍错配
- **位置**：`asio/asio_minimal.cpp:34`
- **问题**：`kBufSize=64` 与 RTP 48 帧包步长不整除，每回调丢帧。
- **状态**：commit `73a2f92` 已把 `kBufSize` 改为 48，定时器周期动态计算。

#### P0-3 ✅（作者已修复）单例 `delete this` 悬垂指针
- **位置**：`asio/asio_minimal.cpp` `Release()`
- **问题**：全局单例 `g_drv` 被 `delete this` 后，导出函数持悬垂指针。
- **状态**：commit `73a2f92` 已改为 `return --m_ref;`，不再 delete。

#### P0-4 🔴 IOCTL 码不匹配 —— 测试程序调不通驱动
- **位置**：`ioctl_test.cpp:15-16` vs `driver/aes67driver.h:54-56`
- **问题**：
  - 测试程序硬编码 `IOCTL_GET_BUFFER = 0x22E0000`、`IOCTL_GET_POSITION = 0x22E0008`
  - 驱动 `CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_NEITHER, FILE_ANY_ACCESS)` 实际算出 `0x222003`（GET_POSITION 为 `0x22200B`）
  - `0x22E0000 ≠ 0x222003`，`DeviceIoControl` 必然失败或落到 PortCls 默认 handler
- **后果**：IOCTL 通道从测试层面就是断的，M2 无法验证。
- **建议**：测试程序改用与驱动一致的 `CTL_CODE` 宏计算，不要硬编码魔数：
  ```c
  #define IOCTL_AES67_GET_BUFFER   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_NEITHER, FILE_ANY_ACCESS)
  ```

#### P0-5 🔴 共享内存是死缓冲 —— 从不写入音频
- **位置**：`driver/adapter.cpp:582-587`（分配+清零）、`adapter.cpp:295`（IOCTL 只返回物理地址）
- **问题**：`g_SharedBuffer` 分配后只 `RtlZeroMemory`，真实音频却走 `minstream.cpp` 的 `m_SaveData.WriteData()`（WSK 组播）/`m_IVSHMEMSaveData.WriteData()`（ivshmem）——与 `g_SharedBuffer` 是两套互不相连的缓冲。
- **后果**：即使 IOCTL 码修对了（P0-4），用户态映射到的共享内存里也永远是 0。方案 M3 的核心机制未实现。
- **建议**：在 `minstream.cpp::CopyTo` 里把音频同时 memcpy 进 `g_SharedBuffer`（环形写），并在 IOCTL_GET_POSITION 返回真实读写指针（见 P0-6）。

#### P0-6 🔴 IOCTL SET_FORMAT / GET_POSITION 是空壳
- **位置**：`driver/adapter.cpp:307-311`
- **问题**：两个 IOCTL 直接 `return SUCCESS`，不做任何事；`GET_BUFFER` 返回的采样率/声道恒为 `48000/2`（`adapter.cpp:298`），与实际协商格式无关。
- **后果**：用户态拿不到真实读写位置，无法做无锁环形同步。
- **建议**：GET_POSITION 返回 `g_SharedBuffer` 的当前写指针（由 CopyTo 维护的 `volatile ULONG`）。

#### P0-7 🔴 直接暴露物理地址给用户态（安全 + 可靠性）
- **位置**：`driver/adapter.cpp:295` `info.PhysicalAddress = MmGetPhysicalAddress(g_SharedBuffer).QuadPart`
- **问题**：把内核物理地址直接返回用户态让其映射，既不安全，也不是方案要求的机制。
- **建议**：改用方案指定的 `MmMapLockedPagesSpecifyCache` + 把用户态虚拟地址回填，或用 `\Device\PhysicalMemory` 之外的命名 section（`ZwCreateSection` + `MapViewOfFile`）。

### P1 高危

#### P1-1 🟠 JitterBuffer payload 半包读竞态
- **位置**：`engine/jitter_buffer.h:56-58`（写）、`96-97`（读）
- **问题**：`m_slots[]/m_valid[]/m_slotSeq[]` 是**裸数组，无 atomic、无内存屏障**。Insert（网络线程）先置 `m_valid[idx]=true`（:56）再 `memcpy`（:58）；Read（渲染线程）在 :96 读到 `m_valid==true` 时，payload 可能尚未写完 → **读到半包**，音频撕裂。
- **建议**：调整写序为"先 memcpy payload，再用 `store(release)` 置 valid"；读侧用 `load(acquire)` 检查 valid。即把 `m_valid` 改为 `std::atomic<bool>` 数组并配对 acquire/release：
  ```cpp
  // 写：memcpy 在前，release 屏障在后
  memcpy(m_slots[idx], payload, kPayloadSize);
  m_slotSeq[idx] = seq;
  m_valid[idx].store(true, std::memory_order_release);
  // 读：acquire 检查
  if (m_valid[idx].load(std::memory_order_acquire) && m_slotSeq[idx] == seq) { ... }
  ```

#### P1-2 🟠 ASIO ring/jitter 跨线程裸 `size_t`，数据竞争
- **位置**：`asio/asio_minimal.cpp:47-48`（`g_rbH/g_rbT`）、`51-53`（`g_jb_r` 等）
- **问题**：TxThread 读 + AudioThread 写 `g_rbH/g_rbT`，无 atomic/屏障；`g_jb_r` 被 RxThread 写、AudioThread 读，同样无同步。x86 上"看起来能跑"但属未定义行为，压力下撕裂。
- **建议**：全部改 `std::atomic<size_t>`，读写配对 acquire/release。

#### P1-3 🟠 engine 与 ASIO 两套 RTP/jitter 割裂且常量不一致
- **位置**：`engine/jitter_buffer.h:14`（`kTargetDepth=200` 预热）vs `asio/asio_minimal.cpp:63`（即收即读、无预热）
- **问题**：同一项目里两套 jitter 逻辑：engine 有 200ms 预热窗口 + ±128 重排容忍（`jitter_buffer.h:50`），ASIO 无预热、无重排窗口。自发自收时行为不一致。
- **建议**：长期应让 ASIO 走 IOCTL 复用 engine（方案设计）；短期至少抽出公共 RTP/jitter 头文件，统一常量。

#### P1-4 🟠 PTP drift 估算公式错误
- **位置**：`engine/ptp_clock.cpp:65`
- **问题**：`dt_ns = (m_lastSyncQpc - m_qpcBase)`——用的是"距启动的绝对累计时长"，而非"两次测量间隔"。随运行时间增大，`dt_ns` 单调增长，`offsetDelta/dt_ns`（:69）趋近 0，drift 估计恒偏小且失真。
- **建议**：改为相邻两次 Sync 的间隔：
  ```cpp
  double dt_ns = (double)(m_lastSyncQpc - m_prevSyncQpc) * 1e9 / (double)m_qpcFreq;
  ```
  并新增成员 `m_prevSyncQpc` 记录上一次。（注：即使修对，因 ASRC 缺失，`m_driftPpb` 仍是死代码——需配套 P1-5。）

#### P1-5 🟠 ASRC 完全缺失，drift 无人消费
- **位置**：`engine/audio_render_thread.cpp:139-152`
- **问题**：接收侧无采样率转换，PTP 的 `m_driftPpb` 永不被使用。方案 4.1 明确"时钟漂移累积 1ms/s"必须靠 ASRC 补偿，否则长时播放必爆音。
- **建议**：接入 `libsamplerate`（`SRC_STATE* + src_process`），`src_ratio = 1.0 + m_driftPpb * 1e-9`，在 render 线程把 jitter 输出重采样后再写 WASAPI。

#### P1-6 🟠 驱动内核跑重业务逻辑（违反"零业务逻辑"）
- **位置**：`driver/savedata.cpp`（WSK 组播栈）、`driver/ivshmemsavedata.cpp`（ivshmem 协议）、`driver/minstream.cpp:20`（silence 检测）
- **问题**：方案要求内核只做 buffer 搬运，崩了不丢状态。实际驱动内跑完整 UDP 协议栈 + ivshmem + 静音检测，业务过重，任一崩溃即蓝屏丢流。
- **建议**：删除 savedata/ivshmemsavedata 的网络与落盘逻辑，内核只保留"CopyTo → 共享内存环形写"。

#### P1-7 🟠 ivshmemsavedata 析构空指针解引用
- **位置**：`driver/ivshmemsavedata.cpp:184`（析构 `m_pWorkItem->WorkItem`）vs `:95`（分配）
- **问题**：若 :95 的 work item 分配失败（`m_pWorkItem==NULL`），析构里直接解引用 → 崩溃。
- **建议**：析构先判空 `if (m_pWorkItem) { ... }`。（若采纳 P1-6 直接删除此文件，则本条一并消除。）

### P2 中等

#### P2-1 🟡 ASIO TX 打包 stride 硬耦合 2 声道
- **位置**：`asio/asio_minimal.cpp:68`
- **问题**：`pcm[s*6+ch*3]` 的 stride=6 写死"2ch×3B"。`kNumOut=2` 时最大下标 `47*6+3+2=287` 刚好不越界，一旦声道数改变立即越界+错位。
- **建议**：`long o=(s*kNumOut+ch)*3;`，stride 跟随声道数；`JB_P/288` 也应改为 `kBufSize*kNumOut*3`。

#### P2-2 🟡 getBufferSize 范围与实际固定值矛盾
- **位置**：`asio/asio_minimal.cpp:98`（min16/max2048/gran16）vs AudioThread 死写 48
- **问题**：宿主若选 48 以外的 bufSize，`createBuffers` 按宿主值分配，AudioThread 仍按 48 跑 → 大小不匹配。
- **建议**：granularity 固定为 0、min=max=preferred=48，锁死只允许 48。

#### P2-3 🟡 RTP payload type 三处硬编码 PT=97
- **位置**：`engine/network_thread.cpp:120`、`engine/network_receiver.cpp:128`、`asio/asio_minimal.cpp:60`
- **问题**：PT=97（`0x61`）三处硬编码耦合，AES67 L24 应支持动态协商。
- **建议**：抽成配置常量，SDP 通告与收发校验共用同一值。

#### P2-4 🟡 时钟域/payload 尺寸硬编码 48kHz
- **位置**：`engine/ptp_clock.cpp:113`（`/48000`）、`engine/jitter_buffer.h:13`（288=48×2×3）、`asio/asio_minimal.cpp:51`
- **问题**：非 48kHz/2ch/24bit 配置下，payload 尺寸和时钟换算全部错位甚至崩溃。
- **建议**：payload = `sampleRate/1000 * channels * bytesPerSample`，全项目统一由配置推导。

#### P2-5 🟡 RTP 未处理 CSRC/扩展头
- **位置**：`engine/network_receiver.cpp:140`（直接 `buf+12`）
- **问题**：固定跳过 12 字节 RTP 头，遇到带 CSRC 或扩展头的包会解析错位。
- **建议**：按 `CC` 字段（`buf[0]&0x0F`）计算实际头长 `12 + CC*4`，检查 X 位处理扩展头。

#### P2-6 🟡 wavtable 音频格式范围失控 + 缺 capture 端点
- **位置**：`driver/aes67driver.h:76-81`、`driver/wavtable.h:26-30`、`aes67driver.h:71`（`MAX_OUTPUT_STREAMS=0`）
- **问题**：声道 1-8、位深 16-32、采样率 44.1k-192k，远超方案的 2CH/L16-L24/48-96k；且只有 render 端点，无 Mic Array 输入（`toptable.h` 只有 WaveOut/LineOut）。
- **建议**：格式表锁定 2CH、L16/L24、48/96kHz；补 KSCATEGORY_CAPTURE 端点实现录音侧。

#### P2-7 🟡 network_thread 实时路径内日志系统调用
- **位置**：`engine/network_thread.cpp:160-164`
- **问题**：`sendto` 失败时在实时循环内 `Logger::Warn`（虽有 5s 限流），仍属实时路径系统调用。
- **建议**：改为无锁失败计数器，主线程周期性读取上报。

#### P2-8 🟡 PTP 单线程串行收包漏收 Delay_Resp
- **位置**：`engine/ptp_thread.cpp:260`
- **问题**：`RunLoop` 单线程串行 `recvfrom`，若 Delay_Resp 在预期前到达会漏收；且未校验 `requestingPortIdentity`，多从机场景会错配。
- **建议**：用带超时的 select/poll 分发不同 PTP 消息类型；校验 Delay_Resp 的 requesting port identity。

### P3 清理项

| # | 位置 | 问题 | 建议 |
|---|------|------|------|
| P3-1 | 仓库根 | **无 LICENSE 文件**，却把 Steinberg ASIO SDK 全源码+PDF（`docs/ASIOSDK/`，9.9MB）放进公开库 | 删除 SDK 源码，改文档指引自行下载；补根 LICENSE（详见第 5 节） |
| P3-2 | `asio/asio_registry.reg:14`、`asio/_test_load.cpp:9/27` | 硬编码个人路径 `E:\jmdev\AES67\...` | 改为相对/环境变量路径 |
| P3-3 | 占位 CLSID `A1B2C3D4-...` | 未用 guidgen 生成的真实 GUID | 用 `guidgen`/`uuidgen` 生成 |
| P3-4 | `engine/_list_devices.cpp`、`_listdev.cpp`、`sine_test.cpp`、`asio/_test_load.cpp`、`_test_compile.bat` | 下划线调试残留 | 移入 `tests/` 或删除 |
| P3-5 | `driver/aes67driver.h:33` | 池标签 `'DVSM'`（=反写 MSVD，Scream 残留） | 改为项目自己的 4 字符 tag |
| P3-6 | `driver/ivshmemsavedata.cpp:2`、`AES67Driver.inf:83` | 残留原作者 `Marco Martinelli`/`Tom Kistner`、Provider 仍 Tom Kistner、DriverVer 停 2007 | 清理版权头、更新 Provider/DriverVer |
| P3-7 | `build.py` `build_asio()` | 编译命令未显式链接 `ole32.lib/uuid.lib`（依赖 MSVC 默认库） | 显式加库更稳妥 |
| P3-8 | `asio/asio_minimal.cpp:89` | `start()` 用 `srand/rand` 生成 SSRC | 改用 `<random>` 或加密随机 |
| P3-9 | `asio/asio_minimal.cpp:147` | `future()` 把 `ASIOError` 强转 `void*` 返回，类型不符规范 | 按 ASIO 规范返回 `ASIOError` |

---

## 4. 分模块修改建议

### 4.1 打通主动脉（M2/M3，最高优先级）

这是让项目"从三块骨架变成一个系统"的关键。按此顺序：

1. **修 IOCTL 码不匹配**（P0-4）：测试程序与驱动统一用 `CTL_CODE` 宏。
2. **驱动写共享内存**（P0-5）：`minstream.cpp::CopyTo` 把音频环形写入 `g_SharedBuffer`，同时维护 `volatile ULONG` 写指针。
3. **GET_POSITION 返回真实指针**（P0-6）。
4. **改安全的内存映射**（P0-7）：命名 section + `MapViewOfFile`，不暴露物理地址。
5. **engine 改从共享内存读**：替换 `wasapi_device.cpp` 的 LOOPBACK 抓取，改为 `MapViewOfFile` + 读共享内存。
6. **ASIO 改走 IOCTL**：删掉 `asio_minimal.cpp` 自己的 socket/RTP/jitter，改为 `DeviceIoControl` 读写共享内存。

### 4.2 补时钟校准（M7）

- 修 drift 公式（P1-4）。
- 接入 libsamplerate 做 ASRC（P1-5），让 `m_driftPpb` 真正驱动重采样。
- （可选）EMA → 卡尔曼，若 EMA 精度够可暂缓。

### 4.3 并发安全

- JitterBuffer valid/payload 写序 + acquire/release（P1-1）。
- ASIO ring/jitter 全 atomic（P1-2）。

### 4.4 驱动瘦身与清理

- 删除 WSK 组播 + ivshmem（P1-6/P1-7），内核只保留共享内存搬运。
- 清理 Scream 残留标识（P3-5/P3-6）。

---

## 5. 附：合规与许可证

- **仓库根无任何 LICENSE 文件**。
- `docs/ASIOSDK/` 把整份 **Steinberg ASIO SDK 源码 + PDF（9.9MB）** 直接放进公开 GitHub 仓库。
- Steinberg ASIO SDK 是双授权：走专有协议**禁止再分发**，或走 GPLv3**要求整个项目也 GPLv3 开源**——当前两条路都不满足。
- 驱动骨架来自 **Scream**（GPLv3，`duncanthrax/scream`），若沿用其代码，本项目本身也受 GPLv3 约束。

**建议**：
1. 从公开库删除 `docs/ASIOSDK/` 全部 SDK 源码，README 改为"请自行从 Steinberg 官网下载 SDK 放到此目录"。
2. 根目录补 LICENSE（若沿用 Scream 代码，需 GPLv3）。
3. 清理各文件里原作者版权信息，补充本项目的版权/许可声明。

---

*本报告为纯静态审查，所有 bug 均标注文件名+行号，可直接定位。修改后需在 Windows + VS2022 + WDK 环境实际编译验证。*
