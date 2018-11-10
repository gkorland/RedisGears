#include "execution_plan.h"
#include "utils/arr_rm_alloc.h"
#include "mgmt.h"
#include "record.h"
#include "cluster.h"
#include <assert.h>
#include <stdbool.h>
#include "redistar.h"
#include "redistar_memory.h"
#include "utils/dict.h"
#include "utils/adlist.h"
#include "utils/buffer.h"
#include <pthread.h>
#include <unistd.h>

#define NEW_EP_MSG_TYPE 1
#define NEW_COLLECT_MSG_TYPE 2
#define DONE_COLLECT_MSG_TYPE 3
#define NEW_REPARTITION_MSG_TYPE 4
#define DONE_REPARTITION_MSG_TYPE 5

// this is an hack so redis will not crash, we should try to
// avoid this as soon as possible.
static void* modulePointer;

typedef struct ExecutionPlansData{
    list* executionPlansToRun;
    dict* epDict;
    dict* namesDict;
    pthread_mutex_t mutex;
    pthread_t* workers;
}ExecutionPlansData;

ExecutionPlansData epData;

static long long lastId = 0;

static Record* ExecutionPlan_NextRecord(ExecutionPlan* ep, ExecutionStep* step, RedisModuleCtx* rctx, char** err);
static ExecutionPlan* ExecutionPlan_New(FlatExecutionPlan* fep);
static void FlatExecutionPlan_Free(FlatExecutionPlan* fep);
static FlatExecutionReader* FlatExecutionPlan_NewReader(char* reader, void* readerArg);
static FlatExecutionWriter* FlatExecutionPlan_NewWriter(char* writer, void* writerArg);
static void ExecutionPlan_AddToRunList(ExecutionPlan* ep);

static uint64_t idHashFunction(const void *key){
    return dictGenHashFunction(key, EXECUTION_PLAN_ID_LEN);
}

static int idKeyCompare(void *privdata, const void *key1, const void *key2){
    return memcmp(key1, key2, EXECUTION_PLAN_ID_LEN) == 0;
}

static void idKeyDestructor(void *privdata, void *key){
    RS_FREE(key);
}

static void* idKeyDup(void *privdata, const void *key){
	char* ret = RS_ALLOC(EXECUTION_PLAN_ID_LEN);
	memcpy(ret, key , EXECUTION_PLAN_ID_LEN);
    return ret;
}

dictType dictTypeHeapIds = {
        .hashFunction = idHashFunction,
        .keyDup = idKeyDup,
        .valDup = NULL,
        .keyCompare = idKeyCompare,
        .keyDestructor = idKeyDestructor,
        .valDestructor = NULL,
};

ExecutionPlan* ExecutionPlan_FindByName(const char* name){
	dictEntry *entry = dictFind(epData.namesDict, name);
	if(!entry){
		return NULL;
	}
	return dictGetVal(entry);
}

ExecutionPlan* ExecutionPlan_FindById(const char* id){
	dictEntry *entry = dictFind(epData.epDict, id);
	if(!entry){
		return NULL;
	}
	return dictGetVal(entry);
}

static void FlatExecutionPlan_SerializeReader(FlatExecutionReader* rfep, BufferWriter* bw){
    RediStar_BWWriteString(bw, rfep->reader);
    ArgType* type = ReadersMgmt_GetArgType(rfep->reader);
    if(type && type->serialize){
        // if we do not have a type or type do not have a serializer then we assume arg is NULL
        type->serialize(rfep->arg, bw);
    }
}

static void FlatExecutionPlan_SerializeWriter(FlatExecutionWriter* wfep, BufferWriter* bw){
    RediStar_BWWriteString(bw, wfep->writer);
    ArgType* type = WritersMgmt_GetArgType(wfep->writer);
    if(type && type->serialize){
        // if we do not have a type or type do not have a serializer then we assume arg is NULL
        type->serialize(wfep->arg, bw);
    }
}

static void FlatExecutionPlan_SerializeStep(FlatExecutionStep* step, BufferWriter* bw){
    RediStar_BWWriteLong(bw, step->type);
    RediStar_BWWriteString(bw, step->bStep.stepName);
    ArgType* type = NULL;
    switch(step->type){
    case MAP:
        type = MapsMgmt_GetArgType(step->bStep.stepName);
        break;
    case FILTER:
        type = FiltersMgmt_GetArgType(step->bStep.stepName);
        break;
    case EXTRACTKEY:
        type = ExtractorsMgmt_GetArgType(step->bStep.stepName);
        break;
    case REDUCE:
        type = ReducersMgmt_GetArgType(step->bStep.stepName);
        break;
    default:
        break;
    }
    if(type && type->serialize){
        type->serialize(step->bStep.arg, bw);
    }
}

static void FlatExecutionPlan_Serialize(FlatExecutionPlan* fep, BufferWriter* bw){
	RediStar_BWWriteString(bw, fep->name);
    RediStar_BWWriteBuffer(bw, fep->id, EXECUTION_PLAN_ID_LEN);
    FlatExecutionPlan_SerializeReader(fep->reader, bw);
    RediStar_BWWriteLong(bw, array_len(fep->steps));
    for(int i = 0 ; i < array_len(fep->steps) ; ++i){
        FlatExecutionStep* step = fep->steps + i;
        FlatExecutionPlan_SerializeStep(step, bw);
    }
    if(fep->writer){
    	RediStar_BWWriteLong(bw, 1); // writer exits
    	FlatExecutionPlan_SerializeWriter(fep->writer, bw);
    }else{
    	RediStar_BWWriteLong(bw, 0); // writer missing
    }
}

static FlatExecutionWriter* FlatExecutionPlan_DeserializeWriter(BufferReader* br){
    char* writerName = RediStar_BRReadString(br);
    void* arg = NULL;
    ArgType* type = ReadersMgmt_GetArgType(writerName);
    if(type && type->deserialize){
        arg = type->deserialize(br);
    }
    FlatExecutionWriter* writer = FlatExecutionPlan_NewWriter(writerName, arg);
    return writer;
}

static FlatExecutionReader* FlatExecutionPlan_DeserializeReader(BufferReader* br){
    char* readerName = RediStar_BRReadString(br);
    void* arg = NULL;
    ArgType* type = ReadersMgmt_GetArgType(readerName);
    if(type && type->deserialize){
        arg = type->deserialize(br);
    }
    FlatExecutionReader* reader = FlatExecutionPlan_NewReader(readerName, arg);
    return reader;
}

static FlatExecutionStep FlatExecutionPlan_DeserializeStep(BufferReader* br){
    FlatExecutionStep step;
    step.type = RediStar_BRReadLong(br);
    step.bStep.stepName = RS_STRDUP(RediStar_BRReadString(br));
    step.bStep.arg = NULL;
    ArgType* type = NULL;
    switch(step.type){
    case MAP:
        type = MapsMgmt_GetArgType(step.bStep.stepName);
        break;
    case FILTER:
        type = FiltersMgmt_GetArgType(step.bStep.stepName);
        break;
    case EXTRACTKEY:
        type = ExtractorsMgmt_GetArgType(step.bStep.stepName);
        break;
    case REDUCE:
        type = ReducersMgmt_GetArgType(step.bStep.stepName);
        break;
    default:
        break;
    }
    if(type && type->deserialize){
        step.bStep.arg = type->deserialize(br);
    }
    return step;
}

static FlatExecutionPlan* FlatExecutionPlan_Deserialize(BufferReader* br){
	char* name = RediStar_BRReadString(br);
    FlatExecutionPlan* ret = FlatExecutionPlan_New(name);
    size_t idLen;
    char* fepId = RediStar_BRReadBuffer(br, &idLen);
    assert(idLen == EXECUTION_PLAN_ID_LEN);
    memcpy(ret->id, fepId, EXECUTION_PLAN_ID_LEN);
    ret->reader = FlatExecutionPlan_DeserializeReader(br);
    long numberOfSteps = RediStar_BRReadLong(br);
    for(int i = 0 ; i < numberOfSteps ; ++i){
        ret->steps = array_append(ret->steps, FlatExecutionPlan_DeserializeStep(br));
    }
    if(RediStar_BRReadLong(br)){
    	// writer exists
    	ret->writer = FlatExecutionPlan_DeserializeWriter(br);
    }
    return ret;
}

static void ExecutionPlan_Distribute(ExecutionPlan* ep, RedisModuleCtx *rctx){
    Buffer* buff = Buffer_Create();
    BufferWriter bw;
    BufferWriter_Init(&bw, buff);
    FlatExecutionPlan_Serialize(ep->fep, &bw);
    size_t numOfNodes;
    RedisModule_ThreadSafeContextLock(rctx);
    char **nodesList = Cluster_GetNodesList(&numOfNodes);
    for(size_t i = 0 ; i < numOfNodes ; ++i){
        char *node = nodesList[i];
        RedisModule_SendClusterMessage(rctx, node, NEW_EP_MSG_TYPE, buff->buff, buff->size);
    }
    RedisModule_ThreadSafeContextUnlock(rctx);
    Buffer_Free(buff);
}

// not removing yet, we might use it in future
// todo: check if needed
//static void ExecutionPlan_WriteResults(ExecutionPlan* ep, RedisModuleCtx* rctx){
//    ep->writerStep.w->Start(rctx, ep->writerStep.w->ctx);
//    for(size_t i = 0 ; i < array_len(ep->results) ; ++i){
//        Record* r = ep->results[i];
//        ep->writerStep.w->Write(rctx, ep->writerStep.w->ctx, r);
//    }
//    ep->writerStep.w->Done(rctx, ep->writerStep.w->ctx);
//}

static Record* ExecutionPlan_FilterNextRecord(ExecutionPlan* ep, ExecutionStep* step, RedisModuleCtx* rctx, char** err){
    Record* record = NULL;
    while((record = ExecutionPlan_NextRecord(ep, step->prev, rctx, err))){
        if(record == &StopRecord){
            return record;
        }
        bool filterRes = step->filter.filter(rctx, record, step->filter.stepArg.stepArg, err);
        if(*err){
            RediStar_FreeRecord(record);
            return NULL;
        }
        if(filterRes){
            return record;
        }else{
            RediStar_FreeRecord(record);
        }
    }
    return NULL;
}

static Record* ExecutionPlan_MapNextRecord(ExecutionPlan* ep, ExecutionStep* step, RedisModuleCtx* rctx, char** err){
    Record* record = ExecutionPlan_NextRecord(ep, step->prev, rctx, err);
    if(record == NULL){
        return NULL;
    }
    if(record == &StopRecord){
        return record;
    }
    if(*err){
        RediStar_FreeRecord(record);
        return NULL;
    }
    if(record != NULL){
        record = step->map.map(rctx, record, step->map.stepArg.stepArg, err);
        if(*err){
            if(record){
                RediStar_FreeRecord(record);
            }
            return NULL;
        }
    }
    return record;
}

static Record* ExecutionPlan_ExtractKeyNextRecord(ExecutionPlan* ep, ExecutionStep* step, RedisModuleCtx* rctx, char** err){
    size_t buffLen;
    Record* record = ExecutionPlan_NextRecord(ep, step->prev, rctx, err);
    if(record == NULL){
        return NULL;
    }
    if(record == &StopRecord){
        return record;
    }
    char* buff = step->extractKey.extractor(rctx, record, step->extractKey.extractorArg.stepArg, &buffLen, err);
    if(*err){
        RediStar_FreeRecord(record);
        return NULL;
    }
    char* newBuff = RS_ALLOC(buffLen + 1);
    memcpy(newBuff, buff, buffLen);
    newBuff[buffLen] = '\0';
    Record* r = RediStar_KeyRecordCreate();
    RediStar_KeyRecordSetKey(r, newBuff, buffLen);
    RediStar_KeyRecordSetVal(r, record);
    return r;
}

static Record* ExecutionPlan_GroupNextRecord(ExecutionPlan* ep, ExecutionStep* step, RedisModuleCtx* rctx, char** err){
#define GROUP_RECORD_INIT_LEN 10
    dict* d = NULL;
    Record* record = NULL;
    if(step->group.groupedRecords == NULL){
        d = dictCreate(&dictTypeHeapStrings, NULL);
        while((record = ExecutionPlan_NextRecord(ep, step->prev, rctx, err))){
            if(record == &StopRecord){
                dictRelease(d);
                return record;
            }
            assert(RediStar_RecordGetType(record) == KEY_RECORD);
            size_t keyLen;
            char* key = RediStar_KeyRecordGetKey(record, &keyLen);
            if(*err){
                RediStar_FreeRecord(record);
                break;
            }
            dictEntry* entry = dictFind(d, key);
            Record* r = NULL;
            if(!entry){
                r = RediStar_KeyRecordCreate();
                RediStar_KeyRecordSetKey(r, key, keyLen);
                RediStar_KeyRecordSetKey(record, NULL, 0);
                Record* val  = RediStar_ListRecordCreate(GROUP_RECORD_INIT_LEN);
                RediStar_KeyRecordSetVal(r, val);
                dictAdd(d, key, r);
                if(step->group.groupedRecords == NULL){
                    step->group.groupedRecords = array_new(Record*, GROUP_RECORD_INIT_LEN);
                }
                step->group.groupedRecords = array_append(step->group.groupedRecords, r);
            }else{
                r = dictGetVal(entry);
            }
            Record* listRecord = RediStar_KeyRecordGetVal(r);
            RediStar_ListRecordAdd(listRecord, RediStar_KeyRecordGetVal(record));
            RediStar_KeyRecordSetVal(record, NULL);
            RediStar_FreeRecord(record);
        }
        dictRelease(d);
    }
    if(!step->group.groupedRecords || array_len(step->group.groupedRecords) == 0){
        return NULL;
    }
    return array_pop(step->group.groupedRecords);
}

static Record* ExecutionPlan_ReduceNextRecord(ExecutionPlan* ep, ExecutionStep* step, RedisModuleCtx* rctx, char** err){
    Record* record = ExecutionPlan_NextRecord(ep, step->prev, rctx, err);
    if(!record){
        return NULL;
    }
    if(record == &StopRecord){
        return record;
    }
    if(*err){
        if(record){
            RediStar_FreeRecord(record);
        }
        return NULL;
    }
    assert(RediStar_RecordGetType(record) == KEY_RECORD);
    size_t keyLen;
    char* key = RediStar_KeyRecordGetKey(record, &keyLen);
    Record* r = step->reduce.reducer(rctx, key, keyLen, RediStar_KeyRecordGetVal(record), step->reduce.reducerArg.stepArg, err);
    RediStar_KeyRecordSetVal(record, r);
    return record;
}

static Record* ExecutionPlan_RepartitionNextRecord(ExecutionPlan* ep, ExecutionStep* step, RedisModuleCtx* rctx, char** err){
    Buffer* buff;
    BufferWriter bw;
    Record* record;
    if(!Cluster_IsClusterMode()){
        return ExecutionPlan_NextRecord(ep, step->prev, rctx, err);
    }
    if(step->repartion.stoped){
        if(array_len(step->repartion.pendings) > 0){
            return array_pop(step->repartion.pendings);
        }
        return NULL;
    }
    buff = Buffer_Create();
    while((record = ExecutionPlan_NextRecord(ep, step->prev, rctx, err)) != NULL){
        if(record == &StopRecord){
            Buffer_Free(buff);
            return record;
        }
        if(*err){
            Buffer_Free(buff);
            return record;
        }
        size_t len;
        char* key = RediStar_KeyRecordGetKey(record, &len);
        char* shardIdToSendRecord = Cluster_GetNodeIdByKey(key);
        if(memcmp(shardIdToSendRecord, Cluster_GetMyId(), REDISMODULE_NODE_ID_LEN) == 0){
            // this record should stay with us, lets save it.
            RedisModule_ThreadSafeContextLock(rctx);
            step->repartion.pendings = array_append(step->repartion.pendings, record);
            RedisModule_ThreadSafeContextUnlock(rctx);
        }
        else{
            // we need to send the record to another shard
            BufferWriter_Init(&bw, buff);
            RediStar_BWWriteBuffer(&bw, ep->fep->id, EXECUTION_PLAN_ID_LEN); // serialize execution plan id
            RediStar_BWWriteLong(&bw, step->stepId); // serialize step id
            RS_SerializeRecord(&bw, record);
            RediStar_FreeRecord(record);

            RedisModule_ThreadSafeContextLock(rctx);
            RedisModule_SendClusterMessage(rctx, shardIdToSendRecord, NEW_REPARTITION_MSG_TYPE, buff->buff, buff->size);
            RedisModule_ThreadSafeContextUnlock(rctx);

            Buffer_Clear(buff);
        }
    }
    BufferWriter_Init(&bw, buff);
    RediStar_BWWriteBuffer(&bw, ep->fep->id, EXECUTION_PLAN_ID_LEN); // serialize execution plan id
    RediStar_BWWriteLong(&bw, step->stepId); // serialize step id

    RedisModule_ThreadSafeContextLock(rctx);
	RedisModule_SendClusterMessage(rctx, NULL, DONE_REPARTITION_MSG_TYPE, buff->buff, buff->size);
    RedisModule_ThreadSafeContextUnlock(rctx);

    Buffer_Free(buff);
    if(*err){
    	return record;
	}
    step->repartion.stoped = true;
    return &StopRecord;
}

static Record* ExecutionPlan_CollectNextRecord(ExecutionPlan* ep, ExecutionStep* step, RedisModuleCtx* rctx, char** err){
	Record* record = NULL;
	Buffer* buff;
	BufferWriter bw;

	if(!Cluster_IsClusterMode()){
		return ExecutionPlan_NextRecord(ep, step->prev, rctx, err);
	}

	if(step->collect.stoped){
		if(array_len(step->collect.pendings) > 0){
			return array_pop(step->collect.pendings);
		}
		return NULL;
	}

	buff = Buffer_Create();

	while((record = ExecutionPlan_NextRecord(ep, step->prev, rctx, err)) != NULL){
		if(record == &StopRecord){
			Buffer_Free(buff);
			return record;
		}
		if(*err){
			Buffer_Free(buff);
			return record;
		}
		if(Cluster_IsMyId(ep->fep->id)){
			RedisModule_ThreadSafeContextLock(rctx);
			step->collect.pendings = array_append(step->collect.pendings, record);
			RedisModule_ThreadSafeContextUnlock(rctx);
		}else{
			BufferWriter_Init(&bw, buff);
			RediStar_BWWriteBuffer(&bw, ep->fep->id, EXECUTION_PLAN_ID_LEN); // serialize execution plan id
			RediStar_BWWriteLong(&bw, step->stepId); // serialize step id
			RS_SerializeRecord(&bw, record);
			RediStar_FreeRecord(record);

			RedisModule_ThreadSafeContextLock(rctx);
			RedisModule_SendClusterMessage(rctx, ep->fep->id, NEW_COLLECT_MSG_TYPE, buff->buff, buff->size);
			RedisModule_ThreadSafeContextUnlock(rctx);

			Buffer_Clear(buff);
		}
	}

	step->collect.stoped = true;

	if(*err){
		Buffer_Free(buff);
		return record;
	}

	if(Cluster_IsMyId(ep->fep->id)){
		Buffer_Free(buff);
		return &StopRecord;
	}else{
		BufferWriter_Init(&bw, buff);
		RediStar_BWWriteBuffer(&bw, ep->fep->id, EXECUTION_PLAN_ID_LEN); // serialize execution plan id
		RediStar_BWWriteLong(&bw, step->stepId); // serialize step id

		RedisModule_ThreadSafeContextLock(rctx);
		RedisModule_SendClusterMessage(rctx, ep->fep->id, DONE_COLLECT_MSG_TYPE, buff->buff, buff->size);
		RedisModule_ThreadSafeContextUnlock(rctx);
		Buffer_Free(buff);
		return NULL;
	}
}

static Record* ExecutionPlan_NextRecord(ExecutionPlan* ep, ExecutionStep* step, RedisModuleCtx* rctx, char** err){
    Record* record = NULL;
    Record* r;
    dictEntry* entry;
    switch(step->type){
    case READER:
        return step->reader.r->Next(rctx, step->reader.r->ctx);
        break;
    case MAP:
        return ExecutionPlan_MapNextRecord(ep, step, rctx, err);
        break;
    case FILTER:
        return ExecutionPlan_FilterNextRecord(ep, step, rctx, err);
        break;
    case EXTRACTKEY:
        return ExecutionPlan_ExtractKeyNextRecord(ep, step, rctx, err);
        break;
    case GROUP:
        return ExecutionPlan_GroupNextRecord(ep, step, rctx, err);
        break;
    case REDUCE:
        return ExecutionPlan_ReduceNextRecord(ep, step, rctx, err);
    case REPARTITION:
        return ExecutionPlan_RepartitionNextRecord(ep, step, rctx, err);
    case COLLECT:
    	return ExecutionPlan_CollectNextRecord(ep, step, rctx, err);
    default:
        assert(false);
        return NULL;
    }
}

static void ExecutionPlan_WriteResult(ExecutionPlan* ep, RedisModuleCtx* rctx, Record* record){
	if(ep->writerStep.w){
		ep->writerStep.w->Write(rctx, ep->writerStep.w->ctx, record);
	}else{
		RedisModule_ThreadSafeContextLock(rctx);
		ep->results = array_append(ep->results, record);
		RedisModule_ThreadSafeContextUnlock(rctx);
	}
}

static bool ExecutionPlan_Execute(ExecutionPlan* ep, RedisModuleCtx* rctx){
    Record* record = NULL;
    char* err = NULL;

    if(ep->writerStep.w){
    	ep->writerStep.w->Start(rctx, ep->writerStep.w->ctx);
    }

    while((record = ExecutionPlan_NextRecord(ep, ep->steps[0], rctx, &err))){
        if(err){
            Record* r = RediStar_StringRecordCreate(err);
            ExecutionPlan_WriteResult(ep, rctx, r);
            break;
        }
        if(record == &StopRecord){
            // Execution need to be stopped, lets wait for a while.
            return false;
        }
        ExecutionPlan_WriteResult(ep, rctx, record);
    }
    if(err){
        Record* r = RediStar_StringRecordCreate(err);
        ExecutionPlan_WriteResult(ep, rctx, r);
    }

    if(ep->writerStep.w){
    	ep->writerStep.w->Done(rctx, ep->writerStep.w->ctx);
    }

    return true;
}

static void* ExecutionPlan_ThreadMain(void *arg){
    while(true){
        pthread_mutex_lock(&epData.mutex);
        listNode *node = listFirst(epData.executionPlansToRun);
        if(!node){
            pthread_mutex_unlock(&epData.mutex);
            usleep(10000);
            continue;
        }
        ExecutionPlan* ep = node->value;
        listDelNode(epData.executionPlansToRun, node);
        pthread_mutex_unlock(&epData.mutex);
        RedisModuleCtx* rctx = RedisModule_GetThreadSafeContext(NULL);
		((void**)rctx)[1] = modulePointer;
        if(ep->status == CREATED){
            if(Cluster_IsClusterMode()){
                if(memcmp(ep->fep->id, Cluster_GetMyId(), REDISMODULE_NODE_ID_LEN) == 0){
                   ExecutionPlan_Distribute(ep, rctx);
                }
            }
            ep->status = RUNNING;
        }
        bool isDone = ExecutionPlan_Execute(ep, rctx);
        if(isDone){
        	RedisModule_ThreadSafeContextLock(rctx);
        	ep->isDone = true;
        	if(ep->callback){
        		RediStarCtx starCtx = {
        				.fep = ep->fep,
						.ep = ep,
        		};
        		ep->callback(&starCtx, ep->privateData);
        	}
        	RedisModule_ThreadSafeContextUnlock(rctx);
        }
        RedisModule_FreeThreadSafeContext(rctx);
    }
}

static void ExecutionPlan_AddToRunList(ExecutionPlan* ep){
    pthread_mutex_lock(&epData.mutex);
    listAddNodeTail(epData.executionPlansToRun, ep);
    pthread_mutex_unlock(&epData.mutex);

}

static void ExecutionPlan_OnReceived(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Buffer buff = (Buffer){
        .buff = (char*)payload,
        .size = len,
        .cap = len,
    };
    BufferReader br;
    BufferReader_Init(&br, &buff);
    FlatExecutionPlan* fep = FlatExecutionPlan_Deserialize(&br);
    FlatExecutionPlan_Run(fep, NULL, NULL);
}

static void ExecutionPlan_CollectOnRecordReceived(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Buffer buff;
    buff.buff = (char*)payload;
    buff.size = len;
    buff.cap = len;
    BufferReader br;
    BufferReader_Init(&br, &buff);
    size_t epIdLen;
    char* epId = RediStar_BRReadBuffer(&br, &epIdLen);
    size_t stepId = RediStar_BRReadLong(&br);
    assert(epIdLen == EXECUTION_PLAN_ID_LEN);
    Record* r = RS_DeserializeRecord(&br);
    ExecutionPlan* ep = ExecutionPlan_FindById(epId);
    assert(ep);
    assert(ep->steps[stepId]->type == COLLECT);
	ep->steps[stepId]->collect.pendings = array_append(ep->steps[stepId]->collect.pendings, r);
}

static void ExecutionPlan_CollectDoneSendingRecords(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
	Buffer buff;
	buff.buff = (char*)payload;
	buff.size = len;
	buff.cap = len;
	BufferReader br;
	BufferReader_Init(&br, &buff);
	size_t epIdLen;
	char* epId = RediStar_BRReadBuffer(&br, &epIdLen);
	size_t stepId = RediStar_BRReadLong(&br);
	ExecutionPlan* ep = ExecutionPlan_FindById(epId);
	assert(ep);
	assert(ep->steps[stepId]->type == COLLECT);
	++ep->steps[stepId]->collect.totalShardsCompleted;
	assert(Cluster_GetSize() - 1 >= ep->steps[stepId]->collect.totalShardsCompleted);
	if((Cluster_GetSize() - 1) == ep->steps[stepId]->collect.totalShardsCompleted){ // no need to wait to myself
		ExecutionPlan_AddToRunList(ep);
	}
}

static void ExecutionPlan_OnRepartitionRecordReceived(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Buffer buff;
    buff.buff = (char*)payload;
    buff.size = len;
    buff.cap = len;
    BufferReader br;
    BufferReader_Init(&br, &buff);
    size_t epIdLen;
    char* epId = RediStar_BRReadBuffer(&br, &epIdLen);
    size_t stepId = RediStar_BRReadLong(&br);
    assert(epIdLen == EXECUTION_PLAN_ID_LEN);
    Record* r = RS_DeserializeRecord(&br);
    ExecutionPlan* ep = ExecutionPlan_FindById(epId);
    assert(ep);
    assert(ep->steps[stepId]->type == REPARTITION);
    ep->steps[stepId]->repartion.pendings = array_append(ep->steps[stepId]->repartion.pendings, r);
}

static void ExecutionPlan_DoneRepartition(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Buffer buff;
    buff.buff = (char*)payload;
    buff.size = len;
    buff.cap = len;
    BufferReader br;
    BufferReader_Init(&br, &buff);
    size_t epIdLen;
    char* epId = RediStar_BRReadBuffer(&br, &epIdLen);
    size_t stepId = RediStar_BRReadLong(&br);
    ExecutionPlan* ep = ExecutionPlan_FindById(epId);
	assert(ep);
    assert(ep->steps[stepId]->type == REPARTITION);
    ++ep->steps[stepId]->repartion.totalShardsCompleted;
    assert(Cluster_GetSize() - 1 >= ep->steps[stepId]->repartion.totalShardsCompleted);
    if((Cluster_GetSize() - 1) == ep->steps[stepId]->repartion.totalShardsCompleted){ // no need to wait to myself
        ExecutionPlan_AddToRunList(ep);
    }
}

void ExecutionPlan_Initialize(RedisModuleCtx *ctx, size_t numberOfworkers){
    modulePointer = ((void**)ctx)[1];
    epData.executionPlansToRun = listCreate();
    epData.epDict = dictCreate(&dictTypeHeapIds, NULL);
    epData.namesDict = dictCreate(&dictTypeHeapStrings, NULL);
    pthread_mutex_init(&epData.mutex, NULL);
    epData.workers = array_new(pthread_t, numberOfworkers);

    RedisModule_RegisterClusterMessageReceiver(ctx, NEW_EP_MSG_TYPE, ExecutionPlan_OnReceived);
    RedisModule_RegisterClusterMessageReceiver(ctx, NEW_COLLECT_MSG_TYPE, ExecutionPlan_CollectOnRecordReceived);
    RedisModule_RegisterClusterMessageReceiver(ctx, DONE_COLLECT_MSG_TYPE, ExecutionPlan_CollectDoneSendingRecords);
    RedisModule_RegisterClusterMessageReceiver(ctx, NEW_REPARTITION_MSG_TYPE, ExecutionPlan_OnRepartitionRecordReceived);
    RedisModule_RegisterClusterMessageReceiver(ctx, DONE_REPARTITION_MSG_TYPE, ExecutionPlan_DoneRepartition);

    for(size_t i = 0 ; i < numberOfworkers ; ++i){
        pthread_t thread;
        epData.workers = array_append(epData.workers, thread);
        pthread_create(epData.workers + i, NULL, ExecutionPlan_ThreadMain, NULL);
    }
}

ExecutionPlan* FlatExecutionPlan_Run(FlatExecutionPlan* fep, RediStar_OnExecutionDoneCallback callback, void* privateData){
    ExecutionPlan* ep = ExecutionPlan_New(fep);
    ep->callback = callback;
    ep->privateData = privateData;
    ExecutionPlan_AddToRunList(ep);
    return ep;
}

static WriterStep ExecutionPlan_NewWriter(FlatExecutionWriter* writer){
	if(!writer){
		return (WriterStep){.w = NULL, .type = NULL};
	}
    RediStar_WriterCallback callback = WritersMgmt_Get(writer->writer);
    ArgType* type = WritersMgmt_GetArgType(writer->writer);
    assert(callback); // todo: handle as error in future
    return (WriterStep){.w = callback(writer->arg), .type = type};
}

static ReaderStep ExecutionPlan_NewReader(FlatExecutionReader* reader){
    RediStar_ReaderCallback callback = ReadersMgmt_Get(reader->reader);
    ArgType* type = ReadersMgmt_GetArgType(reader->reader);
    assert(callback); // todo: handle as error in future
    return (ReaderStep){.r = callback(reader->arg), .type = type};
}

static ExecutionStep* ExecutionPlan_NewExecutionStep(FlatExecutionStep* step){
#define PENDING_INITIAL_SIZE 10
    ExecutionStep* es = RS_ALLOC(sizeof(*es));
    es->type = step->type;
    switch(step->type){
    case MAP:
        es->map.map = MapsMgmt_Get(step->bStep.stepName);
        es->map.stepArg.type = MapsMgmt_GetArgType(step->bStep.stepName);
        es->map.stepArg.stepArg = step->bStep.arg;
        break;
    case FILTER:
        es->filter.filter = FiltersMgmt_Get(step->bStep.stepName);
        es->filter.stepArg.type = FiltersMgmt_GetArgType(step->bStep.stepName);
        es->filter.stepArg.stepArg = step->bStep.arg;
        break;
    case EXTRACTKEY:
        es->extractKey.extractor = ExtractorsMgmt_Get(step->bStep.stepName);
        es->extractKey.extractorArg.type = ExtractorsMgmt_GetArgType(step->bStep.stepName);
        es->extractKey.extractorArg.stepArg = step->bStep.arg;
        break;
    case REDUCE:
        es->reduce.reducer = ReducersMgmt_Get(step->bStep.stepName);
        es->reduce.reducerArg.type = ReducersMgmt_GetArgType(step->bStep.stepName);
        es->reduce.reducerArg.stepArg = step->bStep.arg;
        break;
    case GROUP:
        es->group.groupedRecords = NULL;
        break;
    case REPARTITION:
        es->repartion.stoped = false;
        es->repartion.pendings = array_new(Record*, PENDING_INITIAL_SIZE);
        es->repartion.totalShardsCompleted = 0;
        break;
    case COLLECT:
    	es->collect.totalShardsCompleted = 0;
    	es->collect.stoped = false;
    	es->collect.pendings = array_new(Record*, PENDING_INITIAL_SIZE);
    	break;
    default:
        assert(false);
    }
    return es;
}

static ExecutionStep* ExecutionPlan_NewReaderExecutionStep(ReaderStep reader){
    ExecutionStep* es = RS_ALLOC(sizeof(*es));
    es->type = READER;
    es->reader = reader;
    es->prev = NULL;
    return es;
}

static ExecutionPlan* ExecutionPlan_New(FlatExecutionPlan* fep){
    ExecutionPlan* ret = RS_ALLOC(sizeof(*ret));
    ret->steps = array_new(FlatExecutionStep*, 10);
    ret->writerStep = ExecutionPlan_NewWriter(fep->writer);
    ExecutionStep* last = NULL;
    for(int i = array_len(fep->steps) - 1 ; i >= 0 ; --i){
        FlatExecutionStep* s = fep->steps + i;
        ExecutionStep* es = ExecutionPlan_NewExecutionStep(s);
        es->stepId = array_len(fep->steps) - 1 - i;
        if(array_len(ret->steps) > 0){
            ret->steps[array_len(ret->steps) - 1]->prev = es;
        }
        ret->steps = array_append(ret->steps, es);
    }
    ReaderStep rs = ExecutionPlan_NewReader(fep->reader);
    ExecutionStep* readerStep = ExecutionPlan_NewReaderExecutionStep(rs);
    if(array_len(ret->steps) > 0){
        ret->steps[array_len(ret->steps) - 1]->prev = readerStep;
    }
    ret->steps = array_append(ret->steps, readerStep);
    ret->fep = fep;
    ret->totalShardsCompleted = 0;
    ret->results = array_new(Record*, 100);
    ret->status = CREATED;
    ret->isDone = false;
    ret->callback = NULL;
    ret->privateData = NULL;
    ret->freeCallback = NULL;
    dictAdd(epData.epDict, fep->id, ret);
    dictAdd(epData.namesDict, fep->name, ret);
    return ret;
}

void ExecutionStep_Free(ExecutionStep* es, RedisModuleCtx *ctx){
    if(es->prev){
        ExecutionStep_Free(es->prev, ctx);
    }
    switch(es->type){
    case MAP:
        if (es->map.stepArg.type && es->map.stepArg.type->free){
            es->map.stepArg.type->free(es->map.stepArg.stepArg);
        }
        break;
    case FILTER:
        if (es->filter.stepArg.type && es->filter.stepArg.type->free){
            es->filter.stepArg.type->free(es->filter.stepArg.stepArg);
        }
        break;
    case EXTRACTKEY:
        if (es->extractKey.extractorArg.type && es->extractKey.extractorArg.type->free){
            es->extractKey.extractorArg.type->free(es->extractKey.extractorArg.stepArg);
        }
        break;
    case REDUCE:
        if(es->reduce.reducerArg.type && es->reduce.reducerArg.type->free){
            es->reduce.reducerArg.type->free(es->reduce.reducerArg.stepArg);
        }
        break;
    case REPARTITION:
    	if(es->repartion.pendings){
			for(size_t i = 0 ; i < array_len(es->repartion.pendings) ; ++i){
				Record* r = es->repartion.pendings[i];
				RediStar_FreeRecord(r);
			}
			array_free(es->repartion.pendings);
		}
		break;
    case COLLECT:
    	if(es->collect.pendings){
    		for(size_t i = 0 ; i < array_len(es->collect.pendings) ; ++i){
				Record* r = es->collect.pendings[i];
				RediStar_FreeRecord(r);
			}
			array_free(es->collect.pendings);
    	}
		break;
    case GROUP:
        if(es->group.groupedRecords){
            for(size_t i = 0 ; i < array_len(es->group.groupedRecords) ; ++i){
                Record* r = es->group.groupedRecords[i];
                RediStar_FreeRecord(r);
            }
            array_free(es->group.groupedRecords);
        }
        break;
    case READER:
        if(es->reader.type && es->reader.type->free){
            es->reader.type->free(es->reader.r->ctx);
        }
        RS_FREE(es->reader.r);
        break;
    default:
        assert(false);
    }
    RS_FREE(es);
}

void ExecutionPlan_Free(ExecutionPlan* ep, RedisModuleCtx *ctx){
    dictDelete(epData.epDict, ep->fep->id);
    dictDelete(epData.namesDict, ep->fep->name);
    FlatExecutionPlan_Free(ep->fep);

    if(ep->writerStep.w){
		if(ep->writerStep.type && ep->writerStep.type->free){
			ep->writerStep.type->free(ep->writerStep.w->ctx);
		}
		RS_FREE(ep->writerStep.w);
    }

    ExecutionStep_Free(ep->steps[0], ctx);
    array_free(ep->steps);

    array_free(ep->results);
    RS_FREE(ep);
}

static FlatExecutionReader* FlatExecutionPlan_NewReader(char* reader, void* readerArg){
    FlatExecutionReader* res = RS_ALLOC(sizeof(*res));
    res->reader = RS_STRDUP(reader);
    res->arg = readerArg;
    return res;
}

static FlatExecutionWriter* FlatExecutionPlan_NewWriter(char* writer, void* writerArg){
    FlatExecutionWriter* res = RS_ALLOC(sizeof(*res));
    res->writer = RS_STRDUP(writer);
    res->arg = writerArg;
    return res;
}

FlatExecutionPlan* FlatExecutionPlan_New(char* name){
#define STEPS_INITIAL_CAP 10
	char noneClusterId[REDISMODULE_NODE_ID_LEN] = {0};
    FlatExecutionPlan* res = RS_ALLOC(sizeof(*res));
    res->name = RS_STRDUP(name);
    res->reader = NULL;
    res->writer = NULL;
    res->steps = array_new(FlatExecutionStep, STEPS_INITIAL_CAP);
    memset(res->id, 0, EXECUTION_PLAN_ID_LEN);
    char* id;
    if(Cluster_IsClusterMode()){
    	id = Cluster_GetMyId();
    }else{
    	id = noneClusterId;
    }
	memcpy(res->id, id, REDISMODULE_NODE_ID_LEN);
	memcpy(res->id + REDISMODULE_NODE_ID_LEN, &lastId, sizeof(long long));
    ++lastId;
    return res;
}

static void FlatExecutionPlan_Free(FlatExecutionPlan* fep){
	RS_FREE(fep->name);
    RS_FREE(fep->reader->reader);
    RS_FREE(fep->reader);
    if(fep->writer){
    	RS_FREE(fep->writer->writer);
    	RS_FREE(fep->writer);
    }
    for(size_t i = 0 ; i < array_len(fep->steps) ; ++i){
        FlatExecutionStep* step = fep->steps + i;
        RS_FREE(step->bStep.stepName);
    }
    array_free(fep->steps);
    RS_FREE(fep);
}

void FlatExecutionPlan_SetReader(FlatExecutionPlan* fep, char* reader, void* readerArg){
    fep->reader = FlatExecutionPlan_NewReader(reader, readerArg);
}

void FlatExecutionPlan_SetWriter(FlatExecutionPlan* fep, char* writer, void* writerArg){
    fep->writer = FlatExecutionPlan_NewWriter(writer, writerArg);
}

static void FlatExecutionPlan_AddBasicStep(FlatExecutionPlan* fep, const char* callbackName, void* arg, enum StepType type){
    FlatExecutionStep s;
    s.type = type;
    s.bStep.arg = arg;
    if(callbackName){
        s.bStep.stepName = RS_STRDUP(callbackName);
    }else{
        s.bStep.stepName = NULL;
    }
    fep->steps = array_append(fep->steps, s);
}

void FlatExecutionPlan_AddMapStep(FlatExecutionPlan* fep, const char* callbackName, void* arg){
    FlatExecutionPlan_AddBasicStep(fep, callbackName, arg, MAP);
}

void FlatExecutionPlan_AddFilterStep(FlatExecutionPlan* fep, const char* callbackName, void* arg){
    FlatExecutionPlan_AddBasicStep(fep, callbackName, arg, FILTER);
}

void FlatExecutionPlan_AddGroupByStep(FlatExecutionPlan* fep, const char* extraxtorName, void* extractorArg,
                                  const char* reducerName, void* reducerArg){
    FlatExecutionStep extractKey;
    FlatExecutionPlan_AddBasicStep(fep, extraxtorName, extractorArg, EXTRACTKEY);
    FlatExecutionPlan_AddBasicStep(fep, "Repartition", NULL, REPARTITION);
    FlatExecutionPlan_AddBasicStep(fep, "Group", NULL, GROUP);
    FlatExecutionPlan_AddBasicStep(fep, reducerName, reducerArg, REDUCE);
}

void FlatExecutionPlan_AddCollectStep(FlatExecutionPlan* fep){
	FlatExecutionPlan_AddBasicStep(fep, "Collect", NULL, COLLECT);
}

int ExecutionPlan_ExecutionsDump(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
	dictIterator *iter = dictGetIterator(epData.epDict);
	dictEntry *entry = NULL;
	RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
	size_t numOfEntries = 0;
	while((entry = dictNext(iter)) != NULL){
		ExecutionPlan* ep = dictGetVal(entry);
		RedisModule_ReplyWithArray(ctx, 6);
		RedisModule_ReplyWithStringBuffer(ctx, "name", strlen("name"));
		RedisModule_ReplyWithStringBuffer(ctx, ep->fep->name, strlen(ep->fep->name));
		RedisModule_ReplyWithStringBuffer(ctx, "id", strlen("id"));
		RedisModule_ReplyWithStringBuffer(ctx, ep->fep->id, EXECUTION_PLAN_ID_LEN);
		RedisModule_ReplyWithStringBuffer(ctx, "status", strlen("status"));
		if(ep->isDone){
			RedisModule_ReplyWithStringBuffer(ctx, "done", strlen("done"));
		}else{
			RedisModule_ReplyWithStringBuffer(ctx, "running", strlen("running"));
		}

		++numOfEntries;
	}
	RedisModule_ReplySetArrayLength(ctx, numOfEntries);
	return REDISMODULE_OK;
}