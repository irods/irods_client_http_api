#ifndef IRODS_HTTP_API_COMPATIBILITY_HPP
#define IRODS_HTTP_API_COMPATIBILITY_HPP

#include <irods/packStruct.h>
#include <irods/rcConnect.h>
#include <irods/rodsDef.h>

namespace irods::http::compatibility
{
	// A custom implementation of procApiRequest which allows the caller to
	// control which packing instruction is used. This implementation is designed
	// to help with compatibility issues stemming from changes in packing instructions.
	auto procApiRequest(
		rcComm_t* _comm,
		int _apiNumber,
		const char* _packingInstruction,
		const PackingInstruction* _packingInstructionTable,
		const void* _inputStruct,
		const bytesBuf_t* _inputBsBBuf,
		void** _outStruct,
		bytesBuf_t* _outBsBBuf) -> int;
} // namespace irods::http::compatibility

#endif // IRODS_HTTP_API_COMPATIBILITY_HPP
