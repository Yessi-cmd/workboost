#include "core/config/config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>

namespace workboost {
namespace {

std::string ReadFile(const std::filesystem::path& path, std::string* error) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    if (error) *error = "cannot open " + path.string();
    return {};
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

std::optional<double> ReadNumber(const std::string& json,
                                 const std::string& key) {
  const std::regex pattern("\\\"" + key +
                           "\\\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)",
                           std::regex::icase);
  std::smatch match;
  if (!std::regex_search(json, match, pattern)) return std::nullopt;
  try {
    return std::stod(match[1].str());
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::string> DelimitedBody(const std::string& json,
                                         const std::string& key,
                                         char open,
                                         char close) {
  const std::string marker = "\"" + key + "\"";
  const auto key_position = json.find(marker);
  if (key_position == std::string::npos) return std::nullopt;
  const auto start = json.find(open, key_position + marker.size());
  if (start == std::string::npos) return std::nullopt;
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (std::size_t i = start; i < json.size(); ++i) {
    const char ch = json[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }
    if (ch == '"') {
      in_string = true;
    } else if (ch == open) {
      ++depth;
    } else if (ch == close && --depth == 0) {
      return json.substr(start + 1, i - start - 1);
    }
  }
  return std::nullopt;
}

std::vector<std::string> QuotedValues(const std::string& body) {
  std::vector<std::string> values;
  const std::regex value_pattern("\\\"([^\\\"]+)\\\"");
  for (auto it = std::sregex_iterator(body.begin(), body.end(), value_pattern);
       it != std::sregex_iterator(); ++it) {
    values.push_back((*it)[1].str());
  }
  return values;
}

void LoadStringSet(const std::string& json, const std::string& key,
                   std::unordered_set<std::string>* target) {
  const auto body = DelimitedBody(json, key, '[', ']');
  if (!body) return;
  target->clear();
  for (auto value : QuotedValues(*body)) {
    target->insert(ToLowerAscii(std::move(value)));
  }
}

void LoadPortSet(const std::string& json, const std::string& key,
                 std::unordered_set<std::uint16_t>* target) {
  const auto body = DelimitedBody(json, key, '[', ']');
  if (!body) return;
  const std::regex number_pattern("[0-9]+");
  std::unordered_set<std::uint16_t> parsed;
  for (auto it = std::sregex_iterator(body->begin(), body->end(), number_pattern);
       it != std::sregex_iterator(); ++it) {
    try {
      const auto value = std::stoul((*it).str());
      if (value > 0 && value <= 65535)
        parsed.insert(static_cast<std::uint16_t>(value));
    } catch (...) {
    }
  }
  if (!parsed.empty()) *target = std::move(parsed);
}

void LoadPriorityMap(const std::string& json, CodingProfile* profile) {
  const auto body = DelimitedBody(json, "foreground_priority", '{', '}');
  if (!body) return;
  const std::regex pair_pattern(
      "\\\"([^\\\"]+)\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
  profile->foreground_priority.clear();
  for (auto it = std::sregex_iterator(body->begin(), body->end(), pair_pattern);
       it != std::sregex_iterator(); ++it) {
    profile->foreground_priority[ToLowerAscii((*it)[1].str())] =
        ToLowerAscii((*it)[2].str());
  }
}

std::optional<std::string> StringField(const std::string& object,
                                       const std::string& key) {
  const std::regex pattern("\\\"" + key +
                           "\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"",
                           std::regex::icase);
  std::smatch match;
  if (!std::regex_search(object, match, pattern)) return std::nullopt;
  return match[1].str();
}

void LoadProcessRules(const std::string& json, Config* config) {
  const auto body = DelimitedBody(json, "rules", '[', ']');
  if (!body) return;
  const std::regex object_pattern("\\{([^{}]+)\\}");
  for (auto it = std::sregex_iterator(body->begin(), body->end(), object_pattern);
       it != std::sregex_iterator(); ++it) {
    const std::string object = (*it)[1].str();
    const auto name = StringField(object, "name");
    const auto process_class = StringField(object, "class");
    const auto protection = StringField(object, "protection");
    if (!name || !process_class || !protection) continue;
    config->process_rules[ToLowerAscii(*name)] = {
        ProcessClassFromString(*process_class),
        ProtectionLevelFromString(*protection)};
  }
}

void AddRule(Config* config, const char* name, ProcessClass process_class,
             ProtectionLevel protection) {
  config->process_rules.emplace(ToLowerAscii(name),
                                ProcessRule{process_class, protection});
}

}  // namespace

std::string ToLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

Config Config::Defaults() {
  Config config;

  for (const char* name : {"system", "registry", "smss.exe", "csrss.exe",
                           "wininit.exe", "services.exe", "lsass.exe",
                           "svchost.exe", "winlogon.exe", "dwm.exe"}) {
    AddRule(&config, name, ProcessClass::System,
            ProtectionLevel::SystemCritical);
  }
  for (const char* name : {"msmpeng.exe", "nissrv.exe", "senseir.exe",
                           "sensece.exe", "securityhealthservice.exe"}) {
    AddRule(&config, name, ProcessClass::Security,
            ProtectionLevel::SystemCritical);
  }
  for (const char* name : {"codex.exe", "code.exe", "devenv.exe",
                           "clion64.exe", "idea64.exe"}) {
    AddRule(&config, name, ProcessClass::Development, ProtectionLevel::Strong);
  }
  for (const char* name : {"ssh.exe", "telnet.exe", "putty.exe", "xshell.exe",
                           "mobaxterm.exe", "windowsterminal.exe"}) {
    AddRule(&config, name, ProcessClass::RemoteTerminal,
            ProtectionLevel::Strong);
  }
  for (const char* name : {"securecrt.exe", "teraterm.exe", "puttytel.exe"}) {
    AddRule(&config, name, ProcessClass::SerialTerminal,
            ProtectionLevel::Strong);
  }
  for (const char* name : {"wireshark.exe", "dumpcap.exe", "tshark.exe"}) {
    AddRule(&config, name, ProcessClass::PacketCapture,
            ProtectionLevel::Strong);
  }
  for (const char* name : {"git.exe", "scp.exe", "sftp.exe",
                           "git-remote-http.exe", "git-remote-https.exe"}) {
    AddRule(&config, name, ProcessClass::VersionControl,
            ProtectionLevel::Strong);
  }
  for (const char* name : {"cmake.exe", "ninja.exe", "make.exe", "msbuild.exe",
                           "cl.exe", "clang.exe", "gcc.exe", "g++.exe",
                           "python.exe", "python3.exe", "bash.exe", "wsl.exe"}) {
    AddRule(&config, name, ProcessClass::BuildTool, ProtectionLevel::Strong);
  }
  for (const char* name : {"chrome.exe", "msedge.exe", "firefox.exe"}) {
    AddRule(&config, name, ProcessClass::Browser, ProtectionLevel::Normal);
  }
  for (const char* name : {"teams.exe", "ms-teams.exe", "wxwork.exe",
                           "wechat.exe"}) {
    AddRule(&config, name, ProcessClass::Communication, ProtectionLevel::Normal);
  }
  for (const char* name : {"onedrive.exe", "dropbox.exe"}) {
    AddRule(&config, name, ProcessClass::CloudSync,
            ProtectionLevel::Optimizable);
  }
  for (const char* name : {"searchindexer.exe"}) {
    AddRule(&config, name, ProcessClass::System,
            ProtectionLevel::SystemCritical);
  }

  config.coding_profile.foreground_priority = {
      {"codex.exe", "above_normal"}, {"code.exe", "above_normal"}};
  for (const char* name : {"ssh.exe", "telnet.exe", "putty.exe", "xshell.exe",
                           "securecrt.exe", "wireshark.exe", "dumpcap.exe",
                           "tshark.exe", "git.exe"}) {
    config.coding_profile.always_protect.insert(name);
  }
  return config;
}

bool Config::LoadDirectory(const std::filesystem::path& directory,
                           std::string* warning) {
  std::vector<std::string> warnings;
  const auto diagnosis_path = directory / "diagnosis.json";
  if (std::filesystem::exists(diagnosis_path)) {
    std::string error;
    const std::string json = ReadFile(diagnosis_path, &error);
    if (json.empty() && !error.empty()) warnings.push_back(error);
    if (const auto value = ReadNumber(json, "sample_interval_ms"))
      sample_interval_ms = std::max(100, static_cast<int>(*value));
    if (const auto value = ReadNumber(json, "history_seconds"))
      history_seconds = std::max(1, static_cast<int>(*value));
    if (const auto value = ReadNumber(json, "commit_warning"))
      thresholds.commit_warning = *value;
    if (const auto value = ReadNumber(json, "available_memory_mb"))
      thresholds.available_memory_mb = *value;
    if (const auto value = ReadNumber(json, "page_reads_per_sec"))
      thresholds.page_reads_per_sec = *value;
    if (const auto value = ReadNumber(json, "disk_active_ratio"))
      thresholds.disk_active_ratio = *value;
    if (const auto value = ReadNumber(json, "hdd_latency_ms"))
      thresholds.hdd_latency_ms = *value;
    if (const auto value = ReadNumber(json, "cpu_saturation_ratio"))
      thresholds.cpu_saturation_ratio = *value;
    if (const auto value = ReadNumber(json, "background_io_bytes_per_sec"))
      thresholds.background_io_bytes_per_sec = *value;
    if (const auto value = ReadNumber(json, "defender_cpu_ratio"))
      thresholds.defender_cpu_ratio = *value;
    if (const auto value = ReadNumber(json, "defender_io_ratio"))
      thresholds.defender_io_ratio = *value;
    LoadPortSet(json, "remote_debug_ports", &remote_debug_ports);
  }

  const auto profiles_path = directory / "profiles.json";
  if (std::filesystem::exists(profiles_path)) {
    std::string error;
    const std::string json = ReadFile(profiles_path, &error);
    if (json.empty() && !error.empty()) warnings.push_back(error);
    LoadPriorityMap(json, &coding_profile);
    LoadStringSet(json, "always_protect", &coding_profile.always_protect);
    LoadStringSet(json, "allow_graceful_close",
                  &coding_profile.allow_graceful_close);
    LoadStringSet(json, "allow_priority_down",
                  &coding_profile.allow_priority_down);
  }

  const auto rules_path = directory / "process_rules.json";
  if (std::filesystem::exists(rules_path)) {
    std::string error;
    const std::string json = ReadFile(rules_path, &error);
    if (json.empty() && !error.empty()) warnings.push_back(error);
    LoadProcessRules(json, this);
  }

  if (warning) {
    std::ostringstream output;
    for (std::size_t i = 0; i < warnings.size(); ++i) {
      if (i != 0) output << "; ";
      output << warnings[i];
    }
    *warning = output.str();
  }
  return warnings.empty();
}

ProcessRule Config::RuleFor(const std::string& process_name) const {
  const std::string lower_name = ToLowerAscii(process_name);
  static const std::unordered_set<std::string> kImmutableSystemCritical{
      "system",      "registry",    "smss.exe",    "csrss.exe",
      "wininit.exe", "services.exe", "lsass.exe",   "svchost.exe",
      "winlogon.exe", "dwm.exe",     "searchindexer.exe"};
  static const std::unordered_set<std::string> kImmutableSecurityCritical{
      "msmpeng.exe", "nissrv.exe", "senseir.exe", "sensece.exe",
      "securityhealthservice.exe"};
  if (kImmutableSystemCritical.count(lower_name) != 0)
    return {ProcessClass::System, ProtectionLevel::SystemCritical};
  if (kImmutableSecurityCritical.count(lower_name) != 0)
    return {ProcessClass::Security, ProtectionLevel::SystemCritical};

  const auto it = process_rules.find(lower_name);
  if (it != process_rules.end()) return it->second;
  // Unknown development tools are protected by default, never optimized.
  return {ProcessClass::Unknown, ProtectionLevel::Strong};
}

bool Config::IsAlwaysProtected(const std::string& process_name) const {
  return coding_profile.always_protect.count(ToLowerAscii(process_name)) != 0;
}

}  // namespace workboost
