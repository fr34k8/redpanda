/*
 * Copyright 2020 Redpanda Data, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#include "redpanda/admin/server.h"

#include "archival/ntp_archiver_service.h"
#include "cloud_storage/cache_service.h"
#include "cloud_storage/partition_manifest.h"
#include "cloud_storage/remote_partition.h"
#include "cloud_storage/spillover_manifest.h"
#include "cluster/cloud_storage_size_reducer.h"
#include "cluster/cluster_recovery_manager.h"
#include "cluster/cluster_recovery_table.h"
#include "cluster/cluster_utils.h"
#include "cluster/config_frontend.h"
#include "cluster/controller.h"
#include "cluster/controller_api.h"
#include "cluster/controller_stm.h"
#include "cluster/errc.h"
#include "cluster/feature_manager.h"
#include "cluster/fwd.h"
#include "cluster/health_monitor_frontend.h"
#include "cluster/health_monitor_types.h"
#include "cluster/members_backend.h"
#include "cluster/members_frontend.h"
#include "cluster/members_table.h"
#include "cluster/metadata_cache.h"
#include "cluster/migrations/tx_manager_migrator.h"
#include "cluster/node_status_table.h"
#include "cluster/partition_balancer_backend.h"
#include "cluster/partition_balancer_rpc_service.h"
#include "cluster/partition_manager.h"
#include "cluster/security_frontend.h"
#include "cluster/self_test_frontend.h"
#include "cluster/shard_table.h"
#include "cluster/topic_recovery_service.h"
#include "cluster/topic_recovery_status_frontend.h"
#include "cluster/topic_recovery_status_rpc_handler.h"
#include "cluster/topics_frontend.h"
#include "cluster/types.h"
#include "config/configuration.h"
#include "config/endpoint_tls_config.h"
#include "features/feature_table.h"
#include "finjector/hbadger.h"
#include "finjector/stress_fiber.h"
#include "json/document.h"
#include "json/schema.h"
#include "json/stringbuffer.h"
#include "json/validator.h"
#include "json/writer.h"
#include "kafka/types.h"
#include "metrics/metrics.h"
#include "model/fundamental.h"
#include "model/metadata.h"
#include "model/namespace.h"
#include "model/record.h"
#include "model/timeout_clock.h"
#include "model/transform.h"
#include "net/dns.h"
#include "net/tls_certificate_probe.h"
#include "net/unresolved_address.h"
#include "pandaproxy/rest/api.h"
#include "pandaproxy/schema_registry/api.h"
#include "pandaproxy/schema_registry/schema_id_validation.h"
#include "raft/types.h"
#include "redpanda/admin/api-doc/broker.json.hh"
#include "redpanda/admin/api-doc/cluster.json.hh"
#include "redpanda/admin/api-doc/cluster_config.json.hh"
#include "redpanda/admin/api-doc/config.json.hh"
#include "redpanda/admin/api-doc/debug.json.hh"
#include "redpanda/admin/api-doc/features.json.hh"
#include "redpanda/admin/api-doc/hbadger.json.hh"
#include "redpanda/admin/api-doc/partition.json.hh"
#include "redpanda/admin/api-doc/raft.json.hh"
#include "redpanda/admin/api-doc/security.json.hh"
#include "redpanda/admin/api-doc/shadow_indexing.json.hh"
#include "redpanda/admin/api-doc/status.json.hh"
#include "redpanda/cluster_config_schema_util.h"
#include "resource_mgmt/memory_sampling.h"
#include "rpc/errc.h"
#include "rpc/rpc_utils.h"
#include "security/acl.h"
#include "security/audit/audit_log_manager.h"
#include "security/audit/schemas/application_activity.h"
#include "security/audit/schemas/iam.h"
#include "security/audit/schemas/types.h"
#include "security/audit/schemas/utils.h"
#include "security/audit/types.h"
#include "security/credential_store.h"
#include "security/oidc_authenticator.h"
#include "security/oidc_service.h"
#include "security/scram_algorithm.h"
#include "security/scram_authenticator.h"
#include "security/scram_credential.h"
#include "ssx/future-util.h"
#include "ssx/sformat.h"
#include "transform/api.h"
#include "utils/fragmented_vector.h"
#include "utils/string_switch.h"
#include "utils/utf8.h"
#include "vlog.h"

#include <seastar/core/coroutine.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/map_reduce.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <seastar/coroutine/as_future.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/http/api_docs.hh>
#include <seastar/http/exception.hh>
#include <seastar/http/httpd.hh>
#include <seastar/http/json_path.hh>
#include <seastar/http/reply.hh>
#include <seastar/http/request.hh>
#include <seastar/http/url.hh>
#include <seastar/json/json_elements.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/net/tls.hh>
#include <seastar/util/later.hh>
#include <seastar/util/log.hh>
#include <seastar/util/short_streams.hh>
#include <seastar/util/variant_utils.hh>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/lexical_cast/bad_lexical_cast.hpp>
#include <fmt/core.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <unordered_map>

using namespace std::chrono_literals;

ss::logger adminlog{"admin_api_server"};

static constexpr auto audit_svc_name = "Redpanda Admin HTTP Server";

// Helpers for partition routes
namespace {

template<typename C>
class lw_shared_container {
public:
    using iterator = C::iterator;
    using value_type = C::value_type;

    explicit lw_shared_container(C&& c)
      : c_{ss::make_lw_shared<C>(std::move(c))} {}

    iterator begin() const { return c_->begin(); }
    iterator end() const { return c_->end(); }

private:
    ss::lw_shared_ptr<C> c_;
};

inline net::unresolved_address from_ss_sa(const ss::socket_address& sa) {
    return {fmt::format("{}", sa.addr()), sa.port(), sa.addr().in_family()};
}

security::audit::authentication::used_cleartext
is_cleartext(const ss::sstring& protocol) {
    return boost::iequals(protocol, "https")
             ? security::audit::authentication::used_cleartext::no
             : security::audit::authentication::used_cleartext::yes;
}

security::audit::authentication_event_options make_authn_event_options(
  ss::httpd::const_req req, const request_auth_result& auth_result) {
    return {
      .auth_protocol = auth_result.get_sasl_mechanism(),
      .server_addr = from_ss_sa(req.get_server_address()),
      .svc_name = audit_svc_name,
      .client_addr = from_ss_sa(req.get_client_address()),
      .is_cleartext = is_cleartext(req.get_protocol_name()),
      .user = {
        .name = auth_result.get_username().empty() ? "{{anonymous}}"
                                                   : auth_result.get_username(),
        .type_id = auth_result.is_authenticated()
                     ? (
                       auth_result.is_superuser()
                         ? security::audit::user::type::admin
                         : security::audit::user::type::user)
                     : security::audit::user::type::unknown}};
}

security::audit::authentication_event_options make_authn_event_options(
  ss::httpd::const_req req,
  const security::credential_user& username,
  const ss::sstring& reason) {
    return {
      .server_addr = from_ss_sa(req.get_server_address()),
      .svc_name = audit_svc_name,
      .client_addr = from_ss_sa(req.get_client_address()),
      .is_cleartext = is_cleartext(req.get_protocol_name()),
      .user
      = {.name = username, .type_id = security::audit::user::type::unknown},
      .error_reason = reason};
}

bool escape_hatch_request(ss::httpd::const_req req) {
    /// The following "break glass" mechanism allows the cluster config
    /// API to be hit in the case the user desires to disable auditing
    /// so the cluster can continue to make progress in the event auditing
    /// is not working as expected.
    static const auto allowed_requests = std::to_array(
      {ss::httpd::cluster_config_json::get_cluster_config_status,
       ss::httpd::cluster_config_json::get_cluster_config_schema,
       ss::httpd::cluster_config_json::patch_cluster_config});

    return std::any_of(
      allowed_requests.cbegin(),
      allowed_requests.cend(),
      [method = req._method,
       url = req.get_url()](const ss::httpd::path_description& d) {
          return d.path == url
                 && d.operations.method == ss::httpd::str2type(method);
      });
}
} // namespace

model::ntp admin_server::parse_ntp_from_request(
  ss::httpd::parameters& param, model::ns ns) {
    auto topic = model::topic(param["topic"]);

    model::partition_id partition;
    try {
        partition = model::partition_id(std::stoi(param["partition"]));
    } catch (...) {
        throw ss::httpd::bad_param_exception(fmt::format(
          "Partition id must be an integer: {}", param["partition"]));
    }

    if (partition() < 0) {
        throw ss::httpd::bad_param_exception(
          fmt::format("Invalid partition id {}", partition));
    }

    return {std::move(ns), std::move(topic), partition};
}

model::ntp admin_server::parse_ntp_from_request(ss::httpd::parameters& param) {
    return parse_ntp_from_request(param, model::ns(param["namespace"]));
}

model::ntp admin_server::parse_ntp_from_query_param(
  const std::unique_ptr<ss::http::request>& req) {
    auto ns = req->get_query_param("namespace");
    auto topic = req->get_query_param("topic");
    auto partition_str = req->get_query_param("partition_id");
    model::partition_id partition;
    try {
        partition = model::partition_id(std::stoi(partition_str));
    } catch (...) {
        throw ss::httpd::bad_param_exception(
          fmt::format("Partition must be an integer: {}", partition_str));
    }

    if (partition() < 0) {
        throw ss::httpd::bad_param_exception(
          fmt::format("Invalid partition id {}", partition));
    }

    return {std::move(ns), std::move(topic), partition};
}

admin_server::admin_server(
  admin_server_cfg cfg,
  ss::sharded<stress_fiber_manager>& looper,
  ss::sharded<cluster::partition_manager>& pm,
  ss::sharded<raft::group_manager>& rgm,
  cluster::controller* controller,
  ss::sharded<cluster::shard_table>& st,
  ss::sharded<cluster::metadata_cache>& metadata_cache,
  ss::sharded<rpc::connection_cache>& connection_cache,
  ss::sharded<cluster::node_status_table>& node_status_table,
  ss::sharded<cluster::self_test_frontend>& self_test_frontend,
  ss::sharded<kafka::usage_manager>& usage_manager,
  pandaproxy::rest::api* http_proxy,
  pandaproxy::schema_registry::api* schema_registry,
  ss::sharded<cloud_storage::topic_recovery_service>& topic_recovery_svc,
  ss::sharded<cluster::topic_recovery_status_frontend>&
    topic_recovery_status_frontend,
  ss::sharded<storage::node>& storage_node,
  ss::sharded<memory_sampling>& memory_sampling_service,
  ss::sharded<cloud_storage::cache>& cloud_storage_cache,
  ss::sharded<resources::cpu_profiler>& cpu_profiler,
  ss::sharded<transform::service>* transform_service,
  ss::sharded<security::audit::audit_log_manager>& audit_mgr,
  std::unique_ptr<cluster::tx_manager_migrator>& tx_manager_migrator,
  ss::sharded<kafka::server>& kafka_server)
  : _log_level_timer([this] { log_level_timer_handler(); })
  , _server("admin")
  , _cfg(std::move(cfg))
  , _stress_fiber_manager(looper)
  , _partition_manager(pm)
  , _raft_group_manager(rgm)
  , _controller(controller)
  , _shard_table(st)
  , _metadata_cache(metadata_cache)
  , _connection_cache(connection_cache)
  , _auth(
      config::shard_local_cfg().admin_api_require_auth.bind(),
      config::shard_local_cfg().superusers.bind(),
      _controller)
  , _node_status_table(node_status_table)
  , _self_test_frontend(self_test_frontend)
  , _usage_manager(usage_manager)
  , _http_proxy(http_proxy)
  , _schema_registry(schema_registry)
  , _topic_recovery_service(topic_recovery_svc)
  , _topic_recovery_status_frontend(topic_recovery_status_frontend)
  , _storage_node(storage_node)
  , _memory_sampling_service(memory_sampling_service)
  , _cloud_storage_cache(cloud_storage_cache)
  , _cpu_profiler(cpu_profiler)
  , _transform_service(transform_service)
  , _audit_mgr(audit_mgr)
  , _tx_manager_migrator(tx_manager_migrator)
  , _kafka_server(kafka_server)
  , _default_blocked_reactor_notify(
      ss::engine().get_blocked_reactor_notify_ms()) {
    _server.set_content_streaming(true);
}

ss::future<> admin_server::start() {
    _blocked_reactor_notify_reset_timer.set_callback([this] {
        return ss::smp::invoke_on_all([ms = _default_blocked_reactor_notify] {
            ss::engine().update_blocked_reactor_notify_ms(ms);
        });
    });
    configure_metrics_route();
    configure_admin_routes();

    co_await configure_listeners();

    vlog(
      adminlog.info,
      "Started HTTP admin service listening at {}",
      _cfg.endpoints);
}

ss::future<> admin_server::stop() {
    _blocked_reactor_notify_reset_timer.cancel();
    return _server.stop();
}

void admin_server::configure_admin_routes() {
    auto rb = ss::make_shared<ss::httpd::api_registry_builder20>(
      _cfg.admin_api_docs_dir, "/v1");

    auto insert_comma = [](ss::output_stream<char>& os) {
        return os.write(",\n");
    };
    rb->set_api_doc(_server._routes);
    rb->register_api_file(_server._routes, "header");
    rb->register_api_file(_server._routes, "config");
    rb->register_function(_server._routes, insert_comma);
    rb->register_api_file(_server._routes, "cluster_config");
    rb->register_function(_server._routes, insert_comma);
    rb->register_api_file(_server._routes, "raft");
    rb->register_function(_server._routes, insert_comma);
    rb->register_api_file(_server._routes, "kafka");
    rb->register_function(_server._routes, insert_comma);
    rb->register_api_file(_server._routes, "partition");
    rb->register_function(_server._routes, insert_comma);
    rb->register_api_file(_server._routes, "security");
    rb->register_function(_server._routes, insert_comma);
    rb->register_api_file(_server._routes, "status");
    rb->register_function(_server._routes, insert_comma);
    rb->register_api_file(_server._routes, "features");
    rb->register_function(_server._routes, insert_comma);
    rb->register_api_file(_server._routes, "hbadger");
    rb->register_function(_server._routes, insert_comma);
    rb->register_api_file(_server._routes, "broker");
    rb->register_function(_server._routes, insert_comma);
    rb->register_api_file(_server._routes, "transaction");
    rb->register_function(_server._routes, insert_comma);
    rb->register_api_file(_server._routes, "debug");
    rb->register_function(_server._routes, insert_comma);
    rb->register_api_file(_server._routes, "cluster");
    rb->register_function(_server._routes, insert_comma);
    rb->register_api_file(_server._routes, "transform");
    register_config_routes();
    register_cluster_config_routes();
    register_raft_routes();
    register_kafka_routes();
    register_security_routes();
    register_status_routes();
    register_features_routes();
    register_broker_routes();
    register_partition_routes();
    register_hbadger_routes();
    register_transaction_routes();
    register_debug_routes();
    register_usage_routes();
    register_self_test_routes();
    register_cluster_routes();
    register_shadow_indexing_routes();
    register_wasm_transform_routes();
    /**
     * Special REST apis active only in recovery mode
     */
    if (config::node().recovery_mode_enabled) {
        register_recovery_mode_routes();
    }
}

namespace {
json::validator make_set_replicas_validator() {
    const std::string schema = R"(
{
    "type": "array",
    "items": {
        "type": "object",
        "properties": {
            "node_id": {
                "type": "number"
            },
            "core": {
                "type": "number"
            }
        },
        "required": [
            "node_id",
            "core"
        ],
        "additionalProperties": false
    }
}
)";
    return json::validator(schema);
}
} // namespace

/**
 * A helper around rapidjson's Parse that checks for errors & raises
 * seastar HTTP exception.  Without that check, something as simple
 * as an empty request body causes a redpanda crash via a rapidjson
 * assertion when trying to GetObject on the resulting document.
 */
ss::future<json::Document>
admin_server::parse_json_body(ss::http::request* req) {
    json::Document doc;
    auto content = co_await ss::util::read_entire_stream_contiguous(
      *req->content_stream);
    doc.Parse(content);
    if (doc.HasParseError()) {
        throw ss::httpd::bad_request_exception(
          fmt::format("JSON parse error: {}", doc.GetParseError()));
    } else {
        co_return doc;
    }
}

namespace {
/**
 * A helper to apply a schema validator to a request and on error,
 * string-ize any schema errors in the 400 response to help
 * caller see what went wrong.
 */
void apply_validator(
  json::validator& validator, json::Document::ValueType const& doc) {
    try {
        json::validate(validator, doc);
    } catch (json::json_validation_error& err) {
        throw ss::httpd::bad_request_exception(fmt::format(
          "JSON request body does not conform to schema: {}", err.what()));
    }
}

ss::future<std::vector<model::broker_shard>> validate_set_replicas(
  const json::Document& doc, const cluster::topics_frontend& topic_fe) {
    static thread_local json::validator set_replicas_validator(
      make_set_replicas_validator());

    apply_validator(set_replicas_validator, doc);

    std::vector<model::broker_shard> replicas;
    if (!doc.IsArray()) {
        throw ss::httpd::bad_request_exception("Expected array");
    }
    for (auto& r : doc.GetArray()) {
        const auto& node_id_json = r["node_id"];
        const auto& core_json = r["core"];
        if (!node_id_json.IsInt() || !core_json.IsInt()) {
            throw ss::httpd::bad_request_exception(
              "`node_id` and `core` must be integers");
        }
        const auto node_id = model::node_id(r["node_id"].GetInt());
        const auto shard = static_cast<uint32_t>(r["core"].GetInt());

        // Validate node ID and shard - subsequent code assumes
        // they exist and may assert if not.
        bool is_valid = co_await topic_fe.validate_shard(node_id, shard);
        if (!is_valid) {
            throw ss::httpd::bad_request_exception(fmt::format(
              "Replica set refers to non-existent node/shard (node "
              "{} "
              "shard {})",
              node_id,
              shard));
        }
        auto contains_already = std::find_if(
                                  replicas.begin(),
                                  replicas.end(),
                                  [node_id](const model::broker_shard& bs) {
                                      return bs.node_id == node_id;
                                  })
                                != replicas.end();
        if (contains_already) {
            throw ss::httpd::bad_request_exception(fmt::format(
              "All the replicas must be placed on separate nodes. "
              "Requested replica set contains node: {} more than "
              "once",
              node_id));
        }
        replicas.push_back(
          model::broker_shard{.node_id = node_id, .shard = shard});
    }
    co_return replicas;
}

/**
 * Helper for requests with boolean URL query parameters that should
 * be treated as false if absent, or true if "true" (case insensitive) or "1"
 */
bool get_boolean_query_param(
  const ss::http::request& req, std::string_view name) {
    auto key = ss::sstring(name);
    if (!req.query_parameters.contains(key)) {
        return false;
    }

    const ss::sstring& str_param = req.query_parameters.at(key);
    return ss::internal::case_insensitive_cmp()(str_param, "true")
           || str_param == "1";
}

/**
 * Helper for requests with decimal_integer URL query parameters.
 *
 * Throws a bad_request exception if the parameter is present but not
 * an integer.
 */
std::optional<uint64_t>
get_integer_query_param(const ss::http::request& req, std::string_view name) {
    auto key = ss::sstring(name);
    if (!req.query_parameters.contains(key)) {
        return std::nullopt;
    }

    const ss::sstring& str_param = req.query_parameters.at(key);
    try {
        return std::stoi(str_param);
    } catch (const std::invalid_argument&) {
        throw ss::httpd::bad_request_exception(
          fmt::format("Parameter {} must be an integer", name));
    }
}

} // namespace

void admin_server::configure_metrics_route() {
    ss::prometheus::add_prometheus_routes(
      _server,
      {.metric_help = "redpanda metrics",
       .prefix = "vectorized",
       .handle = ss::metrics::default_handle(),
       .route = "/metrics"})
      .get();
    ss::prometheus::add_prometheus_routes(
      _server,
      {.metric_help = "redpanda metrics",
       .prefix = "redpanda",
       .handle = metrics::public_metrics_handle,
       .route = "/public_metrics"})
      .get();
}

ss::future<> admin_server::configure_listeners() {
    // We will remember any endpoint that is listening
    // on an external address and does not have mTLS,
    // for emitting a warning later if user/pass auth is disabled.
    std::optional<model::broker_endpoint> insecure_ep;

    for (auto& ep : _cfg.endpoints) {
        // look for credentials matching current endpoint
        auto tls_it = std::find_if(
          _cfg.endpoints_tls.begin(),
          _cfg.endpoints_tls.end(),
          [&ep](const config::endpoint_tls_config& c) {
              return c.name == ep.name;
          });

        const bool localhost = ep.address.host() == "127.0.0.1"
                               || ep.address.host() == "localhost"
                               || ep.address.host() == "localhost.localdomain"
                               || ep.address.host() == "::1";

        ss::shared_ptr<ss::tls::server_credentials> cred;
        if (tls_it != _cfg.endpoints_tls.end()) {
            cred = co_await net::build_reloadable_server_credentials_with_probe(
              tls_it->config,
              "admin",
              tls_it->name,
              [](
                const std::unordered_set<ss::sstring>& updated,
                const std::exception_ptr& eptr) {
                  rpc::log_certificate_reload_event(
                    adminlog, "API TLS", updated, eptr);
              });
            if (!localhost && !tls_it->config.get_require_client_auth()) {
                insecure_ep = ep;
            }
        } else {
            if (!localhost) {
                insecure_ep = ep;
            }
        }

        auto resolved = co_await net::resolve_dns(ep.address);
        co_await ss::with_scheduling_group(_cfg.sg, [this, cred, resolved] {
            return _server.listen(resolved, cred);
        });
    }

    if (
      insecure_ep.has_value()
      && !config::shard_local_cfg().admin_api_require_auth()) {
        auto& ep = insecure_ep.value();
        vlog(
          adminlog.warn,
          "Insecure Admin API listener on {}:{}, consider enabling "
          "`admin_api_require_auth`",
          ep.address.host(),
          ep.address.port());
    }
}

void admin_server::audit_authz(
  ss::httpd::const_req req,
  const request_auth_result& auth_result,
  httpd_authorized authorized,
  std::optional<std::string_view> reason) {
    vlog(adminlog.trace, "Attempting to audit authz for {}", req.format_url());
    auto success = _audit_mgr.local().enqueue_api_activity_event(
      security::audit::event_type::admin,
      req,
      auth_result,
      audit_svc_name,
      bool(authorized),
      reason);
    if (!success) {
        bool is_allowed = escape_hatch_request(req);

        if (!is_allowed) {
            vlog(
              adminlog.error,
              "Failed to audit authorization request for endpoint: {}",
              req.format_url());
            throw ss::httpd::base_exception(
              "Failed to audit authorization request",
              ss::http::reply::status_type::service_unavailable);
        }

        vlog(
          adminlog.error,
          "Request to authorize user to modify or view cluster configuration "
          "was not audited due "
          "to audit queues being full");
    }
}

void admin_server::audit_authn(
  ss::httpd::const_req req, const request_auth_result& auth_result) {
    do_audit_authn(req, make_authn_event_options(req, auth_result));
}

void admin_server::audit_authn_failure(
  ss::httpd::const_req req,
  const security::credential_user& username,
  const ss::sstring& reason) {
    do_audit_authn(req, make_authn_event_options(req, username, reason));
}

void admin_server::do_audit_authn(
  ss::httpd::const_req req,
  security::audit::authentication_event_options options) {
    vlog(adminlog.trace, "Attempting to audit authn for {}", req.format_url());
    auto success = _audit_mgr.local().enqueue_authn_event(std::move(options));

    if (!success) {
        bool is_allowed = escape_hatch_request(req);

        if (!is_allowed) {
            vlog(
              adminlog.error,
              "Failed to audit authentication request for endpoint: {}",
              req.format_url());
            throw ss::httpd::base_exception(
              "Failed to audit authentication request",
              ss::http::reply::status_type::service_unavailable);
        }

        vlog(
          adminlog.error,
          "Request authenticate user to modify or view cluster configuration "
          "was not audited due "
          "to audit queues being full");
    }
}

void admin_server::log_request(
  const ss::http::request& req, const request_auth_result& auth_state) const {
    vlog(
      adminlog.debug,
      "[{}] {} {}",
      auth_state.get_username().size() > 0 ? auth_state.get_username()
                                           : "_anonymous",
      req._method,
      req.get_url());
}

void admin_server::log_exception(
  const ss::sstring& url,
  const request_auth_result& auth_state,
  std::exception_ptr eptr) const {
    using http_status = ss::http::reply::status_type;
    using http_status_ut = std::underlying_type_t<http_status>;
    const auto log_ex = [&](
                          std::optional<http_status_ut> status = std::nullopt) {
        std::stringstream os;
        const auto username
          = (auth_state.get_username().size() > 0 ? auth_state.get_username() : "_anonymous");
        /// Strip URL of query parameters in the case sensitive information
        /// might have been passed
        fmt::print(
          os,
          "[{}] exception intercepted - url: [{}]",
          username,
          url.substr(0, url.find('?')));
        if (status) {
            fmt::print(os, " http_return_status[{}]", *status);
        }
        fmt::print(os, " reason - {}", eptr);
        return os.str();
    };

    try {
        std::rethrow_exception(eptr);
    } catch (ss::httpd::base_exception& ex) {
        const auto status = static_cast<http_status_ut>(ex.status());
        if (ex.status() == http_status::internal_server_error) {
            vlog(adminlog.error, "{}", log_ex(status));
        } else if (status >= 400) {
            vlog(adminlog.warn, "{}", log_ex(status));
        }
    } catch (...) {
        vlog(adminlog.error, "{}", log_ex());
    }
}

void admin_server::rearm_log_level_timer() {
    _log_level_timer.cancel();

    auto next = std::min_element(
      _log_level_resets.begin(),
      _log_level_resets.end(),
      [](const auto& a, const auto& b) {
          return a.second.expires < b.second.expires;
      });

    if (next != _log_level_resets.end()) {
        _log_level_timer.arm(next->second.expires);
    }
}

void admin_server::log_level_timer_handler() {
    for (auto it = _log_level_resets.begin(); it != _log_level_resets.end();) {
        if (it->second.expires <= ss::timer<>::clock::now()) {
            vlog(
              adminlog.info,
              "Expiring log level for {{{}}} to {}",
              it->first,
              it->second.level);
            ss::global_logger_registry().set_logger_level(
              it->first, it->second.level);
            _log_level_resets.erase(it++);
        } else {
            ++it;
        }
    }
    rearm_log_level_timer();
}

ss::future<ss::httpd::redirect_exception> admin_server::redirect_to_leader(
  ss::http::request& req, model::ntp const& ntp) const {
    auto leader_id_opt = _metadata_cache.local().get_leader_id(ntp);

    if (!leader_id_opt.has_value()) {
        vlog(adminlog.info, "Can't redirect, no leader for ntp {}", ntp);

        throw ss::httpd::base_exception(
          fmt::format(
            "Partition {} does not have a leader, cannot redirect", ntp),
          ss::http::reply::status_type::service_unavailable);
    }

    if (leader_id_opt.value() == *config::node().node_id()) {
        vlog(
          adminlog.info,
          "Can't redirect to leader from leader node ({})",
          leader_id_opt.value());
        throw ss::httpd::base_exception(
          fmt::format("Leader not available"),
          ss::http::reply::status_type::service_unavailable);
    }

    auto leader_opt = _metadata_cache.local().get_node_metadata(
      leader_id_opt.value());
    if (!leader_opt.has_value()) {
        throw ss::httpd::base_exception(
          fmt::format(
            "Partition {} leader {} metadata not available",
            ntp,
            leader_id_opt.value()),
          ss::http::reply::status_type::service_unavailable);
    }
    auto leader = leader_opt.value();

    // Heuristic for finding peer's admin API interface that is accessible
    // from the client that sent this request:
    // - if the host in the Host header matches one of our advertised kafka
    //   addresses, then assume that the peer's advertised kafka address
    //   with the same index will also be their public admin API address.
    // - Assume that the peer is listening on the same port that the client
    //   used to make this request (i.e. the port in Host)
    //
    // This will work reliably if all node configs have symmetric kafka listener
    // sections (i.e. all specify the same number of listeners in the same
    // order, for example all nodes have an internal and an external listener in
    // that order), and the hostname used for connecting to the admin API
    // matches one of the hostnames used for a kafka listener.
    //
    // The generic fallback if the heuristic fails is to use the peer's
    // internal RPC address.  This works if the user is e.g. connecting
    // by IP address to a k8s cluster's internal pod IP.

    auto host_hdr = req.get_header("host");

    std::string port;        // String like :123, or blank for default port
    std::string target_host; // Guessed admin API hostname of peer

    if (host_hdr.empty()) {
        vlog(
          adminlog.debug,
          "redirect: Missing Host header, falling back to internal RPC "
          "address");

        // Misbehaving client.  Guess peer address.
        port = fmt::format(
          ":{}", config::node_config().admin()[0].address.port());

    } else {
        // Assumption: the peer will be listening on the same port that this
        // request was sent to: parse the port out of the Host header
        auto colon = host_hdr.find(":");
        if (colon == ss::sstring::npos) {
            // Admin is being served on a standard port, leave port string blank
        } else {
            port = host_hdr.substr(colon);
        }

        auto req_hostname = host_hdr.substr(0, colon);

        // See if this hostname is one of our kafka advertised addresses
        auto kafka_endpoints = config::node().advertised_kafka_api();
        auto match_i = std::find_if(
          kafka_endpoints.begin(),
          kafka_endpoints.end(),
          [req_hostname](model::broker_endpoint const& be) {
              return be.address.host() == req_hostname;
          });
        if (match_i != kafka_endpoints.end()) {
            auto listener_idx = size_t(
              std::distance(kafka_endpoints.begin(), match_i));

            auto leader_advertised_addrs
              = leader.broker.kafka_advertised_listeners();
            if (leader_advertised_addrs.size() < listener_idx + 1) {
                vlog(
                  adminlog.debug,
                  "redirect: leader has no advertised address at matching "
                  "index for {}, "
                  "falling back to internal RPC address",
                  req_hostname);
                target_host = leader.broker.rpc_address().host();
            } else {
                target_host
                  = leader_advertised_addrs[listener_idx].address.host();
            }
        } else {
            vlog(
              adminlog.debug,
              "redirect: {} did not match any kafka listeners, redirecting to "
              "peer's internal RPC address",
              req_hostname);
            target_host = leader.broker.rpc_address().host();
        }
    }

    auto url = fmt::format(
      "{}://{}{}{}", req.get_protocol_name(), target_host, port, req._url);

    vlog(
      adminlog.info, "Redirecting admin API call to {} leader at {}", ntp, url);

    co_return ss::httpd::redirect_exception(
      url, ss::http::reply::status_type::temporary_redirect);
}

bool admin_server::need_redirect_to_leader(
  model::ntp ntp, ss::sharded<cluster::metadata_cache>& metadata_cache) {
    auto leader_id_opt = metadata_cache.local().get_leader_id(ntp);
    if (!leader_id_opt.has_value()) {
        throw ss::httpd::base_exception(
          fmt::format(
            "Partition {} does not have a leader, cannot redirect", ntp),
          ss::http::reply::status_type::service_unavailable);
    }

    return leader_id_opt.value() != *config::node().node_id();
}

model::node_id admin_server::parse_broker_id(const ss::http::request& req) {
    try {
        return model::node_id(
          boost::lexical_cast<model::node_id::type>(req.param["id"]));
    } catch (...) {
        throw ss::httpd::bad_param_exception(
          fmt::format("Broker id: {}, must be an integer", req.param["id"]));
    }
}

namespace {
ss::future<std::vector<ss::httpd::partition_json::partition_result>>
map_partition_results(std::vector<cluster::move_cancellation_result> results) {
    std::vector<ss::httpd::partition_json::partition_result> ret;
    ret.reserve(results.size());

    for (cluster::move_cancellation_result& r : results) {
        ss::httpd::partition_json::partition_result result;
        result.ns = std::move(r.ntp.ns)();
        result.topic = std::move(r.ntp.tp.topic)();
        result.partition = r.ntp.tp.partition;
        result.result = cluster::make_error_code(r.result).message();
        ret.push_back(std::move(result));
        co_await ss::maybe_yield();
    }
    co_return ret;
}

ss::httpd::broker_json::maintenance_status fill_maintenance_status(
  const cluster::broker_state& b_state,
  const cluster::drain_manager::drain_status& s) {
    ss::httpd::broker_json::maintenance_status ret;
    ret.draining = b_state.get_maintenance_state()
                   == model::maintenance_state::active;

    ret.finished = s.finished;
    ret.errors = s.errors;
    ret.partitions = s.partitions.value_or(0);
    ret.transferring = s.transferring.value_or(0);
    ret.eligible = s.eligible.value_or(0);
    ret.failed = s.failed.value_or(0);

    return ret;
}
ss::httpd::broker_json::maintenance_status
fill_maintenance_status(const cluster::broker_state& b_state) {
    ss::httpd::broker_json::maintenance_status ret;

    ret.draining = b_state.get_maintenance_state()
                   == model::maintenance_state::active;

    return ret;
}

// Fetch brokers from the members table and enrich with
// metadata from the health monitor.
ss::future<std::vector<ss::httpd::broker_json::broker>>
get_brokers(cluster::controller* const controller) {
    cluster::node_report_filter filter;

    return controller->get_health_monitor()
      .local()
      .get_cluster_health(
        cluster::cluster_report_filter{
          .node_report_filter = std::move(filter),
        },
        cluster::force_refresh::no,
        model::no_timeout)
      .then([controller](result<cluster::cluster_health_report> h_report) {
          if (h_report.has_error()) {
              throw ss::httpd::base_exception(
                fmt::format(
                  "Unable to get cluster health: {}",
                  h_report.error().message()),
                ss::http::reply::status_type::service_unavailable);
          }

          std::map<model::node_id, ss::httpd::broker_json::broker> broker_map;

          // Collect broker information from the members table.
          auto& members_table = controller->get_members_table().local();
          for (auto& [id, nm] : members_table.nodes()) {
              ss::httpd::broker_json::broker b;
              b.node_id = id;
              b.num_cores = nm.broker.properties().cores;
              if (nm.broker.rack()) {
                  b.rack = *nm.broker.rack();
              }
              b.membership_status = fmt::format(
                "{}", nm.state.get_membership_state());

              // These fields are defaults that will be overwritten with
              // data from the health report.
              b.is_alive = true;
              b.maintenance_status = fill_maintenance_status(nm.state);
              b.internal_rpc_address = nm.broker.rpc_address().host();
              b.internal_rpc_port = nm.broker.rpc_address().port();

              broker_map[id] = b;
          }

          // Enrich the broker information with data from the health report.
          for (auto& ns : h_report.value().node_states) {
              auto it = broker_map.find(ns.id);
              if (it == broker_map.end()) {
                  continue;
              }

              it->second.is_alive = static_cast<bool>(ns.is_alive);

              auto r_it = std::find_if(
                h_report.value().node_reports.begin(),
                h_report.value().node_reports.end(),
                [id = ns.id](const cluster::node_health_report& nhr) {
                    return nhr.id == id;
                });

              if (r_it != h_report.value().node_reports.end()) {
                  it->second.version = r_it->local_state.redpanda_version;
                  it->second.recovery_mode_enabled
                    = r_it->local_state.recovery_mode_enabled;
                  auto nm = members_table.get_node_metadata_ref(r_it->id);
                  if (nm && r_it->drain_status) {
                      it->second.maintenance_status = fill_maintenance_status(
                        nm.value().get().state, r_it->drain_status.value());
                  }

                  auto add_disk = [&ds_list = it->second.disk_space](
                                    const storage::disk& ds) {
                      ss::httpd::broker_json::disk_space_info dsi;
                      dsi.path = ds.path;
                      dsi.free = ds.free;
                      dsi.total = ds.total;
                      ds_list.push(dsi);
                  };
                  add_disk(r_it->local_state.data_disk);
                  if (!r_it->local_state.shared_disk()) {
                      add_disk(r_it->local_state.get_cache_disk());
                  }
              }
          }

          std::vector<ss::httpd::broker_json::broker> brokers;
          brokers.reserve(broker_map.size());

          for (auto&& broker : broker_map) {
              brokers.push_back(std::move(broker.second));
          }

          return ss::make_ready_future<decltype(brokers)>(std::move(brokers));
      });
};

} // namespace

/**
 * Throw an appropriate seastar HTTP exception if we saw
 * a redpanda error during a request.
 *
 * @param ec  error code, may be from any subsystem
 * @param ntp on errors like not_leader, redirect to the leader of this NTP
 * @param id  optional node ID, for operations that acted on a particular
 *            node and would like it referenced in per-node cluster errors
 */
ss::future<> admin_server::throw_on_error(
  ss::http::request& req,
  std::error_code ec,
  model::ntp const& ntp,
  model::node_id id) const {
    if (!ec) {
        co_return;
    }

    if (ec.category() == cluster::error_category()) {
        switch (cluster::errc(ec.value())) {
        case cluster::errc::node_does_not_exists:
            throw ss::httpd::not_found_exception(
              fmt::format("broker with id {} not found", id));
        case cluster::errc::invalid_node_operation:
            throw ss::httpd::bad_request_exception(fmt::format(
              "can not update broker {} state, invalid state transition "
              "requested",
              id));
        case cluster::errc::timeout:
            throw ss::httpd::base_exception(
              fmt::format("Timeout: {}", ec.message()),
              ss::http::reply::status_type::gateway_timeout);
        case cluster::errc::replication_error:
        case cluster::errc::update_in_progress:
        case cluster::errc::leadership_changed:
        case cluster::errc::waiting_for_recovery:
        case cluster::errc::no_leader_controller:
        case cluster::errc::shutting_down:
            throw ss::httpd::base_exception(
              fmt::format("Service unavailable ({})", ec.message()),
              ss::http::reply::status_type::service_unavailable);
        case cluster::errc::not_leader:
            throw co_await redirect_to_leader(req, ntp);
        case cluster::errc::not_leader_controller:
            throw co_await redirect_to_leader(req, model::controller_ntp);
        case cluster::errc::no_update_in_progress:
            throw ss::httpd::bad_request_exception(
              "Cannot cancel partition move operation as there is no move "
              "in progress");
        case cluster::errc::throttling_quota_exceeded:
            throw ss::httpd::base_exception(
              fmt::format("Too many requests: {}", ec.message()),
              ss::http::reply::status_type::too_many_requests);
        case cluster::errc::transform_does_not_exist:
        case cluster::errc::transform_invalid_update:
        case cluster::errc::transform_invalid_create:
        case cluster::errc::transform_invalid_source:
        case cluster::errc::transform_invalid_environment:
        case cluster::errc::source_topic_not_exists:
        case cluster::errc::source_topic_still_in_use:
        case cluster::errc::invalid_partition_operation:
            throw ss::httpd::bad_request_exception(
              fmt::format("{}", ec.message()));
        default:
            throw ss::httpd::server_error_exception(
              fmt::format("Unexpected cluster error: {}", ec.message()));
        }
    } else if (ec.category() == raft::error_category()) {
        switch (raft::errc(ec.value())) {
        case raft::errc::exponential_backoff:
        case raft::errc::disconnected_endpoint:
        case raft::errc::configuration_change_in_progress:
        case raft::errc::leadership_transfer_in_progress:
        case raft::errc::shutting_down:
        case raft::errc::replicated_entry_truncated:
            throw ss::httpd::base_exception(
              fmt::format("Not ready: {}", ec.message()),
              ss::http::reply::status_type::service_unavailable);
        case raft::errc::timeout:
            throw ss::httpd::base_exception(
              fmt::format("Timeout: {}", ec.message()),
              ss::http::reply::status_type::gateway_timeout);
        case raft::errc::transfer_to_current_leader:
            co_return;
        case raft::errc::not_leader:
            throw co_await redirect_to_leader(req, ntp);
        case raft::errc::node_does_not_exists:
        case raft::errc::not_voter:
            // node_does_not_exist is a 400 rather than a 404, because it
            // comes up in the context of a destination for leader transfer,
            // rather than a node ID appearing in a URL path.
            throw ss::httpd::bad_request_exception(
              fmt::format("Invalid request: {}", ec.message()));
        default:
            throw ss::httpd::server_error_exception(
              fmt::format("Unexpected raft error: {}", ec.message()));
        }
    } else if (ec.category() == cluster::tx_error_category()) {
        switch (cluster::tx_errc(ec.value())) {
        case cluster::tx_errc::leader_not_found:
            throw co_await redirect_to_leader(req, ntp);
        case cluster::tx_errc::pid_not_found:
            throw ss::httpd::not_found_exception(
              fmt_with_ctx(fmt::format, "Can not find pid for ntp:{}", ntp));
        case cluster::tx_errc::partition_not_found: {
            ss::sstring error_msg;
            if (
              ntp.tp.topic == model::tx_manager_topic
              && ntp.ns == model::kafka_internal_namespace) {
                error_msg = fmt::format("Can not find ntp:{}", ntp);
            } else {
                error_msg = fmt::format(
                  "Can not find partition({}) in transaction for delete", ntp);
            }
            throw ss::httpd::bad_request_exception(error_msg);
        }
        case cluster::tx_errc::not_coordinator:
            throw ss::httpd::base_exception(
              fmt::format(
                "Node not a coordinator or coordinator leader is not "
                "stabilized yet: {}",
                ec.message()),
              ss::http::reply::status_type::service_unavailable);

        default:
            throw ss::httpd::server_error_exception(
              fmt::format("Unexpected tx_error error: {}", ec.message()));
        }
    } else if (ec.category() == rpc::error_category()) {
        switch (rpc::errc(ec.value())) {
        case rpc::errc::success:
            co_return;
        case rpc::errc::disconnected_endpoint:
        case rpc::errc::exponential_backoff:
        case rpc::errc::shutting_down:
        case rpc::errc::missing_node_rpc_client:
            throw ss::httpd::base_exception(
              fmt::format("Not ready: {}", ec.message()),
              ss::http::reply::status_type::service_unavailable);
        case rpc::errc::client_request_timeout:
        case rpc::errc::connection_timeout:
            throw ss::httpd::base_exception(
              fmt::format("Timeout: {}", ec.message()),
              ss::http::reply::status_type::gateway_timeout);
        case rpc::errc::service_error:
        case rpc::errc::method_not_found:
        case rpc::errc::version_not_supported:
        case rpc::errc::unknown:
            throw ss::httpd::server_error_exception(
              fmt::format("Unexpected error: {}", ec.message()));
        }
    } else {
        throw ss::httpd::server_error_exception(
          fmt::format("Unexpected error: {}", ec.message()));
    }
}

ss::future<ss::json::json_return_type>
admin_server::cancel_node_partition_moves(
  ss::http::request& req, cluster::partition_move_direction direction) {
    auto node_id = parse_broker_id(req);
    auto res = co_await _controller->get_topics_frontend()
                 .local()
                 .cancel_moving_partition_replicas_node(
                   node_id, direction, model::timeout_clock::now() + 5s);

    if (res.has_error()) {
        co_await throw_on_error(
          req, res.error(), model::controller_ntp, node_id);
    }

    co_return ss::json::json_return_type(
      co_await map_partition_results(std::move(res.value())));
}

bool admin_server::str_to_bool(std::string_view s) {
    if (s == "0" || s == "false" || s == "False") {
        return false;
    } else {
        return true;
    }
}

void admin_server::register_config_routes() {
    register_route_raw_sync<superuser>(
      ss::httpd::config_json::get_config,
      [](ss::httpd::const_req, ss::http::reply& reply) {
          json::StringBuffer buf;
          json::Writer<json::StringBuffer> writer(buf);
          config::shard_local_cfg().to_json(
            writer, config::redact_secrets::yes);

          reply.set_status(ss::http::reply::status_type::ok, buf.GetString());
          return "";
      });

    register_route_raw_sync<superuser>(
      ss::httpd::cluster_config_json::get_cluster_config,
      [](ss::httpd::const_req req, ss::http::reply& reply) {
          json::StringBuffer buf;
          json::Writer<json::StringBuffer> writer(buf);

          bool include_defaults = true;
          auto include_defaults_str = req.get_query_param("include_defaults");
          if (!include_defaults_str.empty()) {
              include_defaults = str_to_bool(include_defaults_str);
          }

          config::shard_local_cfg().to_json(
            writer,
            config::redact_secrets::yes,
            [include_defaults](config::base_property& p) {
                return include_defaults || !p.is_default();
            });

          reply.set_status(ss::http::reply::status_type::ok, buf.GetString());
          return "";
      });

    register_route_raw_sync<superuser>(
      ss::httpd::config_json::get_node_config,
      [](ss::httpd::const_req, ss::http::reply& reply) {
          json::StringBuffer buf;
          json::Writer<json::StringBuffer> writer(buf);
          config::node().to_json(writer, config::redact_secrets::yes);

          reply.set_status(ss::http::reply::status_type::ok, buf.GetString());
          return "";
      });

    register_route_raw_sync<superuser>(
      ss::httpd::config_json::get_loggers,
      [](ss::httpd::const_req, ss::http::reply& reply) {
          json::StringBuffer buf;
          json::Writer<json::StringBuffer> writer(buf);
          writer.StartArray();
          for (const auto& name :
               ss::global_logger_registry().get_all_logger_names()) {
              writer.StartObject();
              writer.Key("name");
              writer.String(name);
              writer.EndObject();
          }
          writer.EndArray();
          reply.set_status(ss::http::reply::status_type::ok, buf.GetString());
          return "";
      });

    register_route<superuser>(
      ss::httpd::config_json::set_log_level,
      [this](std::unique_ptr<ss::http::request> req) {
          ss::sstring name;
          if (!ss::http::internal::url_decode(req->param["name"], name)) {
              throw ss::httpd::bad_param_exception(fmt::format(
                "Invalid parameter 'name' got {{{}}}", req->param["name"]));
          }
          validate_no_control(name, string_conversion_exception{name});

          // current level: will be used revert after a timeout (optional)
          ss::log_level cur_level;
          try {
              cur_level = ss::global_logger_registry().get_logger_level(name);
          } catch (std::out_of_range&) {
              throw ss::httpd::bad_param_exception(fmt::format(
                "Cannot set log level: unknown logger {{{}}}", name));
          }

          // decode new level
          ss::log_level new_level;
          try {
              new_level = boost::lexical_cast<ss::log_level>(
                req->get_query_param("level"));
          } catch (const boost::bad_lexical_cast& e) {
              throw ss::httpd::bad_param_exception(fmt::format(
                "Cannot set log level for {{{}}}: unknown level {{{}}}",
                name,
                req->get_query_param("level")));
          }

          // how long should the new log level be active
          std::optional<std::chrono::seconds> expires;
          if (auto e = req->get_query_param("expires"); !e.empty()) {
              try {
                  expires = std::chrono::seconds(
                    boost::lexical_cast<unsigned int>(e));
              } catch (const boost::bad_lexical_cast& e) {
                  throw ss::httpd::bad_param_exception(fmt::format(
                    "Cannot set log level for {{{}}}: invalid expires value "
                    "{{{}}}",
                    name,
                    e));
              }
          }

          vlog(
            adminlog.info,
            "Set log level for {{{}}}: {} -> {}",
            name,
            cur_level,
            new_level);

          ss::global_logger_registry().set_logger_level(name, new_level);

          if (!expires) {
              // if no expiration was given, then use some reasonable default
              // that will prevent the system from remaining in a non-optimal
              // state (e.g. trace logging) indefinitely.
              expires = std::chrono::minutes(10);
          }

          // expires=0 is same as not specifying it at all
          if (expires->count() > 0) {
              auto when = ss::timer<>::clock::now() + expires.value();
              auto res = _log_level_resets.try_emplace(name, cur_level, when);
              if (!res.second) {
                  res.first->second.expires = when;
              }
          } else {
              // perm change. no need to store prev level
              _log_level_resets.erase(name);
          }

          rearm_log_level_timer();

          return ss::make_ready_future<ss::json::json_return_type>(
            ss::json::json_void());
      });
}

namespace {
json::validator make_cluster_config_validator() {
    const std::string schema = R"(
{
    "type": "object",
    "properties": {
        "upsert": {
            "type": "object"
        },
        "remove": {
            "type": "array",
            "items": "string"
        }
    },
    "additionalProperties": false,
    "required": ["upsert", "remove"]
}
)";
    return json::validator(schema);
}

ss::sstring
join_properties(const std::vector<std::reference_wrapper<
                  const config::property<std::optional<ss::sstring>>>>& props) {
    ss::sstring result = "";
    for (size_t idx = 0; const auto& prop : props) {
        if (idx == props.size() - 1) {
            result += ss::sstring{prop.get().name()};
        } else {
            result += ssx::sformat("{}, ", prop.get().name());
        }

        ++idx;
    };

    return result;
}

/**
 * This function provides special case validation for configuration
 * properties that need to check other properties' values as well
 * as their own.
 *
 * Ideally this would be built into the config_store/property generic
 * interfaces, but that's a lot of plumbing for a few relatively simple
 * checks, so for the moment we just do the checks here by hand.
 */
void config_multi_property_validation(
  ss::sstring const& username,
  pandaproxy::schema_registry::api* schema_registry,
  cluster::config_update_request const& req,
  config::configuration const& updated_config,
  std::map<ss::sstring, ss::sstring>& errors) {
    absl::flat_hash_set<ss::sstring> modified_keys;
    for (const auto& i : req.upsert) {
        modified_keys.insert(i.key);
    }

    if (
      (modified_keys.contains("admin_api_require_auth")
       || modified_keys.contains("superusers"))
      && updated_config.admin_api_require_auth()) {
        // We are switching on admin_api_require_auth.  Apply rules to prevent
        // the user "locking themselves out of the house".
        const bool auth_was_enabled
          = config::shard_local_cfg().admin_api_require_auth();

        // There must be some superusers defined
        const auto& superusers = updated_config.superusers();
        absl::flat_hash_set<ss::sstring> superusers_set(
          superusers.begin(), superusers.end());
        if (superusers.empty()) {
            // Some superusers must be defined, or nobody will be able
            // to use the admin API after this request.
            errors["admin_api_require_auth"] = "No superusers defined";
        } else if (!superusers_set.contains(username) && !auth_was_enabled) {
            // When enabling auth, user making the change must be in the list of
            // superusers, or they would be locking themselves out.
            errors["admin_api_require_auth"] = "May only be set by a superuser";
        }
    }

    if (updated_config.cloud_storage_enabled()) {
        // The properties that cloud_storage::configuration requires
        // to be set if cloud storage is enabled.
        using config_properties_seq = std::vector<std::reference_wrapper<
          const config::property<std::optional<ss::sstring>>>>;

        config_properties_seq properties{};

        if (
          updated_config.cloud_storage_credentials_source
          == model::cloud_credentials_source::config_file) {
            config_properties_seq s3_properties = {
              std::ref(updated_config.cloud_storage_region),
              std::ref(updated_config.cloud_storage_bucket),
              std::ref(updated_config.cloud_storage_access_key),
              std::ref(updated_config.cloud_storage_secret_key),
            };

            config_properties_seq abs_properties = {
              std::ref(updated_config.cloud_storage_azure_storage_account),
              std::ref(updated_config.cloud_storage_azure_container),
              std::ref(updated_config.cloud_storage_azure_shared_key),
            };

            std::array<config_properties_seq, 2> valid_configurations = {
              s3_properties, abs_properties};

            bool is_valid_configuration = std::any_of(
              valid_configurations.begin(),
              valid_configurations.end(),
              [](const auto& config) {
                  return std::none_of(
                    config.begin(), config.end(), [](const auto& prop) {
                        return prop() == std::nullopt;
                    });
              });

            if (!is_valid_configuration) {
                errors["cloud_storage_enabled"] = ssx::sformat(
                  "To enable cloud storage you need to configure S3 or Azure "
                  "Blob Storage access. For S3 {} must be set. For ABS {} "
                  "must be set",
                  join_properties(s3_properties),
                  join_properties(abs_properties));
            }
        } else {
            // TODO(vlad): When we add support for non-config file auth
            // methods for ABS, handling here should be updated too.
            config_properties_seq properties = {
              std::ref(updated_config.cloud_storage_region),
              std::ref(updated_config.cloud_storage_bucket),
            };

            for (auto& p : properties) {
                if (p() == std::nullopt) {
                    errors[ss::sstring(p.get().name())]
                      = "Must be set when cloud storage enabled";
                }
            }
        }
    }
    if (
      updated_config.enable_schema_id_validation
        != pandaproxy::schema_registry::schema_id_validation_mode::none
      && !schema_registry) {
        auto name = updated_config.enable_schema_id_validation.name();
        errors[ss::sstring(name)] = ssx::sformat(
          "{} requires schema_registry to be enabled in redpanda.yaml", name);
    }
}
} // namespace

void admin_server::register_cluster_config_routes() {
    register_route<superuser>(
      ss::httpd::cluster_config_json::get_cluster_config_status,
      [this](std::unique_ptr<ss::http::request>) {
          auto& cfg = _controller->get_config_manager();
          return cfg
            .invoke_on(
              cluster::controller_stm_shard,
              [](cluster::config_manager& manager) {
                  return manager.get_projected_status();
              })
            .then([](auto statuses) {
                std::vector<
                  ss::httpd::cluster_config_json::cluster_config_status>
                  res;

                for (const auto& s : statuses) {
                    vlog(adminlog.trace, "status: {}", s.second);
                    auto& rs = res.emplace_back();
                    rs.node_id = s.first;
                    rs.restart = s.second.restart;
                    rs.config_version = s.second.version;

                    // Workaround: seastar json_list hides empty lists by
                    // default.  This complicates API clients, so always push
                    // in a dummy element to get _set=true on json_list (this
                    // is then cleared in the subsequent operator=).
                    rs.invalid.push(ss::sstring("hack"));
                    rs.unknown.push(ss::sstring("hack"));

                    rs.invalid = s.second.invalid;
                    rs.unknown = s.second.unknown;
                }

                return ss::json::json_return_type(std::move(res));
            });
      });

    register_route<publik>(
      ss::httpd::cluster_config_json::get_cluster_config_schema,
      [](std::unique_ptr<ss::http::request>) {
          return ss::make_ready_future<ss::json::json_return_type>(
            util::generate_json_schema(config::shard_local_cfg()));
      });

    register_route<superuser, true>(
      ss::httpd::cluster_config_json::patch_cluster_config,
      [this](
        std::unique_ptr<ss::http::request> req,
        request_auth_result const& auth_state) {
          return patch_cluster_config_handler(std::move(req), auth_state);
      });
}

ss::future<ss::json::json_return_type>
admin_server::patch_cluster_config_handler(
  std::unique_ptr<ss::http::request> req,
  request_auth_result const& auth_state) {
    static thread_local auto cluster_config_validator(
      make_cluster_config_validator());
    auto doc = co_await parse_json_body(req.get());
    apply_validator(cluster_config_validator, doc);

    cluster::config_update_request update;

    // Deserialize removes
    const auto& json_remove = doc["remove"];
    for (const auto& v : json_remove.GetArray()) {
        update.remove.push_back(v.GetString());
    }

    // Deserialize upserts
    const auto& json_upsert = doc["upsert"];
    for (const auto& i : json_upsert.GetObject()) {
        // Re-serialize the individual value.  Our on-disk format
        // for property values is a YAML value (JSON is a subset
        // of YAML, so encoding with JSON is fine)
        json::StringBuffer val_buf;
        json::Writer<json::StringBuffer> w{val_buf};
        i.value.Accept(w);
        auto s = ss::sstring{val_buf.GetString(), val_buf.GetSize()};
        update.upsert.push_back({i.name.GetString(), s});
    }

    // Config property validation happens further down the line
    // at the point that properties are set on each node in
    // response to the deltas that we write to the controller log,
    // but we also do an early validation pass here to avoid writing
    // clearly wrong things into the log & give better feedback
    // to the API consumer.
    absl::flat_hash_set<ss::sstring> upsert_no_op_names;
    if (!get_boolean_query_param(*req, "force")) {
        // A scratch copy of configuration: we must not touch
        // the real live configuration object, that will be updated
        // by config_manager much after config is written to controller
        // log.
        config::configuration cfg;

        // Populate the temporary config object with existing values
        config::shard_local_cfg().for_each(
          [&cfg](const config::base_property& p) {
              auto& tmp_p = cfg.get(p.name());
              tmp_p = p;
          });

        // Configuration properties cannot do multi-property validation
        // themselves, so there is some special casing here for critical
        // properties.

        std::map<ss::sstring, ss::sstring> errors;
        for (const auto& [yaml_name, yaml_value] : update.upsert) {
            // Decode to a YAML object because that's what the property
            // interface expects.
            // Don't both catching ParserException: this was encoded
            // just a few lines above.
            auto val = YAML::Load(yaml_value);

            if (!cfg.contains(yaml_name)) {
                errors[yaml_name] = "Unknown property";
                continue;
            }
            auto& property = cfg.get(yaml_name);

            try {
                auto validation_err = property.validate(val);
                if (validation_err.has_value()) {
                    errors[yaml_name] = validation_err.value().error_message();
                    vlog(
                      adminlog.warn,
                      "Invalid {}: '{}' ({})",
                      yaml_name,
                      property.format_raw(yaml_value),
                      validation_err.value().error_message());
                } else {
                    // In case any property subclass might throw
                    // from it's value setter even after a non-throwing
                    // call to validate (if this happens validate() was
                    // implemented wrongly, but let's be safe)
                    auto changed = property.set_value(val);
                    if (!changed) {
                        upsert_no_op_names.insert(yaml_name);
                    }
                }
            } catch (YAML::BadConversion const& e) {
                // Be helpful, and give the user an example of what
                // the setting should look like, if we have one.
                ss::sstring example;
                auto example_opt = property.example();
                if (example_opt.has_value()) {
                    example = fmt::format(
                      ", for example '{}'", example_opt.value());
                }

                auto message = fmt::format(
                  "expected type {}{}", property.type_name(), example);

                // Special case: we get BadConversion for out-of-range
                // values on smaller integer sizes (e.g. too
                // large value to an int16_t property).
                // ("integer" is a magic string but it's a stable part
                //  of our outward interface)
                if (property.type_name() == "integer") {
                    int64_t n{0};
                    try {
                        n = val.as<int64_t>();
                        // It's a valid integer:
                        message = fmt::format("out of range: '{}'", n);
                    } catch (...) {
                        // This was not an out-of-bounds case, use
                        // the type error message
                    }
                }

                errors[yaml_name] = message;
                vlog(
                  adminlog.warn,
                  "Invalid {}: '{}' ({})",
                  yaml_name,
                  property.format_raw(yaml_value),
                  std::current_exception());
            } catch (...) {
                auto message = fmt::format("{}", std::current_exception());
                errors[yaml_name] = message;
                vlog(
                  adminlog.warn,
                  "Invalid {}: '{}' ({})",
                  yaml_name,
                  property.format_raw(yaml_value),
                  message);
            }
        }

        for (const auto& key : update.remove) {
            if (cfg.contains(key)) {
                cfg.get(key).reset();
            } else {
                errors[key] = "Unknown property";
            }
        }

        // After checking each individual property, check for
        // any multi-property validation errors
        config_multi_property_validation(
          auth_state.get_username(), _schema_registry, update, cfg, errors);

        if (!errors.empty()) {
            json::StringBuffer buf;
            json::Writer<json::StringBuffer> w(buf);

            w.StartObject();
            for (const auto& e : errors) {
                w.Key(e.first.data(), e.first.size());
                w.String(e.second.data(), e.second.size());
            }
            w.EndObject();

            throw ss::httpd::base_exception(
              buf.GetString(),
              ss::http::reply::status_type::bad_request,
              "json");
        }
    }

    if (get_boolean_query_param(*req, "dry_run")) {
        auto current_version
          = co_await _controller->get_config_manager().invoke_on(
            cluster::config_manager::shard,
            [](cluster::config_manager& cm) { return cm.get_version(); });

        // A dry run doesn't really need a result, but it's simpler for
        // the API definition if we return the same structure as a
        // normal write.
        ss::httpd::cluster_config_json::cluster_config_write_result result;
        result.config_version = current_version;
        co_return ss::json::json_return_type(std::move(result));
    }

    if (
      update.upsert.size() == upsert_no_op_names.size()
      && update.remove.empty()) {
        vlog(
          adminlog.trace,
          "patch_cluster_config: ignoring request, {} upserts resulted "
          "in no-ops",
          update.upsert.size());
        auto current_version
          = co_await _controller->get_config_manager().invoke_on(
            cluster::config_manager::shard,
            [](cluster::config_manager& cm) { return cm.get_version(); });
        ss::httpd::cluster_config_json::cluster_config_write_result result;
        result.config_version = current_version;
        co_return ss::json::json_return_type(std::move(result));
    }

    vlog(
      adminlog.trace,
      "patch_cluster_config: {} upserts, {} removes",
      update.upsert.size(),
      update.remove.size());

    auto patch_result = co_await _controller->get_config_frontend().invoke_on(
      cluster::config_frontend::version_shard,
      [update = std::move(update)](cluster::config_frontend& fe) mutable {
          return fe.patch(std::move(update), model::timeout_clock::now() + 5s);
      });

    co_await throw_on_error(*req, patch_result.errc, model::controller_ntp);

    ss::httpd::cluster_config_json::cluster_config_write_result result;
    result.config_version = patch_result.version;
    co_return ss::json::json_return_type(std::move(result));
}

ss::future<ss::json::json_return_type>
admin_server::raft_transfer_leadership_handler(
  std::unique_ptr<ss::http::request> req) {
    raft::group_id group_id;
    try {
        group_id = raft::group_id(std::stoll(req->param["group_id"]));
    } catch (...) {
        throw ss::httpd::bad_param_exception(fmt::format(
          "Raft group id must be an integer: {}", req->param["group_id"]));
    }

    if (group_id() < 0) {
        throw ss::httpd::bad_param_exception(
          fmt::format("Invalid raft group id {}", group_id));
    }

    auto shard = _shard_table.local().shard_for(group_id);
    if (!shard) {
        throw ss::httpd::not_found_exception(
          fmt::format("Raft group {} not found", group_id));
    }

    std::optional<model::node_id> target;
    if (auto node = req->get_query_param("target"); !node.empty()) {
        try {
            target = model::node_id(std::stoi(node));
        } catch (...) {
            throw ss::httpd::bad_param_exception(
              fmt::format("Target node id must be an integer: {}", node));
        }
        if (*target < 0) {
            throw ss::httpd::bad_param_exception(
              fmt::format("Invalid target node id {}", *target));
        }
    }

    vlog(
      adminlog.info,
      "Leadership transfer request for raft group {} to node {}",
      group_id,
      target);

    co_return co_await _partition_manager.invoke_on(
      *shard,
      [group_id, target, this, req = std::move(req)](
        cluster::partition_manager& pm) mutable {
          auto partition = pm.partition_for(group_id);
          if (!partition) {
              throw ss::httpd::not_found_exception();
          }
          const auto ntp = partition->ntp();
          auto r = raft::transfer_leadership_request{
            .group = partition->group(),
            .target = target,
          };
          return partition->transfer_leadership(r).then(
            [this, ntp, req = std::move(req)](auto err) {
                return throw_on_error(*req, err, ntp).then([] {
                    return ss::json::json_return_type(ss::json::json_void());
                });
            });
      });
}

ss::future<ss::json::json_return_type>
admin_server::get_raft_recovery_status_handler(
  std::unique_ptr<ss::http::request>) {
    ss::httpd::raft_json::recovery_status result;

    // Aggregate recovery status from all shards
    auto s = co_await _raft_group_manager.map_reduce0(
      [](auto& rgm) -> raft::recovery_status {
          return rgm.get_recovery_status();
      },
      raft::recovery_status{},
      [](raft::recovery_status acc, raft::recovery_status update) {
          acc.merge(update);
          return acc;
      });

    result.partitions_to_recover = s.partitions_to_recover;
    result.partitions_active = s.partitions_active;
    result.offsets_pending = s.offsets_pending;
    co_return result;
}

void admin_server::register_raft_routes() {
    register_route<superuser>(
      ss::httpd::raft_json::raft_transfer_leadership,
      [this](std::unique_ptr<ss::http::request> req) {
          return raft_transfer_leadership_handler(std::move(req));
      });

    register_route<auth_level::user>(
      ss::httpd::raft_json::get_raft_recovery_status,
      [this](std::unique_ptr<ss::http::request> req) {
          return get_raft_recovery_status_handler(std::move(req));
      });
}

ss::future<ss::json::json_return_type>
admin_server::kafka_transfer_leadership_handler(
  std::unique_ptr<ss::http::request> req) {
    auto ntp = parse_ntp_from_request(req->param);

    std::optional<model::node_id> target;
    if (auto node = req->get_query_param("target"); !node.empty()) {
        try {
            target = model::node_id(std::stoi(node));
        } catch (...) {
            throw ss::httpd::bad_param_exception(
              fmt::format("Target node id must be an integer: {}", node));
        }
        if (*target < 0) {
            throw ss::httpd::bad_param_exception(
              fmt::format("Invalid target node id {}", *target));
        }
    }

    vlog(
      adminlog.info,
      "Leadership transfer request for leader of topic-partition {} to node {}",
      ntp,
      target);

    auto shard = _shard_table.local().shard_for(ntp);
    if (!shard) {
        // This node is not a member of the raft group, redirect.
        throw co_await redirect_to_leader(*req, ntp);
    }

    co_return co_await _partition_manager.invoke_on(
      *shard,
      [ntp = std::move(ntp), target, this, req = std::move(req)](
        cluster::partition_manager& pm) mutable {
          auto partition = pm.get(ntp);
          if (!partition) {
              throw ss::httpd::not_found_exception();
          }
          auto r = raft::transfer_leadership_request{
            .group = partition->group(),
            .target = target,
          };
          return partition->transfer_leadership(r).then(
            [this, ntp, req = std::move(req)](auto err) {
                return throw_on_error(*req, err, ntp).then([] {
                    return ss::json::json_return_type(ss::json::json_void());
                });
            });
      });
}

void admin_server::register_kafka_routes() {
    register_route<superuser>(
      ss::httpd::partition_json::kafka_transfer_leadership,
      [this](std::unique_ptr<ss::http::request> req) {
          return kafka_transfer_leadership_handler(std::move(req));
      });
}

void admin_server::register_status_routes() {
    register_route<publik>(
      ss::httpd::status_json::ready,
      [this](std::unique_ptr<ss::http::request>) {
          std::unordered_map<ss::sstring, ss::sstring> status_map{
            {"status", _ready ? "ready" : "booting"}};
          return ss::make_ready_future<ss::json::json_return_type>(status_map);
      });
}

namespace {
json::validator make_feature_put_validator() {
    const std::string schema = R"(
{
    "type": "object",
    "properties": {
        "state": {
            "type": "string",
            "enum": ["active", "disabled"]
        }
    },
    "additionalProperties": false,
    "required": ["state"]
}
)";
    return json::validator(schema);
}

/// Features are state machines, with multiple 'disabled' states.  Simplify
/// this into the higher level states the the admin API reports to users.
/// (see state machine diagram in feature_state.h)
ss::httpd::features_json::feature_state::feature_state_state
feature_state_to_high_level(features::feature_state::state state) {
    switch (state) {
    case features::feature_state::state::active:
        return ss::httpd::features_json::feature_state::feature_state_state::
          active;
        break;
    case features::feature_state::state::unavailable:
        return ss::httpd::features_json::feature_state::feature_state_state::
          unavailable;
        break;
    case features::feature_state::state::available:
        return ss::httpd::features_json::feature_state::feature_state_state::
          available;
        break;
    case features::feature_state::state::preparing:
        return ss::httpd::features_json::feature_state::feature_state_state::
          preparing;
        break;
    case features::feature_state::state::disabled_clean:
    case features::feature_state::state::disabled_active:
    case features::feature_state::state::disabled_preparing:
        return ss::httpd::features_json::feature_state::feature_state_state::
          disabled;
        break;

        // Exhaustive match
    }
}
} // namespace

ss::future<ss::json::json_return_type>
admin_server::put_feature_handler(std::unique_ptr<ss::http::request> req) {
    static thread_local auto feature_put_validator(
      make_feature_put_validator());

    auto doc = co_await parse_json_body(req.get());
    apply_validator(feature_put_validator, doc);

    auto feature_name = req->param["feature_name"];

    auto feature_id = _controller->get_feature_table().local().resolve_name(
      feature_name);
    if (!feature_id.has_value()) {
        throw ss::httpd::bad_request_exception("Unknown feature name");
    }

    // Retrieve the current state and map to high level disabled/enabled value
    auto& feature_state = _controller->get_feature_table().local().get_state(
      feature_id.value());
    auto current_state = feature_state_to_high_level(feature_state.get_state());

    cluster::feature_update_action action{.feature_name = feature_name};
    auto& new_state_str = doc["state"];
    if (new_state_str == "active") {
        if (
          current_state
          == ss::httpd::features_json::feature_state::feature_state_state::
            active) {
            vlog(
              adminlog.info,
              "Ignoring request to activate feature '{}', already active",
              feature_name);
            co_return ss::json::json_void();
        }
        action.action = cluster::feature_update_action::action_t::activate;
    } else if (new_state_str == "disabled") {
        if (
          current_state
          == ss::httpd::features_json::feature_state::feature_state_state::
            disabled) {
            vlog(
              adminlog.info,
              "Ignoring request to disable feature '{}', already disabled",
              feature_name);
            co_return ss::json::json_void();
        }
        action.action = cluster::feature_update_action::action_t::deactivate;
    } else {
        throw ss::httpd::bad_request_exception("Invalid state");
    }

    if (need_redirect_to_leader(model::controller_ntp, _metadata_cache)) {
        throw co_await redirect_to_leader(*req, model::controller_ntp);
    }

    auto& fm = _controller->get_feature_manager();
    auto err = co_await fm.invoke_on(
      cluster::feature_manager::backend_shard,
      [action](cluster::feature_manager& fm) {
          return fm.write_action(action);
      });
    if (err) {
        throw ss::httpd::bad_request_exception(fmt::format("{}", err));
    } else {
        co_return ss::json::json_void();
    }
}

ss::future<ss::json::json_return_type>
admin_server::put_license_handler(std::unique_ptr<ss::http::request> req) {
    auto raw_license = co_await ss::util::read_entire_stream_contiguous(
      *req->content_stream);
    if (raw_license.empty()) {
        throw ss::httpd::bad_request_exception(
          "Missing redpanda license from request body");
    }
    if (!_controller->get_feature_table().local().is_active(
          features::feature::license)) {
        throw ss::httpd::bad_request_exception(
          "Feature manager reports the cluster is not fully upgraded to "
          "accept license put requests");
    }

    try {
        boost::trim_if(raw_license, boost::is_any_of(" \n\r"));
        auto license = security::make_license(raw_license);
        if (license.is_expired()) {
            throw ss::httpd::bad_request_exception(
              fmt::format("License is expired: {}", license));
        }
        const auto& ft = _controller->get_feature_table().local();
        const auto& loaded_license = ft.get_license();
        if (loaded_license && (*loaded_license == license)) {
            /// Loaded license is idential to license in request, do
            /// nothing and return 200(OK)
            vlog(
              adminlog.info,
              "Attempted to load identical license, doing nothing: {}",
              license);
            co_return ss::json::json_void();
        }
        auto& fm = _controller->get_feature_manager();
        auto err = co_await fm.invoke_on(
          cluster::feature_manager::backend_shard,
          [license = std::move(license)](cluster::feature_manager& fm) mutable {
              return fm.update_license(std::move(license));
          });
        co_await throw_on_error(*req, err, model::controller_ntp);
    } catch (const security::license_malformed_exception& ex) {
        throw ss::httpd::bad_request_exception(
          fmt::format("License is malformed: {}", ex.what()));
    } catch (const security::license_invalid_exception& ex) {
        throw ss::httpd::bad_request_exception(
          fmt::format("License is invalid: {}", ex.what()));
    }
    co_return ss::json::json_void();
}

void admin_server::register_features_routes() {
    register_route<user>(
      ss::httpd::features_json::get_features,
      [this](std::unique_ptr<ss::http::request>) {
          ss::httpd::features_json::features_response res;

          const auto& ft = _controller->get_feature_table().local();
          auto version = ft.get_active_version();

          res.cluster_version = version;
          res.original_cluster_version = ft.get_original_version();
          res.node_earliest_version = ft.get_earliest_logical_version();
          res.node_latest_version = ft.get_latest_logical_version();
          for (const auto& fs : ft.get_feature_state()) {
              ss::httpd::features_json::feature_state item;
              vlog(
                adminlog.trace,
                "feature_state: {} {}",
                fs.spec.name,
                fs.get_state());
              item.name = ss::sstring(fs.spec.name);
              item.state = feature_state_to_high_level(fs.get_state());

              switch (fs.get_state()) {
              case features::feature_state::state::active:
              case features::feature_state::state::preparing:
              case features::feature_state::state::disabled_active:
              case features::feature_state::state::disabled_preparing:
                  item.was_active = true;
                  break;
              default:
                  item.was_active = false;
              }

              res.features.push(item);
          }

          // Report all retired features as active (the code they previously
          // guarded is now on by default).  This enables external programs
          // to check the state of a particular feature flag in perpetuity
          // without having to deal with the ambiguous case of the feature
          // being missing (i.e. unsure if redpanda is too old to have
          // the feature flag, or too new to have it).
          for (const auto& retired_name : features::retired_features) {
              ss::httpd::features_json::feature_state item;
              item.name = ss::sstring(retired_name);
              item.state = ss::httpd::features_json::feature_state::
                feature_state_state::active;
              item.was_active = true;
              res.features.push(item);
          }

          return ss::make_ready_future<ss::json::json_return_type>(
            std::move(res));
      });

    register_route<superuser>(
      ss::httpd::features_json::put_feature,
      [this](std::unique_ptr<ss::http::request> req) {
          return put_feature_handler(std::move(req));
      });

    register_route<user>(
      ss::httpd::features_json::get_license,
      [this](std::unique_ptr<ss::http::request>) {
          if (!_controller->get_feature_table().local().is_active(
                features::feature::license)) {
              throw ss::httpd::bad_request_exception(
                "Feature manager reports the cluster is not fully upgraded to "
                "accept license get requests");
          }
          ss::httpd::features_json::license_response res;
          res.loaded = false;
          const auto& ft = _controller->get_feature_table().local();
          const auto& license = ft.get_license();
          if (license) {
              res.loaded = true;
              ss::httpd::features_json::license_contents lc;
              lc.format_version = license->format_version;
              lc.org = license->organization;
              lc.type = security::license_type_to_string(license->type);
              lc.expires = license->expiry.count();
              lc.sha256 = license->checksum;
              res.license = lc;
          }
          return ss::make_ready_future<ss::json::json_return_type>(
            std::move(res));
      });

    register_route<superuser>(
      ss::httpd::features_json::put_license,
      [this](std::unique_ptr<ss::http::request> req) {
          return put_license_handler(std::move(req));
      });
}

ss::future<ss::json::json_return_type>
admin_server::get_broker_handler(std::unique_ptr<ss::http::request> req) {
    model::node_id id = parse_broker_id(*req);
    auto node_meta = _metadata_cache.local().get_node_metadata(id);
    if (!node_meta) {
        throw ss::httpd::not_found_exception(
          fmt::format("broker with id: {} not found", id));
    }

    auto maybe_drain_status = co_await _controller->get_health_monitor()
                                .local()
                                .get_node_drain_status(
                                  id, model::time_from_now(5s));

    ss::httpd::broker_json::broker ret;
    ret.node_id = node_meta->broker.id();
    ret.internal_rpc_address = node_meta->broker.rpc_address().host();
    ret.internal_rpc_port = node_meta->broker.rpc_address().port();
    ret.num_cores = node_meta->broker.properties().cores;
    if (node_meta->broker.rack()) {
        ret.rack = node_meta->broker.rack().value();
    }
    ret.membership_status = fmt::format(
      "{}", node_meta->state.get_membership_state());
    ret.maintenance_status = fill_maintenance_status(node_meta->state);
    if (
      !maybe_drain_status.has_error()
      && maybe_drain_status.value().has_value()) {
        ret.maintenance_status = fill_maintenance_status(
          node_meta->state, *maybe_drain_status.value());
    }

    co_return ret;
}

ss::future<ss::json::json_return_type> admin_server::decomission_broker_handler(
  std::unique_ptr<ss::http::request> req) {
    model::node_id id = parse_broker_id(*req);

    auto ec
      = co_await _controller->get_members_frontend().local().decommission_node(
        id);

    co_await throw_on_error(*req, ec, model::controller_ntp, id);
    co_return ss::json::json_void();
}

ss::future<ss::json::json_return_type>
admin_server::get_decommission_progress_handler(
  std::unique_ptr<ss::http::request> req) {
    model::node_id id = parse_broker_id(*req);
    auto res
      = co_await _controller->get_api().local().get_node_decommission_progress(
        id, 5s + model::timeout_clock::now());
    if (!res) {
        if (res.error() == cluster::errc::node_does_not_exists) {
            throw ss::httpd::base_exception(
              fmt::format("Node {} does not exists", id),
              ss::http::reply::status_type::not_found);
        } else if (res.error() == cluster::errc::invalid_node_operation) {
            throw ss::httpd::base_exception(
              fmt::format("Node {} is not decommissioning", id),
              ss::http::reply::status_type::bad_request);
        }

        throw ss::httpd::base_exception(
          fmt::format(
            "Unable to get decommission status for {} - {}",
            id,
            res.error().message()),
          ss::http::reply::status_type::internal_server_error);
    }
    ss::httpd::broker_json::decommission_status ret;
    auto& decommission_progress = res.value();

    ret.replicas_left = decommission_progress.replicas_left;
    ret.finished = decommission_progress.finished;

    for (const auto& ntp : decommission_progress.allocation_failures) {
        ret.allocation_failures.push(
          fmt::format("{}/{}/{}", ntp.ns(), ntp.tp.topic(), ntp.tp.partition));
    }

    for (auto& p : decommission_progress.current_reconfigurations) {
        ss::httpd::broker_json::partition_reconfiguration_status status;
        status.ns = p.ntp.ns;
        status.topic = p.ntp.tp.topic;
        status.partition = p.ntp.tp.partition;
        auto added_replicas = cluster::subtract(
          p.current_assignment, p.previous_assignment);
        // we are only interested in reconfigurations where one replica was
        // added to the node
        if (added_replicas.size() != 1) {
            continue;
        }
        ss::httpd::broker_json::broker_shard moving_to{};
        moving_to.node_id = added_replicas.front().node_id();
        moving_to.core = added_replicas.front().shard;
        status.moving_to = moving_to;
        size_t left_to_move = 0;
        size_t already_moved = 0;
        for (auto replica_status : p.already_transferred_bytes) {
            left_to_move += (p.current_partition_size - replica_status.bytes);
            already_moved += replica_status.bytes;
        }
        status.bytes_left_to_move = left_to_move;
        status.bytes_moved = already_moved;
        status.partition_size = p.current_partition_size;
        // if no information from partitions is present yet, we may indicate
        // that everything have to be moved
        if (already_moved == 0 && left_to_move == 0) {
            status.bytes_left_to_move = p.current_partition_size;
        }
        status.reconfiguration_policy = ssx::sformat("{}", p.policy);
        ret.partitions.push(status);
    }

    co_return ret;
}

ss::future<ss::json::json_return_type> admin_server::recomission_broker_handler(
  std::unique_ptr<ss::http::request> req) {
    model::node_id id = parse_broker_id(*req);

    auto ec
      = co_await _controller->get_members_frontend().local().recommission_node(
        id);
    co_await throw_on_error(*req, ec, model::controller_ntp, id);
    co_return ss::json::json_void();
}

ss::future<ss::json::json_return_type>
admin_server::start_broker_maintenance_handler(
  std::unique_ptr<ss::http::request> req) {
    if (_controller->get_members_table().local().node_count() < 2) {
        throw ss::httpd::bad_request_exception(
          "Maintenance mode may not be used on a single node "
          "cluster");
    }

    model::node_id id = parse_broker_id(*req);
    auto ec = co_await _controller->get_members_frontend()
                .local()
                .set_maintenance_mode(id, true);
    co_await throw_on_error(*req, ec, model::controller_ntp, id);
    co_return ss::json::json_void();
}

ss::future<ss::json::json_return_type>
admin_server::stop_broker_maintenance_handler(
  std::unique_ptr<ss::http::request> req) {
    model::node_id id = parse_broker_id(*req);
    auto ec = co_await _controller->get_members_frontend()
                .local()
                .set_maintenance_mode(id, false);
    co_await throw_on_error(*req, ec, model::controller_ntp, id);
    co_return ss::json::json_void();
}

ss::future<ss::json::json_return_type>
admin_server::reset_crash_tracking(std::unique_ptr<ss::http::request>) {
    auto file = config::node().crash_loop_tracker_path().string();
    // we don't need to synchronize access to this file because it is only
    // touched in the very beginning of bootup or very late in shutdown when
    // everything is already cleaned up. This guarantees that there are no
    // concurrent modifications to this file while this API is running.
    co_await ss::remove_file(file);
    co_await ss::sync_directory(config::node().data_directory().as_sstring());
    vlog(adminlog.info, "Deleted crash loop tracker file: {}", file);
    co_return ss::json::json_void();
}

void admin_server::register_broker_routes() {
    register_route<user>(
      ss::httpd::broker_json::get_cluster_view,
      [this](std::unique_ptr<ss::http::request>) {
          return get_brokers(_controller)
            .then([this](std::vector<ss::httpd::broker_json::broker> brokers) {
                auto& members_table = _controller->get_members_table().local();

                ss::httpd::broker_json::cluster_view ret;
                ret.version = members_table.version();
                ret.brokers = std::move(brokers);

                return ss::json::json_return_type(std::move(ret));
            });
      });

    register_route<user>(
      ss::httpd::broker_json::get_brokers,
      [this](std::unique_ptr<ss::http::request>) {
          return get_brokers(_controller)
            .then([](std::vector<ss::httpd::broker_json::broker> brokers) {
                return ss::json::json_return_type(std::move(brokers));
            });
      });

    register_route<user>(
      ss::httpd::broker_json::get_broker,
      [this](std::unique_ptr<ss::http::request> req) {
          return get_broker_handler(std::move(req));
      });

    register_route<user>(
      ss::httpd::broker_json::get_decommission,
      [this](std::unique_ptr<ss::http::request> req) {
          return get_decommission_progress_handler(std::move(req));
      });

    register_route<superuser>(
      ss::httpd::broker_json::decommission,
      [this](std::unique_ptr<ss::http::request> req) {
          return decomission_broker_handler(std::move(req));
      });

    register_route<superuser>(
      ss::httpd::broker_json::recommission,
      [this](std::unique_ptr<ss::http::request> req) {
          return recomission_broker_handler(std::move(req));
      });

    register_route<superuser>(
      ss::httpd::broker_json::start_broker_maintenance,
      [this](std::unique_ptr<ss::http::request> req) {
          return start_broker_maintenance_handler(std::move(req));
      });

    register_route<superuser>(
      ss::httpd::broker_json::stop_broker_maintenance,
      [this](std::unique_ptr<ss::http::request> req) {
          return stop_broker_maintenance_handler(std::move(req));
      });

    /*
     * Unlike start|stop_broker_maintenace, the xxx_local_maintenance
     * versions below operate on local state only and could be used to force
     * a node out of maintenance mode if needed. they don't require the
     * feature flag because the feature is available locally.
     */
    register_route<superuser>(
      ss::httpd::broker_json::start_local_maintenance,
      [this](std::unique_ptr<ss::http::request>) {
          return _controller->get_drain_manager().local().drain().then(
            [] { return ss::json::json_return_type(ss::json::json_void()); });
      });

    register_route<superuser>(
      ss::httpd::broker_json::stop_local_maintenance,
      [this](std::unique_ptr<ss::http::request>) {
          return _controller->get_drain_manager().local().restore().then(
            [] { return ss::json::json_return_type(ss::json::json_void()); });
      });

    register_route<superuser>(
      ss::httpd::broker_json::get_local_maintenance,
      [this](std::unique_ptr<ss::http::request>) {
          return _controller->get_drain_manager().local().status().then(
            [](auto status) {
                ss::httpd::broker_json::maintenance_status res;
                res.draining = status.has_value();
                if (status.has_value()) {
                    res.finished = status->finished;
                    res.errors = status->errors;
                    if (status->partitions.has_value()) {
                        res.partitions = status->partitions.value();
                    }
                    if (status->eligible.has_value()) {
                        res.eligible = status->eligible.value();
                    }
                    if (status->transferring.has_value()) {
                        res.transferring = status->transferring.value();
                    }
                    if (status->failed.has_value()) {
                        res.failed = status->failed.value();
                    }
                }
                return ss::json::json_return_type(res);
            });
      });
    register_route<superuser>(
      ss::httpd::broker_json::cancel_partition_moves,
      [this](std::unique_ptr<ss::http::request> req) {
          return cancel_node_partition_moves(
            *req, cluster::partition_move_direction::all);
      });
    register_route<superuser>(
      ss::httpd::broker_json::reset_crash_tracking,
      [this](std::unique_ptr<ss::http::request> req) {
          return reset_crash_tracking(std::move(req));
      });
}

ss::future<ss::json::json_return_type>
admin_server::get_transactions_handler(std::unique_ptr<ss::http::request> req) {
    const model::ntp ntp = parse_ntp_from_request(req->param);

    if (need_redirect_to_leader(ntp, _metadata_cache)) {
        throw co_await redirect_to_leader(*req, ntp);
    }

    auto shard = _shard_table.local().shard_for(ntp);
    // Strange situation, but need to check it
    if (!shard) {
        throw ss::httpd::server_error_exception(fmt_with_ctx(
          fmt::format, "Can not find shard for partition {}", ntp.tp));
    }

    co_return co_await _partition_manager.invoke_on(
      *shard,
      [ntp = std::move(ntp), req = std::move(req), this](
        cluster::partition_manager& pm) mutable {
          return get_transactions_inner_handler(
            pm, std::move(ntp), std::move(req));
      });
}

ss::future<ss::json::json_return_type>
admin_server::get_transactions_inner_handler(
  cluster::partition_manager& pm,
  model::ntp ntp,
  std::unique_ptr<ss::http::request> req) {
    auto partition = pm.get(ntp);
    if (!partition) {
        throw ss::httpd::server_error_exception(
          fmt_with_ctx(fmt::format, "Can not find partition {}", partition));
    }

    auto rm_stm_ptr = partition->rm_stm();

    if (!rm_stm_ptr) {
        throw ss::httpd::server_error_exception(fmt_with_ctx(
          fmt::format, "Can not get rm_stm for partition {}", partition));
    }

    auto transactions = co_await rm_stm_ptr->get_transactions();

    if (transactions.has_error()) {
        co_await throw_on_error(*req, transactions.error(), ntp);
    }
    ss::httpd::partition_json::transactions ans;

    auto offset_translator = partition->get_offset_translator_state();

    for (auto& [id, tx_info] : transactions.value()) {
        ss::httpd::partition_json::producer_identity pid;
        pid.id = id.get_id();
        pid.epoch = id.get_epoch();

        ss::httpd::partition_json::transaction new_tx;
        new_tx.producer_id = pid;
        new_tx.status = ss::sstring(tx_info.get_status());

        new_tx.lso_bound = offset_translator->from_log_offset(
          tx_info.lso_bound);

        auto staleness = tx_info.get_staleness();
        // -1 is returned for expired transaction, because how
        // long transaction do not do progress is useless for
        // expired tx.
        new_tx.staleness_ms
          = staleness.has_value()
              ? std::chrono::duration_cast<std::chrono::milliseconds>(
                  staleness.value())
                  .count()
              : -1;
        auto timeout = tx_info.get_timeout();
        // -1 is returned for expired transaction, because
        // timeout is useless for expired tx.
        new_tx.timeout_ms
          = timeout.has_value()
              ? std::chrono::duration_cast<std::chrono::milliseconds>(
                  timeout.value())
                  .count()
              : -1;

        if (tx_info.is_expired()) {
            ans.expired_transactions.push(new_tx);
        } else {
            ans.active_transactions.push(new_tx);
        }
    }

    co_return ss::json::json_return_type(ans);
}

ss::future<ss::json::json_return_type>
admin_server::mark_transaction_expired_handler(
  std::unique_ptr<ss::http::request> req) {
    const model::ntp ntp = parse_ntp_from_request(req->param);

    model::producer_identity pid;
    auto node = req->get_query_param("id");
    try {
        pid.id = std::stoi(node);
    } catch (...) {
        throw ss::httpd::bad_param_exception(fmt_with_ctx(
          fmt::format, "Transaction id must be an integer: {}", node));
    }
    node = req->get_query_param("epoch");
    try {
        int64_t epoch = std::stoi(node);
        if (
          epoch < std::numeric_limits<int16_t>::min()
          || epoch > std::numeric_limits<int16_t>::max()) {
            throw ss::httpd::bad_param_exception(
              fmt_with_ctx(fmt::format, "Invalid transaction epoch {}", epoch));
        }
        pid.epoch = epoch;
    } catch (...) {
        throw ss::httpd::bad_param_exception(fmt_with_ctx(
          fmt::format, "Transaction epoch must be an integer: {}", node));
    }

    vlog(adminlog.info, "Mark transaction expired for pid:{}", pid);

    if (need_redirect_to_leader(ntp, _metadata_cache)) {
        throw co_await redirect_to_leader(*req, ntp);
    }

    auto shard = _shard_table.local().shard_for(ntp);
    // Strange situation, but need to check it
    if (!shard) {
        throw ss::httpd::server_error_exception(fmt_with_ctx(
          fmt::format, "Can not find shard for partition {}", ntp.tp));
    }

    co_return co_await _partition_manager.invoke_on(
      *shard,
      [_ntp = std::move(ntp), pid, _req = std::move(req), this](
        cluster::partition_manager& pm) mutable
      -> ss::future<ss::json::json_return_type> {
          auto ntp = std::move(_ntp);
          auto req = std::move(_req);
          auto partition = pm.get(ntp);
          if (!partition) {
              return ss::make_exception_future<ss::json::json_return_type>(
                ss::httpd::server_error_exception(fmt_with_ctx(
                  fmt::format, "Can not find partition {}", partition)));
          }

          auto rm_stm_ptr = partition->rm_stm();

          if (!rm_stm_ptr) {
              return ss::make_exception_future<ss::json::json_return_type>(
                ss::httpd::server_error_exception(fmt_with_ctx(
                  fmt::format,
                  "Can not get rm_stm for partition {}",
                  partition)));
          }

          return rm_stm_ptr->mark_expired(pid).then(
            [this, ntp, req = std::move(req)](auto res) {
                return throw_on_error(*req, res, ntp).then([] {
                    return ss::json::json_return_type(ss::json::json_void());
                });
            });
      });
}

ss::future<ss::json::json_return_type>
admin_server::get_reconfigurations_handler(std::unique_ptr<ss::http::request>) {
    using reconfiguration = ss::httpd::partition_json::reconfiguration;

    auto& in_progress
      = _controller->get_topics_state().local().updates_in_progress();

    std::vector<model::ntp> ntps;
    ntps.reserve(in_progress.size());

    for (auto& [ntp, status] : in_progress) {
        ntps.push_back(ntp);
    }

    auto const deadline = model::timeout_clock::now() + 5s;

    auto [reconfiguration_states, reconciliations]
      = co_await ss::when_all_succeed(
        _controller->get_api().local().get_partitions_reconfiguration_state(
          ntps, deadline),
        _controller->get_api().local().get_global_reconciliation_state(
          ntps, deadline));

    if (reconfiguration_states.has_error()) {
        vlog(
          adminlog.info,
          "unable to get reconfiguration status: {}({})",
          reconfiguration_states.error().message(),
          reconfiguration_states.error());
        throw ss::httpd::base_exception(
          fmt::format(
            "unable to get reconfiguration status: {}({})",
            reconfiguration_states.error().message(),
            reconfiguration_states.error()),
          ss::http::reply::status_type::service_unavailable);
    }
    // we are forced to use shared pointer as underlying chunked_fifo is not
    // copyable
    auto reconciliations_ptr
      = ss::make_lw_shared<cluster::global_reconciliation_state>(
        std::move(reconciliations));
    co_return ss::json::json_return_type(ss::json::stream_range_as_array(
      std::move(reconfiguration_states.value()),
      [reconciliations = std::move(reconciliations_ptr)](auto& s) {
          reconfiguration r;
          r.ns = s.ntp.ns;
          r.topic = s.ntp.tp.topic;
          r.partition = s.ntp.tp.partition;

          for (const model::broker_shard& bs : s.current_assignment) {
              ss::httpd::partition_json::assignment assignment;
              assignment.core = bs.shard;
              assignment.node_id = bs.node_id;
              r.current_replicas.push(assignment);
          }

          for (const model::broker_shard& bs : s.previous_assignment) {
              ss::httpd::partition_json::assignment assignment;
              assignment.core = bs.shard;
              assignment.node_id = bs.node_id;
              r.previous_replicas.push(assignment);
          }

          size_t left_to_move = 0;
          size_t already_moved = 0;
          for (auto replica_status : s.already_transferred_bytes) {
              left_to_move += (s.current_partition_size - replica_status.bytes);
              already_moved += replica_status.bytes;
          }
          r.bytes_left_to_move = left_to_move;
          r.bytes_moved = already_moved;
          r.partition_size = s.current_partition_size;
          // if no information from partitions is present yet, we may indicate
          // that everything have to be moved
          if (already_moved == 0 && left_to_move == 0) {
              r.bytes_left_to_move = s.current_partition_size;
          }
          r.reconfiguration_policy = ssx::sformat("{}", s.policy);
          auto it = reconciliations->ntp_backend_operations.find(s.ntp);
          if (it != reconciliations->ntp_backend_operations.end()) {
              for (auto& node_ops : it->second) {
                  seastar::httpd::partition_json::
                    partition_reconciliation_status per_node_status;
                  per_node_status.node_id = node_ops.node_id;

                  for (auto& op : node_ops.backend_operations) {
                      auto& current_op = node_ops.backend_operations.front();
                      seastar::httpd::partition_json::
                        partition_reconciliation_operation r_op;
                      r_op.core = op.source_shard;
                      r_op.retry_number = current_op.current_retry;
                      r_op.revision = current_op.revision_of_operation;
                      r_op.status = fmt::format(
                        "{} ({})",
                        cluster::error_category().message(
                          (int)current_op.last_operation_result),
                        current_op.last_operation_result);
                      r_op.type = fmt::format("{}", current_op.type);
                      per_node_status.operations.push(r_op);
                  }
                  r.reconciliation_statuses.push(per_node_status);
              }
          }
          return r;
      }));
}

ss::future<ss::json::json_return_type>
admin_server::cancel_partition_reconfig_handler(
  std::unique_ptr<ss::http::request> req) {
    const auto ntp = parse_ntp_from_request(req->param);

    if (ntp == model::controller_ntp) {
        throw ss::httpd::bad_request_exception(
          fmt::format("Can't cancel controller reconfiguration"));
    }
    vlog(
      adminlog.debug,
      "Requesting cancelling of {} partition reconfiguration",
      ntp);

    auto err = co_await _controller->get_topics_frontend()
                 .local()
                 .cancel_moving_partition_replicas(
                   ntp,
                   model::timeout_clock::now()
                     + 10s); // NOLINT(cppcoreguidelines-avoid-magic-numbers)

    co_await throw_on_error(*req, err, model::controller_ntp);
    co_return ss::json::json_void();
}

ss::future<ss::json::json_return_type>
admin_server::unclean_abort_partition_reconfig_handler(
  std::unique_ptr<ss::http::request> req) {
    const auto ntp = parse_ntp_from_request(req->param);

    if (ntp == model::controller_ntp) {
        throw ss::httpd::bad_request_exception(
          "Can't unclean abort controller reconfiguration");
    }
    vlog(
      adminlog.warn,
      "Requesting unclean abort of {} partition reconfiguration",
      ntp);

    auto err = co_await _controller->get_topics_frontend()
                 .local()
                 .abort_moving_partition_replicas(
                   ntp,
                   model::timeout_clock::now()
                     + 10s); // NOLINT(cppcoreguidelines-avoid-magic-numbers)

    co_await throw_on_error(*req, err, model::controller_ntp);
    co_return ss::json::json_void();
}

ss::future<ss::json::json_return_type>
admin_server::force_set_partition_replicas_handler(
  std::unique_ptr<ss::http::request> req) {
    if (unlikely(!_controller->get_feature_table().local().is_active(
          features::feature::force_partition_reconfiguration))) {
        throw ss::httpd::bad_request_exception(
          "Feature not active yet, upgrade in progress?");
    }

    if (need_redirect_to_leader(model::controller_ntp, _metadata_cache)) {
        throw co_await redirect_to_leader(*req, model::controller_ntp);
    }

    auto ntp = parse_ntp_from_request(req->param);
    if (ntp == model::controller_ntp) {
        throw ss::httpd::bad_request_exception(
          fmt::format("Can't reconfigure a controller"));
    }

    auto doc = co_await parse_json_body(req.get());
    auto replicas = co_await validate_set_replicas(
      doc, _controller->get_topics_frontend().local());

    const auto& topics = _controller->get_topics_state().local();
    const auto& in_progress = topics.updates_in_progress();
    const auto in_progress_it = in_progress.find(ntp);

    if (in_progress_it != in_progress.end()) {
        throw ss::httpd::bad_request_exception(
          fmt::format("A partition operation is in progress. Check "
                      "reconfigurations and "
                      "cancel in flight update before issuing force "
                      "replica set update."));
    }
    const auto current_assignment = topics.get_partition_assignment(ntp);
    if (current_assignment) {
        const auto& current_replicas = current_assignment->replicas;
        if (current_replicas == replicas) {
            vlog(
              adminlog.info,
              "Request to change ntp {} replica set to {}, no change",
              ntp,
              replicas);
            co_return ss::json::json_void();
        }
        auto relax_restrictions
          = _controller->get_feature_table().local().is_active(
            features::feature::enhanced_force_reconfiguration);
        if (
          !relax_restrictions
          && !cluster::is_proper_subset(replicas, current_replicas)) {
            throw ss::httpd::bad_request_exception(fmt::format(
              "Target assignment {} is not a proper subset of current {}, "
              "choose a proper subset of existing replicas.",
              replicas,
              current_replicas));
        }
    }

    vlog(
      adminlog.info,
      "Request to force update ntp {} replica set to {}",
      ntp,
      replicas);

    auto err = co_await _controller->get_topics_frontend()
                 .local()
                 .force_update_partition_replicas(
                   ntp,
                   replicas,
                   model::timeout_clock::now()
                     + 10s); // NOLINT(cppcoreguidelines-avoid-magic-numbers)

    vlog(
      adminlog.debug,
      "Request to change ntp {} replica set to {}: err={}",
      ntp,
      replicas,
      err);

    co_await throw_on_error(*req, err, model::controller_ntp);
    co_return ss::json::json_void();
}

ss::future<ss::json::json_return_type>
admin_server::set_partition_replicas_handler(
  std::unique_ptr<ss::http::request> req) {
    auto ntp = parse_ntp_from_request(req->param);

    if (ntp == model::controller_ntp) {
        throw ss::httpd::bad_request_exception(
          fmt::format("Can't reconfigure a controller"));
    }

    auto doc = co_await parse_json_body(req.get());
    auto replicas = co_await validate_set_replicas(
      doc, _controller->get_topics_frontend().local());
    auto current_assignment
      = _controller->get_topics_state().local().get_partition_assignment(ntp);

    // For a no-op change, just return success here, to avoid doing
    // all the raft writes and consensus restarts for a config change
    // that will do nothing.
    if (current_assignment && current_assignment->replicas == replicas) {
        vlog(
          adminlog.info,
          "Request to change ntp {} replica set to {}, no change",
          ntp,
          replicas);
        co_return ss::json::json_void();
    }

    vlog(
      adminlog.info,
      "Request to change ntp {} replica set to {}",
      ntp,
      replicas);

    auto err = co_await _controller->get_topics_frontend()
                 .local()
                 .move_partition_replicas(
                   ntp,
                   replicas,
                   cluster::reconfiguration_policy::full_local_retention,
                   model::timeout_clock::now()
                     + 10s); // NOLINT(cppcoreguidelines-avoid-magic-numbers)

    vlog(
      adminlog.debug,
      "Request to change ntp {} replica set to {}: err={}",
      ntp,
      replicas,
      err);

    co_await throw_on_error(*req, err, model::controller_ntp);
    co_return ss::json::json_void();
}

void admin_server::register_partition_routes() {
    /*
     * Get a list of partition summaries.
     */
    register_route<user>(
      ss::httpd::partition_json::get_partitions,
      [this](std::unique_ptr<ss::http::request>) {
          using summary = ss::httpd::partition_json::partition_summary;
          auto get_summaries =
            [](auto& partition_manager, bool materialized, auto get_leader) {
                return partition_manager.map_reduce0(
                  [materialized, get_leader](auto& pm) {
                      fragmented_vector<summary> partitions;
                      for (const auto& it : pm.partitions()) {
                          summary p;
                          p.ns = it.first.ns;
                          p.topic = it.first.tp.topic;
                          p.partition_id = it.first.tp.partition;
                          p.core = ss::this_shard_id();
                          p.materialized = materialized;
                          p.leader = get_leader(it.second);
                          partitions.push_back(std::move(p));
                      }
                      return partitions;
                  },
                  fragmented_vector<summary>{},
                  [](
                    fragmented_vector<summary> acc,
                    fragmented_vector<summary> update) {
                      std::move(
                        std::make_move_iterator(update.begin()),
                        std::make_move_iterator(update.end()),
                        std::back_inserter(acc));
                      return acc;
                  });
            };
          return get_summaries(
                   _partition_manager,
                   false,
                   [](const auto& p) {
                       return p->get_leader_id().value_or(model::node_id(-1))();
                   })
            .then([](auto partitions) mutable {
                return ss::json::json_return_type(
                  ss::json::stream_range_as_array(
                    lw_shared_container{std::move(partitions)},
                    [](summary const& i) -> summary const& { return i; }));
            });
      });

    register_route<user>(
      ss::httpd::partition_json::get_partitions_local_summary,
      [this](std::unique_ptr<ss::http::request>) {
          // This type mirrors partitions_local_summary, but satisfies
          // the seastar map_reduce requirement of being nothrow move
          // constructible.
          struct summary_t {
              uint64_t count{0};
              uint64_t leaderless{0};
              uint64_t under_replicated{0};
          };

          return _partition_manager
            .map_reduce0(
              [](auto& pm) {
                  summary_t s;
                  for (const auto& it : pm.partitions()) {
                      s.count += 1;
                      if (it.second->get_leader_id() == std::nullopt) {
                          s.leaderless += 1;
                      }
                      if (it.second->get_under_replicated() == std::nullopt) {
                          s.under_replicated += 1;
                      }
                  }
                  return s;
              },
              summary_t{},
              [](summary_t acc, summary_t update) {
                  acc.count += update.count;
                  acc.leaderless += update.leaderless;
                  acc.under_replicated += update.under_replicated;
                  return acc;
              })
            .then([](summary_t summary) {
                ss::httpd::partition_json::partitions_local_summary result;
                result.count = summary.count;
                result.leaderless = summary.leaderless;
                result.under_replicated = summary.under_replicated;
                return ss::json::json_return_type(std::move(result));
            });
      });
    register_route<user>(
      ss::httpd::partition_json::get_topic_partitions,
      [this](std::unique_ptr<ss::http::request> req) {
          return get_topic_partitions_handler(std::move(req));
      });

    /*
     * Get detailed information about a partition.
     */
    register_route<user>(
      ss::httpd::partition_json::get_partition,
      [this](std::unique_ptr<ss::http::request> req) {
          return get_partition_handler(std::move(req));
      });

    /*
     * Get detailed information about transactions for partition.
     */
    register_route<user>(
      ss::httpd::partition_json::get_transactions,
      [this](std::unique_ptr<ss::http::request> req) {
          return get_transactions_handler(std::move(req));
      });

    /*
     * Abort transaction for partition
     */
    register_route<superuser>(
      ss::httpd::partition_json::mark_transaction_expired,
      [this](std::unique_ptr<ss::http::request> req) {
          return mark_transaction_expired_handler(std::move(req));
      });
    register_route<superuser>(
      ss::httpd::partition_json::cancel_partition_reconfiguration,
      [this](std::unique_ptr<ss::http::request> req) {
          return cancel_partition_reconfig_handler(std::move(req));
      });
    register_route<superuser>(
      ss::httpd::partition_json::unclean_abort_partition_reconfiguration,
      [this](std::unique_ptr<ss::http::request> req) {
          return unclean_abort_partition_reconfig_handler(std::move(req));
      });

    register_route<superuser>(
      ss::httpd::partition_json::set_partition_replicas,
      [this](std::unique_ptr<ss::http::request> req) {
          return set_partition_replicas_handler(std::move(req));
      });

    register_route<superuser>(
      ss::httpd::debug_json::force_update_partition_replicas,
      [this](std::unique_ptr<ss::http::request> req) {
          return force_set_partition_replicas_handler(std::move(req));
      });

    register_route<superuser>(
      ss::httpd::partition_json::trigger_partitions_rebalance,
      [this](std::unique_ptr<ss::http::request> req) {
          return trigger_on_demand_rebalance_handler(std::move(req));
      });

    register_route<user>(
      ss::httpd::partition_json::get_partition_reconfigurations,
      [this](std::unique_ptr<ss::http::request> req) {
          return get_reconfigurations_handler(std::move(req));
      });

    register_route<user>(
      ss::httpd::partition_json::majority_lost,
      [this](std::unique_ptr<ss::http::request> req) {
          return get_majority_lost_partitions(std::move(req));
      });

    register_route<user>(
      ss::httpd::partition_json::force_recover_from_nodes,
      [this](std::unique_ptr<ss::http::request> req) {
          return force_recover_partitions_from_nodes(std::move(req));
      });
}
namespace {
ss::httpd::partition_json::partition
build_controller_partition(cluster::metadata_cache& cache) {
    ss::httpd::partition_json::partition p;
    p.ns = model::controller_ntp.ns;
    p.topic = model::controller_ntp.tp.topic;
    p.partition_id = model::controller_ntp.tp.partition;
    p.leader_id = -1;

    // Controller topic is on all nodes.  Report all nodes,
    // with the leader first.
    auto leader_opt = cache.get_controller_leader_id();
    if (leader_opt.has_value()) {
        ss::httpd::partition_json::assignment a;
        a.node_id = leader_opt.value();
        a.core = cluster::controller_stm_shard;
        p.replicas.push(a);
        p.leader_id = *leader_opt;
    }
    // special case, controller is raft group 0
    p.raft_group_id = 0;
    for (const auto& i : cache.node_ids()) {
        if (!leader_opt.has_value() || leader_opt.value() != i) {
            ss::httpd::partition_json::assignment a;
            a.node_id = i;
            a.core = cluster::controller_stm_shard;
            p.replicas.push(a);
        }
    }

    // Controller topic does not have a reconciliation state,
    // but include the field anyway to keep the API output
    // consistent.
    p.status = ssx::sformat("{}", cluster::reconciliation_status::done);
    return p;
}
} // namespace

ss::future<ss::json::json_return_type>
admin_server::get_partition_handler(std::unique_ptr<ss::http::request> req) {
    const model::ntp ntp = parse_ntp_from_request(req->param);
    const bool is_controller = ntp == model::controller_ntp;

    if (!is_controller && !_metadata_cache.local().contains(ntp)) {
        throw ss::httpd::not_found_exception(
          fmt::format("Could not find ntp: {}", ntp));
    }

    ss::httpd::partition_json::partition p;
    p.ns = ntp.ns;
    p.topic = ntp.tp.topic;
    p.partition_id = ntp.tp.partition;
    p.leader_id = -1;

    // Logic for fetching replicas+status is different for normal
    // topics vs. the special controller topic.
    if (is_controller) {
        return ss::make_ready_future<ss::json::json_return_type>(
          build_controller_partition(_metadata_cache.local()));

    } else {
        // Normal topic
        auto assignment
          = _controller->get_topics_state().local().get_partition_assignment(
            ntp);

        if (assignment) {
            for (auto& r : assignment->replicas) {
                ss::httpd::partition_json::assignment a;
                a.node_id = r.node_id;
                a.core = r.shard;
                p.replicas.push(a);
            }
            p.raft_group_id = assignment->group;
        }
        auto leader = _metadata_cache.local().get_leader_id(ntp);
        if (leader) {
            p.leader_id = *leader;
        }

        p.disabled = _controller->get_topics_state().local().is_disabled(ntp);

        return _controller->get_api()
          .local()
          .get_reconciliation_state(ntp)
          .then([p](const cluster::ntp_reconciliation_state& state) mutable {
              p.status = ssx::sformat("{}", state.status());
              return ss::json::json_return_type(std::move(p));
          });
    }
}
ss::future<ss::json::json_return_type>
admin_server::get_topic_partitions_handler(
  std::unique_ptr<ss::http::request> req) {
    model::topic_namespace tp_ns(
      model::ns(req->param["namespace"]), model::topic(req->param["topic"]));
    const bool is_controller_topic = tp_ns.ns == model::controller_ntp.ns
                                     && tp_ns.tp
                                          == model::controller_ntp.tp.topic;

    // Logic for fetching replicas+status is different for normal
    // topics vs. the special controller topic.
    if (is_controller_topic) {
        co_return ss::json::json_return_type(
          build_controller_partition(_metadata_cache.local()));
    }

    auto tp_md = _metadata_cache.local().get_topic_metadata_ref(tp_ns);
    if (!tp_md) {
        throw ss::httpd::not_found_exception(
          fmt::format("Could not find topic: {}/{}", tp_ns.ns, tp_ns.tp));
    }
    using partition_t = ss::httpd::partition_json::partition;
    std::vector<partition_t> partitions;
    const auto& assignments = tp_md->get().get_assignments();
    partitions.reserve(assignments.size());

    const auto* disabled_set
      = _controller->get_topics_state().local().get_topic_disabled_set(tp_ns);

    // Normal topic
    for (const auto& p_as : assignments) {
        partition_t p;
        p.ns = tp_ns.ns;
        p.topic = tp_ns.tp;
        p.partition_id = p_as.id;
        p.raft_group_id = p_as.group;
        for (auto& r : p_as.replicas) {
            ss::httpd::partition_json::assignment a;
            a.node_id = r.node_id;
            a.core = r.shard;
            p.replicas.push(a);
        }
        auto leader = _metadata_cache.local().get_leader_id(tp_ns, p_as.id);
        if (leader) {
            p.leader_id = *leader;
        }
        p.disabled = disabled_set && disabled_set->is_disabled(p_as.id);
        partitions.push_back(std::move(p));
    }

    co_await ss::max_concurrent_for_each(
      partitions, 32, [this, &tp_ns](partition_t& p) {
          return _controller->get_api()
            .local()
            .get_reconciliation_state(model::ntp(
              tp_ns.ns, tp_ns.tp, model::partition_id(p.partition_id())))
            .then([&p](const cluster::ntp_reconciliation_state& state) mutable {
                p.status = ssx::sformat("{}", state.status());
            });
      });

    co_return ss::json::json_return_type(partitions);
}

ss::future<ss::json::json_return_type>
admin_server::get_majority_lost_partitions(
  std::unique_ptr<ss::http::request> request) {
    if (need_redirect_to_leader(model::controller_ntp, _metadata_cache)) {
        throw co_await redirect_to_leader(*request, model::controller_ntp);
    }

    auto input = request->get_query_param("dead_nodes");
    if (input.length() <= 0) {
        throw ss::httpd::bad_param_exception(
          "Query parameter dead_nodes not set, expecting a csv of integers "
          "(broker_ids)");
    }

    std::vector<ss::sstring> tokens;
    boost::split(tokens, input, boost::is_any_of(","));

    std::vector<model::node_id> dead_nodes;
    dead_nodes.reserve(tokens.size());
    for (auto& token : tokens) {
        try {
            dead_nodes.emplace_back(std::stoi(token));
        } catch (...) {
            throw ss::httpd::bad_param_exception(fmt::format(
              "Token {} doesn't parse to an integer in input: {}, expecting a "
              "csv of integer broker_ids",
              token,
              input));
        }
    }

    if (dead_nodes.size() == 0) {
        throw ss::httpd::bad_param_exception(fmt::format(
          "Malformed input query parameter: {}, expecting a csv of "
          "integers (broker_ids)",
          input));
    }

    vlog(
      adminlog.info,
      "Request for majority loss partitions from input defunct nodes: {}",
      dead_nodes);

    auto result = co_await _controller->get_topics_frontend()
                    .local()
                    .partitions_with_lost_majority(std::move(dead_nodes));
    if (!result) {
        if (
          result.error().category() == cluster::error_category()
          && result.error() == cluster::errc::concurrent_modification_error) {
            throw ss::httpd::base_exception(
              "Concurrent changes to topics while the operation, retry after "
              "some time, ensure there are no reconfigurations in progress.",
              ss::http::reply::status_type::service_unavailable);
        } else if (
          result.error().category() == cluster::error_category()
          && result.error() == cluster::errc::invalid_request) {
            throw ss::httpd::base_exception{
              "Invalid request, check the broker log for details.",
              ss::http::reply::status_type::bad_request};
        }
        throw ss::httpd::base_exception(
          fmt::format(
            "Internal error while processing request: {}",
            result.error().message()),
          ss::http::reply::status_type::internal_server_error);
    }
    co_return ss::json::json_return_type(ss::json::stream_range_as_array(
      lw_shared_container(std::move(result.value())),
      [](const cluster::ntp_with_majority_loss& ntp) mutable {
          ss::httpd::partition_json::ntp ntp_json;
          ntp_json.ns = ntp.ntp.ns();
          ntp_json.topic = ntp.ntp.tp.topic();
          ntp_json.partition = ntp.ntp.tp.partition();

          ss::httpd::partition_json::ntp_with_majority_loss result;
          result.ntp = std::move(ntp_json);
          result.topic_revision = ntp.topic_revision;
          for (auto& replica : ntp.assignment) {
              ss::httpd::partition_json::assignment assignment;
              assignment.node_id = replica.node_id;
              assignment.core = replica.shard;
              result.replicas.push(assignment);
          }
          for (auto& node : ntp.dead_nodes) {
              result.dead_nodes.push(node());
          }
          return result;
      }));
}

namespace {

json::validator make_node_id_array_validator() {
    const std::string schema = R"(
    {
      "type": "array",
      "items": {
        "type": "number"
      }
    }
  )";
    return json::validator(schema);
}

std::vector<model::node_id>
parse_node_ids_from_json(const json::Document::ValueType& val) {
    static thread_local json::validator validator(
      make_node_id_array_validator());
    apply_validator(validator, val);
    std::vector<model::node_id> nodes;
    for (auto& r : val.GetArray()) {
        nodes.emplace_back(r.GetInt());
    }
    return nodes;
}

json::validator make_force_recover_partitions_validator() {
    const std::string schema = R"(
{
  "type": "object",
  "properties": {
    "dead_nodes": {
      "type": "array",
      "items": {
        "type": "number"
      }
    },
    "partitions_to_force_recover": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "ntp": {
            "type": "object",
            "properties": {
              "ns": {
                "type": "string"
              },
              "topic": {
                "type": "string"
              },
              "partition": {
                "type": "number"
              }
            },
            "required": [
              "ns",
              "topic",
              "partition"
            ]
          },
          "topic_revision": {
            "type": "number"
          },
          "replicas": {
            "type": "array",
            "items": {
              "type": "object",
              "properties": {
                "node_id": {
                  "type": "number"
                },
                "core": {
                  "type": "number"
                }
              },
              "required": [
                "node_id",
                "core"
              ]
            }
          },
          "dead_nodes": {
            "type": "array",
            "items": {
              "type": "number"
            }
          }
        },
        "required": [
          "ntp",
          "topic_revision",
          "replicas",
          "dead_nodes"
        ]
      }
    }
  },
  "required": [
    "dead_nodes",
    "partitions_to_force_recover"
  ]
}
)";
    return json::validator(schema);
}

json::validator make_ntp_validator() {
    const std::string schema = R"(
{
  "type": "object",
  "properties": {
    "ns": {
      "type": "string"
    },
    "topic": {
      "type": "string"
    },
    "partition": {
      "type": "number"
    }
  },
  "required": [
    "ns",
    "topic",
    "partition"
  ]
}
)";
    return json::validator(schema);
}

model::ntp parse_ntp_from_json(const json::Document::ValueType& value) {
    static thread_local json::validator validator(make_ntp_validator());
    apply_validator(validator, value);
    return {
      model::ns{value["ns"].GetString()},
      model::topic(value["topic"].GetString()),
      model::partition_id(value["partition"].GetInt())};
}

json::validator make_replicas_validator() {
    const std::string schema = R"(
{
  "type": "array",
  "items": {
    "type": "object",
    "properties": {
      "node_id": {
        "type": "number"
      },
      "core": {
        "type": "number"
      }
    },
    "required": [
      "node_id",
      "core"
    ]
  }
}
  )";
    return json::validator(schema);
}

std::vector<model::broker_shard>
parse_replicas_from_json(const json::Document::ValueType& value) {
    static thread_local json::validator validator(make_replicas_validator());
    apply_validator(validator, value);
    std::vector<model::broker_shard> replicas;
    replicas.reserve(value.GetArray().Size());
    for (auto& r : value.GetArray()) {
        model::broker_shard bs;
        bs.node_id = model::node_id(r["node_id"].GetInt());
        bs.shard = r["core"].GetInt();
        replicas.push_back(bs);
    }
    return replicas;
}

} // namespace

ss::future<ss::json::json_return_type>
admin_server::force_recover_partitions_from_nodes(
  std::unique_ptr<ss::http::request> request) {
    if (!_controller->get_feature_table().local().is_active(
          features::feature::enhanced_force_reconfiguration)) {
        throw ss::httpd::bad_request_exception(
          "Required feature is not active yet which indicates the cluster has "
          "not fully upgraded yet, retry after a successful upgrade.");
    }

    if (need_redirect_to_leader(model::controller_ntp, _metadata_cache)) {
        throw co_await redirect_to_leader(*request, model::controller_ntp);
    }

    static thread_local json::validator validator(
      make_force_recover_partitions_validator());

    auto doc = co_await parse_json_body(request.get());
    apply_validator(validator, doc);

    // parse the json body into a controller command.
    std::vector<model::node_id> dead_nodes = parse_node_ids_from_json(
      doc["dead_nodes"]);
    fragmented_vector<cluster::ntp_with_majority_loss>
      partitions_to_force_recover;
    for (auto& r : doc["partitions_to_force_recover"].GetArray()) {
        auto ntp = parse_ntp_from_json(r["ntp"]);
        auto replicas = parse_replicas_from_json(r["replicas"]);
        auto topic_revision = model::revision_id(
          r["topic_revision"].GetInt64());
        auto dead_replicas = parse_node_ids_from_json(r["dead_nodes"]);

        cluster::ntp_with_majority_loss ntp_entry;
        ntp_entry.ntp = std::move(ntp);
        ntp_entry.assignment = std::move(replicas);
        ntp_entry.topic_revision = topic_revision;
        ntp_entry.dead_nodes = std::move(dead_replicas);
        partitions_to_force_recover.push_back(std::move(ntp_entry));
    }

    auto ec = co_await _controller->get_topics_frontend()
                .local()
                .force_recover_partitions_from_nodes(
                  std::move(dead_nodes),
                  std::move(partitions_to_force_recover),
                  model::timeout_clock::now() + 5s);

    co_await throw_on_error(*request, ec, model::controller_ntp);
    co_return ss::json::json_return_type(ss::json::json_void());
}

ss::future<ss::json::json_return_type>
admin_server::trigger_on_demand_rebalance_handler(
  std::unique_ptr<ss::http::request> req) {
    auto ec = co_await _controller->get_partition_balancer().invoke_on(
      cluster::controller_stm_shard,
      [](cluster::partition_balancer_backend& pb) {
          return pb.request_rebalance();
      });

    co_await throw_on_error(*req, ec, model::controller_ntp);
    co_return ss::json::json_return_type(ss::json::json_void());
}

void admin_server::register_hbadger_routes() {
    /**
     * we always register `v1/failure-probes` route. It will ALWAYS return
     * empty list of probes in production mode, and flag indicating that
     * honey badger is disabled
     */

    if constexpr (!finjector::honey_badger::is_enabled()) {
        register_route<user>(
          ss::httpd::hbadger_json::get_failure_probes,
          [](std::unique_ptr<ss::http::request>) {
              ss::httpd::hbadger_json::failure_injector_status status;
              status.enabled = false;
              return ss::make_ready_future<ss::json::json_return_type>(
                std::move(status));
          });
        return;
    }

    register_route<user>(
      ss::httpd::hbadger_json::get_failure_probes,
      [](std::unique_ptr<ss::http::request>) {
          auto modules = finjector::shard_local_badger().modules();
          ss::httpd::hbadger_json::failure_injector_status status;
          status.enabled = true;

          for (auto& m : modules) {
              ss::httpd::hbadger_json::failure_probes pr;
              pr.module = m.first.data();
              for (auto& p : m.second) {
                  pr.points.push(p.data());
              }
              status.probes.push(pr);
          }

          return ss::make_ready_future<ss::json::json_return_type>(
            std::move(status));
      });
    /*
     * Enable failure injector
     */
    static constexpr std::string_view delay_type = "delay";
    static constexpr std::string_view exception_type = "exception";
    static constexpr std::string_view terminate_type = "terminate";

    register_route<superuser>(
      ss::httpd::hbadger_json::set_failure_probe,
      [](std::unique_ptr<ss::http::request> req) {
          auto m = req->param["module"];
          auto p = req->param["point"];
          auto type = req->param["type"];
          vlog(
            adminlog.info,
            "Request to set failure probe of type '{}' in  '{}' at point "
            "'{}'",
            type,
            m,
            p);
          auto f = ss::now();

          if (type == delay_type) {
              f = ss::smp::invoke_on_all(
                [m, p] { finjector::shard_local_badger().set_delay(m, p); });
          } else if (type == exception_type) {
              f = ss::smp::invoke_on_all([m, p] {
                  finjector::shard_local_badger().set_exception(m, p);
              });
          } else if (type == terminate_type) {
              f = ss::smp::invoke_on_all([m, p] {
                  finjector::shard_local_badger().set_termination(m, p);
              });
          } else {
              throw ss::httpd::bad_param_exception(fmt::format(
                "Type parameter has to be one of "
                "['{}','{}','{}']",
                delay_type,
                exception_type,
                terminate_type));
          }

          return f.then(
            [] { return ss::json::json_return_type(ss::json::json_void()); });
      });
    /*
     * Remove all failure injectors at given point
     */
    register_route<superuser>(
      ss::httpd::hbadger_json::delete_failure_probe,
      [](std::unique_ptr<ss::http::request> req) {
          auto m = req->param["module"];
          auto p = req->param["point"];
          vlog(
            adminlog.info,
            "Request to unset failure probe '{}' at point '{}'",
            m,
            p);
          return ss::smp::invoke_on_all(
                   [m, p] { finjector::shard_local_badger().unset(m, p); })
            .then(
              [] { return ss::json::json_return_type(ss::json::json_void()); });
      });
}

namespace {
json::validator make_self_test_start_validator() {
    const std::string schema = R"(
{
    "type": "object",
    "properties": {
        "nodes": {
            "type": "array",
            "items": {
                "type": "number"
            }
        },
        "tests": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "type": {
                        "type": "string"
                    }
                },
                "required": ["type"]
            }
        }
    },
    "required": [],
    "additionalProperties": false
}
)";
    return json::validator(schema);
}
} // namespace

ss::future<ss::json::json_return_type>
admin_server::self_test_start_handler(std::unique_ptr<ss::http::request> req) {
    static thread_local json::validator self_test_start_validator(
      make_self_test_start_validator());
    if (need_redirect_to_leader(model::controller_ntp, _metadata_cache)) {
        vlog(adminlog.debug, "Need to redirect self_test_start request");
        throw co_await redirect_to_leader(*req, model::controller_ntp);
    }
    auto doc = co_await parse_json_body(req.get());
    apply_validator(self_test_start_validator, doc);
    std::vector<model::node_id> ids;
    cluster::start_test_request r;
    if (!doc.IsNull()) {
        if (doc.HasMember("nodes")) {
            const auto& node_ids = doc["nodes"].GetArray();
            for (const auto& element : node_ids) {
                ids.emplace_back(element.GetInt());
            }
        } else {
            /// If not provided, default is to start the test on all nodes
            ids = _controller->get_members_table().local().node_ids();
        }
        if (doc.HasMember("tests")) {
            const auto& params = doc["tests"].GetArray();
            for (const auto& element : params) {
                const auto& obj = element.GetObject();
                const ss::sstring test_type(obj["type"].GetString());
                if (test_type == "disk") {
                    r.dtos.push_back(cluster::diskcheck_opts::from_json(obj));
                } else if (test_type == "network") {
                    r.ntos.push_back(cluster::netcheck_opts::from_json(obj));
                } else {
                    throw ss::httpd::bad_param_exception(
                      "Unknown self_test 'type', valid options are 'disk' or "
                      "'network'");
                }
            }
        } else {
            /// Default test run is to start 1 disk and 1 network test with
            /// default arguments
            r.dtos.push_back(cluster::diskcheck_opts{});
            r.ntos.push_back(cluster::netcheck_opts{});
        }
    }
    try {
        auto tid = co_await _self_test_frontend.invoke_on(
          cluster::self_test_frontend::shard,
          [r, ids](auto& self_test_frontend) {
              return self_test_frontend.start_test(r, ids);
          });
        vlog(adminlog.info, "Request to start self test succeeded: {}", tid);
        co_return ss::json::json_return_type(tid);
    } catch (const std::exception& ex) {
        throw ss::httpd::base_exception(
          fmt::format("Failed to start self test, reason: {}", ex),
          ss::http::reply::status_type::service_unavailable);
    }
}

ss::future<ss::json::json_return_type>
admin_server::self_test_stop_handler(std::unique_ptr<ss::http::request> req) {
    if (need_redirect_to_leader(model::controller_ntp, _metadata_cache)) {
        vlog(adminlog.info, "Need to redirect self_test_stop request");
        throw co_await redirect_to_leader(*req, model::controller_ntp);
    }
    auto r = co_await _self_test_frontend.invoke_on(
      cluster::self_test_frontend::shard,
      [](auto& self_test_frontend) { return self_test_frontend.stop_test(); });
    if (!r.finished()) {
        throw ss::httpd::base_exception(
          fmt::format(
            "Failed to stop one or more self_test jobs: {}",
            r.active_participant_ids()),
          ss::http::reply::status_type::service_unavailable);
    }
    vlog(adminlog.info, "Request to stop self test succeeded");
    co_return ss::json::json_void();
}

namespace {
ss::httpd::debug_json::self_test_result
self_test_result_to_json(const cluster::self_test_result& str) {
    ss::httpd::debug_json::self_test_result r;
    r.test_id = ss::sstring(str.test_id);
    r.name = str.name;
    r.info = str.info;
    r.test_type = str.test_type;
    r.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                   str.duration)
                   .count();
    r.timeouts = str.timeouts;
    if (str.warning) {
        r.warning = *str.warning;
    }
    if (str.error) {
        r.error = *str.error;
        return r;
    }
    r.p50 = str.p50;
    r.p90 = str.p90;
    r.p99 = str.p99;
    r.p999 = str.p999;
    r.max_latency = str.max;
    r.rps = str.rps;
    r.bps = str.bps;
    return r;
}
} // namespace

ss::future<ss::json::json_return_type>
admin_server::self_test_get_results_handler(
  std::unique_ptr<ss::http::request>) {
    namespace dbg_ns = ss::httpd::debug_json;
    std::vector<dbg_ns::self_test_node_report> reports;
    auto status = co_await _self_test_frontend.invoke_on(
      cluster::self_test_frontend::shard,
      [](auto& self_test_frontend) { return self_test_frontend.status(); });
    reports.reserve(status.results().size());
    for (const auto& [id, participant] : status.results()) {
        dbg_ns::self_test_node_report nr;
        nr.node_id = id;
        nr.status = cluster::self_test_status_as_string(participant.status());
        if (participant.response) {
            for (const auto& r : participant.response->results) {
                nr.results.push(self_test_result_to_json(r));
            }
        }
        reports.push_back(nr);
    }
    co_return ss::json::json_return_type(reports);
}

void admin_server::register_self_test_routes() {
    register_route<superuser>(
      ss::httpd::debug_json::self_test_start,
      [this](std::unique_ptr<ss::http::request> req) {
          return self_test_start_handler(std::move(req));
      });

    register_route<superuser>(
      ss::httpd::debug_json::self_test_stop,
      [this](std::unique_ptr<ss::http::request> req) {
          return self_test_stop_handler(std::move(req));
      });

    register_route<user>(
      ss::httpd::debug_json::self_test_status,
      [this](std::unique_ptr<ss::http::request> req) {
          return self_test_get_results_handler(std::move(req));
      });
}

namespace {
storage::node::disk_type resolve_disk_type(std::string_view name) {
    if (name == "data") {
        return storage::node::disk_type::data;
    } else if (name == "cache") {
        return storage::node::disk_type::cache;
    } else {
        throw ss::httpd::bad_param_exception(
          fmt::format("Unknown disk type: {}", name));
    }
}
} // namespace

ss::future<ss::json::json_return_type>
admin_server::get_disk_stat_handler(std::unique_ptr<ss::http::request> req) {
    auto type = resolve_disk_type(req->param["type"]);

    // get effective disk stat
    auto stat = co_await _storage_node.invoke_on(
      0, [type](auto& node) { return node.get_statvfs(type); });

    ss::httpd::debug_json::disk_stat disk;
    disk.total_bytes = stat.stat.f_blocks * stat.stat.f_frsize;
    disk.free_bytes = stat.stat.f_bfree * stat.stat.f_frsize;

    co_return disk;
}

namespace {
json::validator make_disk_stat_overrides_validator() {
    const std::string schema = R"(
{
    "type": "object",
    "properties": {
        "total_bytes": {
            "type": "integer"
        },
        "free_bytes": {
            "type": "integer"
        },
        "free_bytes_delta": {
            "type": "integer"
        }
    },
    "additionalProperties": false
}
)";
    return json::validator(schema);
}
} // namespace

ss::future<ss::json::json_return_type>
admin_server::put_disk_stat_handler(std::unique_ptr<ss::http::request> req) {
    static thread_local auto disk_stat_validator(
      make_disk_stat_overrides_validator());

    auto doc = co_await parse_json_body(req.get());
    apply_validator(disk_stat_validator, doc);
    auto type = resolve_disk_type(req->param["type"]);

    storage::node::statvfs_overrides overrides;
    if (doc.HasMember("total_bytes")) {
        overrides.total_bytes = doc["total_bytes"].GetUint64();
    }
    if (doc.HasMember("free_bytes")) {
        overrides.free_bytes = doc["free_bytes"].GetUint64();
    }
    if (doc.HasMember("free_bytes_delta")) {
        overrides.free_bytes_delta = doc["free_bytes_delta"].GetInt64();
    }

    co_await _storage_node.invoke_on(
      storage::node::work_shard, [type, overrides](auto& node) {
          node.set_statvfs_overrides(type, overrides);
          return ss::now();
      });

    co_return ss::json::json_void();
}

ss::future<ss::json::json_return_type>
admin_server::get_partition_balancer_status_handler(
  std::unique_ptr<ss::http::request> req) {
    vlog(adminlog.debug, "Requested partition balancer status");

    using result_t = std::variant<
      cluster::partition_balancer_overview_reply,
      model::node_id,
      cluster::errc>;

    result_t result = co_await _controller->get_partition_balancer().invoke_on(
      cluster::partition_balancer_backend::shard,
      [](cluster::partition_balancer_backend& backend) {
          if (backend.is_leader()) {
              return result_t(backend.overview());
          } else {
              auto leader_id = backend.leader_id();
              if (leader_id) {
                  return result_t(leader_id.value());
              } else {
                  return result_t(cluster::errc::no_leader_controller);
              }
          }
      });

    cluster::partition_balancer_overview_reply overview;
    if (std::holds_alternative<cluster::partition_balancer_overview_reply>(
          result)) {
        overview = std::move(
          std::get<cluster::partition_balancer_overview_reply>(result));
    } else if (std::holds_alternative<model::node_id>(result)) {
        auto node_id = std::get<model::node_id>(result);
        vlog(
          adminlog.debug,
          "proxying the partition_balancer_overview call to node {}",
          node_id);
        auto rpc_result
          = co_await _connection_cache.local()
              .with_node_client<
                cluster::partition_balancer_rpc_client_protocol>(
                _controller->self(),
                ss::this_shard_id(),
                node_id,
                5s,
                [](cluster::partition_balancer_rpc_client_protocol cp) {
                    return cp.overview(
                      cluster::partition_balancer_overview_request{},
                      rpc::client_opts(5s));
                });

        if (rpc_result.has_error()) {
            co_await throw_on_error(
              *req, rpc_result.error(), model::controller_ntp);
        }

        overview = std::move(rpc_result.value().data);
    } else {
        co_await throw_on_error(
          *req, std::get<cluster::errc>(result), model::controller_ntp);
    }

    ss::httpd::cluster_json::partition_balancer_status ret;

    if (overview.error == cluster::errc::feature_disabled) {
        ret.status = "off";
        co_return ss::json::json_return_type(ret);
    } else if (overview.error != cluster::errc::success) {
        co_await throw_on_error(*req, overview.error, model::controller_ntp);
    }

    ret.status = fmt::format("{}", overview.status);

    if (overview.last_tick_time != model::timestamp::missing()) {
        ret.seconds_since_last_tick = (model::timestamp::now().value()
                                       - overview.last_tick_time.value())
                                      / 1000;
    }

    if (overview.violations) {
        ss::httpd::cluster_json::partition_balancer_violations ret_violations;
        for (const auto& n : overview.violations->unavailable_nodes) {
            ret_violations.unavailable_nodes.push(n.id);
        }
        for (const auto& n : overview.violations->full_nodes) {
            ret_violations.over_disk_limit_nodes.push(n.id);
        }
        ret.violations = ret_violations;
    }

    ret.current_reassignments_count
      = _controller->get_topics_state().local().updates_in_progress().size();

    ret.partitions_pending_force_recovery_count
      = overview.partitions_pending_force_recovery_count;
    for (const auto& ntp : overview.partitions_pending_force_recovery_sample) {
        ret.partitions_pending_force_recovery_sample.push(fmt::format(
          "{}/{}/{}", ntp.ns(), ntp.tp.topic(), ntp.tp.partition()));
    }

    co_return ss::json::json_return_type(ret);
}

ss::future<ss::json::json_return_type>
admin_server::cancel_all_partitions_reconfigs_handler(
  std::unique_ptr<ss::http::request> req) {
    vlog(
      adminlog.info,
      "Requested cancellation of all ongoing partition movements");

    auto res = co_await _controller->get_topics_frontend()
                 .local()
                 .cancel_moving_all_partition_replicas(
                   model::timeout_clock::now() + 5s);
    if (res.has_error()) {
        co_await throw_on_error(*req, res.error(), model::controller_ntp);
    }

    co_return ss::json::json_return_type(
      co_await map_partition_results(std::move(res.value())));
}

static json::validator make_post_cluster_partitions_validator() {
    const std::string schema = R"(
{
    "type": "object",
    "properties": {
        "disabled": {
            "type": "boolean"
        }
    },
    "additionalProperties": false,
    "required": ["disabled"]
}
)";
    return json::validator(schema);
}

ss::future<ss::json::json_return_type>
admin_server::post_cluster_partitions_topic_handler(
  std::unique_ptr<ss::http::request> req) {
    if (need_redirect_to_leader(model::controller_ntp, _metadata_cache)) {
        // In order that we can do a reliably ordered validation of
        // the request (and drop no-op requests), run on controller leader;
        throw co_await redirect_to_leader(*req, model::controller_ntp);
    }

    auto ns_tp = model::topic_namespace{
      model::ns{req->param["namespace"]}, model::topic{req->param["topic"]}};

    static thread_local auto body_validator(
      make_post_cluster_partitions_validator());
    auto doc = co_await parse_json_body(req.get());
    apply_validator(body_validator, doc);
    bool disabled = doc["disabled"].GetBool();

    std::error_code err
      = co_await _controller->get_topics_frontend()
          .local()
          .set_topic_partitions_disabled(
            ns_tp, std::nullopt, disabled, model::timeout_clock::now() + 5s);
    if (err) {
        co_await throw_on_error(*req, err, model::controller_ntp);
    }

    co_return ss::json::json_void();
}

ss::future<ss::json::json_return_type>
admin_server::post_cluster_partitions_topic_partition_handler(
  std::unique_ptr<ss::http::request> req) {
    if (need_redirect_to_leader(model::controller_ntp, _metadata_cache)) {
        // In order that we can do a reliably ordered validation of
        // the request (and drop no-op requests), run on controller leader;
        throw co_await redirect_to_leader(*req, model::controller_ntp);
    }

    auto ntp = parse_ntp_from_request(req->param);

    static thread_local auto body_validator(
      make_post_cluster_partitions_validator());
    auto doc = co_await parse_json_body(req.get());
    apply_validator(body_validator, doc);
    bool disabled = doc["disabled"].GetBool();

    std::error_code err = co_await _controller->get_topics_frontend()
                            .local()
                            .set_topic_partitions_disabled(
                              model::topic_namespace_view{ntp},
                              ntp.tp.partition,
                              disabled,
                              model::timeout_clock::now() + 5s);
    if (err) {
        co_await throw_on_error(*req, err, model::controller_ntp);
    }

    co_return ss::json::json_void();
}

namespace {

struct cluster_partition_info {
    ss::lw_shared_ptr<model::topic_namespace> ns_tp;
    model::partition_id id;
    std::vector<model::broker_shard> replicas;
    bool disabled = false;

    ss::httpd::cluster_json::cluster_partition to_json() const {
        ss::httpd::cluster_json::cluster_partition ret;
        ret.ns = ns_tp->ns();
        ret.topic = ns_tp->tp();
        ret.partition_id = id();
        for (auto& r : replicas) {
            ss::httpd::cluster_json::replica_assignment a;
            a.node_id = r.node_id;
            a.core = r.shard;
            ret.replicas.push(a);
        }
        ret.disabled = disabled;
        return ret;
    }
};

fragmented_vector<cluster_partition_info> topic2cluster_partitions(
  model::topic_namespace ns_tp,
  const cluster::assignments_set& assignments,
  const cluster::topic_disabled_partitions_set* disabled_set,
  std::optional<bool> disabled_filter) {
    fragmented_vector<cluster_partition_info> ret;

    if (disabled_filter) {
        // fast exits
        if (
          disabled_filter.value()
          && (!disabled_set || disabled_set->is_fully_enabled())) {
            return ret;
        }

        if (
          !disabled_filter.value() && disabled_set
          && disabled_set->is_fully_disabled()) {
            return ret;
        }
    }

    auto shared_ns_tp = ss::make_lw_shared<model::topic_namespace>(
      std::move(ns_tp));

    if (
      disabled_filter && disabled_filter.value() && disabled_set
      && disabled_set->partitions) {
        // special handling for disabled=true filter, as we hope that iterating
        // over the disabled set is more optimal.
        for (const auto& id : *disabled_set->partitions) {
            auto as_it = assignments.find(id);
            vassert(
              as_it != assignments.end(),
              "topic: {}, partition {} must be present",
              *shared_ns_tp,
              id);

            ret.push_back(cluster_partition_info{
              .ns_tp = shared_ns_tp,
              .id = id,
              .replicas = as_it->replicas,
              .disabled = true,
            });
        }
    } else {
        for (const auto& p_as : assignments) {
            bool disabled = disabled_set && disabled_set->is_disabled(p_as.id);

            if (disabled_filter && *disabled_filter != disabled) {
                continue;
            }

            ret.push_back(cluster_partition_info{
              .ns_tp = shared_ns_tp,
              .id = p_as.id,
              .replicas = p_as.replicas,
              .disabled = disabled,
            });
        }
    }

    std::sort(ret.begin(), ret.end(), [](const auto& l, const auto& r) {
        return l.id < r.id;
    });

    return ret;
}

} // namespace

ss::future<ss::json::json_return_type>
admin_server::get_cluster_partitions_handler(
  std::unique_ptr<ss::http::request> req) {
    std::optional<bool> disabled_filter;
    if (req->query_parameters.contains("disabled")) {
        disabled_filter = get_boolean_query_param(*req, "disabled");
    }

    bool with_internal = get_boolean_query_param(*req, "with_internal");

    const auto& topics_state = _controller->get_topics_state().local();

    fragmented_vector<model::topic_namespace> topics;
    auto fill_topics = [&](const auto& map) {
        for (const auto& [ns_tp, _] : map) {
            if (!with_internal && !model::is_user_topic(ns_tp)) {
                continue;
            }
            topics.push_back(ns_tp);
        }
    };

    if (disabled_filter && *disabled_filter) {
        // optimization: if disabled filter is on, iterate only over disabled
        // topics;
        fill_topics(topics_state.get_disabled_partitions());
    } else {
        fill_topics(topics_state.topics_map());
    }

    std::sort(topics.begin(), topics.end());

    ss::chunked_fifo<cluster_partition_info> partitions;
    for (const auto& ns_tp : topics) {
        auto topic_it = topics_state.topics_map().find(ns_tp);
        if (topic_it == topics_state.topics_map().end()) {
            // probably got deleted while we were iterating.
            continue;
        }

        auto topic_partitions = topic2cluster_partitions(
          ns_tp,
          topic_it->second.get_assignments(),
          topics_state.get_topic_disabled_set(ns_tp),
          disabled_filter);

        std::move(
          topic_partitions.begin(),
          topic_partitions.end(),
          std::back_inserter(partitions));

        co_await ss::coroutine::maybe_yield();
    }

    co_return ss::json::json_return_type(ss::json::stream_range_as_array(
      lw_shared_container{std::move(partitions)},
      [](const auto& p) { return p.to_json(); }));
}

ss::future<ss::json::json_return_type>
admin_server::get_cluster_partitions_topic_handler(
  std::unique_ptr<ss::http::request> req) {
    auto ns_tp = model::topic_namespace{
      model::ns{req->param["namespace"]}, model::topic{req->param["topic"]}};

    std::optional<bool> disabled_filter;
    if (req->query_parameters.contains("disabled")) {
        disabled_filter = get_boolean_query_param(*req, "disabled");
    }

    const auto& topics_state = _controller->get_topics_state().local();

    auto topic_it = topics_state.topics_map().find(ns_tp);
    if (topic_it == topics_state.topics_map().end()) {
        throw ss::httpd::not_found_exception(
          fmt::format("topic {} not found", ns_tp));
    }

    auto partitions = topic2cluster_partitions(
      ns_tp,
      topic_it->second.get_assignments(),
      topics_state.get_topic_disabled_set(ns_tp),
      disabled_filter);

    co_return ss::json::json_return_type(ss::json::stream_range_as_array(
      lw_shared_container{std::move(partitions)},
      [](const auto& p) { return p.to_json(); }));
}

void admin_server::register_cluster_routes() {
    register_route<publik>(
      ss::httpd::cluster_json::get_cluster_health_overview,
      [this](std::unique_ptr<ss::http::request>) {
          vlog(adminlog.debug, "Requested cluster status");
          return _controller->get_health_monitor()
            .local()
            .get_cluster_health_overview(
              model::time_from_now(std::chrono::seconds(5)))
            .then([](auto health_overview) {
                ss::httpd::cluster_json::cluster_health_overview ret;
                ret.is_healthy = health_overview.is_healthy();

                ret.unhealthy_reasons._set = true;
                ret.all_nodes._set = true;
                ret.nodes_down._set = true;
                ret.leaderless_partitions._set = true;
                ret.under_replicated_partitions._set = true;

                ret.unhealthy_reasons = health_overview.unhealthy_reasons;
                ret.all_nodes = health_overview.all_nodes;
                ret.nodes_down = health_overview.nodes_down;
                ret.nodes_in_recovery_mode
                  = health_overview.nodes_in_recovery_mode;

                ret.leaderless_count = health_overview.leaderless_count;
                ret.under_replicated_count
                  = health_overview.under_replicated_count;

                for (auto& ntp : health_overview.leaderless_partitions) {
                    ret.leaderless_partitions.push(fmt::format(
                      "{}/{}/{}", ntp.ns(), ntp.tp.topic(), ntp.tp.partition));
                }
                for (auto& ntp : health_overview.under_replicated_partitions) {
                    ret.under_replicated_partitions.push(fmt::format(
                      "{}/{}/{}", ntp.ns(), ntp.tp.topic(), ntp.tp.partition));
                }
                if (health_overview.controller_id) {
                    ret.controller_id = health_overview.controller_id.value();
                } else {
                    ret.controller_id = -1;
                }
                if (health_overview.bytes_in_cloud_storage) {
                    ret.bytes_in_cloud_storage
                      = health_overview.bytes_in_cloud_storage.value();
                } else {
                    ret.bytes_in_cloud_storage = -1;
                }

                return ss::json::json_return_type(ret);
            });
      });

    register_route<publik>(
      ss::httpd::cluster_json::get_partition_balancer_status,
      [this](std::unique_ptr<ss::http::request> req) {
          return get_partition_balancer_status_handler(std::move(req));
      });

    register_route<superuser>(
      ss::httpd::cluster_json::cancel_all_partitions_reconfigurations,
      [this](std::unique_ptr<ss::http::request> req) {
          return cancel_all_partitions_reconfigs_handler(std::move(req));
      });

    register_route_sync<publik>(
      ss::httpd::cluster_json::get_cluster_uuid,
      [this](ss::httpd::const_req) -> ss::json::json_return_type {
          vlog(adminlog.debug, "Requested cluster UUID");
          const std::optional<model::cluster_uuid>& cluster_uuid
            = _controller->get_storage().local().get_cluster_uuid();
          if (cluster_uuid) {
              ss::httpd::cluster_json::uuid ret;
              ret.cluster_uuid = ssx::sformat("{}", cluster_uuid);
              return ss::json::json_return_type(std::move(ret));
          }
          return ss::json::json_return_type(ss::json::json_void());
      });

    register_cluster_partitions_routes();
}

void admin_server::register_cluster_partitions_routes() {
    register_route<superuser>(
      ss::httpd::cluster_json::post_cluster_partitions_topic,
      [this](std::unique_ptr<ss::http::request> req) {
          return post_cluster_partitions_topic_handler(std::move(req));
      });
    register_route<superuser>(
      ss::httpd::cluster_json::post_cluster_partitions_topic_partition,
      [this](std::unique_ptr<ss::http::request> req) {
          return post_cluster_partitions_topic_partition_handler(
            std::move(req));
      });

    // The following GET routes provide APIs for getting high-level partition
    // info known to all cluster nodes.

    register_route<user>(
      ss::httpd::cluster_json::get_cluster_partitions,
      [this](std::unique_ptr<ss::http::request> req) {
          return get_cluster_partitions_handler(std::move(req));
      });
    register_route<user>(
      ss::httpd::cluster_json::get_cluster_partitions_topic,
      [this](std::unique_ptr<ss::http::request> req) {
          return get_cluster_partitions_topic_handler(std::move(req));
      });
}

ss::future<ss::json::json_return_type> admin_server::sync_local_state_handler(
  std::unique_ptr<ss::http::request> request) {
    struct manifest_reducer {
        ss::future<>
        operator()(std::optional<cloud_storage::partition_manifest>&& value) {
            _manifest = std::move(value);
            return ss::make_ready_future<>();
        }
        std::optional<cloud_storage::partition_manifest> get() && {
            return std::move(_manifest);
        }
        std::optional<cloud_storage::partition_manifest> _manifest;
    };

    vlog(adminlog.info, "Requested bucket syncup");
    auto ntp = parse_ntp_from_request(request->param, model::kafka_namespace);
    if (need_redirect_to_leader(ntp, _metadata_cache)) {
        vlog(adminlog.info, "Need to redirect bucket syncup request");
        throw co_await redirect_to_leader(*request, ntp);
    } else {
        auto result = co_await _partition_manager.map_reduce(
          manifest_reducer(), [ntp](cluster::partition_manager& p) {
              auto partition = p.get(ntp);
              if (partition) {
                  auto archiver = partition->archiver();
                  if (archiver) {
                      return archiver.value().get().maybe_truncate_manifest();
                  }
              }
              return ss::make_ready_future<
                std::optional<cloud_storage::partition_manifest>>(std::nullopt);
          });
        vlog(adminlog.info, "Requested bucket syncup completed");
        if (result) {
            std::stringstream sts;
            result->serialize_json(sts);
            vlog(adminlog.info, "Requested bucket syncup result {}", sts.str());
        } else {
            vlog(adminlog.info, "Requested bucket syncup result empty");
        }
    }
    co_return ss::json::json_return_type(ss::json::json_void());
}

ss::future<std::unique_ptr<ss::http::reply>>
admin_server::unsafe_reset_metadata(
  std::unique_ptr<ss::http::request> request,
  std::unique_ptr<ss::http::reply> reply) {
    reply->set_content_type("json");

    auto ntp = parse_ntp_from_request(request->param, model::kafka_namespace);
    if (need_redirect_to_leader(ntp, _metadata_cache)) {
        vlog(adminlog.info, "Need to redirect unsafe reset metadata request");
        throw co_await redirect_to_leader(*request, ntp);
    }
    if (request->content_length <= 0) {
        throw ss::httpd::bad_request_exception("Empty request content");
    }

    ss::sstring content = co_await ss::util::read_entire_stream_contiguous(
      *request->content_stream);

    const auto shard = _shard_table.local().shard_for(ntp);
    if (!shard) {
        throw ss::httpd::not_found_exception(fmt::format(
          "{} could not be found on the node. Perhaps it has been moved "
          "during the redirect.",
          ntp));
    }

    try {
        co_await _partition_manager.invoke_on(
          *shard,
          [ntp = std::move(ntp), content = std::move(content), shard](
            auto& pm) mutable {
              auto partition = pm.get(ntp);
              if (!partition) {
                  throw ss::httpd::not_found_exception(
                    fmt::format("Could not find {} on shard {}", ntp, *shard));
              }

              iobuf buf;
              buf.append(content.data(), content.size());
              content = {};

              return partition
                ->unsafe_reset_remote_partition_manifest_from_json(
                  std::move(buf));
          });
    } catch (const std::runtime_error& err) {
        throw ss::httpd::server_error_exception(err.what());
    }

    reply->set_status(ss::http::reply::status_type::ok);
    co_return reply;
}

ss::future<std::unique_ptr<ss::http::reply>>
admin_server::initiate_topic_scan_and_recovery(
  std::unique_ptr<ss::http::request> request,
  std::unique_ptr<ss::http::reply> reply) {
    reply->set_content_type("json");

    if (need_redirect_to_leader(model::controller_ntp, _metadata_cache)) {
        throw co_await redirect_to_leader(*request, model::controller_ntp);
    }

    if (!_topic_recovery_service.local_is_initialized()) {
        throw ss::httpd::bad_request_exception(
          "Topic recovery is not available. is cloud storage enabled?");
    }

    auto result = co_await _topic_recovery_service.invoke_on(
      cloud_storage::topic_recovery_service::shard_id,
      [&request](auto& svc) { return svc.start_recovery(*request); });

    if (result.status_code != ss::http::reply::status_type::accepted) {
        throw ss::httpd::base_exception{result.message, result.status_code};
    }

    auto payload = ss::httpd::shadow_indexing_json::init_recovery_result{};
    payload.status = result.message;

    reply->set_status(result.status_code, payload.to_json());
    co_return reply;
}

namespace {
ss::httpd::shadow_indexing_json::topic_recovery_status
map_status_to_json(cluster::single_status status) {
    ss::httpd::shadow_indexing_json::topic_recovery_status status_json;
    status_json.state = fmt::format("{}", status.state);

    for (const auto& count : status.download_counts) {
        ss::httpd::shadow_indexing_json::topic_download_counts c;
        c.topic_namespace = fmt::format("{}", count.tp_ns);
        c.pending_downloads = count.pending_downloads;
        c.successful_downloads = count.successful_downloads;
        c.failed_downloads = count.failed_downloads;
        status_json.topic_download_counts.push(c);
    }

    ss::httpd::shadow_indexing_json::recovery_request_params r;
    r.topic_names_pattern = status.request.topic_names_pattern.value_or("none");
    r.retention_bytes = status.request.retention_bytes.value_or(-1);
    r.retention_ms = status.request.retention_ms.value_or(-1ms).count();
    status_json.request = r;

    return status_json;
}

ss::json::json_return_type serialize_topic_recovery_status(
  const cluster::status_response& cluster_status, bool extended) {
    if (!extended) {
        return map_status_to_json(cluster_status.status_log.back());
    }

    std::vector<ss::httpd::shadow_indexing_json::topic_recovery_status>
      status_log;
    status_log.reserve(cluster_status.status_log.size());
    for (const auto& entry : cluster_status.status_log) {
        status_log.push_back(map_status_to_json(entry));
    }

    return status_log;
}
} // namespace

ss::future<std::unique_ptr<ss::http::reply>>
admin_server::initialize_cluster_recovery(
  std::unique_ptr<ss::http::request> request,
  std::unique_ptr<ss::http::reply> reply) {
    reply->set_content_type("json");
    if (need_redirect_to_leader(model::controller_ntp, _metadata_cache)) {
        throw co_await redirect_to_leader(*request, model::controller_ntp);
    }
    auto& bucket_property = cloud_storage::configuration::get_bucket_config();
    if (!bucket_property.is_overriden() || !bucket_property().has_value()) {
        throw ss::httpd::bad_request_exception(
          "Cluster recovery is not available. Missing bucket property");
    }
    cloud_storage_clients::bucket_name bucket(bucket_property().value());
    auto result = ss::httpd::shadow_indexing_json::init_recovery_result{};
    auto error_res
      = co_await _controller->get_cluster_recovery_manager().invoke_on(
        cluster::cluster_recovery_manager::shard,
        [bucket](auto& mgr) { return mgr.initialize_recovery(bucket); });
    if (error_res.has_error()) {
        throw ss::httpd::base_exception{
          ssx::sformat(
            "Error starting cluster recovery request: {}", error_res.error()),
          ss::http::reply::status_type::internal_server_error};
    }
    auto err = error_res.value();
    if (err == cluster::errc::not_leader_controller) {
        throw co_await redirect_to_leader(*request, model::controller_ntp);
    }
    if (err == cluster::errc::cluster_already_exists) {
        throw ss::httpd::base_exception{
          "Recovery already active", ss::http::reply::status_type::conflict};
    }
    if (err == cluster::errc::invalid_request) {
        throw ss::httpd::base_exception{
          "Cloud storage not available",
          ss::http::reply::status_type::bad_request};
    }
    // Generic other errors. Just give up and throw.
    if (err != cluster::errc::success) {
        throw ss::httpd::base_exception{
          "Error starting cluster recovery request",
          ss::http::reply::status_type::internal_server_error};
    }

    result.status = "Recovery initialized";
    reply->set_status(ss::http::reply::status_type::accepted, result.to_json());
    co_return reply;
}
ss::future<ss::json::json_return_type>
admin_server::get_cluster_recovery(std::unique_ptr<ss::http::request> req) {
    ss::httpd::shadow_indexing_json::cluster_recovery_status ret;
    ret.state = "inactive";

    auto latest_recovery
      = _controller->get_cluster_recovery_table().local().current_recovery();
    if (
      !latest_recovery.has_value()
      || latest_recovery.value().get().stage
           == cluster::recovery_stage::complete) {
        co_return ret;
    }
    auto& recovery = latest_recovery.value().get();
    ret.state = ssx::sformat("{}", recovery.stage);
    if (recovery.error_msg.has_value()) {
        ret.error = recovery.error_msg.value();
    }
    co_return ret;
}

ss::future<ss::json::json_return_type>
admin_server::query_automated_recovery(std::unique_ptr<ss::http::request> req) {
    ss::httpd::shadow_indexing_json::topic_recovery_status ret;
    ret.state = "inactive";

    if (
      !_topic_recovery_status_frontend.local_is_initialized()
      || !_topic_recovery_service.local_is_initialized()) {
        co_return ret;
    }

    auto controller_leader = _metadata_cache.local().get_leader_id(
      model::controller_ntp);

    if (!controller_leader) {
        throw ss::httpd::server_error_exception{
          "Unable to get controller leader, cannot get recovery status"};
    }

    auto extended = get_boolean_query_param(*req, "extended");
    if (controller_leader.value() == config::node().node_id.value()) {
        auto status_log = co_await _topic_recovery_service.invoke_on(
          cloud_storage::topic_recovery_service::shard_id,
          [](auto& svc) { return svc.recovery_status_log(); });
        co_return serialize_topic_recovery_status(
          cluster::map_log_to_response(std::move(status_log)), extended);
    }

    if (auto status = co_await _topic_recovery_status_frontend.local().status(
          controller_leader.value());
        status.has_value()) {
        co_return serialize_topic_recovery_status(status.value(), extended);
    }

    co_return ret;
}

namespace {
ss::httpd::shadow_indexing_json::partition_cloud_storage_status
map_status_to_json(cluster::partition_cloud_storage_status status) {
    ss::httpd::shadow_indexing_json::partition_cloud_storage_status json;

    json.cloud_storage_mode = fmt::format("{}", status.mode);

    if (status.since_last_manifest_upload) {
        json.ms_since_last_manifest_upload
          = status.since_last_manifest_upload->count();
    }
    if (status.since_last_segment_upload) {
        json.ms_since_last_segment_upload
          = status.since_last_segment_upload->count();
    }
    if (status.since_last_manifest_sync) {
        json.ms_since_last_manifest_sync
          = status.since_last_manifest_sync->count();
    }

    json.metadata_update_pending = status.cloud_metadata_update_pending;

    json.total_log_size_bytes = status.total_log_size_bytes;
    json.cloud_log_size_bytes = status.cloud_log_size_bytes;
    json.stm_region_size_bytes = status.stm_region_size_bytes;
    json.archive_size_bytes = status.archive_size_bytes;
    json.local_log_size_bytes = status.local_log_size_bytes;
    json.stm_region_segment_count = status.stm_region_segment_count;
    // TODO: add spillover segments.
    json.cloud_log_segment_count = status.stm_region_segment_count;
    json.local_log_segment_count = status.local_log_segment_count;

    if (status.cloud_log_start_offset) {
        json.cloud_log_start_offset = status.cloud_log_start_offset.value()();
    }
    if (status.stm_region_start_offset) {
        json.stm_region_start_offset = status.stm_region_start_offset.value()();
    }
    if (status.cloud_log_last_offset) {
        json.cloud_log_last_offset = status.cloud_log_last_offset.value()();
    }
    if (status.local_log_start_offset) {
        json.local_log_start_offset = status.local_log_start_offset.value()();
    }
    if (status.local_log_last_offset) {
        json.local_log_last_offset = status.local_log_last_offset.value()();
    }

    return json;
}

ss::httpd::shadow_indexing_json::segment_meta
map_segment_meta_to_json(const cloud_storage::segment_meta& meta) {
    ss::httpd::shadow_indexing_json::segment_meta json;
    json.base_offset = meta.base_offset();
    json.committed_offset = meta.committed_offset();

    if (meta.delta_offset != model::offset_delta{}) {
        json.delta_offset = meta.delta_offset;
    }
    if (meta.delta_offset_end != model::offset_delta{}) {
        json.delta_offset_end = meta.delta_offset_end;
    }

    json.base_timestamp = meta.base_timestamp();
    json.max_timestamp = meta.max_timestamp();

    json.size_bytes = meta.size_bytes;
    json.is_compacted = meta.is_compacted;

    json.archiver_term = meta.archiver_term();
    json.segment_term = meta.segment_term();
    json.ntp_revision = meta.ntp_revision();

    return json;
}

ss::httpd::shadow_indexing_json::metadata_anomaly
map_metadata_anomaly_to_json(const cloud_storage::anomaly_meta& meta) {
    ss::httpd::shadow_indexing_json::metadata_anomaly json;
    switch (meta.type) {
    case cloud_storage::anomaly_type::missing_delta: {
        json.type = "missing_delta";
        json.explanation = "Segment is missing delta offset";
        json.at_segment = map_segment_meta_to_json(meta.at);
        if (meta.previous) {
            json.previous_segment = map_segment_meta_to_json(*meta.previous);
        }

        break;
    }
    case cloud_storage::anomaly_type::non_monotonical_delta: {
        if (!meta.previous) {
            vlog(
              adminlog.error,
              "Invalid anomaly metadata of type {} at {}",
              meta.type,
              meta.at);
            return json;
        }

        json.type = "non_monotonical_delta";
        json.explanation = ssx::sformat(
          "Segment has lower delta than previous: {} < {}",
          meta.at.delta_offset,
          meta.previous->delta_offset);
        json.at_segment = map_segment_meta_to_json(meta.at);
        json.previous_segment = map_segment_meta_to_json(*meta.previous);

        break;
    }
    case cloud_storage::anomaly_type::end_delta_smaller: {
        json.type = "end_delta_smaller";
        json.explanation = ssx::sformat(
          "Segment has end delta offset lower than start delta offset: {} < {}",
          meta.at.delta_offset_end,
          meta.at.delta_offset);
        json.at_segment = map_segment_meta_to_json(meta.at);

        break;
    }
    case cloud_storage::anomaly_type::committed_smaller: {
        json.type = "committed_smaller";
        json.explanation = ssx::sformat(
          "Segment has committed offset lower start offset: {} < {}",
          meta.at.committed_offset,
          meta.at.base_offset);
        json.at_segment = map_segment_meta_to_json(meta.at);

        break;
    }
    case cloud_storage::anomaly_type::offset_gap: {
        if (!meta.previous) {
            vlog(
              adminlog.error,
              "Invalid anomaly metadata of type {} at {}",
              meta.type,
              meta.at);
            return json;
        }

        json.type = "offset_gap";
        json.explanation = ssx::sformat(
          "Gap between offsets in interval ({}, {})",
          meta.previous->committed_offset(),
          meta.at.base_offset());
        json.at_segment = map_segment_meta_to_json(meta.at);
        json.previous_segment = map_segment_meta_to_json(*meta.previous);

        break;
    }
    case cloud_storage::anomaly_type::offset_overlap: {
        if (!meta.previous) {
            vlog(
              adminlog.error,
              "Invalid anomaly metadata of type {} at {}",
              meta.type,
              meta.at);
            return json;
        }

        json.type = "offest_overlap";
        json.explanation = ssx::sformat(
          "Overlapping offset in interval [{}, {}]",
          meta.at.base_offset(),
          meta.previous->committed_offset());
        json.at_segment = map_segment_meta_to_json(meta.at);
        json.previous_segment = map_segment_meta_to_json(*meta.previous);

        break;
    }
    }

    return json;
}

ss::httpd::shadow_indexing_json::cloud_storage_partition_anomalies
map_anomalies_to_json(
  const model::ntp& ntp,
  const model::initial_revision_id& initial_rev,
  const cloud_storage::anomalies& detected) {
    ss::httpd::shadow_indexing_json::cloud_storage_partition_anomalies json;
    json.ns = ntp.ns();
    json.topic = ntp.tp.topic();
    json.partition = ntp.tp.partition();
    json.revision_id = initial_rev();

    if (detected.last_complete_scrub) {
        json.last_complete_scrub_at = detected.last_complete_scrub->value();
    }

    if (detected.missing_partition_manifest) {
        json.missing_partition_manifest = true;
    }

    if (detected.missing_spillover_manifests.size() > 0) {
        const auto& missing_spills = detected.missing_spillover_manifests;
        for (auto iter = missing_spills.begin(); iter != missing_spills.end();
             ++iter) {
            json.missing_spillover_manifests.push(
              cloud_storage::generate_spillover_manifest_path(
                ntp, initial_rev, *iter)()
                .string());
        }
    }

    if (detected.missing_segments.size() > 0) {
        cloud_storage::partition_manifest tmp{ntp, initial_rev};
        const auto& missing_segs = detected.missing_segments;
        for (auto iter = missing_segs.begin(); iter != missing_segs.end();
             ++iter) {
            json.missing_segments.push(
              tmp.generate_segment_path(*iter)().string());
        }
    }

    if (detected.segment_metadata_anomalies.size() > 0) {
        const auto& segment_meta_anomalies
          = detected.segment_metadata_anomalies;
        for (const auto& a : segment_meta_anomalies) {
            json.segment_metadata_anomalies.push(
              map_metadata_anomaly_to_json(a));
        }
    }

    return json;
}
} // namespace

ss::future<ss::json::json_return_type>
admin_server::get_partition_cloud_storage_status(
  std::unique_ptr<ss::http::request> req) {
    const model::ntp ntp = parse_ntp_from_request(
      req->param, model::kafka_namespace);

    if (need_redirect_to_leader(ntp, _metadata_cache)) {
        throw co_await redirect_to_leader(*req, ntp);
    }

    const auto shard = _shard_table.local().shard_for(ntp);
    if (!shard) {
        throw ss::httpd::not_found_exception(fmt::format(
          "{} could not be found on the node. Perhaps it has been moved "
          "during the redirect.",
          ntp));
    }

    auto status = co_await _partition_manager.invoke_on(
      *shard,
      [&ntp](const auto& pm)
        -> std::optional<cluster::partition_cloud_storage_status> {
          const auto& partitions = pm.partitions();
          auto partition_iter = partitions.find(ntp);

          if (partition_iter == partitions.end()) {
              return std::nullopt;
          }

          return partition_iter->second->get_cloud_storage_status();
      });

    if (!status) {
        throw ss::httpd::not_found_exception(
          fmt::format("{} could not be found on shard {}.", ntp, *shard));
    }

    co_return map_status_to_json(*status);
}

ss::future<ss::json::json_return_type>
admin_server::get_cloud_storage_lifecycle(
  std::unique_ptr<ss::http::request> req) {
    ss::httpd::shadow_indexing_json::get_lifecycle_response response;

    auto& topic_table = _controller->get_topics_state().local();

    cluster::topic_table::lifecycle_markers_t markers
      = topic_table.get_lifecycle_markers();

    // Hack: persuade json response to always include the field even if empty
    response.markers._set = true;

    for (auto [nt_revision, marker] : markers) {
        ss::httpd::shadow_indexing_json::lifecycle_marker item;
        item.ns = nt_revision.nt.ns;
        item.topic = nt_revision.nt.tp;
        item.revision_id = nt_revision.initial_revision_id;

        // At time of writing, a lifecycle marker's existence implicitly means
        // it is in a purging state.  In future this will change, e.g. when we
        // use lifecycle markers to track offloaded topics that were deleted
        // with remote.delete=false
        item.status = "purging";

        response.markers.push(item);
    }

    co_return response;
}

ss::future<ss::json::json_return_type>
admin_server::delete_cloud_storage_lifecycle(
  std::unique_ptr<ss::http::request> req) {
    auto topic = model::topic(req->param["topic"]);

    model::initial_revision_id revision;
    try {
        revision = model::initial_revision_id(
          std::stoi(req->param["revision"]));
    } catch (...) {
        throw ss::httpd::bad_param_exception(fmt::format(
          "Revision id must be an integer: {}", req->param["revision"]));
    }

    auto& tp_frontend = _controller->get_topics_frontend();
    cluster::nt_revision ntr{
      .nt = model::topic_namespace(model::kafka_namespace, model::topic{topic}),
      .initial_revision_id = revision};
    auto r = co_await tp_frontend.local().purged_topic(ntr, 5s);
    co_await throw_on_error(*req, r.ec, model::controller_ntp);

    co_return ss::json::json_return_type(ss::json::json_void());
}

ss::future<ss::json::json_return_type>
admin_server::post_cloud_storage_cache_trim(
  std::unique_ptr<ss::http::request> req) {
    auto size_limit = get_integer_query_param(*req, "objects");
    auto bytes_limit = static_cast<std::optional<size_t>>(
      get_integer_query_param(*req, "bytes"));

    co_await _cloud_storage_cache.invoke_on(
      ss::shard_id{0}, [size_limit, bytes_limit](auto& c) {
          return c.trim_manually(size_limit, bytes_limit);
      });

    co_return ss::json::json_return_type(ss::json::json_void());
}

ss::future<std::unique_ptr<ss::http::reply>> admin_server::get_manifest(
  std::unique_ptr<ss::http::request> req,
  std::unique_ptr<ss::http::reply> rep) {
    model::ntp ntp = parse_ntp_from_request(req->param, model::kafka_namespace);

    if (!_metadata_cache.local().contains(ntp)) {
        throw ss::httpd::not_found_exception(
          fmt::format("Could not find {} on the cluster", ntp));
    }

    if (need_redirect_to_leader(ntp, _metadata_cache)) {
        throw co_await redirect_to_leader(*req, ntp);
    }

    const auto shard = _shard_table.local().shard_for(ntp);
    if (!shard) {
        throw ss::httpd::not_found_exception(fmt::format(
          "Could not find {} on node {}", ntp, config::node().node_id.value()));
    }

    co_return co_await _partition_manager.invoke_on(
      *shard,
      [rep = std::move(rep), ntp = std::move(ntp), shard](auto& pm) mutable {
          auto partition = pm.get(ntp);
          if (!partition) {
              throw ss::httpd::not_found_exception(
                fmt::format("Could not find {} on shard {}", ntp, *shard));
          }

          if (!partition->remote_partition()) {
              throw ss::httpd::bad_request_exception(
                fmt::format("Cluster is not configured for cloud storage"));
          }

          // The 'remote_partition' 'ss::shared_ptr' belongs to the
          // shard with sid '*shard'. Hence, we need to ensure that
          // when Seastar calls into the labmda provided by read body,
          // all access to the pointer happens on its home shard.
          rep->write_body(
            "json",
            [part = std::move(partition),
             sid = *shard](ss::output_stream<char>&& output_stream) mutable {
                return ss::smp::submit_to(
                  sid,
                  [os = std::move(output_stream),
                   part = std::move(part)]() mutable {
                      return ss::do_with(
                        std::move(os),
                        std::move(part),
                        [](auto& os, auto& part) mutable {
                            return part
                              ->serialize_json_manifest_to_output_stream(os)
                              .finally([&os] { return os.close(); });
                        });
                  });
            });

          return ss::make_ready_future<std::unique_ptr<ss::http::reply>>(
            std::move(rep));
      });
}

ss::future<ss::json::json_return_type>
admin_server::get_cloud_storage_anomalies(
  std::unique_ptr<ss::http::request> req) {
    const model::ntp ntp = parse_ntp_from_request(req->param);

    if (need_redirect_to_leader(ntp, _metadata_cache)) {
        throw co_await redirect_to_leader(*req, ntp);
    }

    const auto& topic_table = _controller->get_topics_state().local();
    const auto initial_rev = topic_table.get_initial_revision(ntp);
    if (!initial_rev) {
        throw ss::httpd::not_found_exception(
          fmt::format("topic {} not found", ntp.tp));
    }

    const auto shard = _shard_table.local().shard_for(ntp);
    if (!shard) {
        throw ss::httpd::not_found_exception(fmt::format(
          "{} could not be found on the node. Perhaps it has been moved "
          "during the redirect.",
          ntp));
    }

    auto status = co_await _partition_manager.invoke_on(
      *shard,
      [&ntp](const auto& pm) -> std::optional<cloud_storage::anomalies> {
          const auto& partitions = pm.partitions();
          auto partition_iter = partitions.find(ntp);

          if (partition_iter == partitions.end()) {
              return std::nullopt;
          }

          return partition_iter->second->get_cloud_storage_anomalies();
      });

    if (!status) {
        throw ss::httpd::not_found_exception(fmt::format(
          "Cloud partition {} could not be found on shard {}.", ntp, *shard));
    }

    co_return map_anomalies_to_json(ntp, *initial_rev, *status);
}

ss::future<std::unique_ptr<ss::http::reply>>
admin_server::unsafe_reset_metadata_from_cloud(
  std::unique_ptr<ss::http::request> request,
  std::unique_ptr<ss::http::reply> reply) {
    reply->set_content_type("json");

    auto ntp = parse_ntp_from_request(request->param);
    if (need_redirect_to_leader(ntp, _metadata_cache)) {
        vlog(
          adminlog.info,
          "Need to redirect unsafe reset metadata from cloud request");
        throw co_await redirect_to_leader(*request, ntp);
    }

    const auto shard = _shard_table.local().shard_for(ntp);
    if (!shard) {
        throw ss::httpd::not_found_exception(fmt::format(
          "{} could not be found on the node. Perhaps it has been moved "
          "during the redirect.",
          ntp));
    }

    bool force = get_boolean_query_param(*request, "force");

    try {
        co_await _partition_manager.invoke_on(
          *shard, [ntp = std::move(ntp), shard, force](auto& pm) {
              auto partition = pm.get(ntp);
              if (!partition) {
                  throw ss::httpd::not_found_exception(
                    fmt::format("Could not find {} on shard {}", ntp, *shard));
              }

              return partition
                ->unsafe_reset_remote_partition_manifest_from_cloud(force);
          });
    } catch (const std::runtime_error& err) {
        throw ss::httpd::server_error_exception(err.what());
    }

    reply->set_status(ss::http::reply::status_type::ok);
    co_return reply;
}

ss::future<ss::json::json_return_type>
admin_server::reset_scrubbing_metadata(std::unique_ptr<ss::http::request> req) {
    const model::ntp ntp = parse_ntp_from_request(
      req->param, model::kafka_namespace);

    if (need_redirect_to_leader(ntp, _metadata_cache)) {
        throw co_await redirect_to_leader(*req, ntp);
    }

    const auto shard = _shard_table.local().shard_for(ntp);
    if (!shard) {
        throw ss::httpd::not_found_exception(fmt::format(
          "{} could not be found on the node. Perhaps it has been moved "
          "during the redirect.",
          ntp));
    }

    auto status = co_await _partition_manager.invoke_on(
      *shard, [&ntp, shard](const auto& pm) {
          const auto& partitions = pm.partitions();
          auto partition_iter = partitions.find(ntp);

          if (partition_iter == partitions.end()) {
              throw ss::httpd::not_found_exception(
                fmt::format("{} could not be found on shard {}.", ntp, *shard));
          }

          auto archiver = partition_iter->second->archiver();
          if (!archiver) {
              throw ss::httpd::not_found_exception(
                fmt::format("{} has no archiver on shard {}.", ntp, *shard));
          }

          return archiver.value().get().reset_scrubbing_metadata();
      });

    if (status != cluster::errc::success) {
        throw ss::httpd::server_error_exception{
          "Failed to replicate or apply scrubber metadata reset command"};
    }

    co_return ss::json::json_return_type(ss::json::json_void());
}

void admin_server::register_shadow_indexing_routes() {
    register_route<superuser>(
      ss::httpd::shadow_indexing_json::sync_local_state,
      [this](std::unique_ptr<ss::http::request> req) {
          return sync_local_state_handler(std::move(req));
      });

    request_handler_fn recovery_handler = [this](auto req, auto reply) {
        return initiate_topic_scan_and_recovery(
          std::move(req), std::move(reply));
    };

    register_route<superuser>(
      ss::httpd::shadow_indexing_json::initiate_topic_scan_and_recovery,
      std::move(recovery_handler));

    register_route<superuser>(
      ss::httpd::shadow_indexing_json::query_automated_recovery,
      [this](auto req) { return query_automated_recovery(std::move(req)); });

    register_route<superuser>(
      ss::httpd::shadow_indexing_json::initialize_cluster_recovery,
      request_handler_fn{[this](auto req, auto reply) {
          return initialize_cluster_recovery(std::move(req), std::move(reply));
      }});
    register_route<superuser>(
      ss::httpd::shadow_indexing_json::get_cluster_recovery,
      [this](auto req) { return get_cluster_recovery(std::move(req)); });

    register_route<user>(
      ss::httpd::shadow_indexing_json::get_partition_cloud_storage_status,
      [this](auto req) {
          return get_partition_cloud_storage_status(std::move(req));
      });

    register_route<user>(
      ss::httpd::shadow_indexing_json::get_cloud_storage_lifecycle,
      [this](auto req) { return get_cloud_storage_lifecycle(std::move(req)); });

    register_route<user>(
      ss::httpd::shadow_indexing_json::delete_cloud_storage_lifecycle,
      [this](auto req) {
          return delete_cloud_storage_lifecycle(std::move(req));
      });

    register_route<user>(
      ss::httpd::shadow_indexing_json::post_cloud_storage_cache_trim,
      [this](auto req) {
          return post_cloud_storage_cache_trim(std::move(req));
      });

    register_route_raw_async<user>(
      ss::httpd::shadow_indexing_json::get_manifest,
      [this](
        std::unique_ptr<ss::http::request> req,
        std::unique_ptr<ss::http::reply> rep) {
          return get_manifest(std::move(req), std::move(rep));
      });

    register_route<user>(
      ss::httpd::shadow_indexing_json::get_cloud_storage_anomalies,
      [this](auto req) { return get_cloud_storage_anomalies(std::move(req)); });

    request_handler_fn unsafe_reset_metadata_from_cloud_handler =
      [this](auto req, auto reply) {
          return unsafe_reset_metadata_from_cloud(
            std::move(req), std::move(reply));
      };

    register_route<superuser>(
      ss::httpd::shadow_indexing_json::unsafe_reset_metadata_from_cloud,
      std::move(unsafe_reset_metadata_from_cloud_handler));

    register_route<user>(
      ss::httpd::shadow_indexing_json::reset_scrubbing_metadata,
      [this](auto req) { return reset_scrubbing_metadata(std::move(req)); });
}

constexpr std::string_view to_string_view(service_kind kind) {
    switch (kind) {
    case service_kind::schema_registry:
        return "schema-registry";
    case service_kind::http_proxy:
        return "http-proxy";
    }
    return "invalid";
}

template<typename E>
std::enable_if_t<std::is_enum_v<E>, std::optional<E>>
  from_string_view(std::string_view);

template<>
constexpr std::optional<service_kind>
from_string_view<service_kind>(std::string_view sv) {
    return string_switch<std::optional<service_kind>>(sv)
      .match(
        to_string_view(service_kind::schema_registry),
        service_kind::schema_registry)
      .match(to_string_view(service_kind::http_proxy), service_kind::http_proxy)
      .default_match(std::nullopt);
}

namespace {
template<typename service_t>
ss::future<>
try_service_restart(service_t* svc, std::string_view service_str_view) {
    if (svc == nullptr) {
        throw ss::httpd::server_error_exception(fmt::format(
          "{} is undefined. Is it set in the .yaml config file?",
          service_str_view));
    }

    try {
        co_await svc->restart();
    } catch (const std::exception& ex) {
        vlog(
          adminlog.error,
          "Unknown issue restarting {}: {}",
          service_str_view,
          ex.what());
        throw ss::httpd::server_error_exception(
          fmt::format("Unknown issue restarting {}", service_str_view));
    }
}
} // namespace

ss::future<> admin_server::restart_redpanda_service(service_kind service) {
    switch (service) {
    case service_kind::schema_registry:
        co_await try_service_restart(_schema_registry, to_string_view(service));
        break;
    case service_kind::http_proxy:
        co_await try_service_restart(_http_proxy, to_string_view(service));
        break;
    }
}

ss::future<ss::json::json_return_type>
admin_server::restart_service_handler(std::unique_ptr<ss::http::request> req) {
    auto service_param = req->get_query_param("service");
    std::optional<service_kind> service = from_string_view<service_kind>(
      service_param);
    if (!service.has_value()) {
        throw ss::httpd::not_found_exception(
          fmt::format("Invalid service: {}", service_param));
    }

    vlog(
      adminlog.info, "Restart redpanda service: {}", to_string_view(*service));
    co_await restart_redpanda_service(*service);
    co_return ss::json::json_return_type(ss::json::json_void());
}