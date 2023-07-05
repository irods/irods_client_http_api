#ifndef IRODS_HTTP_API_GLOBALS_HPP
#define IRODS_HTTP_API_GLOBALS_HPP

#include <irods/connection_pool.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>
#include <nlohmann/json.hpp>

namespace irods::http::globals
{
    inline const nlohmann::json* config = nullptr;

    inline boost::asio::io_context* req_handler_ioc = nullptr;

    inline boost::asio::thread_pool* thread_pool_bg = nullptr;

    inline irods::connection_pool* conn_pool = nullptr;

    inline const nlohmann::json* oidc_config = nullptr;

    inline const nlohmann::json* oidc_endpoints = nullptr;
} // namespace irods::http::globals

#endif // IRODS_HTTP_API_GLOBALS_HPP
