你现在需要为 WorkBoost 实现 GUI。

WorkBoost 是一个运行于 Windows 10 的轻量级系统性能诊断与优化工具，主要面向嵌入式网络开发工作环境。典型使用场景包括：

- Codex / VS Code
- SSH
- Telnet
- 串口终端
- Wireshark / 抓包
- Git
- 编译脚本
- 浏览器
- 企业办公软件

GUI 的首要目标是：

1. 信息清晰
2. 操作简单
3. 低资源占用
4. 视觉现代但克制
5. 不做传统“电脑管家 / 杀毒软件”风格
6. 让用户能够在 3~5 秒内知道当前系统瓶颈是什么
7. 让 Coding Mode 成为整个应用最核心的操作入口

---

# 1. 开发原则

首先阅读现有 WorkBoost 项目代码和设计文档。

不要为了 GUI 重写已经存在的 Monitor、Diagnosis、Optimization、ProtectionPolicy 等核心逻辑。

GUI 只负责：

- 展示状态
- 接收用户操作
- 调用 Core 层接口
- 展示执行结果

禁止将系统监控、诊断规则、进程 Kill、Service Control 等逻辑直接写入 UI 层。

如果项目当前已经确定 GUI 框架，则沿用现有框架。

如果项目尚未确定 GUI 技术方案，请先检查当前工程结构，并选择：

- Windows 10 支持良好
- C++ 集成简单
- 启动速度快
- 常驻内存较低
- 不依赖 Electron

的方案。

不要为了视觉效果引入明显重量级 Runtime。

---

# 2. 整体视觉风格

设计方向：

**Minimal / Clean / Developer Tool**

参考的视觉感觉：

- Windows 11 Settings 的克制布局
- VS Code 设置页的信息密度
- Linear / Raycast 类工具的简洁感
- 系统性能工具的专业感

但不要机械复制任何产品。

整体应当：

- 扁平
- 干净
- 少装饰
- 少渐变
- 少阴影
- 大面积纯色背景
- 明确的信息层级
- 中等信息密度
- 更像开发者工具，而不是消费级“优化软件”

---

# 3. 明确禁止的视觉元素

不要出现：

- 巨大的“电脑健康分数”
- 圆形 98 分评分盘
- 火箭动画
- 扫描雷达动画
- “您的电脑击败了 98% 用户”
- 大量绿色对勾
- 大量发光效果
- 霓虹色
- 游戏加速器风格
- 360 / 腾讯电脑管家式界面
- 大量卡通图标
- 大面积渐变
- 毛玻璃堆叠
- 每个数据都做成独立 Card
- 过度圆角
- 过度动画
- “立即加速 3.8GB”这类营销型文案

WorkBoost 是工程工具，不是“清理大师”。

---

# 4. 窗口结构

默认窗口尺寸建议：

1280 × 800 左右。

最小尺寸：

约 1000 × 650。

整体采用：

```text
┌─────────────────────────────────────────────────────────────┐
│ WorkBoost                                      Coding Mode   │
├─────────────┬───────────────────────────────────────────────┤
│             │                                               │
│ Dashboard   │                                               │
│ Processes   │                 Main Content                  │
│ Diagnosis   │                                               │
│ Protected   │                                               │
│ Coding Mode │                                               │
│ History     │                                               │
│             │                                               │
│ Settings    │                                               │
│             │                                               │
└─────────────┴───────────────────────────────────────────────┘
```

左侧固定 Sidebar。

宽度约：

180~210 px。

Sidebar 不需要复杂图标。

每个入口使用：

图标 + 文本

即可。

当前页面通过：

- 浅色背景
- 左侧细强调条
- 或轻微高亮

表示。

不要使用巨大的彩色按钮作为 Navigation。

---

# 5. 页面结构

V1 GUI 包含：

1. Dashboard
2. Processes
3. Diagnosis
4. Protected Workload
5. Coding Mode
6. History / Reports
7. Settings

优先完成：

Dashboard

然后再逐步实现其他页面。

---

# 6. Dashboard

Dashboard 是最重要的页面。

核心问题只有两个：

**电脑现在怎么样？**

**为什么卡？**

建议布局：

```text
Dashboard

System
────────────────────────────────────────

CPU          34%
Memory       13.8 / 15.8 GB
Available    2.0 GB
Commit       82%

SSD C:       18%      1.4 ms
HDD D:       96%      71 ms


Diagnosis
────────────────────────────────────────

HIGH
HDD I/O Bottleneck

PhysicalDrive1
Active Time 96%
Average Latency 71 ms

Paging activity is also elevated.


MEDIUM
Memory Pressure

Available Memory 2.0 GB
Commit 82%


Top Impact
────────────────────────────────────────

MsMpEng.exe        24 MB/s       High
SearchIndexer.exe  11 MB/s       Medium
WeCom.exe          820 MB        Medium


Protected Workload
────────────────────────────────────────

SSH       172.16.x.x:22        Active
Xshell                           Protected
Wireshark                        Capturing
```

不要把 CPU、Memory、SSD、HDD 分别设计成巨大 Card。

推荐：

一个 System Overview 区域，使用紧凑的行。

例如：

```text
CPU        ███████░░░    34%

Memory     █████████░    82%
           13.8 / 15.8 GB

SSD C:     ██░░░░░░░░    18%     1.4 ms

HDD D:     ██████████    96%     71 ms
```

进度条要非常克制。

---

# 7. 顶部状态区域

窗口顶部右侧显示当前模式：

```text
Normal
```

或：

```text
Coding Mode
```

Coding Mode 激活后可以使用一个轻量强调色状态：

```text
● Coding Mode
```

旁边提供：

```text
Exit
```

不要做成巨大的常驻 Banner。

---

# 8. Coding Mode 核心入口

Dashboard 中可以放一个主要操作区域：

```text
Coding Mode

Reduce unnecessary background activity while
keeping development connections protected.

Protected now:
3 remote sessions
1 packet capture

[ Enter Coding Mode ]
```

如果当前已经进入：

```text
Coding Mode Active

12 min

3 optimization actions active
4 protected workloads

[ View Actions ]    [ Exit Coding Mode ]
```

Coding Mode 按钮是整个 GUI 唯一需要明显 Primary Style 的按钮之一。

不要出现：

“一键加速”

“一键释放内存”

“立即优化”

这样的文案。

统一称为：

**Enter Coding Mode**

---

# 9. Diagnosis 区域

Diagnosis 必须比单纯性能数字更醒目。

优先级：

```text
Diagnosis > Raw Metrics
```

诊断结果使用：

Severity：

- Critical
- High
- Medium
- Low
- Info

但颜色要克制。

建议：

Critical / High：

红色或橙红色小标记

Medium：

橙色

Low / Info：

灰蓝色

颜色仅用于：

- 小圆点
- Badge
- 左侧 3~4px 边线
- 小型文本强调

不要把整张卡染成红色。

例如：

```text
● HIGH

HDD I/O Bottleneck

PhysicalDrive1 is saturated.

Active Time        97%
Average Latency    68 ms
Queue Length       6.2
```

下方可以展开：

```text
Evidence
```

查看详细证据。

---

# 10. Processes 页面

这是一个偏专业的 Data Table。

顶部：

```text
Processes                     Search processes...

[ All ] [ High Impact ] [ Protected ]
```

表格：

```text
Name             CPU     Memory     Read/s    Write/s    Impact   Status
────────────────────────────────────────────────────────────────────────
Codex.exe        5.2%    820 MB     2 MB/s    1 MB/s     Low      Protected
MsMpEng.exe      8.1%    420 MB     24 MB/s   1 MB/s     High     Security
Xshell.exe       0.2%    140 MB     —         —          Low      Protected
WeCom.exe        2.1%    830 MB     3 MB/s    1 MB/s     Medium   Normal
```

表格支持：

- CPU 排序
- Memory 排序
- Disk I/O 排序
- Impact 排序
- Search
- Protected Filter

点击进程后在右侧或者下方展开详情。

不要默认放：

```text
Kill Process
```

这种高风险按钮。

详细页面可提供：

```text
Actions

Set priority
Add to Always Protect
Allow in Coding Mode
Graceful Close
```

Terminate Process 放进：

```text
Advanced Actions
```

并使用危险操作样式。

---

# 11. Protected Workload 页面

这个页面对本项目很重要。

用于明确告诉用户：

**WorkBoost 当前不会碰哪些开发任务。**

页面布局：

```text
Protected Workload

Remote Sessions
────────────────────────────────────────

SSH

ssh.exe
PID 4312

Remote
172.16.30.137:22

State
ESTABLISHED


Serial
────────────────────────────────────────

Xshell.exe
COM3

Protected


Packet Capture
────────────────────────────────────────

Wireshark.exe
dumpcap.exe

Capture Active
```

Protection 原因需要显示：

```text
Protected because:
Active SSH session
```

或：

```text
Protected because:
Packet capture is running
```

不要只显示一个抽象的：

Protected

否则用户不知道为什么。

---

# 12. Coding Mode 页面

显示当前 Optimization Plan。

进入前：

```text
Coding Mode

The following actions will be applied:

────────────────────────────────────────

Codex.exe
Priority
Normal → Above Normal

Safe


ExampleUpdater.exe
Priority
Normal → Below Normal

Safe


ExampleUpdater.exe
Close application

Low Risk


Protected

ssh.exe
Xshell.exe
Wireshark.exe
dumpcap.exe

These processes will not be modified.

────────────────────────────────────────

[ Enter Coding Mode ]
```

进入后：

```text
Coding Mode Active

Started
14:32

Duration
18 min

Active Actions
3

Protected Workloads
4


Active Changes
────────────────────────────────────────

Codex.exe
Priority
Above Normal

ExampleUpdater.exe
Closed

...


[ Exit and Restore ]
```

---

# 13. Optimization Plan 必须明确展示

不要让 Coding Mode 成为黑盒。

进入 Coding Mode 前最好明确展示：

```text
Will change

3 items

Will protect

7 processes

Will not change

Windows Defender
Network Configuration
SSH Sessions
Serial Sessions
Packet Capture
```

用户可以展开：

```text
View details
```

---

# 14. History / Reports

列表式布局：

```text
History

Today

14:32
Coding Mode
18 min

Main bottleneck:
HDD I/O

Disk latency
68 ms → 24 ms

Available memory
1.9 GB → 3.8 GB


11:14
Diagnostic Recording
60 sec

Main bottleneck:
Memory Pressure
```

点击进入详细报告。

不要做成复杂 Dashboard Analytics。

---

# 15. Settings

分为：

```text
General
Monitoring
Coding Mode
Protected Apps
Diagnosis
Advanced
```

重点设置：

### General

- Start with Windows
- Minimize to tray
- Theme
- Language

### Monitoring

- Sample interval
- History duration

### Coding Mode

- Codex priority
- IDE priority
- Allowed background apps
- Allow graceful close

### Protected Apps

用户可以看到：

```text
Always Protect

Xshell.exe
ssh.exe
Wireshark.exe
...
```

支持：

Add Process

Remove

### Advanced

高风险功能集中在这里。

需要视觉隔离。

---

# 16. 色彩规范

优先支持：

Light Theme

后续再实现 Dark Theme。

Light：

```text
Background:
接近 #F7F8FA

Surface:
白色

Primary Text:
接近 #1E1E1E

Secondary Text:
中性灰

Border:
非常浅的灰

Accent:
单一蓝色系
```

不要到处使用 Accent Color。

Accent 主要用于：

- Primary Button
- Active Navigation
- Selected Control
- Chart Highlight

状态颜色：

```text
Normal:
灰 / 蓝灰

Success:
低饱和绿色

Warning:
低饱和橙色

Critical:
低饱和红色
```

保持低饱和。

---

# 17. 字体

优先使用 Windows 系统字体。

中文：

Microsoft YaHei UI

英文 / 数字：

Segoe UI

数值数据可考虑使用：

Consolas

但不要整个界面使用等宽字体。

例如：

```text
Memory

13.8 / 15.8 GB
```

数字部分可使用等宽字体，方便数据对齐。

---

# 18. 间距系统

使用统一 spacing system。

例如：

```text
4
8
12
16
24
32
```

禁止随意出现：

13px
19px
27px

之类无法解释的间距。

常用：

页面边距：

24~32px

Section 间距：

24px

组件内部：

12~16px

表格行：

约 36~44px

---

# 19. 圆角

保持非常克制。

建议：

按钮：

4~6px

Card / Panel：

6~8px

不要使用：

16px
20px
24px

这种明显移动 App 风格的大圆角。

---

# 20. 阴影

原则：

尽量不用。

优先通过：

- Background
- Border
- Spacing

区分层级。

如果需要阴影，只用于：

- Dialog
- Floating Popup

普通 Panel 不使用明显阴影。

---

# 21. 动画

动画不是重点。

只允许：

- 页面切换轻微 transition
- Progress 数值更新平滑
- Expand/Collapse
- Loading Indicator

持续时间：

100~200ms 左右。

禁止：

- 数字疯狂滚动
- 火箭动画
- 粒子
- 雷达扫描
- 长时间渐变动画

性能工具本身不能为了动画制造额外负载。

---

# 22. 图表

图表只在真正需要时间趋势时使用。

例如：

```text
CPU last 60s

Memory last 60s

Disk latency last 60s
```

使用简单 Line Chart。

原则：

- 无渐变填充
- 网格线非常淡
- 不显示复杂 Legend
- 鼠标 Hover 显示 Timestamp + Value

Dashboard 不需要同时出现五六张图。

默认只显示当前状态。

历史曲线放到：

```text
Details
```

中。

---

# 23. 实时刷新

性能数据默认：

1000 ms

UI 不要因为每次 Sample 就重新创建整个 Widget Tree。

尽量：

更新已有控件状态。

避免：

- 整页重新布局
- 表格频繁闪烁
- CPU 排序导致每秒整张表跳动

Processes 页面如果按照 CPU 排序：

可以每 2~3 秒更新排序一次，而数据本身仍每秒刷新。

---

# 24. Loading State

初始化采集器时：

```text
Collecting system metrics...
```

简单 Spinner 即可。

不要使用：

```text
Scanning your computer...
Checking 127 optimization items...
```

---

# 25. Empty State

例如没有 SSH：

```text
No active remote sessions
```

没有诊断异常：

```text
No significant bottleneck detected.
```

不要写：

```text
Great! Your PC is perfectly healthy!
```

---

# 26. 错误状态

例如权限不足：

```text
Administrator permission required

This action requires elevated privileges.

[ Continue ]
```

如果企业策略阻止：

```text
Action unavailable

This setting is managed by your organization.
```

直接展示技术事实。

不要把错误吞掉。

允许查看：

```text
Details

Error:
ERROR_ACCESS_DENIED (5)
```

---

# 27. Tray

后续支持系统托盘。

Tray Menu：

```text
WorkBoost

Current:
Normal

CPU      32%
Memory   81%
HDD      94%

Open WorkBoost
Enter Coding Mode
Exit
```

Coding Mode 开启：

```text
● Coding Mode
```

托盘图标不需要动态显示 CPU 百分比。

---

# 28. GUI 与 Core 的边界

UI 层禁止出现类似：

```cpp
TerminateProcess(...)
ControlService(...)
SetPriorityClass(...)
```

调用。

GUI 只能调用 Application/Core 提供的接口，例如：

```cpp
app.EnterCodingMode();

app.ExitCodingMode();

app.GetCurrentSnapshot();

app.GetDiagnosis();

app.GetProtectedWorkloads();

app.GetProcesses();
```

系统状态修改全部经过：

```text
UI
 ↓
Application
 ↓
OptimizationPlanner
 ↓
ProtectionPolicy
 ↓
SafetyValidator
 ↓
ActionExecutor
 ↓
Windows Adapter
```

---

# 29. UI State Model

建议 GUI 不直接读取各个 Monitor。

维护统一：

```cpp
struct DashboardViewModel {
    SystemSummary system;
    std::vector<DiagnosisViewModel> diagnosis;
    std::vector<ImpactProcessViewModel> top_processes;
    std::vector<ProtectedWorkloadViewModel> protected_workloads;
    CodingModeState coding_mode;
};
```

UI 只绑定 ViewModel。

Core Snapshot 转 ViewModel 的逻辑放在 Application 层或者专用 Presenter/ViewModel 层。

---

# 30. 第一阶段 GUI 实现范围

不要一次实现全部 GUI。

第一阶段只实现：

## Main Window

- Sidebar
- Header
- Dashboard

## Dashboard

- System Overview
- Diagnosis
- Top Impact Processes
- Protected Workload
- Coding Mode Entry

数据暂时使用现有 Core。

如果部分 Core API 尚未完成：

定义清晰 Interface + Mock Data。

不要为了 GUI 临时把测试数据写死在 Widget 中。

---

# 31. 第二阶段

实现：

Processes

Protected Workload

Coding Mode

---

# 32. 第三阶段

实现：

Diagnosis Details

History

Settings

Tray

---

# 33. 第一阶段验收标准

运行程序后，应看到一个现代、克制的 Windows 开发工具界面。

必须满足：

1. 没有传统电脑管家视觉元素。
2. Dashboard 一屏可以看完核心状态。
3. HDD、Memory、Diagnosis 的重要程度一眼可区分。
4. Coding Mode 是最明显的主操作，但不会压过整个界面。
5. SSH/Telnet/串口/Wireshark 等 Protected Workload 状态清晰可见。
6. 界面在 1280×800 下无需滚动即可看到 Dashboard 核心内容。
7. 1000×650 下仍然可正常使用。
8. 实时数据更新无明显闪烁。
9. GUI 自身不得显著增加 CPU / 内存占用。
10. UI 层不包含系统优化实现逻辑。
11. 所有高风险操作必须有明确状态和错误反馈。
12. Windows 10 下正常运行。

---

# 34. 实施方式

开始编码前：

1. 阅读整个 WorkBoost 工程。
2. 找出已有 Core API。
3. 确认当前 UI 技术栈。
4. 列出需要新增的 ViewModel / Application API。
5. 给出计划中的 GUI 文件结构。
6. 然后直接开始实现。

不要大规模修改已经工作的核心模块。

优先让：

```text
Dashboard
```

达到完整可运行状态。

完成 Dashboard 后运行程序检查：

- Layout
- Alignment
- Font
- Spacing
- Resize
- Realtime Update
- CPU / RAM overhead

发现视觉或布局问题直接修复。

最终输出：

1. 修改了哪些文件
2. GUI 架构
3. 当前已经实现哪些页面
4. 哪些数据来自真实 Core
5. 哪些仍是 Mock
6. 如何编译运行
7. 下一阶段建议