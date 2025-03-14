// This file Copyright © 2015-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <cerrno> /* errno */
#include <string>
#include <unordered_set>

#include <fcntl.h> /* open() */
#include <unistd.h> /* close() */

#include <sys/types.h>
#include <sys/event.h>

#ifndef O_EVTONLY
#define O_EVTONLY O_RDONLY
#endif

#include <event2/event.h>

#define LIBTRANSMISSION_WATCHDIR_MODULE

#include "transmission.h"
#include "log.h"
#include "tr-assert.h"
#include "utils.h"
#include "watchdir.h"
#include "watchdir-common.h"

/***
****
***/

#define log_error(...) \
    (!tr_logLevelIsActive(TR_LOG_ERROR) ? (void)0 : \
                                          tr_logAddMessage(__FILE__, __LINE__, TR_LOG_ERROR, "watchdir:kqueue", __VA_ARGS__))

/***
****
***/

struct tr_watchdir_kqueue
{
    tr_watchdir_backend base;

    int kq;
    int dirfd;
    struct event* event;
    std::unordered_set<std::string> dir_entries;
};

#define BACKEND_UPCAST(b) (reinterpret_cast<tr_watchdir_kqueue*>(b))

#define KQUEUE_WATCH_MASK (NOTE_WRITE | NOTE_EXTEND)

/***
****
***/

static void tr_watchdir_kqueue_on_event(evutil_socket_t /*fd*/, short /*type*/, void* context)
{
    auto const handle = static_cast<tr_watchdir_t>(context);
    tr_watchdir_kqueue* const backend = BACKEND_UPCAST(tr_watchdir_get_backend(handle));
    struct kevent ke;
    auto ts = timespec{};

    if (kevent(backend->kq, nullptr, 0, &ke, 1, &ts) == -1)
    {
        log_error("Failed to fetch kevent: %s", tr_strerror(errno));
        return;
    }

    /* Read directory with generic scan */
    tr_watchdir_scan(handle, &backend->dir_entries);
}

static void tr_watchdir_kqueue_free(tr_watchdir_backend* backend_base)
{
    tr_watchdir_kqueue* const backend = BACKEND_UPCAST(backend_base);

    if (backend == nullptr)
    {
        return;
    }

    TR_ASSERT(backend->base.free_func == &tr_watchdir_kqueue_free);

    if (backend->event != nullptr)
    {
        event_del(backend->event);
        event_free(backend->event);
    }

    if (backend->kq != -1)
    {
        close(backend->kq);
    }

    if (backend->dirfd != -1)
    {
        close(backend->dirfd);
    }

    delete backend;
}

tr_watchdir_backend* tr_watchdir_kqueue_new(tr_watchdir_t handle)
{
    char const* const path = tr_watchdir_get_path(handle);
    struct kevent ke;

    auto* backend = new tr_watchdir_kqueue{};
    backend->base.free_func = &tr_watchdir_kqueue_free;
    backend->kq = -1;
    backend->dirfd = -1;

    if ((backend->kq = kqueue()) == -1)
    {
        log_error("Failed to start kqueue");
        goto fail;
    }

    /* Open fd for watching */
    if ((backend->dirfd = open(path, O_RDONLY | O_EVTONLY)) == -1)
    {
        log_error("Failed to passively watch directory \"%s\": %s", path, tr_strerror(errno));
        goto fail;
    }

    /* Register kevent filter with kqueue descriptor */
    EV_SET(&ke, backend->dirfd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR, KQUEUE_WATCH_MASK, 0, NULL);

    if (kevent(backend->kq, &ke, 1, nullptr, 0, nullptr) == -1)
    {
        log_error("Failed to set directory event filter with fd %d: %s", backend->kq, tr_strerror(errno));
        goto fail;
    }

    /* Create libevent task for event descriptor */
    if ((backend->event = event_new(
             tr_watchdir_get_event_base(handle),
             backend->kq,
             EV_READ | EV_ET | EV_PERSIST,
             &tr_watchdir_kqueue_on_event,
             handle)) == nullptr)
    {
        log_error("Failed to create event: %s", tr_strerror(errno));
        goto fail;
    }

    if (event_add(backend->event, nullptr) == -1)
    {
        log_error("Failed to add event: %s", tr_strerror(errno));
        goto fail;
    }

    /* Trigger one event for the initial scan */
    event_active(backend->event, EV_READ, 0);

    return BACKEND_DOWNCAST(backend);

fail:
    tr_watchdir_kqueue_free(BACKEND_DOWNCAST(backend));
    return nullptr;
}
