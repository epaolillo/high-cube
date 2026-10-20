#pragma once

#include "api/router.hpp"
#include "services/services.hpp"

namespace highcube::admin {

void registerHandlers(Router& router, AppContext& ctx);

}  // namespace highcube::admin
