#include "runtime/git_checkout.hpp"

#include <boost/process.hpp>
#include <boost/process/extend.hpp>
#include <spdlog/spdlog.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

#include <unistd.h>

namespace bp = boost::process;

namespace highcube::agent {
namespace {

std::string base64Encode(const std::string& input) {
  static const char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((input.size() + 2) / 3) * 4);
  for (std::size_t i = 0; i < input.size(); i += 3) {
    const unsigned int n =
        (static_cast<unsigned char>(input[i]) << 16) |
        ((i + 1 < input.size() ? static_cast<unsigned char>(input[i + 1]) : 0) << 8) |
        (i + 2 < input.size() ? static_cast<unsigned char>(input[i + 2]) : 0);
    out.push_back(table[(n >> 18) & 63]);
    out.push_back(table[(n >> 12) & 63]);
    out.push_back(i + 1 < input.size() ? table[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < input.size() ? table[n & 63] : '=');
  }
  return out;
}

struct ProcessGroup : bp::extend::handler {
  template <typename Executor>
  void on_exec_setup(Executor&) const {
    ::setpgid(0, 0);
  }
};

void killProcessGroup(int pid) {
  if (pid <= 0) {
    return;
  }
  ::kill(-pid, SIGKILL);
  ::kill(pid, SIGKILL);
}

template <typename Out>
int runGit(const std::vector<std::string>& args, const bp::environment& env, Out&& output,
           const std::function<void(int)>& onSpawn, const std::function<bool()>& isCancelled) {
  if (isCancelled && isCancelled()) {
    throw std::runtime_error("deploy cancelled");
  }
  bp::child child(bp::search_path("git"), bp::args(args), env, ProcessGroup(), bp::std_out > output,
                  bp::std_err > bp::null);
  const int pid = static_cast<int>(child.id());
  if (onSpawn) {
    onSpawn(pid);
  }
  if (isCancelled && isCancelled()) {
    killProcessGroup(pid);
  }
  child.wait();
  if (onSpawn) {
    onSpawn(0);
  }
  if (isCancelled && isCancelled()) {
    throw std::runtime_error("deploy cancelled");
  }
  return child.exit_code();
}

}  // namespace

CheckoutResult checkoutRepository(const std::string& dataDir, int64_t applicationId,
                                  int64_t deploymentId, const std::string& repoUrl,
                                  const std::string& branch, const std::string& commitSha,
                                  const std::string& deployKeyPrivate,
                                  const std::string& gitAccessToken,
                                  const std::function<void(int)>& onSpawn,
                                  const std::function<bool()>& isCancelled) {
  const auto root = std::filesystem::path(dataDir) / "applications" /
                    std::to_string(applicationId) / std::to_string(deploymentId);
  std::filesystem::create_directories(root);
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  bp::environment env = boost::this_process::environment();
  std::filesystem::path keyFile;
  if (!deployKeyPrivate.empty() && gitAccessToken.empty()) {
    keyFile = root / "deploy_key";
    std::ofstream out(keyFile);
    out << deployKeyPrivate;
    if (!deployKeyPrivate.empty() && deployKeyPrivate.back() != '\n') {
      out << '\n';
    }
    out.close();
    std::filesystem::permissions(keyFile, std::filesystem::perms::owner_read |
                                              std::filesystem::perms::owner_write);
    env["GIT_SSH_COMMAND"] = "ssh -i " + keyFile.string() + " -o StrictHostKeyChecking=no";
  }

  std::string cloneUrl = repoUrl;
  int cloneCode = 1;
  if (!gitAccessToken.empty()) {
    if (cloneUrl.rfind("git@github.com:", 0) == 0) {
      auto rest = cloneUrl.substr(std::string("git@github.com:").size());
      if (rest.size() < 4 || rest.substr(rest.size() - 4) != ".git") {
        rest += ".git";
      }
      cloneUrl = "https://github.com/" + rest;
    }
    const std::string header =
        "Authorization: Basic " + base64Encode("x-access-token:" + gitAccessToken);
    cloneCode = runGit({"-c", "http.extraHeader=" + header, "clone", "--depth", "1", "--branch",
                        branch, cloneUrl, root.string()},
                       env, bp::null, onSpawn, isCancelled);
  } else {
    if (!deployKeyPrivate.empty() && cloneUrl.rfind("https://github.com/", 0) == 0) {
      auto rest = cloneUrl.substr(std::string("https://github.com/").size());
      if (!rest.empty() && rest.back() == '/') {
        rest.pop_back();
      }
      if (rest.size() < 4 || rest.substr(rest.size() - 4) != ".git") {
        rest += ".git";
      }
      cloneUrl = "git@github.com:" + rest;
    }
    cloneCode = runGit({"clone", "--depth", "1", "--branch", branch, cloneUrl, root.string()}, env,
                       bp::null, onSpawn, isCancelled);
  }
  if (cloneCode != 0) {
    throw std::runtime_error("git clone failed");
  }

  CheckoutResult result;
  result.path = root.string();
  if (!commitSha.empty()) {
    runGit({"-C", root.string(), "checkout", commitSha}, env, bp::null, onSpawn, isCancelled);
    result.commitSha = commitSha;
  } else {
    bp::ipstream out;
    runGit({"-C", root.string(), "rev-parse", "HEAD"}, env, out, onSpawn, isCancelled);
    std::getline(out, result.commitSha);
  }
  {
    bp::ipstream out;
    runGit({"-C", root.string(), "log", "-1", "--format=%s"}, env, out, onSpawn, isCancelled);
    std::getline(out, result.commitMessage);
  }
  return result;
}

}  // namespace highcube::agent
