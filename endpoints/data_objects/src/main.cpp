#include "irods/private/http_api/handlers.hpp"

#include "irods/private/http_api/common.hpp"
#include "irods/private/http_api/compatibility.hpp"
#include "irods/private/http_api/globals.hpp"
#include "irods/private/http_api/log.hpp"
#include "irods/private/http_api/session.hpp"
#include "irods/private/http_api/shared_api_operations.hpp"
#include "irods/private/http_api/version.hpp"

#include <irods/apiNumber.h>
#include <irods/client_connection.hpp>
#include <irods/connection_pool.hpp>
#include <irods/dataObjChksum.h>
#include <irods/dataObjCopy.h>
#include <irods/dataObjRepl.h>
#include <irods/dataObjTrim.h>
#include <irods/dataObjUnlink.h>
#include <irods/filesystem.hpp>
#include <irods/filesystem/path_utilities.hpp>
#include <irods/irods_at_scope_exit.hpp>
#include <irods/irods_exception.hpp>
#include <irods/irods_version.h>
#include <irods/key_value_proxy.hpp>
#include <irods/modDataObjMeta.h>
#include <irods/packStruct.h>
#include <irods/phyPathReg.h>
#include <irods/rcMisc.h>
#include <irods/replica_truncate.h>
#include <irods/rodsErrorTable.h>
#include <irods/rodsKeyWdDef.h>
#include <irods/rodsPackInstruct.h>
#include <irods/ticketAdmin.h>
#include <irods/touch.h>
#include <irods/version.hpp>

#ifdef IRODS_DEV_PACKAGE_IS_AT_LEAST_IRODS_5
#  include <irods/authenticate.h>
#  include <irods/irods_auth_constants.hpp> // For AUTH_PASSWORD_KEY.
#endif // IRODS_DEV_PACKAGE_IS_AT_LEAST_IRODS_5

#include <irods/transport/default_transport.hpp>
#include <irods/dstream.hpp>

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <mutex>
#include <span>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// clang-format off
namespace beast = boost::beast;     // from <boost/beast.hpp>
namespace http  = beast::http;      // from <boost/beast/http.hpp>
namespace net   = boost::asio;      // from <boost/asio.hpp>

namespace fs      = irods::experimental::filesystem;
namespace io      = irods::experimental::io;
namespace logging = irods::http::log;

using json = nlohmann::json;
// clang-format on

#define IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(name) \
  auto name(                                              \
	  irods::http::session_pointer_type _sess_ptr,        \
	  irods::http::request_type& _req,                    \
	  irods::http::query_arguments_type& _args)           \
	  ->void

namespace
{
	class parallel_write_stream
	{
	  public:
		// TODO May need to accept a zone for the client (i.e. federation).
		parallel_write_stream(
			const std::string& _client_username,
			const std::string& _path,
			const std::optional<std::string>& _resource,
			const std::ios_base::openmode _openmode,
			const std::optional<std::string>& _ticket,
			const irods::experimental::io::odstream* _base = nullptr)
		{
			const auto& client = irods::http::globals::configuration().at("irods_client");
			const auto& zone = client.at("zone").get_ref<const std::string&>();
			const auto& rodsadmin = client.at("proxy_admin_account");

			conn_.connect(
				irods::experimental::defer_authentication,
				client.at("host").get_ref<const std::string&>(),
				client.at("port").get<int>(),
				{rodsadmin.at("username").get_ref<const std::string&>(), zone},
				{_client_username, zone});

			auto password = rodsadmin.at("password").get<std::string>();

#ifdef IRODS_DEV_PACKAGE_IS_AT_LEAST_IRODS_5
			// clang-format off
			const auto ec = rc_authenticate_client(
				static_cast<RcComm*>(conn_),
				nlohmann::json{
					{"scheme", "native"},
					{irods::AUTH_PASSWORD_KEY, password},
				}.dump().c_str());
			// clang-format on
#else
			const auto ec = clientLoginWithPassword(static_cast<RcComm*>(conn_), password.data());
#endif // IRODS_DEV_PACKAGE_IS_AT_LEAST_IRODS_5
			if (ec != 0) {
				conn_.disconnect();
				THROW(SYS_INTERNAL_ERR, "Could not connect to iRODS server as proxied user.");
			}

			// Enable ticket if the request includes one.
			if (_ticket) {
				if (const auto ec = irods::enable_ticket(conn_, *_ticket); ec < 0) {
					THROW(ec, "Error enabling ticket on connection.");
				}
			}

			tp_ = std::make_unique<irods::experimental::io::client::native_transport>(conn_);

			if (_base) {
				stream_.open(*tp_, _base->replica_token(), _path, _base->replica_number(), _openmode);
			}
			else if (_resource) {
				stream_.open(*tp_, _path, irods::experimental::io::root_resource_name{*_resource}, _openmode);
			}
			else {
				stream_.open(*tp_, _path, _openmode);
			}

			if (!stream_) {
				THROW(INVALID_HANDLE, fmt::format("Could not open output stream to [{}].", _path));
			}
		} // parallel_write_stream (constructor)

		auto stream() noexcept -> irods::experimental::io::odstream&
		{
			return stream_;
		} // stream

		auto is_in_use() const noexcept -> bool
		{
			return in_use_.load();
		} // is_in_use

		auto in_use(bool _value) noexcept -> void
		{
			in_use_.store(_value);
		} // in_use

	  private:
		irods::experimental::client_connection conn_{irods::experimental::defer_connection};
		std::unique_ptr<irods::experimental::io::client::native_transport> tp_;
		irods::experimental::io::odstream stream_;
		std::atomic<bool> in_use_{false};
	}; // class parallel_write_stream

	struct parallel_write_context
	{
		std::vector<std::shared_ptr<parallel_write_stream>> streams;
		std::unique_ptr<std::mutex> mtx;

		auto find_available_parallel_write_stream() -> parallel_write_stream*
		{
			std::scoped_lock lk{*mtx};

			auto iter = std::find_if(
				std::begin(streams), std::end(streams), [](auto& _stream) { return !_stream->is_in_use(); });

			if (iter == std::end(streams)) {
				return nullptr;
			}

			(*iter)->in_use(true);

			return (*iter).get();
		} // find_available_parallel_write_stream
	}; // struct parallel_write_context

	std::shared_mutex pwc_mtx;
	std::unordered_map<std::string, parallel_write_context> parallel_write_contexts;

	//
	// Handler function prototypes
	//

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_read);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_write);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_parallel_write_init);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_parallel_write_shutdown);

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_replicate);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_trim);

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_set_permission);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_modify_permissions);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_stat);

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_register);

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_rename);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_copy);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_remove);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_touch);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_truncate);

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_calculate_checksum);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_verify_checksum);

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_modify_metadata);

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_modify_replica);

	//
	// Operation to Handler mappings
	//

	// clang-format off
	const std::unordered_map<std::string, irods::http::handler_type> handlers_for_get{
		{"read", op_read},
		{"stat", op_stat},
		{"verify_checksum", op_verify_checksum}
	};

	const std::unordered_map<std::string, irods::http::handler_type> handlers_for_post{
		{"touch", op_touch},
		{"truncate", op_truncate},
		{"remove", op_remove},

		{"write", op_write},
		{"parallel_write_init", op_parallel_write_init},
		{"parallel_write_shutdown", op_parallel_write_shutdown},

		{"rename", op_rename},
		{"copy", op_copy},

		{"replicate", op_replicate},
		{"trim", op_trim},

		{"register", op_register},

		{"set_permission", op_set_permission},
		{"modify_permissions", op_modify_permissions},

		{"calculate_checksum", op_calculate_checksum},

		{"modify_metadata", op_modify_metadata},

		{"modify_replica", op_modify_replica}
	};
	// clang-format on
} // anonymous namespace

namespace irods::http::handler
{
	// NOLINTNEXTLINE(performance-unnecessary-value-param)
	IRODS_HTTP_API_ENDPOINT_ENTRY_FUNCTION_SIGNATURE(data_objects)
	{
		execute_operation(_sess_ptr, _req, handlers_for_get, handlers_for_post);
	} // data_objects
} // namespace irods::http::handler

namespace
{
	//
	// Utility functions
	//

	class incremental_read : public std::enable_shared_from_this<incremental_read>
	{
	  public:
		incremental_read(
			irods::http::session_pointer_type& _sess_ptr,
			unsigned int _http_version,
			bool _http_keep_alive,
			irods::experimental::client_connection& _conn,
			std::unique_ptr<io::client::native_transport>& _tp,
			io::idstream& _in,
			std::int64_t _buffer_size,
			std::int64_t _remaining_bytes)
			: sess_ptr_{_sess_ptr->shared_from_this()}
			, res_{http::status::ok, _http_version}
			, serializer_{res_}
			, conn_{std::move(_conn)}
			, tp_{std::move(_tp)}
			, in_{std::move(_in)}
			, buffer_(_buffer_size)
			, remaining_bytes_{_remaining_bytes}
		{
			res_.set(http::field::server, irods::http::version::server_name);
			res_.set(http::field::content_type, "application/octet-stream");
			res_.keep_alive(_http_keep_alive);
			res_.chunked(true);
			res_.body().data = nullptr;
			res_.body().more = true;
		}

		auto start() -> void
		{
			logging::trace(*sess_ptr_, "{}: Posting task for asynchronously writing headers.", __func__);

			async_write_header(
				sess_ptr_->stream(),
				serializer_,
				[self = shared_from_this(), fn = __func__](const auto& ec, std::size_t _bytes_transferred) mutable {
					logging::trace(
						*self->sess_ptr_, "{}: Wrote [{}] bytes representing headers.", fn, _bytes_transferred);

					if (ec) {
						logging::error(*self->sess_ptr_, "{}: Encountered unexpected error while writing headers.", fn);
						return;
					}

					self->stream_bytes_to_client();
				});
		} // start

	  private:
		auto stream_bytes_to_client() -> void
		{
			irods::http::globals::background_task([self = shared_from_this(), fn = __func__]() mutable {
				self->in_.read(
					self->buffer_.data(),
					// NOLINTNEXTLINE(bugprone-narrowing-conversions, cppcoreguidelines-narrowing-conversions)
					std::min<std::streamsize>(self->buffer_.size(), self->remaining_bytes_));

				if (self->in_.fail()) {
					logging::error(*self->sess_ptr_, "{}: Stream is in a bad state.", fn);
					return;
				}

				if (self->in_.eof() || 0 == self->remaining_bytes_) {
					logging::debug(*self->sess_ptr_, "{}: All bytes have been read.", fn);
					self->res_.body().data = nullptr;
					self->res_.body().more = false;
				}
				else {
					logging::debug(*self->sess_ptr_, "{}: Read [{}] bytes from data object.", fn, self->in_.gcount());
					self->remaining_bytes_ -= self->in_.gcount();
					self->res_.body().data = self->buffer_.data();
					self->res_.body().size = self->in_.gcount();
					self->res_.body().more = true;
				}

				async_write(
					self->sess_ptr_->stream(),
					self->serializer_,
					[self = self->shared_from_this(), fn = fn](
						const auto& _ec, std::size_t _bytes_transferred) mutable {
						logging::debug(*self->sess_ptr_, "{}: Wrote [{}] bytes to socket.", fn, _bytes_transferred);

						if (_ec == http::error::need_buffer) {
							self->stream_bytes_to_client();
						}
						else if (_ec) {
							logging::error(*self->sess_ptr_, "{}: Error writing bytes to socket: {}", fn, _ec.what());
						}
					});
			});
		} // stream_bytes_to_client

		irods::http::session_pointer_type sess_ptr_;
		http::response<http::buffer_body> res_;
		http::response_serializer<http::buffer_body> serializer_;

		irods::experimental::client_connection conn_;
		std::unique_ptr<io::client::native_transport> tp_;
		io::idstream in_;

		std::vector<char> buffer_;
		std::int64_t remaining_bytes_;
	}; // incremental_read

	class incremental_write : public std::enable_shared_from_this<incremental_write>
	{
	  public:
		incremental_write(
			irods::http::session_pointer_type& _sess_ptr,
			unsigned int _http_version,
			bool _http_keep_alive,
			irods::http::connection_facade _conn,
			std::unique_ptr<io::client::native_transport> _tp,
			std::unique_ptr<io::odstream> _out,
			io::odstream* _out_ptr,
			std::unique_ptr<irods::at_scope_exit<std::function<void()>>> _mark_pw_stream_as_usable,
			std::string _buffer,
			std::int64_t _remaining_bytes,
			std::int64_t _max_bytes_per_write,
			bool _is_parallel_write)
			: sess_ptr_{_sess_ptr->shared_from_this()}
			, res_{http::status::ok, _http_version}
			, conn_{std::move(_conn)}
			, tp_{std::move(_tp)}
			, out_{std::move(_out)}
			, out_ptr_{_out_ptr}
			, mark_pw_stream_as_usable_{std::move(_mark_pw_stream_as_usable)}
			, buffer_{std::move(_buffer)}
			, remaining_bytes_{_remaining_bytes}
			, max_bytes_per_write_{_max_bytes_per_write}
			, read_pos_{buffer_.data()}
			, is_parallel_write_{_is_parallel_write}
		{
			res_.set(http::field::server, irods::http::version::server_name);
			res_.set(http::field::content_type, "application/json");
			res_.keep_alive(_http_keep_alive);
		} // constructor

		auto start() -> void
		{
			stream_bytes_to_irods();
		} // start

	  private:
		auto stream_bytes_to_irods() -> void
		{
			irods::http::globals::background_task([self = shared_from_this(), fn = __func__]() mutable {
				try {
					if (self->remaining_bytes_ > 0) {
						if (!*self->out_ptr_) {
							logging::error(
								*self->sess_ptr_,
								"{}: Output stream is in a bad state. Client should restart the entire transfer.",
								fn);
							return self->sess_ptr_->send(
								irods::http::fail(self->res_, http::status::internal_server_error));
						}

						const auto to_send =
							std::min<std::streamsize>(self->remaining_bytes_, self->max_bytes_per_write_);
						logging::debug(
							*self->sess_ptr_,
							"{}: Write buffer: remaining=[{}], sending=[{}].",
							fn,
							self->remaining_bytes_,
							to_send);
						self->out_ptr_->write(self->read_pos_, to_send);
						self->read_pos_ += to_send; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
						self->remaining_bytes_ -= to_send;

						return self->stream_bytes_to_irods();
					}

					// If we're performing a normal write, close the stream before returning a response.
					// This is required so that the iRODS server triggers appropriate policy before handing
					// back control to the client. For example, replication resources and synchronous replication.
					if (!self->is_parallel_write_) {
						self->out_ptr_->close();
					}

					self->res_.body() = json{{"irods_response", {{"status_code", 0}}}}.dump();
					self->res_.prepare_payload();
					self->sess_ptr_->send(std::move(self->res_));
				}
				catch (const json::exception& e) {
					logging::error(*self->sess_ptr_, "{}: {}", fn, e.what());
					return self->sess_ptr_->send(irods::http::fail(self->res_, http::status::internal_server_error));
				}
				catch (const std::exception& e) {
					logging::error(*self->sess_ptr_, "{}: {}", fn, e.what());
					return self->sess_ptr_->send(irods::http::fail(self->res_, http::status::internal_server_error));
				}
			});
		} // stream_bytes_to_irods

		// The following member variables represent state initialized by op_write.
		// Instances of this class require the state to last until after the final write
		// operation completes or an error occurs, whichever happens first.
		//
		// Instances of this class own all state passed from op_write.

		irods::http::session_pointer_type sess_ptr_;
		http::response<http::string_body> res_;

		irods::http::connection_facade conn_;
		std::unique_ptr<io::client::native_transport> tp_;
		std::unique_ptr<io::odstream> out_;
		io::odstream* out_ptr_;

		// A callable for signaling when a parallel-write stream is available for use.
		std::unique_ptr<irods::at_scope_exit<std::function<void()>>> mark_pw_stream_as_usable_;

		// The data to write to iRODS and information for tracking progress.
		std::string buffer_;
		std::int64_t remaining_bytes_;
		std::int64_t max_bytes_per_write_;
		const char* read_pos_;

		// Indicates whether the client is performing a parallel write.
		const bool is_parallel_write_;
	}; // incremental_write

	class streaming_write : public std::enable_shared_from_this<streaming_write>
	{
	  public:
		streaming_write(
			irods::http::session_pointer_type& _sess_ptr,
			unsigned int _http_version,
			bool _http_keep_alive,
			irods::http::connection_facade _conn,
			std::unique_ptr<io::client::native_transport> _tp,
			std::unique_ptr<io::odstream> _out,
			io::odstream* _out_ptr,
			std::unique_ptr<irods::at_scope_exit<std::function<void()>>> _mark_pw_stream_as_usable,
			std::int64_t _max_bytes_per_write,
			bool _is_parallel_write)
			: sess_ptr_{_sess_ptr->shared_from_this()}
			, res_{http::status::ok, _http_version}
			, bb_parser_{std::move(*sess_ptr_->parser())}
			, conn_{std::move(_conn)}
			, tp_{std::move(_tp)}
			, out_{std::move(_out)}
			, out_ptr_{_out_ptr}
			, mark_pw_stream_as_usable_{std::move(_mark_pw_stream_as_usable)}
			, buffer_(_max_bytes_per_write)
			, is_parallel_write_{_is_parallel_write}
		{
			res_.set(http::field::server, irods::http::version::server_name);
			res_.set(http::field::content_type, "application/json");
			res_.keep_alive(_http_keep_alive);
		} // constructor

		auto start() -> void
		{
			stream_bytes_from_client();
		} // start

	  private:
		auto stream_bytes_from_client() -> void
		{
			try {
				logging::trace(
					*sess_ptr_, "{}: Reading bytes from socket/client and filling internal buffer.", __func__);

				if (!bb_parser_.is_done()) {
					// Set up the body for writing into our internal buffer.
					bb_parser_.get().body().data = buffer_.data();
					bb_parser_.get().body().size = buffer_.size();

					http::async_read(
						sess_ptr_->stream(),
						sess_ptr_->buffer(),
						bb_parser_,
						[self = shared_from_this(), fn = __func__](
							beast::error_code _ec, std::size_t _bytes_transferred) mutable {
							if (http::error::need_buffer == _ec) {
								_ec = {};
								logging::trace(
									*self->sess_ptr_,
									"{}: Read [{}] bytes from socket/client. Expecting more data.",
									fn,
									_bytes_transferred);
							}

							if (_ec) {
								logging::error(
									*self->sess_ptr_,
									"{}: Error while reading bytes from client socket; error=[{}]",
									fn,
									_ec.message());
								return self->sess_ptr_->send(
									irods::http::fail(self->res_, http::status::internal_server_error));
							}

							self->stream_bytes_to_irods();
						});
				}
				else {
					bb_parser_.get().body().data = nullptr;
					bb_parser_.get().body().size = 0;

					// If we're performing a normal write, close the stream before returning a response.
					// This is required so that the iRODS server triggers appropriate policy before handing
					// back control to the client. For example, replication resources and synchronous replication.
					if (!is_parallel_write_) {
						out_ptr_->close();
					}

					res_.body() = json{{"irods_response", {{"status_code", 0}}}}.dump();
					res_.prepare_payload();
					sess_ptr_->send(std::move(res_));
				}
			}
			catch (const std::exception& e) {
				logging::error(*sess_ptr_, "{}: {}", __func__, e.what());
				return sess_ptr_->send(irods::http::fail(res_, http::status::internal_server_error));
			}
		} // stream_bytes_from_client

		auto stream_bytes_to_irods() -> void
		{
			irods::http::globals::background_task([self = shared_from_this(), fn = __func__]() mutable {
				try {
					if (!*self->out_ptr_) {
						logging::error(
							*self->sess_ptr_,
							"{}: Output stream is in a bad state. Client should restart the entire transfer.",
							fn);
						return self->sess_ptr_->send(
							irods::http::fail(self->res_, http::status::internal_server_error));
					}

					const auto to_send = self->buffer_.size() - self->bb_parser_.get().body().size;
					logging::debug(*self->sess_ptr_, "{}: Writing [{}] bytes to the data object.", fn, to_send);
					self->out_ptr_->write(self->buffer_.data(), to_send);

					return self->stream_bytes_from_client();
				}
				catch (const json::exception& e) {
					logging::error(*self->sess_ptr_, "{}: {}", fn, e.what());
					return self->sess_ptr_->send(irods::http::fail(self->res_, http::status::internal_server_error));
				}
				catch (const std::exception& e) {
					logging::error(*self->sess_ptr_, "{}: {}", fn, e.what());
					return self->sess_ptr_->send(irods::http::fail(self->res_, http::status::internal_server_error));
				}
			});
		} // stream_bytes_to_irods

		// The following member variables represent state initialized by op_streaming_write.
		// Instances of this class require the state to last until after the final write
		// operation completes or an error occurs, whichever happens first.
		//
		// Instances of this class own all state passed from op_write.

		irods::http::session_pointer_type sess_ptr_;
		http::response<http::string_body> res_;
		http::request_parser<http::buffer_body> bb_parser_;

		irods::http::connection_facade conn_;
		std::unique_ptr<io::client::native_transport> tp_;
		std::unique_ptr<io::odstream> out_;
		io::odstream* out_ptr_;

		// A callable for signaling when a parallel-write stream is available for use.
		std::unique_ptr<irods::at_scope_exit<std::function<void()>>> mark_pw_stream_as_usable_;

		// The data to write to iRODS and information for tracking progress.
		std::vector<char> buffer_;

		// Indicates whether the client is performing a parallel write.
		const bool is_parallel_write_;
	}; // streaming_write

	//
	// Operation handler implementations
	//

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_read)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task([fn = __func__,
		                                       client_info,
		                                       _sess_ptr,
		                                       _req = std::move(_req),
		                                       _args = std::move(_args)]() mutable {
			logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

			http::response<http::string_body> res{http::status::ok, _req.version()};
			res.set(http::field::server, irods::http::version::server_name);
			res.set(http::field::content_type, "application/json");
			res.keep_alive(_req.keep_alive());

			try {
				const auto lpath_iter = _args.find("lpath");
				if (lpath_iter == std::end(_args)) {
					logging::error(*_sess_ptr, "{}: Missing [lpath] parameter.", fn);
					return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
				}

				auto conn = irods::get_connection(client_info.username);

				// Enable ticket if the request includes one.
				if (const auto iter = _args.find("ticket"); iter != std::end(_args)) {
					if (const auto ec = irods::enable_ticket(conn, iter->second); ec < 0) {
						res.result(http::status::internal_server_error);
						res.body() =
							json{{"irods_response",
						          {{"status_code", ec}, {"status_message", "Error enabling ticket on connection."}}}}
								.dump();
						res.prepare_payload();
						return _sess_ptr->send(std::move(res));
					}
				}

				const auto status = fs::client::status(conn, lpath_iter->second);

				if (!fs::client::is_data_object(status)) {
					logging::error(
						*_sess_ptr,
						"{}: Logical path [{}] does not point to a data object or does not exist.",
						fn,
						lpath_iter->second);
					res.result(http::status::not_found);
					res.prepare_payload();
					return _sess_ptr->send(std::move(res));
				}

				std::int64_t offset = 0;
				if (const auto iter = _args.find("offset"); iter != std::end(_args)) {
					try {
						offset = std::stoll(iter->second);
					}
					catch (const std::exception& e) {
						logging::error(
							*_sess_ptr, "{}: Invalid value for [offset] parameter. Received [{}].", fn, iter->second);
						res.result(http::status::bad_request);
						res.prepare_payload();
						return _sess_ptr->send(std::move(res));
					}

					if (std::cmp_less(offset, 0)) {
						logging::error(
							*_sess_ptr,
							"{}: Invalid value for [offset] parameter. Must be non-negative. Received [{}].",
							fn,
							iter->second);
						res.result(http::status::bad_request);
						res.prepare_payload();
						return _sess_ptr->send(std::move(res));
					}
				}

				const auto data_object_size = fs::client::data_object_size(conn, lpath_iter->second);
				std::int64_t count = data_object_size - offset;

				if (const auto iter = _args.find("count"); iter != std::end(_args)) {
					try {
						count = std::stoll(iter->second);
					}
					catch (const std::exception& e) {
						logging::error(
							*_sess_ptr, "{}: Invalid value for [count] parameter. Received [{}].", fn, iter->second);
						res.result(http::status::bad_request);
						res.prepare_payload();
						return _sess_ptr->send(std::move(res));
					}

					if (std::cmp_less(count, 0)) {
						logging::error(
							*_sess_ptr,
							"{}: Invalid value for [count] parameter. Must be non-negative. Received [{}].",
							fn,
							iter->second);
						res.result(http::status::bad_request);
						res.prepare_payload();
						return _sess_ptr->send(std::move(res));
					}

					if (const auto real_count = data_object_size - offset; std::cmp_greater(count, real_count)) {
						count = real_count;
					}
				}

				// When the internal buffer size is exceeded, we have to execute the reads across
				// multiple tasks. Each read would be posted to the thread pool individually and
				// sequentially. For that reason, the reads must have a dedicated connection. We
				// can't use the connections from the connection pool because doing that can lead
				// to an unresponsive server.

				static const auto read_buffer_size =
					irods::http::globals::configuration()
						.at(json::json_pointer{"/irods_client/max_number_of_bytes_per_read_operation"})
						.get<int>();

				if (std::cmp_greater(count, read_buffer_size)) {
					static const auto enable_4_2_compat =
						irods::http::globals::configuration()
							.at(json::json_pointer{"/irods_client/enable_4_2_compatibility"})
							.get<bool>();

					irods::experimental::client_connection dedicated_conn{irods::experimental::defer_connection};

					if (enable_4_2_compat) {
						logging::trace(
							*_sess_ptr, "{}: 4.2 compatibility enabled. Using existing iRODS connection.", fn);

						// get_connection() always returns connections to the iRODS server when 4.2
						// compatibility is enabled. Therefore, we can continue to use the existing
						// connection instead of creating another connection like in the else-branch.
						dedicated_conn = std::move(conn.get_ref<irods::experimental::client_connection>());
					}
					else {
						logging::trace(
							*_sess_ptr,
							"{}: 4.2 compatibility disabled. Internal buffer size exceeded. Using dedicated iRODS "
							"connection.",
							fn);

						const auto& config = irods::http::globals::configuration();
						const auto& host =
							config.at(json::json_pointer{"/irods_client/host"}).get_ref<const std::string&>();
						const auto port = config.at(json::json_pointer{"/irods_client/port"}).get<int>();
						const auto& zone =
							config.at(json::json_pointer{"/irods_client/zone"}).get_ref<const std::string&>();
						const auto& rodsadmin_username =
							config.at(json::json_pointer{"/irods_client/proxy_admin_account/username"})
								.get_ref<const std::string&>();
						auto rodsadmin_password =
							config.at(json::json_pointer{"/irods_client/proxy_admin_account/password"})
								.get<std::string>();

						logging::trace(*_sess_ptr, "{}: Connecting to iRODS server as [{}].", fn, client_info.username);
						dedicated_conn.connect(
							irods::experimental::defer_authentication,
							host,
							port,
							{rodsadmin_username, zone},
							{client_info.username, zone});

#ifdef IRODS_DEV_PACKAGE_IS_AT_LEAST_IRODS_5
						// clang-format off
						const auto ec = rc_authenticate_client(
							static_cast<RcComm*>(dedicated_conn),
							nlohmann::json{
								{"scheme", "native"},
								{irods::AUTH_PASSWORD_KEY, rodsadmin_password},
							}.dump().c_str());
						// clang-format on
#else
						const auto ec =
							clientLoginWithPassword(static_cast<RcComm*>(dedicated_conn), rodsadmin_password.data());
#endif // IRODS_DEV_PACKAGE_IS_AT_LEAST_IRODS_5
						if (ec < 0) {
							logging::error(
								*_sess_ptr, "{}: Could not create dedicated connection for read operation.", fn);
							return _sess_ptr->send(irods::http::fail(
								res,
								http::status::internal_server_error,
								json{{"irods_response", {{"status_code", ec}}}}.dump()));
						}
					}

					logging::trace(
						*_sess_ptr, "{}: Opening stream for reading to data object [{}].", fn, lpath_iter->second);
					auto tp = std::make_unique<io::client::native_transport>(dedicated_conn);
					io::idstream in;

					if (auto iter = _args.find("resource"); iter != std::end(_args)) {
						logging::debug(
							*_sess_ptr,
							"{}: Opening replica of [{}] on resource [{}].",
							fn,
							lpath_iter->second,
							iter->second);
						in.open(*tp, lpath_iter->second, io::root_resource_name{iter->second});
					}
					else if (iter = _args.find("replica-number"); iter != std::end(_args)) {
						int value = -1;
						try {
							value = std::stoi(iter->second);
						}
						catch (const std::exception& e) {
							logging::error(
								*_sess_ptr, "{}: Could not convert replica number [{}] to integer.", fn, iter->second);
							res.result(http::status::bad_request);
							res.prepare_payload();
							return _sess_ptr->send(std::move(res));
						}

						logging::debug(
							*_sess_ptr, "{}: Opening replica [{}] of [{}].", fn, iter->second, lpath_iter->second);
						in.open(*tp, lpath_iter->second, io::replica_number{value});
					}
					else {
						in.open(*tp, lpath_iter->second);
					}

					if (!in) {
						logging::error(
							*_sess_ptr, "{}: Could not open data object [{}] for read.", fn, lpath_iter->second);
						res.result(http::status::internal_server_error);
						res.prepare_payload();
						return _sess_ptr->send(std::move(res));
					}

					logging::trace(
						*_sess_ptr, "{}: Seeking to offset [{}] in data object [{}].", fn, offset, lpath_iter->second);
					if (offset > 0 && !in.seekg(offset)) {
						logging::error(
							*_sess_ptr,
							"{}: Could not seek to position [{}] in data object [{}].",
							fn,
							offset,
							lpath_iter->second);
						res.result(http::status::internal_server_error);
						res.prepare_payload();
						return _sess_ptr->send(std::move(res));
					}

					// clang-format off
					std::make_shared<incremental_read>(
						_sess_ptr, _req.version(), _req.keep_alive(), dedicated_conn, tp, in, read_buffer_size, count)
							->start();
					// clang-format on

					return;
				}

				//
				// At this point, we know the requested number of bytes to read can fit inside the
				// the buffer used for reading data object (i.e. the size defined in the config file).
				//

				logging::trace(
					*_sess_ptr,
					"{}: Requested number of bytes fits into internal buffer. Using existing connection.",
					fn,
					lpath_iter->second);

				io::client::native_transport tp{conn};
				io::idstream in;

				if (auto iter = _args.find("resource"); iter != std::end(_args)) {
					logging::debug(
						*_sess_ptr,
						"{}: Opening replica of [{}] on resource [{}].",
						fn,
						lpath_iter->second,
						iter->second);
					in.open(tp, lpath_iter->second, io::root_resource_name{iter->second});
				}
				else if (iter = _args.find("replica-number"); iter != std::end(_args)) {
					int value = -1;
					try {
						value = std::stoi(iter->second);
					}
					catch (const std::exception& e) {
						logging::error(
							*_sess_ptr, "{}: Could not convert replica number [{}] to integer.", fn, iter->second);
						res.result(http::status::bad_request);
						res.prepare_payload();
						return _sess_ptr->send(std::move(res));
					}

					logging::debug(
						*_sess_ptr, "{}: Opening replica [{}] of [{}].", fn, iter->second, lpath_iter->second);
					in.open(tp, lpath_iter->second, io::replica_number{value});
				}
				else {
					in.open(tp, lpath_iter->second);
				}

				if (!in) {
					logging::error(*_sess_ptr, "{}: Could not open data object [{}] for read.", fn, lpath_iter->second);
					res.result(http::status::internal_server_error);
					res.prepare_payload();
					return _sess_ptr->send(std::move(res));
				}

				if (offset > 0 && !in.seekg(offset)) {
					logging::error(
						*_sess_ptr,
						"{}: Could not seek to position [{}] in data object [{}].",
						fn,
						offset,
						lpath_iter->second);
					res.result(http::status::internal_server_error);
					res.prepare_payload();
					return _sess_ptr->send(std::move(res));
				}

				std::vector<char> buffer(count);

				if (!in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()))) {
					logging::error(
						*_sess_ptr, "{}: Could not read bytes from data object [{}].", fn, lpath_iter->second);
					res.result(http::status::internal_server_error);
					res.prepare_payload();
					return _sess_ptr->send(std::move(res));
				}

				res.body() = std::string_view(buffer.data(), in.gcount());
			}
			catch (const fs::filesystem_error& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
				res.result(http::status::internal_server_error);
			}
			catch (const irods::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
				res.result(http::status::internal_server_error);
			}
			catch (const std::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
				res.result(http::status::internal_server_error);
			}

			res.prepare_payload();

			_sess_ptr->send(std::move(res));
		});
	} // op_read

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_write)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task([fn = __func__,
		                                       client_info,
		                                       _sess_ptr,
		                                       _req = std::move(_req),
		                                       _args = std::move(_args)]() mutable {
			logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

			http::response<http::string_body> res{http::status::ok, _req.version()};
			res.set(http::field::server, irods::http::version::server_name);
			res.set(http::field::content_type, "application/json");
			res.keep_alive(_req.keep_alive());

			try {
				// Used to determine whether the data object should be closed following the write
				// operation or by the parallel_write_shutdown HTTP API operation.
				bool is_parallel_write = false;

				irods::http::connection_facade conn;
				std::unique_ptr<io::client::native_transport> tp;

				std::unique_ptr<io::odstream> out;
				io::odstream* out_ptr{};

				const auto parallel_write_handle_iter = _args.find("parallel-write-handle");

				using at_scope_exit_type = irods::at_scope_exit<std::function<void()>>;
				std::unique_ptr<at_scope_exit_type> mark_pw_stream_as_usable;

				if (parallel_write_handle_iter != std::end(_args)) {
					logging::debug(
						*_sess_ptr,
						"{}: (write) Parallel Write Handle = [{}].",
						fn,
						parallel_write_handle_iter->second);

					decltype(parallel_write_contexts)::iterator iter;

					{
						std::shared_lock lk{pwc_mtx};

						iter = parallel_write_contexts.find(parallel_write_handle_iter->second);
						if (iter == std::end(parallel_write_contexts)) {
							logging::error(*_sess_ptr, "{}: Invalid handle for parallel write.", fn);
							return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
						}
					}

					//
					// We've found a matching handle!
					//

					is_parallel_write = true;

					if (const auto stream_index_iter = _args.find("stream-index"); stream_index_iter != std::end(_args))
					{
						logging::debug(
							*_sess_ptr,
							"{}: Client selected [{}] for [stream-index] parameter.",
							fn,
							stream_index_iter->second);

						try {
							const auto sindex = std::stoi(stream_index_iter->second);
							out_ptr = &iter->second.streams.at(sindex)->stream();
						}
						catch (const std::exception& e) {
							logging::error(*_sess_ptr, "{}: Invalid argument for [stream-index] parameter.", fn);
							return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
						}
					}
					else {
						auto* pw_stream = iter->second.find_available_parallel_write_stream();
						if (!pw_stream) {
							logging::error(
								*_sess_ptr,
								"{}: Parallel write streams are busy. Client must wait for one to become available.",
								fn);
							return _sess_ptr->send(irods::http::fail(res, http::status::too_many_requests));
						}

						mark_pw_stream_as_usable =
							std::make_unique<at_scope_exit_type>([pw_stream] { pw_stream->in_use(false); });

						out_ptr = &pw_stream->stream();
					}

					logging::debug(
						*_sess_ptr,
						"{}: (write) Parallel Write - stream memory address = [{}].",
						fn,
						fmt::ptr(out_ptr));
				}
				else {
					const auto lpath_iter = _args.find("lpath");
					if (lpath_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [lpath] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					auto openmode = std::ios_base::out;

					if (const auto iter = _args.find("truncate"); iter != std::end(_args) && iter->second == "0") {
						openmode |= std::ios_base::in;
					}

					if (const auto iter = _args.find("append"); iter != std::end(_args) && iter->second == "1") {
						openmode |= std::ios_base::app;
					}

					logging::trace(*_sess_ptr, "{}: Opening data object [{}] for write.", fn, lpath_iter->second);
					logging::trace(*_sess_ptr, "{}: (write) Initializing for single buffer write.", fn);

					conn = irods::get_connection(client_info.username);

					// Enable ticket if the request includes one.
					if (const auto iter = _args.find("ticket"); iter != std::end(_args)) {
						if (const auto ec = irods::enable_ticket(conn, iter->second); ec < 0) {
							res.result(http::status::internal_server_error);
							res.body() = json{{"irods_response",
							                   {{"status_code", ec},
							                    {"status_message", "Error enabling ticket on connection."}}}}
							                 .dump();
							res.prepare_payload();
							return _sess_ptr->send(std::move(res));
						}
					}

					tp = std::make_unique<io::client::native_transport>(conn);

					if (const auto iter = _args.find("resource"); iter != std::end(_args)) {
						out = std::make_unique<io::odstream>(
							*tp, lpath_iter->second, io::root_resource_name{iter->second}, openmode);
					}
					else if (const auto iter = _args.find("replica-number"); iter != std::end(_args)) {
						int value = -1;
						try {
							value = std::stoi(iter->second);
						}
						catch (const std::exception& e) {
							logging::error(
								*_sess_ptr, "{}: Could not convert replica number [{}] to integer.", fn, iter->second);
							res.result(http::status::bad_request);
							res.prepare_payload();
							return _sess_ptr->send(std::move(res));
						}

						out = std::make_unique<io::odstream>(
							*tp, lpath_iter->second, io::replica_number{value}, openmode);
					}
					else {
						out = std::make_unique<io::odstream>(*tp, lpath_iter->second, openmode);
					}

					out_ptr = out.get();
				}

				if (!*out_ptr) {
					logging::error(*_sess_ptr, "{}: Output stream to data object is in a bad state.", fn);
					// clang-format off
					res.body() = json{
						{"irods_response", {
							{"status_code", INVALID_HANDLE},
							{"status_message", "Output stream to data object is in a bad state."}
						}}
					}.dump();
					// clang-format on
					res.prepare_payload();
					return _sess_ptr->send(std::move(res));
				}

				auto close_output_stream_if_not_parallel_write_stream = [&is_parallel_write, out_ptr] {
					// If we're performing a normal write, close the stream before returning a response.
					// This is required so that the iRODS server triggers appropriate policy before handing
					// back control to the client. For example, replication resources and synchronous replication.
					if (!is_parallel_write) {
						out_ptr->close();
					}
				};

				auto iter = _args.find("offset");
				if (iter != std::end(_args)) {
					logging::trace(*_sess_ptr, "{}: Setting offset for write.", fn);
					try {
						out_ptr->seekp(std::stoll(iter->second));
					}
					catch (const std::exception& e) {
						logging::error(
							*_sess_ptr, "{}: Could not seek to position [{}] in data object.", fn, iter->second);
						close_output_stream_if_not_parallel_write_stream();
						res.result(http::status::bad_request);
						res.prepare_payload();
						return _sess_ptr->send(std::move(res));
					}

					if (!*out_ptr) {
						logging::error(*_sess_ptr, "{}: Output stream to data object is in a bad state.", __func__);
						close_output_stream_if_not_parallel_write_stream();
						// clang-format off
						res.body() = json{
							{"irods_response", {
								{"status_code", INVALID_HANDLE},
								{"status_message", "Output stream to data object is in a bad state."}
							}}
						}.dump();
						// clang-format on
						res.prepare_payload();
						return _sess_ptr->send(std::move(res));
					}
				}

				iter = _args.find("bytes");
				if (iter == std::end(_args)) {
					logging::error(*_sess_ptr, "{}: Missing [bytes] parameter.", fn);
					close_output_stream_if_not_parallel_write_stream();
					return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
				}

				auto remaining_bytes = iter->second.size();

				if (!std::cmp_equal(remaining_bytes, iter->second.size())) {
					logging::error(
						*_sess_ptr, "{}: Requirement violated: [count] and size of [bytes] do not match.", fn);
					close_output_stream_if_not_parallel_write_stream();
					return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
				}

				static const auto max_number_of_bytes_per_write =
					irods::http::globals::configuration()
						.at(json::json_pointer{"/irods_client/max_number_of_bytes_per_write_operation"})
						.get<std::int64_t>();

				// clang-format off
				std::make_shared<incremental_write>(
					_sess_ptr,
					_req.version(),
					_req.keep_alive(),
					std::move(conn),
					std::move(tp),
					std::move(out),
					out_ptr,
					std::move(mark_pw_stream_as_usable),
					std::move(iter->second),
					remaining_bytes,
					max_number_of_bytes_per_write,
					is_parallel_write)->start();
				// clang-format on
			}
			catch (const fs::filesystem_error& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
				// clang-format off
				res.body() = json{
					{"irods_response", {
						{"status_code", e.code().value()},
						{"status_message", e.what()}
					}}
				}.dump();
				// clang-format on
				res.prepare_payload();
				_sess_ptr->send(std::move(res));
			}
			catch (const irods::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
				// clang-format off
				res.body() = json{
					{"irods_response", {
						{"status_code", e.code()},
						{"status_message", e.client_display_what()}
					}}
				}.dump();
				// clang-format on
				res.prepare_payload();
				_sess_ptr->send(std::move(res));
			}
			catch (const std::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
				res.result(http::status::internal_server_error);
				res.prepare_payload();
				_sess_ptr->send(std::move(res));
			}
		});
	} // op_write

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_parallel_write_init)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task([fn = __func__,
		                                       client_info,
		                                       _sess_ptr,
		                                       _req = std::move(_req),
		                                       _args = std::move(_args)] {
			logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

			http::response<http::string_body> res{http::status::ok, _req.version()};
			res.set(http::field::server, irods::http::version::server_name);
			res.set(http::field::content_type, "application/json");
			res.keep_alive(_req.keep_alive());

			try {
				const auto lpath_iter = _args.find("lpath");
				if (lpath_iter == std::end(_args)) {
					logging::error(*_sess_ptr, "{}: Missing [lpath] parameter.", fn);
					return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
				}

				const auto stream_count_iter = _args.find("stream-count");
				if (stream_count_iter == std::end(_args)) {
					logging::error(*_sess_ptr, "{}: Missing [stream-count] parameter.", fn);
					return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
				}

				const auto stream_count = std::stoi(stream_count_iter->second);
				if (stream_count > irods::http::globals::configuration()
				                       .at(json::json_pointer{"/irods_client/max_number_of_parallel_write_streams"})
				                       .get<int>())
				{
					logging::error(
						*_sess_ptr,
						"{}: Argument for [stream-count] parameter exceeds maximum number of streams allowed.",
						fn,
						stream_count_iter->second);
					res.result(http::status::bad_request);
					res.prepare_payload();
					return _sess_ptr->send(std::move(res));
				}

				namespace io = irods::experimental::io;

				logging::trace(*_sess_ptr, "{}: Opening primary output stream to [{}].", fn, lpath_iter->second);

				std::vector<std::shared_ptr<parallel_write_stream>> pw_streams;
				pw_streams.reserve(stream_count);

				try {
					auto openmode = std::ios_base::out;

					if (const auto iter = _args.find("truncate"); iter != std::end(_args) && iter->second == "0") {
						openmode |= std::ios_base::in;
					}

					if (const auto iter = _args.find("append"); iter != std::end(_args) && iter->second == "1") {
						openmode |= std::ios_base::app;
					}

					std::optional<std::string> ticket;
					if (const auto iter = _args.find("ticket"); iter != std::end(_args)) {
						ticket = iter->second;
					}

					std::optional<std::string> resource;
					if (const auto iter = _args.find("resource"); iter != std::end(_args)) {
						resource = iter->second;
					}

					// Open the primary stream.
					pw_streams.emplace_back(std::make_shared<parallel_write_stream>(
						client_info.username, lpath_iter->second, resource, openmode, ticket));

					auto& first_stream = pw_streams.front()->stream();
					logging::debug(
						*_sess_ptr,
						"{}: replica token=[{}], replica number=[{}], leaf resource name=[{}]",
						fn,
						first_stream.replica_token().value,
						first_stream.replica_number().value,
						first_stream.leaf_resource_name().value);

					// Open secondary streams using the first stream as a base.
					logging::trace(*_sess_ptr, "{}: Opening secondary output streams to [{}].", fn, lpath_iter->second);
					for (int i = 0; i < stream_count; ++i) {
						pw_streams.emplace_back(std::make_shared<parallel_write_stream>(
							client_info.username,
							lpath_iter->second,
							std::nullopt,
							openmode,
							ticket,
							&pw_streams.front()->stream()));
					}
				}
				catch (const irods::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
					// clang-format off
					res.body() = json{
						{"irods_response", {
							{"status_code", e.code()},
							{"status_message", e.client_display_what()}
						}}
					}.dump();
					// clang-format on
					res.prepare_payload();
					return _sess_ptr->send(std::move(res));
				}

				std::string transfer_handle;
				decltype(parallel_write_contexts)::iterator pwc_iter;

				{
					std::scoped_lock lk{pwc_mtx};

					transfer_handle = irods::generate_uuid(parallel_write_contexts);
					logging::debug(*_sess_ptr, "{}: (init) Parallel Write Handle = [{}].", fn, transfer_handle);

					auto [iter, insertion_result] =
						parallel_write_contexts.emplace(transfer_handle, parallel_write_context{});
					if (!insertion_result) {
						logging::error(
							*_sess_ptr,
							"{}: Could not initialize parallel write context for [{}].",
							fn,
							lpath_iter->second);
						res.result(http::status::internal_server_error);
						res.prepare_payload();
						return _sess_ptr->send(std::move(res));
					}

					pwc_iter = iter;
				}

				auto& pw_context = pwc_iter->second;
				pw_context.streams = std::move(pw_streams);
				pw_context.mtx = std::make_unique<std::mutex>();

				// clang-format off
				res.body() = json{
					{"irods_response", {
						{"status_code", 0}
					}},
					{"parallel_write_handle", transfer_handle}
				}.dump();
				// clang-format on
			}
			catch (const fs::filesystem_error& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
				res.body() =
					json{{"irods_response", {{"status_code", e.code().value()}, {"status_message", e.what()}}}}.dump();
			}
			catch (const irods::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
				res.body() =
					json{{"irods_response", {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
						.dump();
			}
			catch (const std::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
				res.result(http::status::internal_server_error);
			}

			res.prepare_payload();

			_sess_ptr->send(std::move(res));
		});
	} // op_parallel_write_init

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_parallel_write_shutdown)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task([fn = __func__,
		                                       client_info,
		                                       _sess_ptr,
		                                       _req = std::move(_req),
		                                       _args = std::move(_args)] {
			logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

			http::response<http::string_body> res{http::status::ok, _req.version()};
			res.set(http::field::server, irods::http::version::server_name);
			res.set(http::field::content_type, "application/json");
			res.keep_alive(_req.keep_alive());

			try {
				// 1. Verify transfer handle and lookup PTC.
				// 2. Close all streams in reverse order.
				// 3. Disassociate the transfer handle and PTC.
				// 4. Free resources.

				const auto parallel_write_handle_iter = _args.find("parallel-write-handle");
				if (parallel_write_handle_iter == std::end(_args)) {
					logging::error(*_sess_ptr, "{}: Missing [parallel-write-handle] parameter.", fn);
					return _sess_ptr->send(irods::http::fail(http::status::bad_request));
				}

				logging::debug(*_sess_ptr, "{}: Parallel write handle = [{}]", fn, parallel_write_handle_iter->second);

				{
					std::scoped_lock lk{pwc_mtx};

					const auto pw_iter = parallel_write_contexts.find(parallel_write_handle_iter->second);
					if (pw_iter != std::end(parallel_write_contexts)) {
						logging::trace(
							*_sess_ptr, "{}: Closing secondary output streams. Skipping catalog update.", fn);

						// Ignore the first stream. It must be closed last so that replication resources
						// are triggered correctly.
						auto end = std::prev(std::rend(pw_iter->second.streams));

						io::on_close_success close_input{};
						close_input.update_size = false;
						close_input.update_status = false;
						close_input.compute_checksum = false;
						close_input.send_notifications = false;
						close_input.preserve_replica_state_table = false;

						for (auto iter = std::rbegin(pw_iter->second.streams); iter != end; ++iter) {
							(*iter)->stream().close(&close_input);
						}

						// Allow the first stream to update the catalog.
						logging::trace(
							*_sess_ptr, "{}: Closing primary output stream and updating catalog information.", fn);
						pw_iter->second.streams.front()->stream().close();

						logging::trace(
							*_sess_ptr,
							"{}: Removing parallel write handle [{}].",
							fn,
							parallel_write_handle_iter->second);
						parallel_write_contexts.erase(pw_iter);
					}
				}

				res.body() = json{{"irods_response", {{"status_code", 0}}}}.dump();
			}
			catch (const fs::filesystem_error& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
				res.body() =
					json{{"irods_response", {{"status_code", e.code().value()}, {"status_message", e.what()}}}}.dump();
			}
			catch (const irods::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
				res.body() =
					json{{"irods_response", {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
						.dump();
			}
			catch (const std::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
				res.result(http::status::internal_server_error);
			}

			res.prepare_payload();

			_sess_ptr->send(std::move(res));
		});
	} // op_parallel_write_shutdown

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_replicate)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task(
			[fn = __func__, client_info, _sess_ptr, _req = std::move(_req), _args = std::move(_args)] {
				logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

				http::response<http::string_body> res{http::status::ok, _req.version()};
				res.set(http::field::server, irods::http::version::server_name);
				res.set(http::field::content_type, "application/json");
				res.keep_alive(_req.keep_alive());

				try {
					DataObjInp input{};
					irods::at_scope_exit free_memory{[&input] { clearKeyVal(&input.condInput); }};

					if (const auto iter = _args.find("lpath"); iter != std::end(_args)) {
						irods::strncpy_null_terminated(input.objPath, iter->second.c_str());
					}
					else {
						logging::error(*_sess_ptr, "{}: Missing [lpath] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					if (const auto iter = _args.find("dst-resource"); iter != std::end(_args)) {
						addKeyVal(&input.condInput, DEST_RESC_NAME_KW, iter->second.c_str());
					}
					else {
						logging::error(*_sess_ptr, "{}: Missing [dst-resource] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(http::status::bad_request));
					}

					if (const auto iter = _args.find("src-resource"); iter != std::end(_args)) {
						addKeyVal(&input.condInput, RESC_NAME_KW, iter->second.c_str());
					}

					if (const auto iter = _args.find("admin"); iter != std::end(_args) && iter->second == "1") {
						addKeyVal(&input.condInput, ADMIN_KW, "");
					}

					auto conn = irods::get_connection(client_info.username);
					const auto ec = rcDataObjRepl(static_cast<RcComm*>(conn), &input);

					// clang-format off
					res.body() = json{
						{"irods_response", {
							{"status_code", ec}
						}}
					}.dump();
					// clang-format on
				}
				catch (const irods::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
					// clang-format off
					res.body() = json{
						{"irods_response", {
							{"status_code", e.code()},
							{"status_message", e.client_display_what()}
						}}
					}.dump();
					// clang-format on
				}
				catch (const std::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.what());
					res.result(http::status::internal_server_error);
				}

				res.prepare_payload();

				_sess_ptr->send(std::move(res));
			});
	} // op_replicate

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_trim)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task(
			[fn = __func__, client_info, _sess_ptr, _req = std::move(_req), _args = std::move(_args)] {
				logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

				http::response<http::string_body> res{http::status::ok, _req.version()};
				res.set(http::field::server, irods::http::version::server_name);
				res.set(http::field::content_type, "application/json");
				res.keep_alive(_req.keep_alive());

				try {
					DataObjInp input{};
					irods::at_scope_exit free_memory{[&input] { clearKeyVal(&input.condInput); }};

					if (const auto iter = _args.find("lpath"); iter != std::end(_args)) {
						irods::strncpy_null_terminated(input.objPath, iter->second.c_str());
					}
					else {
						logging::error(*_sess_ptr, "{}: Missing [lpath] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					if (const auto iter = _args.find("replica-number"); iter != std::end(_args)) {
						addKeyVal(&input.condInput, REPL_NUM_KW, iter->second.c_str());
					}
					else {
						logging::error(*_sess_ptr, "{}: Missing [replica-number] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					if (const auto iter = _args.find("catalog-only"); iter != std::end(_args) && iter->second == "1") {
						input.oprType = UNREG_OPR;
					}

					if (const auto iter = _args.find("admin"); iter != std::end(_args) && iter->second == "1") {
						addKeyVal(&input.condInput, ADMIN_KW, "");
					}

					addKeyVal(&input.condInput, COPIES_KW, "1");

					auto conn = irods::get_connection(client_info.username);
					const auto ec = rcDataObjTrim(static_cast<RcComm*>(conn), &input);

					res.body() = json{{"irods_response", {{"status_code", ec < 0 ? ec : 0}}}}.dump();
				}
				catch (const irods::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
					// clang-format off
					res.body() = json{
						{"irods_response", {
							{"status_code", e.code()},
							{"status_message", e.client_display_what()}
						}
					}}.dump();
					// clang-format on
				}
				catch (const std::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.what());
					res.result(http::status::internal_server_error);
				}

				res.prepare_payload();

				_sess_ptr->send(std::move(res));
			});
	} // op_trim

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_set_permission)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task([fn = __func__,
		                                       client_info,
		                                       _sess_ptr,
		                                       _req = std::move(_req),
		                                       _args = std::move(_args)] {
			logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

			http::response<http::string_body> res{http::status::ok, _req.version()};
			res.set(http::field::server, irods::http::version::server_name);
			res.set(http::field::content_type, "application/json");
			res.keep_alive(_req.keep_alive());

			try {
				const auto lpath_iter = _args.find("lpath");
				if (lpath_iter == std::end(_args)) {
					logging::error(*_sess_ptr, "{}: Missing [lpath] parameter.", fn);
					return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
				}

				auto conn = irods::get_connection(client_info.username);

				if (!fs::client::is_data_object(conn, lpath_iter->second)) {
					return _sess_ptr->send(irods::http::fail(
						res,
						http::status::bad_request,
						json{{"irods_response", {{"status_code", NOT_A_DATA_OBJECT}}}}.dump()));
				}

				const auto entity_name_iter = _args.find("entity-name");
				if (entity_name_iter == std::end(_args)) {
					logging::error(*_sess_ptr, "{}: Missing [entity-name] parameter.", fn);
					return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
				}

				const auto perm_iter = _args.find("permission");
				if (perm_iter == std::end(_args)) {
					logging::error(*_sess_ptr, "{}: Missing [permission] parameter.", fn);
					return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
				}

				const auto perm_enum = irods::to_permission_enum(perm_iter->second);
				if (!perm_enum) {
					logging::error(*_sess_ptr, "{}: Invalid value for [permission] parameter.", fn);
					return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
				}

				const auto admin_mode_iter = _args.find("admin");
				if (admin_mode_iter != std::end(_args) && admin_mode_iter->second == "1") {
					fs::client::permissions(fs::admin, conn, lpath_iter->second, entity_name_iter->second, *perm_enum);
				}
				else {
					fs::client::permissions(conn, lpath_iter->second, entity_name_iter->second, *perm_enum);
				}

				res.body() = json{
					{"irods_response",
				     {
						 {"status_code", 0},
					 }}}.dump();
			}
			catch (const fs::filesystem_error& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
				res.body() =
					json{{"irods_response", {{"status_code", e.code().value()}, {"status_message", e.what()}}}}.dump();
			}
			catch (const irods::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
				res.body() =
					json{{"irods_response", {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
						.dump();
			}
			catch (const std::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
				res.result(http::status::internal_server_error);
			}

			res.prepare_payload();

			_sess_ptr->send(std::move(res));
		});
	} // op_set_permission

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_modify_permissions)
	{
		using namespace irods::http::shared_api_operations;
		return op_atomic_apply_acl_operations(_sess_ptr, _req, _args, entity_type::data_object);
	} // op_modify_permissions

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_stat)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task([fn = __func__,
		                                       client_info,
		                                       _sess_ptr,
		                                       _req = std::move(_req),
		                                       _args = std::move(_args)] {
			logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

			http::response<http::string_body> res{http::status::ok, _req.version()};
			res.set(http::field::server, irods::http::version::server_name);
			res.set(http::field::content_type, "application/json");
			res.keep_alive(_req.keep_alive());

			try {
				const auto lpath_iter = _args.find("lpath");
				if (lpath_iter == std::end(_args)) {
					logging::error(*_sess_ptr, "{}: Missing [lpath] parameter.", fn);
					return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
				}

				auto conn = irods::get_connection(client_info.username);

				// Enable ticket if the request includes one.
				if (const auto iter = _args.find("ticket"); iter != std::end(_args)) {
					if (const auto ec = irods::enable_ticket(conn, iter->second); ec < 0) {
						res.result(http::status::internal_server_error);
						// clang-format off
						res.body() = json{
							{"irods_response", {
								{"status_code", ec},
								{"status_message", "Error enabling ticket on connection."}
							}}
						}.dump();
						// clang-format on
						res.prepare_payload();
						return _sess_ptr->send(std::move(res));
					}
				}

				const auto status = fs::client::status(conn, lpath_iter->second);

				if (!fs::client::is_data_object(status)) {
					res.body() = json{{"irods_response", {{"status_code", NOT_A_DATA_OBJECT}}}}.dump();
					res.prepare_payload();
					return _sess_ptr->send(std::move(res));
				}

				json perms;
				for (auto&& ep : status.permissions()) {
					perms.push_back(json{
						{"name", ep.name},
						{"zone", ep.zone},
						{"type", ep.type},
						{"perm", irods::to_permission_string(ep.prms)},
					});
				}

				// clang-format off
				res.body() = json{
					{"irods_response", {{"status_code", 0}}},
					{"type", irods::to_object_type_string(status.type())},
					{"permissions", perms},
					{"size", fs::client::data_object_size(conn, lpath_iter->second)},
					{"checksum", fs::client::data_object_checksum(conn, lpath_iter->second)},
					{"modified_at", fs::client::last_write_time(conn, lpath_iter->second).time_since_epoch().count()}
				}.dump();
				// clang-format on
			}
			catch (const fs::filesystem_error& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
				res.body() =
					json{{"irods_response", {{"status_code", e.code().value()}, {"status_message", e.what()}}}}.dump();
			}
			catch (const irods::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
				res.body() =
					json{{"irods_response", {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
						.dump();
			}
			catch (const std::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
				res.result(http::status::internal_server_error);
			}

			res.prepare_payload();

			_sess_ptr->send(std::move(res));
		});
	} // op_stat

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_register)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task(
			[fn = __func__, client_info, _sess_ptr, _req = std::move(_req), _args = std::move(_args)] {
				logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

				http::response<http::string_body> res{http::status::ok, _req.version()};
				res.set(http::field::server, irods::http::version::server_name);
				res.set(http::field::content_type, "application/json");
				res.keep_alive(_req.keep_alive());

				try {
					DataObjInp input{};
					irods::at_scope_exit free_memory{[&input] { clearKeyVal(&input.condInput); }};

					if (const auto lpath_iter = _args.find("lpath"); lpath_iter != std::end(_args)) {
						irods::strncpy_null_terminated(input.objPath, lpath_iter->second.c_str());
					}
					else {
						logging::error(*_sess_ptr, "{}: Missing [lpath] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					if (const auto iter = _args.find("ppath"); iter != std::end(_args)) {
						addKeyVal(&input.condInput, FILE_PATH_KW, iter->second.c_str());
					}
					else {
						logging::error(*_sess_ptr, "{}: Missing [ppath] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					if (const auto iter = _args.find("resource"); iter != std::end(_args)) {
						addKeyVal(&input.condInput, DEST_RESC_NAME_KW, iter->second.c_str());
					}
					else {
						logging::error(*_sess_ptr, "{}: Missing [resource] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					if (const auto iter = _args.find("as-additional-replica"); iter != std::end(_args)) {
						if (iter->second == "1") {
							addKeyVal(&input.condInput, REG_REPL_KW, "");
						}
					}

					if (const auto iter = _args.find("data-size"); iter != std::end(_args)) {
						addKeyVal(&input.condInput, DATA_SIZE_KW, iter->second.c_str());
					}

					if (const auto iter = _args.find("checksum"); iter != std::end(_args) && iter->second == "1") {
						addKeyVal(&input.condInput, REG_CHKSUM_KW, "");
					}

					if (const auto iter = _args.find("force"); iter != std::end(_args) && iter->second == "1") {
						addKeyVal(&input.condInput, FORCE_FLAG_KW, "");
					}

					auto conn = irods::get_connection(client_info.username);

					if (const auto ec = rcPhyPathReg(static_cast<RcComm*>(conn), &input); ec < 0) {
						res.body() = json{{"irods_response", {{"status_code", ec}}}}.dump();
						res.prepare_payload();
						return _sess_ptr->send(std::move(res));
					}

					res.body() = json{{"irods_response", {{"status_code", 0}}}}.dump();
				}
				catch (const irods::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
					// clang-format off
					res.body() = json{
						{"irods_response", {
							{"status_code", e.code()},
							{"status_message", e.client_display_what()}
						}}
					}.dump();
					// clang-format on
				}
				catch (const std::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.what());
					res.result(http::status::internal_server_error);
				}

				res.prepare_payload();

				_sess_ptr->send(std::move(res));
			});
	} // op_register

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_remove)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task([fn = __func__,
		                                       client_info,
		                                       _sess_ptr,
		                                       _req = std::move(_req),
		                                       _args = std::move(_args)] {
			logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

			http::response<http::string_body> res{http::status::ok, _req.version()};
			res.set(http::field::server, irods::http::version::server_name);
			res.set(http::field::content_type, "application/json");
			res.keep_alive(_req.keep_alive());

			try {
				const auto lpath_iter = _args.find("lpath");
				if (lpath_iter == std::end(_args)) {
					logging::error(*_sess_ptr, "{}: Missing [lpath] parameter.", fn);
					return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
				}

				const auto catalog_only_iter = _args.find("catalog-only");
				if (catalog_only_iter == std::end(_args)) {
					logging::error(*_sess_ptr, "{}: Missing [catalog-only] parameter.", fn);
					return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
				}

				DataObjInp input{};
				irods::at_scope_exit free_memory{[&input] { clearKeyVal(&input.condInput); }};

				irods::strncpy_null_terminated(input.objPath, lpath_iter->second.c_str());

				if (catalog_only_iter->second == "1") {
					input.oprType = UNREG_OPR;

					if (const auto iter = _args.find("no-trash"); iter != std::end(_args) && iter->second == "1") {
						logging::error(
							*_sess_ptr, "{}: [catalog-only] and [no-trash] parameters are incompatible.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}
				}
				else if (const auto iter = _args.find("no-trash"); iter != std::end(_args) && iter->second == "1") {
					addKeyVal(&input.condInput, FORCE_FLAG_KW, "");
				}

				if (const auto iter = _args.find("admin"); iter != std::end(_args) && iter->second == "1") {
					addKeyVal(&input.condInput, ADMIN_KW, "");
				}

				auto conn = irods::get_connection(client_info.username);
				const auto ec = rcDataObjUnlink(static_cast<RcComm*>(conn), &input);

				res.body() = json{{"irods_response", {{"status_code", ec}}}}.dump();
			}
			catch (const irods::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
				// clang-format off
				res.body() = json{
					{"irods_response", {
						{"status_code", e.code()},
						{"status_message", e.client_display_what()}
					}}
				}.dump();
				// clang-format on
			}
			catch (const std::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
				res.result(http::status::internal_server_error);
			}

			res.prepare_payload();

			_sess_ptr->send(std::move(res));
		});
	} // op_remove

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_rename)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task([fn = __func__,
		                                       client_info,
		                                       _sess_ptr,
		                                       _req = std::move(_req),
		                                       _args = std::move(_args)] {
			logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

			http::response<http::string_body> res{http::status::ok, _req.version()};
			res.set(http::field::server, irods::http::version::server_name);
			res.set(http::field::content_type, "application/json");
			res.keep_alive(_req.keep_alive());

			try {
				const auto old_lpath_iter = _args.find("old-lpath");
				if (old_lpath_iter == std::end(_args)) {
					logging::error(*_sess_ptr, "{}: Missing [old-lpath] parameter.", fn);
					return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
				}

				auto conn = irods::get_connection(client_info.username);

				if (!fs::client::is_data_object(conn, old_lpath_iter->second)) {
					return _sess_ptr->send(irods::http::fail(
						res,
						http::status::bad_request,
						json{{"irods_response", {{"status_code", NOT_A_DATA_OBJECT}}}}.dump()));
				}

				const auto new_lpath_iter = _args.find("new-lpath");
				if (new_lpath_iter == std::end(_args)) {
					logging::error(*_sess_ptr, "{}: Missing [new-lpath] parameter.", fn);
					return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
				}

				fs::client::rename(conn, old_lpath_iter->second, new_lpath_iter->second);

				res.body() = json{{"irods_response", {{"status_code", 0}}}}.dump();
			}
			catch (const fs::filesystem_error& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
				res.body() =
					json{{"irods_response", {{"status_code", e.code().value()}, {"status_message", e.what()}}}}.dump();
			}
			catch (const irods::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
				res.body() =
					json{{"irods_response", {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
						.dump();
			}
			catch (const std::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
				res.result(http::status::internal_server_error);
			}

			res.prepare_payload();

			_sess_ptr->send(std::move(res));
		});
	} // op_rename

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_copy)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task(
			[fn = __func__, client_info, _sess_ptr, _req = std::move(_req), _args = std::move(_args)] {
				logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

				http::response<http::string_body> res{http::status::ok, _req.version()};
				res.set(http::field::server, irods::http::version::server_name);
				res.set(http::field::content_type, "application/json");
				res.keep_alive(_req.keep_alive());

				try {
					const auto src_lpath_iter = _args.find("src-lpath");
					if (src_lpath_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [src-lpath] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					const auto dst_lpath_iter = _args.find("dst-lpath");
					if (dst_lpath_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [dst-lpath] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					dataObjCopyInp_t input{};

					irods::at_scope_exit free_memory{[&input] {
						clearKeyVal(&input.srcDataObjInp.condInput);
						clearKeyVal(&input.destDataObjInp.condInput);
					}};

					if (const auto iter = _args.find("src-resource"); iter != std::end(_args)) {
						addKeyVal(&input.srcDataObjInp.condInput, RESC_NAME_KW, iter->second.c_str());
					}

					if (const auto iter = _args.find("dst-resource"); iter != std::end(_args)) {
						addKeyVal(&input.destDataObjInp.condInput, DEST_RESC_NAME_KW, iter->second.c_str());
					}

					if (const auto iter = _args.find("overwrite"); iter != std::end(_args) && iter->second == "1") {
						addKeyVal(&input.destDataObjInp.condInput, FORCE_FLAG_KW, "");
					}

					const fs::path from = src_lpath_iter->second;
					const fs::path to = dst_lpath_iter->second;

					fs::throw_if_path_length_exceeds_limit(from);
					fs::throw_if_path_length_exceeds_limit(to);

					auto conn = irods::get_connection(client_info.username);

					if (!fs::client::is_data_object(conn, from)) {
						res.result(http::status::bad_request);
						res.body() = json{{"irods_response", {{"status_code", NOT_A_DATA_OBJECT}}}}.dump();
						return _sess_ptr->send(std::move(res));
					}

					irods::strncpy_null_terminated(input.srcDataObjInp.objPath, from.c_str());
					irods::strncpy_null_terminated(input.destDataObjInp.objPath, to.c_str());

					const auto ec = rcDataObjCopy(static_cast<RcComm*>(conn), &input);

					res.body() = json{{"irods_response", {{"status_code", ec}}}}.dump();
				}
				catch (const fs::filesystem_error& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.what());
					const auto ec = e.code().value();
					// clang-format off
					res.body() = json{
						{"irods_response", {
							{"status_code", ec == INVALID_OBJECT_TYPE ? NOT_A_DATA_OBJECT : ec},
							{"status_message", e.what()}
						}}
					}.dump();
					// clang-format on
				}
				catch (const irods::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
					// clang-format off
					res.body() = json{
						{"irods_response", {
							{"status_code", e.code()},
							{"status_message", e.client_display_what()}
						}}
					}.dump();
					// clang-format on
				}
				catch (const std::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.what());
					res.result(http::status::internal_server_error);
				}

				res.prepare_payload();

				_sess_ptr->send(std::move(res));
			});
	} // op_copy

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_touch)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task([fn = __func__,
		                                       client_info,
		                                       _sess_ptr,
		                                       _req = std::move(_req),
		                                       _args = std::move(_args)] {
			logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

			http::response<http::string_body> res{http::status::ok, _req.version()};
			res.set(http::field::server, irods::http::version::server_name);
			res.set(http::field::content_type, "application/json");
			res.keep_alive(_req.keep_alive());

			try {
				const auto lpath_iter = _args.find("lpath");
				if (lpath_iter == std::end(_args)) {
					logging::error(*_sess_ptr, "{}: Missing [lpath] parameter.", fn);
					return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
				}

				json::object_t options;

				auto opt_iter = _args.find("no-create");
				if (opt_iter != std::end(_args)) {
					options["no_create"] = (opt_iter->second == "1");
				}

				opt_iter = _args.find("replica-number");
				if (opt_iter != std::end(_args)) {
					try {
						options["replica_number"] = std::stoi(opt_iter->second);
					}
					catch (const std::exception& e) {
						logging::error(
							*_sess_ptr,
							"{}: Could not convert replica number [{}] into an integer.",
							fn,
							opt_iter->second);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}
				}

				opt_iter = _args.find("leaf-resource");
				if (opt_iter != std::end(_args)) {
					options["leaf_resource_name"] = opt_iter->second;
				}

				opt_iter = _args.find("seconds-since-epoch");
				if (opt_iter != std::end(_args)) {
					try {
						options["seconds_since_epoch"] = std::stoi(opt_iter->second);
					}
					catch (const std::exception& e) {
						logging::error(
							*_sess_ptr,
							"{}: Could not convert seconds-since-epoch [{}] into an integer.",
							fn,
							opt_iter->second);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}
				}

				opt_iter = _args.find("reference");
				if (opt_iter != std::end(_args)) {
					options["reference"] = opt_iter->second;
				}

				const json input{{"logical_path", lpath_iter->second}, {"options", options}};

				auto conn = irods::get_connection(client_info.username);

				const auto status = fs::client::status(conn, lpath_iter->second);

				if (fs::client::exists(status) && !fs::client::is_data_object(status)) {
					return _sess_ptr->send(irods::http::fail(
						res,
						http::status::bad_request,
						json{{"irods_response", {{"status_code", NOT_A_DATA_OBJECT}}}}.dump()));
				}

				const auto ec = rc_touch(static_cast<RcComm*>(conn), input.dump().c_str());

				res.body() = json{{"irods_response", {{"status_code", ec}}}}.dump();
			}
			catch (const irods::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
				res.body() =
					json{{"irods_response", {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
						.dump();
			}
			catch (const std::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
				res.result(http::status::internal_server_error);
			}

			res.prepare_payload();

			_sess_ptr->send(std::move(res));
		});
	} // op_touch

	// NOLINTNEXTLINE(performance-unnecessary-value-param)
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_truncate)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task(
			[fn = __func__, client_info, _sess_ptr, _req = std::move(_req), _args = std::move(_args)] {
				logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

				http::response<http::string_body> res{http::status::ok, _req.version()};
				res.set(http::field::server, irods::http::version::server_name);
				res.set(http::field::content_type, "application/json");
				res.keep_alive(_req.keep_alive());

				try {
					const auto lpath_iter = _args.find("lpath");
					if (lpath_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [lpath] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					const auto size_iter = _args.find("size");
					if (lpath_iter == std::end(_args)) {
						logging::error(*_sess_ptr, "{}: Missing [size] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					DataObjInp input{};
					lpath_iter->second.copy(input.objPath, sizeof(DataObjInp::objPath) - 1);

					try {
						input.dataSize = std::stoll(size_iter->second);
					}
					catch (const std::exception& e) {
						logging::error(
							*_sess_ptr, "{}: Could not convert size [{}] into an integer.", fn, size_iter->second);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					if (const auto iter = _args.find("resource"); iter != std::end(_args)) {
						addKeyVal(&input.condInput, RESC_NAME_KW, iter->second.c_str());
					}

					if (const auto iter = _args.find("replica-number"); iter != std::end(_args)) {
						addKeyVal(&input.condInput, REPL_NUM_KW, iter->second.c_str());
					}

					if (const auto iter = _args.find("admin"); iter != std::end(_args) && iter->second == "1") {
						addKeyVal(&input.condInput, ADMIN_KW, "");
					}

					auto conn = irods::get_connection(client_info.username);

					char* json_output{};
					// NOLINTNEXTLINE(cppcoreguidelines-no-malloc, *-owning-memory)
					const irods::at_scope_exit free_json_output{[&json_output] { std::free(json_output); }};

					const auto ec = rc_replica_truncate(static_cast<RcComm*>(conn), &input, &json_output);

					json response{{"irods_response", {{"status_code", ec}}}};
					if (json_output) {
						response["irods_response"]["additional_info"] = json::parse(json_output);
					}

					res.body() = response.dump();
				}
				catch (const irods::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
					// clang-format off
					res.body() = json{
						{"irods_response", {
							{"status_code", e.code()},
							{"status_message", e.client_display_what()}
						}}
					}.dump();
					// clang-format on
				}
				catch (const std::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.what());
					res.result(http::status::internal_server_error);
				}

				res.prepare_payload();

				_sess_ptr->send(std::move(res));
			});
	} // op_truncate

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_calculate_checksum)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task(
			[fn = __func__, client_info, _sess_ptr, _req = std::move(_req), _args = std::move(_args)] {
				logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

				http::response<http::string_body> res{http::status::ok, _req.version()};
				res.set(http::field::server, irods::http::version::server_name);
				res.set(http::field::content_type, "application/json");
				res.keep_alive(_req.keep_alive());

				try {
					DataObjInp input{};
					irods::at_scope_exit free_memory{[&input] { clearKeyVal(&input.condInput); }};

					if (const auto iter = _args.find("lpath"); iter != std::end(_args)) {
						irods::strncpy_null_terminated(input.objPath, iter->second.c_str());
					}
					else {
						logging::error(*_sess_ptr, "{}: Missing [lpath] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					if (const auto iter = _args.find("resource"); iter != std::end(_args)) {
						addKeyVal(&input.condInput, RESC_NAME_KW, iter->second.c_str());
					}

					if (const auto iter = _args.find("replica-number"); iter != std::end(_args)) {
						addKeyVal(&input.condInput, REPL_NUM_KW, iter->second.c_str());
					}

					if (const auto iter = _args.find("force"); iter != std::end(_args) && iter->second == "1") {
						addKeyVal(&input.condInput, FORCE_CHKSUM_KW, "");
					}

					if (const auto iter = _args.find("all"); iter != std::end(_args) && iter->second == "1") {
						addKeyVal(&input.condInput, CHKSUM_ALL_KW, "");
					}

					if (const auto iter = _args.find("admin"); iter != std::end(_args) && iter->second == "1") {
						addKeyVal(&input.condInput, ADMIN_KW, "");
					}

					char* checksum{};
					irods::at_scope_exit free_checksum{[&checksum] { std::free(checksum); }};

					auto conn = irods::get_connection(client_info.username);
					const auto ec = rcDataObjChksum(static_cast<RcComm*>(conn), &input, &checksum);

					if (ec < 0) {
						res.body() = json{{"irods_response", {{"status_code", ec}}}}.dump();
						res.prepare_payload();
						return _sess_ptr->send(std::move(res));
					}

					res.body() = json{{"irods_response", {{"status_code", ec}}}, {"checksum", checksum}}.dump();
				}
				catch (const irods::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
					// clang-format off
					res.body() = json{
						{"irods_response", {
							{"status_code", e.code()},
							{"status_message", e.client_display_what()}
						}}
					}.dump();
					// clang-format on
				}
				catch (const std::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.what());
					res.result(http::status::internal_server_error);
				}

				res.prepare_payload();

				_sess_ptr->send(std::move(res));
			});
	} // op_calculate_checksum

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_verify_checksum)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task(
			[fn = __func__, client_info, _sess_ptr, _req = std::move(_req), _args = std::move(_args)] {
				logging::info(*_sess_ptr, "{}: client_info.username = [{}]", fn, client_info.username);

				http::response<http::string_body> res{http::status::ok, _req.version()};
				res.set(http::field::server, irods::http::version::server_name);
				res.set(http::field::content_type, "application/json");
				res.keep_alive(_req.keep_alive());

				try {
					DataObjInp input{};
					irods::at_scope_exit free_memory{[&input] { clearKeyVal(&input.condInput); }};

					if (const auto iter = _args.find("lpath"); iter != std::end(_args)) {
						irods::strncpy_null_terminated(input.objPath, iter->second.c_str());
					}
					else {
						logging::error(*_sess_ptr, "{}: Missing [lpath] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					addKeyVal(&input.condInput, VERIFY_CHKSUM_KW, "");

					if (const auto iter = _args.find("resource"); iter != std::end(_args)) {
						addKeyVal(&input.condInput, RESC_NAME_KW, iter->second.c_str());
					}

					if (const auto iter = _args.find("replica-number"); iter != std::end(_args)) {
						addKeyVal(&input.condInput, REPL_NUM_KW, iter->second.c_str());
					}

					if (const auto iter = _args.find("compute-checksums");
				        iter != std::end(_args) && iter->second == "0") {
						addKeyVal(&input.condInput, NO_COMPUTE_KW, "");
					}

					if (const auto iter = _args.find("admin"); iter != std::end(_args) && iter->second == "1") {
						addKeyVal(&input.condInput, ADMIN_KW, "");
					}

					char* results{};
					irods::at_scope_exit free_results{[&results] { std::free(results); }};

					auto conn = irods::get_connection(client_info.username);
					const auto ec = rcDataObjChksum(static_cast<RcComm*>(conn), &input, &results);

					json response{{"irods_response", {{"status_code", ec}}}};

					if (ec < 0) {
						if (ec == CHECK_VERIFICATION_RESULTS) {
							response["results"] = json::parse(results);
						}
						else {
							res.body() = response.dump();
							res.prepare_payload();
							return _sess_ptr->send(std::move(res));
						}
					}

					if (auto* rerr_info = static_cast<RcComm*>(conn)->rError; rerr_info) {
						json error_info;

						for (auto&& err : std::span(rerr_info->errMsg, rerr_info->len)) {
							error_info.push_back(json{
								{"status", err->status},
								{"message", err->msg},
							});
						}

						response["r_error_info"] = error_info;
					}

					res.body() = response.dump();
				}
				catch (const irods::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
					res.body() = json{{"irods_response",
				                       {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
				                     .dump();
				}
				catch (const std::exception& e) {
					logging::error(*_sess_ptr, "{}: {}", fn, e.what());
					res.result(http::status::internal_server_error);
				}

				res.prepare_payload();

				_sess_ptr->send(std::move(res));
			});
	} // op_verify_checksum

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_modify_metadata)
	{
		using namespace irods::http::shared_api_operations;
		return op_atomic_apply_metadata_operations(_sess_ptr, _req, _args, entity_type::data_object);
	} // op_modify_metadata

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_modify_replica)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		irods::http::globals::background_task([fn = __func__,
		                                       client_info = std::move(result.client_info),
		                                       _sess_ptr,
		                                       _req = std::move(_req),
		                                       _args = std::move(_args)] {
			logging::info(*_sess_ptr, "{}: client_info->username = [{}]", fn, client_info.username);

			http::response<http::string_body> res{http::status::ok, _req.version()};
			res.set(http::field::server, irods::http::version::server_name);
			res.set(http::field::content_type, "application/json");
			res.keep_alive(_req.keep_alive());

			try {
				// clang-format off
				static constexpr auto properties = std::to_array<std::pair<const char*, const char*>>({
					{"new-data-checksum", CHKSUM_KW},
					{"new-data-comments", DATA_COMMENTS_KW},
					{"new-data-create-time", DATA_CREATE_KW},
					{"new-data-expiry", DATA_EXPIRY_KW},
					{"new-data-mode", DATA_MODE_KW},
					{"new-data-modify-time", DATA_MODIFY_KW},
					//{"new-data-owner-name", DATA_OWNER_KW},
					//{"new-data-owner-zone", DATA_OWNER_ZONE_KW},
					{"new-data-path", FILE_PATH_KW},
					{"new-data-replica-number", REPL_NUM_KW},
					{"new-data-replica-status", REPL_STATUS_KW},
					{"new-data-resource-id", RESC_ID_KW},
					{"new-data-size", DATA_SIZE_KW},
					{"new-data-status", STATUS_STRING_KW},
					{"new-data-type-name", DATA_TYPE_KW},
					{"new-data-version", VERSION_KW},
#ifdef IRODS_DEV_PACKAGE_IS_AT_LEAST_IRODS_5
					{"new-data-access-time", DATA_ACCESS_TIME_KW},
#else
					// This makes it possible for the HTTP API to support iRODS 5 functionality
					// without being compiled against the iRODS 5 development package.
					{"new-data-access-time", "dataAccessTime"}
#endif // IRODS_DEV_PACKAGE_IS_AT_LEAST_IRODS_5
				});
				// clang-format on

#ifdef IRODS_DEV_PACKAGE_IS_AT_LEAST_IRODS_5
				constexpr auto built_against_irods5_or_later = true;
#else
				constexpr auto built_against_irods5_or_later = false;
#endif // IRODS_DEV_PACKAGE_IS_AT_LEAST_IRODS_5

				auto conn = irods::get_connection(client_info.username);

				const auto connected_to_irods5_or_later = [&conn] {
					const auto version = irods::to_version(static_cast<RcComm*>(conn)->svrVersion->relVersion);
					return version && *version >= irods::version{4, 90, 0};
				}();

				// Access time is only supported by iRODS 5 and later. This is defined here so that we don't
				// need to define it in each block below.
				constexpr std::string_view atime_prop = "new-data-access-time";

				if ((built_against_irods5_or_later && connected_to_irods5_or_later) ||
				    (!built_against_irods5_or_later && !connected_to_irods5_or_later))
				{
					DataObjInfo info{};
					const irods::at_scope_exit free_memory{[&info] { clearKeyVal(&info.condInput); }};

					if (const auto iter = _args.find("lpath"); iter != std::end(_args)) {
						irods::strncpy_null_terminated(info.objPath, iter->second.c_str());
					}
					else {
						logging::error(*_sess_ptr, "{}: Missing [lpath] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					if (auto iter = _args.find("resource-hierarchy"); iter != std::end(_args)) {
						irods::strncpy_null_terminated(info.rescHier, iter->second.c_str());
					}
					else if (iter = _args.find("replica-number"); iter != std::end(_args)) {
						info.replNum = std::stoi(iter->second);
					}
					else {
						logging::error(
							*_sess_ptr, "{}: Missing [resource-hierarchy] or [replica-number] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					KeyValPair reg_params{};
					irods::experimental::key_value_proxy kvp{reg_params};
					const irods::at_scope_exit clear_kvp{[&kvp] { kvp.clear(); }};

					for (auto&& [external_pname, internal_pname] : properties) {
						if (const auto iter = _args.find(external_pname); iter != std::end(_args)) {
							// Return an error so that it's clear to the user that their request cannot be
							// satisfied and that they need to adjust their request. This makes it so that users
							// know what to expect when a property isn't supported by this operation.
							//
							// Consider what the user would experience if we allowed the operation to proceed
							// with properties which aren't supported.
							// - Case 1: Modifying the atime + other properties => [atime ignored, but success]
							// - Case 2: Modifying just the atime => [CAT_SQL_ERR]
							if (!connected_to_irods5_or_later && external_pname == atime_prop) {
								logging::error(*_sess_ptr, "{}: iRODS server does not support access time.", fn);
								return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
							}

							kvp[internal_pname] = iter->second;
						}
					}

					if (kvp.empty()) {
						logging::error(*_sess_ptr, "{}: No properties provided.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					// Setting this flag helps to enforce only rodsadmins are allowed to invoke
					// this endpoint operation.
					kvp[ADMIN_KW] = "";

					ModDataObjMetaInp input{};
					input.dataObjInfo = &info;
					input.regParam = &reg_params;

					const auto ec = rcModDataObjMeta(static_cast<RcComm*>(conn), &input);

					res.body() = json{{"irods_response", {{"status_code", ec}}}}.dump();
				}
				// TODO(#434): Remove this else-block once iRODS 4.3 is EOL.
				else if (!built_against_irods5_or_later && connected_to_irods5_or_later) {
					//
					// The following code exists to maintain compatibility between different runtimes.
					// It resolves problems stemming from a change in the packing instructions for rcModDataObjMeta.
					//

					// This data structure mirrors the iRODS 5.0.1 layout.
					struct Compatibility_DataObjInfo
					{
						// NOLINTBEGIN(*-avoid-c-arrays)
						char objPath[MAX_NAME_LEN];
						char rescName[NAME_LEN];
						char rescHier[MAX_NAME_LEN];
						char dataType[NAME_LEN];
						rodsLong_t dataSize;
						char chksum[NAME_LEN];
						char version[NAME_LEN];
						char filePath[MAX_NAME_LEN];
						char dataOwnerName[NAME_LEN];
						char dataOwnerZone[NAME_LEN];
						int replNum;
						int replStatus;
						char statusString[NAME_LEN];
						rodsLong_t dataId;
						rodsLong_t collId;
						int dataMapId;
						int flags;
						char dataComments[LONG_NAME_LEN];
						char dataMode[SHORT_STR_LEN];
						char dataExpiry[TIME_LEN];
						char dataCreate[TIME_LEN];
						char dataModify[TIME_LEN];
						char dataAccess[NAME_LEN];
						int dataAccessInx;
						int writeFlag;
						char destRescName[NAME_LEN];
						char backupRescName[NAME_LEN];
						char subPath[MAX_NAME_LEN];
						specColl_t* specColl;
						int regUid;
						int otherFlags;
						keyValPair_t condInput;
						char in_pdmo[MAX_NAME_LEN];
						Compatibility_DataObjInfo* next;
						rodsLong_t rescId;
						char dataAccessTime[TIME_LEN];
						// NOLINTEND(*-avoid-c-arrays)
					} info{};

					const irods::at_scope_exit free_memory{[&info] { clearKeyVal(&info.condInput); }};

					if (const auto iter = _args.find("lpath"); iter != std::end(_args)) {
						irods::strncpy_null_terminated(info.objPath, iter->second.c_str());
					}
					else {
						logging::error(*_sess_ptr, "{}: Missing [lpath] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					if (auto iter = _args.find("resource-hierarchy"); iter != std::end(_args)) {
						irods::strncpy_null_terminated(info.rescHier, iter->second.c_str());
					}
					else if (iter = _args.find("replica-number"); iter != std::end(_args)) {
						info.replNum = std::stoi(iter->second);
					}
					else {
						logging::error(
							*_sess_ptr, "{}: Missing [resource-hierarchy] or [replica-number] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					KeyValPair reg_params{};
					irods::experimental::key_value_proxy kvp{reg_params};
					const irods::at_scope_exit clear_kvp{[&kvp] { kvp.clear(); }};

					for (auto&& [external_pname, internal_pname] : properties) {
						if (const auto iter = _args.find(external_pname); iter != std::end(_args)) {
							kvp[internal_pname] = iter->second;
						}
					}

					if (kvp.empty()) {
						logging::error(*_sess_ptr, "{}: No properties provided.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					// Setting this flag helps to enforce only rodsadmins are allowed to invoke
					// this endpoint operation.
					kvp[ADMIN_KW] = "";

					struct Compatibility_ModDataObjMetaInp
					{
						Compatibility_DataObjInfo* dataObjInfo;
						KeyValPair* regParam;
					} input{};

					input.dataObjInfo = &info;
					input.regParam = &reg_params;

					const auto pi_table = std::to_array<PackingInstruction>({
						{"KeyValPair_PI", KeyValPair_PI, nullptr},
						{"DataObjInfo_PI",
					     "str objPath[MAX_NAME_LEN]; str rescName[NAME_LEN]; str rescHier[MAX_NAME_LEN]; str "
					     "dataType[NAME_LEN]; double dataSize; str chksum[NAME_LEN]; str version[NAME_LEN]; str "
					     "filePath[MAX_NAME_LEN]; str dataOwnerName[NAME_LEN]; str dataOwnerZone[NAME_LEN]; int "
					     "replNum; int replStatus; str statusString[NAME_LEN]; double dataId; double collId; int "
					     "dataMapId; int flags; str dataComments[LONG_NAME_LEN]; str dataMode[SHORT_STR_LEN]; str "
					     "dataExpiry[TIME_LEN]; str dataCreate[TIME_LEN]; str dataModify[TIME_LEN]; str "
					     "dataAccess[NAME_LEN]; int dataAccessInx; int writeFlag; str destRescName[NAME_LEN]; str "
					     "backupRescName[NAME_LEN]; str subPath[MAX_NAME_LEN]; int *specColl; int regUid; int "
					     "otherFlags; struct KeyValPair_PI; str in_pdmo[MAX_NAME_LEN]; int *next; double rescId; str "
					     "dataAccessTime[TIME_LEN];",
					     nullptr},
						{PACK_TABLE_END_PI, nullptr, nullptr},
					});

					const auto ec = irods::http::compatibility::procApiRequest(
						static_cast<RcComm*>(conn),
						MOD_DATA_OBJ_META_AN,
						"ModDataObjMeta_PI",
						pi_table.data(),
						&input,
						nullptr,
						static_cast<void**>(nullptr),
						nullptr);

					res.body() = json{{"irods_response", {{"status_code", ec}}}}.dump();
				}
				// TODO(#434): Remove this else-block once iRODS 4.3 is EOL.
				else if (built_against_irods5_or_later && !connected_to_irods5_or_later) {
					//
					// The following code exists to maintain compatibility between different runtimes.
					// It resolves problems stemming from a change in the packing instructions for rcModDataObjMeta.
					//

					// This data structure mirrors the iRODS 4.3.4 layout.
					struct Compatibility_DataObjInfo
					{
						// NOLINTBEGIN(*-avoid-c-arrays)
						char objPath[MAX_NAME_LEN];
						char rescName[NAME_LEN];
						char rescHier[MAX_NAME_LEN];
						char dataType[NAME_LEN];
						rodsLong_t dataSize;
						char chksum[NAME_LEN];
						char version[NAME_LEN];
						char filePath[MAX_NAME_LEN];
						char dataOwnerName[NAME_LEN];
						char dataOwnerZone[NAME_LEN];
						int replNum;
						int replStatus;
						char statusString[NAME_LEN];
						rodsLong_t dataId;
						rodsLong_t collId;
						int dataMapId;
						int flags;
						char dataComments[LONG_NAME_LEN];
						char dataMode[SHORT_STR_LEN];
						char dataExpiry[TIME_LEN];
						char dataCreate[TIME_LEN];
						char dataModify[TIME_LEN];
						char dataAccess[NAME_LEN];
						int dataAccessInx;
						int writeFlag;
						char destRescName[NAME_LEN];
						char backupRescName[NAME_LEN];
						char subPath[MAX_NAME_LEN];
						specColl_t* specColl;
						int regUid;
						int otherFlags;
						keyValPair_t condInput;
						char in_pdmo[MAX_NAME_LEN];
						Compatibility_DataObjInfo* next;
						rodsLong_t rescId;
						// NOLINTEND(*-avoid-c-arrays)
					} info{};

					const irods::at_scope_exit free_memory{[&info] { clearKeyVal(&info.condInput); }};

					if (const auto iter = _args.find("lpath"); iter != std::end(_args)) {
						irods::strncpy_null_terminated(info.objPath, iter->second.c_str());
					}
					else {
						logging::error(*_sess_ptr, "{}: Missing [lpath] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					if (auto iter = _args.find("resource-hierarchy"); iter != std::end(_args)) {
						irods::strncpy_null_terminated(info.rescHier, iter->second.c_str());
					}
					else if (iter = _args.find("replica-number"); iter != std::end(_args)) {
						info.replNum = std::stoi(iter->second);
					}
					else {
						logging::error(
							*_sess_ptr, "{}: Missing [resource-hierarchy] or [replica-number] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					KeyValPair reg_params{};
					irods::experimental::key_value_proxy kvp{reg_params};
					const irods::at_scope_exit clear_kvp{[&kvp] { kvp.clear(); }};

					for (auto&& [external_pname, internal_pname] : properties) {
						if (const auto iter = _args.find(external_pname); iter != std::end(_args)) {
							// Return an error so that it's clear to the user that their request cannot be
							// satisfied and that they need to adjust their request. This makes it so that users
							// know what to expect when a property isn't supported by this operation.
							//
							// Consider what the user would experience if we allowed the operation to proceed
							// with properties which aren't supported.
							// - Case 1: Modifying the atime + other properties => [atime ignored, but success]
							// - Case 2: Modifying just the atime => [CAT_SQL_ERR]
							if (external_pname == atime_prop) {
								logging::error(*_sess_ptr, "{}: iRODS server does not support access time.", fn);
								return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
							}

							kvp[internal_pname] = iter->second;
						}
					}

					if (kvp.empty()) {
						logging::error(*_sess_ptr, "{}: No properties provided.", fn);
						return _sess_ptr->send(irods::http::fail(res, http::status::bad_request));
					}

					// Setting this flag helps to enforce only rodsadmins are allowed to invoke
					// this endpoint operation.
					kvp[ADMIN_KW] = "";

					struct Compatibility_ModDataObjMetaInp
					{
						Compatibility_DataObjInfo* dataObjInfo;
						KeyValPair* regParam;
					} input{};

					input.dataObjInfo = &info;
					input.regParam = &reg_params;

					const auto pi_table = std::to_array<PackingInstruction>({
						{"KeyValPair_PI", KeyValPair_PI, nullptr},
						{"DataObjInfo_PI",
					     "str objPath[MAX_NAME_LEN]; str rescName[NAME_LEN]; str rescHier[MAX_NAME_LEN]; str "
					     "dataType[NAME_LEN]; double dataSize; str chksum[NAME_LEN]; str version[NAME_LEN]; str "
					     "filePath[MAX_NAME_LEN]; str dataOwnerName[NAME_LEN]; str dataOwnerZone[NAME_LEN]; int "
					     "replNum; int replStatus; str statusString[NAME_LEN]; double dataId; double collId; int "
					     "dataMapId; int flags; str dataComments[LONG_NAME_LEN]; str dataMode[SHORT_STR_LEN]; str "
					     "dataExpiry[TIME_LEN]; str dataCreate[TIME_LEN]; str dataModify[TIME_LEN]; str "
					     "dataAccess[NAME_LEN]; int dataAccessInx; int writeFlag; str destRescName[NAME_LEN]; str "
					     "backupRescName[NAME_LEN]; str subPath[MAX_NAME_LEN]; int *specColl; int regUid; int "
					     "otherFlags; struct KeyValPair_PI; str in_pdmo[MAX_NAME_LEN]; int *next; double rescId;",
					     nullptr},
						{PACK_TABLE_END_PI, nullptr, nullptr},
					});

					const auto ec = irods::http::compatibility::procApiRequest(
						static_cast<RcComm*>(conn),
						MOD_DATA_OBJ_META_AN,
						"ModDataObjMeta_PI",
						pi_table.data(),
						&input,
						nullptr,
						static_cast<void**>(nullptr),
						nullptr);

					res.body() = json{{"irods_response", {{"status_code", ec}}}}.dump();
				}
			}
			catch (const irods::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.client_display_what());
				// clang-format off
				res.body() = json{
					{"irods_response", {
						{"status_code", e.code()},
						{"status_message", e.client_display_what()}
					}}
				}.dump();
				// clang-format on
			}
			catch (const std::exception& e) {
				logging::error(*_sess_ptr, "{}: {}", fn, e.what());
				res.result(http::status::internal_server_error);
			}

			res.prepare_payload();

			_sess_ptr->send(std::move(res));
		});
	} // op_modify_replica
} // anonymous namespace

namespace irods::http::endpoint_operation
{
	// This operation is a more efficient version of the original "write" operation provided by the
	// /data-objects endpoint. It requires a special code path in session.cpp because it can only be
	// triggered through the use of HTTP headers.
	//
	// The main benefit of this alternative implementation is that it leads to improved memory usage
	// and network usage. Clients will most certainly prefer this style of writing to a data object
	// because it can also lead to faster data transfers.
	auto op_write_streaming(irods::http::session_pointer_type _sess_ptr) -> void
	{
		const auto& req = _sess_ptr->parser()->get();

		auto result = irods::http::resolve_client_identity(req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		logging::info(*_sess_ptr, "{}: client_info.username = [{}]", __func__, client_info.username);

		::http::response<::http::string_body> res{::http::status::ok, req.version()};
		res.set(::http::field::server, irods::http::version::server_name);
		res.set(::http::field::content_type, "application/json");
		res.keep_alive(req.keep_alive());

		try {
			// Used to determine whether the data object should be closed following the write
			// operation or by the parallel_write_shutdown HTTP API operation.
			bool is_parallel_write = false;

			irods::http::connection_facade conn;
			std::unique_ptr<io::client::native_transport> tp;

			std::unique_ptr<io::odstream> out;
			io::odstream* out_ptr{};

			const auto& headers = req.base();
			const auto parallel_write_handle_iter = headers.find("irods-api-request-parallel-write-handle");

			using at_scope_exit_type = irods::at_scope_exit<std::function<void()>>;
			std::unique_ptr<at_scope_exit_type> mark_pw_stream_as_usable;

			if (parallel_write_handle_iter != std::end(headers)) {
				logging::debug(
					*_sess_ptr,
					"{}: (write) Parallel Write Handle = [{}].",
					__func__,
					parallel_write_handle_iter->value());

				decltype(parallel_write_contexts)::iterator iter;

				{
					std::shared_lock lk{pwc_mtx};

					iter = parallel_write_contexts.find(parallel_write_handle_iter->value());
					if (iter == std::end(parallel_write_contexts)) {
						logging::error(*_sess_ptr, "{}: Invalid handle for parallel write.", __func__);
						return _sess_ptr->send(irods::http::fail(res, ::http::status::bad_request));
					}
				}

				//
				// We've found a matching handle!
				//

				is_parallel_write = true;

				if (const auto stream_index_iter = headers.find("irods-api-request-stream-index");
				    stream_index_iter != std::end(headers))
				{
					logging::debug(
						*_sess_ptr,
						"{}: Client selected [{}] for [stream-index] parameter.",
						__func__,
						stream_index_iter->value());

					try {
						const auto sindex = std::stoi(stream_index_iter->value());
						out_ptr = &iter->second.streams.at(sindex)->stream();
					}
					catch (const std::exception& e) {
						logging::error(*_sess_ptr, "{}: Invalid argument for [stream-index] parameter.", __func__);
						return _sess_ptr->send(irods::http::fail(res, ::http::status::bad_request));
					}
				}
				else {
					auto* pw_stream = iter->second.find_available_parallel_write_stream();
					if (!pw_stream) {
						logging::error(
							*_sess_ptr,
							"{}: Parallel write streams are busy. Client must wait for one to become available.",
							__func__);
						return _sess_ptr->send(irods::http::fail(res, ::http::status::too_many_requests));
					}

					mark_pw_stream_as_usable =
						std::make_unique<at_scope_exit_type>([pw_stream] { pw_stream->in_use(false); });

					out_ptr = &pw_stream->stream();
				}

				logging::debug(
					*_sess_ptr,
					"{}: (write) Parallel Write - stream memory address = [{}].",
					__func__,
					fmt::ptr(out_ptr));
			}
			else {
				const auto lpath_iter = headers.find("irods-api-request-lpath");
				if (lpath_iter == std::end(headers)) {
					logging::error(*_sess_ptr, "{}: Missing [lpath] parameter.", __func__);
					return _sess_ptr->send(irods::http::fail(res, ::http::status::bad_request));
				}

				auto openmode = std::ios_base::out;

				if (const auto iter = headers.find("irods-api-request-truncate");
				    iter != std::end(headers) && iter->value() == "0")
				{
					openmode |= std::ios_base::in;
				}

				if (const auto iter = headers.find("irods-api-request-append");
				    iter != std::end(headers) && iter->value() == "1")
				{
					openmode |= std::ios_base::app;
				}

				logging::trace(*_sess_ptr, "{}: Opening data object [{}] for write.", __func__, lpath_iter->value());
				logging::trace(*_sess_ptr, "{}: (write) Initializing for single buffer write.", __func__);

				conn = irods::get_connection(client_info.username);

				// Enable ticket if the request includes one.
				if (const auto iter = headers.find("irods-api-request-ticket"); iter != std::end(headers)) {
					if (const auto ec = irods::enable_ticket(conn, iter->value()); ec < 0) {
						res.result(::http::status::internal_server_error);
						res.body() =
							json{{"irods_response",
						          {{"status_code", ec}, {"status_message", "Error enabling ticket on connection."}}}}
								.dump();
						res.prepare_payload();
						return _sess_ptr->send(std::move(res));
					}
				}

				tp = std::make_unique<io::client::native_transport>(conn);

				if (const auto iter = headers.find("irods-api-request-resource"); iter != std::end(headers)) {
					out = std::make_unique<io::odstream>(
						*tp,
						std::string{lpath_iter->value()},
						io::root_resource_name{std::string{iter->value()}},
						openmode);
				}
				else if (const auto iter = headers.find("irods-api-request-replica-number"); iter != std::end(headers))
				{
					int value = -1;
					try {
						value = std::stoi(iter->value());
					}
					catch (const std::exception& e) {
						logging::error(
							*_sess_ptr,
							"{}: Could not convert replica number [{}] to integer.",
							__func__,
							iter->value());
						res.result(::http::status::bad_request);
						res.prepare_payload();
						return _sess_ptr->send(std::move(res));
					}

					out = std::make_unique<io::odstream>(
						*tp, std::string{lpath_iter->value()}, io::replica_number{value}, openmode);
				}
				else {
					out = std::make_unique<io::odstream>(*tp, std::string{lpath_iter->value()}, openmode);
				}

				out_ptr = out.get();
			}

			if (!*out_ptr) {
				logging::error(*_sess_ptr, "{}: Output stream to data object is in a bad state.", __func__);
				// clang-format off
				res.body() = json{
					{"irods_response", {
						{"status_code", INVALID_HANDLE},
						{"status_message", "Output stream to data object is in a bad state."}
					}}
				}.dump();
				// clang-format on
				res.prepare_payload();
				return _sess_ptr->send(std::move(res));
			}

			auto close_output_stream_if_not_parallel_write_stream = [&is_parallel_write, out_ptr] {
				// If we're performing a normal write, close the stream before returning a response.
				// This is required so that the iRODS server triggers appropriate policy before handing
				// back control to the client. For example, replication resources and synchronous replication.
				if (!is_parallel_write) {
					out_ptr->close();
				}
			};

			auto iter = headers.find("irods-api-request-offset");
			if (iter != std::end(headers)) {
				logging::trace(*_sess_ptr, "{}: Setting offset for write.", __func__);
				try {
					out_ptr->seekp(std::stoll(iter->value()));
				}
				catch (const std::exception& e) {
					logging::error(
						*_sess_ptr, "{}: Could not seek to position [{}] in data object.", __func__, iter->value());
					close_output_stream_if_not_parallel_write_stream();
					res.result(::http::status::bad_request);
					res.prepare_payload();
					return _sess_ptr->send(std::move(res));
				}

				if (!*out_ptr) {
					logging::error(*_sess_ptr, "{}: Output stream to data object is in a bad state.", __func__);
					close_output_stream_if_not_parallel_write_stream();
					// clang-format off
					res.body() = json{
						{"irods_response", {
							{"status_code", INVALID_HANDLE},
							{"status_message", "Output stream to data object is in a bad state."}
						}}
					}.dump();
					// clang-format on
					res.prepare_payload();
					return _sess_ptr->send(std::move(res));
				}
			}

			static const auto max_number_of_bytes_per_write =
				irods::http::globals::configuration()
					.at(json::json_pointer{"/irods_client/max_number_of_bytes_per_write_operation"})
					.get<std::int64_t>();

			// clang-format off
			std::make_shared<streaming_write>(
				_sess_ptr,
				req.version(),
				req.keep_alive(),
				std::move(conn),
				std::move(tp),
				std::move(out),
				out_ptr,
				std::move(mark_pw_stream_as_usable),
				max_number_of_bytes_per_write,
				is_parallel_write)->start();
			// clang-format off
		}
		catch (const fs::filesystem_error& e) {
			logging::error(*_sess_ptr, "{}: {}", __func__, e.what());
			// clang-format off
			res.body() = json{
				{"irods_response", {
					{"status_code", e.code().value()},
					{"status_message", e.what()}
				}}
			}.dump();
			// clang-format on
			res.prepare_payload();
			_sess_ptr->send(std::move(res));
		}
		catch (const irods::exception& e) {
			logging::error(*_sess_ptr, "{}: {}", __func__, e.client_display_what());
			// clang-format off
			res.body() = json{
				{"irods_response", {
					{"status_code", e.code()},
					{"status_message", e.client_display_what()}
				}}
			}.dump();
			// clang-format on
			res.prepare_payload();
			_sess_ptr->send(std::move(res));
		}
		catch (const std::exception& e) {
			logging::error(*_sess_ptr, "{}: {}", __func__, e.what());
			res.result(::http::status::internal_server_error);
			res.prepare_payload();
			_sess_ptr->send(std::move(res));
		}
	} // op_write_streaming
} // namespace irods::http::endpoint_operation
