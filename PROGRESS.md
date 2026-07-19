# 开发进度快照

> 人类可读的阶段时间线。**想看全局进度表** → `开发计划.md §0`；**想看每阶段完成态代码** → 对应 `git tag pN-done`。
> 维护约定见 `开发计划.md §4`。每完成一个阶段追加一行。

图例：✅ 已完成 · 🚧 进行中 · ⬜ 未开始

| 日期 | 阶段 | 状态 | git tag | 结果 / 备注 |
|---|---|:---:|---|---|
| 2026-07-19 | — | — | — | 文档基线：`方案设计.md` / `开发计划.md` / `CODE_REVIEW.md` 定稿；引入本进度机制 |
| 2026-07-19 | P1 清理驱动 Scream 残留 | ✅ 完成 | `p1-done` | 删 WSK 组播发包引擎 + ivshmem（净删 ~1250 行）；CSaveData 掏空为哑 sink（保留公共接口签名）；网络全局变量/注册表项全删；换皮标识（PoolTag→AS67 / .rc / .inf / DriverVer）；死缓冲、空壳 IOCTL、符号链接、METHOD 不一致均标注 `TODO(P9)`。**Windows 实测已通过**：编译干净、驱动加载不蓝屏、声音设置出现 AES67Driver 播放端点。修坑记录：IOCTL handler 误入 INIT 段致 0xFC（移到 PAGE 段修复）；`MmGetPhysicalAddress` 引 ntddk.h 冲突（P9 空壳暂填 0）。已合入 main、打 `p1-done` |
| 2026-07-19 | P2 修 M9 死锁 + pipe 生命周期 | 🚧 待编译验证 | `p2-done`(待打) | 代码已改完（分支 `feature/dev-fix`）：M9-1 STOP 不再在 pipe 线程直接 `Stop()`（会 join 自己→卡 3s），改为置原子标志由引擎主循环执行；M9-2 pipe server 从音频 `Start()` 移到 `Initialize()` 起、只在 `Shutdown()` 停（引擎活着=管道在听，面板随时可连/远程 START）；主循环生命周期改为按进程存活而非音频 Running，STOP 后进程/pipe 存活可 re-START；M9-3 SET 改地址后置 reconfig 标志、主循环安全重建 TX/RX socket 使新地址真正生效；M9-4 面板 Apply 补发 `SET sourceport`（RX Port 之前采集但从未下发）。**待 Windows 编译加载 + 面板联调验证后打 tag、合 main** |

---

## 阶段清单速览

- [x] **P1** 清理驱动 Scream 残留　✅ `p1-done`
- [ ] **P2** 修 M9 死锁 + pipe 生命周期
- [ ] **P3** 修 SAP 地址 + 锁死音频格式 ⭐
- [ ] **P4** 路由契约(JSON) + 引擎读表
- [ ] **P5** 多源汇聚（混音总线）
- [ ] **P6** Qt 面板接路由矩阵
- [ ] **P7** DSP 挂载点
- [ ] **P8** 内核收流 → 虚拟麦克风端点
- [ ] **P9** 打通 IOCTL + 共享内存（终局）
