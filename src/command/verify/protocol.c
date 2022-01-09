/***********************************************************************************************************************************
Verify Protocol Handler
***********************************************************************************************************************************/
#include "build.auto.h"

#include "command/verify/file.h"
#include "command/verify/protocol.h"
#include "common/debug.h"
#include "common/io/io.h"
#include "common/log.h"
#include "common/memContext.h"
#include "config/config.h"
#include "storage/helper.h"

/**********************************************************************************************************************************/
void
verifyFileProtocol(PackRead *const param, ProtocolServer *const server)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(PACK_READ, param);
        FUNCTION_LOG_PARAM(PROTOCOL_SERVER, server);
    FUNCTION_LOG_END();

    ASSERT(param != NULL);
    ASSERT(server != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Verify file
        const String *const filePathName = pckReadStrP(param);
        const uint64_t bundleId = pckReadU64P(param);
        const uint64_t bundleOffset = pckReadU64P(param);
        const uint64_t bundleSize = pckReadU64P(param);
        const String *const fileChecksum = pckReadStrP(param);
        const uint64_t fileSize = pckReadU64P(param);
        const String *const cipherPass = pckReadStrP(param);

        const VerifyResult result = verifyFile(
            filePathName, bundleId, bundleOffset, bundleSize, fileChecksum, fileSize, cipherPass);

        // Return result
        protocolServerDataPut(server, pckWriteU32P(protocolPackNew(), result));
        protocolServerDataEndPut(server);
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}
