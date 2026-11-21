#pragma once

#include <string>
#include <vector>

namespace highcube::agent::audit {

struct Mount {
  std::string filesystem;
  std::string path;
  long long size = 0;
  long long used = 0;
  long long available = 0;
};

// Real filesystems from df (tmpfs, overlay, squashfs and friends excluded).
std::vector<Mount> listMounts();
long long availableBytes(const std::string& path);
long long duBytes(const std::string& path, int timeoutSeconds);
std::vector<std::string> homeDirectories();
// Package-manager caches inside each home that are always safe to drop.
const std::vector<std::string>& userCacheDirectories();
// find predicate matching rotated logs; the audit and the cleanup action share it.
std::vector<std::string> rotatedLogsPredicate();

}  // namespace highcube::agent::audit
