#pragma once

#include "db/database.hpp"
#include "domain/models.hpp"

#include <optional>
#include <string>
#include <vector>

namespace highcube::admin {

// Rows of vpn_nodes. The network settings stay in the settings table.
class VpnNodeRepository {
public:
  explicit VpnNodeRepository(Database& database);

  std::vector<VpnNode> list();
  std::optional<VpnNode> findById(int64_t id);
  std::optional<VpnNode> findByServerId(int64_t serverId);
  std::optional<VpnNode> findByTokenHash(const std::string& tokenHash);
  std::optional<VpnNode> findByPublicKey(const std::string& publicKey);
  int64_t create(const VpnNode& node);
  void update(const VpnNode& node);
  void remove(int64_t id);
  void removeAll();
  // Enrollments that were never used before their token expired.
  void removeExpiredEnrollments(const std::string& nowIso);

private:
  Database& database_;
  std::optional<VpnNode> findOne(const std::string& where, const std::string& value);
};

}  // namespace highcube::admin
