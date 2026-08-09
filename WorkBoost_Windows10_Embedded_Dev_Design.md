# WorkBoost

**Windows 10 嵌入式网络开发环境性能诊断与优化工具  
开发设计文档**

目标读者：Codex / 开发实现 Agent  
目标平台：Windows 10 x64  
建议语言：C++17  
文档用途：项目初始化、模块拆分、实现约束、验收基准

# 1. 项目背景与目标

目标设备为企业办公 Windows 10 PC，典型硬件为第 10 代 Intel Core i5、16 GB 内存、小容量 SSD + 1 TB HDD。主要工作负载是网络通信相关嵌入式开发，日常同时运行代码编辑器、浏览器、Codex、SSH/Telnet 客户端、串口终端、抓包工具、Git、编译脚本及企业办公软件。

WorkBoost 的目标是构建一个低开销、可诊断、可回滚的 Windows 性能优化工具。工具首先识别 CPU、内存、分页、磁盘 I/O、后台进程和服务中的实际瓶颈，再生成并执行受控优化计划。

## 1.1 核心目标

- 实时采集系统及进程级 CPU、内存、Commit、分页、磁盘 I/O 等指标。

- 识别 Codex、IDE、终端、串口工具等交互式应用发生卡顿时的主要系统瓶颈。

- 提供 Coding Mode，一次性执行一组安全、可恢复的优化动作。

- 保护嵌入式开发所依赖的 SSH、Telnet、串口、抓包、编译、Git 和终端进程，不因通用优化策略导致连接中断或任务丢失。

- 记录优化前后指标，输出可量化的 A/B 对比结果。

- 所有高风险系统修改均需要显式策略允许，并保存原始状态用于恢复。

## 1.2 非目标

- 不实现注册表清理、垃圾文件清理、驱动更新等综合管家功能。

- 不以强制清空 Working Set、Standby List 等方式作为常规“内存优化”。

- 不绕过企业组策略、Tamper Protection 或其他组织安全控制。

- 不默认永久禁用 Windows Defender、Windows Update、网络栈或远程连接相关系统组件。

- 不自动关闭未知进程或未知服务。未知对象默认进入只监控状态。

# 2. 目标工作负载与保护边界

## 2.1 典型开发工作负载

| **类别**    | **典型工具/进程**                                     | **WorkBoost 默认策略**                |
|-------------|-------------------------------------------------------|---------------------------------------|
| 代码/AI     | Codex、VS Code、其他编辑器                            | 保护；可提升优先级；纳入前台延迟监控  |
| 远程终端    | OpenSSH、PuTTY、Xshell、MobaXterm、Windows Terminal   | 强保护；禁止自动结束                  |
| Telnet      | telnet.exe、PuTTY/Xshell Telnet Session               | 强保护；禁止自动结束                  |
| 串口        | Xshell Serial、PuTTY Serial、SecureCRT、其他 COM 终端 | 强保护；禁止自动结束                  |
| 抓包        | Wireshark、dumpcap、Npcap 相关进程                    | 保护；抓包期间禁止暂停/结束           |
| 版本控制    | git.exe、ssh.exe、git-remote-\*                       | 活动期间保护                          |
| 编译/脚本   | make、ninja、cmake、python、bash、wsl 等              | 活动期间保护；高 CPU 不自动判定为异常 |
| 办公        | 企业微信、Teams、浏览器、Office                       | 根据 Profile 和用户配置决定           |
| 更新器/托盘 | 第三方 updater、厂商辅助程序                          | 可列入低风险优化候选                  |

## 2.2 连接保护原则

- 存在活跃 TCP 会话的 SSH/Telnet 客户端，不执行 Kill、Suspend 或优先级降至 Idle。

- 持有 COM 端口句柄的串口程序，不执行自动关闭或 Suspend。

- 正在抓包的 dumpcap/Wireshark 进程，不执行自动结束。

- 存在前台窗口且最近 60 秒内有用户输入的终端程序，默认视为 Active Interactive Process。

- git、ssh、scp、sftp、rsync、编译器和脚本解释器仅在确认处于空闲状态后才允许被候选策略处理；V1.0 默认只保护，不自动处理。

- 任何未识别的开发工具默认归类为 UnknownProtectedCandidate，而不是 BackgroundDisposable。

# 3. 产品工作模式

## 3.1 Monitor Mode

默认模式。只采集指标、生成诊断，不修改系统状态。

## 3.2 Coding Mode

面向 Codex + IDE + 远程终端 + 串口调试场景。进入模式时生成 Optimization Plan，并在用户配置允许范围内执行。退出 Coding Mode 时恢复所有可逆动作。

## 3.3 Diagnostic Recording

用于复现卡顿。启动后以较高采样率记录最近一段时间的系统、进程和磁盘状态，生成诊断报告。

## 3.4 Safe Mode

WorkBoost 检测到状态恢复失败、权限异常或企业策略限制时进入 Safe Mode。此模式停止执行任何系统修改，仅提供监控与恢复入口。

# 4. 总体架构

WorkBoost.exe

│

├─ UI / CLI

│

├─ Application Layer

│ ├─ ProfileManager

│ ├─ DiagnosisEngine

│ ├─ OptimizationPlanner

│ ├─ BenchmarkManager

│ └─ SessionManager

│

├─ Core

│ ├─ CpuMonitor

│ ├─ MemoryMonitor

│ ├─ DiskMonitor

│ ├─ ProcessMonitor

│ ├─ NetworkSessionMonitor

│ ├─ SerialPortMonitor

│ ├─ ServiceManager

│ ├─ StartupManager

│ └─ ProtectionPolicy

│

├─ Windows Platform Adapter

│ ├─ Win32 / PSAPI

│ ├─ PDH

│ ├─ IP Helper API

│ ├─ Service Control Manager

│ ├─ Registry

│ ├─ SetupAPI / Device APIs

│ └─ ETW (V2)

│

└─ WorkBoostElevated.exe

└─ 仅执行经过白名单校验的高权限动作

## 4.1 进程模型

- WorkBoost.exe：普通用户权限运行，负责 UI、采集、诊断、策略和状态管理。

- WorkBoostElevated.exe：按需通过 UAC 启动，只执行需要管理员权限的预定义动作。

- 两个进程通过 Named Pipe 通信。Elevated Helper 不接受任意 Shell 命令。

## 4.2 线程模型

| **线程/任务**          | **职责**                       | **建议周期** |
|------------------------|--------------------------------|--------------|
| UI Thread              | 渲染、用户操作、状态展示       | 事件驱动     |
| System Monitor Worker  | CPU/内存/磁盘全局指标          | 1000 ms      |
| Process Monitor Worker | 进程 CPU/内存/I/O/窗口状态     | 1000-2000 ms |
| Connection Worker      | TCP 连接、SSH/Telnet 活跃度    | 2000 ms      |
| Serial Worker          | COM 设备及候选串口进程保护状态 | 3000-5000 ms |
| Diagnosis Worker       | 滑动窗口规则评估               | 1000 ms      |
| Action Worker          | 优化动作执行、验证、回滚       | 按需         |

# 5. 数据采集设计

## 5.1 CPU

- 系统总 CPU：GetSystemTimes 或 PDH。

- 进程 CPU：GetProcessTimes 计算采样区间差值。

- 记录逻辑 CPU 数，正确归一化进程 CPU 百分比。

- 后续可扩展单核热点识别。

## 5.2 内存

| **指标**                    | **API/来源**               | **用途**                   |
|-----------------------------|----------------------------|----------------------------|
| Physical Total / Available  | GlobalMemoryStatusEx       | 判断物理内存压力           |
| Commit Total / Limit / Peak | GetPerformanceInfo         | 判断虚拟内存压力           |
| Working Set                 | GetProcessMemoryInfo       | 观察当前驻留 RAM           |
| Private Usage               | PROCESS_MEMORY_COUNTERS_EX | 进程实际私有提交           |
| Page Reads/sec              | PDH                        | 识别需要磁盘参与的分页压力 |
| Pages Input/sec             | PDH                        | 辅助识别持续换页           |

内存诊断以 Available RAM、Commit Ratio 和分页 I/O 的组合为依据。禁止仅根据“已用内存比例”执行清理动作。

## 5.3 磁盘

- 采集每个物理磁盘 Active Time、Read/Write Bytes/s、IOPS、Average Latency、Queue Length。

- 建立逻辑盘符与 PhysicalDrive 的映射。

- 识别 SSD/HDD；针对 HDD 高延迟随机 I/O 提高诊断权重。

- SSD 空间不足需要作为独立警告项记录，但 V1.0 不自动执行文件迁移。

## 5.4 进程

每个 ProcessSnapshot 至少包含：

pid

parent_pid

name

image_path

publisher(optional)

cpu_percent

working_set_bytes

private_bytes

read_bytes_per_sec

write_bytes_per_sec

priority_class

session_id

has_visible_window

is_foreground

last_user_interaction_age(optional)

start_time

classification

protection_level

# 6. 网络与串口开发保护

## 6.1 NetworkSessionMonitor

使用 Windows IP Helper API 枚举 TCP 连接，建立 PID 与连接的映射。V1.0 重点识别本机发起的 TCP 22、23 及用户配置的远程调试端口。

struct TcpSession {

DWORD pid;

IpAddress local_address;

uint16_t local_port;

IpAddress remote_address;

uint16_t remote_port;

TcpState state;

};

### 保护判定

- 目标进程存在 ESTABLISHED 状态的远程 TCP 连接。

- 远程端口为 22（SSH）或 23（Telnet）。

- 进程命中用户定义 remote_terminal_processes 列表。

- 进程为 ssh.exe/scp.exe/sftp.exe 且当前仍存在活动子任务。

- 满足任一条件时 protection_level 至少提升到 Strong。

## 6.2 SerialPortMonitor

串口保护不依赖具体软件名称作为唯一依据。优先通过设备枚举获取当前 COM 端口列表；进程是否持有串口句柄的精确检测可在后续 ETW/Handle 分析版本实现。V1.0 采用“已知串口工具 + 前台状态 + 用户锁定”的组合策略。

- 维护 known_serial_tools 数据库。

- 允许用户将任意进程标记为 Always Protect。

- 若串口工具存在可见窗口且最近处于前台，禁止自动结束。

- 任何包含未保存会话风险的终端工具，只允许 graceful close，且默认不自动执行。

## 6.3 Wireshark / 抓包保护

- Wireshark.exe、dumpcap.exe、tshark.exe 默认 Protection=Strong。

- 检测到 dumpcap 存在时视为 Active Capture。

- Active Capture 状态禁止结束相关进程、停止 Npcap 相关服务或修改网络接口。

- WorkBoost 不修改路由、DNS、网卡、电源管理、TCP 参数等网络配置。

# 7. 进程分类与保护策略

## 7.1 ProtectionLevel

enum class ProtectionLevel {

SystemCritical, // 永不操作

Strong, // SSH/Telnet/Serial/Capture/IDE active workload

Normal, // 普通用户应用

Optimizable, // 已知可优化后台应用

UserExplicit // 用户显式指定策略

};

## 7.2 ProcessClass

enum class ProcessClass {

System,

Security,

Development,

RemoteTerminal,

SerialTerminal,

PacketCapture,

BuildTool,

VersionControl,

Browser,

Communication,

Office,

Updater,

CloudSync,

VendorUtility,

Unknown

};

## 7.3 默认动作矩阵

| **分类**       | **Close** | **Suspend** | **Priority Down** | **Priority Up** | **备注**              |
|----------------|-----------|-------------|-------------------|-----------------|-----------------------|
| System         | 禁止      | 禁止        | 禁止              | 禁止            | 系统关键进程          |
| Security       | 禁止      | 禁止        | 禁止              | 禁止            | 仅诊断；受策略管理    |
| Development    | 禁止      | 禁止        | 否                | 允许            | Codex/IDE             |
| RemoteTerminal | 禁止      | 禁止        | 否                | 可选            | 保护连接              |
| SerialTerminal | 禁止      | 禁止        | 否                | 可选            | 保护调试会话          |
| PacketCapture  | 禁止      | 禁止        | 否                | 可选            | 保护抓包              |
| BuildTool      | 禁止      | 禁止        | 否                | 否              | 高 CPU 可能是预期行为 |
| VersionControl | 禁止      | 禁止        | 否                | 否              | 避免中断 Git/SSH      |
| Communication  | 用户配置  | 默认禁止    | 允许              | 否              | 企业微信等            |
| Updater        | 允许      | 可选        | 允许              | 否              | 候选优化对象          |
| CloudSync      | 用户配置  | 可选        | 允许              | 否              | 大 I/O 时提示         |
| Unknown        | 禁止      | 禁止        | 否                | 否              | 只监控                |

# 8. 服务与启动项策略

## 8.1 ServiceManager

服务动作统一使用 Service Control Manager。默认仅支持 Query、Temporary Stop、Start、Restore。Disable Startup Type 属于高风险动作，必须由显式配置启用，且不在默认 Coding Mode 中使用。

## 8.2 服务黑名单

以下类型不得由通用优化策略停止：

- 网络栈、DHCP、DNS Client、Network Location、RPC、DCOM 等基础系统服务。

- 企业 VPN、Endpoint Security、EDR、设备管理、域环境相关服务。

- OpenSSH Server/Client 相关依赖（如用户实际使用）。

- Npcap 等当前抓包依赖服务。

- 串口驱动、USB 设备管理相关核心服务。

- Windows Installer 等短时系统任务仅监控，不作为常规优化目标。

## 8.3 启动项

扫描 HKCU/HKLM Run、Startup Folder。V1.0 仅展示和生成建议；禁用启动项必须由用户显式选择并记录原状态。

# 9. Windows Defender 处理策略

Defender 在 WorkBoost 中作为 Security 类对象处理。默认功能为影响监控和性能诊断，不直接实现绕过组织策略的永久禁用。

## 9.1 监控

- 识别 MsMpEng.exe CPU、Working Set、Read/Write I/O。

- 计算 MsMpEng 对总 CPU 与总磁盘 I/O 的占比。

- 将 Defender 活动与 Codex/编译/源码目录访问期间的磁盘延迟做时间关联。

## 9.2 优化动作边界

- 如果系统由组织策略管理，只输出诊断结果，不修改 Defender 状态。

- 如果后续实现 Defender Performance Analyzer 集成，仅调用系统支持的诊断接口并解析报告。

- 不将“永久关闭 Defender”纳入默认功能和验收目标。

- 所有 Security 类进程不进入自动 Kill/Suspend 计划。

# 10. Diagnosis Engine

## 10.1 数据窗口

维护最近 60 秒 Ring Buffer。Monitor Mode 默认 1 秒采样；Diagnostic Recording 可提升到 250-500 ms。每条规则基于时间窗口而非单点值。

## 10.2 初始规则

| **规则**                    | **示例条件**                             | **输出**               |
|-----------------------------|------------------------------------------|------------------------|
| MemoryPressure              | Available RAM 持续低 + Commit Ratio 高   | 内存压力               |
| PagingPressure              | MemoryPressure + Page Reads/sec 持续升高 | 分页导致交互延迟       |
| DiskBottleneck              | Active Time 高 + Latency 高              | 磁盘瓶颈               |
| HddPagingBottleneck         | 分页压力 + HDD 高延迟                    | HDD/分页组合瓶颈       |
| CpuSaturation               | CPU 长时间接近满载                       | CPU 资源竞争           |
| DefenderImpact              | MsMpEng 占磁盘或 CPU 比例明显升高        | 安全扫描影响           |
| BackgroundIoImpact          | 非保护后台进程大量读写                   | 后台 I/O 竞争          |
| ForegroundAppMemoryPressure | Codex/IDE 响应期间系统可用内存过低       | 前台应用受内存压力影响 |

## 10.3 Confidence

enum class Confidence {

Low,

Medium,

High

};

每条诊断必须附带 Evidence。阈值存放在配置文件中，不硬编码为不可修改常量。

## 10.4 Evidence 示例

{

"type": "HddPagingBottleneck",

"severity": "high",

"confidence": "high",

"evidence": {

"available_memory_mb": 1240,

"commit_ratio": 0.86,

"page_reads_per_sec": 118,

"disk": "PhysicalDrive1",

"media": "HDD",

"active_time": 0.98,

"avg_latency_ms": 74

}

}

# 11. Optimization Planner

## 11.1 生成流程

SystemSnapshot history

\+

DiagnosisResult\[\]

\+

ProtectionPolicy

\+

UserProfile

↓

OptimizationPlanner

↓

OptimizationPlan

↓

SafetyValidator

↓

ActionExecutor

## 11.2 ActionType

enum class ActionType {

GracefulCloseProcess,

TerminateProcess, // 默认关闭，需显式允许

SetPriorityClass,

StopServiceTemporary,

StartService,

DisableStartupEntry,

RestoreStartupEntry

};

## 11.3 风险等级

| **Risk**  | **要求**                    | **示例**                           |
|-----------|-----------------------------|------------------------------------|
| Safe      | 可作为默认 Coding Mode 动作 | 降低已知 updater 优先级            |
| Low       | 用户 Profile 明确允许       | 关闭无未保存状态的后台应用         |
| Medium    | 需要提示并记录状态          | 临时停止已知非关键服务             |
| High      | 不自动执行                  | 修改 Startup Type、强制 Terminate  |
| Forbidden | 实现层直接拒绝              | 操作系统关键进程、绕过组织安全策略 |

# 12. Coding Mode 默认策略

## 12.1 进入前

1\. 采集 Baseline Snapshot，至少持续 10 秒。

2\. 保存当前进程优先级、待修改服务状态、启动项状态。

3\. 检查 SSH/Telnet/TCP Session、串口工具、Wireshark/dumpcap、Git/编译进程。

4\. 构建 Protected PID Set。

5\. 生成 Optimization Plan 并通过 SafetyValidator。

## 12.2 默认允许

- 将 Codex/主要 IDE 设置为 ABOVE_NORMAL_PRIORITY_CLASS（可配置）。

- 对已知且明确列入 Profile 的 updater/辅助程序降低优先级。

- 对用户显式允许关闭的后台应用发起 Graceful Close。

- 输出 Windows Search、云同步、Defender 等后台 I/O 影响建议。

- 记录磁盘和内存改善情况。

## 12.3 默认禁止

- 停止 SSH/Telnet/串口/抓包相关进程。

- 暂停 git、ssh、scp、编译器、Python 构建脚本。

- 停止未知 Windows 服务。

- 修改网络接口、IP、DNS、路由、Proxy、MTU、防火墙规则。

- 永久修改 Security/EDR 组件状态。

- 清空系统缓存或 Working Set 作为常规动作。

## 12.4 退出

1\. 停止生成新的优化动作。

2\. 按逆序回滚本 Session 中所有 reversible actions。

3\. 验证服务、优先级、启动项状态是否恢复。

4\. 生成 Before/After 报告。

5\. 删除 active_session 标记并持久化 Session Summary。

# 13. 事务、状态与崩溃恢复

## 13.1 OptimizationSession

struct OptimizationSession {

std::string session_id;

SessionState state;

Timestamp start_time;

SystemSnapshot baseline;

std::vector\<ExecutedAction\> actions;

};

## 13.2 ExecutedAction

struct ExecutedAction {

OptimizationAction action;

ActionState state;

RestorePayload restore_payload;

std::optional\<WindowsError\> error;

};

## 13.3 持久化

%LOCALAPPDATA%\WorkBoost\\

├─ config.json

├─ profiles.json

├─ process_rules.json

├─ state\\

│ └─ active_session.json

├─ logs\\

└─ reports\\

## 13.4 崩溃恢复

- 启动时发现 active_session.json 且状态未 Completed，则进入 Recovery Flow。

- 优先执行可确定的回滚动作；无法确定状态的动作只提示，不盲目再次修改。

- 回滚失败时保持 Safe Mode，并输出具体 Windows Error Code。

- 每个动作执行后立即持久化状态，避免内存状态与实际系统状态不一致。

# 14. 配置模型

## 14.1 profiles.json 示例

{

"profiles": {

"coding": {

"foreground_priority": {

"Codex.exe": "above_normal",

"Code.exe": "above_normal"

},

"always_protect": \[

"ssh.exe",

"telnet.exe",

"putty.exe",

"xshell.exe",

"SecureCRT.exe",

"Wireshark.exe",

"dumpcap.exe",

"tshark.exe",

"git.exe"

\],

"allow_graceful_close": \[

"ExampleUpdater.exe"

\],

"allow_priority_down": \[

"ExampleUpdater.exe"

\]

}

}

}

## 14.2 diagnosis.json 示例

{

"sample_interval_ms": 1000,

"history_seconds": 60,

"thresholds": {

"commit_warning": 0.80,

"available_memory_mb": 2048,

"disk_active_ratio": 0.90,

"hdd_latency_ms": 30,

"cpu_saturation_ratio": 0.90

}

}

所有阈值均作为初始启发式参数，可在测试后调整。

# 15. CLI 与 GUI

## 15.1 CLI 优先实现

workboost status

workboost top cpu

workboost top mem

workboost top io

workboost connections

workboost protected

workboost diagnose --duration 60

workboost profile show coding

workboost coding enter

workboost coding exit

workboost recovery status

## 15.2 status 输出要求

SYSTEM

CPU 34%

RAM 13.9 / 15.8 GB

AVAILABLE 1.9 GB

COMMIT 84%

DISK

C: SSD Active 18% Latency 1.6 ms

D: HDD Active 97% Latency 68 ms

PROTECTED WORKLOAD

ssh.exe PID 4312 ESTABLISHED -\> 172.16.x.x:22

Xshell.exe PID 8820 protected: remote-terminal

Wireshark.exe PID 6004 capture-active

TOP IO

MsMpEng.exe 24 MB/s

SearchIndexer.exe 11 MB/s

DIAGNOSIS

HIGH HddPagingBottleneck

MEDIUM DefenderImpact

## 15.3 GUI

GUI 在核心采集和策略稳定后实现。至少包含 Dashboard、Processes、Diagnosis、Coding Mode、Protected Workload、Recovery、Settings 七个页面。

# 16. Windows API 映射

| **功能**      | **首选接口**                                                        |
|---------------|---------------------------------------------------------------------|
| 系统内存      | GlobalMemoryStatusEx / GetPerformanceInfo                           |
| 进程枚举      | CreateToolhelp32Snapshot 或 EnumProcesses                           |
| 进程内存      | GetProcessMemoryInfo                                                |
| 进程时间      | GetProcessTimes                                                     |
| 进程 I/O      | GetProcessIoCounters                                                |
| 性能计数器    | PDH                                                                 |
| TCP 连接/PID  | GetExtendedTcpTable                                                 |
| 服务          | OpenSCManager / OpenService / QueryServiceStatusEx / ControlService |
| 启动项        | Registry API + Startup Folder                                       |
| 物理磁盘属性  | DeviceIoControl / Storage Query Property                            |
| 窗口/前台状态 | EnumWindows / GetForegroundWindow / GetWindowThreadProcessId        |
| 权限提升      | ShellExecute runas + Elevated Helper                                |
| 高精度事件    | ETW，V2 实现                                                        |

# 17. 源码目录

WorkBoost/

├─ CMakeLists.txt

├─ README.md

├─ docs/

├─ config/

├─ src/

│ ├─ app/

│ │ ├─ session_manager.\*

│ │ ├─ profile_manager.\*

│ │ └─ benchmark_manager.\*

│ ├─ core/

│ │ ├─ model/

│ │ ├─ monitor/

│ │ ├─ diagnosis/

│ │ ├─ policy/

│ │ ├─ optimization/

│ │ └─ recovery/

│ ├─ platform/

│ │ └─ windows/

│ │ ├─ pdh_adapter.\*

│ │ ├─ process_api.\*

│ │ ├─ service_api.\*

│ │ ├─ network_api.\*

│ │ ├─ storage_api.\*

│ │ └─ registry_api.\*

│ ├─ cli/

│ ├─ ui/

│ └─ utils/

├─ elevated/

└─ tests/

├─ unit/

├─ integration/

└─ fixtures/

# 18. 核心接口

## 18.1 Monitor

class IMonitor {

public:

virtual ~IMonitor() = default;

virtual bool Initialize() = 0;

virtual void Sample() = 0;

virtual void Shutdown() = 0;

};

## 18.2 ProtectionPolicy

class ProtectionPolicy {

public:

ProtectionLevel Evaluate(

const ProcessSnapshot& process,

const RuntimeContext& context,

const UserProfile& profile) const;

bool CanExecute(

const OptimizationAction& action,

const RuntimeContext& context) const;

};

## 18.3 DiagnosisRule

class IDiagnosisRule {

public:

virtual ~IDiagnosisRule() = default;

virtual std::optional\<DiagnosisResult\> Evaluate(

const SnapshotHistory& history,

const RuntimeContext& context) const = 0;

};

## 18.4 ActionExecutor

class ActionExecutor {

public:

ActionResult Execute(const OptimizationAction& action);

ActionResult Rollback(const ExecutedAction& action);

};

# 19. Benchmark 与效果评估

## 19.1 系统指标

- Available RAM、Commit Ratio。

- HDD/SSD Active Time、Average Latency、Queue Length。

- Page Reads/sec。

- Codex/IDE CPU 与 Working Set。

- 后台 Top I/O Process。

## 19.2 交互指标

- Codex cold start：Process Start -\> 主窗口可响应。

- Codex warm start。

- 打开会话的人工标记耗时或可检测窗口状态变化。

- Coding Mode 前后重复 3 次，使用 Median 进行比较。

## 19.3 报告

报告至少包含 Baseline、Optimized、Delta、执行动作、受保护工作负载、诊断证据、失败动作和回滚状态。

# 20. 日志与隐私

- 默认所有数据保存在本机，不上传。

- 日志不记录 SSH 密码、Telnet 内容、串口正文、抓包数据包内容。

- TCP Session 诊断报告默认允许对远端 IP 做脱敏配置。

- 源码文件名和项目路径不作为默认长期日志字段；需要 I/O 深度诊断时由用户显式启用。

- 日志等级：TRACE / DEBUG / INFO / WARN / ERROR。

# 21. 非功能要求

| **项目**       | **目标**                                    |
|----------------|---------------------------------------------|
| 空闲 CPU       | \< 0.5% 目标值                              |
| 正常监控 CPU   | \< 1% 目标值                                |
| 常驻内存       | \< 80 MB 目标值                             |
| 默认采样周期   | 1000 ms                                     |
| 无管理员权限   | 完整支持监控与诊断                          |
| 异常恢复       | 存在未完成 Session 时必须提供 Recovery      |
| 未知对象策略   | 默认不修改                                  |
| 网络开发安全性 | 不得因默认优化动作中断 SSH/Telnet/串口/抓包 |

# 22. 开发阶段

## Phase 0 - 项目骨架

- CMake + C++17 工程。

- WindowsError、Logger、Config、基础数据模型。

- CLI 框架。

## Phase 1 - Monitor MVP

1\. CpuMonitor。

2\. MemoryMonitor。

3\. ProcessMonitor：CPU/Working Set/Private/I/O。

4\. DiskMonitor：Active/Latency/Throughput。

5\. SSD/HDD 识别。

6\. Ring Buffer。

7\. workboost status / top 命令。

## Phase 2 - Embedded Workload Protection

1\. NetworkSessionMonitor，PID \<-\> TCP Session 映射。

2\. SSH/Telnet 连接保护。

3\. known remote/serial/capture process database。

4\. Wireshark/dumpcap Active Capture 保护。

5\. ProtectionPolicy 和 protected 命令。

## Phase 3 - Diagnosis

1\. MemoryPressureRule。

2\. PagingPressureRule。

3\. DiskBottleneckRule。

4\. HddPagingBottleneckRule。

5\. DefenderImpactRule。

6\. BackgroundIoImpactRule。

7\. diagnose 命令及 JSON/TXT 报告。

## Phase 4 - Coding Mode

1\. ProfileManager。

2\. OptimizationPlanner。

3\. SafetyValidator。

4\. Priority Action。

5\. Graceful Close Action。

6\. OptimizationSession。

7\. Rollback / Crash Recovery。

8\. workboost coding enter/exit。

## Phase 5 - Service / Elevated Helper

1\. SCM 读状态。

2\. 临时 Stop/Restore。

3\. Elevated Helper。

4\. Named Pipe IPC。

5\. Action 白名单验证。

## Phase 6 - Benchmark / GUI

1\. Baseline / Optimized 对比。

2\. Codex 启动时间检测。

3\. Dashboard。

4\. Diagnosis / Protected Workload / Recovery 页面。

5\. 结果导出。

## Phase 7 - ETW 深度诊断

- 进程级 File I/O。

- 更精确的 Hard Fault / Paging 关联。

- 将 Codex 卡顿时间点与具体文件 I/O、HDD 延迟、后台扫描活动进行关联。

# 23. 测试计划

## 23.1 单元测试

- CPU 百分比归一化。

- Commit Ratio 计算。

- ProcessClass/ProtectionLevel 匹配。

- TCP 22/23 Session -\> Strong Protection。

- 未知进程 -\> 默认保护。

- Risk/Action 白名单。

- OptimizationPlan 过滤。

- Rollback payload 序列化。

## 23.2 集成测试场景

| **场景**                        | **预期**                           |
|---------------------------------|------------------------------------|
| 保持 SSH 登录后进入 Coding Mode | SSH 进程和会话不被关闭/暂停        |
| Telnet 会话连接设备             | 客户端保持运行                     |
| 串口终端打开 COM 口             | 已知串口工具保持运行               |
| Wireshark 正在抓包              | Wireshark/dumpcap 不被处理         |
| 后台 updater 高 I/O             | 可被识别为优化候选                 |
| Codex + 浏览器导致内存压力      | 输出 Memory/Paging 相关诊断        |
| 分页 + HDD 100% 高延迟          | 输出 HddPagingBottleneck           |
| MsMpEng 大量 I/O                | 输出 DefenderImpact，但不自动 Kill |
| WorkBoost 执行动作后崩溃        | 下次启动进入 Recovery              |
| 无管理员权限运行                | 监控正常，高权限动作返回需要提升   |

## 23.3 回归要求

任何新增优化动作都必须增加至少一个“开发连接不中断”测试。ProtectionPolicy 的覆盖率优先级高于优化命中率。

# 24. V1.0 验收标准

1\. Windows 10 x64 正常运行。

2\. 可持续监控 CPU、RAM、Commit、分页、SSD/HDD、进程 CPU/内存/I/O。

3\. 能建立 SSH/Telnet TCP Session 与 PID 映射。

4\. 能识别并保护常用远程终端、串口和抓包进程。

5\. Coding Mode 不会默认中断 SSH、Telnet、串口、Git、抓包或编译任务。

6\. 能识别 Memory Pressure、Paging、Disk Bottleneck、HDD Paging、Defender Impact。

7\. 所有可修改动作具备状态记录和回滚路径。

8\. 发生崩溃后可检测未完成 Session。

9\. 默认配置不永久关闭 Defender、不修改网络配置、不停止未知 Windows 服务。

10\. 输出优化前后的量化对比。

11\. WorkBoost 自身资源占用满足非功能目标或在报告中明确记录偏差。

# 25. Codex 实现约束

Codex 在生成代码或修改架构时遵循以下约束：

1\. 先完成可测试的数据模型和 Monitor 层，再实现优化动作；不得先写大量进程 Kill/Service Stop 逻辑。

2\. Windows API 封装集中在 platform/windows，Core 不直接散落 Win32 调用。

3\. 所有系统修改通过 OptimizationAction + SafetyValidator + ActionExecutor 链路执行。

4\. 任何 Action 不得绕过 ProtectionPolicy。

5\. 所有高权限命令使用强类型 Action 协议，不允许从主进程向 Elevated Helper 传递任意 command line 或 PowerShell 脚本。

6\. 新增进程规则必须指定 ProcessClass、ProtectionLevel、允许动作和测试用例。

7\. Unknown 默认不可优化。

8\. Security 默认不可 Kill/Suspend。

9\. RemoteTerminal、SerialTerminal、PacketCapture 默认 Strong Protection。

10\. 代码优先使用 RAII (Resource Acquisition Is Initialization) 管理 HANDLE、PDH Query、Service Handle 等 Windows 资源。

11\. 避免在 UI Thread 进行阻塞式性能采样和系统控制。

12\. Windows API 错误必须保留 GetLastError 错误码和上下文。

13\. 每完成一个 Phase，先提供 CLI 可验证结果和测试，再进入下一阶段。

14\. 阈值、进程规则和 Profile 尽可能配置化；不把个人机器上的具体进程路径硬编码进核心逻辑。

# 26. 首个开发里程碑

首个里程碑定义为 WorkBoost CLI Diagnostic MVP。该版本不执行系统优化，只回答“当前电脑为什么卡”和“哪些进程/磁盘/内存状态与卡顿相关”。

## 必须完成的命令

workboost status

workboost top cpu

workboost top mem

workboost top io

workboost connections

workboost protected

workboost diagnose --duration 60

## 完成条件

- 能在目标 Windows 10 办公机运行。

- Codex 卡顿复现期间可以明确看到 CPU、内存、Commit、分页、磁盘和 Top I/O Process。

- SSH/Telnet 会话能出现在 connections/protected 输出。

- 串口和抓包工具能进入 protected 列表。

- 诊断报告结构稳定，可作为后续 Coding Mode 的输入。

- 完成上述条件后再开始实现优化执行层。
