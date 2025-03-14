// This file Copyright © 2021-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

struct evbuffer;

class tr_web
{
public:
    // The response struct passed to the user's FetchDoneFunc callback
    // when a fetch() finishes.
    struct FetchResponse
    {
        long status; // http server response, e.g. 200
        std::string body;
        bool did_connect;
        bool did_timeout;
        void* user_data;
    };

    // Callback to invoke when fetch() is done
    using FetchDoneFunc = std::function<void(FetchResponse const&)>;

    class FetchOptions
    {
    public:
        FetchOptions(std::string_view url_in, FetchDoneFunc&& done_func_in, void* done_func_user_data_in)
            : url{ url_in }
            , done_func{ std::move(done_func_in) }
            , done_func_user_data{ done_func_user_data_in }
        {
        }

        // the URL to fetch
        std::string url;

        // Callback to invoke with a FetchResponse when done
        FetchDoneFunc done_func = nullptr;
        void* done_func_user_data = nullptr;

        // If you need to set multiple cookies, set them all using a single
        // option concatenated like this: "name1=content1; name2=content2;"
        std::optional<std::string> cookies;

        // https://developer.mozilla.org/en-US/docs/Web/HTTP/Range_requests
        std::optional<std::string> range;

        // Tag used by tr_web::Controller to limit some transfers' bandwidth
        std::optional<int> speed_limit_tag;

        // Optionaly set the underlying sockets' send/receive buffers' size.
        // Can be used to conserve resources for scrapes and announces, where
        // the payload is known to be small.
        std::optional<int> sndbuf;
        std::optional<int> rcvbuf;

        // Maximum time to wait before timeout
        int timeout_secs = DefaultTimeoutSecs;

        // If provided, this buffer will be used to hold the response body.
        // Provided for webseeds, which need to set low-level callbacks on
        // the buffer itself.
        evbuffer* buffer = nullptr;

        static constexpr int DefaultTimeoutSecs = 120;
    };

    void fetch(FetchOptions&& options);

    // Notify tr_web that it's going to be destroyed sooon.
    // New fetch() tasks will be rejected, but already-running tasks
    // are left alone so that they can finish.
    void closeSoon();

    // True when tr_web is ready to be destroyed.
    // Will never be true until after closeSoon() is called.
    [[nodiscard]] bool isClosed() const;

    // If you want to give running tasks a chance to finish, call closeSoon()
    // before destroying the tr_web object. Deleting the object will cancel
    // all of its tasks.
    ~tr_web();

    /**
     * Mediates between tr_web and its clients.
     *
     * NB: Note that tr_web calls all these methods from its own thread.
     * Overridden methods should take care to be threadsafe.
     */
    class Controller
    {
    public:
        virtual ~Controller() = default;

        // Return the location of the cookie file, or nullopt to not use one
        [[nodiscard]] virtual std::optional<std::string> cookieFile() const
        {
            return std::nullopt;
        }

        // Return the preferred user public address string, or nullopt to not use one
        [[nodiscard]] virtual std::optional<std::string> publicAddress() const
        {
            return std::nullopt;
        }

        // Return the preferred user aagent, or nullopt to not use one
        [[nodiscard]] virtual std::optional<std::string> userAgent() const
        {
            return std::nullopt;
        }

        // Notify the system that `byte_count` of download bandwidth was used
        virtual void notifyBandwidthConsumed([[maybe_unused]] int bandwidth_tag, [[maybe_unused]] size_t byte_count)
        {
        }

        // Return the number of bytes that should be allowed. See Bandwidth::clamp()
        [[nodiscard]] virtual unsigned int clamp([[maybe_unused]] int bandwidth_tag, unsigned int byte_count) const
        {
            return byte_count;
        }

        // Invoke the user-provided fetch callback
        virtual void run(FetchDoneFunc&& func, FetchResponse&& response) const
        {
            func(response);
        }
    };

    // Note that tr_web does no management of the `controller` reference.
    // The caller must ensure `controller` is valid for tr_web's lifespan.
    static std::unique_ptr<tr_web> create(Controller& controller);

private:
    class Impl;
    std::unique_ptr<Impl> const impl_;
    explicit tr_web(Controller& controller);
};

void tr_sessionFetch(struct tr_session* session, tr_web::FetchOptions&& options);
