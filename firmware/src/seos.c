/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <plat/inc/eeData.h>
#include <plat/inc/plat.h>
#include <plat/inc/bl.h>
#include <platform.h>
#include <hostIntf.h>
#include <inttypes.h>
#include <syscall.h>
#include <sensors.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <printf.h>
#include <eventQ.h>
#include <apInt.h>
#include <timer.h>
#include <osApi.h>
#include <seos.h>
#include <heap.h>
#include <slab.h>
#include <cpu.h>
#include <crc.h>
#include <util.h>
#include <mpu.h>
#include <nanohubPacket.h>

/*
 * Since locking is difficult to do right for adding/removing listeners and such
 * since it can happen in interrupt context and not, and one such operation can
 * interrupt another, and we do have a working event queue, we enqueue all the
 * requests and then deal with them in the main code only when the event bubbles
 * up to the front of the quque. This allows us to not need locks around the
 * data structures.
 */

struct Task {
    /* pointers may become invalid. Tids do not. Zero tid -> not a valid task */
    uint32_t tid;

    uint16_t subbedEvtCount;
    uint16_t subbedEvtListSz;
    uint32_t *subbedEvents; /* NULL for invalid tasks */

    /* App entry points */
    const struct AppHdr *appHdr;

    /* per-platform app info */
    struct PlatAppInfo platInfo;

    /* for some basic number of subbed events, the array is stored directly here. after that, a heap chunk is used */
    uint32_t subbedEventsInt[MAX_EMBEDDED_EVT_SUBS];
};

union InternalThing {
    struct {
        uint32_t tid;
        uint32_t evt;
    } evtSub;
    struct {
        OsDeferCbkF callback;
        void *cookie;
    } deferred;
    struct {
        uint32_t evtType;
        void *evtData;
        TaggedPtr evtFreeInfo;
        uint32_t toTid;
    } privateEvt;
    union OsApiSlabItem osApiItem;
};

#define EVT_SUBSCRIBE_TO_EVT         0x00000000
#define EVT_UNSUBSCRIBE_TO_EVT       0x00000001
#define EVT_DEFERRED_CALLBACK        0x00000002
#define EVT_PRIVATE_EVT              0x00000003


static struct EvtQueue *mEvtsInternal;
static struct SlabAllocator* mMiscInternalThingsSlab;
static struct Task mTasks[MAX_TASKS];
static uint32_t mTaskCnt;
static uint32_t mNextTidInfo = FIRST_VALID_TID;
static TaggedPtr *mCurEvtEventFreeingInfo = NULL; //used as flag for retaining. NULL when none or already retained

static struct Task* osTaskFindByTid(uint32_t tid)
{
    uint32_t i;

    for(i = 0; i < mTaskCnt; i++)
        if (mTasks[i].tid && mTasks[i].tid == tid)
            return mTasks + i;

    return NULL;
}

static void handleEventFreeing(uint32_t evtType, void *evtData, uintptr_t evtFreeData) // watch out, this is synchronous
{
    if ((taggedPtrIsPtr(evtFreeData) && !taggedPtrToPtr(evtFreeData)) ||
        (taggedPtrIsUint(evtFreeData) && !taggedPtrToUint(evtFreeData)))
        return;

    if (taggedPtrIsPtr(evtFreeData))
        ((EventFreeF)taggedPtrToPtr(evtFreeData))(evtData);
    else {
        struct AppEventFreeData fd = {.evtType = evtType, .evtData = evtData};
        struct Task* task = osTaskFindByTid(taggedPtrToUint(evtFreeData));

        if (!task)
            osLog(LOG_ERROR, "EINCEPTION: Failed to find app to call app to free event sent to app(s).\n");
        else
            cpuAppHandle(task->appHdr, &task->platInfo, EVT_APP_FREE_EVT_DATA, &fd);
    }
}

static void osInit(void)
{
    heapInit();
    platInitialize();

    osLog(LOG_INFO, "SEOS Initializing\n");
    cpuInitLate();

    /* create the queues */
    if (!(mEvtsInternal = evtQueueAlloc(512, handleEventFreeing))) {
        osLog(LOG_INFO, "events failed to init\n");
        return;
    }

    mMiscInternalThingsSlab = slabAllocatorNew(sizeof(union InternalThing), alignof(union InternalThing), 64 /* for now? */);
    if (!mMiscInternalThingsSlab) {
        osLog(LOG_INFO, "deferred actions list failed to init\n");
        return;
    }
}

static struct Task* osTaskFindByAppID(uint64_t appID)
{
    uint32_t i;

    for (i = 0; i < mTaskCnt; i++)
        if (mTasks[i].appHdr && mTasks[i].appHdr->appId == appID)
            return mTasks + i;

    return NULL;
}

static uint32_t osGetFreeTid(void)
{
    do {
        if (mNextTidInfo == LAST_VALID_TID)
            mNextTidInfo = FIRST_VALID_TID;
        else
            mNextTidInfo++;
    } while (osTaskFindByTid(mNextTidInfo));

    return mNextTidInfo;
}

struct ExtAppIterator {
    const uint8_t *shared;
    const uint8_t *sharedEnd;
    const struct AppHdr *app;
    uint32_t appLen;
};

static void osExtAppIteratorInit(struct ExtAppIterator *it)
{
    uint32_t sz;

    it->shared = platGetSharedAreaInfo(&sz);
    it->sharedEnd = it->shared + sz;
    it->app = NULL;
    it->appLen = 0;
}

static bool osExtAppIteratorNext(struct ExtAppIterator *it)
{
    struct AppHdr *app;
    uint32_t len;
    uint32_t total_len;
    const uint8_t *p = it->shared;
    uint8_t id1, id2;

    // 32-bit header: 1 byte MARK, 3-byte len (BE, in bytes), data is 32-bit aligned; 32-bit footer: CRC-32, including header
    // both will soon be obsoleted; app header has enough data to find next app (app->rel_end)
    do {
        id1 = p[0] & 0x0F;
        id2 = (p[0] >> 4) & 0x0F;
        len = (p[1] << 16) | (p[2] << 8) | p[3];
        total_len = sizeof(uint32_t) + ((len + 3) & ~3) + sizeof(uint32_t);
        if ((p + total_len) > it->sharedEnd)
            return false;
        app = (struct AppHdr *)(&p[4]);
        p += total_len;
    } while (id1 != id2 && id1 != BL_FLASH_APP_ID);

    it->shared = p;
    it->appLen = len;
    it->app = app;

    return true;
}

static bool osExtAppIsValid(const struct AppHdr *app, uint32_t len)
{
    static const char magic[] = APP_HDR_MAGIC;

    return len >= sizeof(struct AppHdr) &&
           memcmp(magic, app->magic, sizeof(magic) - 1) == 0 &&
           app->fmtVer == APP_HDR_VER_CUR &&
           app->marker == APP_HDR_MARKER_VALID;
}

static bool osExtAppErase(const struct AppHdr *app)
{
    bool done;
    uint16_t marker = APP_HDR_MARKER_DELETED;

    mpuAllowRamExecution(true);
    mpuAllowRomWrite(true);
    done = BL.blProgramShared((uint8_t *)&app->marker, (uint8_t *)&marker, sizeof(marker), BL_FLASH_KEY1, BL_FLASH_KEY2);
    mpuAllowRomWrite(false);
    mpuAllowRamExecution(false);

    return done;
}

static struct Task *osLoadApp(const struct AppHdr *app) {
    struct Task *task;

    if (mTaskCnt == MAX_TASKS) {
        osLog(LOG_WARN, "External app id %016" PRIX64 " @ %p cannot be used as too many apps already exist.\n", app->appId, app);
        return NULL;
    }
    task = &mTasks[mTaskCnt];
    task->appHdr = app;
    bool done = app->marker == APP_HDR_MARKER_INTERNAL ?
                cpuInternalAppLoad(task->appHdr, &task->platInfo) :
                cpuAppLoad(task->appHdr, &task->platInfo);

    if (!done) {
        osLog(LOG_WARN, "App @ %p ID %016" PRIX64 " failed to load\n", app, app->appId);
        task = NULL;
    } else {
        mTaskCnt++;
    }

    return task;
}

static void osUnloadApp(struct Task *task)
{
    // this is called on task that has stopped running, or had never run
    cpuAppUnload(task->appHdr, &task->platInfo);
    struct Task *last = &mTasks[mTaskCnt-1];
    if (last > task)
        memcpy(task, last, sizeof(struct Task));
    mTaskCnt--;
}

static bool osStartApp(const struct AppHdr *app)
{
    bool done = false;
    struct Task *task;

    if ((task = osLoadApp(app)) != NULL) {
        task->subbedEvtListSz = MAX_EMBEDDED_EVT_SUBS;
        task->subbedEvents = task->subbedEventsInt;
        task->tid = osGetFreeTid();

        done = cpuAppInit(task->appHdr, &task->platInfo, task->tid);

        if (!done) {
            osLog(LOG_WARN, "App @ %p ID %016" PRIX64 "failed to init\n", task->appHdr, task->appHdr->appId);
            osUnloadApp(task);
        }
    }

    return done;
}

static bool osStopTask(struct Task *task)
{
    if (!task)
        return false;

    cpuAppEnd(task->appHdr, &task->platInfo);
    osUnloadApp(task);

    return true;
}

static bool osExtAppFind(struct ExtAppIterator *it, uint64_t appId)
{
    uint64_t vendor = APP_ID_GET_VENDOR(appId);
    uint64_t seqId = APP_ID_GET_SEQ_ID(appId);
    uint64_t curAppId;
    const struct AppHdr *app;

    while (osExtAppIteratorNext(it)) {
        app = it->app;
        curAppId = app->appId;

        if ((vendor == APP_VENDOR_ANY || vendor == APP_ID_GET_VENDOR(curAppId)) &&
            (seqId == APP_SEQ_ID_ANY || seqId == APP_ID_GET_SEQ_ID(curAppId)))
            return true;
    }

    return false;
}

static uint32_t osExtAppStopEraseApps(uint64_t appId, bool doErase)
{
    const struct AppHdr *app;
    uint32_t len;
    struct Task *task;
    struct ExtAppIterator it;
    uint32_t stopCount = 0;
    uint32_t eraseCount = 0;
    uint32_t appCount = 0;
    uint32_t taskCount = 0;
    struct MgmtStatus stat = { .value = 0 };

    osExtAppIteratorInit(&it);
    while (osExtAppFind(&it, appId)) {
        app = it.app;
        len = it.appLen;
        if (!osExtAppIsValid(app, len))
            continue;
        appCount++;
        task = osTaskFindByAppID(app->appId);
        if (task)
            taskCount++;
        if (task && task->appHdr == app && app->marker == APP_HDR_MARKER_VALID) {
            if (osStopTask(task))
                stopCount++;
            else
                continue;
            if (doErase && osExtAppErase(app))
                eraseCount++;
        }
    }
    SET_COUNTER(stat.app,   appCount);
    SET_COUNTER(stat.task,  taskCount);
    SET_COUNTER(stat.op,    stopCount);
    SET_COUNTER(stat.erase, eraseCount);

    return stat.value;
}

uint32_t osExtAppStopApps(uint64_t appId)
{
    return osExtAppStopEraseApps(appId, false);
}

uint32_t osExtAppEraseApps(uint64_t appId)
{
    return osExtAppStopEraseApps(appId, true);
}

uint32_t osExtAppStartApps(uint64_t appId)
{
    const struct AppHdr *app;
    size_t len;
    struct ExtAppIterator it;
    struct ExtAppIterator checkIt;
    uint32_t startCount = 0;
    uint32_t eraseCount = 0;
    uint32_t appCount = 0;
    uint32_t taskCount = 0;
    struct MgmtStatus stat = { .value = 0 };

    osExtAppIteratorInit(&it);
    while (osExtAppFind(&it, appId)) {
        app = it.app;
        len = it.appLen;

        // skip erased or malformed apps
        if (!osExtAppIsValid(app, len))
            continue;

        appCount++;
        checkIt = it;
        // find the most recent copy
        while (osExtAppFind(&checkIt, app->appId)) {
            if (osExtAppErase(app)) // erase the old one, so we skip it next time
                eraseCount++;
            app = checkIt.app;
        }

        if (osTaskFindByAppID(app->appId)) {
            // this either the most recent external app with the same ID,
            // or internal app with the same id; in both cases we do nothing
            taskCount++;
            continue;
        }

        if (osStartApp(app))
            startCount++;
    }
    SET_COUNTER(stat.app,   appCount);
    SET_COUNTER(stat.task,  taskCount);
    SET_COUNTER(stat.op,    startCount);
    SET_COUNTER(stat.erase, eraseCount);

    return stat.value;
}

static void osStartTasks(void)
{
    const struct AppHdr *app;
    uint32_t i, nApps;
    struct Task* task;
    uint32_t status = 0;

    mTaskCnt = 0;
    /* first enum all internal apps, making sure to check for dupes */
    osLog(LOG_DEBUG, "Starting internal apps...\n");
    for (i = 0, app = platGetInternalAppList(&nApps); i < nApps; i++, app++) {
        if (app->fmtVer != APP_HDR_VER_CUR) {
            osLog(LOG_WARN, "Unexpected app @ %p ID %016" PRIX64 "header version: %d\n",
                  app, app->appId, app->fmtVer);
            continue;
        }

        if (app->marker != APP_HDR_MARKER_INTERNAL) {
            osLog(LOG_WARN, "Invalid marker on internal app: [%p]=0x%04X ID %016" PRIX64 "; ignored\n",
                  app, app->marker, app->appId);
            continue;
        }
        if ((task = osTaskFindByAppID(app->appId))) {
            osLog(LOG_WARN, "Internal app ID %016" PRIX64
                            "@ %p attempting to update internal app @ %p; app @%p ignored.\n",
                            app->appId, app, task->appHdr, app);
            continue;
        }
        osStartApp(app);
    }
    nApps = mTaskCnt;

    osLog(LOG_DEBUG, "Starting external apps...\n");
    status = osExtAppStartApps(APP_ID_ANY);

    osLog(LOG_DEBUG, "Started %" PRIu32 " internal apps; total %" PRIu32
                     " apps; EXT status: %08" PRIX32 "\n", nApps, mTaskCnt, status);
}

static void osInternalEvtHandle(uint32_t evtType, void *evtData)
{
    union InternalThing *da = (union InternalThing*)evtData;
    struct Task *task;
    uint32_t i;

    switch (evtType) {
    case EVT_SUBSCRIBE_TO_EVT:
    case EVT_UNSUBSCRIBE_TO_EVT:
        /* get task */
        task = osTaskFindByTid(da->evtSub.tid);
        if (!task)
            break;

        /* find if subscribed to this evt */
        for (i = 0; i < task->subbedEvtCount && task->subbedEvents[i] != da->evtSub.evt; i++);

        /* if unsub & found -> unsub */
        if (evtType == EVT_UNSUBSCRIBE_TO_EVT && i != task->subbedEvtCount)
            task->subbedEvents[i] = task->subbedEvents[--task->subbedEvtCount];
        /* if sub & not found -> sub */
        else if (evtType == EVT_SUBSCRIBE_TO_EVT && i == task->subbedEvtCount) {
            if (task->subbedEvtListSz == task->subbedEvtCount) { /* enlarge the list */
                uint32_t newSz = (task->subbedEvtListSz * 3 + 1) / 2;
                uint32_t *newList = heapAlloc(sizeof(uint32_t[newSz])); /* grow by 50% */
                if (newList) {
                    memcpy(newList, task->subbedEvents, sizeof(uint32_t[task->subbedEvtListSz]));
                    if (task->subbedEvents != task->subbedEventsInt)
                        heapFree(task->subbedEvents);
                    task->subbedEvents = newList;
                    task->subbedEvtListSz = newSz;
                }
            }
            if (task->subbedEvtListSz > task->subbedEvtCount) { /* have space ? */
                task->subbedEvents[task->subbedEvtCount++] = da->evtSub.evt;
            }
        }
        break;

    case EVT_DEFERRED_CALLBACK:
        da->deferred.callback(da->deferred.cookie);
        break;

    case EVT_PRIVATE_EVT:
        task = osTaskFindByTid(da->privateEvt.toTid);
        if (task) {
            //private events cannot be retained
            TaggedPtr *tmp = mCurEvtEventFreeingInfo;
            mCurEvtEventFreeingInfo = NULL;

            cpuAppHandle(task->appHdr, &task->platInfo, da->privateEvt.evtType, da->privateEvt.evtData);

            mCurEvtEventFreeingInfo = tmp;
        }

        handleEventFreeing(da->privateEvt.evtType, da->privateEvt.evtData, da->privateEvt.evtFreeInfo);
        break;
    }
}

void abort(void)
{
    /* this is necessary for va_* funcs... */
    osLog(LOG_ERROR, "Abort called");
    while(1);
}

bool osRetainCurrentEvent(TaggedPtr *evtFreeingInfoP)
{
    if (!mCurEvtEventFreeingInfo)
        return false;

    *evtFreeingInfoP = *mCurEvtEventFreeingInfo;
    mCurEvtEventFreeingInfo = NULL;
    return true;
}

void osFreeRetainedEvent(uint32_t evtType, void *evtData, TaggedPtr *evtFreeingInfoP)
{
    handleEventFreeing(evtType, evtData, *evtFreeingInfoP);
}

void osMainInit(void)
{
    cpuInit();
    cpuIntsOff();
    osInit();
    timInit();
    sensorsInit();
    syscallInit();
    osApiExport(mMiscInternalThingsSlab);
    apIntInit();
    cpuIntsOn();
    osStartTasks();

    //broadcast app start to all already-loaded apps
    (void)osEnqueueEvt(EVT_APP_START, NULL, NULL);
}

void osMainDequeueLoop(void)
{
    TaggedPtr evtFreeingInfo;
    uint32_t evtType, i, j;
    void *evtData;

    /* get an event */
    if (!evtQueueDequeue(mEvtsInternal, &evtType, &evtData, &evtFreeingInfo, true))
        return;

    /* by default we free them when we're done with them */
    mCurEvtEventFreeingInfo = &evtFreeingInfo;

    if (evtType < EVT_NO_FIRST_USER_EVENT) { /* no need for discardable check. all internal events arent discardable */
        /* handle deferred actions and other reserved events here */
        osInternalEvtHandle(evtType, evtData);
    }
    else {
        /* send this event to all tasks who want it */
        for (i = 0; i < mTaskCnt; i++) {
            if (!mTasks[i].subbedEvents) /* only check real tasks */
                continue;
            for (j = 0; j < mTasks[i].subbedEvtCount; j++) {
                if (mTasks[i].subbedEvents[j] == (evtType & ~EVENT_TYPE_BIT_DISCARDABLE)) {
                    cpuAppHandle(mTasks[i].appHdr, &mTasks[i].platInfo, evtType & ~EVENT_TYPE_BIT_DISCARDABLE, evtData);
                    break;
                }
            }
        }
    }

    /* free it */
    if (mCurEvtEventFreeingInfo)
        handleEventFreeing(evtType, evtData, evtFreeingInfo);

    /* avoid some possible errors */
    mCurEvtEventFreeingInfo = NULL;
}

void __attribute__((noreturn)) osMain(void)
{
    osMainInit();

    while (true)
    {
        osMainDequeueLoop();
    }
}

static void osDeferredActionFreeF(void* event)
{
    slabAllocatorFree(mMiscInternalThingsSlab, event);
}

static bool osEventSubscribeUnsubscribe(uint32_t tid, uint32_t evtType, bool sub)
{
    union InternalThing *act = slabAllocatorAlloc(mMiscInternalThingsSlab);

    if (!act)
        return false;
    act->evtSub.evt = evtType;
    act->evtSub.tid = tid;

    return osEnqueueEvtOrFree(sub ? EVT_SUBSCRIBE_TO_EVT : EVT_UNSUBSCRIBE_TO_EVT, act, osDeferredActionFreeF);
}

bool osEventSubscribe(uint32_t tid, uint32_t evtType)
{
    return osEventSubscribeUnsubscribe(tid, evtType, true);
}

bool osEventUnsubscribe(uint32_t tid, uint32_t evtType)
{
    return osEventSubscribeUnsubscribe(tid, evtType, false);
}

bool osEnqueueEvt(uint32_t evtType, void *evtData, EventFreeF evtFreeF)
{
    return evtQueueEnqueue(mEvtsInternal, evtType, evtData, taggedPtrMakeFromPtr(evtFreeF), false);
}

bool osEnqueueEvtOrFree(uint32_t evtType, void *evtData, EventFreeF evtFreeF)
{
    bool success = osEnqueueEvt(evtType, evtData, evtFreeF);

    if (!success && evtFreeF)
        evtFreeF(evtData);

    return success;
}

bool osEnqueueEvtAsApp(uint32_t evtType, void *evtData, uint32_t fromAppTid)
{
    return evtQueueEnqueue(mEvtsInternal, evtType, evtData, taggedPtrMakeFromUint(fromAppTid), false);
}

bool osDefer(OsDeferCbkF callback, void *cookie, bool urgent)
{
    union InternalThing *act = slabAllocatorAlloc(mMiscInternalThingsSlab);
    if (!act)
            return false;

    act->deferred.callback = callback;
    act->deferred.cookie = cookie;

    if (evtQueueEnqueue(mEvtsInternal, EVT_DEFERRED_CALLBACK, act, taggedPtrMakeFromPtr(osDeferredActionFreeF), urgent))
        return true;

    slabAllocatorFree(mMiscInternalThingsSlab, act);
    return false;
}

static bool osEnqueuePrivateEvtEx(uint32_t evtType, void *evtData, TaggedPtr evtFreeInfo, uint32_t toTid)
{
    union InternalThing *act = slabAllocatorAlloc(mMiscInternalThingsSlab);
    if (!act)
            return false;

    act->privateEvt.evtType = evtType;
    act->privateEvt.evtData = evtData;
    act->privateEvt.evtFreeInfo = evtFreeInfo;
    act->privateEvt.toTid = toTid;

    return osEnqueueEvtOrFree(EVT_PRIVATE_EVT, act, osDeferredActionFreeF);
}

bool osEnqueuePrivateEvt(uint32_t evtType, void *evtData, EventFreeF evtFreeF, uint32_t toTid)
{
    return osEnqueuePrivateEvtEx(evtType, evtData, taggedPtrMakeFromPtr(evtFreeF), toTid);
}

bool osEnqueuePrivateEvtAsApp(uint32_t evtType, void *evtData, uint32_t fromAppTid, uint32_t toTid)
{
    return osEnqueuePrivateEvtEx(evtType, evtData, taggedPtrMakeFromUint(fromAppTid), toTid);
}

bool osTidById(uint64_t appId, uint32_t *tid)
{
    uint32_t i;

    for (i = 0; i < mTaskCnt; i++) {
        if (mTasks[i].appHdr && mTasks[i].appHdr->appId == appId) {
            *tid = mTasks[i].tid;
            return true;
        }
    }

    return false;
}

bool osAppInfoById(uint64_t appId, uint32_t *appIdx, uint32_t *appVer, uint32_t *appSize)
{
    uint32_t i;

    for (i = 0; i < mTaskCnt; i++) {
        if (mTasks[i].appHdr && mTasks[i].appHdr->appId == appId) {
            *appIdx = i;
            *appVer = mTasks[i].appHdr->appVer;
            *appSize = mTasks[i].appHdr->rel_end;
            return true;
        }
    }

    return false;
}

bool osAppInfoByIndex(uint32_t appIdx, uint64_t *appId, uint32_t *appVer, uint32_t *appSize)
{
    if (appIdx < mTaskCnt && mTasks[appIdx].appHdr) {
        *appId = mTasks[appIdx].appHdr->appId;
        *appVer = mTasks[appIdx].appHdr->appVer;
        *appSize = mTasks[appIdx].appHdr->rel_end;
        return true;
    }

    return false;
}

void osLogv(enum LogLevel level, const char *str, va_list vl)
{
    void *userData = platLogAllocUserData();

    platLogPutcharF(userData, level);
    cvprintf(platLogPutcharF, userData, str, vl);

    platLogFlush(userData);
}

void osLog(enum LogLevel level, const char *str, ...)
{
    va_list vl;

    va_start(vl, str);
    osLogv(level, str, vl);
    va_end(vl);
}




//Google's public key for Google's apps' signing
const uint8_t __attribute__ ((section (".pubkeys"))) _RSA_KEY_GOOGLE[] = {
    0xd9, 0xcd, 0x83, 0xae, 0xb5, 0x9e, 0xe4, 0x63, 0xf1, 0x4c, 0x26, 0x6a, 0x1c, 0xeb, 0x4c, 0x12,
    0x5b, 0xa6, 0x71, 0x7f, 0xa2, 0x4e, 0x7b, 0xa2, 0xee, 0x02, 0x86, 0xfc, 0x0d, 0x31, 0x26, 0x74,
    0x1e, 0x9c, 0x41, 0x43, 0xba, 0x16, 0xe9, 0x23, 0x4d, 0xfc, 0xc4, 0xca, 0xcc, 0xd5, 0x27, 0x2f,
    0x16, 0x4c, 0xe2, 0x85, 0x39, 0xb3, 0x0b, 0xcb, 0x73, 0xb6, 0x56, 0xc2, 0x98, 0x83, 0xf6, 0xfa,
    0x7a, 0x6e, 0xa0, 0x9a, 0xcc, 0x83, 0x97, 0x9d, 0xde, 0x89, 0xb2, 0xa3, 0x05, 0x46, 0x0c, 0x12,
    0xae, 0x01, 0xf8, 0x0c, 0xf5, 0x39, 0x32, 0xe5, 0x94, 0xb9, 0xa0, 0x8f, 0x19, 0xe4, 0x39, 0x54,
    0xad, 0xdb, 0x81, 0x60, 0x74, 0x63, 0xd5, 0x80, 0x3b, 0xd2, 0x88, 0xf4, 0xcb, 0x6b, 0x47, 0x28,
    0x80, 0xb0, 0xd1, 0x89, 0x6d, 0xd9, 0x62, 0x88, 0x81, 0xd6, 0xc0, 0x13, 0x88, 0x91, 0xfb, 0x7d,
    0xa3, 0x7f, 0xa5, 0x40, 0x12, 0xfb, 0x77, 0x77, 0x4c, 0x98, 0xe4, 0xd3, 0x62, 0x39, 0xcc, 0x63,
    0x34, 0x76, 0xb9, 0x12, 0x67, 0xfe, 0x83, 0x23, 0x5d, 0x40, 0x6b, 0x77, 0x93, 0xd6, 0xc0, 0x86,
    0x6c, 0x03, 0x14, 0xdf, 0x78, 0x2d, 0xe0, 0x9b, 0x5e, 0x05, 0xf0, 0x93, 0xbd, 0x03, 0x1d, 0x17,
    0x56, 0x88, 0x58, 0x25, 0xa6, 0xae, 0x63, 0xd2, 0x01, 0x43, 0xbb, 0x7e, 0x7a, 0xa5, 0x62, 0xdf,
    0x8a, 0x31, 0xbd, 0x24, 0x1b, 0x1b, 0xeb, 0xfe, 0xdf, 0xd1, 0x31, 0x61, 0x4a, 0xfa, 0xdd, 0x6e,
    0x62, 0x0c, 0xa9, 0xcd, 0x08, 0x0c, 0xa1, 0x1b, 0xe7, 0xf2, 0xed, 0x36, 0x22, 0xd0, 0x5d, 0x80,
    0x78, 0xeb, 0x6f, 0x5a, 0x58, 0x18, 0xb5, 0xaf, 0x82, 0x77, 0x4c, 0x95, 0xce, 0xc6, 0x4d, 0xda,
    0xca, 0xef, 0x68, 0xa6, 0x6d, 0x71, 0x4d, 0xf1, 0x14, 0xaf, 0x68, 0x25, 0xb8, 0xf3, 0xff, 0xbe,
};


#ifdef DEBUG

//debug key whose privatekey is checked in as misc/debug.privkey
const uint8_t __attribute__ ((section (".pubkeys"))) _RSA_KEY_GOOGLE_DEBUG[] = {
    0x2d, 0xff, 0xa6, 0xb5, 0x65, 0x87, 0xbe, 0x61, 0xd1, 0xe1, 0x67, 0x10, 0xa1, 0x9b, 0xc6, 0xca,
    0xc8, 0xb1, 0xf0, 0xaa, 0x88, 0x60, 0x9f, 0xa1, 0x00, 0xa1, 0x41, 0x9a, 0xd8, 0xb4, 0xd1, 0x74,
    0x9f, 0x23, 0x28, 0x0d, 0xc2, 0xc4, 0x37, 0x15, 0xb1, 0x4a, 0x80, 0xca, 0xab, 0xb9, 0xba, 0x09,
    0x7d, 0xf8, 0x44, 0xd6, 0xa2, 0x72, 0x28, 0x12, 0x91, 0xf6, 0xa5, 0xea, 0xbd, 0xf8, 0x81, 0x6b,
    0xd2, 0x3c, 0x50, 0xa2, 0xc6, 0x19, 0x54, 0x48, 0x45, 0x8d, 0x92, 0xac, 0x01, 0xda, 0x14, 0x32,
    0xdb, 0x05, 0x82, 0x06, 0x30, 0x25, 0x09, 0x7f, 0x5a, 0xbb, 0x86, 0x64, 0x70, 0x98, 0x64, 0x1e,
    0xe6, 0xca, 0x1d, 0xc1, 0xcb, 0xb6, 0x23, 0xd2, 0x62, 0x00, 0x46, 0x97, 0xd5, 0xcc, 0xe6, 0x36,
    0x72, 0xec, 0x2e, 0x43, 0x1f, 0x0a, 0xaf, 0xf2, 0x51, 0xe1, 0xcd, 0xd2, 0x98, 0x5d, 0x7b, 0x64,
    0xeb, 0xd1, 0x35, 0x4d, 0x59, 0x13, 0x82, 0x6c, 0xbd, 0xc4, 0xa2, 0xfc, 0xad, 0x64, 0x73, 0xe2,
    0x71, 0xb5, 0xf4, 0x45, 0x53, 0x6b, 0xc3, 0x56, 0xb9, 0x8b, 0x3d, 0xeb, 0x00, 0x48, 0x6e, 0x29,
    0xb1, 0xb4, 0x8e, 0x2e, 0x43, 0x39, 0xef, 0x45, 0xa0, 0xb8, 0x8b, 0x5f, 0x80, 0xb5, 0x0c, 0xc3,
    0x03, 0xe3, 0xda, 0x51, 0xdc, 0xec, 0x80, 0x2c, 0x0c, 0xdc, 0xe2, 0x71, 0x0a, 0x14, 0x4f, 0x2c,
    0x22, 0x2b, 0x0e, 0xd1, 0x8b, 0x8f, 0x93, 0xd2, 0xf3, 0xec, 0x3a, 0x5a, 0x1c, 0xba, 0x80, 0x54,
    0x23, 0x7f, 0xb0, 0x54, 0x8b, 0xe3, 0x98, 0x22, 0xbb, 0x4b, 0xd0, 0x29, 0x5f, 0xce, 0xf2, 0xaa,
    0x99, 0x89, 0xf2, 0xb7, 0x5d, 0x8d, 0xb2, 0x72, 0x0b, 0x52, 0x02, 0xb8, 0xa4, 0x37, 0xa0, 0x3b,
    0xfe, 0x0a, 0xbc, 0xb3, 0xb3, 0xed, 0x8f, 0x8c, 0x42, 0x59, 0xbe, 0x4e, 0x31, 0xed, 0x11, 0x9b,
};

#endif


PREPOPULATED_ENCR_KEY(google_encr_key, ENCR_KEY_GOOGLE_PREPOPULATED, 0xf1, 0x51, 0x9b, 0x2e, 0x26, 0x6c, 0xeb, 0xe7, 0xd6, 0xd6, 0x0d, 0x17, 0x11, 0x94, 0x99, 0x19, 0x1c, 0xfb, 0x71, 0x56, 0x53, 0xf7, 0xe0, 0x7d, 0x90, 0x07, 0x53, 0x68, 0x10, 0x95, 0x1b, 0x70);




