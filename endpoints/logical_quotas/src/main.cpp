#include "irods/private/http_api/handlers.hpp"

#include "irods/private/http_api/common.hpp"
#include "irods/private/http_api/globals.hpp"
#include "irods/private/http_api/log.hpp"
#include "irods/private/http_api/session.hpp"
#include "irods/private/http_api/version.hpp"

#include <irods/generalAdmin.h>
#include <irods/irods_at_scope_exit.hpp>
#include <irods/irods_exception.hpp>
#include <irods/irods_query.hpp>
#include <irods/library_features.h>

#ifdef IRODS_LIBRARY_FEATURE_LOGICAL_QUOTAS
#  include <irods/get_logical_quota.h>
#endif // IRODS_LIBRARY_FEATURE_LOGICAL_QUOTAS

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <iterator>
#include <span>
#include <string>
#include <unordered_map>

// clang-format off
namespace beast = boost::beast;     // from <boost/beast.hpp>
namespace http  = beast::http;      // from <boost/beast/http.hpp>

namespace logging = irods::http::log;

using json = nlohmann::json;
// clang-format on

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(name) \
	auto name(                                            \
		irods::http::session_pointer_type _sess_ptr,      \
		irods::http::request_type& _req,                  \
		irods::http::query_arguments_type& _args)         \
		->void

namespace
{
	//
	// Handler function prototypes
	//

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_stat);

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_set_quota);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_recalculate);

	//
	// Operation to Handler mappings
	//

	// clang-format off
	const std::unordered_map<std::string, irods::http::handler_type> handlers_for_get{
		{"stat", op_stat}
	};

	const std::unordered_map<std::string, irods::http::handler_type> handlers_for_post{
		{"set_quota", op_set_quota},
		{"recalculate", op_recalculate}
	};
	// clang-format on
} // anonymous namespace

namespace irods::http::handler
{
	// NOLINTNEXTLINE(performance-unnecessary-value-param)
	IRODS_HTTP_API_ENDPOINT_ENTRY_FUNCTION_SIGNATURE(logical_quotas)
	{
		// NOLINTNEXTLINE(performance-unnecessary-value-param)
		execute_operation(_sess_ptr, _req, handlers_for_get, handlers_for_post);
	} // logical_quotas
} // namespace irods::http::handler

namespace
{
	//
	// Operation handler implementations
	//

	// NOLINTNEXTLINE(performance-unnecessary-value-param)
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_stat)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task(
			[fn = __func__, client_info, _sess_ptr, _req = std::move(_req), _args = std::move(_args)] {
				logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

				http::response<http::string_body> res{http::status::ok, _req.version()};
				res.set(http::field::server, irods::http::version::server_name);
				res.set(http::field::content_type, "application/json");
				res.keep_alive(_req.keep_alive());

#ifdef IRODS_LIBRARY_FEATURE_LOGICAL_QUOTAS
				try {
					GetLogicalQuotaInput input{};
					LogicalQuotaList* output{};

					irods::at_scope_exit free_output{[&input, &output] {
						clear_get_logical_quota_input(&input);
						clear_logical_quota_list(output);
						std::free(output); // NOLINT(cppcoreguidelines-owning-memory, cppcoreguidelines-no-malloc)
					}};

					if (const auto iter = _args.find("lpath"); iter != std::end(_args)) {
						input.coll_name = strdup(iter->second.c_str());
					}

					auto conn = irods::get_connection(client_info.username);
					const auto ec = rc_get_logical_quota(static_cast<RcComm*>(conn), &input, &output);

					std::vector<json> quota_info;

					if (ec >= 0 && output->len > 0) {
						quota_info.reserve(output->len);

						std::span entries(output->list, output->len);

						std::transform(
							std::begin(entries),
							std::end(entries),
							std::back_inserter(quota_info),
							[](const LogicalQuota& _e) {
								// clang-format off
								return json{
									{"collection", _e.coll_name},
									{"maximum_bytes", _e.max_bytes},
									{"maximum_objects", _e.max_objects},
									{"over_bytes", _e.over_bytes},
									{"over_objects", _e.over_objects}
								};
								// clang-format on
							});
					}

					// clang-format off
					res.body() = json{
						{"irods_response", {{"status_code", ec}}},
						{"quotas", quota_info}
					}.dump();
					// clang-format on
				}
				catch (const irods::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
					// clang-format off
					res.body() = json{
						{"irods_response", {
							{"status_code", e.code()},
							{"status_message", e.client_display_what()}
						}}
					}.dump();
					// clang-format on
				}
				catch (const std::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.what());
					res.result(http::status::internal_server_error);
				}
#else
				res.result(http::status::not_implemented);
#endif // IRODS_LIBRARY_FEATURE_LOGICAL_QUOTAS

				res.prepare_payload();

				return _sess_ptr->send(std::move(res));
			});
	} // op_stat

	// NOLINTNEXTLINE(performance-unnecessary-value-param)
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_set_quota)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task([fn = __func__,
		                                       client_info,
		                                       _sess_ptr,
		                                       _req = std::move(_req),
		                                       _args = std::move(_args)] {
			logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

			http::response<http::string_body> res{http::status::ok, _req.version()};
			res.set(http::field::server, irods::http::version::server_name);
			res.set(http::field::content_type, "application/json");
			res.keep_alive(_req.keep_alive());

#ifdef IRODS_LIBRARY_FEATURE_LOGICAL_QUOTAS
			try {
				const auto lpath_iter = _args.find("lpath");
				if (lpath_iter == std::end(_args)) {
					logging::error(*_sess_ptr, "{}: Missing [lpath] parameter.", fn);
					return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
				}

				GeneralAdminInput input{};
				input.arg0 = "set_logical_quota";
				input.arg1 = lpath_iter->second.c_str();

				const auto max_bytes_iter = _args.find("maximum-bytes");
				if (max_bytes_iter != std::end(_args)) {
					input.arg2 = max_bytes_iter->second.c_str();
				}

				const auto max_objects_iter = _args.find("maximum-objects");
				if (max_objects_iter != std::end(_args)) {
					input.arg3 = max_objects_iter->second.c_str();
				}

				if (!input.arg2 && !input.arg3) {
					logging::error(
						*_sess_ptr,
						"{}: No quota parameter provided. Expected [maximum-bytes] and/or [maximum-objects] parameter.",
						fn);
					return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
				}

				auto conn = irods::get_connection(client_info.username);
				const auto ec = rcGeneralAdmin(static_cast<RcComm*>(conn), &input);

				res.body() = json{{"irods_response", {{"status_code", ec}}}}.dump();
			}
			catch (const irods::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
				// clang-format off
					res.body() = json{
						{"irods_response", {
							{"status_code", e.code()},
							{"status_message", e.client_display_what()}
						}}
					}.dump();
				// clang-format on
			}
			catch (const std::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
				res.result(http::status::internal_server_error);
			}
#else
			res.result(http::status::not_implemented);
#endif // IRODS_LIBRARY_FEATURE_LOGICAL_QUOTAS

			res.prepare_payload();

			return _sess_ptr->send(std::move(res));
		});
	} // op_set_quota

	// NOLINTNEXTLINE(performance-unnecessary-value-param)
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_recalculate)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task(
			[fn = __func__, client_info, _sess_ptr, _req = std::move(_req), _args = std::move(_args)] {
				logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

				http::response<http::string_body> res{http::status::ok, _req.version()};
				res.set(http::field::server, irods::http::version::server_name);
				res.set(http::field::content_type, "application/json");
				res.keep_alive(_req.keep_alive());

#ifdef IRODS_LIBRARY_FEATURE_LOGICAL_QUOTAS
				try {
					GeneralAdminInput input{};
					input.arg0 = "calculate_logical_usage";

					auto conn = irods::get_connection(client_info.username);
					const auto ec = rcGeneralAdmin(static_cast<RcComm*>(conn), &input);

					res.body() = json{{"irods_response", {{"status_code", ec}}}}.dump();
				}
				catch (const irods::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
					// clang-format off
					res.body() = json{
						{"irods_response", {
							{"status_code", e.code()},
							{"status_message", e.client_display_what()}
						}}
					}.dump();
					// clang-format on
				}
				catch (const std::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.what());
					res.result(http::status::internal_server_error);
				}
#else
				res.result(http::status::not_implemented);
#endif // IRODS_LIBRARY_FEATURE_LOGICAL_QUOTAS

				res.prepare_payload();

				return _sess_ptr->send(std::move(res));
			});
	} // op_recalculate
} // anonymous namespace
