/* Copyright (c) 2018 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include <errno.h>
#include <folly/ssl/Init.h>
#include <signal.h>
#include <string.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include "common/base/Base.h"
#include "common/fs/FileUtils.h"
#include "common/network/NetworkUtils.h"
#include "common/process/ProcessUtils.h"
#include "common/ssl/SSLConfig.h"
#include "common/time/TimezoneInfo.h"
#include "graph/service/GraphFlags.h"
#include "graph/service/GraphServer.h"
#include "graph/service/GraphService.h"
#include "graph/stats/GraphStats.h"
#include "version/Version.h"
#include "webservice/WebService.h"

using nebula::ProcessUtils;
using nebula::Status;
using nebula::StatusOr;
using nebula::fs::FileUtils;
using nebula::graph::GraphService;
using nebula::network::NetworkUtils;

static void signalHandler(int sig);
static Status setupSignalHandler();
extern Status setupLogging();
static void printHelp(const char *prog);
#if defined(__x86_64__)
extern Status setupBreakpad();
#endif

DECLARE_string(flagfile);
DECLARE_bool(containerized);

std::unique_ptr<nebula::graph::GraphServer> gServer;

int main(int argc, char *argv[]) {
  google::SetVersionString(nebula::versionString());
  if (argc == 1) {
    printHelp(argv[0]);
    return EXIT_FAILURE;
  }
  if (argc == 2) {
    if (::strcmp(argv[1], "-h") == 0) {
      printHelp(argv[0]);
      return EXIT_SUCCESS;
    }
  }

  folly::init(&argc, &argv, true);
  if (FLAGS_enable_ssl || FLAGS_enable_graph_ssl || FLAGS_enable_meta_ssl) {
    folly::ssl::init();
  }
  nebula::initGraphStats();

  if (FLAGS_flagfile.empty()) {
    printHelp(argv[0]);
    return EXIT_FAILURE;
  }

  // Setup logging
  auto status = setupLogging();
  if (!status.ok()) {
    LOG(ERROR) << status;
    return EXIT_FAILURE;
  }

#if defined(__x86_64__)
  status = setupBreakpad();
  if (!status.ok()) {
    LOG(ERROR) << status;
    return EXIT_FAILURE;
  }
#endif

  // Detect if the server has already been started
  auto pidPath = FLAGS_pid_file;
  status = ProcessUtils::isPidAvailable(pidPath);
  if (!status.ok()) {
    LOG(ERROR) << status;
    return EXIT_FAILURE;
  }

  if (FLAGS_daemonize) {
    status = ProcessUtils::daemonize(pidPath);
    if (!status.ok()) {
      LOG(ERROR) << status;
      return EXIT_FAILURE;
    }
  } else {
    // Write the current pid into the pid file
    status = ProcessUtils::makePidFile(pidPath);
    if (!status.ok()) {
      LOG(ERROR) << status;
      return EXIT_FAILURE;
    }
  }

  // Validate the IPv4 address or hostname
  status = NetworkUtils::validateHostOrIp(FLAGS_local_ip);
  if (!status.ok()) {
    LOG(ERROR) << status;
    return EXIT_FAILURE;
  }
  nebula::HostAddr localhost{FLAGS_local_ip, FLAGS_port};

  // load the time zone data
  status = nebula::time::Timezone::init();
  if (!status.ok()) {
    LOG(ERROR) << status;
    return EXIT_FAILURE;
  }

  // Initialize the global timezone, it's only used for datetime type compute
  // won't affect the process timezone.
  status = nebula::time::Timezone::initializeGlobalTimezone();
  if (!status.ok()) {
    LOG(ERROR) << status;
    return EXIT_FAILURE;
  }

  LOG(INFO) << "Starting Graph HTTP Service";
  auto webSvc = std::make_unique<nebula::WebService>();
  status = webSvc->start();
  if (!status.ok()) {
    return EXIT_FAILURE;
  }

  if (FLAGS_num_netio_threads == 0) {
    FLAGS_num_netio_threads = std::thread::hardware_concurrency();
  }
  if (FLAGS_num_netio_threads <= 0) {
    LOG(WARNING) << "Number of networking IO threads should be greater than zero";
    return EXIT_FAILURE;
  }
  LOG(INFO) << "Number of networking IO threads: " << FLAGS_num_netio_threads;

  if (FLAGS_num_worker_threads == 0) {
    FLAGS_num_worker_threads = std::thread::hardware_concurrency();
  }
  if (FLAGS_num_worker_threads <= 0) {
    LOG(WARNING) << "Number of worker threads should be greater than zero";
    return EXIT_FAILURE;
  }
  LOG(INFO) << "Number of worker threads: " << FLAGS_num_worker_threads;

  // Setup the signal handlers
  status = setupSignalHandler();
  if (!status.ok()) {
    LOG(ERROR) << status;
    return EXIT_FAILURE;
  }

  gServer = std::make_unique<nebula::graph::GraphServer>(localhost);

  if (!gServer->start()) {
    LOG(ERROR) << "The graph server start failed";
    gServer->stop();
    return EXIT_FAILURE;
  }

  gServer->waitUntilStop();
  LOG(INFO) << "The graph Daemon stopped";
  return EXIT_SUCCESS;
}

Status setupSignalHandler() {
  return nebula::SignalHandler::install(
      {SIGINT, SIGTERM},
      [](nebula::SignalHandler::GeneralSignalInfo *info) { signalHandler(info->sig()); });
}

void signalHandler(int sig) {
  switch (sig) {
    case SIGINT:
    case SIGTERM:
      FLOG_INFO("Signal %d(%s) received, stopping this server", sig, ::strsignal(sig));
      gServer->notifyStop();
      break;
    default:
      FLOG_ERROR("Signal %d(%s) received but ignored", sig, ::strsignal(sig));
  }
}

void printHelp(const char *prog) {
  fprintf(stderr, "%s --flagfile <config_file>\n", prog);
}
