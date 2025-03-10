// This file Copyright © 2009-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include "Session.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <string_view>

#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QStyle>
#include <QTextStream>
#include <QtDebug>

#include <libtransmission/session-id.h>
#include <libtransmission/transmission.h>
#include <libtransmission/utils.h> // tr_free
#include <libtransmission/variant.h>

#include "AddData.h"
#include "CustomVariantType.h"
#include "Prefs.h"
#include "RpcQueue.h"
#include "SessionDialog.h"
#include "Torrent.h"
#include "Utils.h"
#include "VariantHelpers.h"

using ::trqt::variant_helpers::dictAdd;
using ::trqt::variant_helpers::dictFind;
using ::trqt::variant_helpers::getValue;
using ::trqt::variant_helpers::listAdd;

/***
****
***/

void Session::sessionSet(tr_quark const key, QVariant const& value)
{
    tr_variant args;
    tr_variantInitDict(&args, 1);

    switch (value.type())
    {
    case QVariant::Bool:
        dictAdd(&args, key, value.toBool());
        break;

    case QVariant::Int:
        dictAdd(&args, key, value.toInt());
        break;

    case QVariant::Double:
        dictAdd(&args, key, value.toDouble());
        break;

    case QVariant::String:
        dictAdd(&args, key, value.toString());
        break;

    default:
        assert(false);
    }

    exec("session-set", &args);
}

void Session::portTest()
{
    auto* q = new RpcQueue();

    q->add([this]() { return exec("port-test", nullptr); });

    q->add(
        [this](RpcResponse const& r)
        {
            bool is_open = false;

            if (r.success)
            {
                auto const value = dictFind<bool>(r.args.get(), TR_KEY_port_is_open);
                if (value)
                {
                    is_open = *value;
                }
            }

            emit portTested(is_open);
        });

    q->run();
}

void Session::copyMagnetLinkToClipboard(int torrent_id)
{
    auto constexpr MagnetLinkKey = std::string_view{ "magnetLink" };
    auto constexpr Fields = std::array<std::string_view, 1>{ MagnetLinkKey };

    tr_variant args;
    tr_variantInitDict(&args, 2);
    dictAdd(&args, TR_KEY_ids, std::array<int, 1>{ torrent_id });
    dictAdd(&args, TR_KEY_fields, Fields);

    auto* q = new RpcQueue();

    q->add([this, &args]() { return exec(TR_KEY_torrent_get, &args); });

    q->add(
        [](RpcResponse const& r)
        {
            tr_variant* torrents = nullptr;
            if (!tr_variantDictFindList(r.args.get(), TR_KEY_torrents, &torrents))
            {
                return;
            }

            tr_variant* const child = tr_variantListChild(torrents, 0);
            if (child != nullptr)
            {
                auto const link = dictFind<QString>(child, TR_KEY_magnetLink);
                if (link)
                {
                    QApplication::clipboard()->setText(*link);
                }
            }
        });

    q->run();
}

void Session::updatePref(int key)
{
    if (prefs_.isCore(key))
    {
        switch (key)
        {
        case Prefs::ALT_SPEED_LIMIT_DOWN:
        case Prefs::ALT_SPEED_LIMIT_ENABLED:
        case Prefs::ALT_SPEED_LIMIT_TIME_BEGIN:
        case Prefs::ALT_SPEED_LIMIT_TIME_DAY:
        case Prefs::ALT_SPEED_LIMIT_TIME_ENABLED:
        case Prefs::ALT_SPEED_LIMIT_TIME_END:
        case Prefs::ALT_SPEED_LIMIT_UP:
        case Prefs::BLOCKLIST_DATE:
        case Prefs::BLOCKLIST_ENABLED:
        case Prefs::BLOCKLIST_URL:
        case Prefs::DHT_ENABLED:
        case Prefs::DOWNLOAD_QUEUE_ENABLED:
        case Prefs::DOWNLOAD_QUEUE_SIZE:
        case Prefs::DSPEED:
        case Prefs::DSPEED_ENABLED:
        case Prefs::IDLE_LIMIT:
        case Prefs::IDLE_LIMIT_ENABLED:
        case Prefs::INCOMPLETE_DIR:
        case Prefs::INCOMPLETE_DIR_ENABLED:
        case Prefs::LPD_ENABLED:
        case Prefs::PEER_LIMIT_GLOBAL:
        case Prefs::PEER_LIMIT_TORRENT:
        case Prefs::PEER_PORT:
        case Prefs::PEER_PORT_RANDOM_ON_START:
        case Prefs::QUEUE_STALLED_MINUTES:
        case Prefs::PEX_ENABLED:
        case Prefs::PORT_FORWARDING:
        case Prefs::RENAME_PARTIAL_FILES:
        case Prefs::SCRIPT_TORRENT_DONE_ENABLED:
        case Prefs::SCRIPT_TORRENT_DONE_FILENAME:
        case Prefs::SCRIPT_TORRENT_DONE_SEEDING_ENABLED:
        case Prefs::SCRIPT_TORRENT_DONE_SEEDING_FILENAME:
        case Prefs::START:
        case Prefs::TRASH_ORIGINAL:
        case Prefs::USPEED:
        case Prefs::USPEED_ENABLED:
        case Prefs::UTP_ENABLED:
            sessionSet(prefs_.getKey(key), prefs_.variant(key));
            break;

        case Prefs::DOWNLOAD_DIR:
            sessionSet(prefs_.getKey(key), prefs_.variant(key));
            /* this will change the 'freespace' argument, so refresh */
            refreshSessionInfo();
            break;

        case Prefs::RATIO:
            sessionSet(TR_KEY_seedRatioLimit, prefs_.variant(key));
            break;

        case Prefs::RATIO_ENABLED:
            sessionSet(TR_KEY_seedRatioLimited, prefs_.variant(key));
            break;

        case Prefs::ENCRYPTION:
            switch (int const i = prefs_.variant(key).toInt(); i)
            {
            case 0:
                sessionSet(prefs_.getKey(key), QStringLiteral("tolerated"));
                break;

            case 1:
                sessionSet(prefs_.getKey(key), QStringLiteral("preferred"));
                break;

            case 2:
                sessionSet(prefs_.getKey(key), QStringLiteral("required"));
                break;
            }

            break;

        case Prefs::RPC_AUTH_REQUIRED:
            if (session_ != nullptr)
            {
                tr_sessionSetRPCPasswordEnabled(session_, prefs_.getBool(key));
            }

            break;

        case Prefs::RPC_ENABLED:
            if (session_ != nullptr)
            {
                tr_sessionSetRPCEnabled(session_, prefs_.getBool(key));
            }

            break;

        case Prefs::RPC_PASSWORD:
            if (session_ != nullptr)
            {
                tr_sessionSetRPCPassword(session_, prefs_.getString(key).toUtf8().constData());
            }

            break;

        case Prefs::RPC_PORT:
            if (session_ != nullptr)
            {
                tr_sessionSetRPCPort(session_, static_cast<tr_port>(prefs_.getInt(key)));
            }

            break;

        case Prefs::RPC_USERNAME:
            if (session_ != nullptr)
            {
                tr_sessionSetRPCUsername(session_, prefs_.getString(key).toUtf8().constData());
            }

            break;

        case Prefs::RPC_WHITELIST_ENABLED:
            if (session_ != nullptr)
            {
                tr_sessionSetRPCWhitelistEnabled(session_, prefs_.getBool(key));
            }

            break;

        case Prefs::RPC_WHITELIST:
            if (session_ != nullptr)
            {
                tr_sessionSetRPCWhitelist(session_, prefs_.getString(key).toUtf8().constData());
            }

            break;

        default:
            qWarning() << "unhandled pref:" << key;
        }
    }
}

/***
****
***/

Session::Session(QString config_dir, Prefs& prefs)
    : config_dir_(std::move(config_dir))
    , prefs_(prefs)
{
    stats_ = {};
    stats_.ratio = TR_RATIO_NA;
    cumulative_stats_ = stats_;

    connect(&prefs_, &Prefs::changed, this, &Session::updatePref);
    connect(&rpc_, &RpcClient::httpAuthenticationRequired, this, &Session::httpAuthenticationRequired);
    connect(&rpc_, &RpcClient::dataReadProgress, this, &Session::dataReadProgress);
    connect(&rpc_, &RpcClient::dataSendProgress, this, &Session::dataSendProgress);
    connect(&rpc_, &RpcClient::networkResponse, this, &Session::networkResponse);

    duplicates_timer_.setSingleShot(true);
    connect(&duplicates_timer_, &QTimer::timeout, this, &Session::onDuplicatesTimer);
}

Session::~Session()
{
    stop();
}

/***
****
***/

void Session::stop()
{
    rpc_.stop();

    if (session_ != nullptr)
    {
        tr_sessionClose(session_);
        session_ = nullptr;
    }
}

void Session::restart()
{
    stop();
    start();
}

void Session::start()
{
    if (prefs_.get<bool>(Prefs::SESSION_IS_REMOTE))
    {
        QUrl url;
        url.setScheme(QStringLiteral("http"));
        url.setHost(prefs_.get<QString>(Prefs::SESSION_REMOTE_HOST));
        url.setPort(prefs_.get<int>(Prefs::SESSION_REMOTE_PORT));
        url.setPath(QStringLiteral("/transmission/rpc"));

        if (prefs_.get<bool>(Prefs::SESSION_REMOTE_AUTH))
        {
            url.setUserName(prefs_.get<QString>(Prefs::SESSION_REMOTE_USERNAME));
            url.setPassword(prefs_.get<QString>(Prefs::SESSION_REMOTE_PASSWORD));
        }

        rpc_.start(url);
    }
    else
    {
        tr_variant settings;
        tr_variantInitDict(&settings, 0);
        tr_sessionLoadSettings(&settings, config_dir_.toUtf8().constData(), "qt");
        session_ = tr_sessionInit(config_dir_.toUtf8().constData(), true, &settings);
        tr_variantFree(&settings);

        rpc_.start(session_);

        auto* const ctor = tr_ctorNew(session_);
        tr_free(tr_sessionLoadTorrents(session_, ctor, nullptr));
        tr_ctorFree(ctor);
    }

    emit sourceChanged();
}

bool Session::isServer() const
{
    return session_ != nullptr;
}

bool Session::isLocal() const
{
    if (!session_id_.isEmpty())
    {
        return is_definitely_local_session_;
    }

    return rpc_.isLocal();
}

/***
****
***/

void Session::addOptionalIds(tr_variant* args, torrent_ids_t const& ids) const
{
    auto constexpr RecentlyActiveKey = std::string_view{ "recently-active" };

    if (&ids == &RecentlyActiveIDs)
    {
        dictAdd(args, TR_KEY_ids, RecentlyActiveKey);
    }
    else if (!ids.empty())
    {
        dictAdd(args, TR_KEY_ids, ids);
    }
}

Session::Tag Session::torrentSetImpl(tr_variant* args)
{
    auto* const q = new RpcQueue();
    auto const tag = q->tag();

    q->add([this, args]() { return rpc_.exec(TR_KEY_torrent_set, args); });
    q->add([this, tag]() { emit sessionCalled(tag); });
    q->setTolerateErrors();
    q->run();

    return tag;
}

Session::Tag Session::torrentSet(torrent_ids_t const& ids, tr_quark const key, double value)
{
    tr_variant args;
    tr_variantInitDict(&args, 2);
    addOptionalIds(&args, ids);
    dictAdd(&args, key, value);
    return torrentSetImpl(&args);
}

Session::Tag Session::torrentSet(torrent_ids_t const& ids, tr_quark const key, int value)
{
    tr_variant args;
    tr_variantInitDict(&args, 2);
    addOptionalIds(&args, ids);
    dictAdd(&args, key, value);
    return torrentSetImpl(&args);
}

Session::Tag Session::torrentSet(torrent_ids_t const& ids, tr_quark const key, bool value)
{
    tr_variant args;
    tr_variantInitDict(&args, 2);
    addOptionalIds(&args, ids);
    dictAdd(&args, key, value);
    return torrentSetImpl(&args);
}

Session::Tag Session::torrentSet(torrent_ids_t const& ids, tr_quark const key, QString const& value)
{
    tr_variant args;
    tr_variantInitDict(&args, 2);
    addOptionalIds(&args, ids);
    dictAdd(&args, key, value);
    return torrentSetImpl(&args);
}

Session::Tag Session::torrentSet(torrent_ids_t const& ids, tr_quark const key, QStringList const& value)
{
    tr_variant args;
    tr_variantInitDict(&args, 2);
    addOptionalIds(&args, ids);
    dictAdd(&args, key, value);
    return torrentSetImpl(&args);
}

Session::Tag Session::torrentSet(torrent_ids_t const& ids, tr_quark const key, QList<int> const& value)
{
    tr_variant args;
    tr_variantInitDict(&args, 2);
    addOptionalIds(&args, ids);
    dictAdd(&args, key, value);
    return torrentSetImpl(&args);
}

Session::Tag Session::torrentSet(torrent_ids_t const& ids, tr_quark const key, QPair<int, QString> const& value)
{
    tr_variant args;
    tr_variantInitDict(&args, 2);
    addOptionalIds(&args, ids);
    tr_variant* list(tr_variantDictAddList(&args, key, 2));
    listAdd(list, value.first);
    listAdd(list, value.second);
    return torrentSetImpl(&args);
}

void Session::torrentSetLocation(torrent_ids_t const& ids, QString const& location, bool do_move)
{
    tr_variant args;
    tr_variantInitDict(&args, 3);
    addOptionalIds(&args, ids);
    dictAdd(&args, TR_KEY_location, location);
    dictAdd(&args, TR_KEY_move, do_move);

    exec(TR_KEY_torrent_set_location, &args);
}

void Session::torrentRenamePath(torrent_ids_t const& ids, QString const& oldpath, QString const& newname)
{
    tr_variant args;
    tr_variantInitDict(&args, 2);
    addOptionalIds(&args, ids);
    dictAdd(&args, TR_KEY_path, oldpath);
    dictAdd(&args, TR_KEY_name, newname);

    auto* q = new RpcQueue();

    q->add(
        [this, &args]() { return exec("torrent-rename-path", &args); },
        [](RpcResponse const& r)
        {
            auto str = dictFind<QString>(r.args.get(), TR_KEY_path);
            auto const path = str ? *str : QStringLiteral("(unknown)");
            str = dictFind<QString>(r.args.get(), TR_KEY_name);
            auto const name = str ? *str : QStringLiteral("(unknown)");

            auto* d = new QMessageBox(
                QMessageBox::Information,
                tr("Error Renaming Path"),
                tr(R"(<p><b>Unable to rename "%1" as "%2": %3.</b></p><p>Please correct the errors and try again.</p>)")
                    .arg(path)
                    .arg(name)
                    .arg(r.result),
                QMessageBox::Close,
                QApplication::activeWindow());
            QObject::connect(d, &QMessageBox::rejected, d, &QMessageBox::deleteLater);
            d->show();
        });

    q->add([this, ids](RpcResponse const& /*r*/) { refreshTorrents(ids, TorrentProperties::Rename); });

    q->run();
}

std::vector<std::string_view> const& Session::getKeyNames(TorrentProperties props)
{
    std::vector<std::string_view>& names = names_[props];

    if (names.empty())
    {
        // unchanging fields needed by the main window
        static auto constexpr MainInfoKeys = std::array<tr_quark, 8>{
            TR_KEY_addedDate, //
            TR_KEY_downloadDir, //
            TR_KEY_file_count, //
            TR_KEY_hashString, //
            TR_KEY_name, //
            TR_KEY_primary_mime_type, //
            TR_KEY_totalSize, //
            TR_KEY_trackers, //
        };

        // changing fields needed by the main window
        static auto constexpr MainStatKeys = std::array<tr_quark, 25>{
            TR_KEY_downloadedEver,
            TR_KEY_editDate,
            TR_KEY_error,
            TR_KEY_errorString,
            TR_KEY_eta,
            TR_KEY_haveUnchecked,
            TR_KEY_haveValid,
            TR_KEY_isFinished,
            TR_KEY_leftUntilDone,
            TR_KEY_manualAnnounceTime,
            TR_KEY_metadataPercentComplete,
            TR_KEY_peersConnected,
            TR_KEY_peersGettingFromUs,
            TR_KEY_peersSendingToUs,
            TR_KEY_percentDone,
            TR_KEY_queuePosition,
            TR_KEY_rateDownload,
            TR_KEY_rateUpload,
            TR_KEY_recheckProgress,
            TR_KEY_seedRatioLimit,
            TR_KEY_seedRatioMode,
            TR_KEY_sizeWhenDone,
            TR_KEY_status,
            TR_KEY_uploadedEver,
            TR_KEY_webseedsSendingToUs,
        };

        // unchanging fields needed by the details dialog
        static auto constexpr DetailInfoKeys = std::array<tr_quark, 9>{
            TR_KEY_comment, //
            TR_KEY_creator, //
            TR_KEY_dateCreated, //
            TR_KEY_files, //
            TR_KEY_isPrivate, //
            TR_KEY_pieceCount, //
            TR_KEY_pieceSize, //
            TR_KEY_trackerList, //
            TR_KEY_trackers, //
        };

        // changing fields needed by the details dialog
        static auto constexpr DetailStatKeys = std::array<tr_quark, 18>{
            TR_KEY_activityDate, //
            TR_KEY_bandwidthPriority, //
            TR_KEY_corruptEver, //
            TR_KEY_desiredAvailable, //
            TR_KEY_downloadedEver, //
            TR_KEY_downloadLimit, //
            TR_KEY_downloadLimited, //
            TR_KEY_fileStats, //
            TR_KEY_haveUnchecked, //
            TR_KEY_honorsSessionLimits, //
            TR_KEY_peer_limit, //
            TR_KEY_peers, //
            TR_KEY_seedIdleLimit, //
            TR_KEY_seedIdleMode, //
            TR_KEY_startDate, //
            TR_KEY_trackerStats, //
            TR_KEY_uploadLimit, //
            TR_KEY_uploadLimited, //
        };

        // keys needed after renaming a torrent
        static auto constexpr RenameKeys = std::array<tr_quark, 3>{
            TR_KEY_fileStats,
            TR_KEY_files,
            TR_KEY_name,
        };

        auto const append = [&names](tr_quark key)
        {
            size_t len = {};
            char const* str = tr_quark_get_string(key, &len);
            names.emplace_back(str, len);
        };

        switch (props)
        {
        case TorrentProperties::DetailInfo:
            std::for_each(DetailInfoKeys.begin(), DetailInfoKeys.end(), append);
            break;

        case TorrentProperties::DetailStat:
            std::for_each(DetailStatKeys.begin(), DetailStatKeys.end(), append);
            break;

        case TorrentProperties::MainAll:
            std::for_each(MainInfoKeys.begin(), MainInfoKeys.end(), append);
            std::for_each(MainStatKeys.begin(), MainStatKeys.end(), append);
            break;

        case TorrentProperties::MainInfo:
            std::for_each(MainInfoKeys.begin(), MainInfoKeys.end(), append);
            break;

        case TorrentProperties::MainStats:
            std::for_each(MainStatKeys.begin(), MainStatKeys.end(), append);
            break;

        case TorrentProperties::Rename:
            std::for_each(RenameKeys.begin(), RenameKeys.end(), append);
            break;
        }

        // must be in every torrent req
        append(TR_KEY_id);

        // sort and remove dupes
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
    }

    return names;
}

void Session::refreshTorrents(torrent_ids_t const& ids, TorrentProperties props)
{
    auto constexpr Table = std::string_view{ "table" };

    tr_variant args;
    tr_variantInitDict(&args, 3);
    dictAdd(&args, TR_KEY_format, Table);
    dictAdd(&args, TR_KEY_fields, getKeyNames(props));
    addOptionalIds(&args, ids);

    auto* q = new RpcQueue();

    q->add([this, &args]() { return exec(TR_KEY_torrent_get, &args); });

    bool const all_torrents = ids.empty();

    q->add(
        [this, all_torrents](RpcResponse const& r)
        {
            tr_variant* torrents = nullptr;

            if (tr_variantDictFindList(r.args.get(), TR_KEY_torrents, &torrents))
            {
                emit torrentsUpdated(torrents, all_torrents);
            }

            if (tr_variantDictFindList(r.args.get(), TR_KEY_removed, &torrents))
            {
                emit torrentsRemoved(torrents);
            }
        });

    q->run();
}

void Session::refreshDetailInfo(torrent_ids_t const& ids)
{
    refreshTorrents(ids, TorrentProperties::DetailInfo);
}

void Session::refreshExtraStats(torrent_ids_t const& ids)
{
    refreshTorrents(ids, TorrentProperties::DetailStat);
}

void Session::sendTorrentRequest(std::string_view request, torrent_ids_t const& ids)
{
    tr_variant args;
    tr_variantInitDict(&args, 1);
    addOptionalIds(&args, ids);

    auto* q = new RpcQueue();

    q->add([this, request, &args]() { return exec(request, &args); });

    q->add([this, ids]() { refreshTorrents(ids, TorrentProperties::MainStats); });

    q->run();
}

void Session::pauseTorrents(torrent_ids_t const& ids)
{
    sendTorrentRequest("torrent-stop", ids);
}

void Session::startTorrents(torrent_ids_t const& ids)
{
    sendTorrentRequest("torrent-start", ids);
}

void Session::startTorrentsNow(torrent_ids_t const& ids)
{
    sendTorrentRequest("torrent-start-now", ids);
}

void Session::queueMoveTop(torrent_ids_t const& ids)
{
    sendTorrentRequest("queue-move-top", ids);
}

void Session::queueMoveUp(torrent_ids_t const& ids)
{
    sendTorrentRequest("queue-move-up", ids);
}

void Session::queueMoveDown(torrent_ids_t const& ids)
{
    sendTorrentRequest("queue-move-down", ids);
}

void Session::queueMoveBottom(torrent_ids_t const& ids)
{
    sendTorrentRequest("queue-move-bottom", ids);
}

void Session::refreshActiveTorrents()
{
    // If this object is passed as "ids" (compared by address), then recently active torrents are queried.
    refreshTorrents(RecentlyActiveIDs, TorrentProperties::MainStats);
}

void Session::refreshAllTorrents()
{
    // if an empty ids object is used, all torrents are queried.
    torrent_ids_t const ids = {};
    refreshTorrents(ids, TorrentProperties::MainStats);
}

void Session::initTorrents(torrent_ids_t const& ids)
{
    refreshTorrents(ids, TorrentProperties::MainAll);
}

void Session::refreshSessionStats()
{
    auto* q = new RpcQueue();

    q->add([this]() { return exec("session-stats", nullptr); });

    q->add([this](RpcResponse const& r) { updateStats(r.args.get()); });

    q->run();
}

void Session::refreshSessionInfo()
{
    auto* q = new RpcQueue();

    q->add([this]() { return exec("session-get", nullptr); });

    q->add([this](RpcResponse const& r) { updateInfo(r.args.get()); });

    q->run();
}

void Session::updateBlocklist()
{
    auto* q = new RpcQueue();

    q->add([this]() { return exec("blocklist-update", nullptr); });

    q->add(
        [this](RpcResponse const& r)
        {
            if (auto const size = dictFind<int>(r.args.get(), TR_KEY_blocklist_size); size)
            {
                setBlocklistSize(*size);
            }
        });

    q->run();
}

/***
****
***/

RpcResponseFuture Session::exec(tr_quark method, tr_variant* args)
{
    return rpc_.exec(method, args);
}

RpcResponseFuture Session::exec(std::string_view method, tr_variant* args)
{
    return rpc_.exec(method, args);
}

void Session::updateStats(tr_variant* d, tr_session_stats* stats)
{
    auto value = dictFind<uint64_t>(d, TR_KEY_uploadedBytes);
    if (value)
    {
        stats->uploadedBytes = *value;
    }

    if ((value = dictFind<uint64_t>(d, TR_KEY_downloadedBytes)))
    {
        stats->downloadedBytes = *value;
    }

    if ((value = dictFind<uint64_t>(d, TR_KEY_filesAdded)))
    {
        stats->filesAdded = *value;
    }

    if ((value = dictFind<uint64_t>(d, TR_KEY_sessionCount)))
    {
        stats->sessionCount = *value;
    }

    if ((value = dictFind<uint64_t>(d, TR_KEY_secondsActive)))
    {
        stats->secondsActive = *value;
    }

    stats->ratio = static_cast<float>(tr_getRatio(stats->uploadedBytes, stats->downloadedBytes));
}

void Session::updateStats(tr_variant* dict)
{
    if (tr_variant* var = nullptr; tr_variantDictFindDict(dict, TR_KEY_current_stats, &var))
    {
        updateStats(var, &stats_);
    }

    if (tr_variant* var = nullptr; tr_variantDictFindDict(dict, TR_KEY_cumulative_stats, &var))
    {
        updateStats(var, &cumulative_stats_);
    }

    emit statsUpdated();
}

void Session::updateInfo(tr_variant* d)
{
    disconnect(&prefs_, &Prefs::changed, this, &Session::updatePref);

    for (int i = Prefs::FIRST_CORE_PREF; i <= Prefs::LAST_CORE_PREF; ++i)
    {
        tr_variant const* b(tr_variantDictFind(d, prefs_.getKey(i)));

        if (b == nullptr)
        {
            continue;
        }

        if (i == Prefs::ENCRYPTION)
        {
            if (auto const str = getValue<QString>(b); str)
            {
                if (*str == QStringLiteral("required"))
                {
                    prefs_.set(i, 2);
                }
                else if (*str == QStringLiteral("preferred"))
                {
                    prefs_.set(i, 1);
                }
                else if (*str == QStringLiteral("tolerated"))
                {
                    prefs_.set(i, 0);
                }
            }

            continue;
        }

        switch (prefs_.type(i))
        {
        case QVariant::Int:
            if (auto const value = getValue<int>(b); value)
            {
                prefs_.set(i, *value);
            }

            break;

        case QVariant::Double:
            if (auto const value = getValue<double>(b); value)
            {
                prefs_.set(i, *value);
            }

            break;

        case QVariant::Bool:
            if (auto const value = getValue<bool>(b); value)
            {
                prefs_.set(i, *value);
            }

            break;

        case CustomVariantType::FilterModeType:
        case CustomVariantType::SortModeType:
        case QVariant::String:
            if (auto const value = getValue<QString>(b); value)
            {
                prefs_.set(i, *value);
            }

            break;

        default:
            break;
        }
    }

    if (auto const b = dictFind<bool>(d, TR_KEY_seedRatioLimited); b)
    {
        prefs_.set(Prefs::RATIO_ENABLED, *b);
    }

    if (auto const x = dictFind<double>(d, TR_KEY_seedRatioLimit); x)
    {
        prefs_.set(Prefs::RATIO, *x);
    }

    /* Use the C API to get settings that, for security reasons, aren't supported by RPC */
    if (session_ != nullptr)
    {
        prefs_.set(Prefs::RPC_ENABLED, tr_sessionIsRPCEnabled(session_));
        prefs_.set(Prefs::RPC_AUTH_REQUIRED, tr_sessionIsRPCPasswordEnabled(session_));
        prefs_.set(Prefs::RPC_PASSWORD, QString::fromUtf8(tr_sessionGetRPCPassword(session_)));
        prefs_.set(Prefs::RPC_PORT, tr_sessionGetRPCPort(session_));
        prefs_.set(Prefs::RPC_USERNAME, QString::fromUtf8(tr_sessionGetRPCUsername(session_)));
        prefs_.set(Prefs::RPC_WHITELIST_ENABLED, tr_sessionGetRPCWhitelistEnabled(session_));
        prefs_.set(Prefs::RPC_WHITELIST, QString::fromUtf8(tr_sessionGetRPCWhitelist(session_)));
    }

    if (auto const size = dictFind<int>(d, TR_KEY_blocklist_size); size && *size != blocklistSize())
    {
        setBlocklistSize(*size);
    }

    if (auto const str = dictFind<QString>(d, TR_KEY_version); str)
    {
        session_version_ = *str;
    }

    if (auto const str = dictFind<QString>(d, TR_KEY_session_id); str)
    {
        session_id_ = *str;
        is_definitely_local_session_ = tr_session_id_is_local(session_id_.toUtf8().constData());
    }
    else
    {
        session_id_.clear();
    }

    connect(&prefs_, &Prefs::changed, this, &Session::updatePref);

    emit sessionUpdated();
}

void Session::setBlocklistSize(int64_t i)
{
    blocklist_size_ = i;

    emit blocklistUpdated(i);
}

void Session::addTorrent(AddData const& add_me, tr_variant* args, bool trash_original)
{
    assert(tr_variantDictFind(args, TR_KEY_filename) == nullptr);
    assert(tr_variantDictFind(args, TR_KEY_metainfo) == nullptr);

    if (tr_variantDictFind(args, TR_KEY_paused) == nullptr)
    {
        dictAdd(args, TR_KEY_paused, !prefs_.getBool(Prefs::START));
    }

    switch (add_me.type)
    {
    case AddData::MAGNET:
        dictAdd(args, TR_KEY_filename, add_me.magnet);
        break;

    case AddData::URL:
        dictAdd(args, TR_KEY_filename, add_me.url.toString());
        break;

    case AddData::FILENAME:
        [[fallthrough]];
    case AddData::METAINFO:
        dictAdd(args, TR_KEY_metainfo, add_me.toBase64());
        break;

    default:
        qWarning() << "Unhandled AddData type: " << add_me.type;
        break;
    }

    auto* q = new RpcQueue();

    q->add(
        [this, args]() { return exec("torrent-add", args); },
        [add_me](RpcResponse const& r)
        {
            auto* d = new QMessageBox(
                QMessageBox::Warning,
                tr("Error Adding Torrent"),
                QStringLiteral("<p><b>%1</b></p><p>%2</p>").arg(r.result).arg(add_me.readableName()),
                QMessageBox::Close,
                QApplication::activeWindow());
            QObject::connect(d, &QMessageBox::rejected, d, &QMessageBox::deleteLater);
            d->show();
        });

    q->add(
        [this, add_me](RpcResponse const& r)
        {
            bool session_has_torrent = false;

            if (tr_variant* dup = nullptr; tr_variantDictFindDict(r.args.get(), TR_KEY_torrent_added, &dup))
            {
                session_has_torrent = true;
            }
            else if (tr_variantDictFindDict(r.args.get(), TR_KEY_torrent_duplicate, &dup))
            {
                session_has_torrent = true;

                auto const hash = dictFind<QString>(dup, TR_KEY_hashString);
                if (hash)
                {
                    duplicates_.try_emplace(add_me.readableShortName(), *hash);
                    duplicates_timer_.start(1000);
                }
            }

            if (session_has_torrent && !add_me.filename.isEmpty())
            {
                QFile(add_me.filename).rename(QStringLiteral("%1.added").arg(add_me.filename));
            }
        });

    if (trash_original && add_me.type == AddData::FILENAME)
    {
        q->add(
            [add_me]()
            {
                QFile original(add_me.filename);
                original.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
                original.remove();
            });
    }

    q->run();
}

void Session::onDuplicatesTimer()
{
    decltype(duplicates_) duplicates;
    duplicates.swap(duplicates_);

    QStringList lines;
    for (auto [dupe, original] : duplicates)
    {
        lines.push_back(tr("%1 (copy of %2)").arg(dupe).arg(original.left(7)));
    }

    if (!lines.empty())
    {
        lines.sort(Qt::CaseInsensitive);
        auto const title = tr("Duplicate Torrent(s)", "", lines.size());
        auto const detail = lines.join(QStringLiteral("\n"));
        auto const detail_text = tr("Unable to add %n duplicate torrent(s)", "", lines.size());
        auto const use_detail = lines.size() > 1;
        auto const text = use_detail ? detail_text : detail;

        auto* d = new QMessageBox(QMessageBox::Warning, title, text, QMessageBox::Close, QApplication::activeWindow());
        if (use_detail)
        {
            d->setDetailedText(detail);
        }

        QObject::connect(d, &QMessageBox::rejected, d, &QMessageBox::deleteLater);
        d->show();
    }
}

void Session::addTorrent(AddData const& add_me)
{
    tr_variant args;
    tr_variantInitDict(&args, 3);

    addTorrent(add_me, &args, prefs_.getBool(Prefs::TRASH_ORIGINAL));
}

void Session::addNewlyCreatedTorrent(QString const& filename, QString const& local_path)
{
    QByteArray const b64 = AddData(filename).toBase64();

    tr_variant args;
    tr_variantInitDict(&args, 3);
    dictAdd(&args, TR_KEY_download_dir, local_path);
    dictAdd(&args, TR_KEY_paused, !prefs_.getBool(Prefs::START));
    dictAdd(&args, TR_KEY_metainfo, b64);

    exec("torrent-add", &args);
}

void Session::removeTorrents(torrent_ids_t const& ids, bool delete_files)
{
    if (!ids.empty())
    {
        tr_variant args;
        tr_variantInitDict(&args, 2);
        addOptionalIds(&args, ids);
        dictAdd(&args, TR_KEY_delete_local_data, delete_files);

        exec("torrent-remove", &args);
    }
}

void Session::verifyTorrents(torrent_ids_t const& ids)
{
    if (!ids.empty())
    {
        tr_variant args;
        tr_variantInitDict(&args, 1);
        addOptionalIds(&args, ids);

        exec("torrent-verify", &args);
    }
}

void Session::reannounceTorrents(torrent_ids_t const& ids)
{
    if (!ids.empty())
    {
        tr_variant args;
        tr_variantInitDict(&args, 1);
        addOptionalIds(&args, ids);

        exec("torrent-reannounce", &args);
    }
}

/***
****
***/

void Session::launchWebInterface() const
{
    QUrl url;

    if (session_ == nullptr) // remote session
    {
        url = rpc_.url();
        url.setPath(QStringLiteral("/transmission/web/"));
    }
    else // local session
    {
        url.setScheme(QStringLiteral("http"));
        url.setHost(QStringLiteral("localhost"));
        url.setPort(prefs_.getInt(Prefs::RPC_PORT));
    }

    QDesktopServices::openUrl(url);
}
