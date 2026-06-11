#include "irods/private/http_api/globals.hpp"

#include <boost/asio.hpp>

namespace
{
	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
	const nlohmann::json* g_config{};

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
	boost::asio::io_context* g_req_handler_ioc{};

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
	boost::asio::thread_pool* g_bg_thread_pool{};

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
	irods::connection_pool* g_conn_pool{};

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
	const nlohmann::json* g_oidc_config{};

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
	const nlohmann::json* g_oidc_endpoints{};

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
	boost::dll::shared_library g_user_map_lib;

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
	std::string g_irods_server_version;
} // anonymous namespace

namespace irods::http::globals
{
	auto set_configuration(const nlohmann::json& _config) -> void
	{
		g_config = &_config;
	} // set_configuration

	auto configuration() -> const nlohmann::json&
	{
		return *g_config;
	} // configuration

	auto set_request_handler_io_context(boost::asio::io_context& _ioc) -> void
	{
		g_req_handler_ioc = &_ioc;
	} // set_request_handler_io_context

	auto request_handler_io_context() -> boost::asio::io_context&
	{
		return *g_req_handler_ioc;
	} // request_handler_io_context

	auto set_background_thread_pool(boost::asio::thread_pool& _tp) -> void
	{
		g_bg_thread_pool = &_tp;
	} // set_background_thread_pool

	auto background_thread_pool() -> boost::asio::thread_pool&
	{
		return *g_bg_thread_pool;
	} // background_thread_pool

	auto background_task(std::function<void()> _task) -> void
	{
		boost::asio::post(background_thread_pool(), [t = std::move(_task)] {
			try {
				t();
			}
			catch (...) {
			}
		});
	} // background_task

	auto set_connection_pool(irods::connection_pool& _cp) -> void
	{
		g_conn_pool = &_cp;
	} // set_connection_pool

	auto connection_pool() -> irods::connection_pool&
	{
		return *g_conn_pool;
	} // connection_pool

	auto set_oidc_endpoint_configuration(const nlohmann::json& _config) -> void
	{
		g_oidc_endpoints = &_config;
	} // set_oidc_endpoint_configuration

	auto oidc_endpoint_configuration() -> const nlohmann::json&
	{
		return *g_oidc_endpoints;
	} // oidc_endpoint_configuration

	auto set_oidc_configuration(const nlohmann::json& _config) -> void
	{
		g_oidc_config = &_config;
	} // set_oidc_configuration

	auto oidc_configuration() -> const nlohmann::json&
	{
		return *g_oidc_config;
	} // oidc_configuration

	auto set_user_mapping_lib(boost::dll::shared_library _lib) -> void
	{
		g_user_map_lib = std::move(_lib);
	} // set_user_mapping_lib

	auto user_mapping_lib() -> boost::dll::shared_library&
	{
		return g_user_map_lib;
	} // user_mapping_lib

	auto set_irods_server_version(std::string _version) -> void
	{
		g_irods_server_version = std::move(_version);
	} // set_irods_server_version

	auto get_irods_server_version() -> const std::string&
	{
		return g_irods_server_version;
	} // get_irods_server_version
} // namespace irods::http::globals
