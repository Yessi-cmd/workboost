# AGENTS.md

本文件适用于整个 WorkBoost 仓库。后续若某个子目录包含更具体的
`AGENTS.md`，则该文件只可补充或收紧本文件的要求，不得放宽安全边界。

## 1. 项目使命与当前范围

WorkBoost 是面向 Windows 10 x64 嵌入式网络开发工作站的低开销性能诊断
工具，使用 C++17、Win32、PSAPI、PDH、IP Helper API 和 Storage IOCTL。

当前稳定范围是：

- CLI Diagnostic MVP；
- 系统、磁盘、进程、TCP/PID、SCM 服务、串口和启动项只读监控；
- 开发工作负载分类与保护；
- 基于时间窗口的诊断；
- 安全优先级调整、显式允许 Graceful Close，以及显式确认的临时服务 Stop/Restore；
- 原子会话持久化、逆序回滚和崩溃恢复；
- 被动启动响应 Benchmark、Baseline/Optimized 三次 Median 对比和原生 Win32 七页 Dashboard；
- 250–500 ms Diagnostic Recording，以及只接受固定事件的本地轮转日志。

启动项修改和 ETW 深度诊断仍属于后续阶段。
除非用户明确要求扩展对应阶段，否则不要顺手引入这些功能。

主要参考文档：

- `WorkBoost_Windows10_Embedded_Dev_Design.md`：产品设计和验收基准；
- `README.md`：当前已实现能力、命令和用户边界；
- `docs/ARCHITECTURE.md`：实现数据流、安全链路和恢复语义；
- `docs/TESTING.md`：现有测试和目标机验收场景。

若文档表述存在差异，采用更安全、更保守的解释，并在实现或交付说明中指出。

## 2. 不可破坏的安全不变量

任何代码、配置、测试和重构都必须保持以下不变量：

1. `System` 和 `Security` 对象不可被通用优化策略修改。
2. `RemoteTerminal`、`SerialTerminal`、`PacketCapture`、`BuildTool` 和
   `VersionControl` 默认至少为 `Strong` Protection。
   内置已知开发工作负载的名称、类别和 Strong 下限不可被配置重分类或降级；配置只能
   新增未知工具规则或进一步收紧保护。
3. 未识别进程默认为 `Unknown + Strong`，不是优化候选。
4. 用户配置只能收紧系统关键对象的保护，不能把内置 denylist 降级。
   `always_protect` 优先于 Development 进程升优先级特例，禁止任何自动修改。
5. 已建立的 SSH、Telnet 或用户配置远程调试端口连接不得被中断、暂停或降至
   Idle 优先级。
   当进程或 TCP 清单不完整时，任何降优先级或 Graceful Close 动作必须
   fail-closed；不能把“未观察到连接”当成“确定无连接”。
6. dumpcap 存在时视为 Active Capture；不得结束抓包进程、停止 Npcap 或修改
   网络接口。
7. 默认不得 Kill/Suspend 进程、停止未知服务、禁用启动项或修改安全软件。
8. 不得修改 IP、DNS、路由、Proxy、MTU、防火墙、网卡电源或 TCP 参数。
9. 不得通过清空 Working Set、Standby List 或系统缓存伪造内存改善。
10. 不得执行由配置或 IPC 传入的任意 Shell、PowerShell 或命令行字符串。
11. 每个系统修改必须经过
    `OptimizationAction -> SafetyValidator -> ActionExecutor`，不得绕过
    `ProtectionPolicy`。
12. 操作进程时必须同时校验 PID 和进程启动时间，防止 PID 复用。
    启动时间或当前 Priority Class 无法采集时不得生成对应进程动作。
13. 执行动作前必须持久化 `Planned` 状态和完整恢复载荷；结果随后立即落盘。
14. `Planned` 必须可被视为“执行状态未知”并通过幂等回写原值安全恢复。
15. 回滚按动作执行顺序的逆序进行。恢复或持久化失败时必须保持
    `active_session.json` 并进入 Safe Mode。
16. 报告和日志不得记录 SSH/Telnet/串口正文、抓包内容、凭据或令牌；远端 IP
    默认脱敏。
17. `Logger` 的生产调用只可传入固定 `LogEvent`、`LogLevel` 和数值错误码；不得为
    方便调试扩展任意字符串、路径、PID、地址或命令载荷字段。

安全覆盖率优先于优化命中率。不能确认对象是否安全时，选择只监控、不修改。

## 3. 架构和依赖方向

保持以下依赖方向：

```text
platform/windows -> core model
collector -> SystemSnapshot -> SnapshotHistory
SnapshotHistory -> DiagnosisEngine
SystemSnapshot + RuntimeContext -> ProtectionPolicy
ProtectionPolicy -> OptimizationPlanner -> SafetyValidator -> ActionExecutor
ActionExecutor -> SessionManager
CLI -> application/core/platform public interfaces
```

目录职责：

- `src/platform/windows`：所有 Win32、HANDLE、PDH、IP Helper 和 Storage API
  细节；
- `src/core/model`：稳定、无平台资源所有权的数据模型；
- `src/core/config`：安全默认值和向后兼容的配置加载；
- `src/core/benchmark`：被动启动样本汇总、Median 和比较报告；
- `src/core/logging`：无任意文本载荷的本地结构化事件日志；
- `src/core/policy`：分类后的最终保护判定；
- `src/core/diagnosis`：只读、可测试的窗口规则；
- `src/core/optimization`：强类型计划、校验、执行和回滚；
- `src/app`：会话和应用级编排；
- `src/cli`：参数、呈现和退出码，不承载底层 Win32 逻辑；
- `src/gui`：Win32 窗口、后台采样编排和可测试的 DashboardPresenter；
- `tests/unit`：确定性规则测试及受控的最小 Windows 集成测试。

Core 层不得散落裸 Win32 调用。Windows 资源使用 RAII；HANDLE 使用
`windows::UniqueHandle` 或等价封装，PDH Query、Service Handle 等新资源也必须
有确定的释放路径。

Windows API 失败必须保留 `GetLastError()` 错误码和操作上下文。不要只返回
`false` 或模糊的“操作失败”。

Winsock 相关实现必须先包含 `winsock2.h`/`ws2tcpip.h`，再包含
`iphlpapi.h`。保持 `_WIN32_WINNT=0x0A00` 和
`NTDDI_VERSION=0x0A000000`，避免 MinGW 与 MSVC 暴露不同 API。

## 4. 编码规范

- 使用 C++17，不依赖编译器私有扩展。
- 遵循仓库 `.clang-format`：Google 基础风格、2 空格缩进、88 列。
- 文本使用 UTF-8；遵循 `.editorconfig`。
- 优先使用固定宽度整数、`std::chrono`、RAII 和值语义。
- 区间速率使用 `steady_clock`；需要写入报告的时间使用系统时钟。
- Coding Mode 的 Baseline/Optimized 指标必须聚合完整采样窗口并记录样本数，不能退化为
  仅取最后一个快照。
  Development 指标只使用 Process 完整样本；后台未保护 Top I/O 只使用 Process+TCP
  同时完整的样本，并持久化覆盖计数。
- 进程名匹配必须大小写无关，并在边界处统一标准化。
- CPU/I/O 速率必须基于至少两次采样的差值；进程 CPU 必须按逻辑处理器数
  归一化为整机百分比。
- 进入模型或 JSON 前拒绝 PDH 的 NaN/Infinity；机器可读输出不得出现非标准数值。
- 阈值、进程规则和 Profile 应配置化；只有不可降级的安全 denylist 可以固化。
- 保持低开销：正常监控目标 CPU `< 1%`、Private Bytes `< 80 MB`。
- 不新增重量级依赖，除非现有标准库/Windows API 无法合理完成任务，并在说明中
  给出体积、部署和安全影响。

## 5. 配置和数据兼容性

配置加载顺序必须保持：

```text
内置安全默认值
  -> 可执行文件旁 config
  -> 当前目录 config
  -> %LOCALAPPDATA%\WorkBoost 用户覆盖
```

修改配置时：

- 缺失文件或字段必须回退到安全默认值；
- 新字段应有向后兼容默认值；
- 不得允许配置覆盖不可降级的系统/安全进程规则；
- 新进程规则必须声明 `ProcessClass`、`ProtectionLevel`、允许动作和测试；
- 新远程端口必须进入动态保护判定；
- TCP 22/23 是不可删除的保护基线；`remote_debug_ports` 只能增加或重置其他自定义
  端口，不能移除 SSH/Telnet。
- 破坏 JSON 报告结构时必须提升 `schema_version` 并更新文档/测试；
- 不要把开发者个人路径、机器专有 PID 或内网地址写入默认配置。

## 6. 诊断规则要求

新增或修改诊断规则时：

1. 使用 `SnapshotHistory` 时间窗口，不用单点峰值直接下结论。
2. 阈值进入 `diagnosis.json`/`DiagnosisThresholds`，不散落魔法数字。
3. 每个结果包含稳定的 `type`、`severity`、`confidence`、`summary` 和
   `evidence`。
4. Evidence 必须足以解释结论，但不得包含受保护内容或默认长期保存源码路径。
5. 诊断层只输出结论，不能直接执行优化动作。
6. 至少添加一个命中用例和一个不应命中的边界用例。
7. Defender/EDR 影响只能诊断；不得生成 Kill、Suspend 或永久禁用动作。
8. Defender、后台 I/O 和前台进程归因只能使用完整进程清单；声称后台进程
   “未保护”时还必须有完整 TCP 清单。
9. 依赖可缺失清单或实例的窗口结论必须覆盖至少一半窗口且不少于两个样本；
   不能用两个孤立观测代表长窗口。

## 7. 新优化动作的准入条件

当前自动执行器只允许 `SetPriorityClass`、`GracefulCloseProcess` 和
`StopServiceTemporary`。
优先级自动目标仅限 Below Normal、Normal 和 Above Normal；Graceful Close 为 Low 风险、
默认关闭、只发送 `WM_CLOSE`，且不可声称可逆。多个窗口必须共享动作超时预算；部分投递
无法确认且进程仍存活时记录为 Uncertain。临时服务 Stop 为 Medium 风险，必须同时
具备已知可优化分类、Profile 显式允许、CLI 显式确认、完整保护清单和稳定服务身份；恢复
只回到原始 Running 状态，绝不修改 Startup Type。

若用户明确要求新增动作，必须同时完成：

- 强类型 `ActionType` 和明确 `Risk`；
- 默认关闭或由 Profile 显式允许；
- Planner 候选生成；
- SafetyValidator 独立二次校验；
- ActionExecutor 白名单实现，不接受任意命令文本；
- 执行前可序列化的恢复载荷；
- 幂等回滚和进程/服务身份验证；
- 每步原子持久化；
- 崩溃窗口和恢复失败测试；
- 至少一个“开发连接不中断”回归测试；
- README、ARCHITECTURE 和配置示例更新。

`TerminateProcess`、永久服务/启动项修改和安全组件修改不得作为自动动作。需要
管理员权限动作只能通过强类型 Elevated Helper 协议实现，不能传递 Shell
命令或脚本。

## 8. CLI 和输出约束

不得无故破坏以下命令及现有选项：

```text
workboost
workboost gui
workboost status
workboost top cpu|mem|io
workboost connections
workboost services
workboost serial
workboost startup
workboost benchmark observe <process.exe>
workboost benchmark compare <process.exe>
workboost protected
workboost diagnose --duration N [--interval 250..500]
workboost profile show coding
workboost coding enter|exit
workboost recovery status|restore|acknowledge
```

- JSON 输出必须可被标准解析器直接解析，字段命名保持稳定。
- `status/top/connections/protected --json` 必须暴露相关采集清单的完整性；单快照
  `status` 不得伪装成完整时间窗口诊断。
- 机器可读输出写 stdout；警告和错误写 stderr，避免污染 JSON。
- `connections` 默认脱敏远端 IP，只有显式 `--show-remote-ip` 才显示完整地址。
- 退出码约定：`0` 成功，`1` 一般运行错误，`2` Safe Mode/需要恢复，`64`
  参数用法错误。
- 高成本采样不得阻塞 GUI/UI 线程；CLI 中也要明确告知长时间采样。
- 启动项和串口默认只读；启动项输出不得包含完整命令、参数或目录路径。
- `benchmark observe` 和 `benchmark compare` 只能被动观察配置为 Development 的
  进程 basename，不得代替用户启动、关闭或修改目标进程。比较默认 Baseline/Optimized
  各三次；样本不完整时必须返回非零状态且不得伪造 Delta。
- `diagnose --interval` 只接受 250–500 ms，并在 JSON/TXT 中记录实际周期和样本数。
  报告还必须记录 Process/TCP 及两者同时完整的样本数量。

## 9. 构建与测试

Visual Studio 2022：

```powershell
cmake -S . -B build -A x64 -DWORKBOOST_BUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

MinGW-w64：

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DWORKBOOST_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

变更后的最低验证：

- Model/Config/Policy：运行完整单元测试；
- Collector/Windows API：无警告构建，并运行 `status`、相关 `top`、
  `connections --json`、`serial --json`、`startup --json` 冒烟测试；
- Diagnosis：运行合成窗口测试，并用标准 JSON 解析器验证报告；磁盘容量证据不完整时
  不得触发 SSD 空间结论；
- Benchmark/GUI：覆盖无目标超时、多进程启动观察、Median、Baseline/Optimized Delta、
  不完整比较、七页呈现、隐私脱敏、后台线程退出和资源预算；
- Logging：验证 JSONL 每行可解析、等级过滤、字段白名单和运行时 1 MiB 轮转，不把
  本机日志加入仓库；
- Optimization/Recovery：覆盖保护拒绝、PID 启动时间、服务身份、提权协议、恢复载荷
  往返和实际回滚；真实服务 Stop/Start 只允许在隔离测试机手动验收；
- CMake/CI：同时考虑 MSVC 与 MinGW；本地只有一种工具链时，必须等待 Windows
  GitHub Actions 通过再宣称完成；发布目标必须静态链接编译器运行库，安装后配置必须
  位于可执行文件旁的 `bin/config`，并运行
  `tests/integration/installed_cli_smoke.ps1`。

不要为了测试直接对用户当前的 Codex、IDE、SSH、Telnet、串口、抓包、Git、构建
进程或系统服务执行动作。优先使用：

- `coding enter --dry-run`；
- 合成 `SystemSnapshot`；
- 测试进程自身的短暂优先级调整并立即回滚；
- 明确隔离、可恢复的测试进程。

自动测试不得 Stop/Start 用户或系统的真实服务。服务控制测试使用合成快照、协议往返、
只读 SCM 查询和“关键服务必拒绝”路径；需要实际控制时必须使用隔离测试机上的专用服务。

`build/`、可执行文件、报告和本机运行状态不得提交。

## 10. Git 和变更纪律

- 开始前运行 `git status -sb`，保留并避开不属于当前任务的用户改动。
- 不使用 `git reset --hard`、强制 checkout 或其他破坏性命令清理工作树。
- 只提交与任务有关的源文件、配置、测试和文档。
- 不提交 `%LOCALAPPDATA%\WorkBoost` 状态、真实远端 IP、机器路径或诊断样本。
- 未经用户明确要求，不提交、不推送、不创建 PR。
- 推送前运行相关测试和 `git diff --check`；推送后确认 GitHub Actions 结果。
- 新安全边界、配置字段、CLI 行为或报告 schema 必须同步更新文档。

## 11. 完成定义

任务只有在以下条件同时满足时才算完成：

1. 实现符合当前阶段范围和上述安全不变量。
2. MSVC/MinGW 可用工具链下无新增编译警告。
3. 相关单元、集成和 CLI 冒烟测试通过。
4. 新动作具有执行前恢复载荷、持久化和回滚测试。
5. JSON 输出和隐私默认值未被破坏。
6. 配置、README、架构或测试文档已按需同步。
7. 工作树中没有意外构建产物或无关改动。
