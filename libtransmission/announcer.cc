// This file Copyright © 2010-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <climits> /* INT_MAX */
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <event2/buffer.h>
#include <event2/event.h> /* evtimer */

#define LIBTRANSMISSION_ANNOUNCER_MODULE

#include "transmission.h"

#include "announce-list.h"
#include "announcer-common.h"
#include "announcer.h"
#include "crypto-utils.h" /* tr_rand_int(), tr_rand_int_weak() */
#include "log.h"
#include "peer-mgr.h" /* tr_peerMgrCompactToPex() */
#include "session.h"
#include "torrent.h"
#include "tr-assert.h"
#include "utils.h"
#include "web-utils.h"

using namespace std::literals;

#define dbgmsg(tier, ...) \
    do \
    { \
        if (tr_logGetDeepEnabled()) \
        { \
            auto name = std::array<char, 128>{}; \
            tier->buildLogName(std::data(name), std::size(name)); \
            tr_logAddDeep(__FILE__, __LINE__, std::data(name), __VA_ARGS__); \
        } \
    } while (0)

/* unless the tracker says otherwise, rescrape this frequently */
static auto constexpr DefaultScrapeIntervalSec = int{ 60 * 30 };
/* unless the tracker says otherwise, this is the announce interval */
static auto constexpr DefaultAnnounceIntervalSec = int{ 60 * 10 };
/* unless the tracker says otherwise, this is the announce min_interval */
static auto constexpr DefaultAnnounceMinIntervalSec = int{ 60 * 2 };
/* the value of the 'numwant' argument passed in tracker requests. */
static auto constexpr Numwant = int{ 80 };

/* how often to announce & scrape */
static auto constexpr UpkeepIntervalMsec = int{ 500 };
static auto constexpr MaxAnnouncesPerUpkeep = int{ 20 };
static auto constexpr MaxScrapesPerUpkeep = int{ 20 };

/* this is how often to call the UDP tracker upkeep */
static auto constexpr TauUpkeepIntervalSecs = int{ 5 };

/* how many infohashes to remove when we get a scrape-too-long error */
static auto constexpr TrMultiscrapeStep = int{ 5 };

/***
****
***/

char const* tr_announce_event_get_string(tr_announce_event e)
{
    switch (e)
    {
    case TR_ANNOUNCE_EVENT_COMPLETED:
        return "completed";

    case TR_ANNOUNCE_EVENT_STARTED:
        return "started";

    case TR_ANNOUNCE_EVENT_STOPPED:
        return "stopped";

    default:
        return "";
    }
}

namespace
{

struct StopsCompare
{
    static int compare(tr_announce_request const* a, tr_announce_request const* b) // <=>
    {
        // primary key: volume of data transferred
        auto const ax = a->up + a->down;
        auto const bx = b->up + b->down;
        if (ax < bx)
        {
            return -1;
        }
        if (ax > bx)
        {
            return 1;
        }

        // secondary key: the torrent's info_hash
        if (a->info_hash < b->info_hash)
        {
            return -1;
        }
        if (a->info_hash > b->info_hash)
        {
            return 1;
        }

        // tertiary key: the tracker's announce url
        if (a->announce_url < b->announce_url)
        {
            return -1;
        }
        if (a->announce_url > b->announce_url)
        {
            return 1;
        }

        return 0;
    }

    bool operator()(tr_announce_request const* a, tr_announce_request const* b) const // less than
    {
        return compare(a, b) < 0;
    }
};

} // namespace

/***
****
***/

struct tr_scrape_info
{
    tr_interned_string scrape_url;

    int multiscrape_max;

    tr_scrape_info(tr_interned_string scrape_url_in, int const multiscrape_max_in)
        : scrape_url{ scrape_url_in }
        , multiscrape_max{ multiscrape_max_in }
    {
    }
};

static void onUpkeepTimer(evutil_socket_t fd, short what, void* vannouncer);

/**
 * "global" (per-tr_session) fields
 */
struct tr_announcer
{
    explicit tr_announcer(tr_session* session_in)
        : session{ session_in }
        , upkeep_timer{ evtimer_new(session_in->event_base, onUpkeepTimer, this) }
    {
        scheduleNextUpdate();
    }

    ~tr_announcer()
    {
        event_free(upkeep_timer);
    }

    void scheduleNextUpdate() const
    {
        tr_timerAddMsec(this->upkeep_timer, UpkeepIntervalMsec);
    }

    std::set<tr_announce_request*, StopsCompare> stops;
    std::map<tr_interned_string, tr_scrape_info> scrape_info;

    tr_session* const session;
    event* const upkeep_timer;
    int const key = tr_rand_int(INT_MAX);
    time_t tau_upkeep_at = 0;
};

static tr_scrape_info* tr_announcerGetScrapeInfo(tr_announcer* announcer, tr_interned_string url)
{
    if (std::empty(url))
    {
        return nullptr;
    }

    auto& scrapes = announcer->scrape_info;
    auto const it = scrapes.try_emplace(url, url, TR_MULTISCRAPE_MAX);
    return &it.first->second;
}

void tr_announcerInit(tr_session* session)
{
    TR_ASSERT(tr_isSession(session));

    auto* a = new tr_announcer{ session };

    session->announcer = a;
}

static void flushCloseMessages(tr_announcer* announcer);

void tr_announcerClose(tr_session* session)
{
    tr_announcer* announcer = session->announcer;

    flushCloseMessages(announcer);

    tr_tracker_udp_start_shutdown(session);

    session->announcer = nullptr;
    delete announcer;
}

/***
****
***/

/* a row in tr_tier's list of trackers */
struct tr_tracker
{
    explicit tr_tracker(tr_announcer* announcer, tr_announce_list::tracker_info const& info)
        : host{ info.host }
        , announce_url{ info.announce_str }
        , sitename{ info.announce.sitename }
        , scrape_info{ std::empty(info.scrape_str) ? nullptr : tr_announcerGetScrapeInfo(announcer, info.scrape_str) }
        , id{ info.id }
    {
    }

    [[nodiscard]] int getRetryInterval() const
    {
        switch (consecutive_failures)
        {
        case 0:
            return 0;

        case 1:
            return 20;

        case 2:
            return tr_rand_int_weak(60) + 60 * 5;

        case 3:
            return tr_rand_int_weak(60) + 60 * 15;

        case 4:
            return tr_rand_int_weak(60) + 60 * 30;

        case 5:
            return tr_rand_int_weak(60) + 60 * 60;

        default:
            return tr_rand_int_weak(60) + 60 * 120;
        }
    }

    tr_interned_string const host;
    tr_interned_string const announce_url;
    std::string_view const sitename;
    tr_scrape_info* const scrape_info;

    std::string tracker_id;

    int seeder_count = -1;
    int leecher_count = -1;
    int download_count = -1;
    int downloader_count = -1;

    int consecutive_failures = 0;

    tr_tracker_id_t const id;
};

// format: `${host}:${port}`
tr_interned_string tr_announcerGetKey(tr_url_parsed_t const& parsed)
{
    std::string buf;
    tr_buildBuf(buf, parsed.host, ":"sv, parsed.portstr);
    return tr_interned_string{ buf };
}

/***
****
***/

/** @brief A group of trackers in a single tier, as per the multitracker spec */
struct tr_tier
{
    tr_tier(tr_announcer* announcer, tr_torrent* tor_in, std::vector<tr_announce_list::tracker_info const*> const& infos)
        : tor{ tor_in }
        , id{ next_key++ }
    {
        trackers.reserve(std::size(infos));
        for (auto const* info : infos)
        {
            trackers.emplace_back(announcer, *info);
        }
        useNextTracker();
        scrapeSoon();
    }

    [[nodiscard]] tr_tracker* currentTracker()
    {
        if (!current_tracker_index_)
        {
            return nullptr;
        }

        TR_ASSERT(*current_tracker_index_ < std::size(trackers));
        return &trackers[*current_tracker_index_];
    }

    [[nodiscard]] tr_tracker const* currentTracker() const
    {
        if (!current_tracker_index_)
        {
            return nullptr;
        }

        TR_ASSERT(*current_tracker_index_ < std::size(trackers));
        return &trackers[*current_tracker_index_];
    }

    [[nodiscard]] bool needsToAnnounce(time_t now) const
    {
        return !isAnnouncing && !isScraping && announceAt != 0 && announceAt <= now && !std::empty(announce_events);
    }

    [[nodiscard]] bool needsToScrape(time_t now) const
    {
        auto const* const tracker = currentTracker();

        return !isScraping && scrapeAt != 0 && scrapeAt <= now && tracker != nullptr && tracker->scrape_info != nullptr;
    }

    [[nodiscard]] auto countDownloaders() const
    {
        auto const* const tracker = currentTracker();

        return tracker == nullptr ? 0 : tracker->downloader_count + tracker->leecher_count;
    }

    tr_tracker* useNextTracker()
    {
        // move our index to the next tracker in the tier
        if (std::empty(trackers))
        {
            current_tracker_index_ = std::nullopt;
        }
        else if (!current_tracker_index_)
        {
            current_tracker_index_ = 0;
        }
        else
        {
            current_tracker_index_ = (*current_tracker_index_ + 1) % std::size(trackers);
        }

        // reset some of the tier's fields
        scrapeIntervalSec = DefaultScrapeIntervalSec;
        announceIntervalSec = DefaultAnnounceIntervalSec;
        announceMinIntervalSec = DefaultAnnounceMinIntervalSec;
        isAnnouncing = false;
        isScraping = false;
        lastAnnounceStartTime = 0;
        lastScrapeStartTime = 0;

        return currentTracker();
    }

    [[nodiscard]] std::optional<size_t> indexOf(tr_interned_string const& announce_url) const
    {
        for (size_t i = 0, n = std::size(trackers); i < n; ++i)
        {
            if (announce_url == trackers[i].announce_url)
            {
                return i;
            }
        }

        return std::nullopt;
    }

    void buildLogName(char* buf, size_t buflen) const
    {
        auto const* const torrent_name = tr_torrentName(tor);
        auto const* const current_tracker = currentTracker();
        auto const host_sv = current_tracker == nullptr ? "?"sv : current_tracker->host.sv();
        tr_snprintf(buf, buflen, "[%s---%" TR_PRIsv "]", torrent_name, TR_PRIsv_ARG(host_sv));
    }

    [[nodiscard]] bool canManualAnnounce() const
    {
        return this->manualAnnounceAllowedAt <= tr_time();
    }

    void scheduleNextScrape()
    {
        scheduleNextScrape(this->scrapeIntervalSec);
    }

    void scrapeSoon()
    {
        scheduleNextScrape(0);
    }

    void scheduleNextScrape(int interval)
    {
        this->scrapeAt = getNextScrapeTime(tor->session, this, interval);
    }

    tr_torrent* const tor;

    /* number of up/down/corrupt bytes since the last time we sent an
     * "event=stopped" message that was acknowledged by the tracker */
    std::array<uint64_t, 3> byteCounts = {};

    std::vector<tr_tracker> trackers;
    std::optional<size_t> current_tracker_index_;

    time_t scrapeAt = 0;
    time_t lastScrapeStartTime = 0;
    time_t lastScrapeTime = 0;
    bool lastScrapeSucceeded = false;
    bool lastScrapeTimedOut = false;

    time_t announceAt = 0;
    time_t manualAnnounceAllowedAt = 0;
    time_t lastAnnounceStartTime = 0;
    time_t lastAnnounceTime = 0;
    bool lastAnnounceSucceeded = false;
    bool lastAnnounceTimedOut = false;

    std::deque<tr_announce_event> announce_events;
    int announce_event_priority = 0;

    int const id;

    int scrapeIntervalSec = DefaultScrapeIntervalSec;
    int announceIntervalSec = DefaultAnnounceIntervalSec;
    int announceMinIntervalSec = DefaultAnnounceMinIntervalSec;

    int lastAnnouncePeerCount = 0;

    bool isRunning = false;
    bool isAnnouncing = false;
    bool isScraping = false;

    std::string last_announce_str;
    std::string last_scrape_str;

private:
    [[nodiscard]] static time_t getNextScrapeTime(tr_session const* session, tr_tier const* tier, int interval)
    {
        // Maybe don't scrape paused torrents
        if (!tier->isRunning && !session->scrapePausedTorrents)
        {
            return 0;
        }

        /* Add the interval, and then increment to the nearest 10th second.
         * The latter step is to increase the odds of several torrents coming
         * due at the same time to improve multiscrape. */
        auto ret = tr_time() + interval;
        while (ret % 10 != 0)
        {
            ++ret;
        }

        return ret;
    }

    static int next_key;
};

int tr_tier::next_key = 0;

/***
****
***/

/**
 * @brief Opaque, per-torrent data structure for tracker announce information
 *
 * this opaque data structure can be found in tr_torrent.tiers
 */
struct tr_torrent_announcer
{
    tr_torrent_announcer(tr_announcer* announcer, tr_torrent* tor)
    {
        // build the trackers
        auto tier_to_infos = std::map<tr_tracker_tier_t, std::vector<tr_announce_list::tracker_info const*>>{};
        auto const& announce_list = tor->announceList();
        for (auto const& info : announce_list)
        {
            tier_to_infos[info.tier].emplace_back(&info);
        }

        for (auto const& tt : tier_to_infos)
        {
            tiers.emplace_back(announcer, tor, tt.second);
        }
    }

    tr_tier* getTier(int tier_id)
    {
        for (auto& tier : tiers)
        {
            if (tier.id == tier_id)
            {
                return &tier;
            }
        }

        return nullptr;
    }

    tr_tier* getTierFromScrape(tr_interned_string const& scrape_url)
    {
        for (auto& tier : tiers)
        {
            auto const* const tracker = tier.currentTracker();

            if (tracker != nullptr && tracker->scrape_info != nullptr && tracker->scrape_info->scrape_url == scrape_url)
            {
                return &tier;
            }
        }

        return nullptr;
    }

    [[nodiscard]] bool canManualAnnounce() const
    {
        return std::any_of(std::begin(tiers), std::end(tiers), [](auto const& tier) { return tier.canManualAnnounce(); });
    }

    [[nodiscard]] bool findTracker(
        tr_interned_string const& announce_url,
        tr_tier const** setme_tier,
        tr_tracker const** setme_tracker) const
    {
        for (auto const& tier : tiers)
        {
            for (auto const& tracker : tier.trackers)
            {
                if (tracker.announce_url == announce_url)
                {
                    *setme_tier = &tier;
                    *setme_tracker = &tracker;
                    return true;
                }
            }
        }

        return false;
    }

    std::vector<tr_tier> tiers;

    tr_tracker_callback callback = nullptr;
    void* callback_data = nullptr;
};

static tr_tier* getTier(tr_announcer* announcer, tr_sha1_digest_t const& info_hash, int tier_id)
{
    if (announcer == nullptr)
    {
        return nullptr;
    }

    auto* const tor = announcer->session->getTorrent(info_hash);
    if (tor == nullptr || tor->torrent_announcer == nullptr)
    {
        return nullptr;
    }

    return tor->torrent_announcer->getTier(tier_id);
}

/***
****  PUBLISH
***/

static void publishMessage(tr_tier* tier, std::string_view msg, TrackerEventType type)
{
    if (tier != nullptr && tier->tor != nullptr && tier->tor->torrent_announcer != nullptr &&
        tier->tor->torrent_announcer->callback != nullptr)
    {
        auto* const ta = tier->tor->torrent_announcer;
        auto event = tr_tracker_event{};
        event.messageType = type;
        event.text = msg;

        if (auto const* const current_tracker = tier->currentTracker(); current_tracker != nullptr)
        {
            event.announce_url = current_tracker->announce_url;
        }

        (*ta->callback)(tier->tor, &event, ta->callback_data);
    }
}

static void publishErrorClear(tr_tier* tier)
{
    publishMessage(tier, ""sv, TR_TRACKER_ERROR_CLEAR);
}

static void publishWarning(tr_tier* tier, std::string_view msg)
{
    publishMessage(tier, msg, TR_TRACKER_WARNING);
}

static void publishError(tr_tier* tier, std::string_view msg)
{
    publishMessage(tier, msg, TR_TRACKER_ERROR);
}

static void publishPeerCounts(tr_tier* tier, int seeders, int leechers)
{
    if (tier->tor->torrent_announcer->callback != nullptr)
    {
        auto e = tr_tracker_event{};
        e.messageType = TR_TRACKER_COUNTS;
        e.seeders = seeders;
        e.leechers = leechers;
        dbgmsg(tier, "peer counts: %d seeders, %d leechers.", seeders, leechers);

        (*tier->tor->torrent_announcer->callback)(tier->tor, &e, nullptr);
    }
}

static void publishPeersPex(tr_tier* tier, int seeders, int leechers, std::vector<tr_pex> const& pex)
{
    if (tier->tor->torrent_announcer->callback != nullptr)
    {
        auto e = tr_tracker_event{};
        e.messageType = TR_TRACKER_PEERS;
        e.seeders = seeders;
        e.leechers = leechers;
        e.pex = pex;
        dbgmsg(
            tier,
            "tracker knows of %d seeders and %d leechers and gave a list of %zu peers.",
            seeders,
            leechers,
            std::size(pex));

        (*tier->tor->torrent_announcer->callback)(tier->tor, &e, nullptr);
    }
}

/***
****
***/

tr_torrent_announcer* tr_announcerAddTorrent(tr_torrent* tor, tr_tracker_callback callback, void* callback_data)
{
    TR_ASSERT(tr_isTorrent(tor));

    auto* ta = new tr_torrent_announcer(tor->session->announcer, tor);
    ta->callback = callback;
    ta->callback_data = callback_data;
    return ta;
}

/***
****
***/

bool tr_announcerCanManualAnnounce(tr_torrent const* tor)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tor->torrent_announcer != nullptr);

    return tor->isRunning && tor->torrent_announcer->canManualAnnounce();
}

time_t tr_announcerNextManualAnnounce(tr_torrent const* tor)
{
    time_t ret = ~(time_t)0;

    for (auto const& tier : tor->torrent_announcer->tiers)
    {
        if (tier.isRunning)
        {
            ret = std::min(ret, tier.manualAnnounceAllowedAt);
        }
    }

    return ret;
}

static void dbgmsg_tier_announce_queue(tr_tier const* tier)
{
    if (tr_logGetDeepEnabled())
    {
        auto name = std::array<char, 128>{};
        tier->buildLogName(std::data(name), std::size(name));

        auto* const buf = evbuffer_new();
        for (size_t i = 0, n = std::size(tier->announce_events); i < n; ++i)
        {
            tr_announce_event const e = tier->announce_events[i];
            char const* str = tr_announce_event_get_string(e);
            evbuffer_add_printf(buf, "[%zu:%s]", i, str);
        }

        auto const message = evbuffer_free_to_str(buf);
        tr_logAddDeep(__FILE__, __LINE__, std::data(name), "announce queue is %" TR_PRIsv, TR_PRIsv_ARG(message));
    }
}

// higher priorities go to the front of the announce queue
static void tier_update_announce_priority(tr_tier* tier)
{
    int priority = -1;

    for (auto const& event : tier->announce_events)
    {
        priority = std::max(priority, int{ event });
    }

    tier->announce_event_priority = priority;
}

static void tier_announce_remove_trailing(tr_tier* tier, tr_announce_event e)
{
    while (!std::empty(tier->announce_events) && tier->announce_events.back() == e)
    {
        tier->announce_events.resize(std::size(tier->announce_events) - 1);
    }

    tier_update_announce_priority(tier);
}

static void tier_announce_event_push(tr_tier* tier, tr_announce_event e, time_t announceAt)
{
    TR_ASSERT(tier != nullptr);

    dbgmsg_tier_announce_queue(tier);
    dbgmsg(tier, "queued \"%s\"", tr_announce_event_get_string(e));

    auto& events = tier->announce_events;
    if (!std::empty(events))
    {
        /* special case #1: if we're adding a "stopped" event,
         * dump everything leading up to it except "completed" */
        if (e == TR_ANNOUNCE_EVENT_STOPPED)
        {
            bool has_completed = std::count(std::begin(events), std::end(events), TR_ANNOUNCE_EVENT_COMPLETED) != 0;
            events.clear();
            if (has_completed)
            {
                events.push_back(TR_ANNOUNCE_EVENT_COMPLETED);
            }
        }

        /* special case #2: dump all empty strings leading up to this event */
        tier_announce_remove_trailing(tier, TR_ANNOUNCE_EVENT_NONE);

        /* special case #3: no consecutive duplicates */
        tier_announce_remove_trailing(tier, e);
    }

    /* add it */
    events.push_back(e);
    tier->announceAt = announceAt;
    tier_update_announce_priority(tier);

    dbgmsg_tier_announce_queue(tier);
    dbgmsg(tier, "announcing in %d seconds", (int)difftime(announceAt, tr_time()));
}

static auto tier_announce_event_pull(tr_tier* tier)
{
    auto const e = tier->announce_events.front();
    tier->announce_events.pop_front();
    tier_update_announce_priority(tier);
    return e;
}

static void torrentAddAnnounce(tr_torrent* tor, tr_announce_event e, time_t announceAt)
{
    // tell each tier to announce
    for (auto& tier : tor->torrent_announcer->tiers)
    {
        tier_announce_event_push(&tier, e, announceAt);
    }
}

void tr_announcerTorrentStarted(tr_torrent* tor)
{
    torrentAddAnnounce(tor, TR_ANNOUNCE_EVENT_STARTED, tr_time());
}

void tr_announcerManualAnnounce(tr_torrent* tor)
{
    torrentAddAnnounce(tor, TR_ANNOUNCE_EVENT_NONE, tr_time());
}

void tr_announcerTorrentStopped(tr_torrent* tor)
{
    torrentAddAnnounce(tor, TR_ANNOUNCE_EVENT_STOPPED, tr_time());
}

void tr_announcerTorrentCompleted(tr_torrent* tor)
{
    torrentAddAnnounce(tor, TR_ANNOUNCE_EVENT_COMPLETED, tr_time());
}

void tr_announcerChangeMyPort(tr_torrent* tor)
{
    tr_announcerTorrentStarted(tor);
}

/***
****
***/

void tr_announcerAddBytes(tr_torrent* tor, int type, uint32_t byteCount)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(type == TR_ANN_UP || type == TR_ANN_DOWN || type == TR_ANN_CORRUPT);

    for (auto& tier : tor->torrent_announcer->tiers)
    {
        tier.byteCounts[type] += byteCount;
    }
}

/***
****
***/

static tr_announce_request* announce_request_new(
    tr_announcer const* announcer,
    tr_torrent* tor,
    tr_tier const* tier,
    tr_announce_event event)
{
    auto* const req = new tr_announce_request();
    req->port = tr_sessionGetPublicPeerPort(announcer->session);
    req->announce_url = tier->currentTracker()->announce_url;
    req->tracker_id = tier->currentTracker()->tracker_id;
    req->info_hash = tor->infoHash();
    req->peer_id = tr_torrentGetPeerId(tor);
    req->up = tier->byteCounts[TR_ANN_UP];
    req->down = tier->byteCounts[TR_ANN_DOWN];
    req->corrupt = tier->byteCounts[TR_ANN_CORRUPT];
    req->leftUntilComplete = tor->hasMetadata() ? tor->totalSize() - tor->hasTotal() : INT64_MAX;
    req->event = event;
    req->numwant = event == TR_ANNOUNCE_EVENT_STOPPED ? 0 : Numwant;
    req->key = announcer->key;
    req->partial_seed = tor->isPartialSeed();
    tier->buildLogName(req->log_name, sizeof(req->log_name));
    return req;
}

void tr_announcerRemoveTorrent(tr_announcer* announcer, tr_torrent* tor)
{
    auto* const ta = tor->torrent_announcer;
    if (ta == nullptr)
    {
        return;
    }

    for (auto const& tier : ta->tiers)
    {
        if (tier.isRunning)
        {
            auto const e = TR_ANNOUNCE_EVENT_STOPPED;
            auto* req = announce_request_new(announcer, tor, &tier, e);

            if (announcer->stops.count(req) != 0U)
            {
                delete req;
            }
            else
            {
                announcer->stops.insert(req);
            }
        }
    }

    tor->torrent_announcer = nullptr;
    delete ta;
}

struct announce_data
{
    int const tier_id;
    time_t const time_sent;
    tr_announce_event event = {};
    tr_session* session = nullptr;

    /** If the request succeeds, the value for tier's "isRunning" flag */
    bool is_running_on_success = false;
};

static void on_announce_error(tr_tier* tier, char const* err, tr_announce_event e)
{
    /* increment the error count */
    auto* current_tracker = tier->currentTracker();
    if (current_tracker != nullptr)
    {
        ++current_tracker->consecutive_failures;
    }

    /* set the error message */
    tier->last_announce_str = err;

    /* switch to the next tracker */
    current_tracker = tier->useNextTracker();

    /* schedule a reannounce */
    int const interval = current_tracker->getRetryInterval();
    auto const* const host_cstr = current_tracker->host.c_str();
    dbgmsg(tier, "Tracker '%s' announce error: %s (Retrying in %d seconds)", host_cstr, err, interval);
    tr_logAddTorInfo(tier->tor, "Tracker '%s' announce error: %s (Retrying in %d seconds)", host_cstr, err, interval);
    tier_announce_event_push(tier, e, tr_time() + interval);
}

static void on_announce_done(tr_announce_response const* response, void* vdata)
{
    auto* const data = static_cast<announce_data*>(vdata);

    tr_announcer* announcer = data->session->announcer;
    tr_tier* tier = getTier(announcer, response->info_hash, data->tier_id);
    time_t const now = tr_time();
    tr_announce_event const event = data->event;

    if (tier != nullptr)
    {
        dbgmsg(
            tier,
            "Got announce response: "
            "connected:%d "
            "timeout:%d "
            "seeders:%d "
            "leechers:%d "
            "downloads:%d "
            "interval:%d "
            "min_interval:%d "
            "tracker_id_str:%s "
            "pex:%zu "
            "pex6:%zu "
            "err:%s "
            "warn:%s",
            (int)response->did_connect,
            (int)response->did_timeout,
            response->seeders,
            response->leechers,
            response->downloads,
            response->interval,
            response->min_interval,
            (!std::empty(response->tracker_id) ? response->tracker_id.c_str() : "none"),
            std::size(response->pex),
            std::size(response->pex6),
            (!std::empty(response->errmsg) ? response->errmsg.c_str() : "none"),
            (!std::empty(response->warning) ? response->warning.c_str() : "none"));

        tier->lastAnnounceTime = now;
        tier->lastAnnounceTimedOut = response->did_timeout;
        tier->lastAnnounceSucceeded = false;
        tier->isAnnouncing = false;
        tier->manualAnnounceAllowedAt = now + tier->announceMinIntervalSec;

        if (!response->did_connect)
        {
            on_announce_error(tier, _("Could not connect to tracker"), event);
        }
        else if (response->did_timeout)
        {
            on_announce_error(tier, _("Tracker did not respond"), event);
        }
        else if (!std::empty(response->errmsg))
        {
            /* If the torrent's only tracker returned an error, publish it.
               Don't bother publishing if there are other trackers -- it's
               all too common for people to load up dozens of dead trackers
               in a torrent's metainfo... */
            if (tier->tor->trackerCount() < 2)
            {
                publishError(tier, response->errmsg);
            }

            on_announce_error(tier, response->errmsg.c_str(), event);
        }
        else
        {
            auto const isStopped = event == TR_ANNOUNCE_EVENT_STOPPED;
            auto leechers = int{};
            auto scrape_fields = int{};
            auto seeders = int{};

            publishErrorClear(tier);

            tr_tracker* const tracker = tier->currentTracker();
            if (tracker != nullptr)
            {
                tracker->consecutive_failures = 0;

                if (response->seeders >= 0)
                {
                    tracker->seeder_count = seeders = response->seeders;
                    ++scrape_fields;
                }

                if (response->leechers >= 0)
                {
                    tracker->leecher_count = leechers = response->leechers;
                    ++scrape_fields;
                }

                if (response->downloads >= 0)
                {
                    tracker->download_count = response->downloads;
                    ++scrape_fields;
                }

                if (!std::empty(response->tracker_id))
                {
                    tracker->tracker_id = response->tracker_id;
                }
            }

            if (auto const& warning = response->warning; !std::empty(warning))
            {
                tier->last_announce_str = warning;
                dbgmsg(tier, "tracker gave \"%s\"", warning.c_str());
                publishWarning(tier, warning);
            }
            else
            {
                tier->last_announce_str = _("Success");
            }

            if (response->min_interval != 0)
            {
                tier->announceMinIntervalSec = response->min_interval;
            }

            if (response->interval != 0)
            {
                tier->announceIntervalSec = response->interval;
            }

            if (!std::empty(response->pex))
            {
                publishPeersPex(tier, seeders, leechers, response->pex);
            }

            if (!std::empty(response->pex6))
            {
                publishPeersPex(tier, seeders, leechers, response->pex6);
            }

            publishPeerCounts(tier, seeders, leechers);

            tier->isRunning = data->is_running_on_success;

            /* if the tracker included scrape fields in its announce response,
               then a separate scrape isn't needed */
            if (scrape_fields >= 3 || (scrape_fields >= 1 && tracker->scrape_info == nullptr))
            {
                tr_logAddTorDbg(
                    tier->tor,
                    "Announce response contained scrape info; "
                    "rescheduling next scrape to %d seconds from now.",
                    tier->scrapeIntervalSec);
                tier->scheduleNextScrape();
                tier->lastScrapeTime = now;
                tier->lastScrapeSucceeded = true;
            }
            else if (tier->lastScrapeTime + tier->scrapeIntervalSec <= now)
            {
                tier->scrapeSoon();
            }

            tier->lastAnnounceSucceeded = true;
            tier->lastAnnouncePeerCount = std::size(response->pex) + std::size(response->pex6);

            if (isStopped)
            {
                /* now that we've successfully stopped the torrent,
                 * we can reset the up/down/corrupt count we've kept
                 * for this tracker */
                tier->byteCounts[TR_ANN_UP] = 0;
                tier->byteCounts[TR_ANN_DOWN] = 0;
                tier->byteCounts[TR_ANN_CORRUPT] = 0;
            }

            if (!isStopped && std::empty(tier->announce_events))
            {
                /* the queue is empty, so enqueue a perodic update */
                int const i = tier->announceIntervalSec;
                dbgmsg(tier, "Sending periodic reannounce in %d seconds", i);
                tier_announce_event_push(tier, TR_ANNOUNCE_EVENT_NONE, now + i);
            }
        }
    }

    delete data;
}

static void announce_request_delegate(
    tr_announcer* announcer,
    tr_announce_request* request,
    tr_announce_response_func callback,
    announce_data* callback_data)
{
    tr_session* session = announcer->session;

#if 0

    fprintf(stderr, "ANNOUNCE: event %s isPartialSeed %d port %d key %d numwant %d up %" PRIu64 " down %" PRIu64
        " corrupt %" PRIu64 " left %" PRIu64 " url [%s] tracker_id_str [%s] peer_id [%20.20s]\n",
        tr_announce_event_get_string(request->event), (int)request->partial_seed, (int)request->port, request->key,
        request->numwant, request->up, request->down, request->corrupt, request->leftUntilComplete, request->url,
        request->tracker_id_str, request->peer_id);

#endif

    if (auto const announce_sv = request->announce_url.sv();
        tr_strvStartsWith(announce_sv, "http://"sv) || tr_strvStartsWith(announce_sv, "https://"sv))
    {
        tr_tracker_http_announce(session, request, callback, callback_data);
    }
    else if (tr_strvStartsWith(announce_sv, "udp://"sv))
    {
        tr_tracker_udp_announce(session, request, callback, callback_data);
    }
    else
    {
        tr_logAddError("Unsupported url: %" TR_PRIsv, TR_PRIsv_ARG(announce_sv));
        delete callback_data;
    }

    delete request;
}

static void tierAnnounce(tr_announcer* announcer, tr_tier* tier)
{
    TR_ASSERT(!tier->isAnnouncing);
    TR_ASSERT(!std::empty(tier->announce_events));

    time_t const now = tr_time();

    tr_torrent* tor = tier->tor;
    tr_announce_event announce_event = tier_announce_event_pull(tier);
    tr_announce_request* req = announce_request_new(announcer, tor, tier, announce_event);

    auto* const data = new announce_data{ tier->id, now, announce_event, announcer->session, tor->isRunning };

    tier->isAnnouncing = true;
    tier->lastAnnounceStartTime = now;

    announce_request_delegate(announcer, req, on_announce_done, data);
}

/***
****
****  SCRAPE
****
***/

static bool multiscrape_too_big(std::string_view errmsg)
{
    /* Found a tracker that returns some bespoke string for this case?
       Add your patch here and open a PR */
    auto constexpr TooLongErrors = std::array<std::string_view, 3>{
        "Bad Request",
        "GET string too long",
        "Request-URI Too Long",
    };

    return std::any_of(
        std::begin(TooLongErrors),
        std::end(TooLongErrors),
        [&errmsg](auto const& substr) { return tr_strvContains(errmsg, substr); });
}

static void on_scrape_error(tr_session const* /*session*/, tr_tier* tier, char const* errmsg)
{
    // increment the error count
    auto* current_tracker = tier->currentTracker();
    if (current_tracker != nullptr)
    {
        ++current_tracker->consecutive_failures;
    }

    // set the error message
    tier->last_scrape_str = errmsg != nullptr ? errmsg : "";

    // switch to the next tracker
    current_tracker = tier->useNextTracker();

    // schedule a rescrape
    auto const interval = current_tracker->getRetryInterval();
    auto const* const host_cstr = current_tracker->host.c_str();
    dbgmsg(tier, "Tracker '%s' scrape error: %s (Retrying in %zu seconds)", host_cstr, errmsg, (size_t)interval);
    tr_logAddTorInfo(tier->tor, "Tracker '%s' error: %s (Retrying in %zu seconds)", host_cstr, errmsg, (size_t)interval);
    tier->lastScrapeSucceeded = false;
    tier->scheduleNextScrape(interval);
}

static void checkMultiscrapeMax(tr_announcer* announcer, tr_scrape_response const* response)
{
    if (!multiscrape_too_big(response->errmsg))
    {
        return;
    }

    auto const& url = response->scrape_url;
    auto* const scrape_info = tr_announcerGetScrapeInfo(announcer, url);
    if (scrape_info == nullptr)
    {
        return;
    }

    // Lower the max only if it hasn't already lowered for a similar
    // error. So if N parallel multiscrapes all have the same `max`
    // and error out, lower the value once for that batch, not N times.
    int& multiscrape_max = scrape_info->multiscrape_max;
    if (multiscrape_max < response->row_count)
    {
        return;
    }

    int const n = std::max(1, int{ multiscrape_max - TrMultiscrapeStep });
    if (multiscrape_max != n)
    {
        // don't log the full URL, since that might have a personal announce id
        // (note: we know 'parsed' will be successful since this url has a scrape_info)
        auto const parsed = *tr_urlParse(url.sv());
        auto clean_url = std::string{};
        tr_buildBuf(clean_url, parsed.scheme, "://"sv, parsed.host, ":"sv, parsed.portstr);
        tr_logAddNamedInfo(clean_url.c_str(), "Reducing multiscrape max to %d", n);
        multiscrape_max = n;
    }
}

static void on_scrape_done(tr_scrape_response const* response, void* vsession)
{
    auto const now = tr_time();
    auto* const session = static_cast<tr_session*>(vsession);
    auto* const announcer = session->announcer;

    for (int i = 0; i < response->row_count; ++i)
    {
        auto const& row = response->rows[i];
        auto* const tor = session->getTorrent(row.info_hash);

        if (tor != nullptr)
        {
            auto* tier = tor->torrent_announcer->getTierFromScrape(response->scrape_url);

            if (tier == nullptr)
            {
                continue;
            }

            auto const scrape_url_sv = response->scrape_url.sv();

            dbgmsg(
                tier,
                "scraped url:%" TR_PRIsv
                " -- "
                "did_connect:%d "
                "did_timeout:%d "
                "seeders:%d "
                "leechers:%d "
                "downloads:%d "
                "downloaders:%d "
                "min_request_interval:%d "
                "err:%s ",
                TR_PRIsv_ARG(scrape_url_sv),
                (int)response->did_connect,
                (int)response->did_timeout,
                row.seeders,
                row.leechers,
                row.downloads,
                row.downloaders,
                response->min_request_interval,
                std::empty(response->errmsg) ? "none" : response->errmsg.c_str());

            tier->isScraping = false;
            tier->lastScrapeTime = now;
            tier->lastScrapeSucceeded = false;
            tier->lastScrapeTimedOut = response->did_timeout;

            if (!response->did_connect)
            {
                on_scrape_error(session, tier, _("Could not connect to tracker"));
            }
            else if (response->did_timeout)
            {
                on_scrape_error(session, tier, _("Tracker did not respond"));
            }
            else if (!std::empty(response->errmsg))
            {
                on_scrape_error(session, tier, response->errmsg.c_str());
            }
            else
            {
                tier->lastScrapeSucceeded = true;
                tier->scrapeIntervalSec = std::max(int{ DefaultScrapeIntervalSec }, response->min_request_interval);
                tier->scheduleNextScrape();
                tr_logAddTorDbg(tier->tor, "Scrape successful. Rescraping in %d seconds.", tier->scrapeIntervalSec);

                if (tr_tracker* const tracker = tier->currentTracker(); tracker != nullptr)
                {
                    if (row.seeders >= 0)
                    {
                        tracker->seeder_count = row.seeders;
                    }

                    if (row.leechers >= 0)
                    {
                        tracker->leecher_count = row.leechers;
                    }

                    if (row.downloads >= 0)
                    {
                        tracker->download_count = row.downloads;
                    }

                    tracker->downloader_count = row.downloaders;
                    tracker->consecutive_failures = 0;
                }

                if (row.seeders >= 0 && row.leechers >= 0 && row.downloads >= 0)
                {
                    publishPeerCounts(tier, row.seeders, row.leechers);
                }
            }
        }
    }

    checkMultiscrapeMax(announcer, response);
}

static void scrape_request_delegate(
    tr_announcer* announcer,
    tr_scrape_request const* request,
    tr_scrape_response_func callback,
    void* callback_data)
{
    tr_session* session = announcer->session;

    auto const scrape_sv = request->scrape_url.sv();

    if (tr_strvStartsWith(scrape_sv, "http://"sv) || tr_strvStartsWith(scrape_sv, "https://"sv))
    {
        tr_tracker_http_scrape(session, request, callback, callback_data);
    }
    else if (tr_strvStartsWith(scrape_sv, "udp://"sv))
    {
        tr_tracker_udp_scrape(session, request, callback, callback_data);
    }
    else
    {
        tr_logAddError("Unsupported url: %" TR_PRIsv, TR_PRIsv_ARG(scrape_sv));
    }
}

static void multiscrape(tr_announcer* announcer, std::vector<tr_tier*> const& tiers)
{
    auto const now = tr_time();
    auto requests = std::array<tr_scrape_request, MaxScrapesPerUpkeep>{};
    auto request_count = size_t{};

    // batch as many info_hashes into a request as we can
    for (auto* tier : tiers)
    {
        auto const* const scrape_info = tier->currentTracker()->scrape_info;
        bool found = false;

        TR_ASSERT(scrape_info != nullptr);

        /* if there's a request with this scrape URL and a free slot, use it */
        for (size_t j = 0; !found && j < request_count; ++j)
        {
            auto* const req = &requests[j];

            if (req->info_hash_count >= scrape_info->multiscrape_max)
            {
                continue;
            }

            if (scrape_info->scrape_url != req->scrape_url)
            {
                continue;
            }

            req->info_hash[req->info_hash_count] = tier->tor->infoHash();
            ++req->info_hash_count;
            tier->isScraping = true;
            tier->lastScrapeStartTime = now;
            found = true;
        }

        /* otherwise, if there's room for another request, build a new one */
        if (!found && request_count < MaxScrapesPerUpkeep)
        {
            auto* const req = &requests[request_count];
            req->scrape_url = scrape_info->scrape_url;
            tier->buildLogName(req->log_name, sizeof(req->log_name));

            req->info_hash[req->info_hash_count] = tier->tor->infoHash();
            ++req->info_hash_count;
            tier->isScraping = true;
            tier->lastScrapeStartTime = now;

            ++request_count;
        }
    }

    /* send the requests we just built */
    for (size_t i = 0; i < request_count; ++i)
    {
        scrape_request_delegate(announcer, &requests[i], on_scrape_done, announcer->session);
    }
}

static void flushCloseMessages(tr_announcer* announcer)
{
    auto& stops = announcer->stops;
    std::for_each(
        std::begin(stops),
        std::end(stops),
        [&announcer](auto* stop) { announce_request_delegate(announcer, stop, nullptr, nullptr); });
    stops.clear();
}

static int compareAnnounceTiers(tr_tier const* a, tr_tier const* b)
{
    /* prefer higher-priority events */
    if (auto const priority_a = a->announce_event_priority, priority_b = b->announce_event_priority; priority_a != priority_b)
    {
        return priority_a > priority_b ? -1 : 1;
    }

    /* prefer swarms where we might upload */
    if (auto const leechers_a = a->countDownloaders(), leechers_b = b->countDownloaders(); leechers_a != leechers_b)
    {
        return leechers_a > leechers_b ? -1 : 1;
    }

    /* prefer swarms where we might download */
    if (auto const is_done_a = a->tor->isDone(), is_done_b = b->tor->isDone(); is_done_a != is_done_b)
    {
        return is_done_a ? 1 : -1;
    }

    /* prefer larger stats, to help ensure stats get recorded when stopping on shutdown */
    if (auto const xa = a->byteCounts[TR_ANN_UP] + a->byteCounts[TR_ANN_DOWN],
        xb = b->byteCounts[TR_ANN_UP] + b->byteCounts[TR_ANN_DOWN];
        xa != xb)
    {
        return xa > xb ? -1 : 1;
    }

    // announcements that have been waiting longer go first
    if (a->announceAt != b->announceAt)
    {
        return a->announceAt < b->announceAt ? -1 : 1;
    }

    // the tiers are effectively equal priority, but add an arbitrary
    // differentiation because ptrArray sorted mode hates equal items.
    return a < b ? -1 : 1;
}

static void scrapeAndAnnounceMore(tr_announcer* announcer)
{
    time_t const now = tr_time();

    /* build a list of tiers that need to be announced */
    auto announce_me = std::vector<tr_tier*>{};
    auto scrape_me = std::vector<tr_tier*>{};
    for (auto* tor : announcer->session->torrents)
    {
        for (auto& tier : tor->torrent_announcer->tiers)
        {
            if (tier.needsToAnnounce(now))
            {
                announce_me.push_back(&tier);
            }

            if (tier.needsToScrape(now))
            {
                scrape_me.push_back(&tier);
            }
        }
    }

    /* First, scrape what we can. We handle scrapes first because
     * we can work through that queue much faster than announces
     * (thanks to multiscrape) _and_ the scrape responses will tell
     * us which swarms are interesting and should be announced next. */
    multiscrape(announcer, scrape_me);

    /* Second, announce what we can. If there aren't enough slots
     * available, use compareAnnounceTiers to prioritize. */
    if (announce_me.size() > MaxAnnouncesPerUpkeep)
    {
        std::partial_sort(
            std::begin(announce_me),
            std::begin(announce_me) + MaxAnnouncesPerUpkeep,
            std::end(announce_me),
            [](auto const* a, auto const* b) { return compareAnnounceTiers(a, b) < 0; });
        announce_me.resize(MaxAnnouncesPerUpkeep);
    }

    for (auto*& tier : announce_me)
    {
        tr_logAddTorDbg(tier->tor, "%s", "Announcing to tracker");
        tierAnnounce(announcer, tier);
    }
}

static void onUpkeepTimer(evutil_socket_t /*fd*/, short /*what*/, void* vannouncer)
{
    auto* announcer = static_cast<tr_announcer*>(vannouncer);
    tr_session* session = announcer->session;
    auto const lock = session->unique_lock();

    bool const is_closing = session->isClosed;
    time_t const now = tr_time();

    /* maybe send out some "stopped" messages for closed torrents */
    flushCloseMessages(announcer);

    /* maybe kick off some scrapes / announces whose time has come */
    if (!is_closing)
    {
        scrapeAndAnnounceMore(announcer);
    }

    /* TAU upkeep */
    if (announcer->tau_upkeep_at <= now)
    {
        announcer->tau_upkeep_at = now + TauUpkeepIntervalSecs;
        tr_tracker_udp_upkeep(session);
    }

    // set up the next timer
    announcer->scheduleNextUpdate();
}

/***
****
***/

static tr_tracker_view trackerView(tr_torrent const& tor, int tier_index, tr_tier const& tier, tr_tracker const& tracker)
{
    auto const now = tr_time();
    auto view = tr_tracker_view{};

    view.host = tracker.host.c_str();
    view.announce = tracker.announce_url.c_str();
    view.scrape = tracker.scrape_info == nullptr ? "" : tracker.scrape_info->scrape_url.c_str();
    *std::copy_n(
        std::begin(tracker.sitename),
        std::min(std::size(tracker.sitename), sizeof(view.sitename) - 1),
        view.sitename) = '\0';

    view.id = tracker.id;
    view.tier = tier_index;
    view.isBackup = &tracker != tier.currentTracker();
    view.lastScrapeStartTime = tier.lastScrapeStartTime;
    view.seederCount = tracker.seeder_count;
    view.leecherCount = tracker.leecher_count;
    view.downloadCount = tracker.download_count;

    if (view.isBackup)
    {
        view.scrapeState = TR_TRACKER_INACTIVE;
        view.announceState = TR_TRACKER_INACTIVE;
        view.nextScrapeTime = 0;
        view.nextAnnounceTime = 0;
    }
    else
    {
        view.hasScraped = tier.lastScrapeTime != 0;
        if (view.hasScraped)
        {
            view.lastScrapeTime = tier.lastScrapeTime;
            view.lastScrapeSucceeded = tier.lastScrapeSucceeded;
            view.lastScrapeTimedOut = tier.lastScrapeTimedOut;
            tr_strlcpy(view.lastScrapeResult, tier.last_scrape_str.c_str(), sizeof(view.lastScrapeResult));
        }

        if (tier.isScraping)
        {
            view.scrapeState = TR_TRACKER_ACTIVE;
        }
        else if (tier.scrapeAt == 0)
        {
            view.scrapeState = TR_TRACKER_INACTIVE;
        }
        else if (tier.scrapeAt > now)
        {
            view.scrapeState = TR_TRACKER_WAITING;
            view.nextScrapeTime = tier.scrapeAt;
        }
        else
        {
            view.scrapeState = TR_TRACKER_QUEUED;
        }

        view.lastAnnounceStartTime = tier.lastAnnounceStartTime;

        view.hasAnnounced = tier.lastAnnounceTime != 0;
        if (view.hasAnnounced)
        {
            view.lastAnnounceTime = tier.lastAnnounceTime;
            view.lastAnnounceSucceeded = tier.lastAnnounceSucceeded;
            view.lastAnnounceTimedOut = tier.lastAnnounceTimedOut;
            view.lastAnnouncePeerCount = tier.lastAnnouncePeerCount;
            tr_strlcpy(view.lastAnnounceResult, tier.last_announce_str.c_str(), sizeof(view.lastAnnounceResult));
        }

        if (tier.isAnnouncing)
        {
            view.announceState = TR_TRACKER_ACTIVE;
        }
        else if (!tor.isRunning || tier.announceAt == 0)
        {
            view.announceState = TR_TRACKER_INACTIVE;
        }
        else if (tier.announceAt > now)
        {
            view.announceState = TR_TRACKER_WAITING;
            view.nextAnnounceTime = tier.announceAt;
        }
        else
        {
            view.announceState = TR_TRACKER_QUEUED;
        }
    }

    TR_ASSERT(0 <= view.tier);
    return view;
}

size_t tr_announcerTrackerCount(tr_torrent const* tor)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tor->torrent_announcer != nullptr);

    auto const& tiers = tor->torrent_announcer->tiers;
    return std::accumulate(
        std::begin(tiers),
        std::end(tiers),
        size_t{},
        [](size_t acc, auto const& cur) { return acc + std::size(cur.trackers); });
}

tr_tracker_view tr_announcerTracker(tr_torrent const* tor, size_t nth)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tor->torrent_announcer != nullptr);

    auto i = size_t{ 0 };
    for (auto const& tier : tor->torrent_announcer->tiers)
    {
        for (auto const& tracker : tier.trackers)
        {
            if (i == nth)
            {
                return trackerView(*tor, i, tier, tracker);
            }

            ++i;
        }
    }

    TR_ASSERT(false);
    return {};
}

/***
****
***/

// called after the torrent's announceList was rebuilt --
// so announcer needs to update the tr_tier / tr_trackers to match
void tr_announcerResetTorrent(tr_announcer* /*announcer*/, tr_torrent* tor)
{
    // make a new tr_announcer_tier
    auto* const older = tor->torrent_announcer;
    tor->torrent_announcer = new tr_torrent_announcer(tor->session->announcer, tor);
    auto* const newer = tor->torrent_announcer;

    // copy the tracker counts into the new replacementa
    if (older != nullptr)
    {
        for (auto& new_tier : newer->tiers)
        {
            for (auto& new_tracker : new_tier.trackers)
            {
                tr_tier const* old_tier = nullptr;
                tr_tracker const* old_tracker = nullptr;
                if (older->findTracker(new_tracker.announce_url, &old_tier, &old_tracker))
                {
                    new_tracker.seeder_count = old_tracker->seeder_count;
                    new_tracker.leecher_count = old_tracker->leecher_count;
                    new_tracker.download_count = old_tracker->download_count;
                    new_tracker.downloader_count = old_tracker->downloader_count;

                    new_tier.announce_events = old_tier->announce_events;
                    new_tier.announce_event_priority = old_tier->announce_event_priority;

                    auto const* const old_current = old_tier->currentTracker();
                    new_tier.current_tracker_index_ = old_current == nullptr ? std::nullopt :
                                                                               new_tier.indexOf(old_current->announce_url);
                }
            }
        }
    }

    // kickstart any tiers that didn't get started
    if (tor->isRunning)
    {
        auto const now = tr_time();
        for (auto& tier : newer->tiers)
        {
            if (!tier.current_tracker_index_)
            {
                tier_announce_event_push(&tier, TR_ANNOUNCE_EVENT_STARTED, now);
            }
        }
    }

    delete older;
}
