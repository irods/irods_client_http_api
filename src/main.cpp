#include "common.hpp"
#include "globals.hpp"
#include "handlers.hpp"
#include "log.hpp"
#include "session.hpp"
#include "version.hpp"

#include <boost/beast/core/flat_buffer.hpp>
#include <irods/connection_pool.hpp>
#include <irods/rcConnect.h>
#include <irods/rcMisc.h>
#include <irods/rodsClient.h>

//#include <curl/curl.h>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
//#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/config.hpp>
//#include <boost/algorithm/string.hpp>
#include <boost/program_options.hpp>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// clang-format off
namespace beast = boost::beast; // from <boost/beast.hpp>
//namespace http  = beast::http;  // from <boost/beast/http.hpp>
namespace net   = boost::asio;  // from <boost/asio.hpp>
namespace po    = boost::program_options;
namespace log   = irods::http::log;

using json = nlohmann::json;
using tcp  = boost::asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>

// IRODS_HTTP_API_BASE_URL is a macro defined by the CMakeLists.txt.
const irods::http::request_handler_map_type req_handlers{
    {IRODS_HTTP_API_BASE_URL "/authenticate", irods::http::handler::authentication},
    {IRODS_HTTP_API_BASE_URL "/collections",  irods::http::handler::collections},
//  {IRODS_HTTP_API_BASE_URL "/config",       irods::http::handler::configuration},
    {IRODS_HTTP_API_BASE_URL "/data-objects", irods::http::handler::data_objects},
    {IRODS_HTTP_API_BASE_URL "/info",         irods::http::handler::information},
    {IRODS_HTTP_API_BASE_URL "/metadata",     irods::http::handler::metadata},
    {IRODS_HTTP_API_BASE_URL "/query",        irods::http::handler::query},
    {IRODS_HTTP_API_BASE_URL "/resources",    irods::http::handler::resources},
    {IRODS_HTTP_API_BASE_URL "/rules",        irods::http::handler::rules},
    {IRODS_HTTP_API_BASE_URL "/tickets",      irods::http::handler::tickets},
    {IRODS_HTTP_API_BASE_URL "/users-groups", irods::http::handler::users_groups},
    {IRODS_HTTP_API_BASE_URL "/zones",        irods::http::handler::zones}
};
// clang-format on

// Accepts incoming connections and launches the sessions.
class listener : public std::enable_shared_from_this<listener>
{
  public:
    listener(net::io_context& ioc, const tcp::endpoint& endpoint, const json& _config)
        : ioc_{ioc}
        , acceptor_{net::make_strand(ioc)}
        , max_rbuffer_size_{_config.at(json::json_pointer{"/http_server/requests/max_rbuffer_size_in_bytes"}).get<int>()}
        , timeout_in_secs_{_config.at(json::json_pointer{"/http_server/requests/timeout_in_seconds"}).get<int>()}
    {
        beast::error_code ec;

        // Open the acceptor
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            irods::fail(ec, "open");
            return;
        }

        // Allow address reuse
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) {
            irods::fail(ec, "set_option");
            return;
        }

        // Bind to the server address
        acceptor_.bind(endpoint, ec);
        if (ec) {
            irods::fail(ec, "bind");
            return;
        }

        // Start listening for connections
        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            irods::fail(ec, "listen");
            return;
        }
    } // listener (constructor)

    // Start accepting incoming connections.
    auto run() -> void
    {
        do_accept();
    } // run

  private:
    auto do_accept() -> void
    {
        // The new connection gets its own strand
        acceptor_.async_accept(
            net::make_strand(ioc_),
            beast::bind_front_handler(
                &listener::on_accept,
                shared_from_this()));
    } // do_accept

    auto on_accept(beast::error_code ec, tcp::socket socket) -> void
    {
        if (ec) {
            irods::fail(ec, "accept");
            //return; // To avoid infinite loop
        }
        else {
            // Create the session and run it
            std::make_shared<irods::http::session>(
                std::move(socket), req_handlers, max_rbuffer_size_, timeout_in_secs_)->run();
        }

        // Accept another connection
        do_accept();
    } // on_accept

    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    const int max_rbuffer_size_;
    const int timeout_in_secs_;
}; // class listener

auto print_version_info() -> void
{
    namespace version = irods::http::version;
    const std::string_view sha = version::sha;
    constexpr auto sha_size = 7;
    fmt::print("{} v{}-{}\n", version::binary_name, version::api_version, sha.substr(0, sha_size));
} // print_version_info

auto print_usage() -> void
{
    fmt::print(R"_(irods_http_api - Exposes the iRODS API over HTTP

Usage: irods_http_api [OPTION]... CONFIG_FILE_PATH

CONFIG_FILE_PATH must point to a file containing a JSON structure containing
configuration options.

Options:
  -h, --help     Display this help message and exit.
  -v, --version  Display version information and exit.

)_");

    print_version_info();
} // print_usage

auto set_log_level(const json& _config) -> void
{
    const auto iter = _config.find("log_level");

    if (iter == std::end(_config)) {
        spdlog::set_level(spdlog::level::info);
    }

    const auto& lvl_string = iter->get_ref<const std::string&>();
    auto lvl_enum = spdlog::level::info;

    // clang-format off
    if      (lvl_string == "trace")    { lvl_enum = spdlog::level::trace; }
    else if (lvl_string == "info")     { lvl_enum = spdlog::level::info; }
    else if (lvl_string == "debug")    { lvl_enum = spdlog::level::debug; }
    else if (lvl_string == "warn")     { lvl_enum = spdlog::level::warn; }
    else if (lvl_string == "error")    { lvl_enum = spdlog::level::err; }
    else if (lvl_string == "critical") { lvl_enum = spdlog::level::critical; }
    else                               { log::warn("Invalid log_level. Setting to [info]."); }
    // clang-format on

    spdlog::set_level(lvl_enum);
} // set_log_level

auto init_irods_connection_pool(const json& _config) -> irods::connection_pool
{
    const auto& client = _config.at("irods_client");
    const auto& zone = client.at("zone").get_ref<const std::string&>();
    const auto& conn_pool = client.at("connection_pool");
    const auto& rodsadmin = client.at("rodsadmin");
    const auto& username = rodsadmin.at("username").get_ref<const std::string&>();

    return {
        conn_pool.at("size").get<int>(),
        client.at("host").get_ref<const std::string&>(),
        client.at("port").get<int>(),
        {{username, zone}},
        {username, zone},
        conn_pool.at("refresh_timeout_in_seconds").get<int>(),
        [pw = rodsadmin.at("password").get<std::string>()](RcComm& _comm) mutable {
            if (const auto ec = clientLoginWithPassword(&_comm, pw.data()); ec != 0) {
                throw std::invalid_argument{fmt::format("Could not authenticate rodsadmin user: [{}]", ec)};
            }
        }
    };
} // init_irods_connection_pool

auto main(int _argc, char* _argv[]) -> int
{
    po::options_description opts_desc{""};
    opts_desc.add_options()
        ("config-file,f", po::value<std::string>(), "")
        ("help,h", "")
        ("version,v", "");

    po::positional_options_description pod;
    pod.add("config-file", 1);

    set_ips_display_name("irods_http_api");

    try {
        po::variables_map vm;
        po::store(po::command_line_parser(_argc, _argv).options(opts_desc).positional(pod).run(), vm);
        po::notify(vm);

        if (vm.count("help") > 0) {
            print_usage();
            return 0;
        }

        if (vm.count("version") > 0) {
            print_version_info();
            return 0;
        }

        if (vm.count("config-file") == 0) {
            fmt::print(stderr, "Error: Missing [CONFIG_FILE_PATH] parameter.");
            return 1;
        }

        const auto config = json::parse(std::ifstream{vm["config-file"].as<std::string>()});
        irods::http::globals::config = &config;

        const auto& http_server_config = config.at("http_server");
        set_log_level(http_server_config);
        spdlog::set_pattern("[%Y-%m-%d %T.%e] [P:%P] [%^%l%$] [T:%t] %v");

        // Confirm OIDC endpoint is valid (Assume all provide endpoint)
        log::trace("Verifing OIDC endpoint configuration");

        nlohmann::json oi_config;
        nlohmann::json endpoint_config;

        // Scope as we're likely going to move this part out into a sep func...
        try {
            oi_config = http_server_config.at(json::json_pointer{"/authentication/oidc"});
            irods::http::globals::oidc_config = &oi_config;
            // Consider reusing context further down main?
            net::io_context io_ctx;
            net::ip::tcp::resolver tcp_res{io_ctx};
            beast::tcp_stream tcp_stream{io_ctx};

            // Load config
            const auto host{oi_config.at("config_host").get<const std::string>()};
            const auto port{oi_config.at("port").get<const std::string>()};
            const auto uri{oi_config.at("uri").get<const std::string>()};

            // Resolve addr
            const auto resolve{tcp_res.resolve(host, port)};

            // Connect and get config
            tcp_stream.connect(resolve);

            // Build Request
            constexpr auto version_number{11};
            beast::http::request<beast::http::string_body> req{beast::http::verb::get, uri, version_number};
            req.set(beast::http::field::host, host);
            req.set(beast::http::field::user_agent, BOOST_BEAST_VERSION_STRING);

            // Send request
            beast::http::write(tcp_stream, req);

            // Read back req
            beast::flat_buffer buffer;
            beast::http::response<beast::http::string_body> res;
            beast::http::read(tcp_stream, buffer, res);

            // TODO: Check resposnse code...
            log::debug("Got the following back: {}", res.body());
            
            // Convert http json response to nlomman json response
            endpoint_config = json::parse(res.body());
            irods::http::globals::oidc_endpoints = &endpoint_config;

            // Close teh socket
            beast::error_code ec;
            tcp_stream.socket().shutdown(net::ip::tcp::socket::shutdown_both, ec);
        } catch (const json::out_of_range& e) {
            log::trace("Invalid OIDC configuration, ignoring. Reason: {}", e.what());
        }
            

        // TODO For LONG running tasks, see the following:
        //
        //   - https://stackoverflow.com/questions/17648725/long-running-blocking-operations-in-boost-asio-handlers
        //   - https://www.open-std.org/JTC1/SC22/WG21/docs/papers/2012/n3388.pdf
        //

        log::trace("Loading API plugins.");
        load_client_api_plugins();

        const auto address = net::ip::make_address(http_server_config.at("host").get_ref<const std::string&>());
        const auto port = http_server_config.at("port").get<std::uint16_t>();
        const auto request_thread_count = std::max(http_server_config.at(json::json_pointer{"/requests/threads"}).get<int>(), 1);
        
        log::trace("Initializing iRODS connection pool.");
        auto conn_pool = init_irods_connection_pool(config);
        irods::http::globals::conn_pool = &conn_pool;

        // The io_context is required for all I/O.
        log::trace("Initializing HTTP components.");
        net::io_context ioc{request_thread_count};
        irods::http::globals::req_handler_ioc = &ioc;

        // Create and launch a listening port.
        log::trace("Initializing listening socket (host=[{}], port=[{}]).", address.to_string(), port);
        std::make_shared<listener>(ioc, tcp::endpoint{address, port}, config)->run();

        // Capture SIGINT and SIGTERM to perform a clean shutdown.
        log::trace("Initializing signal handlers.");
        net::signal_set signals{ioc, SIGINT, SIGTERM};
        signals.async_wait([&ioc](const beast::error_code&, int _signal) {
            // Stop the io_context. This will cause run() to return immediately, eventually destroying the
            // io_context and all of the sockets in it.
            log::warn("Received signal [{}]. Shutting down.", _signal);
            ioc.stop();
        });

        // Launch the requested number of dedicated backgroup I/O threads.
        // These threads are used for long running tasks (e.g. reading/writing bytes, database, etc.)
        log::trace("Initializing thread pool for long running I/O tasks.");
        net::thread_pool io_threads(std::max(http_server_config.at(json::json_pointer{"/background_io/threads"}).get<int>(), 1));
        irods::http::globals::thread_pool_bg = &io_threads;

        // Run the I/O service on the requested number of threads.
        log::trace("Initializing thread pool for HTTP requests.");
        net::thread_pool request_handler_threads(request_thread_count);
        for (auto i = request_thread_count - 1; i > 0; --i) {
            net::post(request_handler_threads, [&ioc] { ioc.run(); });
        }
        log::info("Server is ready.");
        ioc.run();

        request_handler_threads.stop();
        io_threads.stop();

        log::trace("Waiting for HTTP requests thread pool to shut down.");
        request_handler_threads.join();

        log::trace("Waiting for I/O thread pool to shut down.");
        io_threads.join();

        log::info("Shutdown complete.");

        return 0;
    }
    catch (const std::exception& e) {
        fmt::print(stderr, "Error: {}\n", e.what());
        return 1;
    }
} // main
