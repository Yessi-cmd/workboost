#pragma once

#include "core/config/config.h"
#include "core/helper/helper_protocol.h"
#include "core/model/types.h"

namespace workboost {

helper::Response HandleElevatedRequest(const helper::Request& request,
                                       const Config& config,
                                       const RuntimeContext& context);

}  // namespace workboost
