#include "irods/private/http_api/handlers.hpp"

#include "irods/private/http_api/common.hpp"
#include "irods/private/http_api/globals.hpp"
#include "irods/private/http_api/log.hpp"
#include "irods/private/http_api/process_stash.hpp"
#include "irods/private/http_api/session.hpp"
#include "irods/private/http_api/transport.hpp"
#include "irods/private/http_api/version.hpp"

#include <irods/base64.hpp>
#include <irods/check_auth_credentials.h>
#include <irods/client_connection.hpp>
#include <irods/irods_at_scope_exit.hpp>
#include <irods/irods_exception.hpp>
#include <irods/rcConnect.h>
#include <irods/user_administration.hpp>

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/url/parse.hpp>

#include <iterator>
#include <nlohmann/json.hpp>

#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/nlohmann-json/traits.h>

#include <fmt/core.h>

#include <curl/curl.h>
#include <curl/urlapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// clang-format off
namespace net     = boost::asio;  // from <boost/asio.hpp>
namespace logging = irods::http::log;
// clang-format on

namespace irods::http::handler
{
	auto remove_client_from_body_if_confidential_client(body_arguments _body) -> body_arguments
	{
		const static bool has_client_secret{irods::http::globals::oidc_configuration().contains("client_secret")};

		if (has_client_secret) {
			_body.erase("client_id");
		}

		return _body;
	}

	auto hit_token_endpoint(std::string _encoded_body) -> nlohmann::json
	{
		const auto token_endpoint{
			irods::http::globals::oidc_endpoint_configuration().at("token_endpoint").get_ref<const std::string&>()};

		// Setup net
		net::io_context io_ctx;

		const auto parsed_uri{boost::urls::parse_uri(token_endpoint)};

		if (parsed_uri.has_error()) {
			logging::error(
				"{}: Error trying to parse token_endpoint [{}]. Please check configuration.", __func__, token_endpoint);
			return {{"error", "bad endpoint"}};
		}

		const auto url{*parsed_uri};
		const auto port{irods::http::get_port_from_url(url)};
		auto req{irods::http::create_oidc_request(url)};

		// Attach body to request
		req.body() = std::move(_encoded_body);
		req.prepare_payload();

		// Connect to remote
		auto tcp_stream{irods::http::transport_factory(url.scheme_id(), io_ctx)};
		tcp_stream->connect(url.host(), *port);

		// Send request and Read back response
		auto res{tcp_stream->communicate(req)};
		logging::debug("Got the following resp back: {}", res.body());

		// JSONize response
		return nlohmann::json::parse(res.body());
	}

	auto is_error_response(const nlohmann::json& _response_to_check) -> bool
	{
		if (const auto error{_response_to_check.find("error")}; error != std::cend(_response_to_check)) {
			std::string token_error_log;
			token_error_log.reserve(500);

			auto error_log_itter{fmt::format_to(
				std::back_inserter(token_error_log), "{}: Token request failed! Error: [{}]", __func__, *error)};

			// Optional OAuth 2.0 error parameters follow
			if (const auto error_description{_response_to_check.find("error_description")};
			    error_description != std::cend(_response_to_check))
			{
				error_log_itter = fmt::format_to(error_log_itter, ", Error Description [{}]", *error_description);
			}

			if (const auto error_uri{_response_to_check.find("error_uri")}; error_uri != std::cend(_response_to_check))
			{
				error_log_itter = fmt::format_to(error_log_itter, ", Error URI [{}]", *error_uri);
			}

			logging::warn(fmt::runtime(token_error_log));
			return true;
		}

		return false;
	}

	auto decode_username_and_password(std::string_view _encoded_data) -> std::pair<std::string, std::string>
	{
		std::string authorization{_encoded_data};
		boost::trim(authorization);
		logging::debug("{}: Authorization value (trimmed): [{}]", __func__, authorization);

		constexpr auto max_creds_size = 128;
		std::uint64_t size{max_creds_size};
		std::array<std::uint8_t, max_creds_size> creds{};
		// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
		const auto ec = irods::base64_decode(
			reinterpret_cast<unsigned char*>(authorization.data()), authorization.size(), creds.data(), &size);
		logging::debug("{}: base64 - error code=[{}], decoded size=[{}]", __func__, ec, size);

		// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
		std::string_view sv{reinterpret_cast<char*>(creds.data()), size};

		const auto colon = sv.find(':');
		if (colon == std::string_view::npos) {
			return {"", ""};
		}

		std::string username{sv.substr(0, colon)};
		std::string password{sv.substr(colon + 1)};

		return {std::move(username), std::move(password)};
	}

	auto is_oidc_running_as_client() -> bool
	{
		static const auto oidc_stanza_exists = irods::http::globals::configuration().contains(
			nlohmann::json::json_pointer{"/http_server/authentication/openid_connect"});

		if (oidc_stanza_exists) {
			static const auto is_client{
				irods::http::globals::oidc_configuration().at("mode").get_ref<const std::string&>() == "client"};
			return is_client;
		}

		return false;
	}

	IRODS_HTTP_API_ENDPOINT_ENTRY_FUNCTION_SIGNATURE(authentication)
	{
		if (_req.method() == boost::beast::http::verb::get) {
			if (!is_oidc_running_as_client()) {
				logging::error(*_sess_ptr, "{}: HTTP GET method cannot be used for Basic authentication.", __func__);
				return _sess_ptr->send(fail(status_type::method_not_allowed));
			}

			url url;
			bool did_except{false};
			try {
				url = irods::http::parse_url(_req);
			}
			catch (irods::exception& e) {
				did_except = true;
			}

			if (did_except) {
				irods::http::globals::background_task([fn = __func__, _sess_ptr, _req = std::move(_req)] {
					const auto timeout{
						irods::http::globals::oidc_configuration().at("state_timeout_in_seconds").get<int>()};
					const auto state{irods::http::process_stash::insert(
						std::chrono::steady_clock::now() + std::chrono::seconds(timeout))};

					body_arguments args{
						{"client_id",
					     irods::http::globals::oidc_configuration().at("client_id").get_ref<const std::string&>()},
						{"response_type", "code"},
						{"scope", "openid"},
						{"redirect_uri",
					     irods::http::globals::oidc_configuration().at("redirect_uri").get_ref<const std::string&>()},
						{"state", state}};

					const auto auth_endpoint{irods::http::globals::oidc_endpoint_configuration()
					                             .at("authorization_endpoint")
					                             .get_ref<const std::string&>()};
					const auto encoded_url{fmt::format("{}?{}", auth_endpoint, irods::http::url_encode_body(args))};

					logging::debug(*_sess_ptr, "{}: Proper redirect to [{}]", fn, encoded_url);

					response_type res{status_type::found, _req.version()};
					res.set(field_type::server, irods::http::version::server_name);
					res.set(field_type::location, encoded_url);
					res.keep_alive(_req.keep_alive());
					res.prepare_payload();

					return _sess_ptr->send(std::move(res));
				});
			}
			else {
				irods::http::globals::background_task([fn = __func__,
				                                       _sess_ptr,
				                                       _req = std::move(_req),
				                                       url = std::move(url)] {
					// Will always be in response, as we always send it out
					const auto state_iter{url.query.find("state")};

					// Invalid/Fake request... Should have state query param
					if (state_iter == std::end(url.query)) {
						logging::warn(
							*_sess_ptr,
							"{}: Received an Authorization response with no 'state' query parameter. Ignoring.",
							fn);
						return _sess_ptr->send(fail(status_type::bad_request));
					}

					const auto is_state_valid{[](const std::string& _in_state) {
						const auto mapped_value{irods::http::process_stash::find(_in_state)};
						if (!mapped_value) {
							return false;
						}

						const auto* expire_time{
							boost::any_cast<const std::chrono::steady_clock::time_point>(&*mapped_value)};
						if (expire_time == nullptr) {
							return false;
						}

						// Ensure state is not expired...
						const auto is_expired{std::chrono::steady_clock::now() >= *expire_time};

						// Remove item from valid states
						if (!irods::http::process_stash::erase(_in_state)) {
							// Someone else got validated first!
							return false;
						}

						// Return state validity
						return !is_expired;
					}};

					// The state is invalid (i.e. doesn't exist, or have been used)
					if (!is_state_valid(state_iter->second)) {
						logging::warn(
							*_sess_ptr,
							"{}: Received an Authorization response with an invalid 'state' query parameter. Ignoring.",
							fn);
						return _sess_ptr->send(fail(status_type::bad_request));
					}

					// Will only be available if authorization was successful
					const auto code_iter{url.query.find("code")};

					// Code does not exist, process response for error details...
					if (code_iter == std::end(url.query)) {
						const auto error_iter{url.query.find("error")};

						// Required error parameter missing, malformed response
						if (error_iter == std::end(url.query)) {
							logging::warn(
								*_sess_ptr,
								"{}: Received an Authorization response with no 'code' or 'error' query parameters. "
								"Ignoring.",
								fn);
							return _sess_ptr->send(fail(status_type::bad_request));
						}
						std::string responses;
						responses.reserve(500);

						auto responses_iter{fmt::format_to(
							std::back_inserter(responses), "{}: Error Code [{}]", fn, error_iter->second)};

						// Optional OAuth 2.0 error parameters follow
						const auto error_description_iter{url.query.find("error_description")};
						if (error_description_iter != std::end(url.query)) {
							responses_iter = fmt::format_to(
								responses_iter, ", Error Description [{}]", error_description_iter->second);
						}

						const auto error_uri_iter{url.query.find("error_uri")};
						if (error_uri_iter != std::end(url.query)) {
							responses_iter = fmt::format_to(responses_iter, ", Error URI [{}]", error_uri_iter->second);
						}

						logging::warn(*_sess_ptr, fmt::runtime(responses));

						return _sess_ptr->send(fail(status_type::bad_request));
					}

					// We have a (possibly) valid code, and a valid state!
					// We can attempt to retrieve a token!

					// Populate arguments
					body_arguments args{
						{"grant_type", "authorization_code"},
						{"client_id",
					     irods::http::globals::oidc_configuration().at("client_id").get_ref<const std::string&>()},
						{"code", code_iter->second},
						{"redirect_uri",
					     irods::http::globals::oidc_configuration().at("redirect_uri").get_ref<const std::string&>()}};

					// Encode the string, hit endpoint, get res
					nlohmann::json oidc_response{hit_token_endpoint(
						irods::http::url_encode_body(remove_client_from_body_if_confidential_client(args)))};

					// Determine if we have an "error" json...
					if (is_error_response(oidc_response)) {
						return _sess_ptr->send(fail(status_type::bad_request));
					}

					// Not an error, likely to have id_token
					// TODO: Consider handling bit flip cases
					const std::string jwt_token{oidc_response.at("id_token").get_ref<const std::string&>()};

					// Get OIDC token && feed to JWT parser
					// TODO: Handle case where we throw!!!
					auto decoded_token{jwt::decode<jwt::traits::nlohmann_json>(jwt_token).get_payload_json()};

					auto irods_username{irods::http::map_json_to_user(decoded_token)};

					if (!irods_username) {
						const auto user{
							decoded_token.contains("preferred_username")
								? decoded_token.at("preferred_username").get<const std::string>()
								: ""};

						logging::error(
							*_sess_ptr, "{}: No irods user associated with authenticated user [{}].", fn, user);
						return _sess_ptr->send(fail(status_type::bad_request));
					}

					// Issue token?
					static const auto seconds =
						irods::http::globals::configuration()
							.at(nlohmann::json::json_pointer{
								"/http_server/authentication/openid_connect/timeout_in_seconds"})
							.get<int>();

					auto bearer_token = irods::http::process_stash::insert(authenticated_client_info{
						.auth_scheme = authorization_scheme::openid_connect,
						.username = *std::move(irods_username),
						.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds{seconds}});

					response_type res_rep{status_type::ok, _req.version()};
					res_rep.set(field_type::server, irods::http::version::server_name);
					res_rep.set(field_type::content_type, "text/plain");
					res_rep.keep_alive(_req.keep_alive());
					res_rep.body() = std::move(bearer_token);
					res_rep.prepare_payload();

					return _sess_ptr->send(std::move(res_rep));
				});
			}
		}
		// Handle posts
		else if (_req.method() == boost::beast::http::verb::post) {
			irods::http::globals::background_task([fn = __func__, _sess_ptr, _req = std::move(_req)] {
				const auto& hdrs{_req.base()};
				const auto iter{hdrs.find("authorization")};

				if (iter == std::end(hdrs)) {
					return _sess_ptr->send(fail(status_type::bad_request));
				}

				logging::debug(*_sess_ptr, "{}: Authorization value: [{}]", fn, iter->value());

				// Basic Auth case
				if (const auto pos{iter->value().find("Basic ")}; pos != std::string_view::npos) {
					constexpr auto basic_auth_scheme_prefix_size = 6;
					auto [username, password]{
						decode_username_and_password(iter->value().substr(pos + basic_auth_scheme_prefix_size))};

					static const auto seconds =
						irods::http::globals::configuration()
							.at(nlohmann::json::json_pointer{"/http_server/authentication/basic/timeout_in_seconds"})
							.get<int>();

					// The anonymous user account must be handled in a special way because rc_check_auth_credentials
					// doesn't support it. To get around that, the HTTP API will return a bearer token whenever the
					// anonymous user is seen. If the iRODS zone doesn't contain an anonymous user, any request sent
					// by the client will result in an error.
					//
					// The error will occur when rc_switch_user is invoked on the non-existent user.
					if ("anonymous" == username && password.empty()) {
						logging::trace(
							*_sess_ptr,
							"{}: Detected the anonymous user account. Skipping auth check and returning token.",
							fn);

						auto bearer_token = irods::http::process_stash::insert(authenticated_client_info{
							.auth_scheme = authorization_scheme::basic,
							.username = std::move(username),
							.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds{seconds}});

						response_type res{status_type::ok, _req.version()};
						res.set(field_type::server, irods::http::version::server_name);
						res.set(field_type::content_type, "text/plain");
						res.keep_alive(_req.keep_alive());
						res.body() = std::move(bearer_token);
						res.prepare_payload();

						return _sess_ptr->send(std::move(res));
					}

					if (username.empty() || password.empty()) {
						return _sess_ptr->send(fail(status_type::unauthorized));
					}

					bool login_successful = false;

					try {
						using json_pointer = nlohmann::json::json_pointer;

						static const auto& config = irods::http::globals::configuration();
						static const auto& rodsadmin_username =
							config.at(json_pointer{"/irods_client/proxy_admin_account/username"})
								.get_ref<const std::string&>();
						static const auto& rodsadmin_password =
							config.at(json_pointer{"/irods_client/proxy_admin_account/password"})
								.get_ref<const std::string&>();
						static const auto& zone =
							config.at(json_pointer{"/irods_client/zone"}).get_ref<const std::string&>();

						if (config.at(json_pointer{"/irods_client/enable_4_2_compatibility"}).get<bool>()) {
							// When operating in 4.2 compatibility mode, all we can do is create a new iRODS connection
							// and authenticate using the client's username and password. iRODS 4.2 does not provide an
							// API for checking native authentication credentials.

							const auto& host =
								config.at(json_pointer{"/irods_client/host"}).get_ref<const std::string&>();
							const auto port = config.at(json_pointer{"/irods_client/port"}).get<int>();

							irods::experimental::client_connection conn{
								irods::experimental::defer_authentication, host, port, {username, zone}};

							login_successful =
								(clientLoginWithPassword(static_cast<RcComm*>(conn), password.data()) == 0);
						}
						else {
							// If we're in this branch, assume we're talking to an iRODS 4.3.1+ server. Therefore, we
							// can use existing iRODS connections to verify the correctness of client provided
							// credentials for native authentication.

							CheckAuthCredentialsInput input{};
							username.copy(input.username, sizeof(CheckAuthCredentialsInput::username));
							zone.copy(input.zone, sizeof(CheckAuthCredentialsInput::zone));

							namespace adm = irods::experimental::administration;
							const adm::user_password_property prop{password, rodsadmin_password};
							const auto obfuscated_password =
								irods::experimental::administration::obfuscate_password(prop);
							obfuscated_password.copy(input.password, sizeof(CheckAuthCredentialsInput::password));

							int* correct{};

							// NOLINTNEXTLINE(cppcoreguidelines-owning-memory, cppcoreguidelines-no-malloc)
							irods::at_scope_exit free_memory{[&correct] { std::free(correct); }};

							auto conn = irods::get_connection(rodsadmin_username);

							if (const auto ec = rc_check_auth_credentials(static_cast<RcComm*>(conn), &input, &correct);
							    ec < 0) {
								logging::error(
									*_sess_ptr,
									"{}: Error verifying native authentication credentials for user [{}]: error code "
									"[{}].",
									fn,
									username,
									ec);
							}
							else {
								logging::debug(*_sess_ptr, "{}: correct = [{}]", fn, fmt::ptr(correct));
								logging::debug(*_sess_ptr, "{}: *correct = [{}]", fn, (correct ? *correct : -1));
								login_successful = (correct && 1 == *correct);
							}
						}
					}
					catch (const irods::exception& e) {
						logging::error(
							*_sess_ptr,
							"{}: Error verifying native authentication credentials for user [{}]: {}",
							fn,
							username,
							e.client_display_what());
					}
					catch (const std::exception& e) {
						logging::error(
							*_sess_ptr,
							"{}: Error verifying native authentication credentials for user [{}]: {}",
							fn,
							username,
							e.what());
					}

					if (!login_successful) {
						return _sess_ptr->send(fail(status_type::unauthorized));
					}

					auto bearer_token = irods::http::process_stash::insert(authenticated_client_info{
						.auth_scheme = authorization_scheme::basic,
						.username = std::move(username),
						.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds{seconds}});

					response_type res{status_type::ok, _req.version()};
					res.set(field_type::server, irods::http::version::server_name);
					res.set(field_type::content_type, "text/plain");
					res.keep_alive(_req.keep_alive());
					res.body() = std::move(bearer_token);
					res.prepare_payload();

					return _sess_ptr->send(std::move(res));
				}
				// OAuth 2.0 Resource Owner Password Credentials Grant
				else if (const auto alt_method{iter->value().find("iRODS ")};
				         is_oidc_running_as_client() && alt_method != std::string_view::npos)
				{
					// Decode username and password here!!!!!
					constexpr auto basic_auth_scheme_prefix_size = 6;
					const auto [username, password]{
						decode_username_and_password(iter->value().substr(alt_method + basic_auth_scheme_prefix_size))};

					if (username.empty() || password.empty()) {
						return _sess_ptr->send(fail(status_type::unauthorized));
					}

					// Build up arguments for OIDC Token endpoint
					body_arguments args{
						{"client_id",
					     irods::http::globals::oidc_configuration().at("client_id").get_ref<const std::string&>()},
						{"grant_type", "password"},
						{"scope", "openid"},
						{"username", username},
						{"password", password}};

					// Query endpoint
					nlohmann::json oidc_response{hit_token_endpoint(
						irods::http::url_encode_body(remove_client_from_body_if_confidential_client(args)))};

					// Determine if we have an "error" json...
					if (is_error_response(oidc_response)) {
						return _sess_ptr->send(fail(status_type::bad_request));
					}

					// Assume passed, get oidc token
					const std::string& jwt_token{oidc_response.at("id_token").get_ref<const std::string&>()};

					// Feed to JWT parser
					auto decoded_token{jwt::decode<jwt::traits::nlohmann_json>(jwt_token).get_payload_json()};
					auto irods_username{irods::http::map_json_to_user(decoded_token)};

					if (!irods_username) {
						const auto user{
							decoded_token.contains("preferred_username")
								? decoded_token.at("preferred_username").get<const std::string>()
								: ""};

						logging::error(
							*_sess_ptr, "{}: No irods user associated with authenticated user [{}].", fn, user);
						return _sess_ptr->send(fail(status_type::bad_request));
					}

					// Issue token?
					static const auto seconds =
						irods::http::globals::configuration()
							.at(nlohmann::json::json_pointer{
								"/http_server/authentication/openid_connect/timeout_in_seconds"})
							.get<int>();
					auto bearer_token = irods::http::process_stash::insert(authenticated_client_info{
						.auth_scheme = authorization_scheme::openid_connect,
						.username = *std::move(irods_username),
						.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds{seconds}});

					response_type res_rep{status_type::ok, _req.version()};
					res_rep.set(field_type::server, irods::http::version::server_name);
					res_rep.set(field_type::content_type, "text/plain");
					res_rep.keep_alive(_req.keep_alive());
					res_rep.body() = std::move(bearer_token);
					res_rep.prepare_payload();

					return _sess_ptr->send(std::move(res_rep));
				}

				// Fail case
				return _sess_ptr->send(fail(status_type::bad_request));
			});
		}
		else {
			// Nothing recognized
			logging::error(*_sess_ptr, "{}: HTTP method not supported.", __func__);
			return _sess_ptr->send(fail(status_type::method_not_allowed));
		}
	} // authentication
} //namespace irods::http::handler
