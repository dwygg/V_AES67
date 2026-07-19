# V_AES67 代码审查报告

> 基线：`af9de07 feat: M9 IPC pipe server + Qt panel (WIP)`
> 范围：`engine/` · `asio/` · `driver/` · `panel/`
> 定位：本报告只做**现状审查 + bug 清单**（「现在到哪了、有什么问题」）。产品目标、架构、里程碑、执行计划见 `AES67软件栈设计与开发计划.md`。
> 目标坐标系：**形态 B（整机 AES67 软件栈）**——评审据此判断「哪些是必须补的主干、哪些是可延后的旁支」。

---

## 0. 一句话结论

引擎（`engine/`）是全项目最扎实的部分，能自洽地收发 RTP；M9 刚补上「常驻引擎 + 命名管道 + Qt 面板」骨架，**方向正是形态 B 需要的地基**。但距离目标产品还差三件事：① M9 骨架有 2 个真 bug（死锁 + 生命周期）先得修好才能用；② 发出的流因 **SAP 地址 bug** 实际**订阅不到**，对外契约还没立住；③ 声道路由 / 混音总线 / DSP（形态 B 的核心价值）尚未开始。驱动仍是 Scream 换皮，其 IOCTL / 共享内存通路（阶段②）是死的，但按计划本就后置，不属当前阻断。

---

## 1. 各模块现状速览

| 模块 | 现状 | 判断 |
|---|---|---|
| `engine/` 主控 + RTP 收发 | ✅ 能自洽收发，架构清晰 | 形态 B 主干，保留深耕 |
| `engine/` PTP | 🟡 EMA 冒充卡尔曼；drift 公式有 bug；`m_driftPpb` 死代码 | 跨机才关键，暂缓 |
| `engine/` ASRC | ❌ 完全缺失（全库零命中 libsamplerate） | 跨机才需要，暂缓 |
| `engine/pipe_server` + `panel/`（M9） | 🟢 骨架成型，方向对；有 2 真 bug | **当前重点** |
| SAP / SDP 通告 | 🔴 地址 bug，接收方订阅不到 | **互通头号阻断** |
| `asio/` | ⚠️ 自包含直连；早期 P0 已修 | 降级为可选客户端，非主线 |
| `driver/` | 🟡 Scream 换皮；IOCTL/共享内存空壳 | 阶段②，后置 |
| 声道路由 / 混音 / DSP | ⚪ 未开始 | 形态 B 核心，待做（P3-P6） |

---

## 2. M9 新代码审查（本次重点）

M9 提交 = `engine/pipe_server.*`（引擎侧命名管道）+ `aes67_engine.cpp` 挂 handler + `panel/`（Qt 面板 + `pipe_client`）。**架构方向完全正确**：面板不碰音频，只通过 `\\.\pipe\AES67Engine` 收发文本命令（STATUS/START/STOP/SET…），引擎是常驻服务——这正是形态 B 的「引擎 + 对外接口」雏形。`PipeServer` 本身写得规范（`OVERLAPPED` + `m_stopEvent` 优雅退出、`PIPE_TYPE_MESSAGE`、handler 回调解耦、`m_running` 用 atomic）。下面是必须修的问题。

### 🔴 M9-1 STOP/EXIT 命令自锁 pipe 线程（真 bug）
`aes67_engine.cpp:119` handler 里 `if(cmd=="STOP"){ Stop(); }`，而 `Stop()`（`:220`）第一步就是 `m_pipeServer.Stop()`（`:223`）→ `PipeServer::Stop()` 里 `WaitForSingleObject(m_thread, 3000)`。**但这段此刻正运行在 `m_thread`（pipe 线程）里**——线程等自己结束，必然卡满 3 秒超时，期间响应写不回、面板卡死。
**改法**：handler 不在 pipe 线程里直接调 `Stop()`；改为置 `std::atomic<PendingCmd>`，由引擎主循环（`main.cpp`）择机执行；或 `Stop()` 里判断「若在 pipe 线程内则跳过 `m_pipeServer.Stop()`」。

### 🔴 M9-2 pipe 绑在音频 Start 里 → 鸡蛋悖论（真 bug）
`m_pipeServer.Start()` 在 `Start()`（`aes67_engine.cpp:158`）里。但面板要发 STATUS，pipe **必须在引擎 Start 音频之前就监听**——否则引擎没 Start 时面板永远「Disconnected」，也就无法用面板去 START 它。
**改法**：把 `m_pipeServer.Start()` 移到 `Initialize()` 末尾；`Stop()` 里删掉 `:223` 的 `m_pipeServer.Stop()`；pipe 只在 `Shutdown()` 停。即「引擎进程活着 = 管道在听」，面板随时能连、能远程 START。

### 🟠 M9-3 SET 命令改了配置但不生效
handler 里 `SET dest/source/port` 只改 `m_netConfig` 内存字段。但网络线程在 `Start()` 时已按旧值建好 socket / 绑定组播，运行中改 `m_netConfig` **不会重新绑定**——面板点 Apply 看着成功，实际没换地址。
**改法**：SET 后触发网络线程重建（或要求先 STOP 再 SET 再 START）；至少响应里说明「重启生效」。

### 🟠 M9-4 面板 Settings 字段与命令不匹配
- `mainwindow.cpp` 有 `m_sourcePort` 输入框，但 `onApplySettings()` 只发 dest/port/source 三条，**漏发 source port**；`m_sourcePort` 定义了却从未被读。
- RX source 用 `SET source`，但引擎侧 RX 端口无对应命令（只有 `SET port` 改 destPort），RX 侧配置实际改不动。

### 🟡 M9-5 pipe 无鉴权 / 长度保护
命名管道默认 ACL 允许本机任意进程连——STATUS 无所谓，但 START/STOP/EXIT/SET 这类控制命令**任何本地进程都能发**。作为本机工具可接受，将来做多用户要加 ACL。

### 🟡 M9-6 build.py 面板路径 / 生成器硬编码
`build.py build_panel()` 写死 `qt_prefix=r"D:\Qt\6.8.3\msvc2022_64"` 和 `-G "Visual Studio 18 2026"`。换机即失效，且「Visual Studio 18 2026」生成器名需确认（VS2022 是 17）。建议从环境变量 / 参数读 Qt 路径。

---

## 3. 与互通直接相关的 bug（形态 B 对外契约）

要点：**互通靠 AES67 标准合规，不靠适配第三方私有软件**（详见设计文档 §1.3）。以下 bug 卡住「流能否被订阅」这一步：

### 🔴 SAP 通告地址错（真 bug，互通头号阻断）
`aes67_engine.cpp:178` 调 `m_sapAnnouncer.Start(ssrc, destPort, config)` **只传 3 个参数**，而签名（`sap_announcer.h:20-23`）第 4 参 `mcastAddr` 默认值是 `"239.255.255.255"`。结果 SDP 里 `c=IN IP4` 通告成 **SAP 组地址** 而非真正的 RTP 目标组播地址——**接收方按此根本订阅不到音频流**。
**改法**：显式传入真实 RTP 组播目标地址。

### 🟠 音频格式 / ptime 不符 AES67
`audio_config.h` 默认虽是 48k/24bit/2ch（合规），但 `periodUs=100000`（=100ms，注释误写「10ms」），而 AES67 需 **ptime=1ms（48 样本/包，1000 pkt/s）**。发流节拍与标准不符。
**改法**：锁死 L24 / 48kHz / 2ch / ptime=1ms 这一 profile。

### 🟡 组播地址两处不一致
驱动侧 Scream 残留组播 `239.255.77.77` 与引擎 / ASIO 的 `239.69.1.128` 不一致——两个组件往不同地址发同一份音频，互相打架。驱动那套本就该在阶段②清理。

---

## 4. engine/ 其它遗留 bug（带行号）

### 🟠 JitterBuffer 半包读竞态
`jitter_buffer.h`：接收线程**先置 `valid` 标志、后 memcpy payload**，渲染线程可能在 memcpy 完成前就读到「已 valid」的槽 → 读到半包（撕裂）。仅 seq 是 atomic，payload 数组无同步。
**改法**：memcpy 完成后再用 release 写 `valid`；读侧 acquire。

### 🟠 PTP drift 公式错
`ptp_clock.cpp:65`：drift 分母 `dt_ns` 用的是 `m_lastSyncQpc - m_qpcBase`（**距启动的绝对时长**）而非「两次测量间隔」，随运行单调增长 → `offsetDelta/dt_ns` 越来越小，运行越久 drift 估计越失真。且算出的 `m_driftPpb` 目前无人消费（死代码）。跨机启用时序才关键。

### 🟡 ASIO 声道 stride 硬编码
`asio_minimal.cpp` 打包 `(s*kNumOut+ch)*3` / 解包 `s*6+ch*3` 把 stride 写死为 2ch×3B。`kNumIn==kNumOut==2` 下正确，改声道数即越界。ASIO 属可选客户端，优先级低。

---

## 5. driver/ 现状（阶段②，本轮非阻断）

按计划驱动通路（IOCTL + 共享内存）是**阶段②后置项**，此处仅记录现状，不作当前必修：

- **IOCTL 码不匹配**：`ioctl_test.cpp` 用 `0x22E0000`，驱动 `CTL_CODE(FILE_DEVICE_UNKNOWN,0x800,METHOD_NEITHER)` 实际算出 `0x222003`，`DeviceIoControl` 必然失败。
- **共享内存死缓冲**：`adapter.cpp` 里 `g_SharedBuffer` 只分配 + 清零，全文无写入音频代码；`SET_FORMAT`/`GET_POSITION` 空壳返回 SUCCESS。
- **Scream 换皮残留**：池标签 `'DVSM'`（MSVD 反写）、作者 `Marco Martinelli`/`Tom Kistner`、INF Provider 仍是 `Tom Kistner`、DriverVer 停在 2007；内核里还跑着 WSK 组播 + ivshmem（违反「薄内核」）。
- **格式范围失控**：声明 1-8ch/16-32bit/44.1-192k，需收敛到 AES67 profile。

> 处理时机：若走阶段② P8 则打通并清理；若长期用 WASAPI 回环则应**删除**这些死代码（WSK/ivshmem/`g_SharedBuffer`/`ioctl_test`/空壳 IOCTL），避免误导后来人以为「有个共享内存通路」。

---

## 6. bug 清单（分级 · 带位置）

| 级别 | 位置 | 问题 | 后果 | 阶段 |
|---|---|---|---|---|
| 🔴 | `aes67_engine.cpp:119,223` | M9-1 STOP 在 pipe 线程内 join 自身 | 面板卡 3s、响应丢失 | P1 |
| 🔴 | `aes67_engine.cpp:158` | M9-2 pipe 绑音频 Start，鸡蛋悖论 | 未 Start 时面板连不上 | P1 |
| 🔴 | `aes67_engine.cpp:178` | SAP 少传 mcastAddr，通告 SAP 组地址 | 第三方订阅不到流 | P2 |
| 🟠 | `aes67_engine.cpp` SET handler | M9-3 SET 不重建 socket | 改地址不生效 | P1 |
| 🟠 | `panel/mainwindow.cpp` | M9-4 Apply 漏发 source port | RX 配置改不动 | P1 |
| 🟠 | `audio_config.h` periodUs | ptime=100ms ≠ AES67 1ms | 发流节拍不合规 | P2 |
| 🟠 | `jitter_buffer.h` | 先置 valid 后 memcpy | 读到半包（撕裂） | P4 前 |
| 🟠 | `ptp_clock.cpp:65` | drift 分母用绝对时长 | 运行越久越失真 | 跨机(P7) |
| 🟡 | `build.py build_panel` | Qt 路径 / 生成器硬编码 | 换机构建失败 | P0/P1 |
| 🟡 | `asio_minimal.cpp` | 声道 stride 硬编码 2ch | 改声道数越界 | 非主线 |
| 🟡 | `driver/` 多处 | IOCTL 码/死缓冲/Scream 残留 | 阶段②再处理 | P8 |

> 已修复不再列：ASIO 三个 P0（缓冲越界、48/64 错配、`delete this`）、裸 `size_t` 竞态、许可证（删 ASIOSDK + 补 GPLv3 LICENSE）、getBufferSize 锁死——分别在 `73a2f92`/`7a2a657`/`c5c4b5e` 已解决。

---

## 7. 下一步（对应执行计划）

按 `AES67软件栈设计与开发计划.md` §5 的 P1 → P2 推进：

1. **P1 修 M9-1/M9-2**（+ M9-3/M9-4）：让「常驻引擎 + 面板」骨架真正可用。不依赖驱动，先做。
2. **P2 修 SAP 地址 + 锁死格式**：让流真正能被第三方订阅（MS-2 验收点）。这是形态 B 对外契约的地基，成本最低收益最高。
3. 之后 P3（路由 JSON 契约）→ P4（混音总线）→ P5（面板矩阵）→ P6（DSP），逐步落地形态 B 的核心价值。

驱动通路（阶段② P7/P8）最后再说，需 WDK + Windows 侧深度参与。
