# Architecture

## 数据流

```text
Windows APIs
  Win32 / PSAPI / PDH / IP Helper / SetupAPI / Registry / SCM
             |
             v
      SystemCollector
             |
             v
 SystemSnapshot + SnapshotHistory
       |                 |
       v                 v
ProtectionPolicy   DiagnosisEngine
       |                 |
       +--------+--------+
                v
      OptimizationPlanner
                |
                v
         SafetyValidator
                |
                v
         ActionExecutor
          /            \
         v              v
 SessionManager   ElevatedHelperClient
                         |
                 authenticated pipe
                         |
                         v
                WorkBoostElevated
                         |
                         v
                    ServiceApi

SetupAPI / Registry / Startup Folder --> read-only Serial/Startup inventory
Process snapshot + EnumWindows -------> passive startup benchmark
SnapshotHistory ----------------------> DashboardPresenter
DashboardPresenter -------------------> Win32 UI (background collector thread)
CLI / GUI fixed events ---------------> Logger -> local rotating JSONL
```

Windows API 调用集中在 `src/platform/windows`。Core 层只依赖稳定数据模型，不直接持有裸 HANDLE 或 PDH Query。
进程修改通过 `ProcessActionApi` 适配器执行；Core 的 ActionExecutor 不直接管理 HANDLE，也不枚举或发送窗口消息。

服务列表通过 `ServiceApi::QueryAll` 按需读取；Coding Mode 仅查询 `allow_service_stop` 中显式配置的服务。服务枚举没有放入每秒采样的 `SystemCollector`，避免无谓增加 Monitor Mode 开销。普通主进程不调用服务控制 API；Stop/Start 仅在独立提升的 Helper 中执行。

串口清单由 `SerialPortApi` 通过 SetupAPI 读取 Ports 设备类和 `PortName`，不调用 `CreateFile` 打开端口。启动项清单由 `StartupApi` 只读访问 HKCU/HKLM Run（含 32/64 位视图）和用户/公共 Startup Folder；跨层模型只保留逻辑位置、条目名与可执行文件 basename，不保留完整命令或路径。

## 采样

`SystemCollector::Initialize` 建立 CPU、进程和 PDH 的第一组计数器基线；后续 `Sample` 使用区间差值计算速率。进程 CPU 会除以逻辑处理器数量，最终表示该进程占整机 CPU 的百分比。进程 PID 与启动时间一起保存，以区分 PID 复用。

磁盘指标使用英文 PDH Counter 路径读取 `PhysicalDisk(*)` 的 Active Time、平均延迟、队列、吞吐和读写操作数。介质类型通过 `StorageDeviceSeekPenaltyProperty` 查询；查询失败时保留 `Unknown`，不会猜测介质类型。Collector 从 PDH 实例名提取已映射的盘符，并通过 `GetDiskFreeSpaceExW` 汇总卷总量与可用空间；映射或查询不完整时设置 `space_inventory_complete=false`，诊断层不会把不完整容量数据当作低空间证据。

Defender、后台 I/O 和前台开发进程归因只统计 `process_inventory_complete=true` 的样本；`BackgroundIoImpact` 还要求同一样本的 TCP 清单完整，避免把清单缺失误解释为“确定未保护”。进程归因或磁盘实例必须覆盖至少半个窗口且不少于两个样本；覆盖不足时只保留与缺失证据无关的结论。
诊断 JSON/TXT 与 Dashboard 同时显示总样本、Process 完整样本、TCP 完整样本和两者
同时完整的保护样本数量，使“未命中”与“证据不完整”可被区分。

GUI 使用独立工作线程持有 `SystemCollector`，按配置的采样周期更新 `SnapshotHistory`，再通过私有窗口消息把不可变的 `DashboardViewModel` 交给 UI 线程。UI 线程不执行性能采样、SCM 查询或系统控制。串口和启动项清单首次采样时加载，之后最多每 10 秒或在用户 Refresh 时刷新。

## 被动启动基准

`StartupBenchmarkApi` 在调用开始时记录已有的同名进程身份（PID + 启动时间），完成基线后才通过 ready 回调让 CLI 输出启动提示，随后只观察新出现的实例，避免用户快速启动造成竞态。它同时跟踪一次启动产生的多个同名进程，使用可见、无 owner 的顶层窗口和有超时的 `WM_NULL` 探测窗口响应。API 不启动、终止、降优先级或打开目标进程的内容。

`BenchmarkManager` 对成功样本分别计算可见窗口和可响应窗口的 Median，并输出 report schema v1。比较模式默认要求 Baseline/Optimized 各三次，由 CLI 在两个阶段之间等待用户确认；Delta 定义为 `Optimized - Baseline`，负值表示改善。任一阶段没有完整成功样本时返回不完整结果和非零退出码，不生成虚假 Delta。报告只包含进程 basename、瞬时 PID、耗时、状态和 Windows 错误码，不包含进程路径或窗口正文。

## 保护优先于优化

ProtectionPolicy 的主要不变量：

- System/Security 永远不可修改。
- RemoteTerminal/SerialTerminal/PacketCapture/BuildTool/VersionControl 默认 Strong。
- 具有已建立受保护远程端口连接的 PID 至少为 Strong；TCP 22/23 在配置加载和
  `BuildRuntimeContext` 两层都是不可删除的固定基线，自定义端口只能追加或重置
  基线以外的端口。
- dumpcap 存在时，抓包相关进程视为 Active Capture。
- 用户 `always_protect` 规则优先级最高，包括拒绝 Development 进程的升优先级特例。
- Unknown 默认 Strong，而不是优化候选。
- 内置 Development/RemoteTerminal/SerialTerminal/PacketCapture/BuildTool/
  VersionControl 进程名在 `RuleFor` 边界重新施加固有类别和 Strong 下限，分层配置不能
  通过重分类绕过保护。

OptimizationPlanner 只能创建强类型 Action。SafetyValidator 会重新查找快照中的 PID、校验启动时间、风险和目标优先级，再次调用 ProtectionPolicy。ActionExecutor 不接受任意命令文本。

服务动作还必须通过 `ServiceProtectionPolicy`：只有显式配置的 Updater、CloudSync 或 VendorUtility 且最终保护级别为 Optimizable 时才可成为候选。System/Network/Security/RemoteAccess/PacketCapture/Device/Unknown、VPN/EDR 关键词、承载受保护远程连接或活动抓包 PID 的服务均被拒绝。进程或 TCP 清单不完整时，Planner 与 SafetyValidator 会拒绝进程降优先级和 Graceful Close；服务动作还由 Helper 再次采用 fail-closed 校验。

## 会话与恢复

Coding Mode 按以下顺序执行：

1. 采集基线并生成计划；报告指标聚合完整窗口，记录样本数和观测跨度。
2. 原子写入空的 Active Session。
3. 每个动作先以 Planned 状态写入磁盘；可逆动作同时写入完整恢复载荷。
4. 通过 SafetyValidator 后执行动作。
5. 写入 Applied/Completed/Rejected/Failed 状态、结果和恢复载荷。
6. 退出时逆序回滚 Applied 动作。
7. 正常退出时在回滚前采集 Optimized 窗口证据，再执行回滚验证。
8. 写入 Baseline/Optimized/Delta、分页、磁盘队列及 SSD/HDD 活跃度/延迟、Development CPU/Working Set、后台 Top I/O、受保护工作负载、诊断证据、失败动作和回滚汇总。
9. 报告写入成功后才删除 Active Session。

若恢复或持久化失败，Active Session 保留并切换为 SafeMode，以阻止新的修改。可恢复会话 schema 当前为 v3；读取器继续兼容仅包含优先级动作的 v1，以及优先级/Graceful Close 动作的 v2 会话。`BenchmarkPoint` 对 CPU、内存、分页和每样本磁盘最大值取完整窗口平均；Development 指标只统计 Process 完整样本，后台 Top I/O 只统计 Process+TCP 同时完整的保护样本，并持久化三类清单覆盖数。单快照恢复报告仍明确记录一个样本；受保护工作负载列表另带当前清单完整性标志。完成报告在会话字段之外声明独立的 `report_schema_version: 1`，以便报告结构后续演进而不改变恢复协议。

Planned 表示“系统调用可能尚未执行，也可能已执行但结果还未落盘”。对于可逆优先级动作，恢复时会幂等回写原值。对于不可逆重放的 Graceful Close，Planned 在恢复时变为 Uncertain：系统不重发 `WM_CLOSE`，也不虚构回滚结果；用户检查实际应用状态并恢复全部可逆动作后，使用 `recovery acknowledge` 显式确认。

对于临时服务 Stop，Planned 记录包含服务名、配置身份、原始 Running 状态、超时和显式确认。恢复时通过提升 Helper 幂等 Start，并再次查询状态与身份；失败则保留 Active Session 并进入 SafeMode。

## 当前执行动作

当前实现以下白名单动作：

`SetPriorityClass`（Safe、可逆）：

- 允许的自动目标为 Below Normal、Normal、Above Normal。
- High、Realtime、Idle 被 SafetyValidator 拒绝。
- 提升仅适用于 Profile 明确列出的 Development 进程。
- 降低仅适用于 Profile 明确列出的非保护进程。
- 回滚载荷包含原始 Priority Class。

`GracefulCloseProcess`（Low、一次性）：

- 仅适用于 `allow_graceful_close` 显式列出的非保护后台应用。
- 目标必须具有可见顶层窗口，且不能是前台进程。
- 平台层校验 PID 与启动时间后，仅通过 `SendMessageTimeout(WM_CLOSE)` 请求关闭。
- 所有顶层窗口共享单个动作超时预算；部分窗口投递超时且进程仍存活时返回
  Uncertain，不能因另一个窗口成功就声称 Completed。
- 应用可以显示未保存内容提示；WorkBoost 不调用 `TerminateProcess`。
- 已持久化 Completed 的请求无需回滚；崩溃窗口中的请求进入 Uncertain。

`StopServiceTemporary`（Medium、可逆）：

- 仅适用于 `service_rules.json` 中明确分类为 Updater、CloudSync 或 VendorUtility、Protection 为 Optimizable，且同时进入 `allow_service_stop` 的服务。
- CLI 必须收到 `--confirm-service-actions`；默认配置允许列表为空。
- 执行前核对 Running、`SERVICE_ACCEPT_STOP`、配置身份、宿主 PID、保护上下文及活动依赖服务。
- 不修改 Startup Type，不级联停止依赖服务。
- 原始 Running 状态和身份在 ControlService 前持久化；退出/恢复时使用 StartService 恢复。

Terminate、永久服务配置修改和启动项修改仍由执行器拒绝。

## Elevated Helper 与 IPC

`workboost services` 返回稳定排序的 Win32 服务名称、显示名称、状态、宿主 PID、退出码、配置身份和 SCM 报告的 `SERVICE_ACCEPT_STOP` 标志。该标志只是当前服务能力的只读展示，不代表策略已允许停止。

主进程为每次高权限动作创建随机、仅本机、单实例的 Named Pipe，DACL 仅允许 SYSTEM、Administrators 和当前 Logon SID。它通过 `ShellExecuteEx(runas)` 启动同目录 `WorkBoostElevated.exe`；命令行只携带随机管道名、256-bit nonce 和父 PID，不携带服务名或命令文本。

双方核对管道 Peer PID、进程镜像路径、会话、提升令牌、请求 ID 和 nonce。二进制协议有固定版本、消息上限、显式小端编码和两个命令枚举：Temporary Stop 与 Start Restore。Helper 只从可执行文件旁及 `%LOCALAPPDATA%\WorkBoost` 加载可信配置，重新采集保护状态并独立执行 ServiceProtectionPolicy；畸形 JSON 或不完整保护清单会拒绝动作。

## 隐私

TCP 报告和 Dashboard 默认脱敏远端地址。快照中的 image path 只用于当前进程视图，不写入长期诊断或 Coding Mode 报告。启动项输出省略完整命令、参数和路径；完成报告中的受保护工作负载按进程 basename 聚合，不持久化 image path。WorkBoost 不读取网络、终端、串口或抓包内容。

`Logger` 写入 `%LOCALAPPDATA%\WorkBoost\logs\workboost.log`，达到 1 MiB 时轮转为一个备份文件。日志 API 只接受 `LogLevel`、固定 `LogEvent` 枚举和数值错误码，无法传入任意文本、PID、远端地址、命令或路径；每行是独立 JSON 对象。TRACE/DEBUG/INFO/WARN/ERROR 均有稳定表示，默认最低等级为 INFO。日志初始化或写入失败不会放宽安全策略，也不会把内部路径输出到机器可读结果。
