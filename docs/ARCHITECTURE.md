# Architecture

## 数据流

```text
Windows APIs
  Win32 / PSAPI / PDH / IP Helper / Storage IOCTL
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
                |
                v
         SessionManager
```

Windows API 调用集中在 `src/platform/windows`。Core 层只依赖稳定数据模型，不直接持有裸 HANDLE 或 PDH Query。

## 采样

`SystemCollector::Initialize` 建立 CPU、进程和 PDH 的第一组计数器基线；后续 `Sample` 使用区间差值计算速率。进程 CPU 会除以逻辑处理器数量，最终表示该进程占整机 CPU 的百分比。进程 PID 与启动时间一起保存，以区分 PID 复用。

磁盘指标使用英文 PDH Counter 路径读取 `PhysicalDisk(*)`。介质类型通过 `StorageDeviceSeekPenaltyProperty` 查询；查询失败时保留 `Unknown`，不会猜测介质类型。

## 保护优先于优化

ProtectionPolicy 的主要不变量：

- System/Security 永远不可修改。
- RemoteTerminal/SerialTerminal/PacketCapture/BuildTool/VersionControl 默认 Strong。
- 具有已建立受保护远程端口连接的 PID 至少为 Strong。
- dumpcap 存在时，抓包相关进程视为 Active Capture。
- 用户 `always_protect` 规则优先级最高。
- Unknown 默认 Strong，而不是优化候选。

OptimizationPlanner 只能创建强类型 Action。SafetyValidator 会重新查找快照中的 PID、校验启动时间、风险和目标优先级，再次调用 ProtectionPolicy。ActionExecutor 不接受任意命令文本。

## 会话与恢复

Coding Mode 按以下顺序执行：

1. 采集基线并生成计划。
2. 原子写入空的 Active Session。
3. 每个动作先以 Planned 状态和原始优先级恢复载荷写入磁盘。
4. 通过 SafetyValidator 后执行动作。
5. 写入 Applied/Rejected/Failed 状态和恢复载荷。
6. 退出时逆序回滚 Applied 动作。
7. 验证采样并写入 Before/After/Delta 报告。
8. 报告写入成功后才删除 Active Session。

若恢复或持久化失败，Active Session 保留并切换为 SafeMode，以阻止新的修改。
Planned 表示“系统调用可能尚未执行，也可能已执行但结果还未落盘”；恢复时会幂等回写原始优先级，因此两个状态都安全。

## 当前执行动作

当前仅实现 `SetPriorityClass`：

- 允许的自动目标为 Below Normal、Normal、Above Normal。
- High、Realtime、Idle 被 SafetyValidator 拒绝。
- 提升仅适用于 Profile 明确列出的 Development 进程。
- 降低仅适用于 Profile 明确列出的非保护进程。
- 回滚载荷包含原始 Priority Class。

Graceful Close、Terminate、服务和启动项枚举已保留在强类型模型中，但执行器拒绝这些尚未实现的动作。

## 隐私

TCP 报告默认脱敏远端地址。快照中的 image path 只用于当前进程视图，不写入长期诊断报告。WorkBoost 不读取网络、终端、串口或抓包内容。
