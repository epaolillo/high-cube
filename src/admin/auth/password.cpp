#include "auth/password.hpp"

#include <crypt.h>

#include <random>
#include <stdexcept>
#include <string>

namespace highcube::admin {
namespace {

std::string randomSalt() {
  static constexpr char kAlphabet[] =
      "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<int> dist(0, static_cast<int>(sizeof(kAlphabet) - 2));
  std::string salt = "$2b$12$";
  for (int i = 0; i < 22; ++i) {
    salt.push_back(kAlphabet[dist(gen)]);
  }
  return salt;
}

}  // namespace

std::string hashPassword(const std::string& password) {
  struct crypt_data data {};
  data.initialized = 0;
  const std::string salt = randomSalt();
  char* hashed = crypt_r(password.c_str(), salt.c_str(), &data);
  if (!hashed) {
    throw std::runtime_error("password hashing failed");
  }
  return hashed;
}

bool verifyPassword(const std::string& password, const std::string& hash) {
  struct crypt_data data {};
  data.initialized = 0;
  char* hashed = crypt_r(password.c_str(), hash.c_str(), &data);
  return hashed && hash == hashed;
}

}  // namespace highcube::admin
