#include "irods/private/http_api/compatibility.hpp"

#include "irods/private/http_api/log.hpp"

#include <irods/generalAdmin.h>
#include <irods/irods_client_api_table.hpp>
#include <irods/irods_exception.hpp>
#include <irods/irods_network_factory.hpp>
#include <irods/irods_query.hpp>
#include <irods/irods_threads.hpp>
#include <irods/rcGlobalExtern.h>
#include <irods/rcMisc.h>
#include <irods/rodsErrorTable.h>
#include <irods/sockComm.h>
#include <irods/sockCommNetworkInterface.hpp>
#include <irods/userAdmin.h>
#include <irods/user_administration.hpp>

namespace
{
	namespace adm = irods::experimental::administration;
	namespace logging = irods::http::log;

	auto send_api_request(
		rcComm_t* conn,
		int apiInx,
		const char* _packingInstruction,
		const PackingInstruction* _packingInstructionTable,
		const void* inputStruct,
		const bytesBuf_t* inputBsBBuf) -> int
	{
		int status = 0;
		bytesBuf_t* inputStructBBuf = nullptr;
		bytesBuf_t* myInputStructBBuf = nullptr;

		cliChkReconnAtSendStart(conn);

		irods::api_entry_table& RcApiTable = irods::get_client_api_table();
		auto itr = RcApiTable.find(apiInx);

		if (itr == RcApiTable.end()) {
			logging::error("{}: Could not find API entry matching at index [{}].", __func__, apiInx);
			return SYS_UNMATCHED_API_NUM;
		}

		if (itr->second->inPackInstruct != nullptr) {
			if (inputStruct == nullptr) {
				cliChkReconnAtSendEnd(conn);
				return USER_API_INPUT_ERR;
			}
			status = pack_struct(
				inputStruct,
				&inputStructBBuf,
				_packingInstruction,
				_packingInstructionTable,
				0,
				conn->irodsProt,
				conn->svrVersion->relVersion);
			if (status < 0) {
				logging::error("{}: Packing instruction error; error_code=[{}]", __func__, status);
				cliChkReconnAtSendEnd(conn);
				return status;
			}

			myInputStructBBuf = inputStructBBuf;
		}
		else {
			myInputStructBBuf = nullptr;
		}

		if (itr->second->inBsFlag <= 0) {
			inputBsBBuf = nullptr;
		}

		irods::network_object_ptr net_obj;
		irods::error ret = irods::network_factory(conn, net_obj);
		if (!ret.ok()) {
			freeBBuf(inputStructBBuf);
			logging::error("{}: network_factory error: [{}], error_code=[{}]", __func__, ret.user_result(), ret.code());
			return static_cast<int>(ret.code());
		}

		ret = sendRodsMsg(
			net_obj, RODS_API_REQ_T, myInputStructBBuf, inputBsBBuf, nullptr, itr->second->apiNumber, conn->irodsProt);
		if (!ret.ok()) {
			logging::error("{}: sendRodsMsg error: [{}].", __func__, ret.code());
			if (conn->svrVersion != nullptr && conn->svrVersion->reconnPort > 0) {
				const auto savedStatus = static_cast<int>(ret.code());
				conn->thread_ctx->lock->lock();
				int status1 = cliSwitchConnect(conn);
				logging::debug(
					"{}: cliSwitchConnect error: [{}]; clientState=[{}], agentState=[{}]",
					__func__,
					status1,
					conn->clientState,
					conn->agentState);
				conn->thread_ctx->lock->unlock();
				if (status1 > 0) {
					// Should not be here.
					logging::info("{}: Switch connection and retry sendRodsMsg.", __func__);
					ret = sendRodsMsg(
						net_obj,
						RODS_API_REQ_T,
						myInputStructBBuf,
						inputBsBBuf,
						nullptr,
						itr->second->apiNumber,
						conn->irodsProt);
					if (!ret.ok()) {
						logging::error("{}: {}; error_code=[{}].", __func__, ret.user_result(), ret.code());
					}
					else {
						status = savedStatus;
					}
				} // if status1 > 0
			} // if svrVersion != nullptr ...
		}
		else {
			// be sure to pass along the return code from the
			// plugin call
			status = static_cast<int>(ret.code());
		}

		freeBBuf(inputStructBBuf);

		return status;
	}
} // anonymous namespace

namespace irods::http::compatibility
{
	auto procApiRequest(
		rcComm_t* _comm,
		int _apiNumber,
		const char* _packingInstruction,
		const PackingInstruction* _packingInstructionTable,
		const void* _inputStruct,
		const bytesBuf_t* _inputBsBBuf,
		void** _outStruct,
		bytesBuf_t* _outBsBBuf) -> int
	{
		if (!_comm) {
			return USER__NULL_INPUT_ERR;
		}

		freeRError(_comm->rError);
		_comm->rError = nullptr;

		const int apiInx = apiTableLookup(_apiNumber);

		if (apiInx < 0) {
			return apiInx;
		}

		if (const auto ec = send_api_request(
				_comm, apiInx, _packingInstruction, _packingInstructionTable, _inputStruct, _inputBsBBuf);
		    ec < 0)
		{
			return ec;
		}

		_comm->apiInx = apiInx;

		return readAndProcApiReply(_comm, apiInx, _outStruct, _outBsBBuf);
	} // procApiRequest

	auto add_group(bool _irods_server_supports_group_keyword, RcComm& _comm, const adm::group& _group) -> void
	{
		// get_local_zone()'s implementation is private to the user_administration library.
		// We copy its client-side implementation here to get around that.
		const auto zone = [&_comm] {
			for (auto&& row : irods::query{&_comm, "select ZONE_NAME where ZONE_TYPE = 'local'"}) {
				return row[0];
			}

			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
			THROW(CAT_NO_ROWS_FOUND, "Cannot get local zone name.");
		}();

		const adm::user current_user{_comm.clientUser.userName, _comm.clientUser.rodsZone};
		const auto user_type = adm::client::type(_comm, current_user);

		if (user_type == irods::experimental::administration::user_type::groupadmin) {
			// NOLINTBEGIN(*-avoid-c-arrays)
			char arg0[] = "mkgroup";
			char arg2[] = "rodsgroup";
			// NOLINTEND(*-avoid-c-arrays)

			userAdminInp_t input{};
			input.arg0 = arg0;
			// NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast))
			input.arg1 = const_cast<char*>(_group.name.data());
			input.arg2 = arg2;
			// NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast))
			input.arg3 = const_cast<char*>(zone.data());

			if (const auto ec = rxUserAdmin(&_comm, &input); ec != 0) {
				// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
				THROW(ec, fmt::format("Failed to add group [{}].", _group.name));
			}

			return;
		}

		GeneralAdminInput input{};
		input.arg0 = "add";
		input.arg2 = _group.name.data();
		input.arg4 = zone.data();

		if (_irods_server_supports_group_keyword) {
			input.arg1 = "group";
		}
		else {
			input.arg1 = "user";
			input.arg3 = "rodsgroup";
		}

		if (const auto ec = rxGeneralAdmin(&_comm, &input); ec != 0) {
			// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
			THROW(ec, fmt::format("Failed to add group [{}].", _group.name));
		}
	} // add_group
} // namespace irods::http::compatibility
