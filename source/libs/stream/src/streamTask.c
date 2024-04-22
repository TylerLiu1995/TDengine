/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "executor.h"
#include "streamInt.h"
#include "streamsm.h"
#include "tmisce.h"
#include "tstream.h"
#include "ttimer.h"
#include "wal.h"

#define CHECK_NOT_RSP_DURATION 10*1000 // 10 sec

static void streamTaskDestroyUpstreamInfo(SUpstreamInfo* pUpstreamInfo);
static void streamTaskUpdateUpstreamInfo(SStreamTask* pTask, int32_t nodeId, const SEpSet* pEpSet);
static void streamTaskUpdateDownstreamInfo(SStreamTask* pTask, int32_t nodeId, const SEpSet* pEpSet);

static int32_t addToTaskset(SArray* pArray, SStreamTask* pTask) {
  int32_t childId = taosArrayGetSize(pArray);
  pTask->info.selfChildId = childId;
  taosArrayPush(pArray, &pTask);
  return 0;
}

static int32_t doUpdateTaskEpset(SStreamTask* pTask, int32_t nodeId, SEpSet* pEpSet) {
  char buf[512] = {0};

  if (pTask->info.nodeId == nodeId) {  // execution task should be moved away
    epsetAssign(&pTask->info.epSet, pEpSet);
    epsetToStr(pEpSet, buf, tListLen(buf));
    stDebug("s-task:0x%x (vgId:%d) self node epset is updated %s", pTask->id.taskId, nodeId, buf);
  }

  // check for the dispatch info and the upstream task info
  int32_t level = pTask->info.taskLevel;
  if (level == TASK_LEVEL__SOURCE) {
    streamTaskUpdateDownstreamInfo(pTask, nodeId, pEpSet);
  } else if (level == TASK_LEVEL__AGG) {
    streamTaskUpdateUpstreamInfo(pTask, nodeId, pEpSet);
    streamTaskUpdateDownstreamInfo(pTask, nodeId, pEpSet);
  } else {  // TASK_LEVEL__SINK
    streamTaskUpdateUpstreamInfo(pTask, nodeId, pEpSet);
  }

  return 0;
}

static void freeItem(void* p) {
  SStreamContinueExecInfo* pInfo = p;
  rpcFreeCont(pInfo->msg.pCont);
}

static void freeUpstreamItem(void* p) {
  SStreamChildEpInfo** pInfo = p;
  taosMemoryFree(*pInfo);
}

static SStreamChildEpInfo* createStreamTaskEpInfo(const SStreamTask* pTask) {
  SStreamChildEpInfo* pEpInfo = taosMemoryMalloc(sizeof(SStreamChildEpInfo));
  if (pEpInfo == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return NULL;
  }

  pEpInfo->childId = pTask->info.selfChildId;
  pEpInfo->epSet = pTask->info.epSet;
  pEpInfo->nodeId = pTask->info.nodeId;
  pEpInfo->taskId = pTask->id.taskId;
  pEpInfo->stage = -1;

  return pEpInfo;
}

SStreamTask* tNewStreamTask(int64_t streamId, int8_t taskLevel, SEpSet* pEpset, bool fillHistory, int64_t triggerParam,
                            SArray* pTaskList, bool hasFillhistory, int8_t subtableWithoutMd5) {
  SStreamTask* pTask = (SStreamTask*)taosMemoryCalloc(1, sizeof(SStreamTask));
  if (pTask == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    stError("s-task:0x%" PRIx64 " failed malloc new stream task, size:%d, code:%s", streamId,
            (int32_t)sizeof(SStreamTask), tstrerror(terrno));
    return NULL;
  }

  pTask->ver = SSTREAM_TASK_VER;
  pTask->id.taskId = tGenIdPI32();
  pTask->id.streamId = streamId;

  pTask->info.taskLevel = taskLevel;
  pTask->info.fillHistory = fillHistory;
  pTask->info.triggerParam = triggerParam;
  pTask->subtableWithoutMd5 = subtableWithoutMd5;

  pTask->status.pSM = streamCreateStateMachine(pTask);
  if (pTask->status.pSM == NULL) {
    taosMemoryFreeClear(pTask);
    return NULL;
  }

  char buf[128] = {0};
  sprintf(buf, "0x%" PRIx64 "-0x%x", pTask->id.streamId, pTask->id.taskId);

  pTask->id.idStr = taosStrdup(buf);
  pTask->status.schedStatus = TASK_SCHED_STATUS__INACTIVE;
  pTask->status.taskStatus = fillHistory? TASK_STATUS__SCAN_HISTORY : TASK_STATUS__READY;
  pTask->inputq.status = TASK_INPUT_STATUS__NORMAL;
  pTask->outputq.status = TASK_OUTPUT_STATUS__NORMAL;

  pTask->taskCheckInfo.pList = taosArrayInit(4, sizeof(SDownstreamStatusInfo));
  taosThreadMutexInit(&pTask->taskCheckInfo.checkInfoLock, NULL);

  if (fillHistory) {
    ASSERT(hasFillhistory);
  }

  epsetAssign(&(pTask->info.mnodeEpset), pEpset);

  addToTaskset(pTaskList, pTask);
  return pTask;
}

int32_t tEncodeStreamEpInfo(SEncoder* pEncoder, const SStreamChildEpInfo* pInfo) {
  if (tEncodeI32(pEncoder, pInfo->taskId) < 0) return -1;
  if (tEncodeI32(pEncoder, pInfo->nodeId) < 0) return -1;
  if (tEncodeI32(pEncoder, pInfo->childId) < 0) return -1;
  if (tEncodeSEpSet(pEncoder, &pInfo->epSet) < 0) return -1;
  if (tEncodeI64(pEncoder, pInfo->stage) < 0) return -1;
  return 0;
}

int32_t tDecodeStreamEpInfo(SDecoder* pDecoder, SStreamChildEpInfo* pInfo) {
  if (tDecodeI32(pDecoder, &pInfo->taskId) < 0) return -1;
  if (tDecodeI32(pDecoder, &pInfo->nodeId) < 0) return -1;
  if (tDecodeI32(pDecoder, &pInfo->childId) < 0) return -1;
  if (tDecodeSEpSet(pDecoder, &pInfo->epSet) < 0) return -1;
  if (tDecodeI64(pDecoder, &pInfo->stage) < 0) return -1;
  return 0;
}

int32_t tEncodeStreamTask(SEncoder* pEncoder, const SStreamTask* pTask) {
  if (tStartEncode(pEncoder) < 0) return -1;
  if (tEncodeI64(pEncoder, pTask->ver) < 0) return -1;
  if (tEncodeI64(pEncoder, pTask->id.streamId) < 0) return -1;
  if (tEncodeI32(pEncoder, pTask->id.taskId) < 0) return -1;
  if (tEncodeI32(pEncoder, pTask->info.totalLevel) < 0) return -1;
  if (tEncodeI8(pEncoder, pTask->info.taskLevel) < 0) return -1;
  if (tEncodeI8(pEncoder, pTask->outputInfo.type) < 0) return -1;
  if (tEncodeI16(pEncoder, pTask->msgInfo.msgType) < 0) return -1;

  if (tEncodeI8(pEncoder, pTask->status.taskStatus) < 0) return -1;
  if (tEncodeI8(pEncoder, pTask->status.schedStatus) < 0) return -1;

  if (tEncodeI32(pEncoder, pTask->info.selfChildId) < 0) return -1;
  if (tEncodeI32(pEncoder, pTask->info.nodeId) < 0) return -1;
  if (tEncodeSEpSet(pEncoder, &pTask->info.epSet) < 0) return -1;
  if (tEncodeSEpSet(pEncoder, &pTask->info.mnodeEpset) < 0) return -1;

  if (tEncodeI64(pEncoder, pTask->chkInfo.checkpointId) < 0) return -1;
  if (tEncodeI64(pEncoder, pTask->chkInfo.checkpointVer) < 0) return -1;
  if (tEncodeI8(pEncoder, pTask->info.fillHistory) < 0) return -1;

  if (tEncodeI64(pEncoder, pTask->hTaskInfo.id.streamId)) return -1;
  int32_t taskId = pTask->hTaskInfo.id.taskId;
  if (tEncodeI32(pEncoder, taskId)) return -1;

  if (tEncodeI64(pEncoder, pTask->streamTaskId.streamId)) return -1;
  taskId = pTask->streamTaskId.taskId;
  if (tEncodeI32(pEncoder, taskId)) return -1;

  if (tEncodeU64(pEncoder, pTask->dataRange.range.minVer)) return -1;
  if (tEncodeU64(pEncoder, pTask->dataRange.range.maxVer)) return -1;
  if (tEncodeI64(pEncoder, pTask->dataRange.window.skey)) return -1;
  if (tEncodeI64(pEncoder, pTask->dataRange.window.ekey)) return -1;

  int32_t epSz = taosArrayGetSize(pTask->upstreamInfo.pList);
  if (tEncodeI32(pEncoder, epSz) < 0) return -1;
  for (int32_t i = 0; i < epSz; i++) {
    SStreamChildEpInfo* pInfo = taosArrayGetP(pTask->upstreamInfo.pList, i);
    if (tEncodeStreamEpInfo(pEncoder, pInfo) < 0) return -1;
  }

  if (pTask->info.taskLevel != TASK_LEVEL__SINK) {
    if (tEncodeCStr(pEncoder, pTask->exec.qmsg) < 0) return -1;
  }

  if (pTask->outputInfo.type == TASK_OUTPUT__TABLE) {
    if (tEncodeI64(pEncoder, pTask->outputInfo.tbSink.stbUid) < 0) return -1;
    if (tEncodeCStr(pEncoder, pTask->outputInfo.tbSink.stbFullName) < 0) return -1;
    if (tEncodeSSchemaWrapper(pEncoder, pTask->outputInfo.tbSink.pSchemaWrapper) < 0) return -1;
  } else if (pTask->outputInfo.type == TASK_OUTPUT__SMA) {
    if (tEncodeI64(pEncoder, pTask->outputInfo.smaSink.smaId) < 0) return -1;
  } else if (pTask->outputInfo.type == TASK_OUTPUT__FETCH) {
    if (tEncodeI8(pEncoder, pTask->outputInfo.fetchSink.reserved) < 0) return -1;
  } else if (pTask->outputInfo.type == TASK_OUTPUT__FIXED_DISPATCH) {
    if (tEncodeI32(pEncoder, pTask->outputInfo.fixedDispatcher.taskId) < 0) return -1;
    if (tEncodeI32(pEncoder, pTask->outputInfo.fixedDispatcher.nodeId) < 0) return -1;
    if (tEncodeSEpSet(pEncoder, &pTask->outputInfo.fixedDispatcher.epSet) < 0) return -1;
  } else if (pTask->outputInfo.type == TASK_OUTPUT__SHUFFLE_DISPATCH) {
    if (tSerializeSUseDbRspImp(pEncoder, &pTask->outputInfo.shuffleDispatcher.dbInfo) < 0) return -1;
    if (tEncodeCStr(pEncoder, pTask->outputInfo.shuffleDispatcher.stbFullName) < 0) return -1;
  }
  if (tEncodeI64(pEncoder, pTask->info.triggerParam) < 0) return -1;
  if (tEncodeI8(pEncoder, pTask->subtableWithoutMd5) < 0) return -1;
  if (tEncodeCStrWithLen(pEncoder, pTask->reserve, sizeof(pTask->reserve) - 1) < 0) return -1;

  tEndEncode(pEncoder);
  return pEncoder->pos;
}

int32_t tDecodeStreamTask(SDecoder* pDecoder, SStreamTask* pTask) {
  int32_t taskId = 0;

  if (tStartDecode(pDecoder) < 0) return -1;
  if (tDecodeI64(pDecoder, &pTask->ver) < 0) return -1;
  if (pTask->ver <= SSTREAM_TASK_INCOMPATIBLE_VER || pTask->ver > SSTREAM_TASK_VER) return -1;

  if (tDecodeI64(pDecoder, &pTask->id.streamId) < 0) return -1;
  if (tDecodeI32(pDecoder, &pTask->id.taskId) < 0) return -1;
  if (tDecodeI32(pDecoder, &pTask->info.totalLevel) < 0) return -1;
  if (tDecodeI8(pDecoder, &pTask->info.taskLevel) < 0) return -1;
  if (tDecodeI8(pDecoder, &pTask->outputInfo.type) < 0) return -1;
  if (tDecodeI16(pDecoder, &pTask->msgInfo.msgType) < 0) return -1;

  if (tDecodeI8(pDecoder, &pTask->status.taskStatus) < 0) return -1;
  if (tDecodeI8(pDecoder, &pTask->status.schedStatus) < 0) return -1;

  if (tDecodeI32(pDecoder, &pTask->info.selfChildId) < 0) return -1;
  if (tDecodeI32(pDecoder, &pTask->info.nodeId) < 0) return -1;
  if (tDecodeSEpSet(pDecoder, &pTask->info.epSet) < 0) return -1;
  if (tDecodeSEpSet(pDecoder, &pTask->info.mnodeEpset) < 0) return -1;

  if (tDecodeI64(pDecoder, &pTask->chkInfo.checkpointId) < 0) return -1;
  if (tDecodeI64(pDecoder, &pTask->chkInfo.checkpointVer) < 0) return -1;
  if (tDecodeI8(pDecoder, &pTask->info.fillHistory) < 0) return -1;

  if (tDecodeI64(pDecoder, &pTask->hTaskInfo.id.streamId)) return -1;
  if (tDecodeI32(pDecoder, &taskId)) return -1;
  pTask->hTaskInfo.id.taskId = taskId;

  if (tDecodeI64(pDecoder, &pTask->streamTaskId.streamId)) return -1;
  if (tDecodeI32(pDecoder, &taskId)) return -1;
  pTask->streamTaskId.taskId = taskId;

  if (tDecodeU64(pDecoder, &pTask->dataRange.range.minVer)) return -1;
  if (tDecodeU64(pDecoder, &pTask->dataRange.range.maxVer)) return -1;
  if (tDecodeI64(pDecoder, &pTask->dataRange.window.skey)) return -1;
  if (tDecodeI64(pDecoder, &pTask->dataRange.window.ekey)) return -1;

  int32_t epSz = -1;
  if (tDecodeI32(pDecoder, &epSz) < 0) return -1;

  pTask->upstreamInfo.pList = taosArrayInit(epSz, POINTER_BYTES);
  for (int32_t i = 0; i < epSz; i++) {
    SStreamChildEpInfo* pInfo = taosMemoryCalloc(1, sizeof(SStreamChildEpInfo));
    if (pInfo == NULL) return -1;
    if (tDecodeStreamEpInfo(pDecoder, pInfo) < 0) {
      taosMemoryFreeClear(pInfo);
      return -1;
    }
    taosArrayPush(pTask->upstreamInfo.pList, &pInfo);
  }

  if (pTask->info.taskLevel != TASK_LEVEL__SINK) {
    if (tDecodeCStrAlloc(pDecoder, &pTask->exec.qmsg) < 0) return -1;
  }

  if (pTask->outputInfo.type == TASK_OUTPUT__TABLE) {
    if (tDecodeI64(pDecoder, &pTask->outputInfo.tbSink.stbUid) < 0) return -1;
    if (tDecodeCStrTo(pDecoder, pTask->outputInfo.tbSink.stbFullName) < 0) return -1;
    pTask->outputInfo.tbSink.pSchemaWrapper = taosMemoryCalloc(1, sizeof(SSchemaWrapper));
    if (pTask->outputInfo.tbSink.pSchemaWrapper == NULL) return -1;
    if (tDecodeSSchemaWrapper(pDecoder, pTask->outputInfo.tbSink.pSchemaWrapper) < 0) return -1;
  } else if (pTask->outputInfo.type == TASK_OUTPUT__SMA) {
    if (tDecodeI64(pDecoder, &pTask->outputInfo.smaSink.smaId) < 0) return -1;
  } else if (pTask->outputInfo.type == TASK_OUTPUT__FETCH) {
    if (tDecodeI8(pDecoder, &pTask->outputInfo.fetchSink.reserved) < 0) return -1;
  } else if (pTask->outputInfo.type == TASK_OUTPUT__FIXED_DISPATCH) {
    if (tDecodeI32(pDecoder, &pTask->outputInfo.fixedDispatcher.taskId) < 0) return -1;
    if (tDecodeI32(pDecoder, &pTask->outputInfo.fixedDispatcher.nodeId) < 0) return -1;
    if (tDecodeSEpSet(pDecoder, &pTask->outputInfo.fixedDispatcher.epSet) < 0) return -1;
  } else if (pTask->outputInfo.type == TASK_OUTPUT__SHUFFLE_DISPATCH) {
    if (tDeserializeSUseDbRspImp(pDecoder, &pTask->outputInfo.shuffleDispatcher.dbInfo) < 0) return -1;
    if (tDecodeCStrTo(pDecoder, pTask->outputInfo.shuffleDispatcher.stbFullName) < 0) return -1;
  }
  if (tDecodeI64(pDecoder, &pTask->info.triggerParam) < 0) return -1;
  if (pTask->ver >= SSTREAM_TASK_SUBTABLE_CHANGED_VER){
    if (tDecodeI8(pDecoder, &pTask->subtableWithoutMd5) < 0) return -1;
  }
  if (tDecodeCStrTo(pDecoder, pTask->reserve) < 0) return -1;

  tEndDecode(pDecoder);
  return 0;
}

int32_t tDecodeStreamTaskChkInfo(SDecoder* pDecoder, SCheckpointInfo* pChkpInfo) {
  int64_t skip64;
  int8_t  skip8;
  int32_t skip32;
  int16_t skip16;
  SEpSet  epSet;

  if (tStartDecode(pDecoder) < 0) return -1;
  if (tDecodeI64(pDecoder, &pChkpInfo->msgVer) < 0) return -1;
  // if (ver <= SSTREAM_TASK_INCOMPATIBLE_VER) return -1;

  if (tDecodeI64(pDecoder, &skip64) < 0) return -1;
  if (tDecodeI32(pDecoder, &skip32) < 0) return -1;
  if (tDecodeI32(pDecoder, &skip32) < 0) return -1;
  if (tDecodeI8(pDecoder, &skip8) < 0) return -1;
  if (tDecodeI8(pDecoder, &skip8) < 0) return -1;
  if (tDecodeI16(pDecoder, &skip16) < 0) return -1;

  if (tDecodeI8(pDecoder, &skip8) < 0) return -1;
  if (tDecodeI8(pDecoder, &skip8) < 0) return -1;

  if (tDecodeI32(pDecoder, &skip32) < 0) return -1;
  if (tDecodeI32(pDecoder, &skip32) < 0) return -1;
  if (tDecodeSEpSet(pDecoder, &epSet) < 0) return -1;
  if (tDecodeSEpSet(pDecoder, &epSet) < 0) return -1;

  if (tDecodeI64(pDecoder, &pChkpInfo->checkpointId) < 0) return -1;
  if (tDecodeI64(pDecoder, &pChkpInfo->checkpointVer) < 0) return -1;

  tEndDecode(pDecoder);
  return 0;
}

int32_t tDecodeStreamTaskId(SDecoder* pDecoder, STaskId* pTaskId) {
  int64_t ver;
  if (tStartDecode(pDecoder) < 0) return -1;
  if (tDecodeI64(pDecoder, &ver) < 0) return -1;
  if (ver <= SSTREAM_TASK_INCOMPATIBLE_VER) return -1;

  if (tDecodeI64(pDecoder, &pTaskId->streamId) < 0) return -1;

  int32_t taskId = 0;
  if (tDecodeI32(pDecoder, &taskId) < 0) return -1;

  pTaskId->taskId = taskId;
  tEndDecode(pDecoder);
  return 0;
}

void tFreeStreamTask(SStreamTask* pTask) {
  char*                p = NULL;
  int32_t              taskId = pTask->id.taskId;
  STaskExecStatisInfo* pStatis = &pTask->execInfo;

  ETaskStatus status1 = TASK_STATUS__UNINIT;
  taosThreadMutexLock(&pTask->lock);
  if (pTask->status.pSM != NULL) {
    SStreamTaskState* pStatus = streamTaskGetStatus(pTask);
    p = pStatus->name;
    status1 = pStatus->state;
  }
  taosThreadMutexUnlock(&pTask->lock);

  stDebug("start to free s-task:0x%x, %p, state:%s", taskId, pTask, p);

  SCheckpointInfo* pCkInfo = &pTask->chkInfo;
  stDebug("s-task:0x%x task exec summary: create:%" PRId64 ", init:%" PRId64 ", start:%" PRId64
          ", updateCount:%d latestUpdate:%" PRId64 ", latestCheckPoint:%" PRId64 ", ver:%" PRId64
          " nextProcessVer:%" PRId64 ", checkpointCount:%d",
          taskId, pStatis->created, pStatis->checkTs, pStatis->readyTs, pStatis->updateCount,
          pStatis->latestUpdateTs, pCkInfo->checkpointId, pCkInfo->checkpointVer, pCkInfo->nextProcessVer,
          pStatis->checkpoint);

  // remove the ref by timer
  while (pTask->status.timerActive > 0) {
    stDebug("s-task:%s wait for task stop timer activities, ref:%d", pTask->id.idStr, pTask->status.timerActive);
    taosMsleep(100);
  }

  if (pTask->schedInfo.pDelayTimer != NULL) {
    taosTmrStop(pTask->schedInfo.pDelayTimer);
    pTask->schedInfo.pDelayTimer = NULL;
  }

  if (pTask->hTaskInfo.pTimer != NULL) {
    /*bool ret = */taosTmrStop(pTask->hTaskInfo.pTimer);
    pTask->hTaskInfo.pTimer = NULL;
  }

  if (pTask->msgInfo.pTimer != NULL) {
    /*bool ret = */taosTmrStop(pTask->msgInfo.pTimer);
    pTask->msgInfo.pTimer = NULL;
  }

  if (pTask->inputq.queue) {
    streamQueueClose(pTask->inputq.queue, pTask->id.taskId);
  }

  if (pTask->outputq.queue) {
    streamQueueClose(pTask->outputq.queue, pTask->id.taskId);
  }

  if (pTask->exec.qmsg) {
    taosMemoryFree(pTask->exec.qmsg);
  }

  if (pTask->exec.pExecutor) {
    qDestroyTask(pTask->exec.pExecutor);
    pTask->exec.pExecutor = NULL;
  }

  if (pTask->exec.pWalReader != NULL) {
    walCloseReader(pTask->exec.pWalReader);
  }

  streamClearChkptReadyMsg(pTask);
  pTask->pReadyMsgList = taosArrayDestroy(pTask->pReadyMsgList);

  if (pTask->msgInfo.pData != NULL) {
    clearBufferedDispatchMsg(pTask);
  }

  if (pTask->outputInfo.type == TASK_OUTPUT__TABLE) {
    tDeleteSchemaWrapper(pTask->outputInfo.tbSink.pSchemaWrapper);
    taosMemoryFree(pTask->outputInfo.tbSink.pTSchema);
    tSimpleHashCleanup(pTask->outputInfo.tbSink.pTblInfo);
  } else if (pTask->outputInfo.type == TASK_OUTPUT__SHUFFLE_DISPATCH) {
    taosArrayDestroy(pTask->outputInfo.shuffleDispatcher.dbInfo.pVgroupInfos);
  }

  streamTaskCleanCheckInfo(&pTask->taskCheckInfo);

  if (pTask->pState) {
    stDebug("s-task:0x%x start to free task state", taskId);
    streamStateClose(pTask->pState, status1 == TASK_STATUS__DROPPING);
    taskDbRemoveRef(pTask->pBackend);
  }

  if (pTask->id.idStr != NULL) {
    taosMemoryFree((void*)pTask->id.idStr);
  }

  if (pTask->pNameMap) {
    tSimpleHashCleanup(pTask->pNameMap);
  }

  if (pTask->pRspMsgList != NULL) {
    taosArrayDestroyEx(pTask->pRspMsgList, freeItem);
    pTask->pRspMsgList = NULL;
  }

  pTask->status.pSM = streamDestroyStateMachine(pTask->status.pSM);
  streamTaskDestroyUpstreamInfo(&pTask->upstreamInfo);

  pTask->msgInfo.pRetryList = taosArrayDestroy(pTask->msgInfo.pRetryList);
  taosMemoryFree(pTask->outputInfo.pTokenBucket);
  taosThreadMutexDestroy(&pTask->lock);

  pTask->outputInfo.pDownstreamUpdateList = taosArrayDestroy(pTask->outputInfo.pDownstreamUpdateList);

  taosMemoryFree(pTask);
  stDebug("s-task:0x%x free task completed", taskId);
}

static void setInitialVersionInfo(SStreamTask* pTask, int64_t ver) {
  SCheckpointInfo* pChkInfo = &pTask->chkInfo;
  SDataRange*      pRange = &pTask->dataRange;

  // only set the version info for stream tasks without fill-history task
  if ((pTask->info.fillHistory == 0) && (!HAS_RELATED_FILLHISTORY_TASK(pTask))) {
    pChkInfo->checkpointVer = ver - 1;  // only update when generating checkpoint
    pChkInfo->processedVer = ver - 1;   // already processed version
    pChkInfo->nextProcessVer = ver;     // next processed version

    pRange->range.maxVer = ver;
    pRange->range.minVer = ver;
  } else {
    // the initial value of processedVer/nextProcessVer/checkpointVer for stream task with related fill-history task
    // is set at the mnode.
    if (pTask->info.fillHistory == 1) {
      pChkInfo->checkpointVer = pRange->range.maxVer;
      pChkInfo->processedVer = pRange->range.maxVer;
      pChkInfo->nextProcessVer = pRange->range.maxVer + 1;
    } else {
      pChkInfo->checkpointVer = pRange->range.minVer - 1;
      pChkInfo->processedVer = pRange->range.minVer - 1;
      pChkInfo->nextProcessVer = pRange->range.minVer;

      {  // for compatible purpose, remove it later
        if (pRange->range.minVer == 0) {
          pChkInfo->checkpointVer = 0;
          pChkInfo->processedVer = 0;
          pChkInfo->nextProcessVer = 1;
          stDebug("s-task:%s update the processedVer to 0 from -1 due to compatible purpose", pTask->id.idStr);
        }
      }
    }
  }
}

int32_t streamTaskInit(SStreamTask* pTask, SStreamMeta* pMeta, SMsgCb* pMsgCb, int64_t ver) {
  pTask->id.idStr = createStreamTaskIdStr(pTask->id.streamId, pTask->id.taskId);
  pTask->refCnt = 1;

  pTask->inputq.status = TASK_INPUT_STATUS__NORMAL;
  pTask->outputq.status = TASK_OUTPUT_STATUS__NORMAL;

  pTask->inputq.queue = streamQueueOpen(512 << 10);
  pTask->outputq.queue = streamQueueOpen(512 << 10);
  if (pTask->inputq.queue == NULL || pTask->outputq.queue == NULL) {
    stError("s-task:%s failed to prepare the input/output queue, initialize task failed", pTask->id.idStr);
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  pTask->status.schedStatus = TASK_SCHED_STATUS__INACTIVE;
  pTask->status.timerActive = 0;
  pTask->status.pSM = streamCreateStateMachine(pTask);
  if (pTask->status.pSM == NULL) {
    stError("s-task:%s failed create state-machine for stream task, initialization failed, code:%s", pTask->id.idStr,
            tstrerror(terrno));
    return terrno;
  }

  pTask->execInfo.created = taosGetTimestampMs();
  setInitialVersionInfo(pTask, ver);

  pTask->pMeta = pMeta;
  pTask->pMsgCb = pMsgCb;
  pTask->msgInfo.pRetryList = taosArrayInit(4, sizeof(int32_t));

  TdThreadMutexAttr attr = {0};
  int               code = taosThreadMutexAttrInit(&attr);
  if (code != 0) {
    stError("s-task:%s initElapsed mutex attr failed, code:%s", pTask->id.idStr, tstrerror(code));
    return code;
  }

  code = taosThreadMutexAttrSetType(&attr, PTHREAD_MUTEX_RECURSIVE);
  if (code != 0) {
    stError("s-task:%s set mutex attr recursive, code:%s", pTask->id.idStr, tstrerror(code));
    return code;
  }

  taosThreadMutexInit(&pTask->lock, &attr);
  taosThreadMutexAttrDestroy(&attr);
  streamTaskOpenAllUpstreamInput(pTask);

  STaskOutputInfo* pOutputInfo = &pTask->outputInfo;
  pOutputInfo->pTokenBucket = taosMemoryCalloc(1, sizeof(STokenBucket));
  if (pOutputInfo->pTokenBucket == NULL) {
    stError("s-task:%s failed to prepare the tokenBucket, code:%s", pTask->id.idStr,
            tstrerror(TSDB_CODE_OUT_OF_MEMORY));
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  // 2MiB per second for sink task
  // 50 times sink operator per second
  streamTaskInitTokenBucket(pOutputInfo->pTokenBucket, 35, 35, tsSinkDataRate, pTask->id.idStr);
  pOutputInfo->pDownstreamUpdateList = taosArrayInit(4, sizeof(SDownstreamTaskEpset));
  if (pOutputInfo->pDownstreamUpdateList == NULL) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  return TSDB_CODE_SUCCESS;
}

int32_t streamTaskGetNumOfDownstream(const SStreamTask* pTask) {
  if (pTask->info.taskLevel == TASK_LEVEL__SINK) {
    return 0;
  }

  int32_t type = pTask->outputInfo.type;
  if (type == TASK_OUTPUT__TABLE) {
    return 0;
  } else if (type == TASK_OUTPUT__FIXED_DISPATCH) {
    return 1;
  } else {
    SArray* vgInfo = pTask->outputInfo.shuffleDispatcher.dbInfo.pVgroupInfos;
    return taosArrayGetSize(vgInfo);
  }
}

int32_t streamTaskSetUpstreamInfo(SStreamTask* pTask, const SStreamTask* pUpstreamTask) {
  SStreamChildEpInfo* pEpInfo = createStreamTaskEpInfo(pUpstreamTask);
  if (pEpInfo == NULL) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  if (pTask->upstreamInfo.pList == NULL) {
    pTask->upstreamInfo.pList = taosArrayInit(4, POINTER_BYTES);
  }

  taosArrayPush(pTask->upstreamInfo.pList, &pEpInfo);
  return TSDB_CODE_SUCCESS;
}

void streamTaskUpdateUpstreamInfo(SStreamTask* pTask, int32_t nodeId, const SEpSet* pEpSet) {
  char buf[512] = {0};
  epsetToStr(pEpSet, buf, tListLen(buf));

  int32_t numOfUpstream = taosArrayGetSize(pTask->upstreamInfo.pList);
  for (int32_t i = 0; i < numOfUpstream; ++i) {
    SStreamChildEpInfo* pInfo = taosArrayGetP(pTask->upstreamInfo.pList, i);
    if (pInfo->nodeId == nodeId) {
      epsetAssign(&pInfo->epSet, pEpSet);
      stDebug("s-task:0x%x update the upstreamInfo taskId:0x%x(nodeId:%d) newEpset:%s", pTask->id.taskId, pInfo->taskId,
              nodeId, buf);
      break;
    }
  }
}

void streamTaskDestroyUpstreamInfo(SUpstreamInfo* pUpstreamInfo) {
  if (pUpstreamInfo->pList != NULL) {
    taosArrayDestroyEx(pUpstreamInfo->pList, freeUpstreamItem);
    pUpstreamInfo->numOfClosed = 0;
    pUpstreamInfo->pList = NULL;
  }
}

void streamTaskSetFixedDownstreamInfo(SStreamTask* pTask, const SStreamTask* pDownstreamTask) {
  STaskDispatcherFixed* pDispatcher = &pTask->outputInfo.fixedDispatcher;
  pDispatcher->taskId = pDownstreamTask->id.taskId;
  pDispatcher->nodeId = pDownstreamTask->info.nodeId;
  pDispatcher->epSet = pDownstreamTask->info.epSet;

  pTask->outputInfo.type = TASK_OUTPUT__FIXED_DISPATCH;
  pTask->msgInfo.msgType = TDMT_STREAM_TASK_DISPATCH;
}

void streamTaskUpdateDownstreamInfo(SStreamTask* pTask, int32_t nodeId, const SEpSet* pEpSet) {
  char buf[512] = {0};
  epsetToStr(pEpSet, buf, tListLen(buf));
  int32_t id = pTask->id.taskId;

  int8_t type = pTask->outputInfo.type;
  if (type == TASK_OUTPUT__SHUFFLE_DISPATCH) {
    SArray* pVgs = pTask->outputInfo.shuffleDispatcher.dbInfo.pVgroupInfos;

    int32_t numOfVgroups = taosArrayGetSize(pVgs);
    for (int32_t i = 0; i < numOfVgroups; i++) {
      SVgroupInfo* pVgInfo = taosArrayGet(pVgs, i);

      if (pVgInfo->vgId == nodeId) {
        epsetAssign(&pVgInfo->epSet, pEpSet);
        stDebug("s-task:0x%x update the dispatch info, task:0x%x(nodeId:%d) newEpset:%s", id, pVgInfo->taskId, nodeId,
                buf);
        break;
      }
    }
  } else if (type == TASK_OUTPUT__FIXED_DISPATCH) {
    STaskDispatcherFixed* pDispatcher = &pTask->outputInfo.fixedDispatcher;
    if (pDispatcher->nodeId == nodeId) {
      epsetAssign(&pDispatcher->epSet, pEpSet);
      stDebug("s-task:0x%x update the dispatch info, task:0x%x(nodeId:%d) newEpset:%s", id, pDispatcher->taskId, nodeId,
              buf);
    }
  }
}

int32_t streamTaskStop(SStreamTask* pTask) {
  int32_t     vgId = pTask->pMeta->vgId;
  int64_t     st = taosGetTimestampMs();
  const char* id = pTask->id.idStr;

  streamTaskHandleEvent(pTask->status.pSM, TASK_EVENT_STOP);
  qKillTask(pTask->exec.pExecutor, TSDB_CODE_SUCCESS);
  while (!streamTaskIsIdle(pTask)) {
    stDebug("s-task:%s level:%d wait for task to be idle and then close, check again in 100ms", id,
            pTask->info.taskLevel);
    taosMsleep(100);
  }

  int64_t el = taosGetTimestampMs() - st;
  stDebug("vgId:%d s-task:%s is closed in %" PRId64 " ms", vgId, id, el);
  return 0;
}

int32_t streamTaskUpdateEpsetInfo(SStreamTask* pTask, SArray* pNodeList) {
  STaskExecStatisInfo* p = &pTask->execInfo;

  int32_t numOfNodes = taosArrayGetSize(pNodeList);
  int64_t prevTs = p->latestUpdateTs;

  p->latestUpdateTs = taosGetTimestampMs();
  p->updateCount += 1;
  stDebug("s-task:0x%x update task nodeEp epset, updatedNodes:%d, updateCount:%d, prevTs:%" PRId64, pTask->id.taskId,
          numOfNodes, p->updateCount, prevTs);

  for (int32_t i = 0; i < taosArrayGetSize(pNodeList); ++i) {
    SNodeUpdateInfo* pInfo = taosArrayGet(pNodeList, i);
    doUpdateTaskEpset(pTask, pInfo->nodeId, &pInfo->newEp);
  }
  return 0;
}

void streamTaskResetUpstreamStageInfo(SStreamTask* pTask) {
  if (pTask->info.taskLevel == TASK_LEVEL__SOURCE) {
    return;
  }

  int32_t size = taosArrayGetSize(pTask->upstreamInfo.pList);
  for (int32_t i = 0; i < size; ++i) {
    SStreamChildEpInfo* pInfo = taosArrayGetP(pTask->upstreamInfo.pList, i);
    pInfo->stage = -1;
  }

  stDebug("s-task:%s reset all upstream tasks stage info", pTask->id.idStr);
}

void streamTaskOpenAllUpstreamInput(SStreamTask* pTask) {
  int32_t num = taosArrayGetSize(pTask->upstreamInfo.pList);
  if (num == 0) {
    return;
  }

  for (int32_t i = 0; i < num; ++i) {
    SStreamChildEpInfo* pInfo = taosArrayGetP(pTask->upstreamInfo.pList, i);
    pInfo->dataAllowed = true;
  }

  pTask->upstreamInfo.numOfClosed = 0;
  stDebug("s-task:%s opening up inputQ for %d upstream tasks", pTask->id.idStr, num);
}

void streamTaskCloseUpstreamInput(SStreamTask* pTask, int32_t taskId) {
  SStreamChildEpInfo* pInfo = streamTaskGetUpstreamTaskEpInfo(pTask, taskId);
  if (pInfo != NULL) {
    pInfo->dataAllowed = false;
  }
}

bool streamTaskIsAllUpstreamClosed(SStreamTask* pTask) {
  return pTask->upstreamInfo.numOfClosed == taosArrayGetSize(pTask->upstreamInfo.pList);
}

bool streamTaskSetSchedStatusWait(SStreamTask* pTask) {
  bool ret = false;

  taosThreadMutexLock(&pTask->lock);
  if (pTask->status.schedStatus == TASK_SCHED_STATUS__INACTIVE) {
    pTask->status.schedStatus = TASK_SCHED_STATUS__WAITING;
    ret = true;
  }
  taosThreadMutexUnlock(&pTask->lock);

  return ret;
}

int8_t streamTaskSetSchedStatusActive(SStreamTask* pTask) {
  taosThreadMutexLock(&pTask->lock);
  int8_t status = pTask->status.schedStatus;
  if (status == TASK_SCHED_STATUS__WAITING) {
    pTask->status.schedStatus = TASK_SCHED_STATUS__ACTIVE;
  }
  taosThreadMutexUnlock(&pTask->lock);

  return status;
}

int8_t streamTaskSetSchedStatusInactive(SStreamTask* pTask) {
  taosThreadMutexLock(&pTask->lock);
  int8_t status = pTask->status.schedStatus;
  ASSERT(status == TASK_SCHED_STATUS__WAITING || status == TASK_SCHED_STATUS__ACTIVE ||
         status == TASK_SCHED_STATUS__INACTIVE);
  pTask->status.schedStatus = TASK_SCHED_STATUS__INACTIVE;
  taosThreadMutexUnlock(&pTask->lock);

  return status;
}

int32_t streamTaskClearHTaskAttr(SStreamTask* pTask, int32_t resetRelHalt) {
  SStreamMeta* pMeta = pTask->pMeta;
  STaskId      sTaskId = {.streamId = pTask->streamTaskId.streamId, .taskId = pTask->streamTaskId.taskId};
  if (pTask->info.fillHistory == 0) {
    return TSDB_CODE_SUCCESS;
  }

  SStreamTask** ppStreamTask = (SStreamTask**)taosHashGet(pMeta->pTasksMap, &sTaskId, sizeof(sTaskId));
  if (ppStreamTask != NULL) {
    stDebug("s-task:%s clear the related stream task:0x%x attr to fill-history task", pTask->id.idStr,
            (int32_t)sTaskId.taskId);

    taosThreadMutexLock(&(*ppStreamTask)->lock);
    CLEAR_RELATED_FILLHISTORY_TASK((*ppStreamTask));

    if (resetRelHalt) {
      stDebug("s-task:0x%" PRIx64 " set the persistent status attr to be ready, prev:%s, status in sm:%s",
              sTaskId.taskId, streamTaskGetStatusStr((*ppStreamTask)->status.taskStatus),
              streamTaskGetStatus(*ppStreamTask)->name);
      (*ppStreamTask)->status.taskStatus = TASK_STATUS__READY;
    }

    streamMetaSaveTask(pMeta, *ppStreamTask);
    taosThreadMutexUnlock(&(*ppStreamTask)->lock);
  }

  return TSDB_CODE_SUCCESS;
}

int32_t streamBuildAndSendDropTaskMsg(SMsgCb* pMsgCb, int32_t vgId, SStreamTaskId* pTaskId, int64_t resetRelHalt) {
  SVDropStreamTaskReq* pReq = rpcMallocCont(sizeof(SVDropStreamTaskReq));
  if (pReq == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }

  pReq->head.vgId = vgId;
  pReq->taskId = pTaskId->taskId;
  pReq->streamId = pTaskId->streamId;
  pReq->resetRelHalt = resetRelHalt;

  SRpcMsg msg = {.msgType = TDMT_STREAM_TASK_DROP, .pCont = pReq, .contLen = sizeof(SVDropStreamTaskReq)};
  int32_t code = tmsgPutToQueue(pMsgCb, WRITE_QUEUE, &msg);
  if (code != TSDB_CODE_SUCCESS) {
    stError("vgId:%d failed to send drop task:0x%x msg, code:%s", vgId, pTaskId->taskId, tstrerror(code));
    return code;
  }

  stDebug("vgId:%d build and send drop task:0x%x msg", vgId, pTaskId->taskId);
  return code;
}

STaskId streamTaskGetTaskId(const SStreamTask* pTask) {
  STaskId id = {.streamId = pTask->id.streamId, .taskId = pTask->id.taskId};
  return id;
}

void streamTaskInitForLaunchHTask(SHistoryTaskInfo* pInfo) {
  pInfo->waitInterval = LAUNCH_HTASK_INTERVAL;
  pInfo->tickCount = ceil(LAUNCH_HTASK_INTERVAL / WAIT_FOR_MINIMAL_INTERVAL);
  pInfo->retryTimes = 0;
}

void streamTaskSetRetryInfoForLaunch(SHistoryTaskInfo* pInfo) {
  ASSERT(pInfo->tickCount == 0);

  pInfo->waitInterval *= RETRY_LAUNCH_INTERVAL_INC_RATE;
  pInfo->tickCount = ceil(pInfo->waitInterval / WAIT_FOR_MINIMAL_INTERVAL);
  pInfo->retryTimes += 1;
}

void streamTaskStatusInit(STaskStatusEntry* pEntry, const SStreamTask* pTask) {
  pEntry->id.streamId = pTask->id.streamId;
  pEntry->id.taskId = pTask->id.taskId;
  pEntry->stage = -1;
  pEntry->nodeId = pTask->info.nodeId;
  pEntry->status = TASK_STATUS__STOP;
}

void streamTaskStatusCopy(STaskStatusEntry* pDst, const STaskStatusEntry* pSrc) {
  pDst->stage = pSrc->stage;
  pDst->inputQUsed = pSrc->inputQUsed;
  pDst->inputRate = pSrc->inputRate;
  pDst->processedVer = pSrc->processedVer;
  pDst->verRange = pSrc->verRange;
  pDst->sinkQuota = pSrc->sinkQuota;
  pDst->sinkDataSize = pSrc->sinkDataSize;
  pDst->checkpointInfo = pSrc->checkpointInfo;
  pDst->startCheckpointId = pSrc->startCheckpointId;
  pDst->startCheckpointVer = pSrc->startCheckpointVer;

  pDst->startTime = pSrc->startTime;
  pDst->hTaskId = pSrc->hTaskId;
}

static int32_t taskPauseCallback(SStreamTask* pTask, void* param) {
  SStreamMeta* pMeta = pTask->pMeta;

  int32_t num = atomic_add_fetch_32(&pMeta->numOfPausedTasks, 1);
  stInfo("vgId:%d s-task:%s pause stream task. pause task num:%d", pMeta->vgId, pTask->id.idStr, num);

  // in case of fill-history task, stop the tsdb file scan operation.
  if (pTask->info.fillHistory == 1) {
    void* pExecutor = pTask->exec.pExecutor;
    qKillTask(pExecutor, TSDB_CODE_SUCCESS);
  }

  stDebug("vgId:%d s-task:%s set pause flag and pause task", pMeta->vgId, pTask->id.idStr);
  return TSDB_CODE_SUCCESS;
}

void streamTaskPause(SStreamMeta* pMeta, SStreamTask* pTask) {
  streamTaskHandleEventAsync(pTask->status.pSM, TASK_EVENT_PAUSE, taskPauseCallback, NULL);
}

void streamTaskResume(SStreamTask* pTask) {
  SStreamTaskState prevState = *streamTaskGetStatus(pTask);

  SStreamMeta* pMeta = pTask->pMeta;
  int32_t      code = streamTaskRestoreStatus(pTask);
  if (code == TSDB_CODE_SUCCESS) {
    char*   pNew = streamTaskGetStatus(pTask)->name;
    int32_t num = atomic_sub_fetch_32(&pMeta->numOfPausedTasks, 1);
    stInfo("s-task:%s status:%s resume from %s, paused task(s):%d", pTask->id.idStr, pNew, prevState.name, num);
  } else {
    stInfo("s-task:%s status:%s no need to resume, paused task(s):%d", pTask->id.idStr, prevState.name, pMeta->numOfPausedTasks);
  }
}

bool streamTaskIsSinkTask(const SStreamTask* pTask) { return pTask->info.taskLevel == TASK_LEVEL__SINK; }

int32_t streamTaskSendCheckpointReq(SStreamTask* pTask) {
  int32_t     code;
  int32_t     tlen = 0;
  int32_t     vgId = pTask->pMeta->vgId;
  const char* id = pTask->id.idStr;

  SStreamTaskCheckpointReq req = {.streamId = pTask->id.streamId, .taskId = pTask->id.taskId, .nodeId = vgId};
  tEncodeSize(tEncodeStreamTaskCheckpointReq, &req, tlen, code);
  if (code < 0) {
    stError("s-task:%s vgId:%d encode stream task req checkpoint failed, code:%s", id, vgId, tstrerror(code));
    return -1;
  }

  void* buf = rpcMallocCont(tlen);
  if (buf == NULL) {
    stError("s-task:%s vgId:%d encode stream task req checkpoint msg failed, code:%s", id, vgId,
            tstrerror(TSDB_CODE_OUT_OF_MEMORY));
    return -1;
  }

  SEncoder encoder;
  tEncoderInit(&encoder, buf, tlen);
  if ((code = tEncodeStreamTaskCheckpointReq(&encoder, &req)) < 0) {
    rpcFreeCont(buf);
    stError("s-task:%s vgId:%d encode stream task req checkpoint msg failed, code:%s", id, vgId, tstrerror(code));
    return -1;
  }
  tEncoderClear(&encoder);

  SRpcMsg msg = {0};
  initRpcMsg(&msg, TDMT_MND_STREAM_REQ_CHKPT, buf, tlen);
  stDebug("s-task:%s vgId:%d build and send task checkpoint req", id, vgId);

  tmsgSendReq(&pTask->info.mnodeEpset, &msg);
  return 0;
}

static int32_t streamTaskInitTaskCheckInfo(STaskCheckInfo* pInfo, STaskOutputInfo* pOutputInfo, int64_t startTs) {
  if (pInfo->pList == NULL) {
    pInfo->pList = taosArrayInit(4, sizeof(SDownstreamStatusInfo));
  } else {
    taosArrayClear(pInfo->pList);
  }

  if (pOutputInfo->type == TASK_OUTPUT__FIXED_DISPATCH) {
    pInfo->notReadyTasks = 1;
  } else if (pOutputInfo->type == TASK_OUTPUT__SHUFFLE_DISPATCH) {
    pInfo->notReadyTasks = taosArrayGetSize(pOutputInfo->shuffleDispatcher.dbInfo.pVgroupInfos);
    ASSERT(pInfo->notReadyTasks == pOutputInfo->shuffleDispatcher.dbInfo.vgNum);
  }

  pInfo->startTs = startTs;
  return TSDB_CODE_SUCCESS;
}

int32_t streamTaskAddReqInfo(STaskCheckInfo* pInfo, int64_t reqId, int32_t taskId, const char* id) {
  SDownstreamStatusInfo info = {.taskId = taskId, .status = -1, .reqId = reqId, .rspTs = 0};

  taosThreadMutexLock(&pInfo->checkInfoLock);

  for(int32_t i = 0; i < taosArrayGetSize(pInfo->pList); ++i) {
    SDownstreamStatusInfo* p = taosArrayGet(pInfo->pList, i);
    if (p->taskId == taskId) {
      stDebug("s-task:%s check info to task:0x%x already sent", id, taskId);

      taosThreadMutexUnlock(&pInfo->checkInfoLock);
      return TSDB_CODE_SUCCESS;
    }
  }

  taosArrayPush(pInfo->pList, &info);

  taosThreadMutexUnlock(&pInfo->checkInfoLock);
  return TSDB_CODE_SUCCESS;
}

int32_t streamTaskUpdateCheckInfo(STaskCheckInfo* pInfo, int32_t taskId, int32_t status, int64_t rspTs, int64_t reqId,
                                  int32_t* pNotReady, const char* id) {
  taosThreadMutexLock(&pInfo->checkInfoLock);

  for(int32_t i = 0; i < taosArrayGetSize(pInfo->pList); ++i) {
    SDownstreamStatusInfo* p = taosArrayGet(pInfo->pList, i);
    if (p->taskId == taskId) {
      ASSERT(reqId == p->reqId);

      // count down one, since it is ready now
      if ((p->status != TASK_DOWNSTREAM_READY) && (status == TASK_DOWNSTREAM_READY)) {
        *pNotReady = atomic_sub_fetch_32(&pInfo->notReadyTasks, 1);
      } else {
        *pNotReady = pInfo->notReadyTasks;
      }

      p->status = status;
      p->rspTs = rspTs;

      taosThreadMutexUnlock(&pInfo->checkInfoLock);
      return TSDB_CODE_SUCCESS;
    }
  }

  taosThreadMutexUnlock(&pInfo->checkInfoLock);
  stError("s-task:%s unexpected check rsp msg, downstream task:0x%x, reqId:%"PRIx64, id, taskId, reqId);
  return TSDB_CODE_FAILED;
}

static int32_t streamTaskStartCheckDownstream(STaskCheckInfo* pInfo, const char* id) {
  if (pInfo->inCheckProcess == 0) {
    pInfo->inCheckProcess = 1;
  } else {
    ASSERT(pInfo->startTs > 0);
    stError("s-task:%s already in check procedure, checkTs:%"PRId64", start monitor check rsp failed", id, pInfo->startTs);
    return TSDB_CODE_FAILED;
  }

  stDebug("s-task:%s set the in-check-procedure flag", id);
  return 0;
}

static int32_t streamTaskCompleteCheckRsp(STaskCheckInfo* pInfo, const char* id) {
  if (!pInfo->inCheckProcess) {
    stWarn("s-task:%s already not in-check-procedure", id);
  }

  int64_t el = taosGetTimestampMs() - pInfo->startTs;
  stDebug("s-task:%s clear the in-check-procedure flag, not in-check-procedure elapsed time:%" PRId64 " ms", id, el);

  pInfo->startTs = 0;
  pInfo->notReadyTasks = 0;
  pInfo->inCheckProcess = 0;
  pInfo->stopCheckProcess = 0;
  taosArrayClear(pInfo->pList);

  return 0;
}

static void doSendCheckMsg(SStreamTask* pTask, SDownstreamStatusInfo* p) {
  SStreamTaskCheckReq req = {
      .streamId = pTask->id.streamId,
      .upstreamTaskId = pTask->id.taskId,
      .upstreamNodeId = pTask->info.nodeId,
      .childId = pTask->info.selfChildId,
      .stage = pTask->pMeta->stage,
  };

  STaskOutputInfo* pOutputInfo = &pTask->outputInfo;
  if (pOutputInfo->type == TASK_OUTPUT__FIXED_DISPATCH) {
    req.reqId = p->reqId;
    req.downstreamNodeId = pOutputInfo->fixedDispatcher.nodeId;
    req.downstreamTaskId = pOutputInfo->fixedDispatcher.taskId;
    stDebug("s-task:%s (vgId:%d) stage:%" PRId64 " re-send check downstream task:0x%x(vgId:%d) req:0x%" PRIx64,
            pTask->id.idStr, pTask->info.nodeId, req.stage, req.downstreamTaskId, req.downstreamNodeId, req.reqId);

    streamSendCheckMsg(pTask, &req, pOutputInfo->fixedDispatcher.nodeId, &pOutputInfo->fixedDispatcher.epSet);
  } else if (pOutputInfo->type == TASK_OUTPUT__SHUFFLE_DISPATCH) {
    SArray* vgInfo = pOutputInfo->shuffleDispatcher.dbInfo.pVgroupInfos;
    int32_t numOfVgs = taosArrayGetSize(vgInfo);

    for (int32_t i = 0; i < numOfVgs; i++) {
      SVgroupInfo* pVgInfo = taosArrayGet(vgInfo, i);

      if (p->taskId == pVgInfo->taskId) {
        req.reqId = p->reqId;
        req.downstreamNodeId = pVgInfo->vgId;
        req.downstreamTaskId = pVgInfo->taskId;

        stDebug("s-task:%s (vgId:%d) stage:%" PRId64 " re-send check downstream task:0x%x (vgId:%d) (shuffle), idx:%d",
                pTask->id.idStr, pTask->info.nodeId, req.stage, req.downstreamTaskId, req.downstreamNodeId, i);
        streamSendCheckMsg(pTask, &req, pVgInfo->vgId, &pVgInfo->epSet);
        break;
      }
    }
  } else {
    ASSERT(0);
  }
}

static void rspMonitorFn(void* param, void* tmrId) {
  SStreamTask*      pTask = param;
  SStreamTaskState* pStat = streamTaskGetStatus(pTask);
  STaskCheckInfo*   pInfo = &pTask->taskCheckInfo;
  int32_t           vgId = pTask->pMeta->vgId;
  int64_t           now = taosGetTimestampMs();
  int64_t           el = now - pInfo->startTs;
  ETaskStatus       state = pStat->state;
  int32_t           numOfReady = 0;
  int32_t           numOfFault = 0;
  const char*       id = pTask->id.idStr;

  stDebug("s-task:%s start to do check downstream rsp check", id);

  if (state == TASK_STATUS__STOP) {
    int32_t ref = atomic_sub_fetch_32(&pTask->status.timerActive, 1);
    stDebug("s-task:%s status:%s vgId:%d quit from monitor check-rsp tmr, ref:%d", id, pStat->name, vgId, ref);

    taosThreadMutexLock(&pInfo->checkInfoLock);
    streamTaskCompleteCheckRsp(pInfo, id);
    taosThreadMutexUnlock(&pInfo->checkInfoLock);

    streamMetaAddTaskLaunchResult(pTask->pMeta, pTask->id.streamId, pTask->id.taskId, pInfo->startTs, now, false);
    return;
  }

  if (state == TASK_STATUS__DROPPING || state == TASK_STATUS__READY) {
    int32_t ref = atomic_sub_fetch_32(&pTask->status.timerActive, 1);
    stDebug("s-task:%s status:%s vgId:%d quit from monitor check-rsp tmr, ref:%d", id, pStat->name, vgId, ref);

    taosThreadMutexLock(&pInfo->checkInfoLock);
    streamTaskCompleteCheckRsp(pInfo, id);
    taosThreadMutexUnlock(&pInfo->checkInfoLock);

    return;
  }

  taosThreadMutexLock(&pInfo->checkInfoLock);
  if (pInfo->notReadyTasks == 0) {
    int32_t ref = atomic_sub_fetch_32(&pTask->status.timerActive, 1);
    stDebug("s-task:%s status:%s vgId:%d all downstream ready, quit from monitor rsp tmr, ref:%d", id, pStat->name,
            vgId, ref);

    streamTaskCompleteCheckRsp(pInfo, id);
    taosThreadMutexUnlock(&pInfo->checkInfoLock);
    return;
  }

  SArray* pNotReadyList = taosArrayInit(4, sizeof(int64_t));
  SArray* pTimeoutList = taosArrayInit(4, sizeof(int64_t));

  if (pStat->state == TASK_STATUS__UNINIT) {
    for (int32_t i = 0; i < taosArrayGetSize(pInfo->pList); ++i) {
      SDownstreamStatusInfo* p = taosArrayGet(pInfo->pList, i);
      if (p->status == TASK_DOWNSTREAM_READY) {
        numOfReady += 1;
      } else if (p->status == TASK_UPSTREAM_NEW_STAGE || p->status == TASK_DOWNSTREAM_NOT_LEADER) {
        stDebug("s-task:%s recv status from downstream, task:0x%x, quit from check downstream tasks", id, p->taskId);
        numOfFault += 1;
      } else {                // TASK_DOWNSTREAM_NOT_READY
        if (p->rspTs == 0) {  // not response yet
          ASSERT(p->status == -1);
          if (el >= CHECK_NOT_RSP_DURATION) {  // not receive info for 10 sec.
            taosArrayPush(pTimeoutList, &p->taskId);
          } else {  // el < CHECK_NOT_RSP_DURATION
            // do nothing and continue waiting for their rsps
          }
        } else {
          taosArrayPush(pNotReadyList, &p->taskId);
        }
      }
    }
  } else {  // unexpected status
    stError("s-task:%s unexpected task status:%s during waiting for check rsp", id, pStat->name);
  }

  int32_t numOfNotReady = (int32_t)taosArrayGetSize(pNotReadyList);
  int32_t numOfTimeout = (int32_t)taosArrayGetSize(pTimeoutList);

  // fault tasks detected, not try anymore
  if (((numOfReady + numOfFault + numOfNotReady + numOfTimeout) == taosArrayGetSize(pInfo->pList)) &&
      (numOfFault > 0)) {
    int32_t ref = atomic_sub_fetch_32(&pTask->status.timerActive, 1);
    stDebug(
        "s-task:%s status:%s vgId:%d all rsp. quit from monitor rsp tmr, since vnode-transfer/leader-change/restart "
        "detected, notReady:%d, fault:%d, timeout:%d, ready:%d ref:%d",
        id, pStat->name, vgId, numOfNotReady, numOfFault, numOfTimeout, numOfReady, ref);

    taosThreadMutexUnlock(&pInfo->checkInfoLock);
    taosArrayDestroy(pNotReadyList);
    taosArrayDestroy(pTimeoutList);

    streamTaskCompleteCheckRsp(pInfo, id);
    return;
  }

  // checking of downstream tasks has been stopped by other threads
  if (pInfo->stopCheckProcess == 1) {
    int32_t ref = atomic_sub_fetch_32(&pTask->status.timerActive, 1);
    stDebug(
        "s-task:%s status:%s vgId:%d stopped by other threads to check downstream process, notReady:%d, fault:%d, "
        "timeout:%d, ready:%d ref:%d",
        id, pStat->name, vgId, numOfNotReady, numOfFault, numOfTimeout, numOfReady, ref);

    streamTaskCompleteCheckRsp(pInfo, id);
    taosThreadMutexUnlock(&pInfo->checkInfoLock);

    // add the not-ready tasks into the final task status result buf, along with related fill-history task if exists.
    streamMetaAddTaskLaunchResult(pTask->pMeta, pTask->id.streamId, pTask->id.taskId, pInfo->startTs, now, false);
    if (HAS_RELATED_FILLHISTORY_TASK(pTask)) {
      STaskId* pHId = &pTask->hTaskInfo.id;
      streamMetaAddTaskLaunchResult(pTask->pMeta, pHId->streamId, pHId->taskId, pInfo->startTs, now, false);
    }
    return;
  }

  if (numOfNotReady > 0) {  // check to make sure not in recheck timer
    ASSERT(pTask->status.downstreamReady == 0);

    // reset the info, and send the check msg to failure downstream again
    for (int32_t i = 0; i < numOfNotReady; ++i) {
      int32_t taskId = *(int32_t*)taosArrayGet(pNotReadyList, i);

      for (int32_t j = 0; j < taosArrayGetSize(pInfo->pList); ++j) {
        SDownstreamStatusInfo* p = taosArrayGet(pInfo->pList, j);
        if (p->taskId == taskId) {
          p->rspTs = 0;
          p->status = -1;
          doSendCheckMsg(pTask, p);
        }
      }
    }

    stDebug("s-task:%s %d downstream task(s) not ready, send check msg again", id, numOfNotReady);
  }

  if (numOfTimeout > 0) {
    pInfo->startTs = now;
    ASSERT(pTask->status.downstreamReady == 0);

    for (int32_t i = 0; i < numOfTimeout; ++i) {
      int32_t taskId = *(int32_t*)taosArrayGet(pTimeoutList, i);

      for (int32_t j = 0; j < taosArrayGetSize(pInfo->pList); ++j) {
        SDownstreamStatusInfo* p = taosArrayGet(pInfo->pList, j);
        if (p->taskId == taskId) {
          ASSERT(p->status == -1 && p->rspTs == 0);
          doSendCheckMsg(pTask, p);
          break;
        }
      }
    }

    stDebug("s-task:%s %d downstream tasks timeout, send check msg again, start ts:%" PRId64, id, numOfTimeout, now);
  }

  taosTmrReset(rspMonitorFn, CHECK_RSP_INTERVAL, pTask, streamTimer, &pInfo->checkRspTmr);
  taosThreadMutexUnlock(&pInfo->checkInfoLock);

  stDebug("s-task:%s continue checking rsp in 300ms, notReady:%d, fault:%d, timeout:%d, ready:%d", id, numOfNotReady,
          numOfFault, numOfTimeout, numOfReady);

  taosArrayDestroy(pNotReadyList);
  taosArrayDestroy(pTimeoutList);
}

int32_t streamTaskStartMonitorCheckRsp(SStreamTask* pTask) {
  STaskCheckInfo* pInfo = &pTask->taskCheckInfo;

  taosThreadMutexLock(&pInfo->checkInfoLock);
  int32_t code = streamTaskStartCheckDownstream(pInfo, pTask->id.idStr);
  if (code != TSDB_CODE_SUCCESS) {

    taosThreadMutexUnlock(&pInfo->checkInfoLock);
    return TSDB_CODE_FAILED;
  }

  streamTaskInitTaskCheckInfo(pInfo, &pTask->outputInfo, taosGetTimestampMs());

  int32_t ref = atomic_add_fetch_32(&pTask->status.timerActive, 1);
  stDebug("s-task:%s start check rsp monit, ref:%d ", pTask->id.idStr, ref);

  if (pInfo->checkRspTmr == NULL) {
    pInfo->checkRspTmr = taosTmrStart(rspMonitorFn, CHECK_RSP_INTERVAL, pTask, streamTimer);
  } else {
    taosTmrReset(rspMonitorFn, CHECK_RSP_INTERVAL, pTask, streamTimer, pInfo->checkRspTmr);
  }

  taosThreadMutexUnlock(&pInfo->checkInfoLock);
  return 0;
}

int32_t streamTaskStopMonitorCheckRsp(STaskCheckInfo* pInfo, const char* id) {
  taosThreadMutexLock(&pInfo->checkInfoLock);
  streamTaskCompleteCheckRsp(pInfo, id);

  pInfo->stopCheckProcess = 1;
  taosThreadMutexUnlock(&pInfo->checkInfoLock);

  stDebug("s-task:%s set stop check rsp mon", id);
  return TSDB_CODE_SUCCESS;
}

void streamTaskCleanCheckInfo(STaskCheckInfo* pInfo) {
  ASSERT(pInfo->inCheckProcess == 0);

  pInfo->pList = taosArrayDestroy(pInfo->pList);
  if (pInfo->checkRspTmr != NULL) {
    /*bool ret = */ taosTmrStop(pInfo->checkRspTmr);
    pInfo->checkRspTmr = NULL;
  }

  taosThreadMutexDestroy(&pInfo->checkInfoLock);
}