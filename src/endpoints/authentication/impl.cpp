#include "handlers.hpp"

#include "common.hpp"
#include "globals.hpp"
#include "log.hpp"
#include "session.hpp"

#include <curl/urlapi.h>
#include <irods/base64.hpp>
#include <irods/client_connection.hpp>
#include <irods/irods_exception.hpp>
#include <irods/process_stash.hpp>
#include <irods/rcConnect.h>

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include <nlohmann/json.hpp>

#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/nlohmann-json/traits.h>

#include <curl/curl.h>

#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

// clang-format off
namespace beast = boost::beast; // from <boost/beast.hpp>
namespace net   = boost::asio;  // from <boost/asio.hpp>
// clang-format on

namespace irods::http::handler
{
    auto authentication(session_pointer_type _sess_ptr, request_type& _req) -> void
    {
        if (_req.method() != boost::beast::http::verb::post) {
            return _sess_ptr->send(fail(status_type::method_not_allowed));
        }

        auto& thread_pool = *irods::http::globals::thread_pool_bg;

        boost::asio::post(thread_pool, [fn = __func__, _sess_ptr, _req = std::move(_req)] {
            // Right, we're kinda being a proxy, so how about proxy-authorization?
            const auto& hdrs = _req.base();
            const auto iter = hdrs.find("authorization");
            
            if (iter == std::end(hdrs)) {
                return _sess_ptr->send(fail(status_type::bad_request));
            }

            log::debug("{}: Authorization value: [{}]", fn, iter->value());

            //
            // TODO Here is where we determine what form of authentication to perform (e.g. Basic or OIDC).

            //
            // BLAH BLAH BLAH, assume we have the user & pass...
            // Prob via Proxy-Authorization????
            //

            const auto pos = iter->value().find("Basic ");
            if (std::string_view::npos == pos) {

                // TEMPORARY PLS MAKE BETTER LATER :)
                
                const auto alt_method{iter->value().find("iRODS ")};
                if (std::string_view::npos == alt_method) {
                    return _sess_ptr->send(fail(status_type::bad_request));
                }


                // BEGIN BASE64 HEADER DECODE
                
                constexpr auto basic_auth_scheme_prefix_size = 6;
                std::string authorization{iter->value().substr(alt_method + basic_auth_scheme_prefix_size)};
                boost::trim(authorization);
                log::debug("{}: Authorization value (trimmed): [{}]", fn, authorization);

                constexpr auto max_creds_size = 128;
                unsigned long size = max_creds_size;
                //std::vector<std::uint8_t> creds(size);
                std::array<std::uint8_t, max_creds_size> creds{};
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                const auto ec = irods::base64_decode(reinterpret_cast<unsigned char*>(authorization.data()), authorization.size(), creds.data(), &size);
                log::debug("{}: base64 - error code=[{}], decoded size=[{}]", fn, ec, size);

                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                std::string_view sv{reinterpret_cast<char*>(creds.data()), size}; 
                log::debug("{}: base64 decode credentials = [{}]", fn, sv); // TODO Don't print the password

                const auto colon = sv.find(':');
                if (colon == std::string_view::npos) {
                    return _sess_ptr->send(fail(status_type::unauthorized));
                }

                std::string username{sv.substr(0, colon)};
                std::string password{sv.substr(colon + 1)};
                log::debug("{}: username=[{}], password=[{}]", fn, username, password); // TODO Don't print the password


                // BEGIN OG OAUTH THING

                // Hit token end-point
                const auto token_endpoint{irods::http::globals::oidc_endpoints->at("token_endpoint").get<const std::string>()};
                log::debug("Got an endpoint of [{}].", token_endpoint);

                // Network context
                {
                    // Setup net
                    net::io_context io_ctx;
                    net::ip::tcp::resolver tcp_res{io_ctx};
                    beast::tcp_stream tcp_stream{io_ctx};
                    
                    //
                    // TODO: We shouldn't process a known url more than once...
                    //
                    
                    // Setup curl
                    CURLU *endpoint{curl_url()};
                    
                    // Parse url
                    CURLUcode rc{curl_url_set(endpoint, CURLUPART_URL, token_endpoint.data(), 0)};
                    if (rc != 0) {
                        log::debug("Something happend....");
                    }
                    
                    // Get host
                    char *host{};
                    rc = curl_url_get(endpoint, CURLUPART_HOST, &host, 0);
                    if (rc != 0) {
                        log::debug("Something happend....");
                    }
                    
                    // Get service/port
                    //char *port{};
                    //rc = curl_url_get(endpoint, CURLUPART_PORT, &port, 0);
                    //if (rc != 0) {
                    //    log::debug("Something happend....");
                    //}
                    // KEYCLOAK does not return the port?
                    const auto port{irods::http::globals::oidc_config->at("port").get<const std::string>()};
                    
                    // Get path
                    char *path{};
                    rc = curl_url_get(endpoint, CURLUPART_PATH, &path, 0);
                    if (rc != 0) {
                        log::debug("Something happend....");
                    }
                    
                    // Addr
                    const auto resolve{tcp_res.resolve(host, port)};
                    
                    // TCP thing
                    tcp_stream.connect(resolve);
                    
                    // Build Request
                    constexpr auto version_number{11};
                    beast::http::request<beast::http::string_body> req{beast::http::verb::post, path, version_number};
                    req.set(beast::http::field::host, host);
                    req.set(beast::http::field::user_agent, BOOST_BEAST_VERSION_STRING);
                    req.set(beast::http::field::content_type, "application/x-www-form-urlencoded"); // Possibly set a diff way?
                    
                    // Build body
                    std::stringstream string_builder;
                    
                    string_builder << "client_id=" << irods::http::globals::oidc_config->at("client_id").get<const std::string>() << '&'
                                   << "grant_type=password&"
                                   << "scope=openid&"
                                   << "username="  << username << '&'
                                   << "password=" << password;
                    
                    // Send
                    auto send_to{string_builder.str()};
                    log::debug("Built the following body: {}", send_to);
                    req.body() = send_to;
                    req.prepare_payload();
                    
                    // Send request
                    beast::http::write(tcp_stream, req);
                    
                    // Read back req
                    beast::flat_buffer buffer;
                    beast::http::response<beast::http::string_body> res;
                    beast::http::read(tcp_stream, buffer, res);
                    
                    log::debug("Got the following resp back: {}", res.body());
                    
                    // JSONize response
                    nlohmann::json res_item{nlohmann::json::parse(res.body())};
                    
                    // Assume passed, get oidc token
                    const std::string jwt_token{res_item.at("id_token").get<const std::string>()};
                    
                    // Feed to JWT parser
                    auto decoded_token{jwt::decode<jwt::traits::nlohmann_json>(jwt_token)};
                    
                    // Get irods username
                    // Zones?
                    // uname#zname
                    const std::string irods_name{decoded_token.get_payload_json().at("irods_username").get<const std::string>()};
                    
                    // Issue token?
                    static const auto seconds = irods::http::globals::config->at(nlohmann::json::json_pointer{"/http_server/authentication/basic/timeout_in_seconds"}).get<int>();
                    auto bearer_token = irods::process_stash::insert(authenticated_client_info{
                            .auth_scheme = authorization_scheme::basic,
                            .username = std::move(irods_name),
                            .expires_at = std::chrono::steady_clock::now() + std::chrono::seconds{seconds}
                        });
                

                    // Close socket
                    beast::error_code ec;
                    tcp_stream.socket().shutdown(net::ip::tcp::socket::shutdown_both, ec);
                    
                    // Free up all items created, reverse n all
                    curl_free(path);
                    //curl_free(port);
                    curl_free(host);
                    
                    // Done
                    curl_url_cleanup(endpoint);
                    
                    response_type res_rep{status_type::ok, _req.version()};
                    res_rep.set(field_type::server, BOOST_BEAST_VERSION_STRING);
                    res_rep.set(field_type::content_type, "text/plain");
                    res_rep.keep_alive(_req.keep_alive());
                    res_rep.body() = std::move(bearer_token);
                    res_rep.prepare_payload();
                    
                    return _sess_ptr->send(std::move(res_rep));
                } 
            }

            constexpr auto basic_auth_scheme_prefix_size = 6;
            std::string authorization{iter->value().substr(pos + basic_auth_scheme_prefix_size)};
            boost::trim(authorization);
            log::debug("{}: Authorization value (trimmed): [{}]", fn, authorization);

            constexpr auto max_creds_size = 128;
            unsigned long size = max_creds_size;
            //std::vector<std::uint8_t> creds(size);
            std::array<std::uint8_t, max_creds_size> creds{};
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            const auto ec = irods::base64_decode(reinterpret_cast<unsigned char*>(authorization.data()), authorization.size(), creds.data(), &size);
            log::debug("{}: base64 - error code=[{}], decoded size=[{}]", fn, ec, size);

            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            std::string_view sv{reinterpret_cast<char*>(creds.data()), size}; 
            log::debug("{}: base64 decode credentials = [{}]", fn, sv); // TODO Don't print the password

            const auto colon = sv.find(':');
            if (colon == std::string_view::npos) {
                return _sess_ptr->send(fail(status_type::unauthorized));
            }

            std::string username{sv.substr(0, colon)};
            std::string password{sv.substr(colon + 1)};
            log::debug("{}: username=[{}], password=[{}]", fn, username, password); // TODO Don't print the password

            bool login_successful = false;

            try {
                const auto& svr = irods::http::globals::config->at("irods_client");
                const auto& host = svr.at("host").get_ref<const std::string&>();
                const auto port = svr.at("port").get<std::uint16_t>();
                const auto& zone = svr.at("zone").get_ref<const std::string&>();

                irods::experimental::client_connection conn{
                    irods::experimental::defer_authentication, host, port, {username, zone}};

                login_successful = (clientLoginWithPassword(static_cast<RcComm*>(conn), password.data()) == 0);
            }
            catch (const irods::exception& e) {
                log::error(e.client_display_what());
            }

            if (!login_successful) {
                return _sess_ptr->send(fail(status_type::unauthorized));
            }

            static const auto seconds = irods::http::globals::config->at(nlohmann::json::json_pointer{"/http_server/authentication/basic/timeout_in_seconds"}).get<int>();
            auto bearer_token = irods::process_stash::insert(authenticated_client_info{
                .auth_scheme = authorization_scheme::basic,
                .username = std::move(username),
                .password = std::move(password),
                .expires_at = std::chrono::steady_clock::now() + std::chrono::seconds{seconds}
            });

            response_type res{status_type::ok, _req.version()};
            res.set(field_type::server, BOOST_BEAST_VERSION_STRING);
            res.set(field_type::content_type, "text/plain");
            res.keep_alive(_req.keep_alive());
            res.body() = std::move(bearer_token);
            res.prepare_payload();

            return _sess_ptr->send(std::move(res));
        });
    } // authentication
} // namespace irods::http::endpoint
