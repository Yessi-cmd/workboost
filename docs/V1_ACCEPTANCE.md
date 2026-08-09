# V1 Acceptance Status

更新时间：2026-08-09

本页把“代码已实现并在当前开发机验证”与“仍需外部环境验证”分开记录。只有目标 Windows 10、GitHub CI 和隔离 UAC/服务场景全部通过后，才能把 V1 标记为最终验收完成。

## 当前结论

- Phase 0–6 的设计范围已在代码中实现；Phase 7 ETW 深度诊断按设计不属于 V1。
- 当前 MinGW-w64 全警告 CMake/Ninja 重编通过，31/31 自动测试连续 10 轮通过。
- 主要只读 CLI、250 ms Diagnostic Recording、受控 GUI 的完整三次 Baseline/Optimized 比较，以及原生七页 GUI 已完成本机冒烟。
- 仍待 GitHub Actions 的 MSVC/MinGW 结果、Windows 10 x64 目标机运行、真实 SSH/Telnet/串口/抓包保持、隔离测试服务 UAC Stop/Restore，以及目标机资源预算。

## Phase 状态

| Phase | 设计范围 | 实现证据 | 状态 |
|---|---|---|---|
| 0 | CMake、WindowsError、Logger、Config、模型、CLI | `CMakeLists.txt`；固定事件 JSONL Logger 与运行时 1 MiB 轮转；配置 fail-closed 测试 | 本地完成 |
| 1 | CPU、内存、进程、磁盘、介质、Ring Buffer、status/top | `SystemCollector`；真实 `status/top` 冒烟；磁盘 IOPS/容量字段 | 本地完成 |
| 2 | TCP/PID、远程/串口/抓包保护、protected | ProtectionPolicy 单测；内置工作负载不可重分类降级；TCP 22/23 不可删除基线；真实连接/串口只读枚举 | 实现完成，真实业务连接待目标机 |
| 3 | 窗口诊断规则、diagnose 报告 | 合成规则测试；250 ms/1 秒得到 4 样本；JSON 可解析 | 本地完成 |
| 4 | Coding Mode、动作、会话、回滚、Recovery | 计划/校验/真实受控优先级/会话兼容/崩溃语义测试 | 实现完成，真实工作负载待目标机 |
| 5 | SCM、临时服务、Helper、Named Pipe、白名单 | 只读 SCM、协议、管道、关键服务拒绝测试 | 实现完成，UAC 真实服务待隔离机 |
| 6 | Baseline/Optimized、启动响应、GUI、导出 | 三次 Median/Delta 合成测试；受控 GUI 两阶段各 3/3 成功；七页 GUI 冒烟 | 本地完成，目标应用待目标机 |
| 7 | ETW 文件 I/O 深度关联 | 设计明确为后续阶段 | V1 不适用 |

## V1.0 验收标准逐条映射

| # | 验收标准 | 当前证据 | 结论 |
|---:|---|---|---|
| 1 | Windows 10 x64 正常运行 | `_WIN32_WINNT=0x0A00`、Windows API/CMake 目标已设置；当前环境并非目标 Win10 验收机 | 待目标机 |
| 2 | 持续监控 CPU、RAM、Commit、分页、SSD/HDD、进程 CPU/内存/I/O | Collector、Dashboard 与 CLI 实测；磁盘含 Active/Latency/Queue/吞吐/IOPS，卷空间有完整性标志 | 本地通过，待目标机复测 |
| 3 | 建立 SSH/Telnet TCP Session 与 PID 映射 | IPv4/IPv6 IP Helper 枚举；22/23 和自定义端口映射单测；`connections --json` 冒烟 | 本地通过，真实会话待目标机 |
| 4 | 识别并保护远程终端、串口和抓包进程 | 进程规则、TCP 强保护、known serial tools、dumpcap Active Capture 测试；COM 口只读枚举 | 本地通过，真实设备待目标机 |
| 5 | Coding Mode 默认不中断 SSH/Telnet/串口/Git/抓包/编译 | Unknown/开发类默认 Strong；不完整进程/TCP 清单下 Planner 与 SafetyValidator 双重拒绝降优先级和关闭；无 Kill/Suspend 动作 | 策略与测试通过，真实并发场景待目标机 |
| 6 | 识别 Memory、Paging、Disk、HDD Paging、Defender Impact | 窗口规则合成测试全部通过；另有 SSD Space、CPU、Background I/O | 本地通过 |
| 7 | 所有修改动作有状态记录和回滚路径 | 优先级和临时服务可逆；Graceful Close 明确一次性并记录 Completed/Uncertain；逐动作原子持久化 | 本地通过，服务恢复待隔离机 |
| 8 | 崩溃后检测未完成 Session | schema v3 Active Session、Safe Mode、restore/acknowledge 与 v1/v2 兼容测试 | 本地通过 |
| 9 | 默认不永久关闭 Defender、不改网络、不停未知服务 | 执行器强类型白名单；默认服务/关闭允许列表为空；Helper 独立复核 | 本地通过 |
| 10 | 输出优化前后量化对比 | Coding 报告含 Baseline/Optimized/Delta 和丰富指标；受控 GUI 启动比较两阶段各 3/3 成功并生成 Median Delta | 本地通过，目标 Codex 数据待测 |
| 11 | 自身资源满足目标或记录偏差 | 当前 12 秒持续监控：归一化 CPU 0.563%、Private 9.07 MiB，满足正常监控 `<1%` 和内存 `<80 MB` | 部分通过；空闲 `<0.5%` 与目标机待测 |

## 最新本机验证记录

| 检查 | 结果 |
|---|---|
| CMake/Ninja 从零配置与安装 | CMake 3.31.10 + MinGW-w64 15.2.0 通过；安装布局由真实 install 规则生成 |
| MinGW-w64 `-Wall -Wextra -Wpedantic` 主程序 | 通过，无输出警告 |
| MinGW-w64 Elevated Helper | 通过，无输出警告 |
| 自动测试 | 31/31 通过；`--repeat until-fail:10` 连续通过；覆盖运行时日志轮转、内置工作负载和 TCP 22/23 配置降级尝试 |
| 模拟安装目录 | `bin/config` 布局正确；静态运行库依赖检查通过；统一脚本通过 10 个 JSON 命令及失败路径 |
| CI 定义 | YAML 标准解析通过；MSVC/MinGW Job 均含 configure/build/test/install/安装态冒烟 |
| `status --json` | 通过；3 个磁盘实例，IOPS/容量字段可解析；Process/TCP 完整性和单快照诊断声明明确 |
| `diagnose --duration 1 --interval 250 --json` | 通过；4 个样本，报告周期 250 ms |
| `diagnose ... --interval 200` | 正确返回退出码 64 |
| `benchmark compare ... --duration 1 --json` 无新实例 | 正确返回退出码 1；schema 可解析；默认 3 runs |
| 受控 `workboost.exe` GUI 启动比较 | Baseline 3/3、Optimized 3/3；Median Delta 可解析；6 个实例均优雅关闭 |
| 本地事件日志 | 所有记录仅含 timestamp/level/event/error_code |
| 原生 GUI | 真实窗口、七页导航、后台刷新、首屏清单和可见窗口关闭路径已冒烟 |

## 发布前未完成门禁

1. 推送当前变更并确认 GitHub Actions 的 MSVC 与 MinGW 两个 Job 通过。
2. 在 Windows 10 x64 目标设备执行完整 CLI/GUI 冒烟。
3. 保持真实 SSH、Telnet、串口、Wireshark、Git 和编译工作负载验证 Coding Mode 不干扰。
4. 仅在隔离机专用服务上验证 UAC Stop/Restore、崩溃恢复和 Startup Type 不变。
5. 完成真实 Baseline/Optimized 各三次启动响应对比。
6. 在目标设备分别测量空闲 CPU、持续监控 CPU 和 Private Bytes；记录任何偏差。

详细步骤见 [`TESTING.md`](TESTING.md)。
