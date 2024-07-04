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

#ifndef _TD_MEMPOOL_INT_H_
#define _TD_MEMPOOL_INT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "os.h"
#include "tlockfree.h"
#include "thash.h"

#define MP_CHUNK_CACHE_ALLOC_BATCH_SIZE 1000
#define MP_NSCHUNK_CACHE_ALLOC_BATCH_SIZE 500
#define MP_SESSION_CACHE_ALLOC_BATCH_SIZE 100

#define MP_MAX_KEEP_FREE_CHUNK_NUM  1000
#define MP_MAX_MALLOC_MEM_SIZE      0xFFFFFFFFFF

#define MP_RETIRE_THRESHOLD_PERCENT           (0.9)
#define MP_RETIRE_UNIT_PERCENT               (0.1)


// FLAGS AREA
#define MP_CHUNK_FLAG_IN_USE   (1 << 0)
#define MP_CHUNK_FLAG_NS_CHUNK (1 << 1)


// STAT FLAGS
#define MP_STAT_FLAG_LOG_ALL_MEM_STAT (1 << 0)
#define MP_STAT_FLAG_LOG_ALL_CHUNK_STAT (1 << 1)

#define MP_STAT_FLAG_LOG_ALL_FILE_STAT (1 << 2)
#define MP_STAT_FLAG_LOG_ALL_LINE_STAT (1 << 3)
#define MP_STAT_FLAG_LOG_ALL_SESSION_STAT (1 << 4)
#define MP_STAT_FLAG_LOG_ALL_NODE_STAT (1 << 5)
#define MP_STAT_FLAG_LOG_ALL_POOL_STAT (1 << 6)

#define MP_STAT_FLAG_LOG_SOME_FILE_STAT (1 << 7)
#define MP_STAT_FLAG_LOG_SOME_LINE_STAT (1 << 8)
#define MP_STAT_FLAG_LOG_SOME_SESSION_STAT (1 << 9)
#define MP_STAT_FLAG_LOG_SOME_NODE_STAT (1 << 10)
#define MP_STAT_FLAG_LOG_SOME_POOL_STAT (1 << 11)

#define MP_STAT_FLAG_LOG_ALL            (0xFFFFFFFFFFFFFFFF)


// STAT PROCESURE FLAGS
#define MP_STAT_PROC_FLAG_EXEC (1 << 0)
#define MP_STAT_PROC_FLAG_INPUT_ERR (1 << 1)
#define MP_STAT_PROC_FLAG_RES_SUCC (1 << 2)
#define MP_STAT_PROC_FLAG_RES_FAIL (1 << 3)

// CTRL FUNC FLAGS
#define MP_CTRL_FLAG_PRINT_STAT (1 << 0)


typedef enum EMPStatLogItem {
  E_MP_STAT_LOG_MEM_MALLOC = 1,
  E_MP_STAT_LOG_MEM_CALLOC,
  E_MP_STAT_LOG_MEM_REALLOC,
  E_MP_STAT_LOG_MEM_FREE,
  E_MP_STAT_LOG_MEM_STRDUP,
  E_MP_STAT_LOG_CHUNK_MALLOC,  
  E_MP_STAT_LOG_CHUNK_RECYCLE,  
  E_MP_STAT_LOG_CHUNK_REUSE,  
  E_MP_STAT_LOG_CHUNK_FREE,
} EMPStatLogItem;

// MEM HEADER FLAGS
#define MP_MEM_HEADER_FLAG_NS_CHUNK (1 << 0)

typedef struct SMPMemHeader {
  uint64_t flags:24;
  uint64_t size:40;
} SMPMemHeader;

typedef struct SMPMemTailer {

} SMPMemTailer;

typedef struct SMPListNode {
  void    *pNext;
} SMPListNode;

typedef struct SMPChunk {
  SMPListNode list;
  char    *pMemStart;
  int32_t  flags;
  /* KEEP ABOVE SAME WITH SMPNSChunk */

  uint32_t offset;
} SMPChunk;

typedef struct SMPNSChunk {
  SMPListNode list;
  char    *pMemStart;
  int32_t  flags;
  /* KEEP ABOVE SAME WITH SMPChunk */

  uint64_t offset;
  uint64_t memBytes;
} SMPNSChunk;


typedef struct SMPCacheGroup {
  int32_t        nodesNum;
  int32_t        idleOffset;
  void          *pNodes;
  void*          pNext;
} SMPCacheGroup;

typedef struct SMPStatInput {
  char*    file;
  int64_t  size;
  int64_t  origSize;
  int32_t  procFlags;
  int32_t  line;
} SMPStatInput;

typedef struct SMPStatItem {
  int64_t  inErr;
  int64_t  exec;
  int64_t  succ;
  int64_t  fail;
} SMPStatItem;

typedef struct SMPStatItemExt {
  int64_t  inErr;
  int64_t  exec;
  int64_t  succ;
  int64_t  fail;
  int64_t  origExec;
  int64_t  origSucc;
  int64_t  origFail;
} SMPStatItemExt;

typedef struct SMPMemoryStat {
  SMPStatItem    memMalloc;
  SMPStatItem    memCalloc;
  SMPStatItemExt memRealloc;
  SMPStatItem    strdup;
  SMPStatItem    memFree;

  SMPStatItem    chunkMalloc;
  SMPStatItem    chunkRecycle;
  SMPStatItem    chunkReUse;
  SMPStatItem    chunkFree;
} SMPMemoryStat;

typedef struct SMPStatDetail {
  SMPMemoryStat times;
  SMPMemoryStat bytes;
} SMPStatDetail;

typedef struct SMPCtrlInfo {
  int64_t  statFlags;
  int64_t  funcFlags;
} SMPCtrlInfo;

typedef struct SMPStatSession {
  int64_t initSucc;
  int64_t initFail;
  int64_t destroyNum;
} SMPStatSession;

typedef struct SMPStatInfo {
  SMPStatDetail  statDetail;
  SMPStatSession statSession;
  SHashObj*      sessStat;
  SHashObj*      nodeStat;
  SHashObj*      fileStat;
  SHashObj*      lineStat;
} SMPStatInfo;

typedef struct SMPSession {
  SMPListNode        list;

  int64_t            sessionId;
  SMPCollection*     pCollection;
  bool               needRetire;
  SMPCtrlInfo        ctrlInfo;

  int64_t            allocChunkNum;
  int64_t            allocChunkMemSize;
  int64_t            allocMemSize;
  int64_t            maxAllocMemSize;
  int64_t            reUseChunkNum;
  
  int32_t            srcChunkNum;
  SMPChunk          *srcChunkHead;
  SMPChunk          *srcChunkTail;

  int32_t            inUseChunkNum;
  SMPChunk          *inUseChunkHead;
  SMPChunk          *inUseChunkTail;

  SMPNSChunk        *inUseNSChunkHead;
  SMPNSChunk        *inUseNSChunkTail;

  SMPChunk          *reUseChunkHead;
  SMPChunk          *reUseChunkTail;

  SMPNSChunk        *reUseNSChunkHead;
  SMPNSChunk        *reUseNSChunkTail;

  SMPStatInfo        stat;
} SMPSession; 

typedef struct SMPCacheGroupInfo {
  int16_t            nodeSize;
  int64_t            allocNum;
  int32_t            groupNum;
  SMPCacheGroup     *pGrpHead;
  SMPCacheGroup     *pGrpTail;
  void              *pIdleList;
} SMPCacheGroupInfo;

typedef struct SMPCollection {
  int64_t            collectionId;
  int64_t            allocMemSize;
  int64_t            maxAllocMemSize;

  SMPStatInfo        stat;
} SMPCollection;

typedef struct SMemPool {
  char              *name;
  int16_t            slotId;
  SMemPoolCfg        cfg;
  int64_t            memRetireThreshold;
  int64_t            memRetireUnit;
  int32_t            maxChunkNum;
  SMPCtrlInfo        ctrlInfo;

  int16_t            maxDiscardSize;
  double             threadChunkReserveNum;
  int64_t            allocChunkNum;
  int64_t            allocChunkSize;
  int64_t            allocNSChunkNum;
  int64_t            allocNSChunkSize;
  int64_t            allocMemSize;
  int64_t            maxAllocMemSize;

  SMPCacheGroupInfo  chunkCache;
  SMPCacheGroupInfo  NSChunkCache;
  SMPCacheGroupInfo  sessionCache;
  
  int32_t            readyChunkNum;
  int32_t            readyChunkReserveNum;
  int32_t            readyChunkLowNum;
  int32_t            readyChunkGotNum;
  SRWLatch           readyChunkLock;
  SMPChunk          *readyChunkHead;
  SMPChunk          *readyChunkTail;

  int64_t              readyNSChunkNum;
  SMPChunk            *readyNSChunkHead;
  SMPChunk            *readyNSChunkTail;

  SMPStatInfo          stat;
} SMemPool;

typedef enum EMPMemStrategy {
  E_MP_STRATEGY_DIRECT = 1,
  E_MP_STRATEGY_CHUNK,
} EMPMemStrategy;

typedef struct SMemPoolMgmt {
  EMPMemStrategy strategy;
  SArray*        poolList;
  TdThreadMutex  poolMutex;
  TdThread       poolMgmtThread;
  int32_t        code;
} SMemPoolMgmt;

#define MP_GET_FLAG(st, f) ((st) & (f))
#define MP_SET_FLAG(st, f) (st) |= (f)
#define MP_CLR_FLAG(st, f) (st) &= (~f)

enum {
  MP_READ = 1,
  MP_WRITE,
};

#define MP_STAT_FORMAT "%s => \tinputError:%" PRId64 "\texec:%" PRId64 "\tsucc:%" PRId64 "\tfail:%" PRId64
#define MP_STAT_ORIG_FORMAT "%s => \tinputError:%" PRId64 "\texec:%" PRId64 "\tsucc:%" PRId64 "\tfail:%" PRId64 "\torigExec:%" PRId64 "\torigSucc:%" PRId64 "\torigFail:%" PRId64

#define MP_STAT_VALUE(_name, _item) _name, (_item).inErr, (_item).exec, (_item).succ, (_item).fail
#define MP_STAT_ORIG_VALUE(_name, _item) _name, (_item).inErr, (_item).exec, (_item).succ, (_item).fail, (_item).origExec, (_item).origSucc, (_item).origFail


#define MP_INIT_MEM_HEADER(_header, _size, _nsChunk)                      \
  do {                                                                    \
    (_header)->size = _size;                                              \
    if (_nsChunk) {                                                       \
      MP_SET_FLAG((_header)->flags, MP_MEM_HEADER_FLAG_NS_CHUNK);         \
    }                                                                     \
  } while (0)

#define MP_ADD_TO_CHUNK_LIST(_chunkHead, _chunkTail, _chunkNum, _chunk)       \
  do {                                                                        \
    if (NULL == _chunkHead) {                                                 \
      _chunkHead = _chunk;                                                    \
      _chunkTail = _chunk;                                                    \
    } else {                                                                  \
      (_chunkTail)->list.pNext = _chunk;                                      \
      (_chunkTail) = _chunk;                                                  \
    }                                                                         \
    (_chunkNum)++;                                                            \
  } while (0)

#define MP_LOCK(type, _lock)                                                                                \
  do {                                                                                                       \
    if (MP_READ == (type)) {                                                                                \
      ASSERTS(atomic_load_32((_lock)) >= 0, "invalid lock value before read lock");                          \
      uDebug("MP RLOCK%p:%d, %s:%d B", (_lock), atomic_load_32(_lock), __FILE__, __LINE__);         \
      taosRLockLatch(_lock);                                                                                 \
      uDebug("MP RLOCK%p:%d, %s:%d E", (_lock), atomic_load_32(_lock), __FILE__, __LINE__);         \
      ASSERTS(atomic_load_32((_lock)) > 0, "invalid lock value after read lock");                            \
    } else {                                                                                                 \
      ASSERTS(atomic_load_32((_lock)) >= 0, "invalid lock value before write lock");                         \
      uDebug("MP WLOCK%p:%d, %s:%d B", (_lock), atomic_load_32(_lock), __FILE__, __LINE__);         \
      taosWLockLatch(_lock);                                                                                 \
      uDebug("MP WLOCK%p:%d, %s:%d E", (_lock), atomic_load_32(_lock), __FILE__, __LINE__);         \
      ASSERTS(atomic_load_32((_lock)) == TD_RWLATCH_WRITE_FLAG_COPY, "invalid lock value after write lock"); \
    }                                                                                                        \
  } while (0)

#define MP_UNLOCK(type, _lock)                                                                                 \
  do {                                                                                                          \
    if (MP_READ == (type)) {                                                                                   \
      ASSERTS(atomic_load_32((_lock)) > 0, "invalid lock value before read unlock");                            \
      uDebug("MP RULOCK%p:%d, %s:%d B", (_lock), atomic_load_32(_lock), __FILE__, __LINE__);           \
      taosRUnLockLatch(_lock);                                                                                  \
      uDebug("MP RULOCK%p:%d, %s:%d E", (_lock), atomic_load_32(_lock), __FILE__, __LINE__);           \
      ASSERTS(atomic_load_32((_lock)) >= 0, "invalid lock value after read unlock");                            \
    } else {                                                                                                    \
      ASSERTS(atomic_load_32((_lock)) == TD_RWLATCH_WRITE_FLAG_COPY, "invalid lock value before write unlock"); \
      uDebug("MP WULOCK%p:%d, %s:%d B", (_lock), atomic_load_32(_lock), __FILE__, __LINE__);           \
      taosWUnLockLatch(_lock);                                                                                  \
      uDebug("MP WULOCK%p:%d, %s:%d E", (_lock), atomic_load_32(_lock), __FILE__, __LINE__);           \
      ASSERTS(atomic_load_32((_lock)) >= 0, "invalid lock value after write unlock");                           \
    }                                                                                                           \
  } while (0)


#define MP_ERR_RET(c)                 \
  do {                                \
    int32_t _code = c;                \
    if (_code != TSDB_CODE_SUCCESS) { \
      terrno = _code;                 \
      return _code;                   \
    }                                 \
  } while (0)
  
#define MP_RET(c)                     \
  do {                                \
    int32_t _code = c;                \
    if (_code != TSDB_CODE_SUCCESS) { \
      terrno = _code;                 \
    }                                 \
    return _code;                     \
  } while (0)
  
#define MP_ERR_JRET(c)               \
  do {                               \
    code = c;                        \
    if (code != TSDB_CODE_SUCCESS) { \
      terrno = code;                 \
      goto _return;                  \
    }                                \
  } while (0)



#ifdef __cplusplus
}
#endif

#endif /* _TD_MEMPOOL_INT_H_ */