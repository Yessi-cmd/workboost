#pragma once

#include <string>

namespace workboost::windows {

// Static hardware identity used to label the dashboard's CPU and memory rows.
// Each returns false with *error set when the value cannot be read; callers
// treat that as "model unknown" and leave the label empty.
bool QueryCpuModel(std::string* model, std::string* error = nullptr);
bool QueryMemoryModel(std::string* model, std::string* error = nullptr);

}  // namespace workboost::windows
