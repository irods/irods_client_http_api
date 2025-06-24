#ifndef IRODS_HTTP_API_OPENID_HPP
#define IRODS_HTTP_API_OPENID_HPP

#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/nlohmann-json/traits.h>
#include <nlohmann/json.hpp>

#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/url/url_view.hpp>

#include <optional>
#include <string>

namespace irods::http::openid
{
	auto create_oidc_request(boost::urls::url_view _url)
		-> boost::beast::http::request<boost::beast::http::string_body>;

	/// Validates an OAuth 2.0 Access Token using the Introspection Endpoint.
	/// See RFC 7662 on OAuth 2.0 Token Introspection for more details.
	///
	/// \param[in] _bearer_token A std::string representing an Access Token to validate.
	///
	/// \returns An optional containing a nlohmann::json object representing the response of the introspection endpoint
	///          if verification was successful. Otherwise,an empty optional is returned.
	auto validate_using_introspection_endpoint(const std::string& _bearer_token) -> std::optional<nlohmann::json>;

	/// Validates an OAuth 2.0 Access Token using provided JWKs and secrets in the HTTP API configuration.
	///
	/// See RFC 9068 for more details on JWT for OAuth 2.0 (OJWT).
	/// See RFC 7516 for details on JSON Web Encryption (JWE).
	/// See RFC 7515 for details on JSON Web Signature (JWS).
	/// See RFC 7518 for details on JSON Web Algorithms (JWA).
	/// See OpenID Connect Core 1.0 for details on OpenID Connect (OIDC).
	///
	/// \param[in] _jwt A jwt::decoded_jwt<jwt::traits::nlohmann_json> representing the JWT to verify.
	///
	/// \returns The set of claims contained in the JWT if the token can be validated. Otherwise, an empty std::optional
	/// is returned
	auto validate_using_local_validation(const jwt::decoded_jwt<jwt::traits::nlohmann_json>& _jwt)
		-> std::optional<nlohmann::json>;
} //namespace irods::http::openid

#endif // IRODS_HTTP_API_OPENID_HPP
