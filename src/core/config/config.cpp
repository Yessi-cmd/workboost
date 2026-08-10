#include "core/config/config.h"

#include "platform/windows/windows_utils.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <string_view>

namespace workboost {
namespace {

constexpr std::uintmax_t kMaximumConfigBytes = 1024 * 1024;

std::string ReadFile(const std::filesystem::path& path, std::string* error) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    if (error) *error = "cannot open " + path.string();
    return {};
  }
  std::error_code size_error;
  const auto size = std::filesystem::file_size(path, size_error);
  if (size_error || size > kMaximumConfigBytes) {
    if (error) {
      *error = size_error
                   ? "cannot size " + path.string() + ": " +
                         size_error.message()
                   : "config file exceeds 1 MiB limit: " + path.string();
    }
    return {};
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

class JsonSyntaxValidator {
 public:
  explicit JsonSyntaxValidator(const std::string& input) : input_(input) {}

  bool ValidateObject() {
    if (input_.size() >= 3 &&
        static_cast<unsigned char>(input_[0]) == 0xef &&
        static_cast<unsigned char>(input_[1]) == 0xbb &&
        static_cast<unsigned char>(input_[2]) == 0xbf) {
      position_ = 3;
    }
    SkipWhitespace();
    if (position_ >= input_.size() || input_[position_] != '{' ||
        !ParseObject(0)) {
      return false;
    }
    SkipWhitespace();
    return position_ == input_.size();
  }

 private:
  void SkipWhitespace() {
    while (position_ < input_.size() &&
           (input_[position_] == ' ' || input_[position_] == '\t' ||
            input_[position_] == '\r' || input_[position_] == '\n')) {
      ++position_;
    }
  }

  bool Consume(char value) {
    SkipWhitespace();
    if (position_ >= input_.size() || input_[position_] != value) return false;
    ++position_;
    return true;
  }

  bool ParseString() {
    if (position_ >= input_.size() || input_[position_++] != '"') return false;
    while (position_ < input_.size()) {
      const unsigned char value =
          static_cast<unsigned char>(input_[position_++]);
      if (value == '"') return true;
      if (value < 0x20) return false;
      if (value != '\\') continue;
      if (position_ >= input_.size()) return false;
      const char escape = input_[position_++];
      if (escape == '"' || escape == '\\' || escape == '/' ||
          escape == 'b' || escape == 'f' || escape == 'n' || escape == 'r' ||
          escape == 't') {
        continue;
      }
      if (escape != 'u' || position_ + 4 > input_.size()) return false;
      for (int digit = 0; digit < 4; ++digit) {
        const char hexadecimal = input_[position_++];
        if (!((hexadecimal >= '0' && hexadecimal <= '9') ||
              (hexadecimal >= 'a' && hexadecimal <= 'f') ||
              (hexadecimal >= 'A' && hexadecimal <= 'F'))) {
          return false;
        }
      }
    }
    return false;
  }

  bool ParseNumber() {
    if (position_ < input_.size() && input_[position_] == '-') ++position_;
    if (position_ >= input_.size()) return false;
    if (input_[position_] == '0') {
      ++position_;
      if (position_ < input_.size() && input_[position_] >= '0' &&
          input_[position_] <= '9') {
        return false;
      }
    } else if (input_[position_] >= '1' && input_[position_] <= '9') {
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
    } else {
      return false;
    }
    if (position_ < input_.size() && input_[position_] == '.') {
      ++position_;
      const std::size_t fraction = position_;
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
      if (fraction == position_) return false;
    }
    if (position_ < input_.size() &&
        (input_[position_] == 'e' || input_[position_] == 'E')) {
      ++position_;
      if (position_ < input_.size() &&
          (input_[position_] == '+' || input_[position_] == '-')) {
        ++position_;
      }
      const std::size_t exponent = position_;
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
      if (exponent == position_) return false;
    }
    return true;
  }

  bool ParseLiteral(std::string_view literal) {
    if (input_.compare(position_, literal.size(), literal) != 0) return false;
    position_ += literal.size();
    return true;
  }

  bool ParseArray(int depth) {
    if (depth > 64 || !Consume('[')) return false;
    SkipWhitespace();
    if (position_ < input_.size() && input_[position_] == ']') {
      ++position_;
      return true;
    }
    for (;;) {
      if (!ParseValue(depth + 1)) return false;
      SkipWhitespace();
      if (position_ < input_.size() && input_[position_] == ']') {
        ++position_;
        return true;
      }
      if (!Consume(',')) return false;
    }
  }

  bool ParseObject(int depth) {
    if (depth > 64 || !Consume('{')) return false;
    SkipWhitespace();
    if (position_ < input_.size() && input_[position_] == '}') {
      ++position_;
      return true;
    }
    std::unordered_set<std::string> keys;
    for (;;) {
      SkipWhitespace();
      const std::size_t key_begin = position_;
      if (!ParseString()) return false;
      const std::string key =
          input_.substr(key_begin, position_ - key_begin);
      if (!keys.insert(key).second || !Consume(':') ||
          !ParseValue(depth + 1)) {
        return false;
      }
      SkipWhitespace();
      if (position_ < input_.size() && input_[position_] == '}') {
        ++position_;
        return true;
      }
      if (!Consume(',')) return false;
    }
  }

  bool ParseValue(int depth) {
    if (depth > 64) return false;
    SkipWhitespace();
    if (position_ >= input_.size()) return false;
    switch (input_[position_]) {
      case '{': return ParseObject(depth);
      case '[': return ParseArray(depth);
      case '"': return ParseString();
      case 't': return ParseLiteral("true");
      case 'f': return ParseLiteral("false");
      case 'n': return ParseLiteral("null");
      default: return ParseNumber();
    }
  }

  const std::string& input_;
  std::size_t position_{};
};

std::optional<std::string> ReadConfig(
    const std::filesystem::path& path, std::vector<std::string>* warnings) {
  std::error_code exists_error;
  const bool exists = std::filesystem::exists(path, exists_error);
  if (exists_error) {
    warnings->push_back("cannot inspect " + path.string() + ": " +
                        exists_error.message());
    return std::nullopt;
  }
  if (!exists) return std::nullopt;
  std::string error;
  std::string json = ReadFile(path, &error);
  if (!error.empty()) {
    warnings->push_back(error);
    return std::nullopt;
  }
  if (!JsonSyntaxValidator(json).ValidateObject()) {
    warnings->push_back("invalid JSON in " + path.string());
    return std::nullopt;
  }
  return json;
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

std::optional<std::string> ReadString(const std::string& json,
                                      const std::string& key) {
  const std::regex pattern("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"",
                           std::regex::icase);
  std::smatch match;
  if (!std::regex_search(json, match, pattern)) return std::nullopt;
  return match[1].str();
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
  std::unordered_set<std::uint16_t> parsed{22, 23};
  for (auto it = std::sregex_iterator(body->begin(), body->end(), number_pattern);
       it != std::sregex_iterator(); ++it) {
    try {
      const auto value = std::stoul((*it).str());
      if (value > 0 && value <= 65535)
        parsed.insert(static_cast<std::uint16_t>(value));
    } catch (...) {
    }
  }
  *target = std::move(parsed);
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

void LoadServiceRules(const std::string& json, Config* config) {
  const auto body = DelimitedBody(json, "rules", '[', ']');
  if (!body) return;
  const std::regex object_pattern("\\{([^{}]+)\\}");
  for (auto it = std::sregex_iterator(body->begin(), body->end(), object_pattern);
       it != std::sregex_iterator(); ++it) {
    const std::string object = (*it)[1].str();
    const auto name = StringField(object, "name");
    const auto service_class = StringField(object, "class");
    const auto protection = StringField(object, "protection");
    if (!name || !service_class || !protection) continue;
    config->service_rules[ToLowerAscii(*name)] = {
        ServiceClassFromString(*service_class),
        ProtectionLevelFromString(*protection)};
  }
}

void AddRule(Config* config, const char* name, ProcessClass process_class,
             ProtectionLevel protection) {
  config->process_rules.emplace(ToLowerAscii(name),
                                ProcessRule{process_class, protection});
}

}  // namespace

bool IsValidJsonObjectSyntax(const std::string& json) {
  return JsonSyntaxValidator(json).ValidateObject();
}

std::string ToLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

Config Config::Defaults() {
  Config config;
  config.language = windows::DetectUILanguage();

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
  for (const char* name : {"securecrt.exe", "ttermpro.exe", "teraterm.exe",
                           "puttytel.exe"}) {
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
  for (const char* name : {"yourphone.exe", "phoneexperiencehost.exe",
                           "widgets.exe", "gamebar.exe",
                           "gamebarftserver.exe",
                           "xboxgamebarwidgets.exe", "systemsettings.exe",
                           "calculatorapp.exe", "notepad.exe", "mspaint.exe",
                           "snippingtool.exe", "microsoft.photos.exe"}) {
    AddRule(&config, name, ProcessClass::VendorUtility,
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
  if (const auto content = ReadConfig(diagnosis_path, &warnings)) {
    const std::string& json = *content;
    if (const auto value = ReadNumber(json, "sample_interval_ms")) {
      sample_interval_ms =
          static_cast<int>(std::clamp(*value, 100.0, 60000.0));
    }
    if (const auto value = ReadNumber(json, "history_seconds")) {
      history_seconds =
          static_cast<int>(std::clamp(*value, 1.0, 3600.0));
    }
    if (const auto value = ReadNumber(json, "commit_warning"))
      thresholds.commit_warning = std::clamp(*value, 0.01, 1.0);
    if (const auto value = ReadNumber(json, "available_memory_mb"))
      thresholds.available_memory_mb =
          std::clamp(*value, 64.0, 1048576.0);
    if (const auto value = ReadNumber(json, "page_reads_per_sec"))
      thresholds.page_reads_per_sec = std::clamp(*value, 0.0, 1.0e9);
    if (const auto value = ReadNumber(json, "disk_active_ratio"))
      thresholds.disk_active_ratio = std::clamp(*value, 0.01, 1.0);
    if (const auto value = ReadNumber(json, "hdd_latency_ms"))
      thresholds.hdd_latency_ms = std::clamp(*value, 0.1, 60000.0);
    if (const auto value = ReadNumber(json, "cpu_saturation_ratio"))
      thresholds.cpu_saturation_ratio = std::clamp(*value, 0.01, 1.0);
    if (const auto value = ReadNumber(json, "background_io_bytes_per_sec"))
      thresholds.background_io_bytes_per_sec =
          std::clamp(*value, 0.0, 1.0e15);
    if (const auto value = ReadNumber(json, "defender_cpu_ratio"))
      thresholds.defender_cpu_ratio = std::clamp(*value, 0.0, 1.0);
    if (const auto value = ReadNumber(json, "defender_io_ratio"))
      thresholds.defender_io_ratio = std::clamp(*value, 0.0, 1.0);
    if (const auto value = ReadNumber(json, "ssd_free_space_ratio"))
      thresholds.ssd_free_space_ratio = std::clamp(*value, 0.0, 1.0);
    LoadPortSet(json, "remote_debug_ports", &remote_debug_ports);
  }

  const auto profiles_path = directory / "profiles.json";
  if (const auto content = ReadConfig(profiles_path, &warnings)) {
    const std::string& json = *content;
    LoadPriorityMap(json, &coding_profile);
    LoadStringSet(json, "always_protect", &coding_profile.always_protect);
    LoadStringSet(json, "allow_graceful_close",
                  &coding_profile.allow_graceful_close);
    LoadStringSet(json, "allow_priority_down",
                  &coding_profile.allow_priority_down);
    LoadStringSet(json, "always_protect_services",
                  &coding_profile.always_protect_services);
    LoadStringSet(json, "allow_service_stop",
                  &coding_profile.allow_service_stop);
    if (const auto value = ReadNumber(json, "graceful_close_batch_budget_ms")) {
      coding_profile.graceful_close_batch_budget_ms =
          static_cast<std::uint32_t>(
              std::clamp(*value, 1000.0, 30000.0));
    }
  }

  const auto rules_path = directory / "process_rules.json";
  if (const auto content = ReadConfig(rules_path, &warnings)) {
    LoadProcessRules(*content, this);
  }

  const auto service_rules_path = directory / "service_rules.json";
  if (const auto content = ReadConfig(service_rules_path, &warnings)) {
    LoadServiceRules(*content, this);
  }

  const auto settings_path = directory / "settings.json";
  if (const auto content = ReadConfig(settings_path, &warnings)) {
    if (const auto value = ReadString(*content, "language")) {
      const std::string normalized = ToLowerAscii(*value);
      if (normalized == "zh" || normalized == "cn" || normalized == "zh-cn" ||
          normalized == "zh_cn" || normalized == "chinese") {
        language = "zh";
      } else if (normalized == "en" || normalized == "english") {
        language = "en";
      }
    }
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
  static const std::unordered_map<std::string, ProcessClass>
      kImmutableProtectedWorkloads{
          {"codex.exe", ProcessClass::Development},
          {"code.exe", ProcessClass::Development},
          {"devenv.exe", ProcessClass::Development},
          {"clion64.exe", ProcessClass::Development},
          {"idea64.exe", ProcessClass::Development},
          {"ssh.exe", ProcessClass::RemoteTerminal},
          {"telnet.exe", ProcessClass::RemoteTerminal},
          {"putty.exe", ProcessClass::RemoteTerminal},
          {"xshell.exe", ProcessClass::RemoteTerminal},
          {"mobaxterm.exe", ProcessClass::RemoteTerminal},
          {"windowsterminal.exe", ProcessClass::RemoteTerminal},
          {"securecrt.exe", ProcessClass::SerialTerminal},
          {"ttermpro.exe", ProcessClass::SerialTerminal},
          {"teraterm.exe", ProcessClass::SerialTerminal},
          {"puttytel.exe", ProcessClass::SerialTerminal},
          {"wireshark.exe", ProcessClass::PacketCapture},
          {"dumpcap.exe", ProcessClass::PacketCapture},
          {"tshark.exe", ProcessClass::PacketCapture},
          {"git.exe", ProcessClass::VersionControl},
          {"scp.exe", ProcessClass::VersionControl},
          {"sftp.exe", ProcessClass::VersionControl},
          {"git-remote-http.exe", ProcessClass::VersionControl},
          {"git-remote-https.exe", ProcessClass::VersionControl},
          {"cmake.exe", ProcessClass::BuildTool},
          {"ninja.exe", ProcessClass::BuildTool},
          {"make.exe", ProcessClass::BuildTool},
          {"msbuild.exe", ProcessClass::BuildTool},
          {"cl.exe", ProcessClass::BuildTool},
          {"clang.exe", ProcessClass::BuildTool},
          {"gcc.exe", ProcessClass::BuildTool},
          {"g++.exe", ProcessClass::BuildTool},
          {"python.exe", ProcessClass::BuildTool},
          {"python3.exe", ProcessClass::BuildTool},
          {"bash.exe", ProcessClass::BuildTool},
          {"wsl.exe", ProcessClass::BuildTool}};
  if (kImmutableSystemCritical.count(lower_name) != 0)
    return {ProcessClass::System, ProtectionLevel::SystemCritical};
  if (kImmutableSecurityCritical.count(lower_name) != 0)
    return {ProcessClass::Security, ProtectionLevel::SystemCritical};
  if (const auto workload = kImmutableProtectedWorkloads.find(lower_name);
      workload != kImmutableProtectedWorkloads.end()) {
    return {workload->second, ProtectionLevel::Strong};
  }

  const auto it = process_rules.find(lower_name);
  if (it != process_rules.end()) return it->second;
  // Unknown development tools are protected by default, never optimized.
  return {ProcessClass::Unknown, ProtectionLevel::Strong};
}

bool Config::IsAlwaysProtected(const std::string& process_name) const {
  return coding_profile.always_protect.count(ToLowerAscii(process_name)) != 0;
}

ServiceRule Config::ServiceRuleFor(const std::string& service_name) const {
  const std::string name = ToLowerAscii(service_name);
  static const std::unordered_set<std::string> kImmutableSystem{
      "appinfo",       "cryptsvc",      "dcomlaunch", "eventlog",
      "lsm",           "plugplay",      "power",      "profsvc",
      "rpceptmapper",  "rpcss",         "samss",      "schedule",
      "services",      "systemeventsbroker", "trustedinstaller",
      "winmgmt"};
  static const std::unordered_set<std::string> kImmutableNetwork{
      "bfe",         "dhcp",       "dnscache", "iphlpsvc",
      "lanmanserver", "lanmanworkstation", "lmhosts",   "mpssvc",
      "netprofm",    "nlasvc",     "nsi",      "rasman",
      "remoteaccess", "termservice", "umrdpservice"};
  static const std::unordered_set<std::string> kImmutableSecurity{
      "sense", "securityhealthservice", "windefend", "wdiservicehost",
      "wdisystemhost", "wscsvc", "wdnissvc"};
  static const std::unordered_set<std::string> kImmutableRemoteOrCapture{
      "npcap", "npf", "sessionenv", "ssh-agent", "sshd"};
  static const std::unordered_set<std::string> kImmutableDevice{
      "deviceassociationservice", "deviceinstall", "devquerybroker",
      "dsmsvc", "shellhwdetection", "wudfsvc", "wpdbusenum"};
  if (kImmutableSystem.count(name) != 0) {
    return {ServiceClass::System, ProtectionLevel::SystemCritical};
  }
  if (kImmutableNetwork.count(name) != 0) {
    return {ServiceClass::Network, ProtectionLevel::SystemCritical};
  }
  if (kImmutableSecurity.count(name) != 0) {
    return {ServiceClass::Security, ProtectionLevel::SystemCritical};
  }
  if (kImmutableRemoteOrCapture.count(name) != 0) {
    return {name == "npcap" || name == "npf"
                ? ServiceClass::PacketCapture
                : ServiceClass::RemoteAccess,
            ProtectionLevel::Strong};
  }
  if (kImmutableDevice.count(name) != 0) {
    return {ServiceClass::Device, ProtectionLevel::Strong};
  }
  const auto configured = service_rules.find(name);
  if (configured != service_rules.end()) return configured->second;
  return {ServiceClass::Unknown, ProtectionLevel::Strong};
}

bool Config::IsAlwaysProtectedService(const std::string& service_name) const {
  return coding_profile.always_protect_services.count(
             ToLowerAscii(service_name)) != 0;
}

}  // namespace workboost
