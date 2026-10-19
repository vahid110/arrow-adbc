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

// The only injection is inside this test translation unit. No fake connection is
// passed to a real libpq accessor, and no socket/connect/execute API is called.
#include "native_protocol_runner.cc"  // NOLINT(build/include)

#include <gtest/gtest.h>
#include <algorithm>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace adbc::qualification {
namespace {
constexpr const char* owned_schema = "adbc_types_0123456789abcdef0123456789abcdef";
NativeStepPlan Plan(std::string_view name) {
  const auto id = FindNativeStep(name);
  if (!id) throw std::invalid_argument("unknown test step");
  return *BuildNativeStep(*id, owned_schema);
}
size_t Index(std::string_view name) { return static_cast<size_t>(Plan(name).id); }
struct Files {
  std::string directory;
  std::array<int, 8> fds{};
  Files() {
    char pattern[] = "/tmp/adbc-runner-XXXXXX";
    const auto path = mkdtemp(pattern);
    if (!path) throw std::bad_alloc();
    directory = path;
    fds.fill(-1);
    for (size_t i = 0; i < 8; ++i) {
      fds[i] = open((directory + "/" + std::to_string(i)).c_str(),
                    O_RDWR | O_CREAT | O_EXCL, 0600);
      if (fds[i] < 0) throw std::bad_alloc();
    }
  }
  ~Files() {
    for (size_t i = 0; i < 8; ++i) {
      if (fds[i] >= 0) close(fds[i]);
      unlink((directory + "/" + std::to_string(i)).c_str());
    }
    rmdir(directory.c_str());
  }
  std::string Read(size_t group) const {
    std::ifstream file(directory + "/" + std::to_string(group));
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }
};
// Synthetic command/SQLSTATE capture only: public PGresult builders cannot set a
// command tag or error fields. Row/describe bodies use actual PGresult + H0.
struct Fake {
  static Fake* current;
  Time time = Time() + std::chrono::seconds(1);
  int token[2] = {};
  std::array<bool, 2> live{true, true}, encoding{true, true};
  std::vector<std::pair<size_t, SendKind>> sent;
  std::map<size_t, std::string> errors;
  std::function<void(std::string_view)> hook;
  NativeStepPlan active = Plan("setup.schema");
  SendKind kind = SendKind::kExecute;
  PGresult* pending = nullptr;
  const RawParameter* parameter = nullptr;
  std::optional<RawParameterView> parameter_view;
  bool bad_prepare_shape = false, missing_result = false, extra_result = false;
  bool busy_forever = false, flush_forever = false, send_fail = false;
  bool write_fail = false, sync_fail = false, one_eintr = false;
  bool wrong_case = false, large_cache = false;
  int result_status = -1, gets = 0, short_write = 0;
  size_t parameter_sends = 0, null_sends = 0, empty_sends = 0;
  Fake() { current = this; }
  ~Fake() {
    if (pending) PQclear(pending);
    current = nullptr;
  }
  void Event(std::string_view name) {
    if (parameter && parameter_view) {
      const auto now = parameter->Get();
      EXPECT_TRUE(now);
      if (now) {
        EXPECT_EQ(now->value, parameter_view->value);
        EXPECT_EQ(now->type, parameter_view->type);
        EXPECT_EQ(now->length, parameter_view->length);
        EXPECT_EQ(now->format, parameter_view->format);
      }
    }
    if (hook) hook(name);
  }
  PGconn* Session(size_t i) { return reinterpret_cast<PGconn*>(&token[i]); }
  size_t Which(PGconn* c) { return c == Session(0) ? 0 : 1; }
  NativeRunInput Input(const Files& files) {
    return {{Session(0), Session(1)},
            files.fds,
            owned_schema,
            owned_schema,
            time + std::chrono::seconds(720),
            time + std::chrono::seconds(900)};
  }
  PGresult* Make() {
    const bool error = errors.count(static_cast<size_t>(active.id));
    ExecStatusType status = error ? PGRES_FATAL_ERROR
                            : kind == SendKind::kPrepare ||
                                    active.operation == NativeOperation::kCommand ||
                                    kind == SendKind::kDescribe
                                ? PGRES_COMMAND_OK
                                : PGRES_TUPLES_OK;
    if (result_status >= 0) status = static_cast<ExecStatusType>(result_status);
    auto* r = PQmakeEmptyPGresult(nullptr, status);
    if (!r) throw std::bad_alloc();
    if (error || (kind == SendKind::kPrepare && !bad_prepare_shape) ||
        active.operation == NativeOperation::kCommand)
      return r;
    auto names = active.columns;
    if (kind == SendKind::kPrepare) names = {"unexpected"};
    if (!active.exact_column_names) names = {"TimeZone"};
    std::vector<PGresAttDesc> attrs(names.size());
    for (size_t i = 0; i < names.size(); ++i) {
      attrs[i].name = names[i].data();
      attrs[i].typid = static_cast<Oid>(8000 + i);
      attrs[i].atttypmod = -1;
      attrs[i].format = active.requested_format.value_or(0);
    }
    EXPECT_TRUE(PQsetResultAttrs(r, static_cast<int>(attrs.size()), attrs.data()));
    if (kind == SendKind::kDescribe || kind == SendKind::kPrepare) return r;
    int rows = active.max_rows > 0 ? 1 : 0;
    if (active.name == "vendor_mixed.values.text" ||
        active.name == "vendor_mixed.values.binary")
      rows = 2;
    if (active.name.starts_with("super_cases.") &&
        (active.name.ends_with(".text") || active.name.ends_with(".binary")))
      rows = 8;
    for (int row = 0; row < rows; ++row)
      for (size_t col = 0; col < names.size(); ++col) {
        const int first = active.name.starts_with("super_cases.9.") ? 9 : 1;
        std::string value = "x";
        if (col == 0 && names[col] == "case_id") {
          if (active.requested_format == 1) {
            value.assign(8, 0);
            value[7] = static_cast<char>(first + row);
          } else
            value = std::to_string(first + row);
        }
        if (!active.required_context_value.empty()) value = active.required_context_value;
        if (wrong_case && col == 0 && names[col] == "case_id") value = "99";
        const bool null =
            active.name.starts_with("super_cases.1.") && row == 0 && col == 1;
        EXPECT_TRUE(PQsetvalue(r, row, static_cast<int>(col),
                               null ? nullptr : value.data(),
                               null ? 0 : static_cast<int>(value.size())));
      }
    return r;
  }
  static Calls Api() {
    return {
        [] { return current->time; },
        [](PGconn* c) {
          auto& f = *current;
          f.Event("ready");
          return f.live[f.Which(c)] && !f.busy_forever;
        },
        [](PGconn* c) {
          auto& f = *current;
          f.Event("utf8");
          return f.encoding[f.Which(c)];
        },
        [](PGconn*, const NativeStepPlan& p, SendKind k, const RawParameter* owner) {
          auto& f = *current;
          f.active = p;
          f.kind = k;
          f.gets = 0;
          f.parameter = owner;
          f.parameter_view = owner ? owner->Get() : std::nullopt;
          if (owner) {
            ++f.parameter_sends;
            EXPECT_TRUE(f.parameter_view);
            if (f.parameter_view) {
              if (!f.parameter_view->value)
                ++f.null_sends;
              else if (!f.parameter_view->length)
                ++f.empty_sends;
              EXPECT_EQ(f.parameter_view->format, p.parameter->format);
            }
          }
          f.sent.emplace_back(static_cast<size_t>(p.id), k);
          f.Event("send");
          if (f.send_fail) return 0;
          f.pending = f.Make();
          return 1;
        },
        [](PGconn*) {
          current->Event("flush");
          return current->flush_forever ? 1 : 0;
        },
        [](PGconn*) {
          current->Event("busy");
          return current->busy_forever ? 1 : 0;
        },
        [](PGconn*) {
          current->Event("consume");
          return 1;
        },
        [](PGconn*) {
          auto& f = *current;
          ++f.gets;
          PGresult* r = nullptr;
          if (!f.missing_result) {
            r = f.pending;
            f.pending = nullptr;
            if (!r && f.extra_result && f.gets == 2) r = f.Make();
          }
          f.Event("get");
          // Owner remains valid through the final NULL result, then libpq no
          // longer retains it. In-flight failure intentionally leaves it live.
          if (!r && !f.missing_result) {
            f.parameter = nullptr;
            f.parameter_view.reset();
          }
          return r;
        },
        [](PGconn*, int16_t, int ms) {
          current->Event("wait");
          current->time += std::chrono::milliseconds(ms);
          return 1;
        },
        [](NativeStepId id, const PGresult* r, Observation* out, CaptureLimits limits) {
          auto& f = *current;
          f.Event("capture");
          const auto p = *BuildNativeStep(id, owned_schema);
          if (f.errors.count(static_cast<size_t>(id))) {
            *out = Observation{};
            out->present = true;
            out->result_status = PGRES_FATAL_ERROR;
            out->sqlstate = f.errors.at(static_cast<size_t>(id));
            out->json = "{\"synthetic_sqlstate\":\"" + *out->sqlstate + "\"}";
            return CaptureStatus::kResultStatus;
          }
          if (p.operation == NativeOperation::kCommand) {
            *out = Observation{};
            out->present = true;
            out->complete = true;
            out->result_status = PGRES_COMMAND_OK;
            out->affected_rows = p.affected_rows;
            out->json = "{\"synthetic_command\":true}";
            return CaptureStatus::kComplete;
          }
          // Public builders cannot set nparams; this one synthetic descriptor
          // fact is isolated from actual row/field capture.
          if (p.expected_parameters == 1) {
            *out = Observation{};
            out->present = true;
            out->complete = true;
            out->result_status = PGRES_COMMAND_OK;
            out->parameters = std::vector<uint32_t>{8001};
            out->json = "{\"synthetic_parameter_descriptor\":true}";
            return CaptureStatus::kComplete;
          }
          const auto status = CaptureNativeResult(id, r, out, limits);
          if (f.large_cache && p.name == "vendor_mixed.values.text")
            out->json.reserve(cache_cap);
          return status;
        },
        [](int fd, const void* data, size_t size) -> ssize_t {
          auto& f = *current;
          f.Event("write");
          if (f.one_eintr) {
            f.one_eintr = false;
            errno = EINTR;
            return -1;
          }
          if (f.write_fail) {
            errno = EIO;
            return -1;
          }
          return ::write(
              fd, data,
              f.short_write ? std::min(size, static_cast<size_t>(f.short_write)) : size);
        },
        [](int fd) {
          current->Event("sync");
          if (current->sync_fail) {
            errno = EIO;
            return -1;
          }
          return ::fsync(fd);
        }};
  }
};
Fake* Fake::current = nullptr;

TEST(NativeRunnerTest, FullFixedRunHas715Records833CallsBoundedCacheAndContexts) {
  Files files;
  Fake fake;
  auto input = fake.Input(files);
  auto result = RunWith(input, Fake::Api());
  ASSERT_FALSE(result.first_failure)
      << (result.first_failure ? result.first_failure->step : 0);
  EXPECT_TRUE(result.discovery_finished);
  EXPECT_TRUE(result.output_complete);
  EXPECT_TRUE(result.spent);
  EXPECT_TRUE(result.schema_created);
  EXPECT_TRUE(result.cleanup_attempted);
  EXPECT_TRUE(result.cleanup_captured);
  EXPECT_EQ(result.records, 715U);
  EXPECT_EQ(result.operation_calls, 833U);
  EXPECT_EQ(result.attempted_steps, 715U);
  EXPECT_EQ(fake.sent.size(), 833U);
  EXPECT_GT(result.cache_peak_bytes, 0U);
  EXPECT_LE(result.cache_peak_bytes, cache_cap);
  EXPECT_EQ(result.cache_remaining_bytes, 0U);
  EXPECT_GT(fake.parameter_sends, 0U);
  EXPECT_GT(fake.null_sends, 0U);
  EXPECT_GT(fake.empty_sends, 0U);
  for (auto state : result.states) EXPECT_EQ(state, NativeRunState::kCaptured);
  for (const auto& owner : result.pending_parameters) EXPECT_FALSE(owner);
  std::string joined;
  for (size_t i = 0; i < 8; ++i) joined += files.Read(i);
  EXPECT_NE(
      joined.find(
          "\"name\":\"bare_aliases.text\",\"session\":0,\"context_qualified\":false"),
      std::string::npos);
  EXPECT_NE(joined.find("\"name\":\"vendor_mixed.values.text\",\"session\":0,\"context_"
                        "qualified\":true"),
            std::string::npos);
  EXPECT_NE(joined.find("\"requires_successful_return_and_reap\":true"),
            std::string::npos);
  EXPECT_EQ(std::count(joined.begin(), joined.end(), '\n'), 716);
}
TEST(NativeRunnerTest, EveryClosedPolicyAndDependencyHasStrictUncertainSemantics) {
  size_t strict = 0, continue_count = 0, cleanup = 0, calls = 0;
  for (size_t i = 0; i < count; ++i) {
    const auto p = *BuildNativeStep(static_cast<NativeStepId>(i), owned_schema);
    SCOPED_TRACE(p.name);
    calls += p.protocol_calls;
    EXPECT_TRUE(Stops(p.definitive_error, false));
    EXPECT_EQ(Stops(p.definitive_error, true),
              p.definitive_error != NativeErrorPolicy::kContinueDefinitive);
    if (p.definitive_error == NativeErrorPolicy::kStopRequired) ++strict;
    if (p.definitive_error == NativeErrorPolicy::kContinueDefinitive) ++continue_count;
    if (p.definitive_error == NativeErrorPolicy::kCleanupUnknown) ++cleanup;
    EXPECT_EQ(p.uncertain, NativeUncertainPolicy::kStopSpentNoRetry);
    for (auto prerequisite : p.prerequisites)
      EXPECT_LT(static_cast<size_t>(prerequisite), i);
  }
  EXPECT_EQ(strict, 40U);
  EXPECT_EQ(continue_count, 674U);
  EXPECT_EQ(cleanup, 1U);
  EXPECT_EQ(calls, 833U);
  Observation error;
  error.result_status = PGRES_FATAL_ERROR;
  for (const auto& state : {"22012", "42704", "42P07", "0A000", "XX000"}) {
    error.sqlstate = state;
    EXPECT_TRUE(Definitive(error));
  }
  for (const auto& state :
       {"08006", "57014", "57P01", "57P02", "57P03", "bad!!", "", "2201", "220120"}) {
    error.sqlstate = state;
    EXPECT_FALSE(Definitive(error));
  }
  error.sqlstate.reset();
  EXPECT_FALSE(Definitive(error));
}
TEST(NativeRunnerTest, Every715FaultUsesActualPrerequisiteAndStopFunctions) {
  std::vector<NativeStepPlan> plans;
  for (size_t i = 0; i < count; ++i)
    plans.push_back(*BuildNativeStep(static_cast<NativeStepId>(i), owned_schema));
  size_t scenarios = 0;
  for (size_t failed = 0; failed < count; ++failed)
    for (bool definitive : {false, true}) {
      std::array<NativeRunState, count> states;
      states.fill(NativeRunState::kCaptured);
      states[failed] =
          definitive ? NativeRunState::kDefinitiveError : NativeRunState::kUncertain;
      const bool stop = Stops(plans[failed].definitive_error, definitive);
      std::set<size_t> descendants;
      // Independent graph traversal computes the exact transitive failure cone.
      for (size_t i = failed + 1; i < count - 1; ++i)
        for (auto prerequisite : plans[i].prerequisites)
          if (static_cast<size_t>(prerequisite) == failed ||
              descendants.count(static_cast<size_t>(prerequisite)))
            descendants.insert(i);
      for (size_t i = failed + 1; i < count - 1; ++i) {
        const auto blocked = Blocked(plans[i], states);
        if (stop || blocked) states[i] = NativeRunState::kSkipped;
        EXPECT_EQ(states[i] == NativeRunState::kSkipped,
                  stop || descendants.count(i) != 0)
            << failed << ":" << i << ":" << definitive;
        if (blocked) EXPECT_NE(states[*blocked], NativeRunState::kCaptured);
      }
      ++scenarios;
    }
  EXPECT_EQ(scenarios, 1430U);
}
TEST(NativeRunnerTest,
     DefinitiveCandidateSkipsOnlyItsDescendantsAndAbsentBaselineSendsNothing) {
  Files files;
  Fake fake;
  const auto failed = Index("vendor_mixed.values.text");
  fake.errors[failed] = "0A000";
  auto result = RunWith(fake.Input(files), Fake::Api());
  ASSERT_TRUE(result.first_failure);
  EXPECT_EQ(result.first_failure->step, failed);
  EXPECT_EQ(result.states[failed], NativeRunState::kDefinitiveError);
  EXPECT_TRUE(result.discovery_finished);
  EXPECT_TRUE(result.cleanup_captured);
  EXPECT_EQ(result.states[Index("vendor_mixed.values.binary")],
            NativeRunState::kCaptured);
  size_t skipped = 0;
  for (size_t i = 0; i < count; ++i)
    if (result.states[i] == NativeRunState::kSkipped) ++skipped;
  EXPECT_EQ(skipped, 100U);  // exact compiled descendants of this baseline.
  for (const auto& [id, kind] : fake.sent) {
    const auto p = *BuildNativeStep(static_cast<NativeStepId>(id), owned_schema);
    if (p.parameter) EXPECT_NE(static_cast<size_t>(p.parameter->source), failed);
  }
}
TEST(NativeRunnerTest,
     RequiredFailureAndUncertainCandidateStopWhileCleanupPreservesFirst) {
  for (const auto& name : {"setup.vendor_mixed", "session.raw_utc.timezone",
                           "vendor_mixed.values.text", "catalog.after"}) {
    SCOPED_TRACE(name);
    Files files;
    Fake fake;
    const auto failed = Index(name);
    fake.errors[failed] = std::string(name).starts_with("setup.") ? "42704" : "57014";
    fake.errors[count - 1] = "XX000";
    auto result = RunWith(fake.Input(files), Fake::Api());
    ASSERT_TRUE(result.first_failure);
    EXPECT_EQ(result.first_failure->step, failed);
    EXPECT_FALSE(result.discovery_finished);
    EXPECT_TRUE(result.cleanup_attempted);
    EXPECT_FALSE(result.cleanup_captured);
    for (auto [id, kind] : fake.sent) EXPECT_TRUE(id <= failed || id == count - 1);
  }
}
TEST(NativeRunnerTest, DuplicateAndUncertainCreateNeverOwnOrDrop) {
  for (bool uncertain : {false, true}) {
    Files files;
    Fake fake;
    if (uncertain)
      fake.send_fail = true;
    else
      fake.errors[0] = "42P06";
    auto result = RunWith(fake.Input(files), Fake::Api());
    EXPECT_FALSE(result.schema_created);
    EXPECT_FALSE(result.cleanup_attempted);
    EXPECT_FALSE(result.cleanup_captured);
    EXPECT_EQ(result.operation_calls, 1U);
    EXPECT_EQ(fake.sent.size(), 1U);
    EXPECT_TRUE(result.spent);
  }
}
TEST(NativeRunnerTest, ExactlyFiveHllSetupStepsHaveTenSeconds) {
  Files files;
  Fake fake;
  std::set<size_t> extended;
  fake.hook = [&](std::string_view event) {
    const auto& name = fake.active.name;
    if (event == "get" && fake.gets == 1 &&
        (name == "setup.hll_input.values" || name == "setup.hll_cases.1" ||
         name == "setup.hll_cases.2" || name == "setup.hll_cases.3" ||
         name == "setup.hll_cases.4")) {
      extended.insert(static_cast<size_t>(fake.active.id));
      fake.time += std::chrono::seconds(9);
    }
  };
  auto result = RunWith(fake.Input(files), Fake::Api());
  EXPECT_EQ(extended.size(), 5U);
  EXPECT_TRUE(result.discovery_finished);
  EXPECT_TRUE(result.cleanup_captured);
  EXPECT_FALSE(result.first_failure);
  EXPECT_EQ(result.operation_calls, 833U);
}
TEST(NativeRunnerTest, CacheBoundAndWrongActualSourceCaseRejectBeforeParameterSend) {
  for (bool large : {false, true}) {
    Files files;
    Fake fake;
    fake.large_cache = large;
    fake.wrong_case = !large;
    auto result = RunWith(fake.Input(files), Fake::Api());
    ASSERT_TRUE(result.first_failure);
    EXPECT_FALSE(result.discovery_finished);
    EXPECT_EQ(fake.parameter_sends, 0U);
    EXPECT_TRUE(result.cleanup_captured);
    EXPECT_EQ(result.cache_remaining_bytes, 0U);
    EXPECT_EQ(result.first_failure->reason,
              large ? NativeRunReason::kCache : NativeRunReason::kParameter);
  }
}
TEST(NativeRunnerTest, PairHasOneDeadlineAndCountsOnlyActuallyAttemptedOperations) {
  const auto target = Index("vendor_mixed.values.describe");
  for (int mode = 0; mode < 4; ++mode) {
    Files files;
    Fake fake;
    fake.hook = [&](std::string_view event) {
      if (static_cast<size_t>(fake.active.id) != target) return;
      if (mode == 0) fake.errors[target] = "0A000";
      if (mode == 1 && event == "get" && fake.kind == SendKind::kPrepare &&
          fake.gets == 2)
        fake.time += std::chrono::seconds(3);
      if (mode == 2 && event == "send" && fake.kind == SendKind::kPrepare)
        fake.bad_prepare_shape = true;
      if (mode == 3 && event == "send" && fake.kind == SendKind::kDescribe)
        fake.send_fail = true;
    };
    auto result = RunWith(fake.Input(files), Fake::Api());
    size_t calls = 0;
    for (auto [id, kind] : fake.sent)
      if (id == target) ++calls;
    EXPECT_EQ(calls, mode == 3 ? 2U : 1U);
    EXPECT_NE(result.states[target], NativeRunState::kCaptured);
    if (mode != 0) EXPECT_FALSE(result.discovery_finished);
  }
}
TEST(NativeRunnerTest, InflightParameterOwnerSurvivesReturnUntilBorrowedSessionDiscard) {
  Files files;
  Fake fake;
  size_t target = count;
  for (size_t i = 0; i < count; ++i)
    if (BuildNativeStep(static_cast<NativeStepId>(i), owned_schema)->parameter) {
      target = i;
      break;
    }
  fake.hook = [&](std::string_view event) {
    if (event == "send" && static_cast<size_t>(fake.active.id) == target)
      fake.busy_forever = true;
  };
  auto result = RunWith(fake.Input(files), Fake::Api());
  ASSERT_LT(target, count);
  ASSERT_TRUE(result.first_failure);
  EXPECT_EQ(result.first_failure->step, target);
  EXPECT_FALSE(result.discovery_finished);
  ASSERT_TRUE(result.pending_parameters[0]);
  auto view = result.pending_parameters[0]->Get();
  ASSERT_TRUE(view);
  EXPECT_EQ(view->value, fake.parameter_view->value);
  fake.live[0] = false;
  fake.live[1] = false;  // Root teardown ordering, no real PGconn.
  fake.parameter = nullptr;
  fake.parameter_view.reset();
  result.pending_parameters[0].reset();
}
TEST(NativeRunnerTest, AllProgressPhasesEnforceFreshDeadlinesAndRetainLateTerminal) {
  for (const auto& event : {"ready", "utf8", "send", "flush", "busy", "get", "capture"}) {
    Files files;
    Fake fake;
    bool advanced = false;
    fake.hook = [&](std::string_view where) {
      if (!advanced && where == event) {
        fake.time += std::chrono::seconds(721);
        advanced = true;
      }
    };
    auto result = RunWith(fake.Input(files), Fake::Api());
    ASSERT_TRUE(result.first_failure);
    EXPECT_EQ(result.first_failure->reason, NativeRunReason::kDeadline);
    EXPECT_FALSE(result.discovery_finished);
    EXPECT_FALSE(result.schema_created);
    EXPECT_LE(result.operation_calls, 1U);
    if (std::string_view(event) == "get") {
      const auto text = files.Read(0);
      EXPECT_NE(text.find("\"observed_terminal\":1"), std::string::npos);
    }
  }
}
TEST(NativeRunnerTest, LateOutputCannotStartNextSqlButCleanupReserveRemains) {
  Files files;
  Fake fake;
  bool advanced = false;
  fake.hook = [&](std::string_view event) {
    if (event == "write" && !advanced) {
      fake.time += std::chrono::seconds(721);
      advanced = true;
    }
  };
  auto result = RunWith(fake.Input(files), Fake::Api());
  EXPECT_TRUE(result.schema_created);
  EXPECT_TRUE(result.cleanup_captured);
  EXPECT_EQ(result.operation_calls, 2U);
  EXPECT_FALSE(result.discovery_finished);
  ASSERT_TRUE(result.first_failure);
  EXPECT_EQ(result.first_failure->reason, NativeRunReason::kDeadline);
}
TEST(NativeRunnerTest,
     PartialFlushNoFinalResultAndUnfamiliarStatusNeverBecomeDefinitive) {
  for (int mode = 0; mode < 5; ++mode) {
    Files files;
    Fake fake;
    if (mode == 0) fake.flush_forever = true;
    if (mode == 1) fake.busy_forever = true;
    if (mode == 2) fake.missing_result = true;
    if (mode == 3) fake.extra_result = true;
    if (mode == 4) fake.result_status = PGRES_COPY_OUT;
    auto result = RunWith(fake.Input(files), Fake::Api());
    EXPECT_FALSE(result.schema_created);
    EXPECT_FALSE(result.cleanup_attempted);
    EXPECT_FALSE(result.discovery_finished);
    EXPECT_EQ(result.operation_calls, mode == 1 ? 0U : 1U);
    ASSERT_TRUE(result.first_failure);
    EXPECT_NE(result.states[0], NativeRunState::kDefinitiveError);
    EXPECT_TRUE(result.spent);
  }
}
TEST(NativeRunnerTest,
     EncodingChangeAfterCompletedActionPreventsInterpretationAndFutureSend) {
  Files files;
  Fake fake;
  bool changed = false;
  fake.hook = [&](std::string_view event) {
    if (event == "get" && !changed) {
      fake.encoding[0] = false;
      changed = true;
    }
  };
  auto result = RunWith(fake.Input(files), Fake::Api());
  ASSERT_TRUE(result.first_failure);
  EXPECT_EQ(result.first_failure->reason, NativeRunReason::kEncoding);
  EXPECT_EQ(result.operation_calls, 1U);
  EXPECT_FALSE(result.schema_created);
  EXPECT_FALSE(result.cleanup_attempted);  // Successful action != validated owned CREATE.
}
TEST(NativeRunnerTest, LostConnectionAfterOwnedCreateLeavesCleanupUnknown) {
  Files files;
  Fake fake;
  const auto target = Index("vendor_mixed.values.text");
  fake.hook = [&](std::string_view event) {
    if (event == "send" && static_cast<size_t>(fake.active.id) == target) {
      fake.live[0] = false;
      fake.send_fail = true;
    }
  };
  auto result = RunWith(fake.Input(files), Fake::Api());
  EXPECT_TRUE(result.schema_created);
  EXPECT_FALSE(result.cleanup_attempted);
  EXPECT_FALSE(result.cleanup_captured);
  ASSERT_TRUE(result.first_failure);
  EXPECT_EQ(result.first_failure->step, target);
  EXPECT_TRUE(result.spent);
}
TEST(NativeRunnerTest, PairDoesNotRefreshBudgetAfterSuccessfulPrepare) {
  Files files;
  Fake fake;
  const auto target = Index("vendor_mixed.values.describe");
  fake.hook = [&](std::string_view event) {
    if (event == "get" && static_cast<size_t>(fake.active.id) == target && fake.gets == 1)
      fake.time +=
          std::chrono::milliseconds(fake.kind == SendKind::kPrepare ? 2000 : 1000);
  };
  auto result = RunWith(fake.Input(files), Fake::Api());
  ASSERT_TRUE(result.first_failure);
  EXPECT_EQ(result.first_failure->step, target);
  EXPECT_EQ(result.first_failure->phase, NativeRunPhase::kRead);
  EXPECT_EQ(result.first_failure->reason, NativeRunReason::kDeadline);
  size_t calls = 0;
  for (auto [id, kind] : fake.sent)
    if (id == target) ++calls;
  EXPECT_EQ(calls, 2U);
  EXPECT_EQ(result.states[target], NativeRunState::kUncertain);
}
TEST(NativeRunnerTest,
     CaptureAllocationFailureStillUsesIndependentCleanupAndStaticPhase) {
  Files files;
  Fake fake;
  const auto target = Index("vendor_mixed.values.text");
  fake.hook = [&](std::string_view event) {
    if (event == "capture" && static_cast<size_t>(fake.active.id) == target)
      throw std::bad_alloc();
  };
  auto result = RunWith(fake.Input(files), Fake::Api());
  ASSERT_TRUE(result.first_failure);
  EXPECT_EQ(result.first_failure->step, target);
  EXPECT_EQ(result.first_failure->phase, NativeRunPhase::kCapture);
  EXPECT_EQ(result.first_failure->reason, NativeRunReason::kAllocation);
  EXPECT_TRUE(result.cleanup_captured);
  EXPECT_FALSE(result.output_complete);
}
TEST(NativeRunnerTest, WrongEncodingAndInvalidOwnershipOrDescriptorArePreSqlFailures) {
  for (int mode = 0; mode < 6; ++mode) {
    Files files;
    Fake fake;
    auto input = fake.Input(files);
    if (mode == 0) fake.encoding[0] = false;
    if (mode == 1)
      input.authenticated_cleanup_schema = "adbc_types_ffffffffffffffffffffffffffffffff";
    if (mode == 2) input.sessions[1] = input.sessions[0];
    if (mode == 3) input.outputs[1] = input.outputs[0];
    if (mode == 4) ASSERT_EQ(::write(input.outputs[0], "x", 1), 1);
    if (mode == 5) ASSERT_EQ(fcntl(input.outputs[0], F_SETFL, O_APPEND), 0);
    auto result = RunWith(input, Fake::Api());
    EXPECT_TRUE(result.first_failure);
    EXPECT_TRUE(fake.sent.empty());
    EXPECT_FALSE(result.output_complete);
  }
}
TEST(NativeRunnerTest, PartialWritesEintrAndFinalSyncFailureAreSticky) {
  {
    Files files;
    Fake fake;
    fake.one_eintr = true;
    fake.short_write = 17;
    auto result = RunWith(fake.Input(files), Fake::Api());
    EXPECT_TRUE(result.output_complete);
    EXPECT_FALSE(result.first_failure);
  }
  for (const auto& event : {"write", "sync"}) {
    Files files;
    Fake fake;
    bool hit = false;
    fake.hook = [&](std::string_view where) {
      if (where == event && !hit) {
        hit = true;
        if (where == "write")
          fake.write_fail = true;
        else
          fake.sync_fail = true;
      }
    };
    auto result = RunWith(fake.Input(files), Fake::Api());
    EXPECT_TRUE(result.first_failure);
    EXPECT_FALSE(result.output_complete);
    EXPECT_TRUE(result.cleanup_captured);
    EXPECT_TRUE(result.spent);
  }
}
TEST(NativeRunnerTest, ExpiredCleanupReserveAndSlowFsyncNeverComplete) {
  for (bool sync : {false, true}) {
    Files files;
    Fake fake;
    bool advanced = false;
    fake.hook = [&](std::string_view event) {
      if (!advanced && event == (sync ? "sync" : "write")) {
        fake.time += std::chrono::seconds(901);
        advanced = true;
      }
    };
    auto result = RunWith(fake.Input(files), Fake::Api());
    EXPECT_FALSE(result.output_complete);
    EXPECT_TRUE(result.first_failure);
    EXPECT_LE(result.operation_calls, 833U);
    if (!sync) {
      EXPECT_EQ(result.operation_calls, 1U);
      EXPECT_FALSE(result.cleanup_attempted);
    }
    // Borrowed files remain open even after an output failure.
    for (int fd : files.fds) EXPECT_GE(fcntl(fd, F_GETFL), 0);
  }
}
TEST(NativeRunnerTest, AllEightGroupAndTotalBytesIncludeFramingAtExactBoundary) {
  Files files;
  Fake fake;
  auto input = fake.Input(files);
  auto calls = Fake::Api();
  Output output(input, calls);
  ASSERT_TRUE(output.Initialize());
  std::string chunk(group_cap / 2 - 1, 'x');
  chunk.front() = '{';
  chunk.back() = '}';
  for (size_t group = 0; group < 8; ++group) {
    ASSERT_TRUE(output.Record(group, chunk));
    ASSERT_TRUE(output.Record(group, chunk));
    EXPECT_EQ(output.bytes()[group], group_cap);
  }
  uint64_t total = 0;
  for (auto n : output.bytes()) total += n;
  EXPECT_EQ(total, total_cap);
  EXPECT_FALSE(output.Record(7, "{}"));
  EXPECT_FALSE(output.Complete());
}
TEST(NativeRunnerTest, RecordGroupAggregateCacheLimitsAndStaleOffsetsReject) {
  {
    Files files;
    Fake fake;
    auto input = fake.Input(files);
    auto calls = Fake::Api();
    Output output(input, calls);
    ASSERT_TRUE(output.Initialize());
    std::string largest(record_cap - 1, 'x');
    largest.front() = '{';
    largest.back() = '}';
    EXPECT_TRUE(output.Record(0, largest));
    EXPECT_FALSE(output.Record(0, largest));  // group cap counts LF.
    EXPECT_FALSE(output.Record(1, "{}"));     // sticky, no new group escapes.
  }
  for (int mode = 0; mode < 4; ++mode) {
    Files files;
    Fake fake;
    auto input = fake.Input(files);
    auto calls = Fake::Api();
    Output output(input, calls);
    ASSERT_TRUE(output.Initialize());
    std::string value = mode == 0 ? std::string(record_cap, 'x') : "{}";
    if (mode == 0) {
      value.front() = '{';
      value.back() = '}';
    }
    if (mode == 1) value = "{\n}";
    if (mode == 2) ASSERT_EQ(lseek(input.outputs[0], 1, SEEK_SET), 1);
    if (mode == 3) ASSERT_EQ(fchmod(input.outputs[0], 0644), 0);
    EXPECT_FALSE(output.Record(0, value));
    EXPECT_FALSE(output.Complete());
  }
  Observation huge;
  huge.json.reserve(cache_cap);
  EXPECT_FALSE(Storage(huge));
  Observation small;
  small.json = "{}";
  ASSERT_TRUE(Storage(small));
  EXPECT_LT(*Storage(small), cache_cap);
  uint64_t total;
  EXPECT_FALSE(detail::CheckedAdd(total_cap, 1, total_cap, &total));
}
}  // namespace
}  // namespace adbc::qualification
