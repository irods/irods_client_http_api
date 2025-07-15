#include "irods/private/http_api/handlers.hpp"

#include "irods/private/http_api/common.hpp"
#include "irods/private/http_api/globals.hpp"
#include "irods/private/http_api/log.hpp"
#include "irods/private/http_api/session.hpp"
#include "irods/private/http_api/version.hpp"

#include <irods/generalAdmin.h>
#include <irods/genquery2.h>
#include <irods/irods_at_scope_exit.hpp>
#include <irods/irods_exception.hpp>
#include <irods/irods_query.hpp>
#include <irods/irods_version.h>
#include <irods/procApiRequest.h>
#include <irods/query_builder.hpp>
#include <irods/rodsErrorTable.h>
#include <irods/rodsGenQuery.h>

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// clang-format off
namespace beast = boost::beast;     // from <boost/beast.hpp>
namespace http  = beast::http;      // from <boost/beast/http.hpp>
namespace net   = boost::asio;      // from <boost/asio.hpp>

namespace logging = irods::http::log;

using json = nlohmann::json;
// clang-format on

#define IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(name) \
  auto name(                                              \
	  irods::http::session_pointer_type _sess_ptr,        \
	  irods::http::request_type& _req,                    \
	  irods::http::query_arguments_type& _args)           \
	  ->void

namespace
{
	//
	// Handler function prototypes
	//

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_execute_genquery);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_execute_specific_query);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_list_genquery_columns);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_list_specific_queries);

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_add_specific_query);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_remove_specific_query);

	//
	// Operation to Handler mappings
	//

	// clang-format off
	const std::unordered_map<std::string, irods::http::handler_type> handlers_for_get{
		{"execute_genquery", op_execute_genquery},
		{"execute_specific_query", op_execute_specific_query},
		{"list_genquery_columns", op_list_genquery_columns},
		{"list_specific_queries", op_list_specific_queries}
	};

	const std::unordered_map<std::string, irods::http::handler_type> handlers_for_post{
		{"add_specific_query", op_add_specific_query},
		{"remove_specific_query", op_remove_specific_query}
	};
	// clang-format on
} // anonymous namespace

namespace irods::http::handler
{
	// NOLINTNEXTLINE(performance-unnecessary-value-param)
	IRODS_HTTP_API_ENDPOINT_ENTRY_FUNCTION_SIGNATURE(query)
	{
		execute_operation(_sess_ptr, _req, handlers_for_get, handlers_for_post);
	} // query
} // namespace irods::http::handler

namespace
{
	//
	// Operation handler implementations
	//

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_execute_genquery)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;
		logging::info(*_sess_ptr, "{}: client_info.username = [{}]", __func__, client_info.username);

		irods::http::globals::background_task(
			[fn = __func__, _sess_ptr, req = std::move(_req), args = std::move(_args), client_info]() mutable {
				auto query_iter = args.find("query");
				if (query_iter == std::end(args)) {
					logging::error(*_sess_ptr, "{}: Missing [query] parameter.", fn);
					return _sess_ptr->send(irods::http::fail(http::status::bad_request));
				}

				std::string parser = "genquery1";
				const auto parser_iter = args.find("parser");
				if (parser_iter != std::end(args)) {
					if (parser_iter->second != "genquery1" && parser_iter->second != "genquery2") {
						logging::error(*_sess_ptr, "{}: Invalid argument for [parser] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(http::status::bad_request));
					}

					parser = parser_iter->second;
				}

				http::response<http::string_body> res{http::status::ok, req.version()};
				res.set(http::field::server, irods::http::version::server_name);
				res.set(http::field::content_type, "application/json");
				res.keep_alive(req.keep_alive());

				try {
					json::array_t row;
					json::array_t rows;

					auto conn = irods::get_connection(client_info.username);

					if ("genquery2" == parser) {
						Genquery2Input input{};
						input.query_string = query_iter->second.data();

						auto sql_only_iter = args.find("sql-only");
						if (sql_only_iter != std::end(args) && "1" == sql_only_iter->second) {
							input.sql_only = 1;
						}

						auto zone_iter = args.find("zone");
						if (zone_iter != std::end(args)) {
							input.zone = zone_iter->second.data();
						}

						char* output{};
						irods::at_scope_exit free_output{[&output] { std::free(output); }};

						const auto ec = rc_genquery2(static_cast<RcComm*>(conn), &input, &output);

						if (ec < 0) {
							res.result(http::status::bad_request);
							res.body() = json{{"irods_response", {{"status_code", ec}}}}.dump();
						}

						if (0 == input.sql_only) {
							// This is a performance optimization.
							constexpr const auto* json_fmt_string =
								R"_({{"irods_response":{{"status_code":0}},"rows":{}}})_";
							res.body() = fmt::format(json_fmt_string, output);
						}
						else {
							constexpr const auto* json_fmt_string =
								R"_({{"irods_response":{{"status_code":0}},"sql":"{}"}})_";
							res.body() = fmt::format(json_fmt_string, output);
						}
					}
					else {
						irods::experimental::query_builder qb;

						int offset = 0;
						if (const auto iter = args.find("offset"); iter != std::end(args)) {
							try {
								offset = std::stoi(iter->second);
							}
							catch (const std::exception& e) {
								logging::error(
									*_sess_ptr, "{}: Could not convert [offset] parameter value into an integer. ", fn);
								return _sess_ptr->send(irods::http::fail(http::status::bad_request));
							}
						}
						offset = std::max(0, offset);
						qb.row_offset(offset);

						static const auto max_row_count =
							irods::http::globals::configuration()
								.at(json::json_pointer{"/irods_client/max_number_of_rows_per_catalog_query"})
								.get<int>();
						int count = max_row_count;
						if (const auto iter = args.find("count"); iter != std::end(args)) {
							try {
								count = std::stoi(iter->second);
							}
							catch (const std::exception& e) {
								logging::error(
									*_sess_ptr, "{}: Could not convert [count] parameter value into an integer.", fn);
								return _sess_ptr->send(irods::http::fail(http::status::bad_request));
							}
						}
						count = std::clamp(count, 1, max_row_count);
						qb.row_limit(count);

						int options = 0;

						if (const auto iter = args.find("case-sensitive"); iter != std::end(args)) {
							if (iter->second == "0") {
								options |= UPPER_CASE_WHERE;
								boost::algorithm::to_upper(query_iter->second);
							}
							else if (iter->second != "1") {
								logging::error(
									*_sess_ptr,
									"{}: Invalid value for [case-sensitive] parameter. Expected a 1 or 0.",
									fn);
								return _sess_ptr->send(irods::http::fail(http::status::bad_request));
							}
						}

						if (const auto iter = args.find("distinct"); iter != std::end(args)) {
							if (iter->second == "0") {
								options |= NO_DISTINCT;
							}
							else if (iter->second != "1") {
								logging::error(
									*_sess_ptr, "{}: Invalid value for [distinct] parameter. Expected a 1 or 0.", fn);
								return _sess_ptr->send(irods::http::fail(http::status::bad_request));
							}
						}

						qb.options(options);

						if (const auto iter = args.find("zone"); iter != std::end(args)) {
							qb.zone_hint(iter->second);
						}

						for (auto&& r : qb.build<RcComm>(conn, query_iter->second)) {
							for (auto&& c : r) {
								row.push_back(c);
							}

							rows.push_back(row);
							row.clear();
						}

						res.body() = json{{"irods_response", {{"status_code", 0}}}, {"rows", rows}}.dump();
					}
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
	} // op_execute_genquery

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_execute_specific_query)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;
		logging::info(*_sess_ptr, "{}: client_info.username = [{}]", __func__, client_info.username);

		const auto name_iter = _args.find("name");
		if (name_iter == std::end(_args)) {
			logging::error(*_sess_ptr, "{}: Missing [name] parameter.", __func__);
			return _sess_ptr->send(irods::http::fail(http::status::bad_request));
		}

		int offset = 0;
		if (const auto iter = _args.find("offset"); iter != std::end(_args)) {
			try {
				offset = std::stoi(iter->second);
			}
			catch (const std::exception& e) {
				logging::error(
					*_sess_ptr, "{}: Could not convert [offset] parameter value into an integer. ", __func__);
				return _sess_ptr->send(irods::http::fail(http::status::bad_request));
			}
		}
		offset = std::max(0, offset);

		static const auto max_row_count =
			irods::http::globals::configuration()
				.at(json::json_pointer{"/irods_client/max_number_of_rows_per_catalog_query"})
				.get<int>();
		int count = max_row_count;
		if (const auto iter = _args.find("count"); iter != std::end(_args)) {
			try {
				count = std::stoi(iter->second);
			}
			catch (const std::exception& e) {
				logging::error(*_sess_ptr, "{}: Could not convert [count] parameter value into an integer. ", __func__);
				return _sess_ptr->send(irods::http::fail(http::status::bad_request));
			}
		}
		count = std::clamp(count, 1, max_row_count);

		std::vector<std::string> args;

		if (const auto iter = _args.find("args"); iter != std::end(_args) && !iter->second.empty()) {
			if (const auto delim_iter = _args.find("args-delimiter");
			    delim_iter != std::end(_args) && !delim_iter->second.empty()) {
				boost::split(args, iter->second, boost::is_any_of(delim_iter->second));
			}
			else {
				boost::split(args, iter->second, boost::is_any_of(","));
			}
		}

		http::response<http::string_body> res{http::status::ok, _req.version()};
		res.set(http::field::server, irods::http::version::server_name);
		res.set(http::field::content_type, "application/json");
		res.keep_alive(_req.keep_alive());

		irods::http::globals::background_task([fn = __func__,
		                                       _sess_ptr,
		                                       client_info,
		                                       name = name_iter->second,
		                                       res = std::move(res),
		                                       offset,
		                                       count,
		                                       args = std::move(args)]() mutable {
			static_cast<void>(fn);

			try {
				json::array_t row;
				json::array_t rows;

				{
					irods::experimental::query_builder qb;

#if IRODS_VERSION_INTEGER < 5000090
					qb.type(irods::experimental::query_type::specific);
#else
					qb.type(irods::query_type::specific);
#endif
					qb.bind_arguments(args);
					//qb.row_offset(offset);
					//qb.row_limit(count);
					//qb.zone_hint("");

					int offset_counter = 0;
					int count_counter = 0;

					auto conn = irods::get_connection(client_info.username);

					for (auto&& r : qb.build<RcComm>(conn, name)) {
						if (offset_counter < offset) {
							++offset_counter;
							continue;
						}

						for (auto&& c : r) {
							row.push_back(c);
						}

						rows.push_back(row);
						row.clear();

						if (++count_counter == count) {
							break;
						}
					}
				}

				res.body() = json{
					{"irods_response",
				     {
						 {"status_code", 0},
					 }},
					{"rows",
				     rows}}.dump();
			}
			catch (const irods::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
				res.body() =
					json{{"irods_response", {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
						.dump();
			}
			catch (const std::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
				res.result(http::status::internal_server_error);
			}

			res.prepare_payload();

			return _sess_ptr->send(std::move(res));
		});
	} // op_execute_genquery

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_list_genquery_columns)
	{
		(void) _req;
		(void) _args;
		logging::error(*_sess_ptr, "{}: Operation not implemented.", __func__);
		return _sess_ptr->send(irods::http::fail(http::status::not_implemented));
	} // op_list_genquery_columns

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_list_specific_queries)
	{
		(void) _req;
		(void) _args;
		logging::error(*_sess_ptr, "{}: Operation not implemented.", __func__);
		return _sess_ptr->send(irods::http::fail(http::status::not_implemented));
	} // op_list_specific_queries

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_add_specific_query)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task(
			[fn = __func__, _sess_ptr, client_info, _req = std::move(_req), _args = std::move(_args)] {
				logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

				http::response<http::string_body> res{http::status::ok, _req.version()};
				res.set(http::field::server, irods::http::version::server_name);
				res.set(http::field::content_type, "application/json");
				res.keep_alive(_req.keep_alive());

				try {
					const auto name_iter = _args.find("name");
					if (name_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [name] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(http::status::bad_request));
					}

					const auto sql_iter = _args.find("sql");
					if (name_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [sql] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(http::status::bad_request));
					}

					GeneralAdminInput input{};
					input.arg0 = "add";
					input.arg1 = "specificQuery";
					input.arg2 = sql_iter->second.c_str();
					input.arg3 = name_iter->second.c_str();

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
	} // op_add_specific_query

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_remove_specific_query)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task(
			[fn = __func__, _sess_ptr, client_info, _req = std::move(_req), _args = std::move(_args)] {
				logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

				http::response<http::string_body> res{http::status::ok, _req.version()};
				res.set(http::field::server, irods::http::version::server_name);
				res.set(http::field::content_type, "application/json");
				res.keep_alive(_req.keep_alive());

				try {
					const auto name_iter = _args.find("name");
					if (name_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [name] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(http::status::bad_request));
					}

					GeneralAdminInput input{};
					input.arg0 = "rm";
					input.arg1 = "specificQuery";
					input.arg2 = name_iter->second.c_str();

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
	} // op_remove_specific_query
} // anonymous namespace
