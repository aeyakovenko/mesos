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

#include "checks/checker.hpp"

#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <glog/logging.h>

#include <mesos/mesos.hpp>
#include <mesos/type_utils.hpp>

#include <process/clock.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/io.hpp>
#include <process/subprocess.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/exit.hpp>
#include <stout/foreach.hpp>
#include <stout/jsonify.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/environment.hpp>
#include <stout/os/killtree.hpp>

#include "common/status_utils.hpp"
#include "common/validation.hpp"

#ifdef __linux__
#include "linux/ns.hpp"
#endif

using process::Clock;
using process::Failure;
using process::Future;
using process::Owned;
using process::Subprocess;

using std::map;
using std::string;
using std::tuple;
using std::vector;

namespace mesos {
namespace internal {
namespace checks {

#ifndef __WINDOWS__
constexpr char HTTP_CHECK_COMMAND[] = "curl";
#else
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
          // This effectively aborts the check.
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


Try<Owned<Checker>> Checker::create(
    const CheckInfo& check,
    const lambda::function<
      void(const TaskID&, const CheckStatusInfo&)>& callback,
    const TaskID& taskID,
    Option<pid_t> taskPid,
    const vector<string>& namespaces)
{
  // Validate the `Check`Info` protobuf.
  Option<Error> error = validation::checkInfo(check);
  if (error.isSome()) {
    return error.get();
  }

  Owned<CheckerProcess> process(new CheckerProcess(
      check,
      callback,
      taskID,
      taskPid,
      namespaces));

  return Owned<Checker>(new Checker(process));
}


Checker::Checker(
    Owned<CheckerProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


Checker::~Checker()
{
  terminate(process.get());
  wait(process.get());
}


void Checker::stop()
{
  LOG(INFO) << "Checking stopped";

  terminate(process.get(), true);
}


CheckerProcess::CheckerProcess(
    const CheckInfo& _check,
    const lambda::function<
      void(const TaskID&, const CheckStatusInfo&)>& _callback,
    const TaskID& _taskID,
    Option<pid_t> _taskPid,
    const vector<string>& _namespaces)
  : ProcessBase(process::ID::generate("checker")),
    check(_check),
    updateCallback(_callback),
    taskID(_taskID),
    taskPid(_taskPid),
    namespaces(_namespaces)
{
  Try<Duration> create = Duration::create(check.delay_seconds());
  CHECK_SOME(create);
  checkDelay = create.get();

  create = Duration::create(check.interval_seconds());
  CHECK_SOME(create);
  checkInterval = create.get();

  // Zero value means infinite timeout.
  create = Duration::create(check.timeout_seconds());
  CHECK_SOME(create);
  checkTimeout =
    (create.get() > Duration::zero()) ? create.get() : Duration::max();

  // The first check update should be sent only when a check succeeds,
  // hence we should deduplicate against a corresponding "empty" result.
  previousCheckStatus.set_type(check.type());
  switch (check.type()) {
    case CheckInfo::COMMAND: {
      previousCheckStatus.mutable_command();
      break;
    }

    case CheckInfo::HTTP: {
      previousCheckStatus.mutable_http();
      break;
    }

    default: {
      UNREACHABLE();
    }
  }

#ifdef __linux__
  if (!namespaces.empty()) {
    clone = lambda::bind(&cloneWithSetns, lambda::_1, taskPid, namespaces);
  }
#endif
}


void CheckerProcess::initialize()
{
  VLOG(1) << "Check configuration:"
          << " '" << jsonify(JSON::Protobuf(check)) << "'";

  startTime = Clock::now();

  scheduleNext(checkDelay);
}


void CheckerProcess::performSingleCheck()
{
  Stopwatch stopwatch;
  stopwatch.start();

  switch (check.type()) {
    case CheckInfo::COMMAND: {
      commandCheck().onAny(defer(
          self(),
          &Self::processCommandCheckResult, stopwatch, lambda::_1));
      break;
    }

    case CheckInfo::HTTP: {
      httpCheck().onAny(defer(
          self(),
          &Self::processHttpCheckResult, stopwatch, lambda::_1));
      break;
    }

    default: {
      UNREACHABLE();
    }
  }
}


void CheckerProcess::scheduleNext(const Duration& duration)
{
  VLOG(1) << "Scheduling check in " << duration;

  delay(duration, self(), &Self::performSingleCheck);
}


void CheckerProcess::processCheckResult(
    const Stopwatch& stopwatch,
    const CheckStatusInfo& result)
{
  VLOG(1) << "Performed " << CheckInfo::Type_Name(check.type())
          << " check in " << stopwatch.elapsed();

  // Trigger the callback if and only if the value or presence
  // of any field in `CheckStatusInfo` has changed.
  if (result != previousCheckStatus) {
    // We assume this is a local send, i.e. the checker library is not used
    // in a binary external to the executor and hence can not exit before
    // the data is sent to the executor.
    updateCallback(taskID, result);
    previousCheckStatus = result;
  }

  scheduleNext(checkInterval);
}


Future<int> CheckerProcess::commandCheck()
{
  CHECK_EQ(CheckInfo::COMMAND, check.type());
  CHECK(check.has_command());

  const CommandInfo& command = check.command().command();

  map<string, string> environment = os::environment();

  foreach (const Environment::Variable& variable,
           command.environment().variables()) {
    environment[variable.name()] = variable.value();
  }

  // Launch the subprocess.
  Try<Subprocess> external = Error("Not launched");

  if (command.shell()) {
    // Use the shell variant.
    VLOG(1) << "Launching command check '" << command.value() << "'";

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

    VLOG(1) << "Launching command check [" << command.value() << ", "
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
        VLOG(1) << "Killing the command check process " << commandPid;

        os::killtree(commandPid, SIGKILL);
      }

      return Failure(
          "Command has not returned after " + stringify(timeout) +
          "; aborting");
    })
    .then([](const Option<int>& exitCode) -> Future<int> {
      if (exitCode.isNone()) {
        return Failure("Failed to reap the command process");
      }

      return exitCode.get();
    });
}


void CheckerProcess::processCommandCheckResult(
    const Stopwatch& stopwatch,
    const Future<int>& result)
{
  CheckStatusInfo checkStatusInfo;
  checkStatusInfo.set_type(check.type());

  if (result.isReady()) {
    VLOG(1) << CheckInfo::Type_Name(check.type())
            << " check returned " << result.get();

    checkStatusInfo.mutable_command()->set_exit_code(
        static_cast<int32_t>(result.get()));
  } else {
    // Check's status is currently not available, which may indicate a change
    // that should be reported as an empty `CheckStatusInfo.Command` message.
    LOG(WARNING) << "Check failed: "
                 << (result.isFailed() ? result.failure() : "discarded");

    checkStatusInfo.mutable_command();
  }

  processCheckResult(stopwatch, checkStatusInfo);
}


Future<int> CheckerProcess::httpCheck()
{
  CHECK_EQ(CheckInfo::HTTP, check.type());
  CHECK(check.has_http());

  const CheckInfo::Http& http = check.http();

  const string scheme = DEFAULT_HTTP_SCHEME;
  const string path = http.has_path() ? http.path() : "";
  const string url = scheme + "://" + DEFAULT_DOMAIN + ":" +
                     stringify(http.port()) + path;

  VLOG(1) << "Launching HTTP check '" << url << "'";

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
        VLOG(1) << "Killing the HTTP check process " << curlPid;

        os::killtree(curlPid, SIGKILL);
      }

      return Failure(
          string(HTTP_CHECK_COMMAND) + " has not returned after " +
          stringify(timeout) + "; aborting");
    })
    .then(defer(self(), &Self::_httpCheck, lambda::_1));
}


Future<int> CheckerProcess::_httpCheck(
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

  int exitCode = status->get();
  if (exitCode != 0) {
    Future<string> error = std::get<2>(t);
    if (!error.isReady()) {
      return Failure(
          string(HTTP_CHECK_COMMAND) + " returned " +
          WSTRINGIFY(exitCode) + "; reading stderr failed: " +
          (error.isFailed() ? error.failure() : "discarded"));
    }

    return Failure(
        string(HTTP_CHECK_COMMAND) + " returned " +
        WSTRINGIFY(exitCode) + ": " + error.get());
  }

  Future<string> output = std::get<1>(t);
  if (!output.isReady()) {
    return Failure(
        "Failed to read stdout from " + string(HTTP_CHECK_COMMAND) + ": " +
        (output.isFailed() ? output.failure() : "discarded"));
  }

  // Parse the output and get the HTTP status code.
  Try<int> statusCode = numify<int>(output.get());
  if (statusCode.isError()) {
    return Failure(
        "Unexpected output from " + string(HTTP_CHECK_COMMAND) + ": " +
        output.get());
  }

  return statusCode.get();
}


void CheckerProcess::processHttpCheckResult(
    const Stopwatch& stopwatch,
    const process::Future<int>& result)
{
  CheckStatusInfo checkStatusInfo;
  checkStatusInfo.set_type(check.type());

  if (result.isReady()) {
    VLOG(1) << CheckInfo::Type_Name(check.type())
            << " check returned " << result.get();

    checkStatusInfo.mutable_http()->set_status_code(
        static_cast<uint32_t>(result.get()));
  } else {
    // Check's status is currently not available, which may indicate a change
    // that should be reported as an empty `CheckStatusInfo.Http` message.
    LOG(WARNING) << "Check failed: "
                 << (result.isFailed() ? result.failure() : "discarded");

    checkStatusInfo.mutable_http();
  }

  processCheckResult(stopwatch, checkStatusInfo);
}

namespace validation {

Option<Error> checkInfo(const CheckInfo& checkInfo)
{
  if (!checkInfo.has_type()) {
    return Error("CheckInfo must specify 'type'");
  }

  switch (checkInfo.type()) {
    case CheckInfo::COMMAND: {
      if (!checkInfo.has_command()) {
        return Error("Expecting 'command' to be set for command check");
      }

      const CommandInfo& command = checkInfo.command().command();

      if (!command.has_value()) {
        string commandType =
          (command.shell() ? "'shell command'" : "'executable path'");

        return Error("Command check must contain " + commandType);
      }

      Option<Error> error =
        common::validation::validateCommandInfo(command);
      if (error.isSome()) {
        return Error(
            "Check's `CommandInfo` is invalid: " + error->message);
      }

      // TODO(alexr): Make sure irrelevant fields, e.g., `uris` are not set.

      break;
    }

    case CheckInfo::HTTP: {
      if (!checkInfo.has_http()) {
        return Error("Expecting 'http' to be set for HTTP check");
      }

      const CheckInfo::Http& http = checkInfo.http();

      if (http.has_path() && !strings::startsWith(http.path(), '/')) {
        return Error(
            "The path '" + http.path() +
            "' of HTTP  check must start with '/'");
      }

      break;
    }

    case CheckInfo::UNKNOWN: {
      return Error(
          "'" + CheckInfo::Type_Name(checkInfo.type()) + "'"
          " is not a valid check type");
    }
  }

  if (checkInfo.has_delay_seconds() && checkInfo.delay_seconds() < 0.0) {
    return Error("Expecting 'delay_seconds' to be non-negative");
  }

  if (checkInfo.has_interval_seconds() && checkInfo.interval_seconds() < 0.0) {
    return Error("Expecting 'interval_seconds' to be non-negative");
  }

  if (checkInfo.has_timeout_seconds() && checkInfo.timeout_seconds() < 0.0) {
    return Error("Expecting 'timeout_seconds' to be non-negative");
  }

  return None();
}


Option<Error> checkStatusInfo(const CheckStatusInfo& checkStatusInfo)
{
  if (!checkStatusInfo.has_type()) {
    return Error("CheckStatusInfo must specify 'type'");
  }

  switch (checkStatusInfo.type()) {
    case CheckInfo::COMMAND: {
      if (!checkStatusInfo.has_command()) {
        return Error(
            "Expecting 'command' to be set for command check's status");
      }
      break;
    }

    case CheckInfo::HTTP: {
      if (!checkStatusInfo.has_http()) {
        return Error("Expecting 'http' to be set for HTTP check's status");
      }
      break;
    }

    case CheckInfo::UNKNOWN: {
      return Error(
          "'" + CheckInfo::Type_Name(checkStatusInfo.type()) + "'"
          " is not a valid check's status type");
    }
  }

  return None();
}

} // namespace validation {

} // namespace checks {
} // namespace internal {
} // namespace mesos {
