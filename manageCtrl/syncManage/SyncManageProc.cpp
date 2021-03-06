//
// Created by Chunel on 2020/5/24.
//


#include "SyncManageProc.h"


CAISS_RET_TYPE SyncManageProc::getResultSize(void *handle, unsigned int &size) {
    CAISS_FUNCTION_BEGIN

    AlgorithmProc *proc = this->getInstance(handle);
    CAISS_ASSERT_NOT_NULL(proc)

    ret = proc->getResultSize(size);
    CAISS_FUNCTION_CHECK_STATUS

    CAISS_FUNCTION_END
}


CAISS_RET_TYPE SyncManageProc::getResult(void *handle, char *result, const unsigned int size) {
    CAISS_FUNCTION_BEGIN

    AlgorithmProc *proc = this->getInstance(handle);
    CAISS_ASSERT_NOT_NULL(proc)

    ret = proc->getResult(result, size);
    CAISS_FUNCTION_CHECK_STATUS

    CAISS_FUNCTION_END
}


CAISS_RET_TYPE SyncManageProc::search(void *handle, void *info, CAISS_SEARCH_TYPE searchType, unsigned int topK,
        const unsigned int filterEditDistance, const CAISS_SEARCH_CALLBACK searchCBFunc, const void *cbParams) {
    CAISS_FUNCTION_BEGIN

    AlgorithmProc *proc = this->getInstance(handle);
    CAISS_ASSERT_NOT_NULL(proc)

    // 查询的时候，使用读锁即可；插入的时候，需要使用写锁
    this->lock_.readLock();    // 在同步的时候，这个lock实际上就锁住proc这个信息了
    ret = proc->search(info, searchType, topK, filterEditDistance, searchCBFunc, cbParams);
    this->lock_.readUnlock();

    CAISS_FUNCTION_CHECK_STATUS

    CAISS_FUNCTION_END
}


CAISS_RET_TYPE SyncManageProc::train(void *handle, const char *dataPath, const unsigned int maxDataSize, CAISS_BOOL normalize,
                      const unsigned int maxIndexSize, const float precision, const unsigned int fastRank,
                      const unsigned int realRank, const unsigned int step, const unsigned int maxEpoch,
                      const unsigned int showSpan) {
    CAISS_FUNCTION_BEGIN

    AlgorithmProc *proc = this->getInstance(handle);
    CAISS_ASSERT_NOT_NULL(proc)

    this->lock_.writeLock();    // 这里决定了，训练不支持多线程操作
    ret = proc->train(dataPath, maxDataSize, normalize, maxIndexSize, precision, fastRank, realRank, step, maxEpoch, showSpan);
    this->lock_.writeUnlock();

    CAISS_FUNCTION_CHECK_STATUS

    CAISS_FUNCTION_END
}


CAISS_RET_TYPE SyncManageProc::save(void *handle, const char *modelPath) {
    CAISS_FUNCTION_BEGIN

    AlgorithmProc *proc = this->getInstance(handle);
    CAISS_ASSERT_NOT_NULL(proc)

    this->lock_.writeLock();
    ret = proc->save(modelPath);
    this->lock_.writeUnlock();
    CAISS_FUNCTION_CHECK_STATUS

    CAISS_FUNCTION_END
}


CAISS_RET_TYPE SyncManageProc::insert(void *handle, CAISS_FLOAT *node, const char *label, CAISS_INSERT_TYPE insertType) {
    CAISS_FUNCTION_BEGIN

    /* 插入逻辑设计到写锁，还是使用同步的方式进行 */
    AlgorithmProc *proc = this->getInstance(handle);
    CAISS_ASSERT_NOT_NULL(proc)

    this->lock_.writeLock();
    ret = proc->insert(node, label, insertType);
    this->lock_.writeUnlock();
    CAISS_FUNCTION_CHECK_STATUS

    CAISS_FUNCTION_END
}

CAISS_RET_TYPE SyncManageProc::ignore(void *handle, const char *label, bool isIgnore) {
    CAISS_FUNCTION_BEGIN

    AlgorithmProc *proc = this->getInstance(handle);
    CAISS_ASSERT_NOT_NULL(proc)

    this->lock_.writeLock();    // 忽略label的功能，需要加写锁实现
    ret = proc->ignore(label, isIgnore);
    this->lock_.writeUnlock();
    CAISS_FUNCTION_CHECK_STATUS

    CAISS_FUNCTION_END
}


