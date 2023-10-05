/*
 * Copyright 2023 Redpanda Data, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#include "transform/api.h"

#include "cluster/errc.h"
#include "cluster/plugin_frontend.h"
#include "features/feature_table.h"
#include "model/timeout_clock.h"
#include "transform/io.h"
#include "transform/logger.h"
#include "transform/rpc/client.h"

#include <seastar/coroutine/as_future.hh>

namespace transform {

namespace {
constexpr auto wasm_binary_timeout = std::chrono::seconds(3);
constexpr auto metadata_timeout = std::chrono::seconds(1);

class rpc_client_sink final : public sink {
public:
    rpc_client_sink(model::ntp ntp, rpc::client* c)
      : _ntp(std::move(ntp))
      , _client(c) {}

    ss::future<> write(ss::chunked_fifo<model::record_batch> batches) override {
        auto ec = co_await _client->produce(_ntp.tp, std::move(batches));
        if (ec != cluster::errc::success) {
            throw std::runtime_error(ss::format(
              "failure to produce transform data: {}",
              cluster::error_category().message(int(ec))));
        }
    }

private:
    model::ntp _ntp;
    rpc::client* _client;
};

class rpc_client_factory final : public sink::factory {
public:
    explicit rpc_client_factory(rpc::client* c)
      : _client(c) {}

    std::optional<std::unique_ptr<sink>> create(model::ntp ntp) override {
        return std::make_unique<rpc_client_sink>(ntp, _client);
    };

private:
    rpc::client* _client;
};

} // namespace

service::service(
  wasm::runtime* runtime,
  model::node_id self,
  ss::sharded<cluster::plugin_frontend>* plugin_frontend,
  ss::sharded<features::feature_table>* feature_table,
  ss::sharded<raft::group_manager>* group_manager,
  ss::sharded<cluster::partition_manager>* partition_manager,
  ss::sharded<rpc::client>* rpc_client)
  : _runtime(runtime)
  , _self(self)
  , _plugin_frontend(plugin_frontend)
  , _feature_table(feature_table)
  , _group_manager(group_manager)
  , _partition_manager(partition_manager)
  , _rpc_client(rpc_client) {}

service::~service() = default;

ss::future<> service::start() { throw std::runtime_error("unimplemented"); }

ss::future<> service::stop() { throw std::runtime_error("unimplemented"); }

ss::future<cluster::errc>
service::delete_transform(model::transform_name name) {
    if (!_feature_table->local().is_active(
          features::feature::wasm_transforms)) {
        co_return cluster::errc::feature_disabled;
    }
    auto _ = _gate.hold();

    vlog(tlog.info, "deleting transform {}", name);
    auto result = co_await _plugin_frontend->local().remove_transform(
      name, model::timeout_clock::now() + metadata_timeout);

    // Make deletes itempotent by translating does not exist into success
    if (result.ec == cluster::errc::transform_does_not_exist) {
        co_return cluster::errc::success;
    }
    if (result.ec != cluster::errc::success) {
        co_return result.ec;
    }
    co_await cleanup_wasm_binary(result.uuid);
    co_return cluster::errc::success;
}

ss::future<cluster::errc>
service::deploy_transform(model::transform_metadata meta, iobuf binary) {
    if (!_feature_table->local().is_active(
          features::feature::wasm_transforms)) {
        co_return cluster::errc::feature_disabled;
    }
    auto _ = _gate.hold();

    vlog(
      tlog.info,
      "deploying wasm binary (size={}) for transform {}",
      binary.size_bytes(),
      meta.name);
    // TODO(rockwood): Validate that the wasm adheres to our ABI
    auto result = co_await _rpc_client->local().store_wasm_binary(
      std::move(binary), wasm_binary_timeout);
    if (result.has_error()) {
        vlog(
          tlog.warn, "storing wasm binary for transform {} failed", meta.name);
        co_return result.error();
    }
    auto [key, offset] = result.value();
    meta.uuid = key;
    meta.source_ptr = offset;
    vlog(
      tlog.debug,
      "stored wasm binary for transform {} at offset {}",
      meta.name,
      offset);
    cluster::errc ec = co_await _plugin_frontend->local().upsert_transform(
      meta, model::timeout_clock::now() + metadata_timeout);
    vlog(
      tlog.debug,
      "deploying transform {} result: {}",
      meta.name,
      cluster::error_category().message(int(ec)));
    if (ec != cluster::errc::success) {
        co_await cleanup_wasm_binary(key);
    }
    co_return ec;
}

ss::future<> service::cleanup_wasm_binary(uuid_t key) {
    // TODO(rockwood): This is a best effort cleanup, we should also have
    // some sort of GC process as well.
    auto result = co_await ss::coroutine::as_future<cluster::errc>(
      _rpc_client->local().delete_wasm_binary(key, wasm_binary_timeout));
    if (result.failed()) {
        vlog(
          tlog.debug,
          "cleaning up wasm binary failed: {}",
          result.get_exception());
        co_return;
    }
    auto ec = result.get();
    if (ec == cluster::errc::success) {
        co_return;
    }
    vlog(
      tlog.debug,
      "cleaning up wasm binary failed: {}",
      cluster::error_category().message(int(ec)));
}

} // namespace transform
