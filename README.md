# WorkBoost

WorkBoost 是面向 Windows 10 x64 嵌入式网络开发工作站的低开销诊断工具。它通过 Win32、PSAPI、PDH 和 IP Helper API 采集真实系统数据，帮助定位 CPU、内存、分页、磁盘和后台进程竞争，同时优先保护 SSH、Telnet、串口终端、抓包、Git 和构建任务。

当前版本实现了设计文档中的 CLI Diagnostic MVP，并提供了安全、可回滚的 Coding Mode 优先级动作基础链路。

## 已实现

- 系统 CPU、物理内存、Commit、Page Reads/sec、Pages Input/sec 采样。
- 物理磁盘 Active Time、延迟、队列、吞吐采样，以及 SSD/HDD 识别。
- 进程 CPU、Working Set、Private Bytes、读写 I/O、优先级、前台窗口状态采样。
- IPv4/IPv6 TCP 连接到 PID 的映射；远端地址默认脱敏。
- SSH/Telnet、自定义远程调试端口、串口工具、Wireshark/dumpcap、Git 和构建任务保护。
- Unknown 进程默认 Strong Protection，不自动优化。
- 基于滑动时间窗口的内存、分页、磁盘、HDD 分页、CPU、Defender 和后台 I/O 诊断。
- 文本/JSON 诊断报告。
- Coding Mode 的强类型优先级计划、安全校验、逐动作持久化、逆序回滚和崩溃恢复。
- Coding Mode 前后指标及 Delta 报告。

以下设计阶段暂不属于当前 CLI 版本：服务 Stop/Restore、Elevated Helper、GUI 和 ETW 深度文件 I/O 关联。默认版本不会停服务、结束进程、修改启动项、网络配置或安全软件状态。

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

生成的程序为 `workboost.exe`。监控和诊断不需要管理员权限。

## 命令

```text
workboost status
workboost top cpu
workboost top mem
workboost top io
workboost connections
workboost protected
workboost diagnose --duration 60
workboost diagnose --duration 60 --json --output report.json
workboost profile show coding
workboost coding enter --dry-run
workboost coding enter
workboost coding exit
workboost recovery status
workboost recovery restore
```

`connections` 默认把远端 IPv4 最后一段或 IPv6 后缀替换为掩码。仅在用户明确需要时使用 `--show-remote-ip`。

进入 Coding Mode 前默认采集 10 秒基线。建议先使用 `--dry-run` 检查计划。当前执行器只接受 `SetPriorityClass` 强类型动作，且目标优先级仅允许 `below_normal`、`normal` 或 `above_normal`；任何动作都必须同时通过 Profile 和 ProtectionPolicy。

## 配置

仓库默认配置位于 [`config`](config)：

- `diagnosis.json`：采样周期、历史窗口、诊断阈值、受保护远程端口。
- `profiles.json`：Coding Mode 前台优先级、Always Protect 和显式允许列表。
- `process_rules.json`：进程分类及默认保护级别。

加载优先级为：内置安全默认值 → 可执行文件旁的 `config` → 当前目录的 `config` → `%LOCALAPPDATA%\WorkBoost` 用户覆盖。也可用 `--config-dir` 指定单一配置目录。

运行时状态保存在：

```text
%LOCALAPPDATA%\WorkBoost\
├── state\active_session.json
└── reports\<session-id>.json
```

发现未完成的 `active_session.json` 时，新的 Coding Mode 会被阻止；使用 `recovery status` 检查，再用 `recovery restore` 逆序恢复已记录动作。恢复失败时状态保持为 Safe Mode，不继续执行新的修改。

## 安全边界

- 不执行任意 Shell、PowerShell 或命令行字符串。
- 不自动 Kill/Suspend 进程，不停未知服务。
- 不修改 Defender、EDR、网络接口、路由、DNS、代理、MTU 或防火墙。
- 不通过清空 Working Set 或系统缓存伪造“内存优化”。
- PID 和进程启动时间一起校验，避免 PID 复用导致误操作。
- 每个动作执行前后立即原子持久化恢复载荷。
- 日志和报告不记录 SSH/Telnet/串口正文或抓包内容。

详细实现边界见 [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)，原始规格见 [`WorkBoost_Windows10_Embedded_Dev_Design.md`](WorkBoost_Windows10_Embedded_Dev_Design.md)。
