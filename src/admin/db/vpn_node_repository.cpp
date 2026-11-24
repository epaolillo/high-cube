#include "db/vpn_node_repository.hpp"

namespace highcube::admin {
namespace {

constexpr const char* kColumns =
    "SELECT id,kind,server_id,user_id,name,COALESCE(address,''),COALESCE(public_key,''),listen_port,"
    "endpoint,lan_ip,observed_ip,status,COALESCE(token_hash,''),COALESCE(token_expires_at,''),"
    "COALESCE(last_seen_at,''),created_at FROM vpn_nodes ";

VpnNode readNode(SQLite::Statement& st) {
  VpnNode node;
  node.id = st.getColumn(0).getInt64();
  node.kind = st.getColumn(1).getString();
  if (!st.getColumn(2).isNull()) {
    node.serverId = st.getColumn(2).getInt64();
  }
  if (!st.getColumn(3).isNull()) {
    node.userId = st.getColumn(3).getInt64();
  }
  node.name = st.getColumn(4).getString();
  node.address = st.getColumn(5).getString();
  node.publicKey = st.getColumn(6).getString();
  node.listenPort = st.getColumn(7).getInt();
  node.endpoint = st.getColumn(8).getString();
  node.lanIp = st.getColumn(9).getString();
  node.observedIp = st.getColumn(10).getString();
  node.status = st.getColumn(11).getString();
  node.tokenHash = st.getColumn(12).getString();
  node.tokenExpiresAt = st.getColumn(13).getString();
  node.lastSeenAt = st.getColumn(14).getString();
  node.createdAt = st.getColumn(15).getString();
  return node;
}

// Empty strings are stored as NULL where the column is nullable and unique.
void bindNullable(SQLite::Statement& st, int index, const std::string& value) {
  if (value.empty()) {
    st.bind(index);
  } else {
    st.bind(index, value);
  }
}

void bindOptional(SQLite::Statement& st, int index, const std::optional<int64_t>& value) {
  if (value) {
    st.bind(index, *value);
  } else {
    st.bind(index);
  }
}

}  // namespace

VpnNodeRepository::VpnNodeRepository(Database& database) : database_(database) {}

std::vector<VpnNode> VpnNodeRepository::list() {
  SQLite::Statement st(database_.db(), std::string(kColumns) + "ORDER BY id ASC");
  std::vector<VpnNode> rows;
  while (st.executeStep()) {
    rows.push_back(readNode(st));
  }
  return rows;
}

std::optional<VpnNode> VpnNodeRepository::findById(int64_t id) {
  SQLite::Statement st(database_.db(), std::string(kColumns) + "WHERE id=?");
  st.bind(1, id);
  if (!st.executeStep()) {
    return std::nullopt;
  }
  return readNode(st);
}

std::optional<VpnNode> VpnNodeRepository::findByServerId(int64_t serverId) {
  SQLite::Statement st(database_.db(), std::string(kColumns) + "WHERE server_id=?");
  st.bind(1, serverId);
  if (!st.executeStep()) {
    return std::nullopt;
  }
  return readNode(st);
}

std::optional<VpnNode> VpnNodeRepository::findByTokenHash(const std::string& tokenHash) {
  return findOne("token_hash", tokenHash);
}

std::optional<VpnNode> VpnNodeRepository::findByPublicKey(const std::string& publicKey) {
  return findOne("public_key", publicKey);
}

std::optional<VpnNode> VpnNodeRepository::findOne(const std::string& column,
                                                  const std::string& value) {
  if (value.empty()) {
    return std::nullopt;
  }
  SQLite::Statement st(database_.db(), std::string(kColumns) + "WHERE " + column + "=? LIMIT 1");
  st.bind(1, value);
  if (!st.executeStep()) {
    return std::nullopt;
  }
  return readNode(st);
}

int64_t VpnNodeRepository::create(const VpnNode& node) {
  SQLite::Statement st(
      database_.db(),
      "INSERT INTO vpn_nodes (kind,server_id,user_id,name,address,public_key,listen_port,endpoint,"
      "lan_ip,observed_ip,status,token_hash,token_expires_at,last_seen_at,created_at) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  st.bind(1, node.kind);
  bindOptional(st, 2, node.serverId);
  bindOptional(st, 3, node.userId);
  st.bind(4, node.name);
  bindNullable(st, 5, node.address);
  bindNullable(st, 6, node.publicKey);
  st.bind(7, node.listenPort);
  st.bind(8, node.endpoint);
  st.bind(9, node.lanIp);
  st.bind(10, node.observedIp);
  st.bind(11, node.status);
  bindNullable(st, 12, node.tokenHash);
  bindNullable(st, 13, node.tokenExpiresAt);
  bindNullable(st, 14, node.lastSeenAt);
  st.bind(15, node.createdAt);
  st.exec();
  return database_.db().getLastInsertRowid();
}

void VpnNodeRepository::update(const VpnNode& node) {
  SQLite::Statement st(
      database_.db(),
      "UPDATE vpn_nodes SET name=?,address=?,public_key=?,listen_port=?,endpoint=?,lan_ip=?,"
      "observed_ip=?,status=?,token_hash=?,token_expires_at=?,last_seen_at=?,user_id=? WHERE id=?");
  st.bind(1, node.name);
  bindNullable(st, 2, node.address);
  bindNullable(st, 3, node.publicKey);
  st.bind(4, node.listenPort);
  st.bind(5, node.endpoint);
  st.bind(6, node.lanIp);
  st.bind(7, node.observedIp);
  st.bind(8, node.status);
  bindNullable(st, 9, node.tokenHash);
  bindNullable(st, 10, node.tokenExpiresAt);
  bindNullable(st, 11, node.lastSeenAt);
  bindOptional(st, 12, node.userId);
  st.bind(13, node.id);
  st.exec();
}

void VpnNodeRepository::remove(int64_t id) {
  SQLite::Statement st(database_.db(), "DELETE FROM vpn_nodes WHERE id=?");
  st.bind(1, id);
  st.exec();
}

void VpnNodeRepository::removeAll() { database_.db().exec("DELETE FROM vpn_nodes"); }

void VpnNodeRepository::removeExpiredEnrollments(const std::string& nowIso) {
  SQLite::Statement st(database_.db(),
                       "DELETE FROM vpn_nodes WHERE status='enrolling' AND token_expires_at < ?");
  st.bind(1, nowIso);
  st.exec();
}

}  // namespace highcube::admin
