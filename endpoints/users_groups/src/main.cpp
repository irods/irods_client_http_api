#include "irods/private/http_api/handlers.hpp"

#include "irods/private/http_api/common.hpp"
#include "irods/private/http_api/compatibility.hpp"
#include "irods/private/http_api/globals.hpp"
#include "irods/private/http_api/log.hpp"
#include "irods/private/http_api/session.hpp"
#include "irods/private/http_api/shared_api_operations.hpp"
#include "irods/private/http_api/version.hpp"

#include <irods/irods_exception.hpp>
#include <irods/irods_query.hpp>
#include <irods/rodsErrorTable.h>
#include <irods/user_administration.hpp>
#include <irods/version.hpp>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// clang-format off
namespace beast = boost::beast;     // from <boost/beast.hpp>
namespace http  = beast::http;      // from <boost/beast/http.hpp>
namespace net   = boost::asio;      // from <boost/asio.hpp>

namespace adm     = irods::experimental::administration;
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

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_create_user);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_remove_user);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_set_password);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_set_user_type);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_add_user_auth);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_remove_user_auth);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_modify_metadata);

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_create_group);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_remove_group);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_add_to_group);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_remove_from_group);

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_users);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_groups);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_members);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_is_member_of_group);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_stat);

	//
	// Operation to Handler mappings
	//

	// clang-format off
	const std::unordered_map<std::string, irods::http::handler_type> handlers_for_get{
		{"users", op_users},
		{"groups", op_groups},
		{"members", op_members},
		{"is_member_of_group", op_is_member_of_group},
		{"stat", op_stat}
	};

	const std::unordered_map<std::string, irods::http::handler_type> handlers_for_post{
		{"create_user", op_create_user},
		{"remove_user", op_remove_user},
		{"set_password", op_set_password},
		{"set_user_type", op_set_user_type},
		{"add_user_auth", op_add_user_auth},
		{"remove_user_auth", op_remove_user_auth},
		{"create_group", op_create_group},
		{"remove_group", op_remove_group},
		{"add_to_group", op_add_to_group},
		{"remove_from_group", op_remove_from_group},
		{"modify_metadata", op_modify_metadata}
	};
	// clang-format on
} // anonymous namespace

namespace irods::http::handler
{
	// NOLINTNEXTLINE(performance-unnecessary-value-param)
	IRODS_HTTP_API_ENDPOINT_ENTRY_FUNCTION_SIGNATURE(users_groups)
	{
		execute_operation(_sess_ptr, _req, handlers_for_get, handlers_for_post);
	} // users_groups
} // namespace irods::http::handler

namespace
{
	//
	// Operation handler implementations
	//

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_create_user)
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
					const auto name_iter = _args.find("name");
					if (name_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [name] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					const auto zone_iter = _args.find("zone");
					if (zone_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [zone] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					auto user_type = adm::user_type::rodsuser;
					const auto user_type_iter = _args.find("user-type");
					if (user_type_iter != std::end(_args) && user_type_iter->second != "rodsuser") {
						if (user_type_iter->second == "rodsadmin") {
							user_type = adm::user_type::rodsadmin;
						}
						else if (user_type_iter->second == "groupadmin") {
							user_type = adm::user_type::groupadmin;
						}
						else {
							logging::error(*_sess_ptr, "{}: Invalid value for [user-type] parameter.", fn);
							return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
						}
					}

					auto zone_type = adm::zone_type::local;
					const auto& client = irods::http::globals::configuration().at("irods_client");
					if (zone_iter->second != client.at("zone").get_ref<const std::string&>()) {
						zone_type = adm::zone_type::remote;
					}

					auto conn = irods::get_connection(client_info.username);
					adm::client::add_user(conn, adm::user{name_iter->second, zone_iter->second}, user_type, zone_type);

					// clang-format off
					res.body() = json{
						{"irods_response", {
							 {"status_code", 0}
						}}
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
	} // op_create_user

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_remove_user)
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
					const auto name_iter = _args.find("name");
					if (name_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [name] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					const auto zone_iter = _args.find("zone");
					if (zone_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [zone] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					auto conn = irods::get_connection(client_info.username);
					adm::client::remove_user(conn, adm::user{name_iter->second, zone_iter->second});

					// clang-format off
					res.body() = json{
						{"irods_response", {
							{"status_code", 0}
						}}
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
	} // op_remove_user

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_set_password)
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
					const auto name_iter = _args.find("name");
					if (name_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [name] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					const auto zone_iter = _args.find("zone");
					if (zone_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [zone] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					const auto new_password_iter = _args.find("new-password");
					if (new_password_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [new-password] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					static const auto& proxy_user_password =
						irods::http::globals::configuration()
							.at(json::json_pointer{"/irods_client/proxy_admin_account/password"})
							.get_ref<const std::string&>();
					const adm::user_password_property prop{new_password_iter->second, proxy_user_password};

					auto conn = irods::get_connection(client_info.username);
					adm::client::modify_user(conn, adm::user{name_iter->second, zone_iter->second}, prop);

					res.body() = json{{"irods_response", {{"status_code", 0}}}}.dump();
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
	} // op_set_password

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_set_user_type)
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
					const auto name_iter = _args.find("name");
					if (name_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [name] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					const auto zone_iter = _args.find("zone");
					if (zone_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [zone] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					const auto new_user_type_iter = _args.find("new-user-type");
					if (new_user_type_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [new-user-type] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					const adm::user_type_property prop{adm::to_user_type(new_user_type_iter->second)};

					auto conn = irods::get_connection(client_info.username);
					adm::client::modify_user(conn, adm::user{name_iter->second, zone_iter->second}, prop);

					// clang-format off
					res.body() = json{
						{"irods_response", {
							{"status_code", 0}
						}}
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
	} // op_set_user_type

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_add_user_auth)
	{
#if 0
        auto result = irods::http::resolve_client_identity(_req);
        if (result.response) {
            return _sess_ptr->send(std::move(*result.response));
        }

        const auto client_info = result.client_info;

        irods::http::globals::background_task([fn = __func__, client_info, _sess_ptr, _req = std::move(_req), _args = std::move(_args)] {
            logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

            http::response<http::string_body> res{http::status::ok, _req.version()};
            res.set(http::field::server, irods::http::version::server_name);
            res.set(http::field::content_type, "application/json");
            res.keep_alive(_req.keep_alive());

            try {
            }
            catch (const irods::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
                res.result(http::status::bad_request);
                res.body() = json{
                    {"irods_response", {
                        {"status_code", e.code()},
                        {"status_message", e.client_display_what()}
                    }}
                }.dump();
            }
            catch (const std::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
                res.result(http::status::internal_server_error);
            }

            res.prepare_payload();

            return _sess_ptr->send(std::move(res));
        });
#else
		(void) _req;
		(void) _args;
		logging::error(*_sess_ptr, "{}: Operation not implemented.", __func__);
		return _sess_ptr->send(irods::http::fail(http::status::not_implemented));
#endif
	} // op_add_user_auth

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_remove_user_auth)
	{
#if 0
        auto result = irods::http::resolve_client_identity(_req);
        if (result.response) {
            return _sess_ptr->send(std::move(*result.response));
        }

        const auto client_info = result.client_info;

        irods::http::globals::background_task([fn = __func__, client_info, _sess_ptr, _req = std::move(_req), _args = std::move(_args)] {
            logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

            http::response<http::string_body> res{http::status::ok, _req.version()};
            res.set(http::field::server, irods::http::version::server_name);
            res.set(http::field::content_type, "application/json");
            res.keep_alive(_req.keep_alive());

            try {
            }
            catch (const irods::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
                res.result(http::status::bad_request);
                res.body() = json{
                    {"irods_response", {
                        {"status_code", e.code()},
                        {"status_message", e.client_display_what()}
                    }}
                }.dump();
            }
            catch (const std::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
                res.result(http::status::internal_server_error);
            }

            res.prepare_payload();

            return _sess_ptr->send(std::move(res));
        });
#else
		(void) _req;
		(void) _args;
		logging::error(*_sess_ptr, "{}: Operation not implemented.", __func__);
		return _sess_ptr->send(irods::http::fail(http::status::not_implemented));
#endif
	} // op_remove_user_auth

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_create_group)
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
					const auto name_iter = _args.find("name");
					if (name_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [name] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					auto conn = irods::get_connection(client_info.username);

					const auto version = irods::to_version(static_cast<RcComm*>(conn)->svrVersion->relVersion);
					const auto irods_server_supports_group_keyword = version && (*version > irods::version{4, 3, 3});

					// TODO(#434): Use adm::client::add_user() when 4.3 is EOL or the HTTP API is iRODS 4.3.4+ only.
					irods::http::compatibility::add_group(
						irods_server_supports_group_keyword, conn, adm::group{name_iter->second});

					res.body() = json{{"irods_response", {{"status_code", 0}}}}.dump();
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
	} // op_create_group

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_remove_group)
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
					const auto name_iter = _args.find("name");
					if (name_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [name] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					auto conn = irods::get_connection(client_info.username);
					adm::client::remove_group(conn, adm::group{name_iter->second});

					// clang-format off
					res.body() = json{
						{"irods_response", {
							{"status_code", 0}
						}}
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
	} // op_remove_group

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_add_to_group)
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
					const auto user_iter = _args.find("user");
					if (user_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [user] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					const auto zone_iter = _args.find("zone");
					if (zone_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [zone] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					const auto group_iter = _args.find("group");
					if (group_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [group] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					auto conn = irods::get_connection(client_info.username);
					adm::client::add_user_to_group(
						conn, adm::group{group_iter->second}, adm::user{user_iter->second, zone_iter->second});

					res.body() = json{
						{"irods_response",
				         {
							 {"status_code", 0},
						 }}}.dump();
				}
				catch (const irods::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
					res.body() = json{{"irods_response",
				                       {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
				                     .dump();
				}
				catch (const std::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.what());
					res.result(http::status::internal_server_error);
				}

				res.prepare_payload();

				return _sess_ptr->send(std::move(res));
			});
	} // op_add_to_group

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_remove_from_group)
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
					const auto user_iter = _args.find("user");
					if (user_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [user] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					const auto zone_iter = _args.find("zone");
					if (zone_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [zone] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					const auto group_iter = _args.find("group");
					if (group_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [group] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					auto conn = irods::get_connection(client_info.username);
					adm::client::remove_user_from_group(
						conn, adm::group{group_iter->second}, adm::user{user_iter->second, zone_iter->second});

					res.body() = json{
						{"irods_response",
				         {
							 {"status_code", 0},
						 }}}.dump();
				}
				catch (const irods::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
					res.body() = json{{"irods_response",
				                       {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
				                     .dump();
				}
				catch (const std::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.what());
					res.result(http::status::internal_server_error);
				}

				res.prepare_payload();

				return _sess_ptr->send(std::move(res));
			});
	} // op_remove_from_group

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_users)
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
					auto conn = irods::get_connection(client_info.username);
					const auto users = adm::client::users(conn);

					std::vector<json> v;
					v.reserve(users.size());

					for (auto&& u : users) {
						v.push_back({{"name", u.name}, {"zone", u.zone}});
					}

					res.body() = json{{"irods_response", {{"status_code", 0}}}, {"users", v}}.dump();
				}
				catch (const irods::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
					res.body() = json{{"irods_response",
				                       {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
				                     .dump();
				}
				catch (const std::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.what());
					res.result(http::status::internal_server_error);
				}

				res.prepare_payload();

				return _sess_ptr->send(std::move(res));
			});
	} // op_users

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_groups)
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
					auto conn = irods::get_connection(client_info.username);
					auto groups = adm::client::groups(conn);

					std::vector<std::string> v;
					v.reserve(groups.size());

					for (auto&& g : groups) {
						v.push_back(std::move(g.name));
					}

					res.body() = json{{"irods_response", {{"status_code", 0}}}, {"groups", v}}.dump();
				}
				catch (const irods::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
					res.body() = json{{"irods_response",
				                       {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
				                     .dump();
				}
				catch (const std::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.what());
					res.result(http::status::internal_server_error);
				}

				res.prepare_payload();

				return _sess_ptr->send(std::move(res));
			});
	} // op_groups

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_members)
	{
#if 0
        auto result = irods::http::resolve_client_identity(_req);
        if (result.response) {
            return _sess_ptr->send(std::move(*result.response));
        }

        const auto client_info = result.client_info;

        irods::http::globals::background_task([fn = __func__, client_info, _sess_ptr, _req = std::move(_req), _args = std::move(_args)] {
            logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

            http::response<http::string_body> res{http::status::ok, _req.version()};
            res.set(http::field::server, irods::http::version::server_name);
            res.set(http::field::content_type, "application/json");
            res.keep_alive(_req.keep_alive());

            try {
            }
            catch (const irods::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
                res.result(http::status::bad_request);
                res.body() = json{
                    {"irods_response", {
                        {"status_code", e.code()},
                        {"status_message", e.client_display_what()}
                    }}
                }.dump();
            }
            catch (const std::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
                res.result(http::status::internal_server_error);
            }

            res.prepare_payload();

            return _sess_ptr->send(std::move(res));
        });
#else
		(void) _req;
		(void) _args;
		logging::error(*_sess_ptr, "{}: Operation not implemented.", __func__);
		return _sess_ptr->send(irods::http::fail(http::status::not_implemented));
#endif
	} // op_members

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_is_member_of_group)
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

					const auto user_iter = _args.find("user");
					if (user_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [user] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					const auto zone_iter = _args.find("zone");
					if (zone_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [zone] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					auto conn = irods::get_connection(client_info.username);

					const adm::group group{adm::group{group_iter->second}};
					const adm::user user{adm::user{user_iter->second, zone_iter->second}};

					res.body() =
						json{
							{"irods_response", {{"status_code", 0}}},
							{"is_member", adm::client::user_is_member_of_group(conn, group, user)}}
							.dump();
				}
				catch (const irods::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
					res.body() = json{{"irods_response",
				                       {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
				                     .dump();
				}
				catch (const std::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.what());
					res.result(http::status::internal_server_error);
				}

				res.prepare_payload();

				return _sess_ptr->send(std::move(res));
			});
	} // op_is_member_of_group

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
					const auto name_iter = _args.find("name");
					if (name_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [name] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					auto conn = irods::get_connection(client_info.username);

					json info{
						{"irods_response",
				         {
							 {"status_code", 0},
						 }},
						{"exists", false}};

					// If the zone parameter is provided, we're likely dealing with a user. Otherwise,
				    // we don't know what we're identifying.
					const auto zone_iter = _args.find("zone");
					if (zone_iter != std::end(_args)) {
						const adm::user user{name_iter->second, zone_iter->second};
						if (const auto id = adm::client::id(conn, user); id) {
							info.update(
								{{"exists", true},
						         {"id", *id},
						         {"local_unique_name", fmt::format("{}#{}", name_iter->second, zone_iter->second)},
						         {"type", adm::to_c_str(*adm::client::type(conn, user))}});
						}

						res.body() = info.dump();
						res.prepare_payload();
						return _sess_ptr->send(std::move(res));
					}

					// The client did not include a zone so we are required to test if the name
				    // identifies a user or group.

					const adm::user user{name_iter->second};
					if (const auto id = adm::client::id(conn, user); id) {
						info.update(
							{{"exists", true},
					         {"id", *id},
					         {"local_unique_name", adm::client::local_unique_name(conn, user)},
					         {"type", adm::to_c_str(*adm::client::type(conn, user))}});
					}
					else {
						const adm::group group{name_iter->second};
						if (const auto id = adm::client::id(conn, group); id) {
							info.update({{"exists", true}, {"id", *id}, {"type", "rodsgroup"}});
						}
					}

					res.body() = info.dump();
				}
				catch (const irods::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
					res.body() = json{{"irods_response",
				                       {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
				                     .dump();
				}
				catch (const std::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.what());
					res.result(http::status::internal_server_error);
				}

				res.prepare_payload();

				return _sess_ptr->send(std::move(res));
			});
	} // op_stat

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_modify_metadata)
	{
		using namespace irods::http::shared_api_operations;
		return op_atomic_apply_metadata_operations(_sess_ptr, _req, _args, entity_type::user);
	} // op_modify_metadata
} // anonymous namespace
