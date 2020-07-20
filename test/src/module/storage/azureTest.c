/***********************************************************************************************************************************
Test Azure Storage
***********************************************************************************************************************************/
#include "common/io/handleRead.h"
#include "common/io/handleWrite.h"

#include "common/harnessConfig.h"
#include "common/harnessFork.h"
#include "common/harnessStorage.h"
#include "common/harnessTls.h"

/***********************************************************************************************************************************
Constants
***********************************************************************************************************************************/
#define TEST_ACCOUNT                                                "account"
    STRING_STATIC(TEST_ACCOUNT_STR,                                 TEST_ACCOUNT);
#define TEST_CONTAINER                                              "container"
    STRING_STATIC(TEST_CONTAINER_STR,                               TEST_CONTAINER);
#define TEST_KEY_SAS                                                "?sig=key"
    STRING_STATIC(TEST_KEY_SAS_STR,                                 TEST_KEY_SAS);
#define TEST_KEY_SHARED                                             "YXpLZXk="
    STRING_STATIC(TEST_KEY_SHARED_STR,                              TEST_KEY_SHARED);

/***********************************************************************************************************************************
Helper to build test requests
***********************************************************************************************************************************/
static StorageAzure *driver;

typedef struct TestRequestParam
{
    VAR_PARAM_HEADER;
    const char *content;
    const char *blobType;
} TestRequestParam;

#define testRequestP(verb, uri, ...)                                                                                               \
    testRequest(verb, uri, (TestRequestParam){VAR_PARAM_INIT, __VA_ARGS__})

static void
testRequest(const char *verb, const char *uri, TestRequestParam param)
{
    String *request = strNewFmt("%s /" TEST_ACCOUNT "/" TEST_CONTAINER, verb);

    // When SAS spit out the query and merge in the SAS key
    if (driver->sasKey != NULL)
    {
        HttpQuery *query = httpQueryNewP();
        StringList *uriQuery = strLstNewSplitZ(STR(uri), "?");

        if (strLstSize(uriQuery) == 2)
            query = httpQueryNewStr(strLstGet(uriQuery, 1));

        httpQueryMerge(query, driver->sasKey);

        strCat(request, strLstGet(uriQuery, 0));
        strCatZ(request, "?");
        strCat(request, httpQueryRenderP(query));
    }
    // Else just output URI as is
    else
        strCatZ(request, uri);

    // Add HTTP version
    strCatZ(request, " HTTP/1.1\r\n");

    // Add authorization string
    if (driver->sharedKey != NULL)
        strCatZ(request, "authorization:SharedKey account:????????????????????????????????????????????\r\n");

    // Add content-length
    strCatFmt(request, "content-length:%zu\r\n", param.content == NULL ? 0 : strlen(param.content));

    // Add md5
    if (param.content != NULL)
    {
        char md5Hash[HASH_TYPE_MD5_SIZE_HEX];
        encodeToStr(encodeBase64, bufPtr(cryptoHashOne(HASH_TYPE_MD5_STR, BUFSTRZ(param.content))), HASH_TYPE_M5_SIZE, md5Hash);
        strCatFmt(request, "content-md5:%s\r\n", md5Hash);
    }

    // Add date
    if (driver->sharedKey != NULL)
        strCatZ(request, "date:???, ?? ??? ???? ??:??:?? GMT\r\n");

    // Add host
    strCatFmt(request, "host:%s\r\n", strPtr(hrnTlsServerHost()));

    // Add blob type
    if (param.blobType != NULL)
        strCatFmt(request, "x-ms-blob-type:%s\r\n", param.blobType);

    // Add version
    if (driver->sharedKey != NULL)
        strCatZ(request, "x-ms-version:2019-02-02\r\n");

    // Complete headers
    strCatZ(request, "\r\n");

    // Add content
    if (param.content != NULL)
        strCatZ(request, param.content);

    hrnTlsServerExpect(request);
}

/***********************************************************************************************************************************
Helper to build test responses
***********************************************************************************************************************************/
typedef struct TestResponseParam
{
    VAR_PARAM_HEADER;
    unsigned int code;
    const char *header;
    const char *content;
} TestResponseParam;

#define testResponseP(...)                                                                                                         \
    testResponse((TestResponseParam){VAR_PARAM_INIT, __VA_ARGS__})

static void
testResponse(TestResponseParam param)
{
    // Set code to 200 if not specified
    param.code = param.code == 0 ? 200 : param.code;

    // Output header and code
    String *response = strNewFmt("HTTP/1.1 %u ", param.code);

    // Add reason for some codes
    switch (param.code)
    {
        case 200:
        {
            strCatZ(response, "OK");
            break;
        }

        case 403:
        {
            strCatZ(response, "Forbidden");
            break;
        }
    }

    // End header
    strCatZ(response, "\r\n");

    // Headers
    if (param.header != NULL)
        strCatFmt(response, "%s\r\n", param.header);

    // Content
    if (param.content != NULL)
    {
        strCatFmt(
            response,
            "content-length:%zu\r\n"
                "\r\n"
                "%s",
            strlen(param.content), param.content);
    }
    else
        strCatZ(response, "\r\n");

    hrnTlsServerReply(response);
}

/***********************************************************************************************************************************
Test Run
***********************************************************************************************************************************/
void
testRun(void)
{
    FUNCTION_HARNESS_VOID();

    // *****************************************************************************************************************************
    if (testBegin("storageRepoGet()"))
    {
        // Test without the host option since that can't be run in a unit test without updating dns or /etc/hosts
        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("storage with default options");

        StringList *argList = strLstNew();
        strLstAddZ(argList, "--" CFGOPT_STANZA "=test");
        strLstAddZ(argList, "--" CFGOPT_REPO1_TYPE "=" STORAGE_AZURE_TYPE);
        strLstAddZ(argList, "--" CFGOPT_REPO1_PATH "=/repo");
        strLstAddZ(argList, "--" CFGOPT_REPO1_AZURE_CONTAINER "=" TEST_CONTAINER);
        setenv("PGBACKREST_" CFGOPT_REPO1_AZURE_ACCOUNT, TEST_ACCOUNT, true);
        setenv("PGBACKREST_" CFGOPT_REPO1_AZURE_KEY, TEST_KEY_SHARED, true);
        harnessCfgLoad(cfgCmdArchivePush, argList);

        Storage *storage = NULL;
        TEST_ASSIGN(storage, storageRepoGet(strNew(STORAGE_AZURE_TYPE), false), "get repo storage");
        TEST_RESULT_STR_Z(storage->path, "/repo", "    check path");
        TEST_RESULT_STR(((StorageAzure *)storage->driver)->account, TEST_ACCOUNT_STR, "    check account");
        TEST_RESULT_STR(((StorageAzure *)storage->driver)->container, TEST_CONTAINER_STR, "    check container");
        TEST_RESULT_STR(((StorageAzure *)storage->driver)->sharedKey, TEST_KEY_SHARED_STR, "    check key");
        TEST_RESULT_STR_Z(((StorageAzure *)storage->driver)->host, TEST_ACCOUNT ".blob.core.windows.net", "    check host");
        TEST_RESULT_STR_Z(((StorageAzure *)storage->driver)->uriPrefix, "/" TEST_CONTAINER, "    check uri prefix");
        TEST_RESULT_UINT(((StorageAzure *)storage->driver)->blockSize, STORAGE_AZURE_BLOCKSIZE_MIN, "    check block size");
        TEST_RESULT_BOOL(storageFeature(storage, storageFeaturePath), false, "    check path feature");
        TEST_RESULT_BOOL(storageFeature(storage, storageFeatureCompress), false, "    check compress feature");
    }

    // *****************************************************************************************************************************
    if (testBegin("storageAzureAuth()"))
    {
        StorageAzure *storage = NULL;
        HttpHeader *header = NULL;
        const String *dateTime = STRDEF("Sun, 21 Jun 2020 12:46:19 GMT");

        TEST_ASSIGN(
            storage,
            (StorageAzure *)storageDriver(
                storageAzureNew(
                    STRDEF("/repo"), false, NULL, TEST_CONTAINER_STR, TEST_ACCOUNT_STR, storageAzureKeyTypeShared,
                    TEST_KEY_SHARED_STR, 16, NULL, 443, 1000, true, NULL, NULL)),
            "new azure storage - shared key");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("minimal auth");

        header = httpHeaderAdd(httpHeaderNew(NULL), HTTP_HEADER_CONTENT_LENGTH_STR, ZERO_STR);

        TEST_RESULT_VOID(storageAzureAuth(storage, HTTP_VERB_GET_STR, STRDEF("/path"), NULL, dateTime, header), "auth");
        TEST_RESULT_STR_Z(
            httpHeaderToLog(header),
            "{authorization: 'SharedKey account:edqgT7EhsiIN3q6Al2HCZlpXr2D5cJFavr2ZCkhG9R8=', content-length: '0'"
                ", date: 'Sun, 21 Jun 2020 12:46:19 GMT', host: 'account.blob.core.windows.net', x-ms-version: '2019-02-02'}",
            "check headers");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("auth with md5 and query");

        header = httpHeaderAdd(httpHeaderNew(NULL), HTTP_HEADER_CONTENT_LENGTH_STR, STRDEF("44"));
        httpHeaderAdd(header, HTTP_HEADER_CONTENT_MD5_STR, STRDEF("b64f49553d5c441652e95697a2c5949e"));

        HttpQuery *query = httpQueryAdd(httpQueryNewP(), STRDEF("a"), STRDEF("b"));

        TEST_RESULT_VOID(storageAzureAuth(storage, HTTP_VERB_GET_STR, STRDEF("/path/file"), query, dateTime, header), "auth");
        TEST_RESULT_STR_Z(
            httpHeaderToLog(header),
            "{authorization: 'SharedKey account:5qAnroLtbY8IWqObx8+UVwIUysXujsfWZZav7PrBON0=', content-length: '44'"
                ", content-md5: 'b64f49553d5c441652e95697a2c5949e', date: 'Sun, 21 Jun 2020 12:46:19 GMT'"
                ", host: 'account.blob.core.windows.net', x-ms-version: '2019-02-02'}",
            "check headers");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("SAS auth");

        TEST_ASSIGN(
            storage,
            (StorageAzure *)storageDriver(
                storageAzureNew(
                    STRDEF("/repo"), false, NULL, TEST_CONTAINER_STR, TEST_ACCOUNT_STR, storageAzureKeyTypeSas, TEST_KEY_SAS_STR,
                    16, NULL, 443, 1000, true, NULL, NULL)),
            "new azure storage - sas key");

        query = httpQueryAdd(httpQueryNewP(), STRDEF("a"), STRDEF("b"));
        header = httpHeaderAdd(httpHeaderNew(NULL), HTTP_HEADER_CONTENT_LENGTH_STR, STRDEF("66"));

        TEST_RESULT_VOID(storageAzureAuth(storage, HTTP_VERB_GET_STR, STRDEF("/path/file"), query, dateTime, header), "auth");
        TEST_RESULT_STR_Z(
            httpHeaderToLog(header), "{content-length: '66', host: 'account.blob.core.windows.net'}", "check headers");
        TEST_RESULT_STR_Z(httpQueryRenderP(query), "a=b&sig=key", "check query");
    }

    // *****************************************************************************************************************************
    if (testBegin("StorageAzure, StorageReadAzure, and StorageWriteAzure"))
    {
        HARNESS_FORK_BEGIN()
        {
            HARNESS_FORK_CHILD_BEGIN(0, true)
            {
                TEST_RESULT_VOID(
                    hrnTlsServerRun(ioHandleReadNew(strNew("azure server read"), HARNESS_FORK_CHILD_READ(), 5000)),
                    "azure server begin");
            }
            HARNESS_FORK_CHILD_END();

            HARNESS_FORK_PARENT_BEGIN()
            {
                hrnTlsClientBegin(ioHandleWriteNew(strNew("azure client write"), HARNESS_FORK_PARENT_WRITE_PROCESS(0)));

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("test against local host");

                StringList *argList = strLstNew();
                strLstAddZ(argList, "--" CFGOPT_STANZA "=test");
                strLstAddZ(argList, "--" CFGOPT_REPO1_TYPE "=" STORAGE_AZURE_TYPE);
                strLstAddZ(argList, "--" CFGOPT_REPO1_PATH "=/");
                strLstAddZ(argList, "--" CFGOPT_REPO1_AZURE_CONTAINER "=" TEST_CONTAINER);
                strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_AZURE_HOST "=%s", strPtr(hrnTlsServerHost())));
                strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_AZURE_PORT "=%u", hrnTlsServerPort()));
                strLstAdd(argList, strNewFmt("--%s" CFGOPT_REPO1_AZURE_VERIFY_TLS, testContainer() ? "" : "no-"));
                setenv("PGBACKREST_" CFGOPT_REPO1_AZURE_ACCOUNT, TEST_ACCOUNT, true);
                setenv("PGBACKREST_" CFGOPT_REPO1_AZURE_KEY, TEST_KEY_SHARED, true);
                harnessCfgLoad(cfgCmdArchivePush, argList);

                Storage *storage = NULL;
                TEST_ASSIGN(storage, storageRepoGet(strNew(STORAGE_AZURE_TYPE), true), "get repo storage");

                driver = (StorageAzure *)storage->driver;
                TEST_RESULT_STR(driver->host, hrnTlsServerHost(), "    check host");
                TEST_RESULT_STR_Z(driver->uriPrefix,  "/" TEST_ACCOUNT "/" TEST_CONTAINER, "    check uri prefix");
                TEST_RESULT_BOOL(driver->fileId == 0, false, "    check file id");

                // Tests need the block size to be 16
                driver->blockSize = 16;

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("ignore missing file");

                hrnTlsServerAccept();
                testRequestP(HTTP_VERB_GET, "/fi%26le.txt");
                testResponseP(.code = 404);

                TEST_RESULT_PTR(
                    storageGetP(storageNewReadP(storage, strNew("fi&le.txt"), .ignoreMissing = true)), NULL, "get file");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("error on missing file");

                testRequestP(HTTP_VERB_GET, "/file.txt");
                testResponseP(.code = 404);

                TEST_ERROR(
                    storageGetP(storageNewReadP(storage, strNew("file.txt"))), FileMissingError,
                    "unable to open '/file.txt': No such file or directory");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("get file");

                testRequestP(HTTP_VERB_GET, "/file.txt");
                testResponseP(.content = "this is a sample file");

                TEST_RESULT_STR_Z(
                    strNewBuf(storageGetP(storageNewReadP(storage, strNew("file.txt")))), "this is a sample file", "get file");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("get zero-length file");

                testRequestP(HTTP_VERB_GET, "/file0.txt");
                testResponseP();

                TEST_RESULT_STR_Z(
                    strNewBuf(storageGetP(storageNewReadP(storage, strNew("file0.txt")))), "", "get zero-length file");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("non-404 error");

                testRequestP(HTTP_VERB_GET, "/file.txt");
                testResponseP(.code = 303, .content = "CONTENT");

                StorageRead *read = NULL;
                TEST_ASSIGN(read, storageNewReadP(storage, strNew("file.txt"), .ignoreMissing = true), "new read file");
                TEST_RESULT_BOOL(storageReadIgnoreMissing(read), true, "    check ignore missing");
                TEST_RESULT_STR_Z(storageReadName(read), "/file.txt", "    check name");

                TEST_ERROR_FMT(
                    ioReadOpen(storageReadIo(read)), ProtocolError,
                    "HTTP request failed with 303:\n"
                    "*** URI/Query ***:\n"
                    "/account/container/file.txt\n"
                    "*** Request Headers ***:\n"
                    "authorization: <redacted>\n"
                    "content-length: 0\n"
                    "date: <redacted>\n"
                    "host: %s\n"
                    "x-ms-version: 2019-02-02\n"
                    "*** Response Headers ***:\n"
                    "content-length: 7\n"
                    "*** Response Content ***:\n"
                    "CONTENT",
                    strPtr(hrnTlsServerHost()));

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("write error");

                testRequestP(HTTP_VERB_PUT, "/file.txt", .blobType = "BlockBlob", .content = "ABCD");
                testResponseP(.code = 403);

                TEST_ERROR_FMT(
                    storagePutP(storageNewWriteP(storage, strNew("file.txt")), BUFSTRDEF("ABCD")), ProtocolError,
                    "HTTP request failed with 403 (Forbidden):\n"
                    "*** URI/Query ***:\n"
                    "/account/container/file.txt\n"
                    "*** Request Headers ***:\n"
                    "authorization: <redacted>\n"
                    "content-length: 4\n"
                    "content-md5: ywjKSnu1+Wg8GRM6hIcspw==\n"
                    "date: <redacted>\n"
                    "host: %s\n"
                    "x-ms-blob-type: BlockBlob\n"
                    "x-ms-version: 2019-02-02",
                    strPtr(hrnTlsServerHost()));

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("write file in one part (with retry)");

                testRequestP(HTTP_VERB_PUT, "/file.txt", .blobType = "BlockBlob", .content = "ABCD");
                testResponseP(.code = 503);
                testRequestP(HTTP_VERB_PUT, "/file.txt", .blobType = "BlockBlob", .content = "ABCD");
                testResponseP();

                StorageWrite *write = NULL;
                TEST_ASSIGN(write, storageNewWriteP(storage, strNew("file.txt")), "new write");
                TEST_RESULT_VOID(storagePutP(write, BUFSTRDEF("ABCD")), "write");

                TEST_RESULT_BOOL(storageWriteAtomic(write), true, "write is atomic");
                TEST_RESULT_BOOL(storageWriteCreatePath(write), true, "path will be created");
                TEST_RESULT_UINT(storageWriteModeFile(write), 0, "file mode is 0");
                TEST_RESULT_UINT(storageWriteModePath(write), 0, "path mode is 0");
                TEST_RESULT_STR_Z(storageWriteName(write), "/file.txt", "check file name");
                TEST_RESULT_BOOL(storageWriteSyncFile(write), true, "file is synced");
                TEST_RESULT_BOOL(storageWriteSyncPath(write), true, "path is synced");

                TEST_RESULT_VOID(storageWriteAzureClose(write->driver), "close file again");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("write zero-length file");

                testRequestP(HTTP_VERB_PUT, "/file.txt", .blobType = "BlockBlob", .content = "");
                testResponseP();

                TEST_ASSIGN(write, storageNewWriteP(storage, strNew("file.txt")), "new write");
                TEST_RESULT_VOID(storagePutP(write, NULL), "write");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("write file in chunks with nothing left over on close");

                testRequestP(HTTP_VERB_PUT, "/file.txt?blockid=0AAAAAAACCCCCCCCx0000000&comp=block", .content = "1234567890123456");
                testResponseP();

                testRequestP(HTTP_VERB_PUT, "/file.txt?blockid=0AAAAAAACCCCCCCCx0000001&comp=block", .content = "7890123456789012");
                testResponseP();

                testRequestP(
                    HTTP_VERB_PUT, "/file.txt?comp=blocklist",
                    .content =
                        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                        "<BlockList>"
                        "<Uncommitted>0AAAAAAACCCCCCCCx0000000</Uncommitted>"
                        "<Uncommitted>0AAAAAAACCCCCCCCx0000001</Uncommitted>"
                        "</BlockList>\n");
                testResponseP();

                // Test needs a predictable file id
                driver->fileId = 0x0AAAAAAACCCCCCCC;

                TEST_ASSIGN(write, storageNewWriteP(storage, strNew("file.txt")), "new write");
                TEST_RESULT_VOID(storagePutP(write, BUFSTRDEF("12345678901234567890123456789012")), "write");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("write file in chunks with something left over on close");

                testRequestP(HTTP_VERB_PUT, "/file.txt?blockid=0AAAAAAACCCCCCCDx0000000&comp=block", .content = "1234567890123456");
                testResponseP();

                testRequestP(HTTP_VERB_PUT, "/file.txt?blockid=0AAAAAAACCCCCCCDx0000001&comp=block", .content = "7890");
                testResponseP();

                testRequestP(
                    HTTP_VERB_PUT, "/file.txt?comp=blocklist",
                    .content =
                        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                        "<BlockList>"
                        "<Uncommitted>0AAAAAAACCCCCCCDx0000000</Uncommitted>"
                        "<Uncommitted>0AAAAAAACCCCCCCDx0000001</Uncommitted>"
                        "</BlockList>\n");
                testResponseP();

                TEST_ASSIGN(write, storageNewWriteP(storage, strNew("file.txt")), "new write");
                TEST_RESULT_VOID(storagePutP(write, BUFSTRDEF("12345678901234567890")), "write");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("info for missing file");

                testRequestP(HTTP_VERB_HEAD, "/BOGUS");
                testResponseP(.code = 404);

                TEST_RESULT_BOOL(
                    storageInfoP(storage, strNew("BOGUS"), .ignoreMissing = true).exists, false, "file does not exist");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("info for file");

                testRequestP(HTTP_VERB_HEAD, "/subdir/file1.txt");
                testResponseP(.header = "content-length:9999\r\nLast-Modified: Wed, 21 Oct 2015 07:28:00 GMT");

                StorageInfo info;
                TEST_ASSIGN(info, storageInfoP(storage, strNew("subdir/file1.txt")), "file exists");
                TEST_RESULT_BOOL(info.exists, true, "    check exists");
                TEST_RESULT_UINT(info.type, storageTypeFile, "    check type");
                TEST_RESULT_UINT(info.size, 9999, "    check exists");
                TEST_RESULT_INT(info.timeModified, 1445412480, "    check time");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("info check existence only");

                testRequestP(HTTP_VERB_HEAD, "/subdir/file2.txt");
                testResponseP(.header = "content-length:777\r\nLast-Modified: Wed, 22 Oct 2015 07:28:00 GMT");

                TEST_ASSIGN(
                    info, storageInfoP(storage, strNew("subdir/file2.txt"), .level = storageInfoLevelExists), "file exists");
                TEST_RESULT_BOOL(info.exists, true, "    check exists");
                TEST_RESULT_UINT(info.type, storageTypeFile, "    check type");
                TEST_RESULT_UINT(info.size, 0, "    check exists");
                TEST_RESULT_INT(info.timeModified, 0, "    check time");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("list basic level");

                testRequestP(HTTP_VERB_GET, "?comp=list&delimiter=%2F&prefix=path%2Fto%2F&restype=container");
                testResponseP(
                    .content =
                        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                        "<EnumerationResults>"
                        "    <Blobs>"
                        "        <Blob>"
                        "            <Name>path/to/test_file</Name>"
                        "            <Properties>"
                        "                <Last-Modified>Mon, 12 Oct 2009 17:50:30 GMT</Last-Modified>"
                        "                <Content-Length>787</Content-Length>"
                        "            </Properties>"
                        "        </Blob>"
                        "        <BlobPrefix>"
                        "           <Name>path/to/test_path/</Name>"
                        "       </BlobPrefix>"
                        "    </Blobs>"
                        "    <NextMarker/>"
                        "</EnumerationResults>");

                HarnessStorageInfoListCallbackData callbackData =
                {
                    .content = strNew(""),
                };

                TEST_ERROR(
                    storageInfoListP(storage, strNew("/"), hrnStorageInfoListCallback, NULL, .errorOnMissing = true),
                    AssertError, "assertion '!param.errorOnMissing || storageFeature(this, storageFeaturePath)' failed");

                TEST_RESULT_VOID(
                    storageInfoListP(storage, strNew("/path/to"), hrnStorageInfoListCallback, &callbackData), "list");
                TEST_RESULT_STR_Z(
                    callbackData.content,
                    "test_path {path}\n"
                    "test_file {file, s=787, t=1255369830}\n",
                    "check");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("list exists level");

                testRequestP(HTTP_VERB_GET, "?comp=list&delimiter=%2F&restype=container");
                testResponseP(
                    .content =
                        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                        "<EnumerationResults>"
                        "    <Blobs>"
                        "        <Blob>"
                        "            <Name>test1.txt</Name>"
                        "            <Properties/>"
                        "        </Blob>"
                        "        <BlobPrefix>"
                        "            <Name>path1/</Name>"
                        "        </BlobPrefix>"
                        "    </Blobs>"
                        "    <NextMarker/>"
                        "</EnumerationResults>");

                callbackData.content = strNew("");

                TEST_RESULT_VOID(
                    storageInfoListP(
                        storage, strNew("/"), hrnStorageInfoListCallback, &callbackData, .level = storageInfoLevelExists),
                    "list");
                TEST_RESULT_STR_Z(
                    callbackData.content,
                    "path1 {}\n"
                    "test1.txt {}\n",
                    "check");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("list a file in root with expression");

                testRequestP(HTTP_VERB_GET, "?comp=list&delimiter=%2F&prefix=test&restype=container");
                testResponseP(
                    .content =
                        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                        "<EnumerationResults>"
                        "    <Blobs>"
                        "        <Blob>"
                        "            <Name>test1.txt</Name>"
                        "            <Properties/>"
                        "        </Blob>"
                        "    </Blobs>"
                        "    <NextMarker/>"
                        "</EnumerationResults>");

                callbackData.content = strNew("");

                TEST_RESULT_VOID(
                    storageInfoListP(
                        storage, strNew("/"), hrnStorageInfoListCallback, &callbackData, .expression = strNew("^test.*$"),
                        .level = storageInfoLevelExists),
                    "list");
                TEST_RESULT_STR_Z(
                    callbackData.content,
                    "test1.txt {}\n",
                    "check");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("list files with continuation");

                testRequestP(HTTP_VERB_GET, "?comp=list&delimiter=%2F&prefix=path%2Fto%2F&restype=container");
                testResponseP(
                    .content =
                        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                        "<EnumerationResults>"
                        "    <Blobs>"
                        "        <Blob>"
                        "            <Name>path/to/test1.txt</Name>"
                        "            <Properties/>"
                        "        </Blob>"
                        "        <Blob>"
                        "            <Name>path/to/test2.txt</Name>"
                        "            <Properties/>"
                        "        </Blob>"
                        "        <BlobPrefix>"
                        "            <Name>path/to/path1/</Name>"
                        "        </BlobPrefix>"
                        "    </Blobs>"
                        "    <NextMarker>ueGcxLPRx1Tr</NextMarker>"
                        "</EnumerationResults>");

                testRequestP(HTTP_VERB_GET, "?comp=list&delimiter=%2F&marker=ueGcxLPRx1Tr&prefix=path%2Fto%2F&restype=container");
                testResponseP(
                    .content =
                        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                        "<EnumerationResults>"
                        "    <Blobs>"
                        "        <Blob>"
                        "            <Name>path/to/test3.txt</Name>"
                        "            <Properties/>"
                        "        </Blob>"
                        "        <BlobPrefix>"
                        "            <Name>path/to/path2/</Name>"
                        "        </BlobPrefix>"
                        "    </Blobs>"
                        "    <NextMarker/>"
                        "</EnumerationResults>");

                callbackData.content = strNew("");

                TEST_RESULT_VOID(
                    storageInfoListP(
                        storage, strNew("/path/to"), hrnStorageInfoListCallback, &callbackData, .level = storageInfoLevelExists),
                    "list");
                TEST_RESULT_STR_Z(
                    callbackData.content,
                    "path1 {}\n"
                    "test1.txt {}\n"
                    "test2.txt {}\n"
                    "path2 {}\n"
                    "test3.txt {}\n",
                    "check");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("list files with expression");

                testRequestP(HTTP_VERB_GET, "?comp=list&delimiter=%2F&prefix=path%2Fto%2Ftest&restype=container");
                testResponseP(
                    .content =
                        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                        "<EnumerationResults>"
                        "    <Blobs>"
                        "        <Blob>"
                        "            <Name>path/to/test1.txt</Name>"
                        "            <Properties/>"
                        "        </Blob>"
                        "        <Blob>"
                        "            <Name>path/to/test2.txt</Name>"
                        "            <Properties/>"
                        "        </Blob>"
                        "        <Blob>"
                        "            <Name>path/to/test3.txt</Name>"
                        "            <Properties/>"
                        "        </Blob>"
                        "        <BlobPrefix>"
                        "            <Name>path/to/test1.path/</Name>"
                        "        </BlobPrefix>"
                        "        <BlobPrefix>"
                        "            <Name>path/to/test2.path/</Name>"
                        "        </BlobPrefix>"
                        "    </Blobs>"
                        "    <NextMarker/>"
                        "</EnumerationResults>");

                callbackData.content = strNew("");

                TEST_RESULT_VOID(
                    storageInfoListP(
                        storage, strNew("/path/to"), hrnStorageInfoListCallback, &callbackData, .expression = strNew("^test(1|3)"),
                        .level = storageInfoLevelExists),
                    "list");
                TEST_RESULT_STR_Z(
                    callbackData.content,
                    "test1.path {}\n"
                    "test1.txt {}\n"
                    "test3.txt {}\n",
                    "check");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("switch to SAS auth");

                hrnTlsServerClose();

                strLstAddZ(argList, "--" CFGOPT_REPO1_AZURE_KEY_TYPE "=" STORAGE_AZURE_KEY_TYPE_SAS);
                setenv("PGBACKREST_" CFGOPT_REPO1_AZURE_KEY, TEST_KEY_SAS, true);
                harnessCfgLoad(cfgCmdArchivePush, argList);

                TEST_ASSIGN(storage, storageRepoGet(strNew(STORAGE_AZURE_TYPE), true), "get repo storage");

                driver = (StorageAzure *)storage->driver;
                TEST_RESULT_PTR_NE(driver->sasKey, NULL, "check sas key");

                hrnTlsServerAccept();

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("remove file");

                testRequestP(HTTP_VERB_DELETE, "/path/to/test.txt");
                testResponseP();

                TEST_RESULT_VOID(storageRemoveP(storage, strNew("/path/to/test.txt")), "remove");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("remove missing file");

                testRequestP(HTTP_VERB_DELETE, "/path/to/missing.txt");
                testResponseP(.code = 404);

                TEST_RESULT_VOID(storageRemoveP(storage, strNew("/path/to/missing.txt")), "remove");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("remove files error to check redacted sig");

                testRequestP(HTTP_VERB_GET, "?comp=list&restype=container");
                testResponseP(.code = 403);

                TEST_ERROR_FMT(
                    storagePathRemoveP(storage, strNew("/"), .recurse = true), ProtocolError,
                    "HTTP request failed with 403 (Forbidden):\n"
                    "*** URI/Query ***:\n"
                    "/account/container?comp=list&restype=container&sig=<redacted>\n"
                    "*** Request Headers ***:\n"
                    "content-length: 0\n"
                    "host: %s",
                    strPtr(hrnTlsServerHost()));

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("remove files from root");

                testRequestP(HTTP_VERB_GET, "?comp=list&restype=container");
                testResponseP(
                    .content =
                        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                        "<EnumerationResults>"
                        "    <Blobs>"
                        "        <Blob>"
                        "            <Name>test1.txt</Name>"
                        "            <Properties/>"
                        "        </Blob>"
                        "        <Blob>"
                        "            <Name>path1/xxx.zzz</Name>"
                        "            <Properties/>"
                        "        </Blob>"
                        "        <BlobPrefix>"
                        "            <Name>not-deleted/</Name>"
                        "        </BlobPrefix>"
                        "    </Blobs>"
                        "    <NextMarker/>"
                        "</EnumerationResults>");

                testRequestP(HTTP_VERB_DELETE, "/test1.txt");
                testResponseP();

                testRequestP(HTTP_VERB_DELETE, "/path1/xxx.zzz");
                testResponseP();

                TEST_RESULT_VOID(storagePathRemoveP(storage, strNew("/"), .recurse = true), "remove");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("remove files from path");

                testRequestP(HTTP_VERB_GET, "?comp=list&prefix=path%2F&restype=container");
                testResponseP(
                    .content =
                        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                        "<EnumerationResults>"
                        "    <Blobs>"
                        "        <Blob>"
                        "            <Name>path/test1.txt</Name>"
                        "            <Properties/>"
                        "        </Blob>"
                        "        <Blob>"
                        "            <Name>path/path1/xxx.zzz</Name>"
                        "            <Properties/>"
                        "        </Blob>"
                        "        <BlobPrefix>"
                        "            <Name>path/not-deleted/</Name>"
                        "        </BlobPrefix>"
                        "    </Blobs>"
                        "    <NextMarker/>"
                        "</EnumerationResults>");

                testRequestP(HTTP_VERB_DELETE, "/path/test1.txt");
                testResponseP();

                testRequestP(HTTP_VERB_DELETE, "/path/path1/xxx.zzz");
                testResponseP();

                TEST_RESULT_VOID(storagePathRemoveP(storage, strNew("/path"), .recurse = true), "remove");

                // -----------------------------------------------------------------------------------------------------------------
                TEST_TITLE("remove files in empty subpath (nothing to do)");

                testRequestP(HTTP_VERB_GET, "?comp=list&prefix=path%2F&restype=container");
                testResponseP(
                    .content =
                        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                        "<EnumerationResults>"
                        "    <Blobs>"
                        "    </Blobs>"
                        "    <NextMarker/>"
                        "</EnumerationResults>");

                TEST_RESULT_VOID(storagePathRemoveP(storage, strNew("/path"), .recurse = true), "remove");

                // -----------------------------------------------------------------------------------------------------------------
                hrnTlsClientEnd();
            }
            HARNESS_FORK_PARENT_END();
        }
        HARNESS_FORK_END();
    }

    FUNCTION_HARNESS_RESULT_VOID();
}