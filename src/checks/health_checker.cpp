// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "checks/health_checker.hpp"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#ifndef __WINDOWS__
#include <unistd.h>
#endif // __WINDOWS__

#include <iostream>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/agent/agent.hpp>

#include <process/collect.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/http.hpp>
#include <process/io.hpp>
#include <process/subprocess.hpp>

#include <stout/duration.hpp>
#include <stout/json.hpp>
#include <stout/jsonify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/killtree.hpp>

#include "checks/utils.hpp"

#include "common/http.hpp"
#include "common/status_utils.hpp"
#include "common/validation.hpp"

#ifdef __linux__
#include "linux/ns.hpp"
#endif

using process::delay;
using process::dispatch;
using process::Clock;
using process::Failure;
using process::Future;
using process::Owned;
using process::Subprocess;
using process::Time;

using process::http::Connection;
using process::http::Response;

using std::map;
using std::string;
using std::tuple;
using std::vector;

namespace mesos {
namespace internal {
namespace checks {

#ifndef __WINDOWS__
constexpr char TCP_CHECK_COMMAND[] = "mesos-tcp-connect";
constexpr char HTTP_CHECK_COMMAND[] = "curl";
#else
constexpr char TCP_CHECK_COMMAND[] = "mesos-tcp-connect.exe";
constexpr char HTTP_CHECK_COMMAND[] = "curl.exe";
#endif // __WINDOWS__

static const string DEFAULT_HTTP_SCHEME = "http";

// Use '127.0.0.1' instead of 'localhost', because the host
// file in some container images may not contain 'localhost'.
static const string DEFAULT_DOMAIN = "127.0.0.1";


#ifdef __linux__
// TODO(alexr): Instead of defining this ad-hoc clone function, provide a
// general solution for entring namespace in child processes, see MESOS-6184.
static pid_t cloneWithSetns(
    const lambda::function<int()>& func,
    Option<pid_t> taskPid,
    const vector<string>& namespaces)
{
  return process::defaultClone([=]() -> int {
    if (taskPid.isSome()) {
      foreach (const string& ns, namespaces) {
        Try<Nothing> setns = ns::setns(taskPid.get(), ns);
        if (setns.isError()) {
          // This effectively aborts the health check.
          LOG(FATAL) << "Failed to enter the " << ns << " namespace of "
                     << "task (pid: '" << taskPid.get() << "'): "
                     << setns.error();
        }

        VLOG(1) << "Entered the " << ns << " namespace of "
                << "task (pid: '" << taskPid.get() << "') successfully";
      }
    }

    return func();
  });
}
#endif


Try<Owned<HealthChecker>> HealthChecker::create(
    const HealthCheck& check,
    const string& launcherDir,
    const lambda::function<void(const TaskHealthStatus&)>& callback,
    const TaskID& taskId,
    Option<pid_t> taskPid,
    const vector<string>& namespaces)
{
  // Validate the 'HealthCheck' protobuf.
  Option<Error> error = validation::healthCheck(check);
  if (error.isSome()) {
    return error.get();
  }

  Owned<HealthCheckerProcess> process(new HealthCheckerProcess(
      check,
      launcherDir,
      callback,
      taskId,
      taskPid,
      namespaces,
      None(),
      None(),
      None(),
      false));

  return Owned<HealthChecker>(new HealthChecker(process));
}


Try<Owned<HealthChecker>> HealthChecker::create(
    const HealthCheck& check,
    const string& launcherDir,
    const lambda::function<void(const TaskHealthStatus&)>& callback,
    const TaskID& taskId,
    const ContainerID& taskContainerId,
    const process::http::URL& agentURL,
    const Option<Environment>& taskEnv)
{
  // Validate the 'HealthCheck' protobuf.
  Option<Error> error = validation::healthCheck(check);
  if (error.isSome()) {
    return error.get();
  }

  Owned<HealthCheckerProcess> process(new HealthCheckerProcess(
      check,
      launcherDir,
      callback,
      taskId,
      None(),
      vector<string>(),
      taskContainerId,
      agentURL,
      taskEnv,
      true));

  return Owned<HealthChecker>(new HealthChecker(process));
}


HealthChecker::HealthChecker(
    Owned<HealthCheckerProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


HealthChecker::~HealthChecker()
{
  terminate(process.get());
  wait(process.get());
}


void HealthChecker::stop()
{
  LOG(INFO) << "Health checking stopped";

  terminate(process.get(), true);
}


HealthCheckerProcess::HealthCheckerProcess(
    const HealthCheck& _check,
    const string& _launcherDir,
    const lambda::function<void(const TaskHealthStatus&)>& _callback,
    const TaskID& _taskId,
    const Option<pid_t>& _taskPid,
    const vector<string>& _namespaces,
    const Option<ContainerID>& _taskContainerId,
    const Option<process::http::URL>& _agentURL,
    const Option<Environment>& _taskEnv,
    bool _agentSpawnsCommandContainer)
  : ProcessBase(process::ID::generate("health-checker")),
    check(_check),
    launcherDir(_launcherDir),
    healthUpdateCallback(_callback),
    taskId(_taskId),
    taskPid(_taskPid),
    namespaces(_namespaces),
    taskContainerId(_taskContainerId),
    agentURL(_agentURL),
    taskEnv(_taskEnv),
    agentSpawnsCommandContainer(_agentSpawnsCommandContainer),
    consecutiveFailures(0),
    initializing(true)
{
  Try<Duration> create = Duration::create(check.delay_seconds());
  CHECK_SOME(create);
  checkDelay = create.get();

  create = Duration::create(check.interval_seconds());
  CHECK_SOME(create);
  checkInterval = create.get();

  create = Duration::create(check.grace_period_seconds());
  CHECK_SOME(create);
  checkGracePeriod = create.get();

  // Zero value means infinite timeout.
  create = Duration::create(check.timeout_seconds());
  CHECK_SOME(create);
  checkTimeout =
    (create.get() > Duration::zero()) ? create.get() : Duration::max();

#ifdef __linux__
  if (!namespaces.empty()) {
    clone = lambda::bind(&cloneWithSetns, lambda::_1, taskPid, namespaces);
  }
#endif
}


void HealthCheckerProcess::initialize()
{
  VLOG(1) << "Health check configuration:"
          << " '" << jsonify(JSON::Protobuf(check)) << "'";

  startTime = Clock::now();

  scheduleNext(checkDelay);
}


void HealthCheckerProcess::failure(const string& message)
{
  if (initializing &&
      checkGracePeriod.secs() > 0 &&
      (Clock::now() - startTime) <= checkGracePeriod) {
    LOG(INFO) << "Ignoring failure as health check still in grace period";
    scheduleNext(checkInterval);
    return;
  }

  consecutiveFailures++;
  LOG(WARNING) << "Health check failed " << consecutiveFailures
               << " times consecutively: " << message;

  bool killTask = consecutiveFailures >= check.consecutive_failures();

  TaskHealthStatus taskHealthStatus;
  taskHealthStatus.set_healthy(false);
  taskHealthStatus.set_consecutive_failures(consecutiveFailures);
  taskHealthStatus.set_kill_task(killTask);
  taskHealthStatus.mutable_task_id()->CopyFrom(taskId);

  // We assume this is a local send, i.e. the health checker library
  // is not used in a binary external to the executor and hence can
  // not exit before the data is sent to the executor.
  healthUpdateCallback(taskHealthStatus);

  // Even if we set the `kill_task` flag, it is an executor who kills the task
  // and honors the flag (or not). We have no control over the task's lifetime,
  // hence we should continue until we are explicitly asked to stop.
  scheduleNext(checkInterval);
}


void HealthCheckerProcess::success()
{
  VLOG(1) << HealthCheck::Type_Name(check.type()) << " health check passed";

  // Send a healthy status update on the first success,
  // and on the first success following failure(s).
  if (initializing || consecutiveFailures > 0) {
    TaskHealthStatus taskHealthStatus;
    taskHealthStatus.set_healthy(true);
    taskHealthStatus.mutable_task_id()->CopyFrom(taskId);
    healthUpdateCallback(taskHealthStatus);
    initializing = false;
  }

  consecutiveFailures = 0;
  scheduleNext(checkInterval);
}


void HealthCheckerProcess::performSingleCheck()
{
  Future<Nothing> checkResult;

  Stopwatch stopwatch;
  stopwatch.start();

  switch (check.type()) {
    case HealthCheck::COMMAND: {
      if (agentSpawnsCommandContainer) {
        checkResult = nestedCommandHealthCheck();
      } else {
        checkResult = commandHealthCheck();
      }

      break;
    }

    case HealthCheck::HTTP: {
      checkResult = httpHealthCheck();
      break;
    }

    case HealthCheck::TCP: {
      checkResult = tcpHealthCheck();
      break;
    }

    default: {
      UNREACHABLE();
    }
  }

  checkResult.onAny(defer(
      self(),
      &Self::processCheckResult, stopwatch, lambda::_1));
}


void HealthCheckerProcess::processCheckResult(
    const Stopwatch& stopwatch,
    const Future<Nothing>& future)
{
  VLOG(1) << "Performed " << HealthCheck::Type_Name(check.type())
          << " health check in " << stopwatch.elapsed();

  if (future.isReady()) {
    success();
    return;
  }

  string message = HealthCheck::Type_Name(check.type()) +
                   " health check failed: " +
                   (future.isFailed() ? future.failure() : "discarded");

  failure(message);
}


Future<Nothing> HealthCheckerProcess::nestedCommandHealthCheck()
{
  CHECK_EQ(HealthCheck::COMMAND, check.type());
  CHECK(taskContainerId.isSome());
  CHECK(check.has_command());
  CHECK(agentURL.isSome());

  return process::http::connect(agentURL.get())
    .repair([](const Future<Connection>& future) {
      return Failure(
          "Unable to establish connection with the agent: " + future.failure());
    })
    .then(defer(self(), &Self::_nestedCommandHealthCheck, lambda::_1));
}


Future<Nothing> HealthCheckerProcess::_nestedCommandHealthCheck(
    Connection connection)
{
  ContainerID checkContainerId;
  checkContainerId.set_value(taskContainerId.get().value() + "-health-check");
  checkContainerId.mutable_parent()->CopyFrom(taskContainerId.get());

  CommandInfo command(check.command());

  if (taskEnv.isSome()) {
    // Merge the task and the check environments. The environment of
    // the health check takes precedence over the one of the task.
    //
    // TODO(gkleiman): This only merges the Task env as described by the
    // framework, but the actual env might be different. A better approach
    // would be to make the agent copy the env of the parent container.

    map<string, string> env;

    foreach (const Environment::Variable& variable, taskEnv->variables()) {
      env[variable.name()] = variable.value();
    }

    foreach (const Environment::Variable& variable,
        check.command().environment().variables()) {
      env[variable.name()] = variable.value();
    }

    command.clear_environment();
    foreachpair (const string& name, const string& value, env) {
      Environment::Variable* variable =
        command.mutable_environment()->mutable_variables()->Add();
      variable->set_name(name);
      variable->set_value(value);
    }
  }

  agent::Call call;
  call.set_type(agent::Call::LAUNCH_NESTED_CONTAINER_SESSION);

  agent::Call::LaunchNestedContainerSession* launch =
    call.mutable_launch_nested_container_session();

  launch->mutable_container_id()->CopyFrom(checkContainerId);
  launch->mutable_command()->CopyFrom(command);

  return post(connection, agentURL.get(), call)
    .after(checkTimeout,
           defer(self(),
                 &Self::nestedCommandHealthCheckTimedOut,
                 checkContainerId,
                 connection,
                 lambda::_1))
    .then(defer(self(),
          &Self::__nestedCommandHealthCheck,
          checkContainerId,
          lambda::_1));
}


Future<Nothing> HealthCheckerProcess::__nestedCommandHealthCheck(
    const ContainerID& checkContainerId,
    const Response& launchResponse)
{
  if (launchResponse.code != process::http::Status::OK) {
    return Failure(
        "Received '" + launchResponse.status + "' (" + launchResponse.body +
        ") while launching command health check in a child container");
  }

  return waitForNestedContainer(checkContainerId)
    .then([](const Option<int> status) -> Future<Nothing> {
      if (status.isNone() || status.get() != 0) {
        return Failure("Command returned " + stringify(status.get()));
      } else {
        return Nothing();
      }
    });
}


Future<Response>
HealthCheckerProcess::nestedCommandHealthCheckTimedOut(
    const ContainerID& checkContainerId,
    Connection connection,
    Future<Response> future)
{
  future.discard();

  // Closing the connection will make the agent kill the container.
  connection.disconnect();

  const Failure failure =
    Failure("Command has not returned after " + stringify(checkTimeout));

  return waitForNestedContainer(checkContainerId)
    .repair([failure](const Future<Option<int>>& future) {
        return failure;
    })
    .then([failure](const Option<int>& status) -> Future<Response> {
        return failure;
    });
}


Future<Option<int>> HealthCheckerProcess::waitForNestedContainer(
    const ContainerID& containerId)
{
  agent::Call call;
  call.set_type(agent::Call::WAIT_NESTED_CONTAINER);

  agent::Call::WaitNestedContainer* containerWait =
    call.mutable_wait_nested_container();

  containerWait->mutable_container_id()->CopyFrom(containerId);

  return post(agentURL.get(), call)
    .then([this](const Response& httpResponse) -> Future<Option<int>> {
      if (httpResponse.code != process::http::Status::OK) {
        return Failure(
            "Received '" + httpResponse.status + "' (" + httpResponse.body +
            ") waiting on health check of task '" + stringify(taskId) + "'");
      }

      Try<agent::Response> response =
        deserialize<agent::Response>(ContentType::PROTOBUF, httpResponse.body);
      CHECK_SOME(response);

      return response->wait_nested_container().exit_status();
    });
}


Future<Nothing> HealthCheckerProcess::commandHealthCheck()
{
  CHECK_EQ(HealthCheck::COMMAND, check.type());
  CHECK(check.has_command());

  const CommandInfo& command = check.command();

  map<string, string> environment = os::environment();

  foreach (const Environment::Variable& variable,
           command.environment().variables()) {
    environment[variable.name()] = variable.value();
  }

  // Launch the subprocess.
  Try<Subprocess> external = Error("Not launched");

  if (command.shell()) {
    // Use the shell variant.
    VLOG(1) << "Launching command health check '" << command.value() << "'";

    external = subprocess(
        command.value(),
        Subprocess::PATH("/dev/null"),
        Subprocess::FD(STDERR_FILENO),
        Subprocess::FD(STDERR_FILENO),
        environment,
        clone);
  } else {
    // Use the exec variant.
    vector<string> argv;
    foreach (const string& arg, command.arguments()) {
      argv.push_back(arg);
    }

    VLOG(1) << "Launching command health check [" << command.value() << ", "
            << strings::join(", ", argv) << "]";

    external = subprocess(
        command.value(),
        argv,
        Subprocess::PATH("/dev/null"),
        Subprocess::FD(STDERR_FILENO),
        Subprocess::FD(STDERR_FILENO),
        nullptr,
        environment,
        clone);
  }

  if (external.isError()) {
    return Failure("Failed to create subprocess: " + external.error());
  }

  pid_t commandPid = external->pid();
  const Duration timeout = checkTimeout;

  return external->status()
    .after(
        timeout,
        [timeout, commandPid](Future<Option<int>> future) {
      future.discard();

      if (commandPid != -1) {
        // Cleanup the external command process.
        VLOG(1) << "Killing the command health check process " << commandPid;

        os::killtree(commandPid, SIGKILL);
      }

      return Failure(
          "Command has not returned after " + stringify(timeout) +
          "; aborting");
    })
    .then([](const Option<int>& status) -> Future<Nothing> {
      if (status.isNone()) {
        return Failure("Failed to reap the command process");
      }

      int statusCode = status.get();
      if (statusCode != 0) {
        return Failure("Command returned " + WSTRINGIFY(statusCode));
      }

      return Nothing();
    });
}


Future<Nothing> HealthCheckerProcess::httpHealthCheck()
{
  CHECK_EQ(HealthCheck::HTTP, check.type());
  CHECK(check.has_http());

  const HealthCheck::HTTPCheckInfo& http = check.http();

  const string scheme = http.has_scheme() ? http.scheme() : DEFAULT_HTTP_SCHEME;
  const string path = http.has_path() ? http.path() : "";
  const string url = scheme + "://" + DEFAULT_DOMAIN + ":" +
                     stringify(http.port()) + path;

  VLOG(1) << "Launching HTTP health check '" << url << "'";

  const vector<string> argv = {
    HTTP_CHECK_COMMAND,
    "-s",                 // Don't show progress meter or error messages.
    "-S",                 // Makes curl show an error message if it fails.
    "-L",                 // Follows HTTP 3xx redirects.
    "-k",                 // Ignores SSL validation when scheme is https.
    "-w", "%{http_code}", // Displays HTTP response code on stdout.
    "-o", "/dev/null",    // Ignores output.
    url
  };

  // TODO(alexr): Consider launching the helper binary once per task lifetime,
  // see MESOS-6766.
  Try<Subprocess> s = subprocess(
      HTTP_CHECK_COMMAND,
      argv,
      Subprocess::PATH("/dev/null"),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      nullptr,
      None(),
      clone);

  if (s.isError()) {
    return Failure(
        "Failed to create the " + string(HTTP_CHECK_COMMAND) +
        " subprocess: " + s.error());
  }

  pid_t curlPid = s->pid();
  const Duration timeout = checkTimeout;

  return await(
      s->status(),
      process::io::read(s->out().get()),
      process::io::read(s->err().get()))
    .after(
        timeout,
        [timeout, curlPid](Future<tuple<Future<Option<int>>,
                                        Future<string>,
                                        Future<string>>> future) {
      future.discard();

      if (curlPid != -1) {
        // Cleanup the HTTP_CHECK_COMMAND process.
        VLOG(1) << "Killing the HTTP health check process " << curlPid;

        os::killtree(curlPid, SIGKILL);
      }

      return Failure(
          string(HTTP_CHECK_COMMAND) + " timed out after " +
          stringify(timeout) + "; aborting");
    })
    .then(defer(self(), &Self::_httpHealthCheck, lambda::_1));
}


Future<Nothing> HealthCheckerProcess::_httpHealthCheck(
    const tuple<
        Future<Option<int>>,
        Future<string>,
        Future<string>>& t)
{
  Future<Option<int>> status = std::get<0>(t);
  if (!status.isReady()) {
    return Failure(
        "Failed to get the exit status of the " + string(HTTP_CHECK_COMMAND) +
        " process: " + (status.isFailed() ? status.failure() : "discarded"));
  }

  if (status->isNone()) {
    return Failure(
        "Failed to reap the " + string(HTTP_CHECK_COMMAND) + " process");
  }

  int statusCode = status->get();
  if (statusCode != 0) {
    Future<string> error = std::get<2>(t);
    if (!error.isReady()) {
      return Failure(
          string(HTTP_CHECK_COMMAND) + " returned " +
          WSTRINGIFY(statusCode) + "; reading stderr failed: " +
          (error.isFailed() ? error.failure() : "discarded"));
    }

    return Failure(
        string(HTTP_CHECK_COMMAND) + " returned " +
        WSTRINGIFY(statusCode) + ": " + error.get());
  }

  Future<string> output = std::get<1>(t);
  if (!output.isReady()) {
    return Failure(
        "Failed to read stdout from " + string(HTTP_CHECK_COMMAND) + ": " +
        (output.isFailed() ? output.failure() : "discarded"));
  }

  // Parse the output and get the HTTP response code.
  Try<int> code = numify<int>(output.get());
  if (code.isError()) {
    return Failure(
        "Unexpected output from " + string(HTTP_CHECK_COMMAND) + ": " +
        output.get());
  }

  if (code.get() < process::http::Status::OK ||
      code.get() >= process::http::Status::BAD_REQUEST) {
    return Failure(
        "Unexpected HTTP response code: " +
        process::http::Status::string(code.get()));
  }

  return Nothing();
}


Future<Nothing> HealthCheckerProcess::tcpHealthCheck()
{
  CHECK_EQ(HealthCheck::TCP, check.type());
  CHECK(check.has_tcp());

  // TCP_CHECK_COMMAND should be reachable.
  CHECK(os::exists(launcherDir));

  const HealthCheck::TCPCheckInfo& tcp = check.tcp();

  VLOG(1) << "Launching TCP health check at port '" << tcp.port() << "'";

  const string tcpConnectPath = path::join(launcherDir, TCP_CHECK_COMMAND);

  const vector<string> tcpConnectArguments = {
    tcpConnectPath,
    "--ip=" + DEFAULT_DOMAIN,
    "--port=" + stringify(tcp.port())
  };

  // TODO(alexr): Consider launching the helper binary once per task lifetime,
  // see MESOS-6766.
  Try<Subprocess> s = subprocess(
      tcpConnectPath,
      tcpConnectArguments,
      Subprocess::PATH("/dev/null"),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      nullptr,
      None(),
      clone);

  if (s.isError()) {
    return Failure(
        "Failed to create the " + string(TCP_CHECK_COMMAND) +
        " subprocess: " + s.error());
  }

  pid_t tcpConnectPid = s->pid();
  const Duration timeout = checkTimeout;

  return await(
      s->status(),
      process::io::read(s->out().get()),
      process::io::read(s->err().get()))
    .after(
        timeout,
        [timeout, tcpConnectPid](Future<tuple<Future<Option<int>>,
                                              Future<string>,
                                              Future<string>>> future) {
      future.discard();

      if (tcpConnectPid != -1) {
        // Cleanup the TCP_CHECK_COMMAND process.
        VLOG(1) << "Killing the TCP health check process " << tcpConnectPid;

        os::killtree(tcpConnectPid, SIGKILL);
      }

      return Failure(
          string(TCP_CHECK_COMMAND) + " timed out after " +
          stringify(timeout) + "; aborting");
    })
    .then(defer(self(), &Self::_tcpHealthCheck, lambda::_1));
}


Future<Nothing> HealthCheckerProcess::_tcpHealthCheck(
    const tuple<
        Future<Option<int>>,
        Future<string>,
        Future<string>>& t)
{
  Future<Option<int>> status = std::get<0>(t);
  if (!status.isReady()) {
    return Failure(
        "Failed to get the exit status of the " + string(TCP_CHECK_COMMAND) +
        " process: " + (status.isFailed() ? status.failure() : "discarded"));
  }

  if (status->isNone()) {
    return Failure(
        "Failed to reap the " + string(TCP_CHECK_COMMAND) + " process");
  }

  int statusCode = status->get();
  if (statusCode != 0) {
    Future<string> error = std::get<2>(t);
    if (!error.isReady()) {
      return Failure(
          string(TCP_CHECK_COMMAND) + " returned " +
          WSTRINGIFY(statusCode) + "; reading stderr failed: " +
          (error.isFailed() ? error.failure() : "discarded"));
    }

    return Failure(
        string(TCP_CHECK_COMMAND) + " returned " +
        WSTRINGIFY(statusCode) + ": " + error.get());
  }

  return Nothing();
}


void HealthCheckerProcess::scheduleNext(const Duration& duration)
{
  VLOG(1) << "Scheduling health check in " << duration;

  delay(duration, self(), &Self::performSingleCheck);
}


namespace validation {

Option<Error> healthCheck(const HealthCheck& check)
{
  if (!check.has_type()) {
    return Error("HealthCheck must specify 'type'");
  }

  switch (check.type()) {
    case HealthCheck::COMMAND: {
      if (!check.has_command()) {
        return Error("Expecting 'command' to be set for command health check");
      }

      const CommandInfo& command = check.command();

      if (!command.has_value()) {
        string commandType =
          (command.shell() ? "'shell command'" : "'executable path'");

        return Error("Command health check must contain " + commandType);
      }

      Option<Error> error =
        common::validation::validateCommandInfo(command);
      if (error.isSome()) {
        return Error(
            "Health check's `CommandInfo` is invalid: " + error->message);
      }

      break;
    }

    case HealthCheck::HTTP: {
      if (!check.has_http()) {
        return Error("Expecting 'http' to be set for HTTP health check");
      }

      const HealthCheck::HTTPCheckInfo& http = check.http();

      if (http.has_scheme() &&
          http.scheme() != "http" &&
          http.scheme() != "https") {
        return Error(
            "Unsupported HTTP health check scheme: '" + http.scheme() + "'");
      }

      if (http.has_path() && !strings::startsWith(http.path(), '/')) {
        return Error(
            "The path '" + http.path() +
            "' of HTTP health check must start with '/'");
      }

      break;
    }

    case HealthCheck::TCP: {
      if (!check.has_tcp()) {
        return Error("Expecting 'tcp' to be set for TCP health check");
      }

      break;
    }

    case HealthCheck::UNKNOWN: {
      return Error(
          "'" + HealthCheck::Type_Name(check.type()) + "'"
          " is not a valid health check type");
    }
  }

  return None();
}

} // namespace validation {

} // namespace checks {
} // namespace internal {
} // namespace mesos {
