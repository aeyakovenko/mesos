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

#ifndef __CHECKER_HPP__
#define __CHECKER_HPP__

#include <cstdint>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/protobuf.hpp>
#include <process/time.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/stopwatch.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace checks {

// Forward declarations.
class CheckerProcess;

class Checker
{
public:
  /**
   * Attempts to create a `Checker` object. In case of success, checking
   * starts immediately after initialization.
   *
   * @param check The protobuf message definition of a check.
   * @param callback A callback `Checker` uses to send check status updates
   *     to its owner (usually an executor).
   * @param taskID The TaskID of the target task.
   * @param taskPid The target task's pid used to enter the specified
   *     namespaces.
   * @param namespaces The namespaces to enter prior performing a single check.
   * @return A `Checker` object or an error if `create` fails.
   *
   * @todo A better approach would be to return a stream of updates, e.g.,
   * `process::Stream<CheckStatusInfo>` rather than invoking a callback.
   */
  static Try<process::Owned<Checker>> create(
      const CheckInfo& checkInfo,
      const lambda::function<
        void(const TaskID&, const CheckStatusInfo&)>& callback,
      const TaskID& taskID,
      Option<pid_t> taskPid,
      const std::vector<std::string>& namespaces);

  ~Checker();

  /**
   * Immediately stops checking. Any in-flight checks are dropped.
   */
  void stop();

private:
  explicit Checker(process::Owned<CheckerProcess> process);

  process::Owned<CheckerProcess> process;
};


class CheckerProcess : public ProtobufProcess<CheckerProcess>
{
public:
  CheckerProcess(
      const CheckInfo& _check,
      const lambda::function<
        void(const TaskID&, const CheckStatusInfo&)>& _callback,
      const TaskID& _taskID,
      Option<pid_t> _taskPid,
      const std::vector<std::string>& _namespaces);

  virtual ~CheckerProcess() {}

protected:
  virtual void initialize() override;

private:
  void performSingleCheck();
  void scheduleNext(const Duration& duration);
  void processCheckResult(
      const Stopwatch& stopwatch,
      const CheckStatusInfo& result);

  process::Future<int> commandCheck();
  void processCommandCheckResult(
      const Stopwatch& stopwatch,
      const process::Future<int>& result);

  process::Future<int> httpCheck();
  process::Future<int> _httpCheck(
      const std::tuple<
          process::Future<Option<int>>,
          process::Future<std::string>,
          process::Future<std::string>>& t);
  void processHttpCheckResult(
      const Stopwatch& stopwatch,
      const process::Future<int>& result);

  CheckInfo check;
  Duration checkDelay;
  Duration checkInterval;
  Duration checkTimeout;

  const lambda::function<
    void(const TaskID&, const CheckStatusInfo&)> updateCallback;
  const TaskID taskID;
  const Option<pid_t> taskPid;
  const std::vector<std::string> namespaces;
  Option<lambda::function<pid_t(const lambda::function<int()>&)>> clone;

  process::Time startTime;
  CheckStatusInfo previousCheckStatus;
};

namespace validation {

// TODO(alexr): A better place for these functions would be something like
// "mesos_validation.cpp", since they validate API protobufs which are not
// solely related to this library.
Option<Error> checkInfo(const CheckInfo& checkInfo);
Option<Error> checkStatusInfo(const CheckStatusInfo& checkStatusInfo);

} // namespace validation {

} // namespace checks {
} // namespace internal {
} // namespace mesos {

#endif // __CHECKER_HPP__
