#include "irods/private/http_api/handlers.hpp"

#include "irods/private/http_api/common.hpp"
#include "irods/private/http_api/globals.hpp"
#include "irods/private/http_api/log.hpp"
#include "irods/private/http_api/session.hpp"
#include "irods/private/http_api/version.hpp"

#include <irods/irods_exception.hpp>

#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include <string>

namespace irods::http::handler
{
	// NOLINTNEXTLINE(performance-unnecessary-value-param)
	IRODS_HTTP_API_ENDPOINT_ENTRY_FUNCTION_SIGNATURE(information)
	{
		try {
			if (_req.method() != boost::beast::http::verb::get) {
				log::error("{}: HTTP method not supported.", __func__);
				return _sess_ptr->send(fail(status_type::method_not_allowed));
			}

			const auto& http_server_config = irods::http::globals::configuration().at("http_server");
			const auto& irods_client_config = irods::http::globals::configuration().at("irods_client");

			using json = nlohmann::json;

			response_type res{status_type::ok, _req.version()};
			res.set(field_type::server, irods::http::version::server_name);
			res.set(field_type::content_type, "application/json");
			res.keep_alive(_req.keep_alive());

			// clang-format off
			res.body() = json{
				{"api_version", irods::http::version::api_version},
				{"build", irods::http::version::sha},
				{"genquery2_enabled", true},
				{"irods_zone", irods_client_config.at("zone")},
				{"max_number_of_parallel_write_streams", irods_client_config.at("max_number_of_parallel_write_streams")},
				{"max_number_of_rows_per_catalog_query", irods_client_config.at("max_number_of_rows_per_catalog_query")},
				{"max_size_of_request_body_in_bytes", http_server_config.at(json::json_pointer{"/requests/max_size_of_request_body_in_bytes"})},
				{"openid_connect_enabled", http_server_config.contains(json::json_pointer{"/authentication/openid_connect"})}
			}.dump();
			// clang-format on

			res.prepare_payload();

			return _sess_ptr->send(std::move(res));
		}
		catch (const std::exception& e) {
			log::error("{}: {}", __func__, e.what());
			return _sess_ptr->send(irods::http::fail(boost::beast::http::status::internal_server_error));
		}
	} // information
} //namespace irods::http::handler
