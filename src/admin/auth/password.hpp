#pragma once

#include <string>

namespace highcube::admin {

std::string hashPassword(const std::string& password);
bool verifyPassword(const std::string& password, const std::string& hash);

}  // namespace highcube::admin
