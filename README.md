# WorkBoost

WorkBoost 是面向 Windows 10 x64 嵌入式网络开发工作站的低开销诊断工具。它通过 Win32、PSAPI、PDH 和 IP Helper API 采集真实系统数据，帮助定位 CPU、内存、分页、磁盘和后台进程竞争，同时优先保护 SSH、Telnet、串口终端、抓包、Git 和构建任务。

当前版本已实现设计文档中的 V1 主链路：CLI/原生 Dashboard 监控、开发负载保护、时间窗口诊断、Coding Mode、崩溃恢复、只读设备与启动项盘点、启动响应基准对比，以及隐私安全的本地事件日志。

## 安装

推荐从 GitHub Releases 页面下载安装包：

- `WorkBoost-<版本>-setup.exe`：一键安装，免管理员权限；自动创建开始菜单和桌面快捷方式，卸载时也会在“应用和功能”中显示。
- `WorkBoost-<版本>-msvc-x64.zip` / `WorkBoost-<版本>-mingw-x64.zip`：绿色版，解压后直接运行 `bin\workboost.exe`。

安装版默认安装到 `%LOCALAPPDATA%\Programs\WorkBoost`。需要静默安装时执行：

```powershell
WorkBoost-0.1.0-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
```

## 已实现

- 系统 CPU、物理内存、Commit、Page Reads/sec、Pages Input/sec 采样。
- 物理磁盘 Active Time、延迟、队列、吞吐、读写 IOPS 采样，以及 SSD/HDD 识别和卷容量映射。
- 进程 CPU、Working Set、Private Bytes、读写 I/O、优先级、前台窗口状态采样。
- IPv4/IPv6 TCP 连接到 PID 的映射；远端地址默认脱敏。
- 通过 SCM 枚举 Windows 服务状态、PID、配置身份和停止能力标志。
- 通过 SetupAPI 只读枚举当前存在的 COM 端口；不会打开串口或读取串口正文。
- 只读盘点 HKCU/HKLM Run 与用户/公共 Startup Folder；输出省略完整路径和命令参数。
- SSH/Telnet、自定义远程调试端口、串口工具、Wireshark/dumpcap、Git 和构建任务保护。
- Unknown 进程默认 Strong Protection，不自动优化；内置开发、远程、串口、抓包、版本
  控制和构建工具不能被用户配置重分类或降到 Strong 以下。
- 基于滑动时间窗口的内存、分页、磁盘、HDD 分页、SSD 空间、CPU、Defender 和后台 I/O 诊断；进程归因只使用完整清单，后台“未保护”判断还要求 TCP 清单完整，可缺失清单/磁盘实例必须覆盖至少半个窗口且不少于两个样本。
- 文本/JSON 诊断报告；Diagnostic Recording 支持 250–500 ms 显式采样周期。
- Coding Mode 的强类型动作计划、安全校验、逐动作持久化、逆序回滚和崩溃恢复。
- Coding Mode 显示完整进程列表及 CPU、Working Set、磁盘 I/O；用户可左键加入/移出清理池，进入模式时仅向清理池中的非保护后台窗口发送 `WM_CLOSE`。
- Profile 显式允许列表中的后台应用也可自动进入 `WM_CLOSE` 计划；两条路径都不使用 `TerminateProcess`，前台或受保护进程始终拒绝。
- 已知且显式允许的 Updater/CloudSync/VendorUtility 服务可被临时停止；动作要求 Medium 风险确认，并在退出或崩溃恢复时重新启动。
- `WorkBoostElevated.exe` 按需由 UAC 启动，通过受 ACL 限制的本地 Named Pipe 接收版本化强类型协议；Helper 不接受 Shell 或任意命令文本。
- 被动观测新启动的 Development 进程，从 Process Start 计时到可见主窗口及窗口响应；支持 Baseline/Optimized 默认各三次的 Median 与 Delta 对比，既不代替用户启动目标，也不修改目标进程。
- 原生 Win32 Dashboard，包含 Dashboard、Processes、Diagnosis、Coding Mode、Protected Workload、Recovery、Settings 七个页面；采样在线程后台完成。
- Coding Mode 报告包含 Baseline、Optimized、Delta、磁盘介质指标、分页、开发工具 CPU/Working Set、后台 Top I/O、受保护工作负载、诊断证据、失败动作和回滚汇总。
- 本地 JSONL 事件日志只接受固定事件枚举和 Windows 错误码，按 1 MiB 单备份轮转；API 不接受任意路径、PID、地址或文本载荷。

启动项修改和 ETW 深度文件 I/O 关联不属于 V1。默认服务允许列表为空，因此默认配置不会停服务、强制结束进程、修改启动项、网络配置或安全软件状态。

## 构建

要求：

- Windows 10 x64 或更高版本
- CMake 3.20+
- Visual Studio 2022（Desktop development with C++ 和 Windows SDK）或 MinGW-w64

使用 Visual Studio：

```powershell
cmake -S . -B build -A x64 -DWORKBOOST_BUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

使用 MinGW-w64：

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DWORKBOOST_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

生成的程序为 `workboost.exe` 和 `WorkBoostElevated.exe`。监控、诊断、GUI、盘点和计划生成不需要管理员权限；仅在执行临时服务 Stop/Restore 时按动作触发 UAC。仓库的 Windows CI 同时构建 MSVC 与 MinGW-w64，并运行单元测试和只读 CLI 冒烟。

安装到自定义目录：

```powershell
cmake --install build --config Release --prefix dist
```

程序、Helper 和可信配置会安装到 `dist\bin`，配置位于可执行文件旁的
`dist\bin\config`。这与主程序和 Elevated Helper 的配置查找/信任边界一致。
MSVC 与 MinGW 发布目标均静态链接编译器运行库，目标机无需另外安装
Visual C++/GCC 运行时；CI 会对安装态产物执行依赖检查和 CLI 冒烟。

## 命令

```text
workboost
workboost gui
workboost status
workboost top cpu
workboost top mem
workboost top io
workboost connections
workboost services --limit 20
workboost services --json
workboost serial
workboost serial --json
workboost startup
workboost startup --json
workboost protected
workboost diagnose --duration 60
workboost diagnose --duration 30 --interval 250
workboost diagnose --duration 60 --json --output report.json
workboost benchmark observe codex.exe --duration 60 --runs 3 --label baseline
workboost benchmark compare codex.exe --duration 60 --label coding-mode --json --output comparison.json
workboost profile show coding
workboost coding enter --dry-run
workboost coding enter
workboost coding enter --confirm-service-actions
workboost coding exit
workboost coding retry-close --close-process PID:START_TIME_100NS ...
workboost recovery status
workboost recovery restore
workboost recovery acknowledge
```

不带参数运行 `workboost` 会打开 Dashboard；`workboost gui` 与其等价。Dashboard 的 Refresh 只请求新的只读采样，Export all 会把七个页面的当前脱敏视图写入用户选择的本地文本文件。点击标题栏关闭按钮或最小化会把窗口隐藏到系统托盘；左键单击托盘图标恢复面板，右键打开包含“打开”和“退出”的菜单，只有其中的“退出”才会结束进程。若托盘图标注册失败，窗口会保持可见。

Coding Mode 页面把可选进程排在前面，并显示 CPU、Working Set 与磁盘 I/O。左键单击进程可加入清理池，再次单击可移出；点击“进入 Coding Mode”后，GUI 只把 PID 与进程启动时间作为固定数字参数传给同一可执行文件。基线保护状态按整个窗口聚合：窗口内任意一个采样点出现的 SSH/Telnet/自定义远程调试连接或 dumpcap 抓包都会让对应 PID 在整个计划中保持受保护，即使它在最后一个采样点已消失。计划使用最后一个完整快照的进程状态，但保护判定使用窗口并集；完整 Process/TCP 样本少于窗口一半或不足两个时，关闭、降优先级和临时停服务动作全部 fail-closed。执行前重新采样并核对 PID/启动时间、可见窗口、前台状态及完整保护清单，再由 `ProtectionPolicy -> OptimizationPlanner -> SafetyValidator -> ActionExecutor` 执行。进程重启、变成前台或获得受保护连接都会使动作被拒绝。

Dashboard 的 Top Impact 按可执行文件名聚合并同时列出 CPU、Private Memory 和 Disk I/O，最终等级取三项中的最高等级。CPU 的 Medium/High 边界为 5%/20%，Private Memory 为 512 MiB/2 GiB；I/O 的 Medium/High 边界分别是 `background_io_bytes_per_sec` 的 0.5 倍和 2 倍，默认即 5 MiB/s 与 20 MiB/s。

`connections` 默认把远端 IPv4 最后一段或 IPv6 后缀替换为掩码。仅在用户明确需要时使用 `--show-remote-ip`。

`status/top/connections/protected --json` 显式返回相关的
`process_inventory_complete`/`tcp_inventory_complete`。`status` 只用于快速快照，
因此还返回 `diagnosis_window_complete=false` 和 `diagnosis_sample_count=1`；时间窗口
结论以 `diagnose` 为准。文本模式会在清单不完整时显示警告，不把空结果冒充完整盘点。

`benchmark observe` 仅接受配置为 Development 的进程名，不接受路径。每次运行会先记录已有同名进程，再提示用户在超时前自行启动一个新实例，因此快速启动也不会被误记为“已有”；报告给出每次可见窗口/可响应窗口耗时及成功样本的 Median。`benchmark compare` 默认先采集三次 Baseline，等待用户确认环境变化，再采集三次 Optimized，并输出 `Optimized - Baseline` Delta（负数表示改善）。两个命令都不会启动、关闭或调整目标进程；任一阶段样本不完整时返回退出码 `1`，不伪造比较结果。

`diagnose --interval` 仅接受 250–500 ms，并在报告中写入实际采样周期、总样本数、
完整 Process/TCP 清单样本数及两者同时完整的保护样本数。不指定时沿用配置的正常
监控周期。

进入 Coding Mode 前采集 10–600 秒基线，默认 10 秒。正常退出时会在回滚前采集最多 5 秒 Optimized 证据，再逆序恢复动作并生成 report schema v1 报告。Baseline/Optimized 系统数值聚合整个采样窗口，并记录 `sample_count`、`observed_span_ms` 及 Process/TCP/保护完整样本数，不是只取最后一个快照；Development 与后台 Top I/O 只使用对应完整样本。建议先使用 `--dry-run` 检查计划。执行器只接受三类白名单动作：`SetPriorityClass`（Safe，可逆，目标仅允许 `below_normal`、`normal` 或 `above_normal`）、`GracefulCloseProcess`（Low，必须来自 `allow_graceful_close` 或用户明确选择的清理池）和 `StopServiceTemporary`（Medium、可逆，必须由 `allow_service_stop` 与 `--confirm-service-actions` 同时允许）。任何动作都必须再次通过 SafetyValidator 和对应 ProtectionPolicy。

Graceful Close 只向目标进程的可见顶层窗口发送 `WM_CLOSE`。应用可以显示“保存更改”提示并继续运行；WorkBoost 不会强制结束进程。进入 Coding Mode 时，同一批清理池进程共享一个可配置的总超时预算（`graceful_close_batch_budget_ms`，默认 5000 ms，范围 1000–30000），在同一份保护快照校验后并发投递，而不是逐个等待；每个目标的独立 `timeout_ms` 仍是该目标窗口的上限。若只有部分窗口确认收到请求且进程仍在运行，结果为 `Uncertain`，不会误报 Completed。退出 Coding Mode 后，WorkBoost 会再次核对精确 PID/启动时间，列出仍运行的清理进程并给出可直接复制的 `workboost coding retry-close --close-process PID:START_TIME_100NS ...` 命令；GUI 会弹窗询问是否立即重试。重试使用与进入模式相同的身份、可见窗口、前台、完整清单和 ProtectionPolicy 校验，并走同一条会话持久化/报告链路；PID 复用或进程重启会被拒绝。仓库默认关闭允许列表为空，因此默认配置不会自动关闭任何应用；只有用户主动加入清理池才会创建手动关闭动作。Your Phone/Phone Link、Widgets、Game Bar、Windows Settings、Calculator、Notepad、Paint、Snipping Tool 和 Photos 等非核心 Windows 附加应用具有明确的 Optimizable 分类，但仍要求可见后台窗口与用户选择。Explorer、Shell/Search Host、System/Security 和所有未知进程仍保持不可选。进程或 TCP 保护清单不完整时，降优先级和 Graceful Close 都会被 Planner 与 SafetyValidator 双重拒绝。

## 配置

仓库默认配置位于 [`config`](config)：

- `diagnosis.json`：采样周期、历史窗口、诊断阈值、受保护远程端口。TCP 22/23
  是不可删除的 SSH/Telnet 保护基线；`remote_debug_ports` 的空数组只清除其他
  自定义端口。
- `profiles.json`：Coding Mode 前台优先级、Always Protect 和显式允许列表。
- `process_rules.json`：进程分类及默认保护级别。
- `service_rules.json`：服务分类及默认保护级别。

加载优先级为：内置安全默认值 → 可执行文件旁的 `config` → 当前目录的 `config` → `%LOCALAPPDATA%\WorkBoost` 用户覆盖。也可用 `--config-dir` 指定单一配置目录。

高权限 Helper 会独立重载并复核可信配置，只接受“可执行文件旁的 `config` → `%LOCALAPPDATA%\WorkBoost`”两层。当前目录和 `--config-dir` 可以用于监控与试算，但不能单独授予服务控制权限。要显式允许一个受控测试服务，需要在可信目录同时配置：

`service_rules.json`：

```json
{
  "rules": [
    {
      "name": "ExampleUpdater",
      "class": "Updater",
      "protection": "Optimizable"
    }
  ]
}
```

`profiles.json`（仅示意相关字段）：

```json
{
  "profiles": {
    "coding": {
      "allow_service_stop": ["ExampleUpdater"]
    }
  }
}
```

配置中的系统、网络、安全、远程接入、抓包、设备、VPN/EDR 类服务无法被降级。Helper 还会重新采集进程和 TCP 保护上下文、核对服务配置身份与宿主 PID，并拒绝存在活动依赖服务的 Stop。WorkBoost 从不修改服务 Startup Type。

运行时状态保存在：

```text
%LOCALAPPDATA%\WorkBoost\
├── logs\workboost.log
├── logs\workboost.1.log
├── state\active_session.json
└── reports\<session-id>.json
```

发现未完成的 `active_session.json` 时，新的 Coding Mode 会被阻止；使用 `recovery status` 检查，再用 `recovery restore` 逆序恢复可逆动作。服务动作会在 Stop 前持久化原始 Running 状态与配置身份；Planned/Applied/Uncertain 状态在恢复时通过 Helper 幂等 Start。若 WorkBoost 在发送 `WM_CLOSE` 与持久化结果之间崩溃，该一次性动作会标记为 `Uncertain`，恢复流程不会重放或声称已回滚。确认应用状态且所有可逆动作已经恢复后，可运行 `recovery acknowledge` 完成会话。恢复或持久化失败时状态保持为 Safe Mode，不继续执行新的修改。

## 安全边界

- 不执行任意 Shell、PowerShell 或命令行字符串。
- 不自动 Kill/Suspend 进程，不停未知服务。
- 不修改 Defender、EDR、网络接口、路由、DNS、代理、MTU 或防火墙。
- 不通过清空 Working Set 或系统缓存伪造“内存优化”。
- PID 和进程启动时间一起校验，避免 PID 复用导致误操作。
- 服务配置身份和宿主 PID 在 Helper 内再次校验；保护清单采集不完整时拒绝控制。
- 每个动作执行前后立即原子持久化恢复载荷。
- 日志和报告不记录 SSH/Telnet/串口正文、抓包内容、源码路径或任意命令载荷；固定事件日志只保存在本机。

详细实现边界见 [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)，当前验收证据与目标机待办见 [`docs/V1_ACCEPTANCE.md`](docs/V1_ACCEPTANCE.md)，原始规格见 [`WorkBoost_Windows10_Embedded_Dev_Design.md`](WorkBoost_Windows10_Embedded_Dev_Design.md)。
