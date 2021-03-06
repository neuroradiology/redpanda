/*
 * Copyright 2020 Vectorized, Inc.
 *
 * Licensed as a Redpanda Enterprise file under the Redpanda Community
 * License (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * https://github.com/vectorizedio/redpanda/blob/master/licenses/rcl.md
 */

#include "coproc/tests/utils/coproc_test_fixture.h"
#include "coproc/tests/utils/helpers.h"
#include "coproc/tests/utils/wasm_event_generator.h"
#include "coproc/wasm_event.h"
#include "coproc/wasm_event_listener.h"
#include "hashing/secure.h"
#include "model/namespace.h"
#include "model/record_batch_reader.h"
#include "model/timeout_clock.h"
#include "raft/types.h"
#include "test_utils/fixture.h"
#include "utils/unresolved_address.h"

#include <boost/test/tools/old/interface.hpp>
#include <boost/test/unit_test_log.hpp>

#include <filesystem>
#include <string>
#include <variant>
#include <vector>

class wasm_event_test_harness : public coproc_test_fixture {
private:
    struct poll_state {
        std::set<std::filesystem::path> not_found;
        std::set<std::filesystem::path> found;
        explicit poll_state(std::set<std::filesystem::path> nf)
          : not_found(std::move(nf)) {}
    };

public:
    wasm_event_test_harness()
      : coproc_test_fixture() {
        startup({{make_ts(copro_topic()), 1}}).get();
        (void)_event_listener.start();
    }

    ~wasm_event_test_harness() { _event_listener.stop().get(); }

    /// \brief Accessors for wasm_event specific data fields
    const model::topic& copro_topic() const { return _coproc_internal_topic; }
    const model::ntp& copro_ntp() const { return _coproc_internal_ntp; }
    const std::filesystem::path& submit_dir() const {
        return _event_listener.submit_dir();
    }

    /// \brief Verify current state of wasm directory hierarchy against an
    /// expected result
    ss::future<size_t> wait_for_scripts(
      std::set<std::filesystem::path> ids,
      model::timeout_clock::duration duration = 4s) {
        auto timeout = model::timeout_clock::now() + duration;
        return ss::do_with(
          poll_state(std::move(ids)), [this, timeout](poll_state& ps) {
              size_t total = ps.not_found.size();
              return ss::do_until(
                       [&ps, timeout] {
                           const auto now = model::timeout_clock::now();
                           return (ps.not_found.empty()) || (now > timeout);
                       },
                       [this, &ps] { return do_poll_files(ps); })
                .then([&ps, total] { return total - ps.not_found.size(); });
          });
    }

private:
    ss::future<> do_poll_files(poll_state& ps) {
        return ss::do_for_each(
                 ps.not_found,
                 [&ps](const std::filesystem::path& fp) {
                     return ss::file_exists(fp.string())
                       .then([&ps, fp](bool exists) {
                           if (exists) {
                               ps.found.insert(fp);
                           }
                       });
                 })
          .then([&ps] {
              for (const auto& s : ps.found) {
                  ps.not_found.erase(s);
              }
              /// ps.found could be local to this method, captured in a do_with,
              /// but to me it seemed easier and cleaner to put it within the
              /// 'poll_state' struct
              ps.found.clear();
              return ss::sleep(100ms);
          });
    }

private:
    coproc::wasm_event_listener _event_listener{
      (std::filesystem::path(data_dir) / "coprocessors")};

    model::topic _coproc_internal_topic{
      model::topic("coprocessor_internal_topic")};
    model::ntp _coproc_internal_ntp{model::ntp(
      model::kafka_namespace, _coproc_internal_topic, model::partition_id(0))};
};

std::set<ss::sstring>
deployed_ids(coproc_test_fixture::opt_reader_data_t reader) {
    using cp_errc = coproc::wasm_event_errc;
    std::set<ss::sstring> ids;
    if (!reader) {
        return ids;
    }
    for (auto& rb : *reader) {
        rb.for_each_record([&ids](model::record r) {
            if (cp_errc::none != coproc::wasm_event_validate(r)) {
                return;
            }
            auto a = coproc::wasm_event_get_action(r);
            if (auto action = std::get_if<coproc::wasm_event_action>(&a)) {
                if (*action == coproc::wasm_event_action::deploy) {
                    /// Ok to blindly deref optional, it passed validation
                    ids.insert(*coproc::wasm_event_get_name(r));
                }
            }
        });
    }
    return ids;
}

FIXTURE_TEST(test_copro_internal_topic_read, wasm_event_test_harness) {
    push(
      copro_ntp(),
      coproc::make_wasm_event_record_batch_reader(model::offset(0), 2, 2))
      .get();
    std::set<ss::sstring> events
      = drain(copro_ntp(), 2 * 2).then(&deployed_ids).get0();

    std::set<std::filesystem::path> paths;
    std::transform(
      events.cbegin(),
      events.cend(),
      std::inserter(paths, paths.begin()),
      [root_dir = submit_dir()](const ss::sstring& name) {
          return root_dir / name.c_str();
      });

    const auto n_expected = events.size();
    const auto found_all = wait_for_scripts(std::move(paths)).get0();
    BOOST_CHECK_EQUAL(found_all, n_expected);
}

FIXTURE_TEST(test_copro_internal_topic_do_undo, wasm_event_test_harness) {
    coproc::wasm_event e{
      .name = "4444-4444",
      .desc = "Simple wasm event",
      .script = "console.log('Hello World!');",
      .action = coproc::wasm_event_action::deploy};
    hash_sha256 h;
    h.update(*e.script);
    iobuf buf;
    auto checksum = h.reset();
    buf.append(checksum.data(), checksum.size());
    e.checksum = iobuf_to_bytes(buf);
    coproc::wasm_event r{
      .name = "444-4444", .action = coproc::wasm_event_action::remove};

    /// Attempts redeploying twice in a row, delete, then deploy, then finally
    /// ending on a deploy. Should expect a single coprocessor in the submit dir
    std::vector<model::record> actions;
    actions.emplace_back(create_wasm_record(e));
    actions.emplace_back(create_wasm_record(e));
    actions.emplace_back(create_wasm_record(r));
    actions.emplace_back(create_wasm_record(e));
    actions.emplace_back(create_wasm_record(r));
    actions.emplace_back(create_wasm_record(e));

    model::record_batch_reader::data_t batches;
    batches.push_back(
      coproc::make_wasm_batch(model::offset(0), std::move(actions)));
    model::record_batch_reader rbr = model::make_memory_record_batch_reader(
      std::move(batches));

    /// Push and assert
    push(copro_ntp(), std::move(rbr)).get();
    std::filesystem::path item = submit_dir() / e.name->c_str();
    const auto found_all = wait_for_scripts({{item}}).get0();
    BOOST_CHECK_EQUAL(found_all, 1);
}
