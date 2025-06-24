#include "irods/private/http_api/openid.hpp"

#include "irods/private/http_api/globals.hpp"
#include "irods/private/http_api/common.hpp"
#include "irods/private/http_api/log.hpp"
#include "irods/private/http_api/transport.hpp"
#include "irods/private/http_api/version.hpp"

#include <boost/algorithm/string.hpp>

// clang-format off
namespace beast = boost::beast; // from <boost/beast.hpp>
namespace net   = boost::asio;  // from <boost/asio.hpp>
// clang-format on

namespace irods::http::openid
{
	using jwt_verifier = jwt::verifier<jwt::default_clock, jwt::traits::nlohmann_json>;

	// NOLINTNEXTLINE(performance-unnecessary-value-param)
	auto create_oidc_request(boost::urls::url_view _url) -> beast::http::request<beast::http::string_body>
	{
		constexpr auto http_version_number{11};
		beast::http::request<beast::http::string_body> req{beast::http::verb::post, _url.path(), http_version_number};

		const auto port{irods::http::get_port_from_url(_url)};

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
	} // create_oidc_request

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
	} // hit_introspection_endpoint

	auto validate_using_introspection_endpoint(const std::string& _bearer_token) -> std::optional<nlohmann::json>
	{
		namespace logging = irods::http::log;

		const body_arguments args{{"token", _bearer_token}, {"token_type_hint", "access_token"}};

		auto json_res{hit_introspection_endpoint(url_encode_body(args))};

		// Check that the access token is active. If active is false or missing from
		// the response, reject the token.
		// The exact meaning of an active state may vary between OpenID Providers,
		// so we should assert the other attributes when available.
		if (const auto active_iter{json_res.find("active")};
		    active_iter == std::end(json_res) || !active_iter->get<bool>()) {
			logging::warn("{}: Access token is invalid or expired.", __func__);
			return std::nullopt;
		}

		// The rest of the members are optional, but validate the ones that do exist.
		// In total, there are 11 optional members defined by the RFC that may be used.

		// Token must not be used early if given 'not before' time.
		if (auto nbf_iter{json_res.find("nbf")}; nbf_iter != std::end(json_res)) {
			logging::trace("{}: Attempting [nbf] validation.", __func__);

			// Get current time
			const auto now{std::chrono::system_clock::now()};

			// Convert to appropriate representation
			const std::chrono::seconds nbf_seconds{nbf_iter->get<std::time_t>()};

			// Setup expire time
			const std::chrono::time_point<std::chrono::system_clock> not_before_time{nbf_seconds};

			// Reject tokens that are used too early
			if (now < not_before_time) {
				logging::warn("{}: Current time is before [nbf]. Validation failed.", __func__);
				return std::nullopt;
			}
		}

		// We should be part of the intended audience for the access token.
		if (const auto aud_iter{json_res.find("aud")}; aud_iter != std::end(json_res)) {
			logging::trace("{}: Attempting [aud] validation.", __func__);

			const nlohmann::json& client_id{irods::http::globals::oidc_configuration().at("client_id")};
			const nlohmann::json& aud{*aud_iter};

			if (aud.is_array()) {
				auto aud_list_iter{aud.items()};
				auto client_id_in_list{std::any_of(
					std::begin(aud_list_iter), std::end(aud_list_iter), [&client_id](const auto& _kvp) -> bool {
						return _kvp.value() == client_id;
					})};

				// If we are not in the list, reject the token
				if (!client_id_in_list) {
					logging::warn("{}: Could not find our [client_id] in [aud]. Validation failed.", __func__);
					return std::nullopt;
				}
			}
			else if (aud.get_ref<const std::string&>() != client_id.get_ref<const std::string&>()) {
				logging::warn("{}: Could not find our [client_id] in [aud]. Validation failed.", __func__);
				return std::nullopt;
			}
		}
		// Some IAM servers (e.g. keycloak) could be set up
		// to exclude 'aud' from a bearer token payload.
		// If no 'aud' was found in bearer token, do not accept.
		else if (auto strict_aud{
					 irods::http::globals::oidc_configuration().find("require_aud_member_from_introspection_endpoint")};
		         strict_aud != std::end(irods::http::globals::oidc_configuration()) && strict_aud->get<bool>())
		{
			logging::warn("{}: Bearer token payload is missing [aud]. Validation failed.", __func__);
			return std::nullopt;
		}

		// The 'iss' provided should match the 'issuer' retrieved from the OpenID Provider's
		// well-known endpoint.
		if (const auto iss_iter{json_res.find("iss")}; iss_iter != std::end(json_res)) {
			logging::trace("{}: Attempting [iss] validation.", __func__);
			if (iss_iter->get_ref<const std::string&>() !=
			    irods::http::globals::oidc_endpoint_configuration().at("issuer").get_ref<const std::string&>()) {
				logging::warn("{}: [iss] did not match OpenID Provider's [issuer]. Validation failed.", __func__);
				return std::nullopt;
			}
		}

		return json_res;
	} // validate_using_introspection_endpoint

	/// Fetches JWKs from the location specified by the OpenID Provider.
	///
	/// See OpenID Connect Discovery 1.0 Section 3 for info on jwks_uri.
	/// See RFC 7517 for more information on JSON Web Key (JWK).
	///
	/// \returns A std::string representing the JWKs from the OpenID Provider.
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

		// Send request and receive response
		auto res{tcp_stream->communicate(req)};

		logging::debug("{}: Received the following response: [{}]", __func__, res.body());

		return res.body();
	} // fetch_jwks_from_openid_provider

	/// Adds the specified symmetric algorithm \p _alg to the verifier \p _verifier, using the secrets provided in the
	/// HTTP API configuration.
	///
	/// See RFC 7518 for details on JSON Web Algorithms (JWA).
	///
	/// \param[in,out] _verifier The jwt_verifier to add additional verification algorithms to.
	/// \param[in]     _alg      The signing algorithm requested by the signed JWT.
	auto add_symmetric_algorithm(jwt_verifier& _verifier, std::string_view _alg) -> void
	{
		namespace logging = irods::http::log;

		auto algorithm_family{_alg.substr(0, 2)};
		auto algorithm_specifics{_alg.substr(2)};

		// The secret used to sign the JWT.
		// Depending on the server configuration, and the token type,
		// the key will be set appropriately. If no secret is
		// available, symmetric validation cannot be performed.
		std::string key;

		// Use a access_token_secret if provided. It should be base64url encoded, as the key might
		// not be ASCII printable.
		if (auto access_token_secret{irods::http::globals::oidc_configuration().find("access_token_secret")};
		    access_token_secret != std::end(irods::http::globals::oidc_configuration()))
		{
			key = jwt::base::decode<jwt::alphabet::base64url>(
				jwt::base::pad<jwt::alphabet::base64url>(access_token_secret->get_ref<const std::string&>()));
		}
		// While not for access tokens, ID Tokens are signed using the client_secret.
		// Some OpenID providers may do the same for access tokens.
		else if (auto secret{irods::http::globals::oidc_configuration().find("client_secret")};
		         secret != std::end(irods::http::globals::oidc_configuration()))
		{
			key = secret->get<std::string>();
		}
		else {
			logging::warn("{}: No secret provided. Unable to use symmetric algorithms.", __func__);
			return;
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
			else {
				logging::warn("{}: Algorithm [{}] is not supported.", __func__, _alg);
			}
			return;
		}

		logging::warn("{}: Algorithm [{}] is not supported.", __func__, _alg);
	} // add_symmetric_algorithm

	/// Adds the specified asymmetric algorithm \p _alg to the verifier \p _verifier, using the additional information
	/// provided by the JWK \p _jwk.
	///
	/// See RFC 7518 for details on JSON Web Algorithms (JWA).
	///
	/// \param[in,out] _verifier The jwt_verifier to add additional verification algorithms to.
	/// \param[in]     _jwk      The JWK containing the required JWA information.
	/// \param[in]     _alg      The signing algorithm requested by the signed JWT.
	auto add_asymmetric_algorithm_from_jwk(
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
				else {
					logging::warn("{}: Algorithm [{}] is not supported.", __func__, _alg);
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
				else {
					logging::warn("{}: Algorithm [{}] is not supported.", __func__, _alg);
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
			else {
				logging::warn("{}: Algorithm [{}] is not supported.", __func__, _alg);
			}
			return;
		}

		logging::warn("{}: Algorithm [{}] is not supported.", __func__, _alg);
	} // add_asymmetric_algorithm_from_jwk

	/// Adds verification algorithm(s) to the \p _verifier based on either the given 'kid' or algorthims matching
	/// the family of the desired algorithm \p _alg.
	///
	/// See RFC 7518 for details on JSON Web Algorithms (JWA).
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
				add_asymmetric_algorithm_from_jwk(_verifier, jwk, alg);

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
			add_symmetric_algorithm(_verifier, alg);
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
						add_asymmetric_algorithm_from_jwk(_verifier, _jwk, alg);
						return;
					}

					logging::trace("{}: JWK [alg] does not match JWT [alg], ignoring.", fn);
					return;
				}

				// Fallback to required claim
				if (_jwk.has_key_type()) {
					// Extract the 'kty' of the JWK, compare to desired 'kty'
					if (_jwk.get_key_type() == search_string) {
						add_asymmetric_algorithm_from_jwk(_verifier, _jwk, alg);
						return;
					}

					logging::trace("{}: JWK [kty] does not match JWT desired [kty], ignoring.", fn);
					return;
				}

				logging::error("{}: Invalid JWK, missing [kty] claim. Ignoring.", fn);
			});

		// Allow for chaining
		return _verifier;
	} // add_algorithms_to_verifier

	auto validate_using_local_validation(const jwt::decoded_jwt<jwt::traits::nlohmann_json>& _jwt)
		-> std::optional<nlohmann::json>
	{
		namespace logging = irods::http::log;

		try {
			// Parse the JWKs discovered from the OpenID Provider
			static auto jwks{jwt::parse_jwks<jwt::traits::nlohmann_json>(fetch_jwks_from_openid_provider())};

			// Handling missing 'typ'
			if (!_jwt.has_type()) {
				logging::error("{}: invalid JWT, missing [typ].", __func__);
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
				logging::error("{}: Invalid JWT, missing [alg].", __func__);
				return std::nullopt;
			}

			// Use the 'alg' specified in the access token
			auto alg{_jwt.get_algorithm()};

			// Reject 'alg' type of 'none'
			// While ID Tokens may support 'none', specific conditions must be met,
			// and these conditions are not met in our case.
			// See OJWT Section 4 and OIDC Section 2
			if (alg == "none") {
				logging::error("{}: JWT with [alg] of type [none] is not supported.", __func__);
				return std::nullopt;
			}

			// Reject JWT with JWS 'crit', we do not support extensions using 'crit' at this moment
			// See JWS Section 4.1.11
			if (_jwt.has_header_claim("crit")) {
				logging::error(
					"{}: JWT with unsupported [crit] claim provided: [{}].",
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
				logging::error("{}: JWT verification failed [{}].", __func__, ec.message());
				return std::nullopt;
			}

			logging::trace("{}: JWT verification succeeded.", __func__);
			return _jwt.get_payload_json();
		}
		catch (const std::exception& e) {
			logging::error("{}: Unexpected exception [{}]", __func__, e.what());
			return std::nullopt;
		}
	} // validate_using_local_validation
} //namespace irods::http::openid
