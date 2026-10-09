/***********************************************************************************************************************************
Fork Archive Routing
***********************************************************************************************************************************/
#include <build.h>

#include <ctype.h>

#include "command/archive/common.h"
#include "command/archive/fork.h"
#include "command/fork/common.h"
#include "common/debug.h"
#include "config/config.h"
#include "postgres/interface.h"
#include "storage/helper.h"

typedef struct ArchiveForkHistoryItem
{
    unsigned int timeline;
    uint64_t begin;
    uint64_t end;                                                   // Exclusive end; zero means the current timeline
    const String *reason;                                           // Reason from the PostgreSQL history file
} ArchiveForkHistoryItem;

// Parse PostgreSQL's timeline history format. Check timeline order and, when requested, switch LSN order.
static List *
archiveForkHistoryParse(const String *const history, const unsigned int timeline, const bool lsnOrder)
{
    ASSERT(history != NULL);
    ASSERT(timeline != 0);

    List *const result = lstNewP(sizeof(ArchiveForkHistoryItem));
    const StringList *const lines = strLstNewSplitZ(history, "\n");
    unsigned int priorTimeline = 0;
    uint64_t begin = 0;

    for (unsigned int idx = 0; idx < strLstSize(lines); idx++)
    {
        const char *line = strZ(strLstGet(lines, idx));

        while (isspace((unsigned char)*line))
            line++;

        if (*line == 0 || *line == '#')
            continue;

        const char *end = line;

        while (*end != 0 && !isspace((unsigned char)*end))
            end++;

        const unsigned int parent = cvtZToUInt(strZ(strNewZN(line, (size_t)(end - line))));
        line = end;

        while (isspace((unsigned char)*line))
            line++;

        end = line;

        while (*end != 0 && !isspace((unsigned char)*end))
            end++;

        const uint64_t lsn = pgLsnFromStr(strNewZN(line, (size_t)(end - line)));

        if (parent <= priorTimeline || parent >= timeline || lsn == 0 || (lsnOrder && lsn < begin))
            THROW(FormatError, "invalid fork timeline history");

        while (isspace((unsigned char)*end))
            end++;

        const ArchiveForkHistoryItem item = {.timeline = parent, .begin = begin, .end = lsn, .reason = strNewZ(end)};
        lstAdd(result, &item);
        priorTimeline = parent;
        begin = lsn;
    }

    if (timeline != 1 && lstEmpty(result))
        THROW(FormatError, "fork timeline history is empty");

    const ArchiveForkHistoryItem item = {.timeline = timeline, .begin = begin};
    lstAdd(result, &item);
    return result;
}

FN_EXTERN bool
archiveForkHistoryContains(
    const String *const history, const unsigned int timeline, const unsigned int pointTimeline, const uint64_t pointLsn)
{
    const List *const list = archiveForkHistoryParse(history, timeline, true);

    for (unsigned int idx = 0; idx < lstSize(list); idx++)
    {
        const ArchiveForkHistoryItem *const item = lstGet(list, idx);

        if (item->timeline == pointTimeline)
            return pointLsn >= item->begin && (item->end == 0 || pointLsn < item->end);
    }

    return false;
}

FN_EXTERN String *
archiveForkHistoryNormalize(const InfoFork *const info, const String *const name, const String *const history)
{
    ASSERT(info != NULL);
    ASSERT(name != NULL);
    ASSERT(history != NULL);

    const InfoForkData *const data = infoForkData(info);
    const unsigned int timeline = cvtZToUIntBase(strZ(strSubN(name, 0, 8)), 16);

    // PostgreSQL can retain later source intervals after recovery to an earlier timeline.
    // Remove them before checking WAL ownership.
    if (timeline > data->sourceTimeline)
    {
        const List *const source = archiveForkHistoryParse(data->sourceHistory, data->sourceTimeline, true);
        const List *const proposed = archiveForkHistoryParse(history, timeline, false);
        const ArchiveForkHistoryItem *const sourceLeaf = lstGet(source, lstSize(source) - 1);

        if (lstSize(proposed) > lstSize(source) &&
            ((const ArchiveForkHistoryItem *)lstGet(proposed, lstSize(source) - 1))->timeline <= data->sourceTimeline &&
            ((const ArchiveForkHistoryItem *)lstGet(proposed, lstSize(source)))->begin <= sourceLeaf->begin)
        {
            // Require the saved source timeline history. If a branch is already confirmed, require that branch next.
            for (unsigned int idx = 0; idx < lstSize(source); idx++)
            {
                const ArchiveForkHistoryItem *const sourceItem = lstGet(source, idx);
                const ArchiveForkHistoryItem *const proposedItem = lstGet(proposed, idx);

                CHECK(
                    ArchiveMismatchError,
                    sourceItem->timeline == proposedItem->timeline && sourceItem->begin == proposedItem->begin &&
                    (sourceItem->end == 0 || sourceItem->end == proposedItem->end),
                    "new timeline history does not extend the fork lineage");
            }

            const ArchiveForkHistoryItem *const own = lstGet(proposed, lstSize(source));
            CHECK(
                ArchiveMismatchError,
                data->ownTimeline == 0 || (own->timeline == data->ownTimeline && own->begin == data->branchLsn),
                "new timeline history does not extend the fork lineage");

            String *const result = strNew();
            const ArchiveForkHistoryItem *const branch = lstGet(proposed, lstSize(source) - 1);

            for (unsigned int idx = 0; idx < lstSize(source); idx++)
            {
                const ArchiveForkHistoryItem *const item = lstGet(source, idx);
                const bool last = item->end == 0 || item->end >= own->begin;
                strCatFmt(
                    result, "%u\t%s\t%s\n", item->timeline,
                    strZ(pgLsnToStr(last ? own->begin : item->end)), last ? strZ(branch->reason) : "pgBackRest fork");

                if (last)
                    break;
            }

            for (unsigned int idx = lstSize(source); idx + 1 < lstSize(proposed); idx++)
            {
                const ArchiveForkHistoryItem *const item = lstGet(proposed, idx);
                strCatFmt(result, "%u\t%s\t%s\n", item->timeline, strZ(pgLsnToStr(item->end)), strZ(item->reason));
            }

            archiveForkHistoryParse(result, timeline, true);
            return result;
        }
    }

    archiveForkHistoryParse(history, timeline, true);
    return strDup(history);
}

FN_EXTERN ArchiveFork *
archiveForkLoad(void)
{
    ArchiveFork *result = NULL;

    for (unsigned int repoIdx = 0; repoIdx < cfgOptionGroupIdxTotal(cfgOptGrpRepo); repoIdx++)
    {
        if (cfgOptionTest(cfgOptRepo) && cfgOptionUInt(cfgOptRepo) != cfgOptionGroupIdxToKey(cfgOptGrpRepo, repoIdx))
            continue;

        InfoFork *const info = forkInfoLoad(repoIdx);

        if (info == NULL)
            continue;

        if (result != NULL)
            THROW(OptionRequiredError, "option 'repo' is required when more than one repository contains a fork");

        result = memNew(sizeof(ArchiveFork));
        result->repoIdx = repoIdx;
        result->info = info;
        result->sourceRepoIdx = forkSourceRepoIdx(info);
    }

    return result;
}

FN_EXTERN bool
archiveForkWalAllowed(const InfoFork *const info, const String *const request, const bool parent)
{
    ASSERT(info != NULL);
    ASSERT(request != NULL);

    // Read history files and other archive metadata from the fork repository.
    if (!walIsSegment(request))
        return !parent;

    const InfoForkData *const data = infoForkData(info);
    const unsigned int timeline = pgTimelineFromWalSegment(strSubN(request, 0, WAL_SEGMENT_NAME_SIZE));
    const List *const history = archiveForkHistoryParse(
        data->historyTimeline == 0 ? data->sourceHistory : data->history,
        data->historyTimeline == 0 ? data->sourceTimeline : data->historyTimeline, true);
    const unsigned int segment = cvtZToUIntBase(strZ(strSubN(request, 16, 8)), 16);

    if (segment >= ((uint64_t)UINT32_MAX + 1) / data->walSegmentSize)
        return false;

    const uint64_t segmentBegin =
        (uint64_t)cvtZToUIntBase(strZ(strSubN(request, 8, 8)), 16) * ((uint64_t)UINT32_MAX + 1) +
        (uint64_t)segment * data->walSegmentSize;

    for (unsigned int idx = 0; idx < lstSize(history); idx++)
    {
        const ArchiveForkHistoryItem *const item = lstGet(history, idx);

        if (item->timeline == timeline)
        {
            // Require the successor timeline's segment name at the branch point. If that segment is unavailable, the old name
            // can make PostgreSQL replay WAL from a different branch.
            return (!parent || timeline <= data->sourceTimeline) &&
                   segmentBegin >= item->begin / data->walSegmentSize * data->walSegmentSize &&
                   (item->end == 0 || (segmentBegin < item->end && item->end - segmentBegin >= data->walSegmentSize));
        }
    }

    return false;
}

FN_EXTERN String *
archiveForkLockName(const unsigned int repoIdx)
{
    return strNewFmt(
        "%s-archive-%u.lock", strZ(cfgOptionStr(cfgOptStanza)), cfgOptionGroupIdxToKey(cfgOptGrpRepo, repoIdx));
}

FN_EXTERN void
archiveForkHistory(const ArchiveFork *const fork, const String *const name, const String *const history)
{
    ASSERT(fork != NULL);
    ASSERT(name != NULL);
    ASSERT(history != NULL);

    const InfoForkData *const data = infoForkData(fork->info);
    const unsigned int timeline = cvtZToUIntBase(strZ(strSubN(name, 0, 8)), 16);

    // Source history files and retries of older history files must not change the active timeline history.
    if (timeline <= data->sourceTimeline || timeline < data->historyTimeline)
        return;

    const String *const normalized = archiveForkHistoryNormalize(fork->info, name, history);
    const List *const proposed = archiveForkHistoryParse(normalized, timeline, true);
    const ArchiveForkHistoryItem *const leaf = lstGet(proposed, lstSize(proposed) - 1);

    if (timeline != data->historyTimeline)
    {
        const List *const accepted = archiveForkHistoryParse(
            data->ownTimeline == 0 ? data->sourceHistory : data->history,
            data->ownTimeline == 0 ? data->sourceTimeline : data->historyTimeline, true);
        unsigned int parentIdx = lstSize(accepted) - 1;

        // Select the interval containing the last byte before the switch.
        if (data->ownTimeline == 0)
        {
            parentIdx = 0;

            while (((const ArchiveForkHistoryItem *)lstGet(accepted, parentIdx))->end != 0 &&
                   ((const ArchiveForkHistoryItem *)lstGet(accepted, parentIdx))->end < leaf->begin)
            {
                parentIdx++;
            }
        }

        CHECK(
            ArchiveMismatchError, lstSize(proposed) == parentIdx + 2,
            "new timeline history does not extend the fork lineage");

        for (unsigned int idx = 0; idx <= parentIdx; idx++)
        {
            const ArchiveForkHistoryItem *const acceptedItem = lstGet(accepted, idx);
            const ArchiveForkHistoryItem *const proposedItem = lstGet(proposed, idx);

            CHECK(
                ArchiveMismatchError,
                acceptedItem->timeline == proposedItem->timeline && acceptedItem->begin == proposedItem->begin &&
                (idx == parentIdx || acceptedItem->end == proposedItem->end),
                "new timeline history does not extend the fork lineage");
        }
    }

    infoForkHistorySet(fork->info, timeline, data->ownTimeline == 0 ? leaf->begin : data->branchLsn, normalized);
    infoForkSaveFile(
        fork->info, storageRepoIdxWrite(fork->repoIdx), STRDEF(INFO_FORK_PATH_FILE), cfgCipherSpecMainIdx(fork->repoIdx));
}
