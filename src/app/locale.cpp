#include "app/locale.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace workboost {
namespace {

LocaleId& CurrentLocaleRef() {
  static LocaleId current = LocaleId::English;
  return current;
}

// English key -> simplified Chinese. Keys are the English strings used as
// literals throughout the GUI, so untranslated strings stay English.
const std::pair<const char*, const char*> kChineseTranslations[] = {
        // Pages and navigation.
        {"Dashboard", "仪表盘"},
        {"Processes", "进程"},
        {"Diagnosis", "诊断"},
        {"Coding Mode", "编码模式"},
        {"Protected Workload", "受保护负载"},
        {"Recovery & History", "恢复与历史"},
        {"Settings", "设置"},
        {"Developer performance", "开发者性能"},

        // Mode pill and window status.
        {"Safe Mode", "安全模式"},
        {"Monitor Mode", "监控模式"},
        {"Monitor unavailable", "监控不可用"},
        {"Initializing", "初始化中"},
        {"Recovery required", "需要恢复"},
        {"Recovery Required", "需要恢复"},
        {"Coding Mode Active", "编码模式运行中"},
        {"Collecting system metrics...", "正在采集系统指标…"},
        {"Refreshing...", "刷新中…"},

        // Header buttons.
        {"Refresh", "刷新"},
        {"Export", "导出"},

        // Dashboard page.
        {"System Overview", "系统概览"},
        {"Top Impact", "高影响进程"},
        {"CPU", "CPU"},
        {"Memory", "内存"},
        {"Impact", "影响"},
        {"Commit", "提交内存"},
        {"{0} available", "{0} 可用"},
        {"{0} page reads/s", "{0} 页读取/秒"},
        {"No physical disk counters available", "无物理磁盘计数器"},
        {"No process inventory available", "无进程清单"},
        {"No active developer workload detected", "未检测到活跃的开发负载"},

        // Diagnosis page.
        {"No significant bottleneck detected.", "未检测到显著瓶颈。"},
        {"Diagnosis uses a sustained time window; isolated spikes do not "
         "become conclusions.",
         "诊断基于持续的时间窗口；孤立的尖峰不会成为结论。"},
        {"Memory Pressure", "内存压力"},
        {"Paging Pressure", "分页压力"},
        {"Disk I/O Bottleneck", "磁盘 I/O 瓶颈"},
        {"HDD Paging Bottleneck", "HDD 分页瓶颈"},
        {"SSD Space Pressure", "SSD 空间压力"},
        {"CPU Saturation", "CPU 饱和"},
        {"Windows Defender Impact", "Windows Defender 影响"},
        {"Background I/O Impact", "后台 I/O 影响"},
        {"Foreground App Memory Pressure", "前台应用内存压力"},

        // Severity and impact display text (logic keeps English values).
        {"high", "高"},
        {"medium", "中"},
        {"low", "低"},
        {"High", "高"},
        {"Medium", "中"},
        {"Low", "低"},
        {"Critical", "严重"},
        {"Safe", "安全"},

        // Protected workload page.
        {"WorkBoost will not modify these active development tasks.",
         "WorkBoost 不会改动这些活跃的开发任务。"},
        {"Protected because: ", "保护原因："},
        {"No active remote, serial, capture, build, Git, or development "
         "workload detected.",
         "未检测到活跃的远程、串口、抓包、构建、Git 或开发负载。"},

        // Coding Mode page.
        {"Reduce unnecessary background activity while keeping SSH, serial, "
         "capture, Git, and build work protected.",
         "降低不必要的后台活动，同时保护 SSH、串口、抓包、Git 与构建工作。"},
        {"{0} planned changes", "{0} 项计划变更"},
        {"{0} cleanup selections", "{0} 个清理池进程"},
        {"{0} protected workloads", "{0} 项受保护负载"},
        {"Working...", "处理中…"},
        {"View Recovery", "查看恢复"},
        {"Exit and Restore", "退出并恢复"},
        {"Enter Coding Mode", "进入编码模式"},
        {"Active Changes", "活动变更"},
        {"Plan Preview", "计划预览"},
        {"Capturing a 10-second baseline, then applying the reviewed plan...",
         "正在采集 10 秒基线，然后应用审核后的计划…"},
        {"Restoring reversible actions and verifying system state...",
         "正在恢复可逆操作并验证系统状态…"},
        {"Restoring the unfinished session and verifying system state...",
         "正在恢复未完成的会话并验证系统状态…"},
        {"Retrying graceful close for unclosed processes...",
         "正在对退出后仍运行的进程重新发送关闭请求…"},
        {"Cleanup retry completed.", "清理重试已完成。"},
        {"Cleanup retry completed. Revalidation, session persistence, and "
         "reporting used the standard Coding Mode path.",
         "清理重试已完成。重新校验、会话持久化与报告均使用标准编码模式链路。"},
        {"Cleanup Retry", "清理重试"},
        {"Retry Cleanup", "重试清理"},
        {"Cleanup processes kept running.", "清理进程保持运行。"},
        {"{0} cleanup process(es) are still running after exit:\r\n{1}\r\n"
         "Retry graceful close now? Choosing No keeps them running.",
         "退出后仍有 {0} 个清理进程在运行：\r\n{1}\r\n"
         "是否立即重新发送关闭请求？选择“否”则保持这些进程运行。"},
        {"Apply the reviewed Coding Mode plan?\n\n{0} planned action(s)\n{1} "
         "cleanup process(es)\n{2} protected workload(s)\n\nA 10-second "
         "baseline is captured first. "
         "All system changes still pass ProtectionPolicy and SafetyValidator.",
         "是否应用审核后的编码模式计划？\n\n{0} 项计划操作\n{1} 个清理池进程"
         "\n{2} 项受保护负载"
         "\n\n将先采集 10 秒基线。所有系统变更仍会通过保护策略与安全校验。"},
        {"Current Processes", "当前进程"},
        {"Cleanup Pool", "清理池"},
        {"Automatic Plan", "自动计划"},
        {"No automatic actions.", "没有自动操作。"},
        {"Priority", "优先级"},
        {"Close application", "关闭应用"},
        {"Temporary service stop", "临时停止服务"},
        {"Click an available process to add it to the cleanup pool.",
         "单击可用进程，将其加入清理池。"},
        {"Click a selected process to remove it.",
         "单击已选进程可将其移出。"},
        {"State", "状态"},
        {"Selected", "已选择"},
        {"Ready to add", "可加入"},
        {"Protection inventory is incomplete", "保护清单不完整"},
        {"Process identity is unavailable", "进程身份不可用"},
        {"Protected by policy", "受策略保护"},
        {"Foreground process", "前台进程"},
        {"No visible window", "没有可见窗口"},
        {"Already included in the plan", "已包含在计划中"},
        {"No process inventory is available.", "没有可用的进程清单。"},
        {"Showing {0}-{1} of {2}; use the mouse wheel.",
         "正在显示第 {0}-{1} 个，共 {2} 个；请使用鼠标滚轮。"},
        {"No processes selected.", "尚未选择进程。"},
        {"{0} selected; additional entries are below.",
         "已选择 {0} 个；其余条目在下方。"},
        {"Process is no longer available.", "该进程已不可用。"},
        {"Removed from the cleanup pool.", "已移出清理池。"},
        {"Cleanup pool is full.", "清理池已满。"},
        {"Added to the cleanup pool.", "已加入清理池。"},

        // Coding Mode status and dialogs.
        {"Coding Mode is active.", "编码模式已激活。"},
        {"System state was restored successfully.", "系统状态已成功恢复。"},
        {"Could not start the Coding Mode operation.", "无法启动编码模式操作。"},
        {"Coding Mode operation failed; no result was hidden.",
         "编码模式操作失败；结果未隐藏。"},
        {"Coding Mode failed", "编码模式失败"},
        {"Coding Mode active", "编码模式已激活"},
        {"Recovery complete", "恢复完成"},
        {"Exit Coding Mode", "退出编码模式"},
        {"A Coding Mode operation is still running. Complete the operation "
         "before closing WorkBoost.",
         "编码模式操作仍在进行中。请在关闭 WorkBoost 前完成该操作。"},
        {"Exit Coding Mode and restore every reversible action in reverse "
         "order?\n\nWorkBoost will verify the restored state and keep Safe "
         "Mode active if restoration cannot be confirmed.",
         "是否退出编码模式并按相反顺序恢复所有可逆操作？\n\nWorkBoost 将验证"
         "恢复后的状态；若无法确认恢复，将保持安全模式。"},

        // Recovery page.
        {"An unfinished Coding Mode session must be restored before new "
         "changes are allowed.",
         "必须先恢复未完成的编码模式会话，之后才允许新的变更。"},
        {"No unfinished session. Completed sessions are listed below.",
         "没有未完成的会话。已完成的会话列在下方。"},
        {"State: ", "状态："},
        {"{0} recorded action(s)", "{0} 条已记录操作"},
        {"Restore Session", "恢复会话"},
        {"No Action Needed", "无需操作"},
        {"Session Details", "会话详情"},
        {"Completion History", "完成历史"},

        // Settings page.
        {"Effective configuration is read-only in this release; edit the "
         "config files beside the executable to adjust it.",
         "当前版本的生效配置为只读；修改可执行文件旁的配置文件以调整。"},
        {"{0} ms sampling", "{0} 毫秒采样"},
        {"{0} s history", "{0} 秒历史"},
        {"{0} process rules", "{0} 条进程规则"},
        {"{0} service rules", "{0} 条服务规则"},
        {"Startup Inventory & Configuration", "启动清单与配置"},
        {"Language", "语言"},

        // Processes page.
        {"All", "全部"},
        {"High Impact", "高影响"},
        {"Protected", "受保护"},
        {"Search processes...", "搜索进程…"},
        {"Name", "名称"},
        {"Disk I/O", "磁盘 I/O"},
        {"Impact / Status", "影响 / 状态"},
        {"No process matches the current search and filter.",
         "没有进程匹配当前的搜索与筛选。"},
        {"Showing {0} of {1} matching processes...",
         "正在显示 {0} / {1} 个匹配进程…"},
        {"· Protected", "· 受保护"},
        {"· Normal", "· 正常"},
        {"Private", "专用"},

        // Tray and export.
        {"Open WorkBoost", "打开 WorkBoost"},
        {"Exit", "退出"},
        {"WorkBoost is still running. Click the tray icon to open the "
         "dashboard.",
         "WorkBoost 仍在运行。单击托盘图标打开仪表盘。"},
        {"Wait for the first sample before exporting.",
         "请等待首次采样后再导出。"},
        {"Dashboard report exported.", "仪表盘报告已导出。"},
        {"Export failed", "导出失败"},
        {"WorkBoost Dashboard", "WorkBoost 仪表盘"},
        {"Generated: ", "生成时间："},
        {"Mode: ", "模式："},

        // Model-derived classification and protection labels.
        {"System", "系统"},
        {"Security", "安全"},
        {"Development", "开发"},
        {"RemoteTerminal", "远程终端"},
        {"SerialTerminal", "串口终端"},
        {"PacketCapture", "抓包"},
        {"BuildTool", "构建"},
        {"VersionControl", "版本控制"},
        {"Browser", "浏览器"},
        {"Communication", "通讯"},
        {"Office", "办公"},
        {"Updater", "更新器"},
        {"CloudSync", "云同步"},
        {"VendorUtility", "厂商工具"},
        {"Unknown", "未知"},
        {"SystemCritical", "系统关键"},
        {"Strong", "强"},
        {"Normal", "普通"},
        {"Optimizable", "可优化"},
        {"UserExplicit", "用户指定"},
        {"Remote", "远程"},
        {"Serial", "串口"},
        {"Packet capture", "抓包"},
        {"Build", "构建"},
        {"Version control", "版本控制"},
        {"Active protected remote session", "活跃的受保护远程会话"},
        {"Packet capture is running", "抓包正在进行"},
        {"Always Protect profile", "始终保护配置"},
        {"Known remote terminal", "已知远程终端"},
        {"Known serial terminal", "已知串口终端"},
        {"Known packet-capture tool", "已知抓包工具"},
        {"Development workload", "开发负载"},
        {"Build task", "构建任务"},
        {"Version-control task", "版本控制任务"},
        {"Fail-closed protection policy", "默认拒绝的保护策略"},

        // Processes page and coding-mode details.
        {"Read", "读取"},
        {"Write", "写入"},
        {" · Protected", " · 受保护"},
        {" · Normal", " · 正常"},
        {"The first sample normally takes about one second.",
         "首次采样通常约需一秒钟。"},
        {"Showing {0} of {1} matching processes. Refine the search to "
         "narrow the list.",
         "正在显示 {0} / {1} 个匹配进程。请优化搜索以缩小列表。"},
        {"WorkBoost did not complete the requested operation.",
         "WorkBoost 未能完成所请求的操作。"},
        {"The reversible session was restored and verified.",
         "可逆会话已恢复并验证。"},
        {"Coding Mode is active. WorkBoost recorded every applied action for "
         "deterministic rollback.",
         "编码模式已激活。WorkBoost 已记录每个已应用的操作，以便确定性回滚。"},
    };

const std::unordered_map<std::string_view, const char*>& ChineseTable() {
  static const std::unordered_map<std::string_view, const char*> table = [] {
    std::unordered_map<std::string_view, const char*> map;
    map.reserve(std::size(kChineseTranslations));
    for (const auto& [english, chinese] : kChineseTranslations) {
      map.emplace(english, chinese);
    }
    return map;
  }();
  return table;
}

}  // namespace

LocaleId Locale::Current() { return CurrentLocaleRef(); }

void Locale::Set(LocaleId id) { CurrentLocaleRef() = id; }

bool Locale::IsChinese() { return Current() == LocaleId::Chinese; }

std::string Locale::Get(const char* key) {
  if (Current() == LocaleId::English) return key;
  const auto& table = ChineseTable();
  const auto it = table.find(std::string_view(key));
  return it == table.end() ? std::string(key) : std::string(it->second);
}

std::string Locale::Get(const std::string& key) { return Get(key.c_str()); }

std::string Locale::Format(const char* key,
                           std::initializer_list<std::string> args) {
  const std::string template_text = Get(key);
  const std::vector<std::string> values(args);
  std::string result;
  result.reserve(template_text.size() + 32);
  for (std::size_t i = 0; i < template_text.size(); ++i) {
    if (template_text[i] == '{' && i + 1 < template_text.size() &&
        template_text[i + 1] >= '0' && template_text[i + 1] <= '9') {
      std::size_t end = i + 1;
      while (end < template_text.size() && template_text[end] >= '0' &&
             template_text[end] <= '9') {
        ++end;
      }
      if (end < template_text.size() && template_text[end] == '}') {
        const std::size_t index =
            static_cast<std::size_t>(std::strtoul(
                template_text.c_str() + i + 1, nullptr, 10));
        if (index < values.size()) {
          result += values[index];
          i = end;
          continue;
        }
        // Unknown index: keep the placeholder verbatim.
        result.append(template_text, i, end - i + 1);
        i = end;
        continue;
      }
    }
    result += template_text[i];
  }
  return result;
}

}  // namespace workboost
