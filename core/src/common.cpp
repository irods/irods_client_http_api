#include "irods/private/http_api/common.hpp"

#include "irods/private/http_api/globals.hpp"
#include "irods/private/http_api/log.hpp"
#include "irods/private/http_api/multipart_form_data.hpp"
#include "irods/private/http_api/process_stash.hpp"
#include "irods/private/http_api/session.hpp"
#include "irods/private/http_api/transport.hpp"
#include "irods/private/http_api/version.hpp"

#include <irods/base64.hpp>
#include <irods/client_connection.hpp>
#include <irods/irods_at_scope_exit.hpp>
#include <irods/irods_exception.hpp>
#include <irods/rcConnect.h>
#include <irods/rcMisc.h> // For addKeyVal().
#include <irods/rodsErrorTable.h>
#include <irods/rodsKeyWdDef.h> // For KW_CLOSE_OPEN_REPLICAS.
#include <irods/switch_user.h>
#include <irods/ticketAdmin.h>

#include <boost/any.hpp>
#include <boost/asio.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/beast.hpp>

#include <curl/curl.h>
#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/nlohmann-json/traits.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <string>
#include <string_view>

// clang-format off
namespace beast = boost::beast; // from <boost/beast.hpp>
namespace net   = boost::asio;  // from <boost/asio.hpp>
// clang-format on

namespace irods::http
{
	auto fail(response_type& _response, status_type _status, const std::string_view _error_msg) -> response_type
	{
		_response.result(_status);
		_response.set(field_type::server, version::server_name);
		_response.set(field_type::content_type, "application/json");
		_response.body() = _error_msg;
		_response.prepare_payload();
		return _response;
	} // fail

	auto fail(response_type& _response, status_type _status) -> response_type
	{
		return fail(_response, _status, "");
	} // fail

	auto fail(status_type _status, const std::string_view _error_msg) -> response_type
	{
		response_type r{_status, 11};
		return fail(r, _status, _error_msg);
	} // fail

	auto fail(status_type _status) -> response_type
	{
		response_type r{_status, 11};
		return fail(r, _status, "");
	} // fail

	auto decode(const std::string_view _v) -> std::string
	{
		std::string result;
		int decoded_length = -1;

		if (auto* decoded = curl_easy_unescape(nullptr, _v.data(), static_cast<int>(_v.size()), &decoded_length);
		    decoded) {
			std::unique_ptr<char, void (*)(void*)> s{decoded, curl_free};
			result.assign(decoded, decoded_length);
		}
		else {
			result.assign(_v);
		}

		return result;
	} // decode

	auto encode(std::string_view _to_encode) -> std::string
	{
		char* tmp_encoded_data{curl_easy_escape(nullptr, _to_encode.data(), _to_encode.size())};
		if (tmp_encoded_data == nullptr) {
			return {std::cbegin(_to_encode), std::cend(_to_encode)};
		}

		std::string encoded_data{tmp_encoded_data};

		curl_free(tmp_encoded_data);
		return encoded_data;
	} // encode

	// TODO Create a better name.
	auto to_argument_list(const std::string_view _urlencoded_string) -> std::unordered_map<std::string, std::string>
	{
		if (_urlencoded_string.empty()) {
			return {};
		}

		std::unordered_map<std::string, std::string> kvps;

		std::vector<std::string> tokens;
		boost::split(tokens, _urlencoded_string, boost::is_any_of("&"));

		std::vector<std::string> kvp;

		for (auto&& t : tokens) {
			boost::split(kvp, t, boost::is_any_of("="));

			if (kvp.size() == 2) {
				auto value = decode(kvp[1]);
				boost::replace_all(value, "+", " ");
				kvps.insert_or_assign(std::move(kvp[0]), value);
			}
			else if (kvp.size() == 1) {
				kvps.insert_or_assign(std::move(kvp[0]), "");
			}

			kvp.clear();
		}

		return kvps;
	} // to_argument_list

	auto get_url_path(const std::string& _url) -> std::optional<std::string>
	{
		namespace logging = irods::http::log;

		std::unique_ptr<CURLU, void (*)(CURLU*)> curl{curl_url(), curl_url_cleanup};

		if (!curl) {
			logging::error("{}: Could not initialize libcurl.", __func__);
			return std::nullopt;
		}

		if (const auto ec = curl_url_set(curl.get(), CURLUPART_URL, _url.c_str(), 0); ec) {
			logging::error("{}: curl_url_set error: {}", __func__, ec);
			return std::nullopt;
		}

		using curl_string = std::unique_ptr<char, void (*)(void*)>;

		// Extract the path.
		// This is what we use to route requests to the various endpoints.
		char* path{};
		const auto ec = curl_url_get(curl.get(), CURLUPART_PATH, &path, 0);

		if (ec == 0) {
			curl_string cpath{path, curl_free};
			return path;
		}

		logging::error("{}: curl_url_get(CURLUPART_PATH) error: {}", __func__, ec);
		return std::nullopt;
	} // get_url_path

	auto parse_url(const std::string& _url) -> url
	{
		namespace logging = irods::http::log;

		std::unique_ptr<CURLU, void (*)(CURLU*)> curl{curl_url(), curl_url_cleanup};

		if (!curl) {
			logging::error("{}: Could not initialize CURLU handle.", __func__);
			THROW(SYS_LIBRARY_ERROR, "curl_url error.");
		}

		// Include a bogus prefix. We only care about the path and query parts of the URL.
		if (const auto ec = curl_url_set(curl.get(), CURLUPART_URL, _url.c_str(), 0); ec) {
			logging::error("{}: curl_url_set error: {}", __func__, ec);
			THROW(SYS_LIBRARY_ERROR, "curl_url_set(CURLUPART_URL) error.");
		}

		url url;

		using curl_string = std::unique_ptr<char, void (*)(void*)>;

		// Extract the path.
		// This is what we use to route requests to the various endpoints.
		char* path{};
		if (const auto ec = curl_url_get(curl.get(), CURLUPART_PATH, &path, 0); ec == 0) {
			curl_string cpath{path, curl_free};
			if (path) {
				url.path = path;
			}
		}
		else {
			logging::error("{}: curl_url_get(CURLUPART_PATH) error: {}", __func__, ec);
			THROW(SYS_LIBRARY_ERROR, "curl_url_get(CURLUPART_PATH) error.");
		}

		// Extract the query.
		// ChatGPT states that the values in the key value pairs must escape embedded equal signs.
		// This allows the HTTP server to parse the query string correctly. Therefore, we don't have
		// to protect against that case. The client must send the correct URL escaped input.
		char* query{};
		if (const auto ec = curl_url_get(curl.get(), CURLUPART_QUERY, &query, 0); ec == 0) {
			curl_string cs{query, curl_free};
			if (query) {
				url.query = to_argument_list(query);
			}
		}
		else {
			logging::error("{}: curl_url_get(CURLUPART_QUERY) error: {}", __func__, ec);
			THROW(SYS_LIBRARY_ERROR, "curl_url_get(CURLUPART_QUERY) error.");
		}

		return url;
	} // parse_url

	auto parse_url(const request_type& _req) -> url
	{
		return parse_url(fmt::format("http://ignored{}", _req.target()));
	} // parse_url

	auto url_encode_body(const body_arguments& _args) -> std::string
	{
		auto encode_pair{[](const body_arguments::value_type& i) {
			return fmt::format("{}={}", encode(i.first), encode(i.second));
		}};

		return std::transform_reduce(
			std::next(std::cbegin(_args)),
			std::cend(_args),
			encode_pair(*std::cbegin(_args)),
			[](const auto& a, const auto& b) { return fmt::format("{}&{}", a, b); },
			encode_pair);
	}

	auto safe_base64_encode(std::string_view _view) -> std::string
	{
		namespace logging = irods::http::log;

		constexpr auto char_per_byte_set{4};
		constexpr auto byte_set_size{3};

		const auto max_size{char_per_byte_set * ((_view.size() + 2) / byte_set_size)};
		auto max_size_plus_null_term{max_size + 1};

		std::string encoded_data;
		encoded_data.resize(max_size);

		auto res{irods::base64_encode(
			reinterpret_cast<const unsigned char*>(_view.data()),
			_view.size(),
			reinterpret_cast<unsigned char*>(encoded_data.data()),
			&max_size_plus_null_term)};
		if (res) {
			logging::error("{}: Failed to encode the string [{}], output may be unusable.", __func__, _view);
		}
		return encoded_data;
	}

	auto create_host_field(boost::urls::url_view _url, std::string_view _port) -> std::string
	{
		if ((_port == "443" && _url.scheme_id() == boost::urls::scheme::https) ||
		    (_port == "80" && _url.scheme_id() == boost::urls::scheme::http))
		{
			return _url.host();
		}
		return fmt::format("{}:{}", _url.host(), _port);
	}

	auto create_oidc_request(boost::urls::url_view _url) -> beast::http::request<beast::http::string_body>
	{
		constexpr auto http_version_number{11};
		beast::http::request<beast::http::string_body> req{beast::http::verb::post, _url.path(), http_version_number};

		const auto port{get_port_from_url(_url)};

		req.set(beast::http::field::host, create_host_field(_url, *port));
		req.set(beast::http::field::user_agent, irods::http::version::server_name);
		req.set(beast::http::field::content_type, "application/x-www-form-urlencoded");
		req.set(beast::http::field::accept, "application/json");

		if (const auto secret_key{irods::http::globals::oidc_configuration().find("client_secret")};
		    secret_key != std::end(irods::http::globals::oidc_configuration()))
		{
			const auto format_bearer_token{[](std::string_view _client_id, std::string_view _client_secret) {
				auto encode_me{fmt::format("{}:{}", encode(_client_id), encode(_client_secret))};
				return safe_base64_encode(encode_me);
			}};

			const auto& client_id{
				irods::http::globals::oidc_configuration().at("client_id").get_ref<const std::string&>()};
			const auto& client_secret{secret_key->get_ref<const std::string&>()};
			const auto auth_string{fmt::format("Basic {}", format_bearer_token(client_id, client_secret))};

			req.set(beast::http::field::authorization, auth_string);
		}

		return req;
	}

	auto hit_introspection_endpoint(std::string _encoded_body) -> nlohmann::json
	{
		namespace logging = irods::http::log;

		const auto introspection_endpoint{irods::http::globals::oidc_endpoint_configuration()
		                                      .at("introspection_endpoint")
		                                      .get_ref<const std::string&>()};

		const auto parsed_uri{boost::urls::parse_uri(introspection_endpoint)};

		if (parsed_uri.has_error()) {
			logging::error(
				"{}: Error trying to parse introspection_endpoint [{}]. Please check configuration.",
				__func__,
				introspection_endpoint);
			return {{"error", "bad endpoint"}};
		}

		const auto url{*parsed_uri};
		const auto port{get_port_from_url(url)};

		// Addr
		net::io_context io_ctx;
		auto tcp_stream{irods::http::transport_factory(url.scheme_id(), io_ctx)};
		tcp_stream->connect(url.host(), *port);

		// Build Request
		auto req{create_oidc_request(url)};
		req.body() = std::move(_encoded_body);
		req.prepare_payload();

		// Send request & receive response
		auto res{tcp_stream->communicate(req)};

		logging::debug("{}: Received the following response: [{}]", __func__, res.body());

		// JSONize response
		return nlohmann::json::parse(res.body());
	}

	auto map_json_to_user(const nlohmann::json& _json) -> std::optional<std::string>
	{
		namespace logging = irods::http::log;

		const auto json_res_string{to_string(_json)};
		const static auto match_func{
			irods::http::globals::user_mapping_lib().get<int(const char*, char**)>("user_mapper_match")};
		const static auto free_func{irods::http::globals::user_mapping_lib().get<void(char*)>("user_mapper_free")};

		char* res{};
		if (auto rc{match_func(json_res_string.c_str(), &res)}; rc != 0) {
			logging::error("{}: An error occured when attempting to match with error code [{}].", __func__, rc);
			return std::nullopt;
		}

		if (nullptr != res) {
			std::string matched_user{res};
			free_func(res);

			return matched_user;
		}

		return std::nullopt;
	}

	/// Validates an OAuth 2.0 Access Token using the Introspection Endpoint
	/// See RFC 7662 for more details
	///
	/// \returns An optional containing a nlohmann::json object if verification was successful. Otherwise,
	///          an empty optional is returned.
	auto validate_using_introspection_endpoint(std::string _bearer_token) -> std::optional<nlohmann::json> {
		namespace logging = irods::http::log;

		body_arguments args{{"token", _bearer_token}, {"token_type_hint", "access_token"}};

		auto json_res{hit_introspection_endpoint(url_encode_body(args))};

		// Validate access token
		if (!json_res.at("active").get<bool>()) {
			logging::warn("{}: Access token is invalid or expired.", __func__);
			return std::nullopt;
		}

		return json_res;
	}


	/// Fetches JWKs from the location specified by the OpenID Provider
	///
	/// See OpenID Connect Discovery 1.0 Section 3 for info on jwks_uri
	/// See RFC 7517 for more information on JSON Web Key (JWK)
	///
	/// \returns A std::string represting the JWKs from the OpenID Provider
	///
	/// \todo Stash the results from this function, only allow to run once.
	auto fetch_jwks_from_openid_provider() -> std::string {
		namespace logging = irods::http::log;

		const auto jwks_uri{irods::http::globals::oidc_endpoint_configuration()
		                                      .at("jwks_uri")
		                                      .get_ref<const std::string&>()};

		const auto parsed_uri{boost::urls::parse_uri(jwks_uri)};

		if (parsed_uri.has_error()) {
			logging::error(
				"{}: Error trying to parse jwks_uri [{}]. Please check configuration.",
				__func__,
				jwks_uri);
			return "{\"error\", \"bad endpoint\"}";
		}

		const auto url{*parsed_uri};
		const auto port{get_port_from_url(url)};

		// Addr
		net::io_context io_ctx;
		auto tcp_stream{irods::http::transport_factory(url.scheme_id(), io_ctx)};
		tcp_stream->connect(url.host(), *port);

		// Build Request
		constexpr auto http_version_number{11};
		beast::http::request<beast::http::string_body> req{beast::http::verb::get, url.path(), http_version_number};
		req.set(beast::http::field::host, irods::http::create_host_field(url, *port));
		req.set(beast::http::field::user_agent, irods::http::version::server_name);
		req.set(beast::http::field::accept, "application/json");
		req.prepare_payload();

		// Send request & receive response
		auto res{tcp_stream->communicate(req)};

		logging::debug("{}: Received the following response: [{}]", __func__, res.body());

		// JSONize response
		return res.body();
	}

	/// Adds the specified algorithm \p _alg to the verifier \p _verifier, using the additional informaiton provided by the JWK \p _jwk.
	///
	/// See RFC 7518 for details on JSON Web Algorithms (JWA)
	///
	/// \param[in,out] _verifier The jwt::verifier to add additional verification algorithms to.
	/// \param[in]     _jwk      The JWK containing the required JWA information.
	/// \param[in]     _alg      The sigining algorithm requested by the signed JWT.
	///
	/// \todo Should this be separated out by algorithm familiy?
	auto add_alg_from_jwk(jwt::verifier<jwt::default_clock, jwt::traits::nlohmann_json>& _verifier, const jwt::jwk<jwt::traits::nlohmann_json>& _jwk, std::string_view _alg) -> void {
		namespace logging = irods::http::log;

		if (_alg == "RS256") {
			logging::trace("{}: Detected [RS256], attempting extraction of attributes from JWK...", __func__);

			// Get modulus parameter (JWA)
			auto mod{_jwk.get_jwk_claim("n").as_string()};

			// Get exponent parameter (JWA)
			auto exp{_jwk.get_jwk_claim("e").as_string()};

			// Add verification algorithm
			_verifier.allow_algorithm(jwt::algorithm::rs256(jwt::helper::create_public_key_from_rsa_components(mod, exp)));
		}
		// For symetric excryption, no parameter 'k' is provided
		// See OpenID Connect Discovery section 3
		if (_alg == "HS256") {
			logging::trace("{}: Detected [HS256], attempting extraction of attributes from JWK...", __func__);

			// Attempt to get key value parameter (JWA)
			auto key{_jwk.get_jwk_claim("k").as_string()};

			// Add verification algorithm
			_verifier.allow_algorithm(jwt::algorithm::hs256(key));
		}
		if (_alg == "ES256") {
			logging::trace("{}: Detected [ES256], attempting extraction of attributes from JWK...", __func__);

			// Get curve parameter (JWA)
			auto crv{_jwk.get_jwk_claim("crv").as_string()};

			// Get x coordinate parameter (JWA)
			auto x{_jwk.get_jwk_claim("x").as_string()};

			// Get y coordinate parameter (JWA)
			auto y{_jwk.get_jwk_claim("y").as_string()};

			// Add verification algorithm
			_verifier.allow_algorithm(jwt::algorithm::es256(jwt::helper::create_public_key_from_ec_components(crv, x, y)));
		}
	}

	/// Bleh blah
	///
	/// \returns A refernce to the provided jwt::verifier, allowing for chaining.
	auto add_algorithms_to_verifier(jwt::verifier<jwt::default_clock, jwt::traits::nlohmann_json>& _verifier, const jwt::jwks<jwt::traits::nlohmann_json>& _jwks, std::string_view _alg) -> jwt::verifier<jwt::default_clock, jwt::traits::nlohmann_json>& {
		namespace logging = irods::http::log;

		// Get the JWK the access token was signed with. This is optional.
		// See RFC 7515 Section 4.1.4
		try {
			auto jwk{_jwks.get_jwk(decoded_token.get_key_id())};
			add_alg_from_jwk(_verifier, jwk, _alg);
		}
		catch (const std::runtime_error& e) {
			// We cannot pick out the specific key used...
			// Find JWKs that match any of the following:
			//   - 'alg' (optional) matches exactly
			//   - 'kty' (required) matches 'alg' family [EC,RSA,oct]
			//   - 'use' (required if sign & encrypt keys exist) to be 'sig'
			//
			// Additionally, symetric algoritms it /seems/ that
			// "... the octects of the 'client_secret'..." are used for the algorithm...
			const auto algorithm_family{_alg.substr(0,2)};
			std::string search_string;

			// RSA family
			if (algorithm_family == "RS") {
				search_string = "RSA";
			}
			// EC family
			else if (algorithm_family == "ES") {
				search_string = "EC";
			}
			// Symmetric algo
			else if (algorithm_family == "HS") {
				search_string = "oct";
			}

			// Go through entire key set
			std::for_each(std::cbegin(_jwks), std::cend(_jwks), [&_verifier, &_alg, &search_string] (const auto& _jwk) -> void {
				try {
					if (auto key_type{_jwk.get_jwk_claim("kty").as_string()}; key_type == search_string) {
						add_alg_from_jwk(_verifier, _jwk, _alg);
					}
				}
				catch (const std::runtime_error& e) {
					logging::error("{}: Invalid key [{}]", __func__, e.what());
					return;
				}
			});
		}

		// Allow for chaining
		return _verifier;
	}
	
	/// Validates an OAuth 2.0 Access Token using
	///
	/// See RFC 9068 for more details on JWT for OAuth 2.0
	/// See RFC 7515 for details on JSON Web Signature (JWS)
	/// See RFC 7518 for details on JSON Web Algorithms (JWA)
	///
	/// \param[in] _jwt A std::string represeting the JWT to verify.
	///
	/// \returns The JWT payload if the token can be validated. Otherwise, an empty std::optional is returned
	///
	/// \todo Handle encrypted tokens. MUST reject unencrypted tokens if encryption was negotiated at registration.
	/// \date 2024-10-16 jwt-cpp does not support encrypted tokens as of current.
	auto validate_using_local_validation(std::string _jwt) -> std::optional<nlohmann::json> {
		namespace logging = irods::http::log;

		// Decode the JWT
		auto decoded_token{jwt::decode<jwt::traits::nlohmann_json>(_jwt)};

		// Parse the JWKs discoverd from the OpenID Provider
		static auto jwks{jwt::parse_jwks<jwt::traits::nlohmann_json>(fetch_jwks_from_openid_provider())};

		// Manually verify 'typ' matches what is specified in RFC 9068
		// Validate 'typ' is 'at+jwt' or 'application/at+jwt', reject all others
		try {
			auto token_type{decoded_token.get_header_claim("typ").as_string()};
			if (!(token_type == "at+jwk" || token_type == "application/at+jwk")) {
				logging::error("{}: Access Token with [typ] of type [{}] is not supported.", __func__, token_type);
				// return std::nullopt;
			}
		}
		// Handling missing 'typ'
		catch (const jwt::error::claim_not_present_exception& e) {
			logging::error("{}: invalid JWT [{}]", __func__, e.what());
			return std::nullopt;
		}

		// Begin building up the JWT verifier...
		auto verifier{
			jwt::verify<jwt::traits::nlohmann_json>()
			// Token MUST have issuer match what is defined by the OpenID Provider
				.with_issuer(
					irods::http::globals::oidc_endpoint_configuration().at("issuer").get_ref<const std::string&>())
			// 'aud' MUST contain identifier we expect (ourselves)
				.with_audience(
					irods::http::globals::oidc_configuration().at("client_id").get_ref<const std::string&>())};

		// TODO: Refactor into separate function, taking verifier, jwk, token(?) as parameter, returning verifier(?)
		// Details of the specific algoritms are defined by RFC 7518 (JWA)
		try {
			// Use the 'alg' specified in the access token
			auto key_type{decoded_token.get_header_claim("alg").as_string()};
			add_algorithms_to_verifier(verifier, jwks, key_type);
			logging::trace("{}: Extracted [alg] having [{}].", __func__, key_type);

			// Reject 'alg' type of 'none'
			if (key_type == "none") {
				logging::error("{}: Access Token with [alg] of type [none] is not supported.", __func__);
				return std::nullopt;
			}
		}
		// Handle missing 'alg' in jwt
		catch (const jwt::error::claim_not_present_exception& e) {
			logging::error("{}: Invalid jwt [{}]", __func__, e.what());
			return std::nullopt;
		}

		// Attempt token validation
		std::error_code ec;
		verifier.verify(decoded_token, ec);

		if (ec) {
			logging::error("{}: Token verification failed [{}].", __func__, ec.message());
			return std::nullopt;
		}

		logging::trace("{}: Token verification succeeded.", __func__);
		return decoded_token.get_payload_json();
	}
	
	auto resolve_client_identity(const request_type& _req) -> client_identity_resolution_result
	{
		namespace logging = irods::http::log;

		//
		// Extract the Bearer token from the Authorization header.
		//

		const auto& hdrs = _req.base();
		const auto iter = hdrs.find("Authorization");
		if (iter == std::end(hdrs)) {
			logging::error("{}: Missing [Authorization] header.", __func__);
			return {.response = fail(status_type::bad_request)};
		}

		logging::debug("{}: Authorization value: [{}]", __func__, iter->value());

		auto pos = iter->value().find("Bearer ");
		if (std::string_view::npos == pos) {
			logging::debug("{}: Malformed authorization header.", __func__);
			return {.response = fail(status_type::bad_request)};
		}

		std::string bearer_token{iter->value().substr(pos + 7)};
		boost::trim(bearer_token);
		logging::debug("{}: Bearer token: [{}]", __func__, bearer_token);

		// Verify the bearer token is known to the server. If not, return an error.
		auto mapped_value{irods::http::process_stash::find(bearer_token)};
		if (!mapped_value.has_value()) {
			const auto& config = irods::http::globals::configuration();

			// It's possible that the admin didn't include the OIDC configuration stanza.
			// This use-case is allowed, therefore we check for the OIDC configuration before
			// attempting to access it. Without this logic, the server would crash.
			const auto oidc_config_iter =
				config.find(nlohmann::json::json_pointer{"/http_server/authentication/openid_connect"});
			if (oidc_config_iter == std::end(config)) {
				logging::debug("{}: No 'openid_connect' stanza found in server configuration.", __func__);
				logging::error("{}: Could not find bearer token matching [{}].", __func__, bearer_token);
				return {.response = fail(status_type::unauthorized)};
			}

			// If we're running as a protected resource, assume we have a OIDC token
			if (oidc_config_iter->at("mode").get_ref<const std::string&>() == "protected_resource") {
				nlohmann::json json_res;

				// Use introspection endpoint if it exsists
				if (auto introspection_endpoint_iter{irods::http::globals::oidc_endpoint_configuration().find(nlohmann::json::json_pointer{"/introspection_endpoint"})}; introspection_endpoint_iter != std::end(irods::http::globals::oidc_endpoint_configuration())) {
					auto possible_json_res{validate_using_introspection_endpoint(bearer_token)};

					if (!possible_json_res) {
						return {.response = fail(status_type::unauthorized)};
					}

					json_res = *possible_json_res;
				}
				// Otherwise, try parsing as a JWT access token
				else {
					auto possible_json_res{validate_using_local_validation(bearer_token)};

					if (!possible_json_res) {
						return {.response = fail(status_type::unauthorized)};
					}

					json_res = *possible_json_res;
				}

				// Do mapping of user to irods user
				auto user{map_json_to_user(json_res)};
				if (user) {
					return {.client_info = {.username = *std::move(user)}};
				}

				logging::warn("{}: Could not find a matching user.", __func__);
				return {.response = fail(status_type::unauthorized)};
			}

			logging::error("{}: Could not find bearer token matching [{}].", __func__, bearer_token);
			return {.response = fail(status_type::unauthorized)};
		}

		auto* client_info{boost::any_cast<authenticated_client_info>(&*mapped_value)};
		if (client_info == nullptr) {
			logging::error("{}: Could not find bearer token matching [{}].", __func__, bearer_token);
			return {.response = fail(status_type::unauthorized)};
		}

		if (std::chrono::steady_clock::now() >= client_info->expires_at) {
			logging::error("{}: Session for bearer token [{}] has expired.", __func__, bearer_token);
			return {.response = fail(status_type::unauthorized)};
		}

		logging::trace("{}: Client is authenticated.", __func__);
		return {.client_info = std::move(*client_info)};
	} // resolve_client_identity

	auto execute_operation(
		session_pointer_type _sess_ptr,
		request_type& _req,
		const std::unordered_map<std::string, handler_type>& _op_table_get,
		const std::unordered_map<std::string, handler_type>& _op_table_post) -> void
	{
		namespace logging = irods::http::log;

		if (_req.method() == verb_type::get) {
			if (_op_table_get.empty()) {
				logging::error("{}: HTTP method not supported.", __func__);
				return _sess_ptr->send(irods::http::fail(status_type::method_not_allowed));
			}

			auto url = irods::http::parse_url(_req);

			const auto op_iter = url.query.find("op");
			if (op_iter == std::end(url.query)) {
				logging::error("{}: Missing [op] parameter.", __func__);
				return _sess_ptr->send(irods::http::fail(status_type::bad_request));
			}

			if (const auto iter = _op_table_get.find(op_iter->second); iter != std::end(_op_table_get)) {
				return (iter->second)(_sess_ptr, _req, url.query);
			}

			logging::error("{}: Operation [{}] not supported.", __func__, op_iter->second);
			return _sess_ptr->send(fail(status_type::bad_request));
		}

		if (_req.method() == verb_type::post) {
			if (_op_table_post.empty()) {
				logging::error("{}: HTTP method not supported.", __func__);
				return _sess_ptr->send(irods::http::fail(status_type::method_not_allowed));
			}

			query_arguments_type args;

			if (auto content_type = _req.base()["content-type"];
			    boost::istarts_with(content_type, "multipart/form-data")) {
				const auto boundary = irods::http::get_multipart_form_data_boundary(content_type);

				if (!boundary) {
					logging::error("{}: Could not extract [boundary] from [Content-Type] header. ", __func__);
					return _sess_ptr->send(irods::http::fail(status_type::bad_request));
				}

				args = irods::http::parse_multipart_form_data(*boundary, _req.body());
			}
			else if (boost::istarts_with(content_type, "application/x-www-form-urlencoded")) {
				args = irods::http::to_argument_list(_req.body());
			}
			else {
				logging::error("{}: Content type [{}] not supported.", __func__, content_type);
				return _sess_ptr->send(irods::http::fail(status_type::bad_request));
			}

			const auto op_iter = args.find("op");
			if (op_iter == std::end(args)) {
				logging::error("{}: Missing [op] parameter.", __func__);
				return _sess_ptr->send(irods::http::fail(status_type::bad_request));
			}

			if (const auto iter = _op_table_post.find(op_iter->second); iter != std::end(_op_table_post)) {
				return (iter->second)(_sess_ptr, _req, args);
			}

			logging::error("{}: Operation [{}] not supported.", __func__, op_iter->second);
			return _sess_ptr->send(fail(status_type::bad_request));
		}

		logging::error("{}: HTTP method not supported.", __func__);
		return _sess_ptr->send(irods::http::fail(status_type::method_not_allowed));
	} // operation_dispatch

	auto get_port_from_url(boost::urls::url_view _url) -> std::optional<std::string>
	{
		namespace logging = irods::http::log;

		if (_url.has_port()) {
			return _url.port();
		}

		switch (_url.scheme_id()) {
			case boost::urls::scheme::https:
				logging::debug("{}: Detected HTTPS scheme, using port 443.", __func__);
				return "443";
			case boost::urls::scheme::http:
				logging::debug("{}: Detected HTTP scheme, using port 80.", __func__);
				return "80";
			default:
				logging::error("{}: Cannot deduce port from url [{}].", __func__, _url.data());
				return std::nullopt;
		}
	} // get_port_from_url
} // namespace irods::http

namespace irods
{
	auto to_permission_string(const irods::experimental::filesystem::perms _p) -> const char*
	{
		using irods::experimental::filesystem::perms;

		// clang-format off
		switch (_p) {
			case perms::null:            return "null";
			case perms::read_metadata:   return "read_metadata";
			case perms::read_object:
			case perms::read:            return "read_object";
			case perms::create_metadata: return "create_metadata";
			case perms::modify_metadata: return "modify_metadata";
			case perms::delete_metadata: return "delete_metadata";
			case perms::create_object:   return "create_object";
			case perms::modify_object:
			case perms::write:           return "modify_object";
			case perms::delete_object:   return "delete_object";
			case perms::own:             return "own";
		}
		// clang-format on

		THROW(SYS_INVALID_INPUT_PARAM, fmt::format("Cannot convert permission enumeration to string."));
	} // to_permission_string

	auto to_permission_enum(const std::string_view _s) -> std::optional<irods::experimental::filesystem::perms>
	{
		using irods::experimental::filesystem::perms;

		// clang-format off
		if (_s == "null")            { return perms::null; }
		if (_s == "read_metadata")   { return perms::read_metadata; }
		if (_s == "read_object")     { return perms::read; }
		if (_s == "read")            { return perms::read; }
		if (_s == "create_metadata") { return perms::create_metadata; }
		if (_s == "modify_metadata") { return perms::modify_metadata; }
		if (_s == "delete_metadata") { return perms::delete_metadata; }
		if (_s == "create_object")   { return perms::create_object; }
		if (_s == "modify_object")   { return perms::write; }
		if (_s == "write")           { return perms::write; }
		if (_s == "delete_object")   { return perms::delete_object; }
		if (_s == "own")             { return perms::own; }
		// clang-format on

		return std::nullopt;
	} // to_permission_enum

	auto to_object_type_string(const irods::experimental::filesystem::object_type _t) -> const char*
	{
		using irods::experimental::filesystem::object_type;

		// clang-format off
		switch (_t) {
			case object_type::collection:         return "collection";
			case object_type::data_object:        return "data_object";
			case object_type::none:               return "none";
			case object_type::not_found:          return "not_found";
			case object_type::special_collection: return "special_collection";
			case object_type::unknown:            return "unknown";
			default:                              return "?";
		}
		// clang-format on
	} // to_object_type_string

	auto to_object_type_enum(const std::string_view _s) -> std::optional<irods::experimental::filesystem::object_type>
	{
		using irods::experimental::filesystem::object_type;

		// clang-format off
		if (_s == "collection")         { return object_type::collection; }
		if (_s == "data_object")        { return object_type::data_object; }
		if (_s == "none")               { return object_type::none; }
		if (_s == "not_found")          { return object_type::not_found; }
		if (_s == "special_collection") { return object_type::special_collection; }
		if (_s == "unknown")            { return object_type::unknown; }
		// clang-format on

		return std::nullopt;
	} // to_object_type_enum

	auto get_connection(const std::string& _username) -> irods::http::connection_facade
	{
		namespace logging = irods::http::log;
		using json_pointer = nlohmann::json::json_pointer;

		static const auto& config = irods::http::globals::configuration();
		static const auto& irods_client_config = config.at("irods_client");
		static const auto& zone = irods_client_config.at("zone").get_ref<const std::string&>();

		if (config.at(json_pointer{"/irods_client/enable_4_2_compatibility"}).get<bool>()) {
			static const auto& rodsadmin_username =
				irods_client_config.at(json_pointer{"/proxy_admin_account/username"}).get_ref<const std::string&>();
			static auto rodsadmin_password =
				irods_client_config.at(json_pointer{"/proxy_admin_account/password"}).get_ref<const std::string&>();

			irods::experimental::client_connection conn{
				irods::experimental::defer_authentication,
				irods_client_config.at("host").get_ref<const std::string&>(),
				irods_client_config.at("port").get<int>(),
				{rodsadmin_username, zone},
				{_username, zone}};

			auto* conn_ptr = static_cast<RcComm*>(conn);

			if (const auto ec = clientLoginWithPassword(conn_ptr, rodsadmin_password.data()); ec < 0) {
				logging::error("{}: clientLoginWithPassword error: {}", __func__, ec);
				THROW(SYS_INTERNAL_ERR, "clientLoginWithPassword error.");
			}

			return irods::http::connection_facade{std::move(conn)};
		}

		auto conn = irods::http::globals::connection_pool().get_connection();

		logging::trace("{}: Changing identity associated with connection to [{}].", __func__, _username);

		SwitchUserInput input{};

		irods::at_scope_exit clear_options{[&input] { clearKeyVal(&input.options); }};

		irods::strncpy_null_terminated(input.username, _username.c_str());
		irods::strncpy_null_terminated(input.zone, zone.c_str());
		addKeyVal(&input.options, KW_CLOSE_OPEN_REPLICAS, "");

		if (const auto ec = rc_switch_user(static_cast<RcComm*>(conn), &input); ec < 0) {
			logging::error("{}: rc_switch_user error: {}", __func__, ec);
			THROW(ec, "rc_switch_user error.");
		}

		logging::trace("{}: Successfully changed identity associated with connection to [{}].", __func__, _username);

		return irods::http::connection_facade{std::move(conn)};
	} // get_connection

	auto fail(boost::beast::error_code ec, char const* what) -> void
	{
		irods::http::log::error("{}: {}: {}", __func__, what, ec.message());
	} // fail

	auto enable_ticket(RcComm& _comm, const std::string& _ticket) -> int
	{
		TicketAdminInput input{};
		input.arg1 = const_cast<char*>("session"); // NOLINT(cppcoreguidelines-pro-type-const-cast)
		input.arg2 = const_cast<char*>(_ticket.c_str()); // NOLINT(cppcoreguidelines-pro-type-const-cast)
		input.arg3 = const_cast<char*>(""); // NOLINT(cppcoreguidelines-pro-type-const-cast)

		return rcTicketAdmin(&_comm, &input);
	} // enable_ticket
} // namespace irods
