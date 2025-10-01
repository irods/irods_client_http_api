#include "irods/private/http_api/handlers.hpp"

#include "irods/private/http_api/common.hpp"
#include "irods/private/http_api/globals.hpp"
#include "irods/private/http_api/log.hpp"
#include "irods/private/http_api/session.hpp"
#include "irods/private/http_api/version.hpp"

#include <irods/generalAdmin.h>
#include <irods/irods_exception.hpp>
#include <irods/irods_query.hpp>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <tuple>
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

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_set_group_quotas);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_recalculate);

	//
	// Operation to Handler mappings
	//

	// clang-format off
	const std::unordered_map<std::string, irods::http::handler_type> handlers_for_get{
		{"stat", op_stat}
	};

	const std::unordered_map<std::string, irods::http::handler_type> handlers_for_post{
		{"set_group_quota", op_set_group_quotas},
		{"recalculate", op_recalculate}
	};
	// clang-format on
} // anonymous namespace

namespace irods::http::handler
{
	// NOLINTNEXTLINE(performance-unnecessary-value-param)
	IRODS_HTTP_API_ENDPOINT_ENTRY_FUNCTION_SIGNATURE(quotas)
	{
		// NOLINTNEXTLINE(performance-unnecessary-value-param)
		execute_operation(_sess_ptr, _req, handlers_for_get, handlers_for_post);
	} // quotas
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

				try {
					std::vector<json> resource_quotas;
					std::vector<json> global_quotas;

					auto conn = irods::get_connection(client_info.username);

					if (const auto iter = _args.find("group"); iter != std::end(_args)) {
						static const auto& local_zone = irods::http::globals::configuration()
					                                        .at(json::json_pointer{"/irods_client/zone"})
					                                        .get_ref<const std::string&>();
						const auto resource_quota_query = fmt::format(
							"select QUOTA_USER_NAME, QUOTA_USER_ZONE, QUOTA_RESC_NAME, QUOTA_LIMIT, QUOTA_OVER, "
							"QUOTA_MODIFY_TIME where QUOTA_USER_NAME = '{}' and QUOTA_USER_ZONE = '{}' and "
							"QUOTA_RESC_ID != '0'",
							iter->second,
							local_zone);
						for (auto&& row : irods::query{static_cast<RcComm*>(conn), resource_quota_query}) {
							resource_quotas.emplace_back(json{
								{"group", std::move(row[0])},
								{"resource", std::move(row[2])},
								{"limit", std::stoll(row[3])},
								{"over", std::stoll(row[4])},
								{"modified_at", std::move(row[5])}});
						}

						const auto global_quotas_query = fmt::format(
							"select QUOTA_USER_NAME, QUOTA_USER_ZONE, QUOTA_LIMIT, QUOTA_OVER, QUOTA_MODIFY_TIME where "
							"QUOTA_USER_NAME = '{}' and QUOTA_USER_ZONE = '{}' and QUOTA_RESC_ID = '0'",
							iter->second,
							local_zone);
						for (auto&& row : irods::query{static_cast<RcComm*>(conn), global_quotas_query}) {
							global_quotas.emplace_back(json{
								{"group", std::move(row[0])},
								{"limit", std::stoll(row[2])},
								{"over", std::stoll(row[3])},
								{"modified_at", std::move(row[4])}});
						}
					}
					else {
						const auto resource_quota_query =
							fmt::format("select QUOTA_USER_NAME, QUOTA_USER_ZONE, QUOTA_RESC_NAME, QUOTA_LIMIT, "
					                    "QUOTA_OVER, QUOTA_MODIFY_TIME where QUOTA_RESC_ID != '0'");
						for (auto&& row : irods::query{static_cast<RcComm*>(conn), resource_quota_query}) {
							resource_quotas.emplace_back(json{
								{"group", std::move(row[0])},
								{"resource", std::move(row[2])},
								{"limit", std::stoll(row[3])},
								{"over", std::stoll(row[4])},
								{"modified_at", std::move(row[5])}});
						}

						const auto global_quotas_query =
							fmt::format("select QUOTA_USER_NAME, QUOTA_USER_ZONE, QUOTA_LIMIT, QUOTA_OVER, "
					                    "QUOTA_MODIFY_TIME where QUOTA_RESC_ID = '0'");
						for (auto&& row : irods::query{static_cast<RcComm*>(conn), global_quotas_query}) {
							global_quotas.emplace_back(json{
								{"group", std::move(row[0])},
								{"limit", std::stoll(row[2])},
								{"over", std::stoll(row[3])},
								{"modified_at", std::move(row[4])}});
						}
					}

					// clang-format off
					res.body() = json{
						{"irods_response", {{"status_code", 0}}},
						{"resource_quotas", resource_quotas},
						{"global_quotas", global_quotas}
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

				res.prepare_payload();

				return _sess_ptr->send(std::move(res));
			});
	} // op_stat

	// NOLINTNEXTLINE(performance-unnecessary-value-param)
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_set_group_quotas)
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

				try {
					const auto group_iter = _args.find("group");
					if (group_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [group] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					const auto quota_iter = _args.find("quota");
					if (group_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [quota] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					GeneralAdminInput input{};
					input.arg0 = "set-quota";
					input.arg1 = "group";
					input.arg2 = group_iter->second.c_str();
					input.arg4 = quota_iter->second.c_str();

					// Apply the quota as a resource quota if the user set the resource parameter.
					// Otherwise, apply it as a global quota across all resources.
					const auto resource_iter = _args.find("resource");
					if (resource_iter != std::end(_args)) {
						input.arg3 = resource_iter->second.c_str();
					}
					else {
						input.arg3 = "total";
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

				res.prepare_payload();

				return _sess_ptr->send(std::move(res));
			});
	} // op_set_group_quotas

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

				try {
					GeneralAdminInput input{};
					input.arg0 = "calculate-usage";

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

				res.prepare_payload();

				return _sess_ptr->send(std::move(res));
			});
	} // op_recalculate
} // anonymous namespace
