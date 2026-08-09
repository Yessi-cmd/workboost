# Testing

单元测试覆盖：

- Commit Ratio 归一化及除零。
- 进程规则大小写无关匹配。
- Unknown 默认 Strong Protection。
- TCP 22 Established Session 到 PID 的保护映射。
- dumpcap Active Capture 保护。
- SSH 优先级降低被 SafetyValidator 拒绝。
- 显式配置的 IDE 优先级提升被允许。
- Memory/Paging/Disk/HDD Paging/CPU/Defender 诊断规则。
- Session 和原始优先级回滚载荷序列化往返。

构建后运行：

```powershell
ctest --test-dir build -C Release --output-on-failure
```

目标机手动验收建议：

1. 保持一个 SSH 或 Telnet 连接，确认 `connections` 中 PID 映射正确。
2. 确认相同进程在 `protected` 中为 Strong 或 UserExplicit。
3. 启动 Wireshark 抓包，确认 Wireshark/dumpcap 均受保护。
4. 执行 `coding enter --dry-run`，确认计划不包含远程终端、串口、抓包、Git 或构建进程。
5. 进入并退出 Coding Mode，确认报告中动作已回滚。
6. 在测试环境中于动作后强制结束 WorkBoost，再运行 `recovery status` 和 `recovery restore`。
7. 用受控负载复现内存分页和 HDD 高延迟，验证时间窗口诊断，而不是单点误报。
