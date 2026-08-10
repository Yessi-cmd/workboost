# Testing

WorkBoost 的自动测试以“只读验证优先、修改范围最小、保护策略 fail-closed”为原则。自动测试不会 Stop/Start 真实 Windows 服务，也不会操作用户当前的 SSH、Telnet、串口、抓包、Git、构建或 IDE 进程。

## 自动化测试范围

当前测试程序覆盖：

- Commit Ratio 归一化和除零保护；
- 进程 CPU 按逻辑处理器数归一化、无效分母保护和上限；
- 进程规则大小写无关匹配、Unknown 默认 Strong，以及内置开发工作负载不可重分类降级；
- SSH/Telnet/自定义远程端口到 PID 的保护映射，以及自定义或空端口覆盖不能移除
  内置 TCP 22/23 基线；
- Wireshark/dumpcap Active Capture 保护；
- 优先级、Graceful Close 和临时服务动作的计划与二次安全校验；
- Coding Mode 清理池的 PID/启动时间身份绑定、显式确认、前台/远程/Unknown 拒绝、进程列表资源列与点击命中；
- 基线窗口保护并集（窗口内任意采样点的远程会话/抓包持续保护）与完整样本覆盖率 fail-closed；
- 多目标共享截止时间的批量 WM_CLOSE 投递、结果顺序保持和受保护/缺失目标拒绝；
- 退出后未关闭进程的精确身份收集、`UNCLOSED_PROCESS` 输出与 `retry-close` 轻量会话/报告链路；
- 系统关键服务、VPN/EDR、远程连接宿主，以及不完整进程/TCP 清单下对降优先级、Graceful Close 和服务控制的 fail-closed 保护；
- 畸形/越界配置的安全回退和数值上限；
- 1 MiB 配置上限，以及只有文件名的相对路径原子输出；
- 只包含固定字段的本地结构化日志、等级过滤和运行中跨越 1 MiB 阈值时的轮转；
- Elevated Helper 二进制协议、nonce、版本、长度和非法命令拒绝；
- 受 ACL 限制的 Named Pipe 双向身份确认；
- Helper 对关键服务请求的独立拒绝；
- 进程镜像、启动时间和提升状态的只读校验；
- Graceful Close 的真实受控窗口请求、部分窗口超时和崩溃窗口 `Uncertain` 语义；
- SCM、SetupAPI 串口、注册表/Startup Folder 的真实只读枚举；
- 被动启动观察的超时和路径型目标拒绝；
- 已有进程基线完成后才发出启动 ready 提示；
- 启动样本 Median、Baseline/Optimized Delta 和比较 JSON schema；
- 七页 Dashboard presenter、远端地址脱敏和清单呈现；
- Coding Mode 完成报告及丰富系统指标；
- Baseline/Optimized 完整窗口聚合、样本数、观测跨度和 Process/TCP/保护覆盖计数；
- Memory/Paging/Disk/HDD Paging/SSD Space/CPU/Defender/Background I/O 规则；
- 不完整进程/TCP 清单抑制 Defender、前台进程和后台未保护归因；
- 稀疏进程清单或磁盘实例不足半个窗口时不生成持续性结论；
- Session schema v3 往返、v1/v2 兼容、安全拒绝未知动作；
- 受控测试进程的真实优先级执行和立即回滚。

构建后运行：

```powershell
ctest --test-dir build -C Release --output-on-failure
```

MinGW 单配置构建可省略 `-C Release`。

对安装后的目录运行统一只读集成冒烟：

```powershell
.\tests\integration\installed_cli_smoke.ps1 -BinDirectory .\dist\bin
```

脚本检查相邻 `config`、产物不导入外部 MSVC/MinGW 编译器运行库、10 个 JSON
命令的 schema、Process/TCP 清单完整性字段、`status` 的单快照诊断声明、250 ms
Diagnostic Recording 及其清单覆盖计数、相对输出文件、参数错误、不完整启动比较和
Helper 参数拒绝。

## CLI 冒烟

每次影响 Collector、CLI 或报告 schema 的改动，至少验证：

```powershell
workboost status --json
workboost top cpu --json
workboost top mem --json
workboost top io --json
workboost connections --json
workboost services --json
workboost serial --json
workboost startup --json
workboost protected --json
workboost diagnose --duration 1 --interval 250 --json
workboost profile show coding --json
workboost coding enter --dry-run
```

所有 JSON 输出都必须使用标准解析器解析。错误和进度写入 stderr，不得污染 stdout。额外边界检查：

- `diagnose --interval 200` 返回 `64`；
- `benchmark compare codex.exe --duration 1 --json` 在没有新实例时返回 `1`，仍输出可解析的“不完整比较”，且默认 `requested_runs` 为 `3`；
- `connections --json` 默认不暴露完整远端地址；
- `startup --json` 不包含完整命令、参数或目录路径；
- `serial --json` 不打开或占用任何 COM 端口。

## GUI 与资源预算

GUI 验证应检查：

1. 不带参数启动 `workboost.exe`，可见原生主窗口。
2. Dashboard、Processes、Diagnosis、Coding Mode、Protected Workload、Recovery、Settings 七页均可切换。
3. 后台采样持续刷新，UI 线程不执行采样或 SCM 控制。
4. Refresh 只请求新采样；Export all 输出七页当前脱敏视图。
5. 标题栏关闭按钮和最小化都会隐藏到托盘，进程继续采样；左键单击托盘图标可恢复窗口。
6. 右键单击托盘图标会显示包含“打开”和“退出”的菜单；只有“退出”会结束进程，随后后台线程在有限时间内退出。
7. Coding Mode 左侧完整进程列表显示 CPU、Working Set、Disk I/O 与可选状态，可滚动；可选进程优先显示。
8. 左键单击可选进程后，它出现在右侧清理池；再次单击任一侧对应行会移出。受保护或无可见窗口的进程不能加入。
9. 使用受控测试窗口验证进入 Coding Mode 会传递 PID/启动时间并发送 `WM_CLOSE`；不要用自动测试关闭用户当前应用。
10. 使用一个忽略 `WM_CLOSE` 的隔离测试窗口验证：退出后输出 `UNCLOSED_PROCESS`，GUI 弹窗询问重试，`coding retry-close` 重新投递并生成独立报告；重试后进程仍存活时 WorkBoost 不得强杀。

当前开发机的最新 12 秒持续监控测量：

| 指标 | 结果 | 对应目标 |
|---|---:|---:|
| 归一化 CPU | 0.563% | 正常监控 `< 1%` |
| Private Bytes | 9.07 MiB | `< 80 MB` |

该记录证明当前构建满足正常监控和内存目标，不代表目标 Windows 10 设备，也不等同于“空闲 CPU `< 0.5%`”场景。发布前必须在目标设备重新测量空闲和持续监控两种状态。

## 启动响应对比

单阶段观察：

```powershell
workboost benchmark observe codex.exe --duration 60 --runs 3 --label cold
```

完整比较：

```powershell
workboost benchmark compare codex.exe --duration 60 --label coding-mode --json --output comparison.json
```

每次提示后由测试者自行启动新实例。已有同名实例必须被忽略；WorkBoost 不得代替用户启动、关闭或修改目标。比较模式先完成 Baseline，等待测试者应用环境变化并按 Enter，再完成 Optimized；两阶段默认各三次，以成功样本 Median 计算 `Optimized - Baseline`。

本机受控端到端验证已使用 WorkBoost GUI 测试进程完成两阶段各 3/3 成功样本，并确认六个实例均通过窗口关闭请求退出。该结果验证比较链路，不代表目标设备上的 Codex cold/warm 性能结论。

## 目标 Windows 10 隔离机验收

以下项目不能由当前开发机自动化结果替代：

1. 保持真实 SSH 和 Telnet 会话，确认 PID 映射正确，进入/退出 Coding Mode 全程不中断。
2. 打开串口终端和 COM 口，确认已知工具保持运行，WorkBoost 不占用端口。
3. 启动 Wireshark/dumpcap 抓包，确认相关进程和 Npcap 服务不进入动作计划。
4. 保持 Git/编译任务运行，确认 `coding enter --dry-run` 不产生处理动作。
5. 在隔离测试应用上验证 `WM_CLOSE`；未保存内容提示不得被绕过。
6. 在优先级动作后强制结束 WorkBoost，验证下次启动进入 Recovery，并能幂等恢复。
7. 模拟 `WM_CLOSE` 请求和落盘之间崩溃，确认动作变为 `Uncertain`，恢复不重放。
8. 用受控负载复现 Memory/Paging、HDD 高延迟和 SSD 低空间，确认基于窗口的诊断证据。
9. 以普通用户运行全部监控、诊断、盘点和 GUI；只有服务动作触发 UAC。
10. 创建无依赖、可安全恢复的专用测试 Updater 服务，在可信配置中显式允许。
11. 先用 `coding enter --dry-run` 验证 Medium 动作，再用 `--confirm-service-actions` 验证 UAC Stop；退出时再次 UAC 并恢复 Running，Startup Type 不变。
12. 在测试服务 Stop 成功但结果落盘前模拟崩溃，确认 `recovery restore` 对 Planned 动作执行幂等 Start。
13. 完成 Baseline/Optimized 各三次真实启动观察，核对窗口可见/响应计时和 Median Delta。
14. 重测空闲 CPU、持续监控 CPU 和 Private Bytes 预算。
15. 用忽略 `WM_CLOSE` 的隔离测试进程完整执行 enter → exit → retry-close，确认窗口保护并集与共享超时行为。

真实服务测试只能针对隔离机上的专用服务，绝不能使用业务、VPN、EDR、远程接入、抓包或设备服务。

## CI 与发布门禁

`.github/workflows/windows.yml` 在 Windows Server 2022 上分别使用 MSVC 和 MinGW-w64：

1. CMake 配置；
2. 启用警告的 Release 构建；
3. 运行全部测试；
4. 执行只读 `status --json` 冒烟。

本地只验证了一种工具链时，不得在 CI 通过前宣称另一工具链已验证。Windows Server CI 也不能替代 Windows 10 目标机、UAC 和真实隔离服务验收。逐条状态见 [`V1_ACCEPTANCE.md`](V1_ACCEPTANCE.md)。
