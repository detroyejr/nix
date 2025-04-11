#include "nix/store/s3-binary-cache-store.hh"

#if NIX_WITH_S3_SUPPORT

#include <assert.h>

#include "nix/store/s3.hh"
#include "nix/store/nar-info.hh"
#include "nix/store/nar-info-disk-cache.hh"
#include "nix/store/globals.hh"
#include "nix/util/compression.hh"
#include "nix/store/filetransfer.hh"
#include "nix/util/signals.hh"

#include <aws/core/Aws.h>
#include <aws/core/VersionConfig.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/core/utils/logging/FormattedLogSystem.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/threading/Executor.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListObjectsRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/transfer/TransferManager.h>

using namespace Aws::Transfer;

namespace nix {

struct S3Error : public Error
{
    Aws::S3::S3Errors err;

    template<typename... Args>
    S3Error(Aws::S3::S3Errors err, const Args & ... args)
        : Error(args...), err(err) { };
};

/* Helper: given an Outcome<R, E>, return R in case of success, or
   throw an exception in case of an error. */
template<typename R, typename E>
R && checkAws(std::string_view s, Aws::Utils::Outcome<R, E> && outcome)
{
    if (!outcome.IsSuccess())
        throw S3Error(
            outcome.GetError().GetErrorType(),
            fmt(
                "%s: %s (request id: %s)",
                s,
                outcome.GetError().GetMessage(),
                outcome.GetError().GetRequestId()));
    return outcome.GetResultWithOwnership();
}

class AwsLogger : public Aws::Utils::Logging::FormattedLogSystem
{
    using Aws::Utils::Logging::FormattedLogSystem::FormattedLogSystem;

    void ProcessFormattedStatement(Aws::String && statement) override
    {
        debug("AWS: %s", chomp(statement));
    }

#if !(AWS_SDK_VERSION_MAJOR <= 1 && AWS_SDK_VERSION_MINOR <= 7 && AWS_SDK_VERSION_PATCH <= 115)
    void Flush() override {}
#endif
};

static void initAWS()
{
    static std::once_flag flag;
    std::call_once(flag, []() {
        Aws::SDKOptions options;

        /* We install our own OpenSSL locking function (see
           shared.cc), so don't let aws-sdk-cpp override it. */
        options.cryptoOptions.initAndCleanupOpenSSL = false;

        if (verbosity >= lvlDebug) {
            options.loggingOptions.logLevel =
                verbosity == lvlDebug
                ? Aws::Utils::Logging::LogLevel::Debug
                : Aws::Utils::Logging::LogLevel::Trace;
            options.loggingOptions.logger_create_fn = [options]() {
                return std::make_shared<AwsLogger>(options.loggingOptions.logLevel);
            };
        }

        Aws::InitAPI(options);
    });
}

S3Helper::S3Helper(
    const std::string & profile,
    const std::string & region,
    const std::string & scheme,
    const std::string & endpoint)
    : config(makeConfig(region, scheme, endpoint))
    , client(make_ref<Aws::S3::S3Client>(
            profile == ""
            ? std::dynamic_pointer_cast<Aws::Auth::AWSCredentialsProvider>(
                std::make_shared<Aws::Auth::DefaultAWSCredentialsProviderChain>())
            : std::dynamic_pointer_cast<Aws::Auth::AWSCredentialsProvider>(
                std::make_shared<Aws::Auth::ProfileConfigFileAWSCredentialsProvider>(profile.c_str())),
            *config,
            // FIXME: https://github.com/aws/aws-sdk-cpp/issues/759
#if AWS_SDK_VERSION_MAJOR == 1 && AWS_SDK_VERSION_MINOR < 3
            false,
#else
            Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
#endif
            endpoint.empty()))
{
}

/* Log AWS retries. */
class RetryStrategy : public Aws::Client::DefaultRetryStrategy
{
    bool ShouldRetry(const Aws::Client::AWSError<Aws::Client::CoreErrors>& error, long attemptedRetries) const override
    {
        checkInterrupt();
        auto retry = Aws::Client::DefaultRetryStrategy::ShouldRetry(error, attemptedRetries);
        if (retry)
            printError("AWS error '%s' (%s; request id: %s), will retry in %d ms",
                error.GetExceptionName(),
                error.GetMessage(),
                error.GetRequestId(),
                CalculateDelayBeforeNextRetry(error, attemptedRetries));
        return retry;
    }
};

ref<Aws::Client::ClientConfiguration> S3Helper::makeConfig(
    const std::string & region,
    const std::string & scheme,
    const std::string & endpoint)
{
    initAWS();
    auto res = make_ref<Aws::Client::ClientConfiguration>();
    res->allowSystemProxy = true;
    res->region = region;
    if (!scheme.empty()) {
        res->scheme = Aws::Http::SchemeMapper::FromString(scheme.c_str());
    }
    if (!endpoint.empty()) {
        res->endpointOverride = endpoint;
    }
    res->requestTimeoutMs = 600 * 1000;
    res->connectTimeoutMs = 5 * 1000;
    res->retryStrategy = std::make_shared<RetryStrategy>();
    res->caFile = settings.caFile;
    return res;
}

S3Helper::FileTransferResult S3Helper::getObject(
    const std::string & bucketName, const std::string & key)
{
    std::string uri = "s3://" + bucketName + "/" + key;
    Activity act(*logger, lvlTalkative, actFileTransfer,
        fmt("downloading '%s'", uri),
        Logger::Fields{uri}, getCurActivity());

    auto request =
        Aws::S3::Model::GetObjectRequest()
        .WithBucket(bucketName)
        .WithKey(key);

    request.SetResponseStreamFactory([&]() {
        return Aws::New<std::stringstream>("STRINGSTREAM");
    });

    size_t bytesDone = 0;
    size_t bytesExpected = 0;
    request.SetDataReceivedEventHandler([&](const Aws::Http::HttpRequest * req, Aws::Http::HttpResponse * resp, long long l) {
        if (!bytesExpected && resp->HasHeader("Content-Length")) {
            if (auto length = string2Int<size_t>(resp->GetHeader("Content-Length"))) {
                bytesExpected = *length;
            }
        }
        bytesDone += l;
        act.progress(bytesDone, bytesExpected);
    });

    request.SetContinueRequestHandler([](const Aws::Http::HttpRequest*) {
        return !isInterrupted();
    });

    FileTransferResult res;

    auto now1 = std::chrono::steady_clock::now();

    try {

        auto result = checkAws(fmt("AWS error fetching '%s'", key),
            client->GetObject(request));

        act.progress(result.GetContentLength(), result.GetContentLength());

        res.data = decompress(result.GetContentEncoding(),
            dynamic_cast<std::stringstream &>(result.GetBody()).str());

    } catch (S3Error & e) {
        if ((e.err != Aws::S3::S3Errors::NO_SUCH_KEY) &&
            (e.err != Aws::S3::S3Errors::ACCESS_DENIED)) throw;
    }

    auto now2 = std::chrono::steady_clock::now();

    res.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(now2 - now1).count();

    return res;
}

S3BinaryCacheStore::S3BinaryCacheStore(const Params & params)
    : BinaryCacheStoreConfig(params)
    , BinaryCacheStore(params)
{ }


S3BinaryCacheStoreConfig::S3BinaryCacheStoreConfig(
    std::string_view uriScheme,
    std::string_view bucketName,
    const Params & params)
    : StoreConfig(params)
    , BinaryCacheStoreConfig(params)
    , bucketName(bucketName)
{
    // Don't want to use use AWS SDK in header, so we check the default
    // here. TODO do this better after we overhaul the store settings
    // system.
    assert(std::string{defaultRegion} == std::string{Aws::Region::US_EAST_1});

    if (bucketName.empty())
        throw UsageError("`%s` store requires a bucket name in its Store URI", uriScheme);
}

std::string S3BinaryCacheStoreConfig::doc()
{
    return
      #include "s3-binary-cache-store.md"
      ;
}


struct S3BinaryCacheStoreImpl : virtual S3BinaryCacheStoreConfig, public virtual S3BinaryCacheStore
{
    Stats stats;

    S3Helper s3Helper;

    S3BinaryCacheStoreImpl(
        std::string_view uriScheme,
        std::string_view bucketName,
        const Params & params)
        : StoreConfig(params)
        , BinaryCacheStoreConfig(params)
        , S3BinaryCacheStoreConfig(uriScheme, bucketName, params)
        , Store(params)
        , BinaryCacheStore(params)
        , S3BinaryCacheStore(params)
        , s3Helper(profile, region, scheme, endpoint)
    {
        diskCache = getNarInfoDiskCache();
    }

    std::string getUri() override
    {
        return "s3://" + bucketName;
    }

    void init() override
    {
        if (auto cacheInfo = diskCache->upToDateCacheExists(getUri())) {
            wantMassQuery.setDefault(cacheInfo->wantMassQuery);
            priority.setDefault(cacheInfo->priority);
        } else {
            BinaryCacheStore::init();
            diskCache->createCache(getUri(), storeDir, wantMassQuery, priority);
        }
    }

    const Stats & getS3Stats() override
    {
        return stats;
    }

    /* This is a specialisation of isValidPath() that optimistically
       fetches the .narinfo file, rather than first checking for its
       existence via a HEAD request. Since .narinfos are small, doing
       a GET is unlikely to be slower than HEAD. */
    bool isValidPathUncached(const StorePath & storePath) override
    {
        try {
            queryPathInfo(storePath);
            return true;
        } catch (InvalidPath & e) {
            return false;
        }
    }

    bool fileExists(const std::string & path) override
    {
        stats.head++;

        auto res = s3Helper.client->HeadObject(
            Aws::S3::Model::HeadObjectRequest()
            .WithBucket(bucketName)
            .WithKey(path));

        if (!res.IsSuccess()) {
            auto & error = res.GetError();
            if (error.GetErrorType() == Aws::S3::S3Errors::RESOURCE_NOT_FOUND
                || error.GetErrorType() == Aws::S3::S3Errors::NO_SUCH_KEY
                // If bucket listing is disabled, 404s turn into 403s
                || error.GetErrorType() == Aws::S3::S3Errors::ACCESS_DENIED)
                return false;
            throw Error("AWS error fetching '%s': %s", path, error.GetMessage());
        }

        return true;
    }

    std::shared_ptr<TransferManager> transferManager;
    std::once_flag transferManagerCreated;

    struct AsyncContext : public Aws::Client::AsyncCallerContext
    {
        mutable std::mutex mutex;
        mutable std::condition_variable cv;
        const Activity & act;

        void notify() const
        {
            cv.notify_one();
        }

        void wait() const
        {
            std::unique_lock<std::mutex> lk(mutex);
            cv.wait(lk);
        }

        AsyncContext(const Activity & act) : act(act) {}
    };

    void uploadFile(const std::string & path,
        std::shared_ptr<std::basic_iostream<char>> istream,
        const std::string & mimeType,
        const std::string & contentEncoding)
    {
        std::string uri = "s3://" + bucketName + "/" + path;
        Activity act(*logger, lvlTalkative, actFileTransfer,
            fmt("uploading '%s'", uri),
            Logger::Fields{uri}, getCurActivity());
        istream->seekg(0, istream->end);
        auto size = istream->tellg();
        istream->seekg(0, istream->beg);

        auto maxThreads = std::thread::hardware_concurrency();

        static std::shared_ptr<Aws::Utils::Threading::PooledThreadExecutor>
            executor = std::make_shared<Aws::Utils::Threading::PooledThreadExecutor>(maxThreads);

        std::call_once(transferManagerCreated, [&]()
        {
            if (multipartUpload) {
                TransferManagerConfiguration transferConfig(executor.get());

                transferConfig.s3Client = s3Helper.client;
                transferConfig.bufferSize = bufferSize;

                transferConfig.uploadProgressCallback =
                    [](const TransferManager * transferManager,
                        const std::shared_ptr<const TransferHandle> & transferHandle)
                    {
                        auto context = std::dynamic_pointer_cast<const AsyncContext>(transferHandle->GetContext());
                        size_t bytesDone = transferHandle->GetBytesTransferred();
                        size_t bytesTotal = transferHandle->GetBytesTotalSize();
                        try {
                            checkInterrupt();
                            context->act.progress(bytesDone, bytesTotal);
                        } catch (...) {
                            context->notify();
                        }
                    };
                transferConfig.transferStatusUpdatedCallback =
                    [](const TransferManager * transferManager,
                        const std::shared_ptr<const TransferHandle> & transferHandle)
                    {
                        auto context = std::dynamic_pointer_cast<const AsyncContext>(transferHandle->GetContext());
                        context->notify();
                    };

                transferManager = TransferManager::Create(transferConfig);
            }
        });

        auto now1 = std::chrono::steady_clock::now();

        if (transferManager) {

            if (contentEncoding != "")
                throw Error("setting a content encoding is not supported with S3 multi-part uploads");

            auto context = std::make_shared<AsyncContext>(act);
            std::shared_ptr<TransferHandle> transferHandle =
                transferManager->UploadFile(
                    istream, bucketName, path, mimeType,
                    Aws::Map<Aws::String, Aws::String>(),
                    context /*, contentEncoding */);

            TransferStatus status = transferHandle->GetStatus();
            while (status == TransferStatus::IN_PROGRESS || status == TransferStatus::NOT_STARTED) {
                if (!isInterrupted()) {
                    context->wait();
                } else {
                    transferHandle->Cancel();
                    transferHandle->WaitUntilFinished();
                }
                status = transferHandle->GetStatus();
            }
            act.progress(transferHandle->GetBytesTransferred(), transferHandle->GetBytesTotalSize());

            if (status == TransferStatus::FAILED)
                throw Error("AWS error: failed to upload 's3://%s/%s': %s",
                    bucketName, path, transferHandle->GetLastError().GetMessage());

            if (status != TransferStatus::COMPLETED)
                throw Error("AWS error: transfer status of 's3://%s/%s' in unexpected state",
                    bucketName, path);

        } else {
            act.progress(0, size);

            auto request =
                Aws::S3::Model::PutObjectRequest()
                .WithBucket(bucketName)
                .WithKey(path);

            size_t bytesSent = 0;
            request.SetDataSentEventHandler([&](const Aws::Http::HttpRequest * req, long long l) {
                bytesSent += l;
                act.progress(bytesSent, size);
            });

            request.SetContinueRequestHandler([](const Aws::Http::HttpRequest*) {
                return !isInterrupted();
            });

            request.SetContentType(mimeType);

            if (contentEncoding != "")
                request.SetContentEncoding(contentEncoding);

            request.SetBody(istream);

            auto result = checkAws(fmt("AWS error uploading '%s'", path),
                s3Helper.client->PutObject(request));

            act.progress(size, size);
        }

        auto now2 = std::chrono::steady_clock::now();

        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(now2 - now1)
                .count();

        printInfo("uploaded 's3://%s/%s' (%d bytes) in %d ms",
            bucketName, path, size, duration);

        stats.putTimeMs += duration;
        stats.putBytes += std::max(size, (decltype(size)) 0);
        stats.put++;
    }

    void upsertFile(const std::string & path,
        std::shared_ptr<std::basic_iostream<char>> istream,
        const std::string & mimeType) override
    {
        auto compress = [&](std::string compression)
        {
            auto compressed = nix::compress(compression, StreamToSourceAdapter(istream).drain());
            return std::make_shared<std::stringstream>(std::move(compressed));
        };

        if (narinfoCompression != "" && hasSuffix(path, ".narinfo"))
            uploadFile(path, compress(narinfoCompression), mimeType, narinfoCompression);
        else if (lsCompression != "" && hasSuffix(path, ".ls"))
            uploadFile(path, compress(lsCompression), mimeType, lsCompression);
        else if (logCompression != "" && hasPrefix(path, "log/"))
            uploadFile(path, compress(logCompression), mimeType, logCompression);
        else
            uploadFile(path, istream, mimeType, "");
    }

    void getFile(const std::string & path, Sink & sink) override
    {
        stats.get++;

        // FIXME: stream output to sink.
        auto res = s3Helper.getObject(bucketName, path);

        stats.getBytes += res.data ? res.data->size() : 0;
        stats.getTimeMs += res.durationMs;

        if (res.data) {
            printTalkative("downloaded 's3://%s/%s' (%d bytes) in %d ms",
                bucketName, path, res.data->size(), res.durationMs);

            sink(*res.data);
        } else
            throw NoSuchBinaryCacheFile("file '%s' does not exist in binary cache '%s'", path, getUri());
    }

    StorePathSet queryAllValidPaths() override
    {
        StorePathSet paths;
        std::string marker;

        do {
            debug("listing bucket 's3://%s' from key '%s'...", bucketName, marker);

            auto res = checkAws(fmt("AWS error listing bucket '%s'", bucketName),
                s3Helper.client->ListObjects(
                    Aws::S3::Model::ListObjectsRequest()
                    .WithBucket(bucketName)
                    .WithDelimiter("/")
                    .WithMarker(marker)));

            auto & contents = res.GetContents();

            debug("got %d keys, next marker '%s'",
                contents.size(), res.GetNextMarker());

            for (const auto & object : contents) {
                auto & key = object.GetKey();
                if (key.size() != 40 || !hasSuffix(key, ".narinfo")) continue;
                paths.insert(parseStorePath(storeDir + "/" + key.substr(0, key.size() - 8) + "-" + MissingName));
            }

            marker = res.GetNextMarker();
        } while (!marker.empty());

        return paths;
    }

    /**
     * For now, we conservatively say we don't know.
     *
     * \todo try to expose our S3 authentication status.
     */
    std::optional<TrustedFlag> isTrustedClient() override
    {
        return std::nullopt;
    }
};

static RegisterStoreImplementation<S3BinaryCacheStoreImpl, S3BinaryCacheStoreConfig> regS3BinaryCacheStore;

}

#endif
