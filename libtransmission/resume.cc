// This file Copyright © 2008-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <cstring>
#include <ctime>
#include <string_view>
#include <vector>

#include "transmission.h"

#include "error.h"
#include "file.h"
#include "log.h"
#include "magnet-metainfo.h"
#include "peer-mgr.h" /* pex */
#include "resume.h"
#include "session.h"
#include "torrent.h"
#include "tr-assert.h"
#include "utils.h"
#include "variant.h"

using namespace std::literals;

namespace
{

constexpr int MAX_REMEMBERED_PEERS = 200;

} // unnamed namespace

/***
****
***/

static void savePeers(tr_variant* dict, tr_torrent const* tor)
{
    tr_pex* pex = nullptr;
    int count = tr_peerMgrGetPeers(tor, &pex, TR_AF_INET, TR_PEERS_INTERESTING, MAX_REMEMBERED_PEERS);

    if (count > 0)
    {
        tr_variantDictAddRaw(dict, TR_KEY_peers2, pex, sizeof(tr_pex) * count);
    }

    tr_free(pex);

    count = tr_peerMgrGetPeers(tor, &pex, TR_AF_INET6, TR_PEERS_INTERESTING, MAX_REMEMBERED_PEERS);

    if (count > 0)
    {
        tr_variantDictAddRaw(dict, TR_KEY_peers2_6, pex, sizeof(tr_pex) * count);
    }

    tr_free(pex);
}

static size_t addPeers(tr_torrent* tor, uint8_t const* buf, size_t buflen)
{
    size_t const n_in = buflen / sizeof(tr_pex);
    size_t const n_pex = std::min(n_in, size_t{ MAX_REMEMBERED_PEERS });

    tr_pex pex[MAX_REMEMBERED_PEERS];
    memcpy(pex, buf, sizeof(tr_pex) * n_pex);
    return tr_peerMgrAddPex(tor, TR_PEER_FROM_RESUME, pex, n_pex);
}

static auto loadPeers(tr_variant* dict, tr_torrent* tor)
{
    auto ret = tr_resume::fields_t{};

    uint8_t const* str = nullptr;
    auto len = size_t{};
    if (tr_variantDictFindRaw(dict, TR_KEY_peers2, &str, &len))
    {
        size_t const numAdded = addPeers(tor, str, len);
        tr_logAddTorDbg(tor, "Loaded %zu IPv4 peers from resume file", numAdded);
        ret = tr_resume::Peers;
    }

    if (tr_variantDictFindRaw(dict, TR_KEY_peers2_6, &str, &len))
    {
        size_t const numAdded = addPeers(tor, str, len);
        tr_logAddTorDbg(tor, "Loaded %zu IPv6 peers from resume file", numAdded);
        ret = tr_resume::Peers;
    }

    return ret;
}

/***
****
***/

static void saveLabels(tr_variant* dict, tr_torrent const* tor)
{
    auto const& labels = tor->labels;
    tr_variant* list = tr_variantDictAddList(dict, TR_KEY_labels, std::size(labels));
    for (auto const& label : labels)
    {
        tr_variantListAddStr(list, label);
    }
}

static auto loadLabels(tr_variant* dict, tr_torrent* tor)
{
    tr_variant* list = nullptr;
    if (!tr_variantDictFindList(dict, TR_KEY_labels, &list))
    {
        return tr_resume::fields_t{};
    }

    int const n = tr_variantListSize(list);
    for (int i = 0; i < n; ++i)
    {
        auto sv = std::string_view{};
        if (tr_variantGetStrView(tr_variantListChild(list, i), &sv) && !std::empty(sv))
        {
            tor->labels.emplace(sv);
        }
    }

    return tr_resume::Labels;
}

/***
****
***/

static void saveDND(tr_variant* dict, tr_torrent const* tor)
{
    auto const n = tor->fileCount();
    tr_variant* const list = tr_variantDictAddList(dict, TR_KEY_dnd, n);

    for (tr_file_index_t i = 0; i < n; ++i)
    {
        tr_variantListAddBool(list, !tr_torrentFile(tor, i).wanted);
    }
}

static auto loadDND(tr_variant* dict, tr_torrent* tor)
{
    auto ret = tr_resume::fields_t{};
    tr_variant* list = nullptr;
    auto const n = tor->fileCount();

    if (tr_variantDictFindList(dict, TR_KEY_dnd, &list) && tr_variantListSize(list) == n)
    {
        auto wanted = std::vector<tr_file_index_t>{};
        auto unwanted = std::vector<tr_file_index_t>{};
        wanted.reserve(n);
        unwanted.reserve(n);

        for (tr_file_index_t i = 0; i < n; ++i)
        {
            auto tmp = false;
            if (tr_variantGetBool(tr_variantListChild(list, i), &tmp) && tmp)
            {
                unwanted.push_back(i);
            }
            else
            {
                wanted.push_back(i);
            }
        }

        tor->initFilesWanted(std::data(unwanted), std::size(unwanted), false);
        tor->initFilesWanted(std::data(wanted), std::size(wanted), true);

        ret = tr_resume::Dnd;
    }
    else
    {
        tr_logAddTorDbg(
            tor,
            "Couldn't load DND flags. DND list (%p) has %zu"
            " children; torrent has %d files",
            (void*)list,
            tr_variantListSize(list),
            (int)n);
    }

    return ret;
}

/***
****
***/

static void saveFilePriorities(tr_variant* dict, tr_torrent const* tor)
{
    auto const n = tor->fileCount();

    tr_variant* const list = tr_variantDictAddList(dict, TR_KEY_priority, n);
    for (tr_file_index_t i = 0; i < n; ++i)
    {
        tr_variantListAddInt(list, tr_torrentFile(tor, i).priority);
    }
}

static auto loadFilePriorities(tr_variant* dict, tr_torrent* tor)
{
    auto ret = tr_resume::fields_t{};

    auto const n = tor->fileCount();
    tr_variant* list = nullptr;
    if (tr_variantDictFindList(dict, TR_KEY_priority, &list) && tr_variantListSize(list) == n)
    {
        for (tr_file_index_t i = 0; i < n; ++i)
        {
            auto priority = int64_t{};
            if (tr_variantGetInt(tr_variantListChild(list, i), &priority))
            {
                tor->setFilePriority(i, tr_priority_t(priority));
            }
        }

        ret = tr_resume::FilePriorities;
    }

    return ret;
}

/***
****
***/

static void saveSingleSpeedLimit(tr_variant* d, tr_torrent* tor, tr_direction dir)
{
    tr_variantDictReserve(d, 3);
    tr_variantDictAddInt(d, TR_KEY_speed_Bps, tor->speedLimitBps(dir));
    tr_variantDictAddBool(d, TR_KEY_use_global_speed_limit, tr_torrentUsesSessionLimits(tor));
    tr_variantDictAddBool(d, TR_KEY_use_speed_limit, tr_torrentUsesSpeedLimit(tor, dir));
}

static void saveSpeedLimits(tr_variant* dict, tr_torrent* tor)
{
    saveSingleSpeedLimit(tr_variantDictAddDict(dict, TR_KEY_speed_limit_down, 0), tor, TR_DOWN);
    saveSingleSpeedLimit(tr_variantDictAddDict(dict, TR_KEY_speed_limit_up, 0), tor, TR_UP);
}

static void saveRatioLimits(tr_variant* dict, tr_torrent* tor)
{
    tr_variant* d = tr_variantDictAddDict(dict, TR_KEY_ratio_limit, 2);
    tr_variantDictAddReal(d, TR_KEY_ratio_limit, tr_torrentGetRatioLimit(tor));
    tr_variantDictAddInt(d, TR_KEY_ratio_mode, tr_torrentGetRatioMode(tor));
}

static void saveIdleLimits(tr_variant* dict, tr_torrent* tor)
{
    tr_variant* d = tr_variantDictAddDict(dict, TR_KEY_idle_limit, 2);
    tr_variantDictAddInt(d, TR_KEY_idle_limit, tr_torrentGetIdleLimit(tor));
    tr_variantDictAddInt(d, TR_KEY_idle_mode, tr_torrentGetIdleMode(tor));
}

static void loadSingleSpeedLimit(tr_variant* d, tr_direction dir, tr_torrent* tor)
{
    auto i = int64_t{};
    auto boolVal = false;

    if (tr_variantDictFindInt(d, TR_KEY_speed_Bps, &i))
    {
        tor->setSpeedLimitBps(dir, i);
    }
    else if (tr_variantDictFindInt(d, TR_KEY_speed, &i))
    {
        tor->setSpeedLimitBps(dir, i * 1024);
    }

    if (tr_variantDictFindBool(d, TR_KEY_use_speed_limit, &boolVal))
    {
        tr_torrentUseSpeedLimit(tor, dir, boolVal);
    }

    if (tr_variantDictFindBool(d, TR_KEY_use_global_speed_limit, &boolVal))
    {
        tr_torrentUseSessionLimits(tor, boolVal);
    }
}

static auto loadSpeedLimits(tr_variant* dict, tr_torrent* tor)
{
    auto ret = tr_resume::fields_t{};

    tr_variant* d = nullptr;
    if (tr_variantDictFindDict(dict, TR_KEY_speed_limit_up, &d))
    {
        loadSingleSpeedLimit(d, TR_UP, tor);
        ret = tr_resume::Speedlimit;
    }

    if (tr_variantDictFindDict(dict, TR_KEY_speed_limit_down, &d))
    {
        loadSingleSpeedLimit(d, TR_DOWN, tor);
        ret = tr_resume::Speedlimit;
    }

    return ret;
}

static auto loadRatioLimits(tr_variant* dict, tr_torrent* tor)
{
    auto ret = tr_resume::fields_t{};

    if (tr_variant* d = nullptr; tr_variantDictFindDict(dict, TR_KEY_ratio_limit, &d))
    {
        if (auto dratio = double{}; tr_variantDictFindReal(d, TR_KEY_ratio_limit, &dratio))
        {
            tr_torrentSetRatioLimit(tor, dratio);
        }

        if (auto i = int64_t{}; tr_variantDictFindInt(d, TR_KEY_ratio_mode, &i))
        {
            tr_torrentSetRatioMode(tor, tr_ratiolimit(i));
        }

        ret = tr_resume::Ratiolimit;
    }

    return ret;
}

static auto loadIdleLimits(tr_variant* dict, tr_torrent* tor)
{
    auto ret = tr_resume::fields_t{};

    if (tr_variant* d = nullptr; tr_variantDictFindDict(dict, TR_KEY_idle_limit, &d))
    {
        if (auto imin = int64_t{}; tr_variantDictFindInt(d, TR_KEY_idle_limit, &imin))
        {
            tr_torrentSetIdleLimit(tor, imin);
        }

        if (auto i = int64_t{}; tr_variantDictFindInt(d, TR_KEY_idle_mode, &i))
        {
            tr_torrentSetIdleMode(tor, tr_idlelimit(i));
        }

        ret = tr_resume::Idlelimit;
    }

    return ret;
}

/***
****
***/

static void saveName(tr_variant* dict, tr_torrent const* tor)
{
    tr_variantDictAddStrView(dict, TR_KEY_name, tr_torrentName(tor));
}

static auto loadName(tr_variant* dict, tr_torrent* tor)
{
    auto ret = tr_resume::fields_t{};

    auto name = std::string_view{};
    if (!tr_variantDictFindStrView(dict, TR_KEY_name, &name))
    {
        return ret;
    }

    name = tr_strvStrip(name);
    if (std::empty(name))
    {
        return ret;
    }

    tor->setName(name);
    ret |= tr_resume::Name;

    return ret;
}

/***
****
***/

static void saveFilenames(tr_variant* dict, tr_torrent const* tor)
{
    auto const n = tor->fileCount();
    tr_variant* const list = tr_variantDictAddList(dict, TR_KEY_files, n);
    for (tr_file_index_t i = 0; i < n; ++i)
    {
        tr_variantListAddStrView(list, tor->fileSubpath(i));
    }
}

static auto loadFilenames(tr_variant* dict, tr_torrent* tor)
{
    auto ret = tr_resume::fields_t{};

    tr_variant* list = nullptr;
    if (!tr_variantDictFindList(dict, TR_KEY_files, &list))
    {
        return ret;
    }

    auto const n_files = tor->fileCount();
    auto const n_list = tr_variantListSize(list);
    for (size_t i = 0; i < n_files && i < n_list; ++i)
    {
        auto sv = std::string_view{};
        if (tr_variantGetStrView(tr_variantListChild(list, i), &sv) && !std::empty(sv))
        {
            tor->setFileSubpath(i, sv);
        }
    }

    ret |= tr_resume::Filenames;
    return ret;
}

/***
****
***/

static void bitfieldToRaw(tr_bitfield const& b, tr_variant* benc)
{
    if (b.hasNone() || (std::empty(b) != 0U))
    {
        tr_variantInitStr(benc, "none"sv);
    }
    else if (b.hasAll())
    {
        tr_variantInitStrView(benc, "all"sv);
    }
    else
    {
        auto const raw = b.raw();
        tr_variantInitRaw(benc, raw.data(), std::size(raw));
    }
}

static void rawToBitfield(tr_bitfield& bitfield, uint8_t const* raw, size_t rawlen)
{
    if (raw == nullptr || rawlen == 0 || (rawlen == 4 && memcmp(raw, "none", 4) == 0))
    {
        bitfield.setHasNone();
    }
    else if (rawlen == 3 && memcmp(raw, "all", 3) == 0)
    {
        bitfield.setHasAll();
    }
    else
    {
        bitfield.setRaw(raw, rawlen);
    }
}

static void saveProgress(tr_variant* dict, tr_torrent const* tor)
{
    tr_variant* const prog = tr_variantDictAddDict(dict, TR_KEY_progress, 4);

    // add the mtimes
    auto const& mtimes = tor->file_mtimes_;
    auto const n = std::size(mtimes);
    tr_variant* const l = tr_variantDictAddList(prog, TR_KEY_mtimes, n);
    for (auto const& mtime : mtimes)
    {
        tr_variantListAddInt(l, mtime);
    }

    // add the 'checked pieces' bitfield
    bitfieldToRaw(tor->checked_pieces_, tr_variantDictAdd(prog, TR_KEY_pieces));

    /* add the progress */
    if (tor->completeness == TR_SEED)
    {
        tr_variantDictAddStrView(prog, TR_KEY_have, "all"sv);
    }

    /* add the blocks bitfield */
    bitfieldToRaw(tor->blocks(), tr_variantDictAdd(prog, TR_KEY_blocks));
}

/*
 * Transmisison has iterated through a few strategies here, so the
 * code has some added complexity to support older approaches.
 *
 * Current approach: 'progress' is a dict with two entries:
 * - 'pieces' a bitfield for whether each piece has been checked.
 * - 'mtimes', an array of per-file timestamps
 * On startup, 'pieces' is loaded. Then we check to see if the disk
 * mtimes differ from the 'mtimes' list. Changed files have their
 * pieces cleared from the bitset.
 *
 * Second approach (2.20 - 3.00): the 'progress' dict had a
 * 'time_checked' entry which was a list with fileCount items.
 * Each item was either a list of per-piece timestamps, or a
 * single timestamp if either all or none of the pieces had been
 * tested more recently than the file's mtime.
 *
 * First approach (pre-2.20) had an "mtimes" list identical to
 * 3.10, but not the 'pieces' bitfield.
 */
static auto loadProgress(tr_variant* dict, tr_torrent* tor)
{
    if (tr_variant* prog = nullptr; tr_variantDictFindDict(dict, TR_KEY_progress, &prog))
    {
        /// CHECKED PIECES

        auto checked = tr_bitfield(tor->pieceCount());
        auto mtimes = std::vector<time_t>{};
        auto const n_files = tor->fileCount();
        mtimes.reserve(n_files);

        // try to load mtimes
        tr_variant* l = nullptr;
        if (tr_variantDictFindList(prog, TR_KEY_mtimes, &l))
        {
            auto fi = size_t{};
            auto t = int64_t{};
            while (tr_variantGetInt(tr_variantListChild(l, fi++), &t))
            {
                mtimes.push_back(t);
            }
        }

        // try to load the piece-checked bitfield
        uint8_t const* raw = nullptr;
        auto rawlen = size_t{};
        if (tr_variantDictFindRaw(prog, TR_KEY_pieces, &raw, &rawlen))
        {
            rawToBitfield(checked, raw, rawlen);
        }

        // maybe it's a .resume file from [2.20 - 3.00] with the per-piece mtimes
        if (tr_variantDictFindList(prog, TR_KEY_time_checked, &l))
        {
            for (tr_file_index_t fi = 0; fi < n_files; ++fi)
            {
                tr_variant* const b = tr_variantListChild(l, fi);
                auto time_checked = time_t{};

                if (tr_variantIsInt(b))
                {
                    auto t = int64_t{};
                    tr_variantGetInt(b, &t);
                    time_checked = time_t(t);
                }
                else if (tr_variantIsList(b))
                {
                    auto offset = int64_t{};
                    tr_variantGetInt(tr_variantListChild(b, 0), &offset);

                    time_checked = tr_time();
                    auto const [begin, end] = tor->piecesInFile(fi);
                    for (size_t i = 0, n = end - begin; i < n; ++i)
                    {
                        int64_t piece_time = 0;
                        tr_variantGetInt(tr_variantListChild(b, i + 1), &piece_time);
                        time_checked = std::min(time_checked, time_t(piece_time));
                    }
                }

                mtimes.push_back(time_checked);
            }
        }

        if (std::size(mtimes) != n_files)
        {
            tr_logAddTorErr(tor, "got %zu mtimes; expected %zu", std::size(mtimes), size_t(n_files));
            // if resizing grows the vector, we'll get 0 mtimes for the
            // new items which is exactly what we want since the pieces
            // in an unknown state should be treated as untested
            mtimes.resize(n_files);
        }

        tor->initCheckedPieces(checked, std::data(mtimes));

        /// COMPLETION

        auto blocks = tr_bitfield{ tor->blockCount() };
        char const* err = nullptr;
        if (tr_variant const* const b = tr_variantDictFind(prog, TR_KEY_blocks); b != nullptr)
        {
            uint8_t const* buf = nullptr;
            auto buflen = size_t{};

            if (!tr_variantGetRaw(b, &buf, &buflen))
            {
                err = "Invalid value for \"blocks\"";
            }
            else
            {
                rawToBitfield(blocks, buf, buflen);
            }
        }
        else if (auto sv = std::string_view{}; tr_variantDictFindStrView(prog, TR_KEY_have, &sv))
        {
            if (sv == "all"sv)
            {
                blocks.setHasAll();
            }
            else
            {
                err = "Invalid value for HAVE";
            }
        }
        else if (tr_variantDictFindRaw(prog, TR_KEY_bitfield, &raw, &rawlen))
        {
            blocks.setRaw(raw, rawlen);
        }
        else
        {
            err = "Couldn't find 'pieces' or 'have' or 'bitfield'";
        }

        if (err != nullptr)
        {
            tr_logAddTorDbg(tor, "Torrent needs to be verified - %s", err);
        }
        else
        {
            tor->setBlocks(blocks);
        }

        return tr_resume::Progress;
    }

    return tr_resume::fields_t{};
}

/***
****
***/

static auto loadFromFile(tr_torrent* tor, tr_resume::fields_t fieldsToLoad, bool* did_migrate_filename)
{
    auto fields_loaded = tr_resume::fields_t{};

    TR_ASSERT(tr_isTorrent(tor));
    auto const wasDirty = tor->isDirty;

    auto const migrated = tr_torrent_metainfo::migrateFile(
        tor->session->resume_dir,
        tor->name(),
        tor->infoHashString(),
        ".resume"sv);
    if (did_migrate_filename != nullptr)
    {
        *did_migrate_filename = migrated;
    }

    auto const filename = tor->resumeFile();
    auto buf = std::vector<char>{};
    tr_error* error = nullptr;
    auto top = tr_variant{};
    if (!tr_loadFile(buf, filename, &error) ||
        !tr_variantFromBuf(
            &top,
            TR_VARIANT_PARSE_BENC | TR_VARIANT_PARSE_INPLACE,
            { std::data(buf), std::size(buf) },
            nullptr,
            &error))
    {
        tr_logAddTorDbg(tor, "Couldn't read \"%s\": %s", filename.c_str(), error->message);
        tr_error_clear(&error);
        return fields_loaded;
    }

    tr_logAddTorDbg(tor, "Read resume file \"%s\"", filename.c_str());

    auto boolVal = false;
    auto i = int64_t{};
    auto sv = std::string_view{};

    if ((fieldsToLoad & tr_resume::Corrupt) != 0 && tr_variantDictFindInt(&top, TR_KEY_corrupt, &i))
    {
        tor->corruptPrev = i;
        fields_loaded |= tr_resume::Corrupt;
    }

    if ((fieldsToLoad & (tr_resume::Progress | tr_resume::DownloadDir)) != 0 &&
        tr_variantDictFindStrView(&top, TR_KEY_destination, &sv) && !std::empty(sv))
    {
        bool const is_current_dir = tor->current_dir == tor->download_dir;
        tor->download_dir = sv;
        if (is_current_dir)
        {
            tor->current_dir = sv;
        }

        fields_loaded |= tr_resume::DownloadDir;
    }

    if ((fieldsToLoad & (tr_resume::Progress | tr_resume::IncompleteDir)) != 0 &&
        tr_variantDictFindStrView(&top, TR_KEY_incomplete_dir, &sv) && !std::empty(sv))
    {
        bool const is_current_dir = tor->current_dir == tor->incomplete_dir;
        tor->incomplete_dir = sv;
        if (is_current_dir)
        {
            tor->current_dir = sv;
        }

        fields_loaded |= tr_resume::IncompleteDir;
    }

    if ((fieldsToLoad & tr_resume::Downloaded) != 0 && tr_variantDictFindInt(&top, TR_KEY_downloaded, &i))
    {
        tor->downloadedPrev = i;
        fields_loaded |= tr_resume::Downloaded;
    }

    if ((fieldsToLoad & tr_resume::Uploaded) != 0 && tr_variantDictFindInt(&top, TR_KEY_uploaded, &i))
    {
        tor->uploadedPrev = i;
        fields_loaded |= tr_resume::Uploaded;
    }

    if ((fieldsToLoad & tr_resume::MaxPeers) != 0 && tr_variantDictFindInt(&top, TR_KEY_max_peers, &i))
    {
        tor->maxConnectedPeers = i;
        fields_loaded |= tr_resume::MaxPeers;
    }

    if ((fieldsToLoad & tr_resume::Run) != 0 && tr_variantDictFindBool(&top, TR_KEY_paused, &boolVal))
    {
        tor->isRunning = !boolVal;
        fields_loaded |= tr_resume::Run;
    }

    if ((fieldsToLoad & tr_resume::AddedDate) != 0 && tr_variantDictFindInt(&top, TR_KEY_added_date, &i))
    {
        tor->addedDate = i;
        fields_loaded |= tr_resume::AddedDate;
    }

    if ((fieldsToLoad & tr_resume::DoneDate) != 0 && tr_variantDictFindInt(&top, TR_KEY_done_date, &i))
    {
        tor->doneDate = i;
        fields_loaded |= tr_resume::DoneDate;
    }

    if ((fieldsToLoad & tr_resume::ActivityDate) != 0 && tr_variantDictFindInt(&top, TR_KEY_activity_date, &i))
    {
        tor->setDateActive(i);
        fields_loaded |= tr_resume::ActivityDate;
    }

    if ((fieldsToLoad & tr_resume::TimeSeeding) != 0 && tr_variantDictFindInt(&top, TR_KEY_seeding_time_seconds, &i))
    {
        tor->secondsSeeding = i;
        fields_loaded |= tr_resume::TimeSeeding;
    }

    if ((fieldsToLoad & tr_resume::TimeDownloading) != 0 && tr_variantDictFindInt(&top, TR_KEY_downloading_time_seconds, &i))
    {
        tor->secondsDownloading = i;
        fields_loaded |= tr_resume::TimeDownloading;
    }

    if ((fieldsToLoad & tr_resume::BandwidthPriority) != 0 && tr_variantDictFindInt(&top, TR_KEY_bandwidth_priority, &i) &&
        tr_isPriority(i))
    {
        tr_torrentSetPriority(tor, i);
        fields_loaded |= tr_resume::BandwidthPriority;
    }

    if ((fieldsToLoad & tr_resume::Peers) != 0)
    {
        fields_loaded |= loadPeers(&top, tor);
    }

    if ((fieldsToLoad & tr_resume::Progress) != 0)
    {
        fields_loaded |= loadProgress(&top, tor);
    }

    // Only load file priorities if we are actually downloading.
    // If we're a seed or partial seed, loading it is a waste of time.
    // NB: this is why loadProgress() comes before loadFilePriorities()
    if (!tor->isDone() && (fieldsToLoad & tr_resume::FilePriorities) != 0)
    {
        fields_loaded |= loadFilePriorities(&top, tor);
    }

    if ((fieldsToLoad & tr_resume::Dnd) != 0)
    {
        fields_loaded |= loadDND(&top, tor);
    }

    if ((fieldsToLoad & tr_resume::Speedlimit) != 0)
    {
        fields_loaded |= loadSpeedLimits(&top, tor);
    }

    if ((fieldsToLoad & tr_resume::Ratiolimit) != 0)
    {
        fields_loaded |= loadRatioLimits(&top, tor);
    }

    if ((fieldsToLoad & tr_resume::Idlelimit) != 0)
    {
        fields_loaded |= loadIdleLimits(&top, tor);
    }

    if ((fieldsToLoad & tr_resume::Filenames) != 0)
    {
        fields_loaded |= loadFilenames(&top, tor);
    }

    if ((fieldsToLoad & tr_resume::Name) != 0)
    {
        fields_loaded |= loadName(&top, tor);
    }

    if ((fieldsToLoad & tr_resume::Labels) != 0)
    {
        fields_loaded |= loadLabels(&top, tor);
    }

    /* loading the resume file triggers of a lot of changes,
     * but none of them needs to trigger a re-saving of the
     * same resume information... */
    tor->isDirty = wasDirty;

    tr_variantFree(&top);
    return fields_loaded;
}

static auto setFromCtor(tr_torrent* tor, tr_resume::fields_t fields, tr_ctor const* ctor, tr_ctorMode mode)
{
    auto ret = tr_resume::fields_t{};

    if ((fields & tr_resume::Run) != 0)
    {
        auto isPaused = bool{};
        if (tr_ctorGetPaused(ctor, mode, &isPaused))
        {
            tor->isRunning = !isPaused;
            ret |= tr_resume::Run;
        }
    }

    if (((fields & tr_resume::MaxPeers) != 0) && tr_ctorGetPeerLimit(ctor, mode, &tor->maxConnectedPeers))
    {
        ret |= tr_resume::MaxPeers;
    }

    if ((fields & tr_resume::DownloadDir) != 0)
    {
        char const* path = nullptr;
        if (tr_ctorGetDownloadDir(ctor, mode, &path) && !tr_str_is_empty(path))
        {
            ret |= tr_resume::DownloadDir;
            tor->download_dir = path;
        }
    }

    return ret;
}

static auto useManditoryFields(tr_torrent* tor, tr_resume::fields_t fields, tr_ctor const* ctor)
{
    return setFromCtor(tor, fields, ctor, TR_FORCE);
}

static auto useFallbackFields(tr_torrent* tor, tr_resume::fields_t fields, tr_ctor const* ctor)
{
    return setFromCtor(tor, fields, ctor, TR_FALLBACK);
}

namespace tr_resume
{

fields_t load(tr_torrent* tor, fields_t fields_to_load, tr_ctor const* ctor, bool* did_rename_to_hash_only_name)
{
    TR_ASSERT(tr_isTorrent(tor));

    auto ret = fields_t{};

    ret |= useManditoryFields(tor, fields_to_load, ctor);
    fields_to_load &= ~ret;
    ret |= loadFromFile(tor, fields_to_load, did_rename_to_hash_only_name);
    fields_to_load &= ~ret;
    ret |= useFallbackFields(tor, fields_to_load, ctor);

    return ret;
}

void save(tr_torrent* tor)
{
    if (!tr_isTorrent(tor))
    {
        return;
    }

    auto top = tr_variant{};
    tr_variantInitDict(&top, 50); /* arbitrary "big enough" number */
    tr_variantDictAddInt(&top, TR_KEY_seeding_time_seconds, tor->secondsSeeding);
    tr_variantDictAddInt(&top, TR_KEY_downloading_time_seconds, tor->secondsDownloading);
    tr_variantDictAddInt(&top, TR_KEY_activity_date, tor->activityDate);
    tr_variantDictAddInt(&top, TR_KEY_added_date, tor->addedDate);
    tr_variantDictAddInt(&top, TR_KEY_corrupt, tor->corruptPrev + tor->corruptCur);
    tr_variantDictAddInt(&top, TR_KEY_done_date, tor->doneDate);
    tr_variantDictAddQuark(&top, TR_KEY_destination, tor->downloadDir().quark());

    if (!std::empty(tor->incompleteDir()))
    {
        tr_variantDictAddQuark(&top, TR_KEY_incomplete_dir, tor->incompleteDir().quark());
    }

    tr_variantDictAddInt(&top, TR_KEY_downloaded, tor->downloadedPrev + tor->downloadedCur);
    tr_variantDictAddInt(&top, TR_KEY_uploaded, tor->uploadedPrev + tor->uploadedCur);
    tr_variantDictAddInt(&top, TR_KEY_max_peers, tor->maxConnectedPeers);
    tr_variantDictAddInt(&top, TR_KEY_bandwidth_priority, tr_torrentGetPriority(tor));
    tr_variantDictAddBool(&top, TR_KEY_paused, !tor->isRunning && !tor->isQueued());
    savePeers(&top, tor);

    if (tor->hasMetadata())
    {
        saveFilePriorities(&top, tor);
        saveDND(&top, tor);
        saveProgress(&top, tor);
    }

    saveSpeedLimits(&top, tor);
    saveRatioLimits(&top, tor);
    saveIdleLimits(&top, tor);
    saveFilenames(&top, tor);
    saveName(&top, tor);
    saveLabels(&top, tor);

    if (auto const err = tr_variantToFile(&top, TR_VARIANT_FMT_BENC, tor->resumeFile()); err != 0)
    {
        tor->setLocalError(tr_strvJoin("Unable to save resume file: ", tr_strerror(err)));
    }

    tr_variantFree(&top);
}

} // namespace tr_resume
