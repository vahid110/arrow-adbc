// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "native_protocol_runner.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace adbc::qualification {
namespace {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
constexpr size_t count = static_cast<size_t>(NativeStepId::kCount);
constexpr uint64_t record_cap = 3 * 1024 * 1024, group_cap = 4 * 1024 * 1024;
constexpr uint64_t total_cap = 32 * 1024 * 1024, cache_cap = 16 * 1024 * 1024;
enum class SendKind { kExecute, kPrepare, kDescribe };
// Translation-unit-private seams; the standalone socket-free test includes this
// source and replaces these calls. No extensible transport API is exported.
struct Calls {
  Time (*now)();
  bool (*ready)(PGconn*);
  bool (*utf8)(PGconn*);
  int (*send)(PGconn*, const NativeStepPlan&, SendKind, const RawParameter*);
  int (*flush)(PGconn*);
  int (*busy)(PGconn*);
  int (*consume)(PGconn*);
  PGresult* (*get)(PGconn*);
  int (*wait)(PGconn*, int16_t, int);
  CaptureStatus (*capture)(NativeStepId, const PGresult*, Observation*, CaptureLimits);
  ssize_t (*write)(int, const void*, size_t);
  int (*sync)(int);
};
bool Healthy(PGconn* c) { return PQstatus(c) == CONNECTION_OK; }
bool Ready(PGconn* c) {
  if (!Healthy(c) || !PQisnonblocking(c) || PQtransactionStatus(c) != PQTRANS_IDLE ||
      PQisBusy(c))
    return false;
#ifdef LIBPQ_HAS_PIPELINING
  if (PQpipelineStatus(c) != PQ_PIPELINE_OFF) return false;
#endif
  return true;
}
bool Utf8(PGconn* c) {
  const char* value = PQparameterStatus(c, "client_encoding");
  return value && std::strcmp(value, "UTF8") == 0;
}
int Send(PGconn* c, const NativeStepPlan& p, SendKind kind, const RawParameter* owner) {
  char statement[40] = "adbc_native_";
  const auto end = std::to_chars(statement + 12, statement + sizeof(statement) - 1,
                                 static_cast<unsigned>(p.id));
  if (end.ec != std::errc()) return 0;
  *end.ptr = 0;
  if (kind == SendKind::kPrepare)
    return PQsendPrepare(c, statement, p.sql.c_str(), 0, nullptr);
  if (kind == SendKind::kDescribe) return PQsendDescribePrepared(c, statement);
  const auto parameter = owner ? owner->Get() : std::nullopt;
  if (owner && !parameter) return 0;
  return PQsendQueryParams(
      c, p.sql.c_str(), parameter ? 1 : 0, parameter ? &parameter->type : nullptr,
      parameter ? &parameter->value : nullptr, parameter ? &parameter->length : nullptr,
      parameter ? &parameter->format : nullptr, p.requested_format.value_or(0));
}
int Wait(PGconn* c, int16_t events, int milliseconds) {
  const int fd = PQsocket(c);
  if (fd < 0) return -1;
  pollfd item{fd, events, 0};
  const int rc = poll(&item, 1, milliseconds);
  if (rc <= 0) return rc;
  if (item.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
  return item.revents & events ? 1 : 0;
}
Calls RealCalls() {
  return {Clock::now, Ready,          Utf8,        Send, PQflush,
          PQisBusy,   PQconsumeInput, PQgetResult, Wait, CaptureNativeResult,
          ::write,    ::fsync};
}
class Output {
 public:
  Output(const NativeRunInput& input, const Calls& calls)
      : input_(input), calls_(calls) {}
  bool Initialize() {
    for (size_t i = 0; i < 8; ++i) {
      if (input_.outputs[i] <= 2 || !Check(i, false)) return Fail();
      for (size_t j = 0; j < i; ++j)
        if (identity_[i].st_dev == identity_[j].st_dev &&
            identity_[i].st_ino == identity_[j].st_ino)
          return Fail();
    }
    initialized_ = true;
    return true;
  }
  bool CheckAll() {
    if (!initialized_ || failed_) return false;
    for (size_t i = 0; i < 8; ++i)
      if (!Check(i, true)) return Fail();
    return true;
  }
  bool Record(size_t group, std::string_view json) {
    if (group >= 8 || failed_ || !initialized_ || json.empty() || json.front() != '{' ||
        json.back() != '}' ||
        json.find_first_of("\r\n\0", 0, 3) != std::string_view::npos ||
        json.size() >= record_cap || json.size() + 1 > group_cap - bytes_[group] ||
        json.size() + 1 > total_cap - total_ || !CheckAll())
      return Fail();
    if (!Write(group, json) || !Write(group, "\n") || !CheckAll()) return false;
    return true;
  }
  bool Complete() {
    if (!CheckAll()) return false;
    for (size_t i = 0; i < 8; ++i) {
      if (calls_.now() >= input_.cleanup_deadline || calls_.sync(input_.outputs[i]) ||
          calls_.now() >= input_.cleanup_deadline || !CheckAll())
        return Fail();
    }
    return true;
  }
  bool failed() const { return failed_; }
  const std::array<uint64_t, 8>& bytes() const { return bytes_; }

 private:
  bool Fail() {
    failed_ = true;
    return false;
  }
  bool Check(size_t i, bool compare) {
    const int fd = input_.outputs[i], flags = fcntl(fd, F_GETFL);
    struct stat st {};
    if (flags < 0 || (flags & O_APPEND) || (flags & O_ACCMODE) == O_RDONLY ||
        fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 07777) != 0600 || st.st_nlink != 1 || st.st_size < 0 ||
        static_cast<uint64_t>(st.st_size) != bytes_[i] ||
        lseek(fd, 0, SEEK_CUR) != static_cast<off_t>(bytes_[i]))
      return false;
    if (compare && (st.st_dev != identity_[i].st_dev || st.st_ino != identity_[i].st_ino))
      return false;
    identity_[i] = st;
    return true;
  }
  bool Write(size_t group, std::string_view data) {
    while (!data.empty()) {
      if (calls_.now() >= input_.cleanup_deadline) return Fail();
      const ssize_t n = calls_.write(input_.outputs[group], data.data(), data.size());
      if (n > 0 && static_cast<size_t>(n) <= data.size()) {
        bytes_[group] += n;
        total_ += n;
        data.remove_prefix(static_cast<size_t>(n));
      } else if (!(n < 0 && errno == EINTR))
        return Fail();
      if (calls_.now() >= input_.cleanup_deadline) return Fail();
    }
    return true;
  }
  const NativeRunInput& input_;
  const Calls& calls_;
  std::array<struct stat, 8> identity_{};
  std::array<uint64_t, 8> bytes_{};
  uint64_t total_ = 0;
  bool initialized_ = false, failed_ = false;
};
struct Execution {
  std::unique_ptr<PGresult, decltype(&PQclear)> result{nullptr, PQclear};
  NativeRunReason reason = NativeRunReason::kTransport;
  NativeRunPhase phase = NativeRunPhase::kSend;
  int calls = 0;
  bool completed = false;
  std::optional<int> observed_terminal;
};
bool Before(Time deadline, const Calls& c) { return c.now() < deadline; }
bool WaitUntil(PGconn* session, int16_t events, Time deadline, const Calls& c) {
  while (Before(deadline, c)) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - c.now()).count();
    if (remaining <= 0) return false;
    const int rc =
        c.wait(session, events, static_cast<int>(std::min<int64_t>(100, remaining)));
    if (!Before(deadline, c)) return false;
    if (rc > 0) return true;
    if (rc < 0 && errno != EINTR) return false;
  }
  return false;
}
Execution Execute(PGconn* session, const NativeStepPlan& p, SendKind kind,
                  const RawParameter* parameter, Time deadline, const Calls& c,
                  Output& output, bool cleanup) {
  Execution e;
  e.phase = kind == SendKind::kPrepare    ? NativeRunPhase::kPrepare
            : kind == SendKind::kDescribe ? NativeRunPhase::kDescribe
                                          : NativeRunPhase::kSend;
  if (!Before(deadline, c)) {
    e.reason = NativeRunReason::kDeadline;
    return e;
  }
  if (!c.ready(session) || !Before(deadline, c)) {
    e.reason =
        Before(deadline, c) ? NativeRunReason::kSession : NativeRunReason::kDeadline;
    return e;
  }
  if (!c.utf8(session) || !Before(deadline, c)) {
    e.reason =
        Before(deadline, c) ? NativeRunReason::kEncoding : NativeRunReason::kDeadline;
    return e;
  }
  if ((!cleanup && !output.CheckAll()) || !Before(deadline, c)) {
    e.reason =
        Before(deadline, c) ? NativeRunReason::kOutput : NativeRunReason::kDeadline;
    return e;
  }
  ++e.calls;
  const int sent = c.send(session, p, kind, parameter);
  if (!Before(deadline, c)) {
    e.reason = NativeRunReason::kDeadline;
    return e;
  }
  if (!sent) return e;
  e.phase = NativeRunPhase::kFlush;
  while (Before(deadline, c)) {
    const int status = c.flush(session);
    if (!Before(deadline, c)) {
      e.reason = NativeRunReason::kDeadline;
      return e;
    }
    if (status == 0) break;
    if (status < 0 || !WaitUntil(session, POLLIN | POLLOUT, deadline, c)) {
      e.reason =
          Before(deadline, c) ? NativeRunReason::kTransport : NativeRunReason::kDeadline;
      return e;
    }
    if (!c.consume(session) || !Before(deadline, c)) {
      e.reason =
          Before(deadline, c) ? NativeRunReason::kTransport : NativeRunReason::kDeadline;
      return e;
    }
  }
  e.phase = NativeRunPhase::kRead;
  for (;;) {
    if (!Before(deadline, c)) {
      e.reason = NativeRunReason::kDeadline;
      return e;
    }
    const int busy = c.busy(session);
    if (!Before(deadline, c)) {
      e.reason = NativeRunReason::kDeadline;
      return e;
    }
    if (busy) {
      if (!WaitUntil(session, POLLIN, deadline, c) || !c.consume(session)) {
        e.reason = Before(deadline, c) ? NativeRunReason::kTransport
                                       : NativeRunReason::kDeadline;
        return e;
      }
      continue;
    }
    std::unique_ptr<PGresult, decltype(&PQclear)> result(c.get(session), PQclear);
    if (result) e.observed_terminal = PQresultStatus(result.get());
    if (!Before(deadline, c)) {
      e.reason = NativeRunReason::kDeadline;
      return e;
    }
    if (!result) {
      if (!e.result || !c.ready(session) || !Before(deadline, c)) {
        e.reason = Before(deadline, c) ? NativeRunReason::kTransport
                                       : NativeRunReason::kDeadline;
        return e;
      }
      e.completed = true;
      return e;
    }
    if (e.result) {
      e.reason = NativeRunReason::kUnexpectedResult;
      return e;
    }
    const auto status = PQresultStatus(result.get());
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK &&
        status != PGRES_FATAL_ERROR) {
      e.reason = NativeRunReason::kUnexpectedResult;
      return e;
    }
    e.result = std::move(result);
  }
}
std::optional<uint64_t> Storage(const Observation& value) {
  uint64_t total = sizeof(value);
  auto add = [&](uint64_t n) { return detail::CheckedAdd(total, n, cache_cap, &total); };
  auto vector = [&](uint64_t n, uint64_t size) {
    uint64_t bytes;
    return detail::CheckedMultiply(n, size, cache_cap, &bytes) && add(bytes);
  };
  if (!add(value.json.capacity() + 1) ||
      !vector(value.fields.capacity(), sizeof(CapturedField)) ||
      !vector(value.rows.capacity(), sizeof(std::vector<CapturedCell>)))
    return std::nullopt;
  for (const auto& f : value.fields)
    if (!add(f.name.capacity() + 1)) return std::nullopt;
  for (const auto& row : value.rows) {
    if (!vector(row.capacity(), sizeof(CapturedCell))) return std::nullopt;
    for (const auto& cell : row)
      if (!add(cell.bytes.capacity() + 1)) return std::nullopt;
  }
  if (value.parameters && !vector(value.parameters->capacity(), sizeof(uint32_t)))
    return std::nullopt;
  return total;
}
struct CacheEntry {
  Observation value;
  PGconn* session;
  uint64_t bytes;
};
bool ValidState(std::string_view s) {
  if (s.size() != 5) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) return false;
  return true;
}
bool Definitive(const Observation& observation) {
  if (observation.result_status != PGRES_FATAL_ERROR || !observation.sqlstate ||
      observation.sqlstate->size() != 5)
    return false;
  const auto& s = *observation.sqlstate;
  if (!ValidState(s)) return false;
  return s.substr(0, 2) != "08" && s != "57014" && s != "57P01" && s != "57P02" &&
         s != "57P03";
}
bool Stops(NativeErrorPolicy policy, bool definitive) {
  return !definitive || policy != NativeErrorPolicy::kContinueDefinitive;
}
std::optional<size_t> Blocked(const NativeStepPlan& p,
                              const std::array<NativeRunState, count>& states) {
  for (auto id : p.prerequisites)
    if (states[static_cast<size_t>(id)] != NativeRunState::kCaptured)
      return static_cast<size_t>(id);
  return std::nullopt;
}
class Runner {
 public:
  Runner(const NativeRunInput& input, const Calls& c)
      : input_(input), c_(c), output_(input, c) {
    last_.fill(-1);
  }
  NativeRunResult Run() noexcept {
    try {
      const auto start = c_.now();
      if (!ValidOwnedSchemaSyntax(input_.schema) ||
          input_.schema != input_.authenticated_cleanup_schema || !input_.sessions[0] ||
          !input_.sessions[1] || input_.sessions[0] == input_.sessions[1] ||
          input_.discovery_deadline <= start ||
          input_.discovery_deadline > start + std::chrono::seconds(720) ||
          input_.cleanup_deadline <= input_.discovery_deadline ||
          input_.cleanup_deadline >
              input_.discovery_deadline + std::chrono::seconds(180) ||
          !output_.Initialize()) {
        Failure(0, NativeRunPhase::kPreflight, NativeRunReason::kInvalidInput);
        return Finish();
      }
      for (auto session : input_.sessions) {
        if (!Before(input_.discovery_deadline, c_)) {
          Failure(0, NativeRunPhase::kPreflight, NativeRunReason::kDeadline);
          return Finish();
        }
        const bool ready = c_.ready(session);
        if (!Before(input_.discovery_deadline, c_)) {
          Failure(0, NativeRunPhase::kPreflight, NativeRunReason::kDeadline);
          return Finish();
        }
        if (!ready) {
          Failure(0, NativeRunPhase::kPreflight, NativeRunReason::kSession);
          return Finish();
        }
        const bool utf8 = c_.utf8(session);
        if (!Before(input_.discovery_deadline, c_)) {
          Failure(0, NativeRunPhase::kPreflight, NativeRunReason::kDeadline);
          return Finish();
        }
        if (!utf8) {
          Failure(0, NativeRunPhase::kPreflight, NativeRunReason::kEncoding);
          return Finish();
        }
      }
      for (size_t i = 0; i < count; ++i) {
        const auto p = BuildNativeStep(static_cast<NativeStepId>(i), input_.schema);
        if (!p) throw std::bad_alloc();
        if (p->parameter)
          last_[static_cast<size_t>(p->parameter->source)] = static_cast<int>(i);
      }
      for (size_t i = 0; i + 1 < count; ++i) {
        current_ = i;
        active_ = NativeRunPhase::kPreflight;
        const auto p = BuildNativeStep(static_cast<NativeStepId>(i), input_.schema);
        if (!p) throw std::bad_alloc();
        if (!Before(input_.discovery_deadline, c_)) {
          Failure(i, NativeRunPhase::kSend, NativeRunReason::kDeadline);
          stopped_ = true;
        }
        auto blocked = Blocked(*p, result_.states);
        if (p->parameter && !cache_[static_cast<size_t>(p->parameter->source)])
          blocked = static_cast<size_t>(p->parameter->source);
        Observation observation;
        NativeRunPhase phase = NativeRunPhase::kPreflight;
        std::optional<int> terminal;
        int calls = 0;
        if (stopped_ || blocked) {
          result_.states[i] = NativeRunState::kSkipped;
          reasons_[i] =
              stopped_ ? NativeRunReason::kStopped : NativeRunReason::kDependency;
        } else
          Step(*p, observation, phase, terminal, calls, false);
        Emit(*p, observation, phase, terminal, calls, blocked);
        for (size_t id = 0; id < count; ++id)
          if (last_[id] == static_cast<int>(i)) Release(id);
      }
      result_.discovery_finished = !stopped_;
    } catch (...) {
      Failure(current_, active_, NativeRunReason::kAllocation);
      stopped_ = true;
    }
    // Independently permitted finally action. Unknown/duplicate CREATE never owns.
    try {
      current_ = count - 1;
      const auto p = BuildNativeStep(static_cast<NativeStepId>(current_), input_.schema);
      if (!p) throw std::bad_alloc();
      Observation observation;
      NativeRunPhase phase = NativeRunPhase::kCleanup;
      std::optional<int> terminal;
      int calls = 0;
      if (result_.schema_created &&
          input_.schema == input_.authenticated_cleanup_schema &&
          Before(input_.cleanup_deadline, c_) && c_.ready(input_.sessions[0]) &&
          c_.utf8(input_.sessions[0]) && Before(input_.cleanup_deadline, c_)) {
        Step(*p, observation, phase, terminal, calls, true);
        result_.cleanup_captured = result_.states[current_] == NativeRunState::kCaptured;
      } else {
        result_.states[current_] = NativeRunState::kSkipped;
        if (result_.schema_created) Failure(current_, phase, NativeRunReason::kOwnership);
      }
      Emit(*p, observation, phase, terminal, calls, std::nullopt);
    } catch (...) {
      Failure(current_, NativeRunPhase::kCleanup, NativeRunReason::kAllocation);
    }
    return Finish();
  }

 private:
  void Failure(size_t step, NativeRunPhase phase, NativeRunReason reason,
               const Observation* obs = nullptr) {
    reasons_[step] = reason;
    if (result_.first_failure) return;
    NativeRunFailure f{step, phase, reason, {}};
    if (obs && obs->sqlstate && ValidState(*obs->sqlstate))
      std::copy_n(obs->sqlstate->data(), 5, f.sqlstate.data());
    result_.first_failure = f;
  }
  void Release(size_t id) {
    if (cache_[id]) {
      cache_bytes_ -= cache_[id]->bytes;
      cache_[id].reset();
    }
  }
  void Step(const NativeStepPlan& p, Observation& observation, NativeRunPhase& phase,
            std::optional<int>& terminal, int& calls, bool cleanup) {
    const size_t i = static_cast<size_t>(p.id), which = static_cast<size_t>(p.connection);
    PGconn* session = input_.sessions[which];
    auto until =
        std::min(cleanup ? input_.cleanup_deadline : input_.discovery_deadline,
                 c_.now() + std::chrono::seconds(p.name == "setup.hll_input.values" ||
                                                         p.name == "setup.hll_cases.1" ||
                                                         p.name == "setup.hll_cases.2" ||
                                                         p.name == "setup.hll_cases.3" ||
                                                         p.name == "setup.hll_cases.4"
                                                     ? 10
                                                     : 3));
    if (i >= 122 && !cleanup && !qualified_[which]) {
      result_.states[i] = NativeRunState::kInvalid;
      Failure(i, NativeRunPhase::kPreflight, NativeRunReason::kSession);
      stopped_ = true;
      return;
    }
    std::unique_ptr<RawParameter> owner;
    if (p.parameter) {
      phase = NativeRunPhase::kParameter;
      active_ = phase;
      auto& source = cache_[static_cast<size_t>(p.parameter->source)];
      if (!source || source->session != session || !c_.utf8(session) ||
          !Before(until, c_)) {
        result_.states[i] = NativeRunState::kInvalid;
        Failure(i, phase, NativeRunReason::kParameter);
        stopped_ = true;
        return;
      }
      owner = std::make_unique<RawParameter>();
      if (PrepareNativeInput(p.id, p.parameter->source, source->value, owner.get()) !=
              ParameterStatus::kReady ||
          !Before(until, c_)) {
        result_.states[i] = NativeRunState::kInvalid;
        Failure(i, phase, NativeRunReason::kParameter);
        stopped_ = true;
        return;
      }
    }
    Execution e;
    if (p.operation == NativeOperation::kPrepareDescribe) {
      e = Execute(session, p, SendKind::kPrepare, nullptr, until, c_, output_, cleanup);
      calls += e.calls;
      result_.operation_calls += e.calls;
      if (e.calls) ++result_.attempted_steps;
      if (e.completed && PQresultStatus(e.result.get()) == PGRES_COMMAND_OK) {
        if (PQnfields(e.result.get()) != 0 || PQntuples(e.result.get()) != 0 ||
            PQnparams(e.result.get()) != 0) {
          phase = NativeRunPhase::kPrepare;
          terminal = e.observed_terminal;
          result_.states[i] = NativeRunState::kUncertain;
          Failure(i, phase, NativeRunReason::kUnexpectedResult);
          stopped_ = true;
          return;
        }
        // Prepare success is not the final describe observation. Same deadline.
        e = Execute(session, p, SendKind::kDescribe, nullptr, until, c_, output_,
                    cleanup);
        calls += e.calls;
        result_.operation_calls += e.calls;
      }
    } else {
      e = Execute(session, p, SendKind::kExecute, owner.get(), until, c_, output_,
                  cleanup);
      calls = e.calls;
      result_.operation_calls += e.calls;
      if (e.calls) ++result_.attempted_steps;
    }
    if (cleanup && calls) result_.cleanup_attempted = true;
    phase = e.phase;
    terminal = e.observed_terminal;
    if (!e.completed) {
      result_.states[i] = NativeRunState::kUncertain;
      Failure(i, phase, e.reason);
      stopped_ = true;
      if (owner && calls) result_.pending_parameters[which] = std::move(owner);
      return;
    }
    // Never decode text after an encoding-changing result or a slow check.
    phase = NativeRunPhase::kCapture;
    active_ = phase;
    if (!c_.utf8(session) || !Before(until, c_)) {
      result_.states[i] = NativeRunState::kUncertain;
      Failure(
          i, phase,
          Before(until, c_) ? NativeRunReason::kEncoding : NativeRunReason::kDeadline);
      stopped_ = true;
      return;
    }
    CaptureLimits limits;
    limits.encoded_bytes = record_cap - 1024;
    const auto status = c_.capture(p.id, e.result.get(), &observation, limits);
    if (!Before(until, c_)) {
      result_.states[i] = NativeRunState::kUncertain;
      Failure(i, phase, NativeRunReason::kDeadline);
      stopped_ = true;
      return;
    }
    if (status == CaptureStatus::kComplete && observation.complete) {
      result_.states[i] = NativeRunState::kCaptured;
      if (i == 0) result_.schema_created = true;
      if (p.name.starts_with("session.")) {
        if (p.name.ends_with(".set_timezone")) set_[which] = true;
        if (p.name.ends_with(".client_encoding")) encoding_[which] = true;
        if (p.name.ends_with(".timezone")) zone_[which] = true;
        qualified_[which] = set_[which] && encoding_[which] && zone_[which];
      }
      if (last_[i] >= 0) {
        phase = NativeRunPhase::kCache;
        active_ = phase;
        const auto bytes = Storage(observation);
        if (!bytes || *bytes > cache_cap - cache_bytes_ || !Before(until, c_)) {
          result_.states[i] = NativeRunState::kInvalid;
          Failure(i, phase, NativeRunReason::kCache);
          stopped_ = true;
          return;
        }
        // Emit before moving the original capture into the bounded cache.
        cache_pending_ =
            std::make_unique<CacheEntry>(CacheEntry{Observation{}, session, *bytes});
        cache_pending_id_ = i;
      }
    } else if (Definitive(observation)) {
      phase = p.operation == NativeOperation::kPrepareDescribe
                  ? (calls == 1 ? NativeRunPhase::kPrepare : NativeRunPhase::kDescribe)
                  : NativeRunPhase::kSend;
      result_.states[i] = NativeRunState::kDefinitiveError;
      Failure(i, phase, NativeRunReason::kSqlError, &observation);
      stopped_ |= Stops(p.definitive_error, true);
    } else {
      result_.states[i] =
          observation.present ? NativeRunState::kDiagnostic : NativeRunState::kInvalid;
      Failure(i, phase,
              observation.result_status == PGRES_FATAL_ERROR ? NativeRunReason::kTransport
                                                             : NativeRunReason::kCapture,
              &observation);
      stopped_ = true;
    }
  }
  void Emit(const NativeStepPlan& p, Observation& observation, NativeRunPhase phase,
            std::optional<int> terminal, int calls, std::optional<size_t> blocked) {
    const size_t i = static_cast<size_t>(p.id), which = static_cast<size_t>(p.connection);
    active_ = NativeRunPhase::kOutput;
    std::string json =
        "{\"version\":1,\"kind\":\"adbc-native-step\",\"step\":" + std::to_string(i) +
        ",\"name\":\"" + p.name + "\",\"session\":" + std::to_string(which) +
        ",\"context_qualified\":" + (qualified_[which] ? "true" : "false") +
        ",\"state\":" + std::to_string(static_cast<int>(result_.states[i])) +
        ",\"phase\":" + std::to_string(static_cast<int>(phase)) + ",\"reason\":" +
        (reasons_[i] ? std::to_string(static_cast<int>(*reasons_[i])) : "null") +
        ",\"calls\":" + std::to_string(calls) +
        ",\"blocked_by\":" + (blocked ? std::to_string(*blocked) : "null") +
        ",\"observed_terminal\":" + (terminal ? std::to_string(*terminal) : "null") +
        ",\"observation\":" + (observation.present ? observation.json : "null") + "}";
    if (output_.Record(static_cast<size_t>(p.output_group - 1), json))
      ++result_.records;
    else {
      Failure(i, NativeRunPhase::kOutput, NativeRunReason::kOutput);
      stopped_ = true;
    }
    if (cache_pending_) {
      cache_pending_->value = std::move(observation);
      cache_bytes_ += cache_pending_->bytes;
      result_.cache_peak_bytes = std::max(result_.cache_peak_bytes, cache_bytes_);
      cache_[cache_pending_id_] = std::move(cache_pending_);
    }
    if (!Before(input_.discovery_deadline, c_) && i + 1 < count) {
      Failure(i, NativeRunPhase::kOutput, NativeRunReason::kDeadline);
      stopped_ = true;
    }
  }
  NativeRunResult Finish() noexcept {
    for (size_t i = 0; i < count; ++i) Release(i);
    cache_pending_.reset();
    result_.cache_remaining_bytes = cache_bytes_;
    try {
      if (result_.records == count) {
        const auto first = result_.first_failure;
        std::string control =
            "{\"version\":1,\"kind\":\"adbc-native-accounting\",\"step_records\":715,"
            "\"calls\":" +
            std::to_string(result_.operation_calls) +
            ",\"discovery_finished\":" + (result_.discovery_finished ? "true" : "false") +
            ",\"schema_created\":" + (result_.schema_created ? "true" : "false") +
            ",\"cleanup_captured\":" + (result_.cleanup_captured ? "true" : "false") +
            ",\"first_failure_step\":" + (first ? std::to_string(first->step) : "null") +
            ",\"first_failure_phase\":" +
            (first ? std::to_string(static_cast<int>(first->phase)) : "null") +
            ",\"first_failure_reason\":" +
            (first ? std::to_string(static_cast<int>(first->reason)) : "null") +
            ",\"requires_successful_return_and_reap\":true}";
        result_.output_complete = output_.Record(7, control) && output_.Complete();
        if (!result_.output_complete)
          Failure(current_, NativeRunPhase::kOutput, NativeRunReason::kOutput);
      }
    } catch (...) {
      Failure(current_, NativeRunPhase::kOutput, NativeRunReason::kAllocation);
    }
    result_.output_bytes = output_.bytes();
    return std::move(result_);
  }
  const NativeRunInput& input_;
  const Calls& c_;
  Output output_;
  NativeRunResult result_;
  std::array<int, count> last_{};
  std::array<std::unique_ptr<CacheEntry>, count> cache_{};
  std::unique_ptr<CacheEntry> cache_pending_;
  size_t cache_pending_id_ = 0, current_ = 0;
  uint64_t cache_bytes_ = 0;
  bool stopped_ = false;
  NativeRunPhase active_ = NativeRunPhase::kPreflight;
  std::array<std::optional<NativeRunReason>, count> reasons_{};
  std::array<bool, 2> set_{}, encoding_{}, zone_{}, qualified_{};
};
NativeRunResult RunWith(const NativeRunInput& input, const Calls& c) noexcept {
  return Runner(input, c).Run();
}
}  // namespace
NativeRunResult RunNativeDiscovery(const NativeRunInput& input) noexcept {
  const auto calls = RealCalls();
  return RunWith(input, calls);
}
}  // namespace adbc::qualification
