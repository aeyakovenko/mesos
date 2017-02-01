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

#include "checks/utils.hpp"

#include <mesos/mesos.hpp>

#include "common/http.hpp"

#include "internal/evolve.hpp"

using process::Future;

using process::http::Connection;
using process::http::Response;

namespace mesos {
namespace internal {
namespace checks {

inline process::http::Request createRequest(
    const process::http::URL& url,
    const agent::Call& call)
{
  process::http::Request request;
  request.method = "POST";
  request.url = url;
  request.body = serialize(ContentType::PROTOBUF, evolve(call));
  request.keepAlive = false;
  request.headers = {{"Accept", stringify(ContentType::PROTOBUF)},
                     {"Content-Type", stringify(ContentType::PROTOBUF)}};

  return request;
}


Future<Response> post(
    const process::http::URL& url,
    const agent::Call& call)
{
  return process::http::request(createRequest(url, call), false);
}


Future<Response> post(
    Connection& connection,
    const process::http::URL& url,
    const agent::Call& call)
{
  return connection.send(createRequest(url, call), false);
}

} // namespace checks {
} // namespace internal {
} // namespace mesos {
