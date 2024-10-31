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

	using jwt_verifier = jwt::verifier<jwt::default_clock, jwt::traits::nlohmann_json>;

	/// Validates an OAuth 2.0 Access Token using the Introspection Endpoint
	/// See RFC 7662 on OAuth 2.0 Token Introspection for more details
	///
	/// \returns An optional containing a nlohmann::json object if verification was successful. Otherwise,
	///          an empty optional is returned.
	auto validate_using_introspection_endpoint(const std::string& _bearer_token) -> std::optional<nlohmann::json>
	{
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
	/// \returns A std::string representing the JWKs from the OpenID Provider
	auto fetch_jwks_from_openid_provider() -> std::string
	{
		namespace logging = irods::http::log;

		const auto jwks_uri{
			irods::http::globals::oidc_endpoint_configuration().at("jwks_uri").get_ref<const std::string&>()};

		const auto parsed_uri{boost::urls::parse_uri(jwks_uri)};

		if (parsed_uri.has_error()) {
			logging::error("{}: Error trying to parse jwks_uri [{}]. Please check configuration.", __func__, jwks_uri);
			throw std::runtime_error{"Invalid [jwks_uri]."};
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

		return res.body();
	}

	/// Adds the specified symmetric algorithm \p _alg to the verifier \p _verifier, using the secrets provided in the
	/// HTTP API configuration
	///
	/// See RFC 7518 for details on JSON Web Algorithms (JWA)
	///
	/// \param[in,out] _verifier The jwt_verifier to add additional verification algorithms to.
	/// \param[in]     _alg      The signing algorithm requested by the signed JWT.
	auto add_symmetric_alg(jwt_verifier& _verifier, std::string_view _alg) -> void
	{
		namespace logging = irods::http::log;

		auto algorithm_family{_alg.substr(0, 2)};
		auto algorithm_specifics{_alg.substr(2)};

		// Extract the secret key
		// OpenID secret should be our secret
		std::string key;
		if (auto realm_secret{irods::http::globals::oidc_configuration().find("realm_secret")};
		    realm_secret != std::end(irods::http::globals::oidc_configuration()))
		{
			key = jwt::base::decode<jwt::alphabet::base64url>(
				jwt::base::pad<jwt::alphabet::base64url>(realm_secret->get_ref<const std::string&>()));
		}
		else if (auto secret{irods::http::globals::oidc_configuration().find("client_secret")};
		         secret != std::end(irods::http::globals::oidc_configuration()))
		{
			key = secret->get<std::string>();
		}

		if (algorithm_family == "HS") {
			if (algorithm_specifics == "256") {
				logging::trace("{}: Adding [{}] to allowed verification algorithms.", __func__, _alg);
				_verifier.allow_algorithm(jwt::algorithm::hs256(key));
			}
			else if (algorithm_specifics == "384") {
				logging::trace("{}: Adding [{}] to allowed verification algorithms.", __func__, _alg);
				_verifier.allow_algorithm(jwt::algorithm::hs384(key));
			}
			else if (algorithm_specifics == "512") {
				logging::trace("{}: Adding [{}] to allowed verification algorithms.", __func__, _alg);
				_verifier.allow_algorithm(jwt::algorithm::hs512(key));
			}
			return;
		}

		logging::warn("{}: Algorithm [{}] is not supported.", __func__, _alg);
	}

	/// Adds the specified asymmetric algorithm \p _alg to the verifier \p _verifier, using the additional information
	/// provided by the JWK \p _jwk.
	///
	/// See RFC 7518 for details on JSON Web Algorithms (JWA)
	///
	/// \param[in,out] _verifier The jwt_verifier to add additional verification algorithms to.
	/// \param[in]     _jwk      The JWK containing the required JWA information.
	/// \param[in]     _alg      The signing algorithm requested by the signed JWT.
	auto add_asymmetric_alg_from_jwk(
		jwt_verifier& _verifier,
		const jwt::jwk<jwt::traits::nlohmann_json>& _jwk,
		std::string_view _alg) -> void
	{
		namespace logging = irods::http::log;

		auto algorithm_family{_alg.substr(0, 2)};
		auto algorithm_specifics{_alg.substr(2)};

		if (algorithm_family == "RS" || algorithm_family == "PS") {
			logging::trace(
				"{}: Detected [{}], attempting extraction of attributes from JWK...", __func__, algorithm_family);

			// Get modulus parameter (JWA Section 6.3.1)
			auto mod{_jwk.get_jwk_claim("n").as_string()};

			// Get exponent parameter (JWA Section 6.3.1)
			auto exp{_jwk.get_jwk_claim("e").as_string()};

			// Create public key
			auto pub_key{jwt::helper::create_public_key_from_rsa_components(mod, exp)};

			// Add verification algorithm
			// NOLINTNEXTLINE(bugprone-branch-clone)
			if (algorithm_family == "RS") {
				// NOLINTNEXTLINE(bugprone-branch-clone)
				if (algorithm_specifics == "256") {
					logging::trace("{}: Adding [{}] to allowed verification algorithms.", __func__, _alg);
					_verifier.allow_algorithm(jwt::algorithm::rs256(pub_key));
				}
				else if (algorithm_specifics == "384") {
					logging::trace("{}: Adding [{}] to allowed verification algorithms.", __func__, _alg);
					_verifier.allow_algorithm(jwt::algorithm::rs384(pub_key));
				}
				else if (algorithm_specifics == "512") {
					logging::trace("{}: Adding [{}] to allowed verification algorithms.", __func__, _alg);
					_verifier.allow_algorithm(jwt::algorithm::rs512(pub_key));
				}
			}
			// "PS"
			else {
				// NOLINTNEXTLINE(bugprone-branch-clone)
				if (algorithm_specifics == "256") {
					logging::trace("{}: Adding [{}] to allowed verification algorithms.", __func__, _alg);
					_verifier.allow_algorithm(jwt::algorithm::ps256(pub_key));
				}
				else if (algorithm_specifics == "384") {
					logging::trace("{}: Adding [{}] to allowed verification algorithms.", __func__, _alg);
					_verifier.allow_algorithm(jwt::algorithm::ps384(pub_key));
				}
				else if (algorithm_specifics == "512") {
					logging::trace("{}: Adding [{}] to allowed verification algorithms.", __func__, _alg);
					_verifier.allow_algorithm(jwt::algorithm::ps512(pub_key));
				}
			}
			return;
		}
		if (algorithm_family == "ES") {
			logging::trace("{}: Detected [ES], attempting extraction of attributes from JWK...", __func__);

			// Get curve parameter (JWA Section 6.2.1)
			auto crv{_jwk.get_curve()};

			// Get x coordinate parameter (JWA Section 6.2.1)
			auto x{_jwk.get_jwk_claim("x").as_string()};

			// Get y coordinate parameter (JWA Section 6.2.1)
			// MUST be present if 'crv' is 'P-256', 'P-384', or 'P-521' (JWA Section 6.2.1)
			auto y{_jwk.get_jwk_claim("y").as_string()};

			// Create public key
			auto pub_key{jwt::helper::create_public_key_from_ec_components(crv, x, y)};

			// Add verification algorithm
			// NOLINTNEXTLINE(bugprone-branch-clone)
			if (algorithm_specifics == "256") {
				logging::trace("{}: Adding [{}] to allowed verification algorithms.", __func__, _alg);
				_verifier.allow_algorithm(jwt::algorithm::es256(pub_key));
			}
			else if (algorithm_specifics == "384") {
				logging::trace("{}: Adding [{}] to allowed verification algorithms.", __func__, _alg);
				_verifier.allow_algorithm(jwt::algorithm::es384(pub_key));
			}
			else if (algorithm_specifics == "512") {
				logging::trace("{}: Adding [{}] to allowed verification algorithms.", __func__, _alg);
				_verifier.allow_algorithm(jwt::algorithm::es512(pub_key));
			}
			return;
		}

		logging::warn("{}: Algorithm [{}] is not supported.", __func__, _alg);
	}

	/// Adds verification algorithm(s) to the \p _verifier based on either the given 'kid' or algorthims matching
	/// the family of the desired algorithm \p _alg.
	///
	/// See RFC 7518 for details on JSON Web Algorithms (JWA)
	///
	/// \param[in,out] _verifier The jwt_verifier to add additional verification algorithms to.
	/// \param[in]     _jwks     The JWKs to search through.
	/// \param[in]     _jwt      The decoded JWT that needs to be verified.
	///
	/// \returns A reference to the provided jwt_verifier, \p _verifier, allowing for chaining.
	auto add_algorithms_to_verifier(
		jwt_verifier& _verifier,
		const jwt::jwks<jwt::traits::nlohmann_json>& _jwks,
		const jwt::decoded_jwt<jwt::traits::nlohmann_json>& _jwt) -> jwt_verifier&
	{
		namespace logging = irods::http::log;

		const auto alg{_jwt.get_algorithm()};

		// Get the JWK the access token was signed with. This is optional.
		// See RFC 7515 Section 4.1.4
		if (_jwt.has_key_id()) {
			auto key_id{_jwt.get_key_id()};
			if (_jwks.has_jwk(key_id)) {
				auto jwk{_jwks.get_jwk(key_id)};
				add_asymmetric_alg_from_jwk(_verifier, jwk, alg);

				return _verifier;
			}
			logging::warn("{}: Could not find the desired [kid] in the JWKs list.", __func__);
		}
		// We cannot pick out the specific key used, go through entire list of JWKs

		// The first two characters of 'alg' should give us
		// enough information to get the algorithm 'family'
		const auto algorithm_family{alg.substr(0, 2)};

		// 'kty' string to search for in JWK
		// The only valid values are 'EC', 'RSA', and 'oct'
		// See (JWA Section 6.1) for the table of valid values
		std::string search_string;

		// RSA family (JWA Section 3.1)
		if (algorithm_family == "RS" || algorithm_family == "PS") {
			// 'kty' string for RSA (JWA Section 6.1)
			search_string = "RSA";
		}
		// EC family (JWA Section 3.1)
		else if (algorithm_family == "ES") {
			// 'kty' string for Elliptic Curve (JWA Section 6.1)
			search_string = "EC";
		}
		// Symmetric algo (JWA Section 3.1)
		else if (algorithm_family == "HS") {
			add_symmetric_alg(_verifier, alg);
			return _verifier;
		}
		// Not a valid or supported algorithm
		else {
			logging::error("{}: 'alg' of [{}] is unsupported.", __func__, alg);
			return _verifier;
		}

		// Go through entire key set
		std::for_each(
			std::cbegin(_jwks),
			std::cend(_jwks),
			[fn = __func__, &_verifier, &alg, &search_string](const auto& _jwk) -> void {
				// Check the optional claims first
			    // Skip JWK if 'use' is not for signing 'sig'
			    // See JWK Section 4.2
				if (_jwk.has_use() && _jwk.get_use() != "sig") {
					logging::trace("{}: JWK not a signing key, ignoring.", fn);
					return;
				}
				// JWK might have 'key_ops', try to select keys based off of this
			    // See JWK Section 4.3
				if (_jwk.has_key_operations() && !_jwk.get_key_operations().contains("verify")) {
					logging::trace("{}: JWK not a key used for verification, ignoring.", fn);
					return;
				}

				if (_jwk.has_algorithm()) {
					// Add the algorithm if 'alg' matches desired.
					if (_jwk.get_algorithm() == alg) {
						add_asymmetric_alg_from_jwk(_verifier, _jwk, alg);
						return;
					}

					logging::trace("{}: JWK [alg] does not match JWT [alg], ignoring.", fn);
					return;
				}

				// Fallback to required claim
				if (_jwk.has_key_type()) {
					// Extract the 'kty' of the JWK, compare to desired 'kty'
					if (_jwk.get_key_type() == search_string) {
						add_asymmetric_alg_from_jwk(_verifier, _jwk, alg);
						return;
					}

					logging::trace("{}: JWK [kty] does not match JWT desired [kty], ignoring.", fn);
					return;
				}

				logging::error("{}: Invalid JWK, missing [kty] claim. Ignoring.", fn);
				return;
			});

		// Allow for chaining
		return _verifier;
	}

	/// Validates an OAuth 2.0 Access Token using provided JWKs and secrets in the HTTP API configuration.
	///
	/// See RFC 9068 for more details on JWT for OAuth 2.0 (OJWT)
	/// See RFC 7516 for details on JSON Web Encryption (JWE)
	/// See RFC 7515 for details on JSON Web Signature (JWS)
	/// See RFC 7518 for details on JSON Web Algorithms (JWA)
	///
	/// \param[in] _jwt A jwt::decoded_jwt<jwt::traits::nlohmann_json> representing the JWT to verify.
	///
	/// \returns The JWT payload if the token can be validated. Otherwise, an empty std::optional is returned
	auto validate_using_local_validation(const jwt::decoded_jwt<jwt::traits::nlohmann_json>& _jwt)
		-> std::optional<nlohmann::json>
	{
		namespace logging = irods::http::log;

		try {
			// Parse the JWKs discovered from the OpenID Provider
			static auto jwks{jwt::parse_jwks<jwt::traits::nlohmann_json>(fetch_jwks_from_openid_provider())};

			// Handling missing 'typ'
			if (!_jwt.has_type()) {
				logging::error("{}: invalid Access Token, missing [typ].", __func__);
				return std::nullopt;
			}

			// 'typ' is case insensitive
			auto token_type{boost::to_lower_copy<std::string>(_jwt.get_type())};

			// Manually verify 'typ' matches what is specified in OJWT
			// Allow for 'JWT'. Typical pre OJWT use had such claims, based on the JWT standard.
			// See OJWT Section 4
			if (!(token_type == "at+jwt" || token_type == "application/at+jwt" || token_type == "jwt")) {
				logging::error("{}: Access Token with [typ] of type [{}] is not supported.", __func__, token_type);
				return std::nullopt;
			}

			// We do not currently support JWEs
			// See JWE Section 4.1.2
			if (_jwt.has_header_claim("enc")) {
				logging::error("{}: JWE is not supported.", __func__);
				return std::nullopt;
			}

			// We do not support nested JWTs
			// This is typically used in JWTs that are signed and then encrypted
			// See JWT Section 5.2
			if (_jwt.has_content_type() && boost::to_lower_copy<std::string>(_jwt.get_content_type()) == "jwt") {
				logging::error("{}: Nested JWTs are not supported.", __func__);
				return std::nullopt;
			}

			// Handle missing 'alg'
			// See JWS Section 4.1.1
			if (!_jwt.has_algorithm()) {
				logging::error("{}: Invalid Access Token, missing [alg].", __func__);
				return std::nullopt;
			}

			// Use the 'alg' specified in the access token
			auto alg{_jwt.get_algorithm()};

			// Reject 'alg' type of 'none'
			// See OJWT Section 4
			if (alg == "none") {
				logging::error("{}: Access Token with [alg] of type [none] is not supported.", __func__);
				return std::nullopt;
			}

			// Reject JWT with JWS 'crit', we do not support extensions using 'crit' at this moment
			// See JWS Section 4.1.11
			if (_jwt.has_header_claim("crit")) {
				logging::error(
					"{}: Access Token with unsupported [crit] claim provided: [{}].",
					__func__,
					_jwt.get_header_claim("crit").as_string());
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

			add_algorithms_to_verifier(verifier, jwks, _jwt);

			// Attempt token validation
			std::error_code ec;
			verifier.verify(_jwt, ec);

			if (ec) {
				logging::error("{}: Token verification failed [{}].", __func__, ec.message());
				return std::nullopt;
			}

			logging::trace("{}: Token verification succeeded.", __func__);
			return _jwt.get_payload_json();
		}
		catch (const std::exception& e) {
			logging::error("{}: Unexpected exception [{}]", __func__, e.what());
			return std::nullopt;
		}
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
			static const auto oidc_conf_exists{
				config.contains(nlohmann::json::json_pointer{"/http_server/authentication/openid_connect"})};
			if (!oidc_conf_exists) {
				logging::debug("{}: No 'openid_connect' stanza found in server configuration.", __func__);
				logging::error("{}: Could not find bearer token matching [{}].", __func__, bearer_token);
				return {.response = fail(status_type::unauthorized)};
			}

			// If we're running as a protected resource, assume we have a OIDC token
			if (irods::http::globals::oidc_configuration().at("mode").get_ref<const std::string&>() ==
			    "protected_resource") {
				nlohmann::json json_res;

				// Try parsing token as JWT Access Token
				try {
					auto token{jwt::decode<jwt::traits::nlohmann_json>(bearer_token)};
					auto possible_json_res{validate_using_local_validation(token)};

					if (possible_json_res) {
						json_res = *possible_json_res;
					}
				}
				// Parsing of the token failed, this is not a JWT access token
				catch (const std::exception& e) {
					logging::debug("{}: {}", __func__, e.what());
				}

				// Use introspection endpoint if it exists and local validation fails
				static const auto introspection_endpoint_exists{
					irods::http::globals::oidc_endpoint_configuration().contains("introspection_endpoint")};
				if (json_res.empty() && introspection_endpoint_exists) {
					auto possible_json_res{validate_using_introspection_endpoint(bearer_token)};
					if (possible_json_res) {
						json_res = *possible_json_res;
					}
				}

				if (json_res.empty()) {
					logging::error("{}: Could not find bearer token matching [{}].", __func__, bearer_token);
					return {.response = fail(status_type::unauthorized)};
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
