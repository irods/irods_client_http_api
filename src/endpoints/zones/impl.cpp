#include "handlers.hpp"

#include "common.hpp"
#include "globals.hpp"
#include "log.hpp"
#include "session.hpp"

#include <irods/irods_at_scope_exit.hpp>
#include <irods/irods_exception.hpp>
#include <irods/rcMisc.h>
#include <irods/zone_report.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <string>

namespace irods::http::handler
{
    auto zones(session_pointer_type _sess_ptr, request_type& _req) -> void
    {
        namespace log = irods::http::log;

        if (_req.method() != boost::beast::http::verb::get) {
            log::error("{}: Incorrect HTTP method.", __func__);
            return _sess_ptr->send(fail(status_type::method_not_allowed));
        }

        auto result = irods::http::resolve_client_identity(_req);
        if (result.response) {
            return _sess_ptr->send(std::move(*result.response));
        }

        const auto* client_info = result.client_info;
        auto& thread_pool = *irods::http::globals::thread_pool_bg;

        boost::asio::post(thread_pool, [fn = __func__, client_info, _sess_ptr, _req = std::move(_req)] {
            log::info("{}: client_info = ({}, {})", fn, client_info->username, client_info->password);

            const auto url = irods::http::parse_url(_req);

            const auto op_iter = url.query.find("op");
            if (op_iter == std::end(url.query)) {
                log::error("{}: Missing [op] parameter.", fn);
                return _sess_ptr->send(irods::http::fail(status_type::bad_request));
            }

            if (op_iter->second != "report") {
                log::error("{}: Invalid [op] value.", fn);
                return _sess_ptr->send(irods::http::fail(status_type::bad_request));
            }

            BytesBuf* bbuf{};
            irods::at_scope_exit free_bbuf{[&bbuf] { freeBBuf(bbuf); }};

            {
                auto conn = irods::get_connection(client_info->username);

                if (const auto ec = rcZoneReport(static_cast<RcComm*>(conn), &bbuf); ec != 0) {
                    log::error("{}: rcZoneReport error: [{}]", fn, ec);
                    return _sess_ptr->send(irods::http::fail(status_type::bad_request));
                }
            }

            response_type res{status_type::ok, _req.version()};
            res.set(field_type::server, BOOST_BEAST_VERSION_STRING);
            res.set(field_type::content_type, "application/json");
            res.keep_alive(_req.keep_alive());
            res.body() = std::string_view(static_cast<char*>(bbuf->buf), bbuf->len);
            res.prepare_payload();

            return _sess_ptr->send(std::move(res));
        });
    } // zones
} // namespace irods::http::endpoint
