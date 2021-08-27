/***********************************************************************************************************************************
TLS Client
***********************************************************************************************************************************/
#include "build.auto.h"

#include <strings.h>

#include "common/crypto/common.h"
#include "common/debug.h"
#include "common/log.h"
#include "common/io/client.h"
#include "common/io/io.h"
#include "common/io/tls/client.h"
#include "common/io/tls/common.h"
#include "common/io/tls/session.h"
#include "common/memContext.h"
#include "common/stat.h"
#include "common/type/object.h"
#include "common/wait.h"

/***********************************************************************************************************************************
Statistics constants
***********************************************************************************************************************************/
STRING_EXTERN(TLS_STAT_CLIENT_STR,                                  TLS_STAT_CLIENT);
STRING_EXTERN(TLS_STAT_RETRY_STR,                                   TLS_STAT_RETRY);
STRING_EXTERN(TLS_STAT_SESSION_STR,                                 TLS_STAT_SESSION);

/***********************************************************************************************************************************
Object type
***********************************************************************************************************************************/
typedef struct TlsClient
{
    MemContext *memContext;                                         // Mem context
    const String *host;                                             // Host to use for peer verification
    TimeMSec timeout;                                               // Timeout for any i/o operation (connect, read, etc.)
    bool verifyPeer;                                                // Should the peer (server) certificate be verified?
    IoClient *ioClient;                                             // Underlying client (usually a SocketClient)

    SSL_CTX *context;                                               // TLS context
} TlsClient;

/***********************************************************************************************************************************
Macros for function logging
***********************************************************************************************************************************/
static String *
tlsClientToLog(const THIS_VOID)
{
    THIS(const TlsClient);

    return strNewFmt(
        "{ioClient: %s, timeout: %" PRIu64", verifyPeer: %s}",
        memContextFreeing(this->memContext) ? NULL_Z : strZ(ioClientToLog(this->ioClient)), this->timeout,
        cvtBoolToConstZ(this->verifyPeer));
}

#define FUNCTION_LOG_TLS_CLIENT_TYPE                                                                                               \
    TlsClient *
#define FUNCTION_LOG_TLS_CLIENT_FORMAT(value, buffer, bufferSize)                                                                  \
    FUNCTION_LOG_STRING_OBJECT_FORMAT(value, tlsClientToLog, buffer, bufferSize)

/***********************************************************************************************************************************
Free connection
***********************************************************************************************************************************/
static void
tlsClientFreeResource(THIS_VOID)
{
    THIS(TlsClient);

    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(TLS_CLIENT, this);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);

    SSL_CTX_free(this->context);

    FUNCTION_LOG_RETURN_VOID();
}

/***********************************************************************************************************************************
Check if a name from the server certificate matches the hostname

Matching is always case-insensitive since DNS is case insensitive.
***********************************************************************************************************************************/
static bool
tlsClientHostVerifyName(const String *host, const String *name)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(STRING, host);
        FUNCTION_LOG_PARAM(STRING, name);
    FUNCTION_LOG_END();

    ASSERT(host != NULL);
    ASSERT(name != NULL);

    // Check for NULLs in the name
    tlsCertificateNameVerify(name);

    bool result = false;

    // Try an exact match
    if (strcasecmp(strZ(name), strZ(host)) == 0)                                                                    // {vm_covered}
    {
        result = true;                                                                                              // {vm_covered}
    }
    // Else check if a wildcard certificate matches the host name
    //
    // The rules are:
    // 1. Only match the '*' character as wildcard
    // 2. Only match wildcards at the start of the string
    // 3. The '*' character does *not* match '.', meaning that we match only a single pathname component.
    // 4. Don't support more than one '*' in a single pattern.
    //
    // This is roughly in line with RFC2818, but contrary to what most browsers appear to be implementing (point 3 being the
    // difference)
    else if (strZ(name)[0] == '*' && strZ(name)[1] == '.' && strSize(name) > 2 &&                                   // {vm_covered}
             strSize(name) < strSize(host) &&                                                                       // {vm_covered}
             strcasecmp(strZ(name) + 1, strZ(host) + strSize(host) - strSize(name) + 1) == 0 &&                     // {vm_covered}
             strChr(host, '.') >= (int)(strSize(host) - strSize(name)))                                             // {vm_covered}
    {
        result = true;                                                                                              // {vm_covered}
    }

    FUNCTION_LOG_RETURN(BOOL, result);
}

/***********************************************************************************************************************************
Verify that the server certificate matches the hostname we connected to

The certificate's Common Name and Subject Alternative Names are considered.
***********************************************************************************************************************************/
static bool
tlsClientHostVerify(const String *host, X509 *certificate)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(STRING, host);
        FUNCTION_LOG_PARAM_P(VOID, certificate);
    FUNCTION_LOG_END();

    ASSERT(host != NULL);

    bool result = false;

    // Error if the certificate is NULL
    if (certificate == NULL)                                                                                        // {vm_covered}
        THROW(CryptoError, "No certificate presented by the TLS server");                                           // {vm_covered}

    MEM_CONTEXT_TEMP_BEGIN()                                                                                        // {vm_covered}
    {
        // First get the subject alternative names from the certificate and compare them against the hostname
        STACK_OF(GENERAL_NAME) *altNameStack = (STACK_OF(GENERAL_NAME) *)X509_get_ext_d2i(                          // {vm_covered}
            certificate, NID_subject_alt_name, NULL, NULL);                                                         // {vm_covered}
        bool altNameFound = false;                                                                                  // {vm_covered}

        if (altNameStack)                                                                                           // {vm_covered}
        {
            for (int altNameIdx = 0; altNameIdx < sk_GENERAL_NAME_num(altNameStack); altNameIdx++)                  // {vm_covered}
            {
                const GENERAL_NAME *name = sk_GENERAL_NAME_value(altNameStack, altNameIdx);                         // {vm_covered}
                altNameFound = true;                                                                                // {vm_covered}

                if (name->type == GEN_DNS)                                                                          // {vm_covered}
                    result = tlsClientHostVerifyName(host, tlsAsn1ToStr(name->d.dNSName));                          // {vm_covered}

                if (result != false)                                                                                // {vm_covered}
                    break;                                                                                          // {vm_covered}
            }

            sk_GENERAL_NAME_pop_free(altNameStack, GENERAL_NAME_free);                                              // {vm_covered}
        }

        // If no subject alternative name was found then check the common name. Per RFC 2818 and RFC 6125, if the subjectAltName
        // extension of type dNSName is present the CN must be ignored.
        if (!altNameFound)                                                                                          // {vm_covered}
            result = tlsClientHostVerifyName(host, tlsCertificateCommonName(certificate));                          // {vm_covered}
    }
    MEM_CONTEXT_TEMP_END();                                                                                         // {vm_covered}

    FUNCTION_LOG_RETURN(BOOL, result);                                                                              // {vm_covered}
}

/***********************************************************************************************************************************
!!!INIT STEPS FOR CLIENT CERT PULLED FROM src/interfaces/libpq/fe-secure-openssl.c initialize_SSL()
***********************************************************************************************************************************/
// static int
// tlsClientPwd(char *buf, int size, int rwflag, void *userdata)
// {
//     (void)buf;
//     (void)size;
//     (void)rwflag;
//     (void)userdata;
// }

static void
tlsClientInit(const TlsClient *const this, SSL *const tlsSession)
{
    FUNCTION_LOG_BEGIN(logLevelTrace)
        FUNCTION_LOG_PARAM(TLS_CLIENT, this);
        FUNCTION_LOG_PARAM_P(VOID, tlsSession);
    FUNCTION_LOG_END();

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // !!! 1------------------------------------------------------------
        /* THIS WILL GENERATE AN ERROR
        * Delegate the client cert password prompt to the libpq wrapper callback
        * if any is defined.
        *
        * If the application hasn't installed its own and the sslpassword
        * parameter is non-null, we install ours now to make sure we supply
        * PGconn->sslpassword to OpenSSL instead of letting it prompt on stdin.
        *
        * This will replace OpenSSL's default PEM_def_callback (which prompts on
        * stdin), but we're only setting it for this SSL context so it's
        * harmless.
        */
        // if (PQsslKeyPassHook
        //     || (conn->sslpassword && strlen(conn->sslpassword) > 0))
        // {
        //     SSL_CTX_set_default_passwd_cb(SSL_context, PQssl_passwd_cb);
        //     SSL_CTX_set_default_passwd_cb_userdata(SSL_context, conn);
        // }

        // Set server host name used for validation
        cryptoError(SSL_set_tlsext_host_name(tlsSession, strZ(this->host)) != 1, "unable to set TLS host name");

        // !!! 9------------------------------------------------------------
        // !!! NOT CLEAR IF THIS IS NEEDED SINCE IT JUST RETURNS OK IN PG CODE
        /*
        * If a root cert was loaded, also set our certificate verification
        * callback.
        */
        // if (have_rootcert)
        //     SSL_set_verify(conn->ssl, SSL_VERIFY_PEER, verify_cb);
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}

/***********************************************************************************************************************************
!!!AUTH STEPS FOR CLIENT CERT PULLED FROM src/interfaces/libpq/fe-secure-openssl.c open_client_SSL()
***********************************************************************************************************************************/
static bool
tlsClientAuth(const TlsClient *const this, SSL *const tlsSession)
{
    FUNCTION_LOG_BEGIN(logLevelTrace)
        FUNCTION_LOG_PARAM(TLS_CLIENT, this);
        FUNCTION_LOG_PARAM_P(VOID, tlsSession);
    FUNCTION_LOG_END();

    bool result = false;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Verify that the certificate presented by the server is valid
        if (this->verifyPeer)                                                                                       // {vm_covered}
        {
            // Verify that the chain of trust leads to a valid CA
            long int verifyResult = SSL_get_verify_result(tlsSession);                                              // {vm_covered}

            if (verifyResult != X509_V_OK)                                                                          // {vm_covered}
            {
                THROW_FMT(                                                                                          // {vm_covered}
                    CryptoError, "unable to verify certificate presented by '%s': [%ld] %s",                        // {vm_covered}
                    strZ(ioClientName(this->ioClient)), verifyResult,                                               // {vm_covered}
                    X509_verify_cert_error_string(verifyResult));                                                   // {vm_covered}
            }

            // Verify that the hostname appears in the certificate
            X509 *certificate = SSL_get_peer_certificate(tlsSession);                                               // {vm_covered}
            bool nameResult = tlsClientHostVerify(this->host, certificate);                                         // {vm_covered}
            X509_free(certificate);                                                                                 // {vm_covered}

            if (!nameResult)                                                                                        // {vm_covered}
            {
                THROW_FMT(                                                                                          // {vm_covered}
                    CryptoError,                                                                                    // {vm_covered}
                    "unable to find hostname '%s' in certificate common name or subject alternative names",         // {vm_covered}
                    strZ(this->host));                                                                              // {vm_covered}
            }
        }

        result = true;
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(BOOL, result);
}

/***********************************************************************************************************************************
Open TLS session on a socket
***********************************************************************************************************************************/
static IoSession *
tlsClientOpen(THIS_VOID)
{
    THIS(TlsClient);

    FUNCTION_LOG_BEGIN(logLevelTrace)
        FUNCTION_LOG_PARAM(TLS_CLIENT, this);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);

    IoSession *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        bool retry;
        Wait *wait = waitNew(this->timeout);
        SSL *tlsSession = NULL;

        do
        {
            // Assume there will be no retry
            retry = false;

            // Create the TLS session
            tlsSession = SSL_new(this->context);
            cryptoError(tlsSession == NULL, "unable to create TLS session");

            // Initialize TLS session
            TRY_BEGIN()
            {
                tlsClientInit(this, tlsSession);
            }
            CATCH_ANY()
            {
                SSL_free(tlsSession);                                                   // {uncovered - !!! add test}
                RETHROW();                                                              // {uncovered - !!! add test}
            }
            TRY_END();

            // Open TLS session
            TRY_BEGIN()
            {
                // Open the underlying session first since this is mostly likely to fail
                IoSession *ioSession = NULL;

                TRY_BEGIN()
                {
                    ioSession = ioClientOpen(this->ioClient);
                }
                CATCH_ANY()
                {
                    SSL_free(tlsSession);
                    RETHROW();
                }
                TRY_END();

                // Open session
                result = tlsSessionNew(tlsSession, ioSession, this->timeout);
            }
            CATCH_ANY()
            {
                result = NULL;

                // Retry if wait time has not expired
                if (waitMore(wait))
                {
                    LOG_DEBUG_FMT("retry %s: %s", errorTypeName(errorType()), errorMessage());
                    retry = true;

                    statInc(TLS_STAT_RETRY_STR);
                }
                else
                    RETHROW();
            }
            TRY_END();
        }
        while (retry);

        // Authenticate TLS session
        ioSessionAuthenticatedSet(result, tlsClientAuth(this, tlsSession));

        // Move session
        ioSessionMove(result, memContextPrior());
    }
    MEM_CONTEXT_TEMP_END();

    statInc(TLS_STAT_SESSION_STR);

    FUNCTION_LOG_RETURN(IO_SESSION, result);
}

/**********************************************************************************************************************************/
static const String *
tlsClientName(THIS_VOID)
{
    THIS(TlsClient);

    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(TLS_CLIENT, this);
    FUNCTION_TEST_END();

    ASSERT(this != NULL);

    FUNCTION_TEST_RETURN(ioClientName(this->ioClient));
}

/**********************************************************************************************************************************/
static const IoClientInterface tlsClientInterface =
{
    .type = IO_CLIENT_TLS_TYPE,
    .name = tlsClientName,
    .open = tlsClientOpen,
    .toLog = tlsClientToLog,
};

IoClient *
tlsClientNew(
    IoClient *const ioClient, const String *const host, const TimeMSec timeout, const bool verifyPeer, const String *const caFile,
    const String *const caPath, const String *const cert, const String *const key, const String *const crlFile)
{
    FUNCTION_LOG_BEGIN(logLevelDebug)
        FUNCTION_LOG_PARAM(IO_CLIENT, ioClient);
        FUNCTION_LOG_PARAM(STRING, host);
        FUNCTION_LOG_PARAM(TIME_MSEC, timeout);
        FUNCTION_LOG_PARAM(BOOL, verifyPeer);
        FUNCTION_LOG_PARAM(STRING, caFile);
        FUNCTION_LOG_PARAM(STRING, caPath);
        FUNCTION_LOG_PARAM(STRING, cert);
        FUNCTION_LOG_PARAM(STRING, key);
        FUNCTION_LOG_PARAM(STRING, crlFile);
    FUNCTION_LOG_END();

    ASSERT(ioClient != NULL);
    ASSERT((cert == NULL && key == NULL) || (cert != NULL && key != NULL));

    IoClient *this = NULL;

    MEM_CONTEXT_NEW_BEGIN("TlsClient")
    {
        TlsClient *driver = memNew(sizeof(TlsClient));

        *driver = (TlsClient)
        {
            .memContext = MEM_CONTEXT_NEW(),
            .ioClient = ioClientMove(ioClient, MEM_CONTEXT_NEW()),
            .host = strDup(host),
            .timeout = timeout,
            .verifyPeer = verifyPeer,
        };

        // Setup TLS context
        // -------------------------------------------------------------------------------------------------------------------------
        cryptoInit();

        // Select the TLS method to use.  To maintain compatibility with older versions of OpenSSL we need to use an SSL method,
        // but SSL versions will be excluded in SSL_CTX_set_options().
        const SSL_METHOD *method = SSLv23_method();
        cryptoError(method == NULL, "unable to load TLS method");

        // Create the TLS context
        driver->context = SSL_CTX_new(method);
        cryptoError(driver->context == NULL, "unable to create TLS context");

        memContextCallbackSet(driver->memContext, tlsClientFreeResource, driver);

        // Exclude SSL versions to only allow TLS and also disable compression
        SSL_CTX_set_options(driver->context, (long)(SSL_OP_ALL | SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION));

        // Disable auto-retry to prevent SSL_read() from hanging
        SSL_CTX_clear_mode(driver->context, SSL_MODE_AUTO_RETRY);

        // Set location of CA certificates if the server certificate will be verified
        // -------------------------------------------------------------------------------------------------------------------------
        if (driver->verifyPeer)
        {
            // If the user specified a location
            if (caFile != NULL || caPath != NULL)                                                                   // {vm_covered}
            {
                cryptoError(                                                                                        // {vm_covered}
                    SSL_CTX_load_verify_locations(driver->context, strZNull(caFile), strZNull(caPath)) != 1,        // {vm_covered}
                    "unable to set user-defined CA certificate location");                                          // {vm_covered}
            }
            // Else use the defaults
            else
            {
                cryptoError(
                    SSL_CTX_set_default_verify_paths(driver->context) != 1, "unable to set default CA certificate location");
            }
        }

        // !!! Load certificate revocation list
        // -------------------------------------------------------------------------------------------------------------------------
        // if (crlFile != NULL)
        // {
        //     // Get cert store
    	// 	X509_STORE *const certStore = SSL_CTX_get_cert_store(driver->context);
        //     cryptoError(certStore == NULL, "unable to get cert store");

        //     // Load CRL file
        //     cryptoError(
        //         X509_STORE_load_locations(certStore, strZ(crlFile), NULL) != 1,
        //         strZ(strNewFmt("unable to load crl file '%s'", strZ(crlFile))));

        //     // Set flags to reject certs in CRL
        //     cryptoError(
        //         X509_STORE_set_flags(certStore, X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL) != 1, "unable to set crl flags");
        // }

        // Load certificate and key if specified
        // -------------------------------------------------------------------------------------------------------------------------
        if (cert != NULL)
        {
            // Load certificate
            cryptoError(
                SSL_CTX_use_certificate_chain_file(driver->context, strZ(cert)) != 1,
                strZ(strNewFmt("unable to load cert '%s'", strZ(cert))));

            // !!! Key engines are not supported (yet)

            // !!! Check that key has the correct permissions
            // if (stat(fnbuf, &buf) != 0)
            // {
            //     appendPQExpBuffer(&conn->errorMessage,
            //                       libpq_gettext("certificate present, but not private key file \"%s\"\n"),
            //                       fnbuf);
            //     return -1;
            // }
            // #ifndef WIN32
            // if (!S_ISREG(buf.st_mode) || buf.st_mode & (S_IRWXG | S_IRWXO))
            // {
            //     appendPQExpBuffer(&conn->errorMessage,
            //                       libpq_gettext("private key file \"%s\" has group or world access; permissions should be u=rw (0600) or less\n"),
            //                       fnbuf);
            //     return -1;
            // }
            // #endif

            // Load key and verify that the key and cert go together
            cryptoError(
                SSL_CTX_use_PrivateKey_file(driver->context, strZ(key), SSL_FILETYPE_PEM) != 1,
                strZ(strNewFmt("unable to load key '%s'", strZ(key))));

            // Verify again that the cert and key go together. It is not clear why this is needed since the key has already been
            // verified in SSL_CTX_use_PrivateKey_file(), but it may be that older versions of OpenSSL need it.
            // !!! TRY ON POSTGRES AND SEE WHAT HAPPENS?
            cryptoError(
                SSL_CTX_check_private_key(driver->context) != 1,
                strZ(strNewFmt("cert '%s' and key '%s' do not match", strZ(cert), strZ(key))));
        }

        statInc(TLS_STAT_CLIENT_STR);

        // Create client interface
        this = ioClientNew(driver, &tlsClientInterface);
    }
    MEM_CONTEXT_NEW_END();

    FUNCTION_LOG_RETURN(IO_CLIENT, this);
}
