//
// Created by Chunel on 2020/5/23.
// hnsw算法的封装层，对外暴漏的算法使用接口
// 这里理论上是不能有加锁操作的，所有的锁在manage这一层保存
//

#include <fstream>
#include <algorithm>
#include <queue>
#include <iomanip>
#include "HnswProc.h"

using namespace std;

// 静态成员变量使用前，先初始化
HierarchicalNSW<CAISS_FLOAT>*  HnswProc::hnsw_alg_ptr_ = nullptr;
RWLock HnswProc::hnsw_lock_;

inline static bool isAnnSuffix(const char *modelPath) {
    string path = string(modelPath);
    bool ret = (path.find(MODEL_SUFFIX) == path.length() - string(MODEL_SUFFIX).length());
    return ret;
}


HnswProc::HnswProc() {
    this->distance_ptr_ = nullptr;
}


HnswProc::~HnswProc() {
    this->reset();
}


/************************ 以下是重写的算法基类接口内容 ************************/
CAISS_RET_TYPE
HnswProc::init(const CAISS_MODE mode, const CAISS_DISTANCE_TYPE distanceType, const unsigned int dim, const char *modelPath,
               const CAISS_DIST_FUNC distFunc = nullptr) {
    CAISS_FUNCTION_BEGIN
    CAISS_ASSERT_NOT_NULL(modelPath);
    if (distanceType == CAISS_DISTANCE_EDITION) {
        CAISS_ASSERT_NOT_NULL(distFunc)    // 如果是定制距离的话，必须传距离计算函数下来
    }

    reset();    // 清空所有数据信息

    this->dim_ = dim;
    this->cur_mode_ = mode;
    // 如果是train模式，则是需要保存到这里；如果process模式，则是读取模型
    this->model_path_ = isAnnSuffix(modelPath) ? (string(modelPath)) : (string(modelPath) + MODEL_SUFFIX);
    this->distance_type_ = distanceType;
    createDistancePtr(distFunc);

    if (this->cur_mode_ == CAISS_MODE_PROCESS) {
        ret = loadModel(modelPath);    // 如果是处理模式的话，则读取模型内容信息
        CAISS_FUNCTION_CHECK_STATUS
    }

    CAISS_FUNCTION_END
}


CAISS_RET_TYPE HnswProc::reset() {
    CAISS_FUNCTION_BEGIN

    CAISS_DELETE_PTR(distance_ptr_)
    this->dim_ = 0;
    this->cur_mode_ = CAISS_MODE_DEFAULT;
    this->normalize_ = 0;
    this->result_.clear();

    CAISS_FUNCTION_END
}


CAISS_RET_TYPE HnswProc::train(const char *dataPath, const unsigned int maxDataSize, const CAISS_BOOL normalize,
                               const unsigned int maxIndexSize, const float precision, const unsigned int fastRank,
                               const unsigned int realRank, const unsigned int step, const unsigned int maxEpoch,
                               const unsigned int showSpan) {
    CAISS_FUNCTION_BEGIN
    CAISS_ASSERT_NOT_NULL(dataPath)
    CAISS_ASSERT_NOT_NULL(this->distance_ptr_)
    CAISS_CHECK_MODE_ENABLE(CAISS_MODE_TRAIN)

    // 设定训练参数
    this->normalize_ = normalize;
    std::vector<CaissDataNode> datas;
    datas.reserve(maxDataSize);    // 提前分配好内存信息

    printf("[caiss] start load datas from [%s]. \n", dataPath);
    ret = loadDatas(dataPath, datas);
    CAISS_FUNCTION_CHECK_STATUS

    HnswProc::createHnswSingleton(this->distance_ptr_, maxDataSize, normalize, maxIndexSize);
    HnswTrainParams params(step);

    unsigned int epoch = 0;
    while (epoch < maxEpoch) {    // 如果批量走完了，则默认返回
        printf("[caiss] start to train caiss model for [%d] in [%d] epochs. \n", ++epoch, maxEpoch);
        ret = trainModel(datas, showSpan);
        CAISS_FUNCTION_CHECK_STATUS
        printf("[caiss] train caiss model finished, check model precision automatic, please wait for a moment... \n");

        float calcPrecision = 0.0f;
        ret = checkModelPrecisionEnable(precision, fastRank, realRank, datas, calcPrecision);
        if (CAISS_RET_OK == ret) {    // 如果训练的准确度符合要求，则直接退出
            printf("[caiss] train success, model is saved to path [%s] \n", this->model_path_.c_str());
            break;
        } else if (CAISS_RET_WARNING == ret) {
            float span = precision - calcPrecision;
            printf("[caiss] warning, the model's precision is not suitable, span = [%f], train again automatic. \n", span);
            params.update(span);
            destroyHnswSingleton();    // 销毁句柄信息，重新训练
            createHnswSingleton(this->distance_ptr_, maxDataSize, normalize, maxIndexSize, params.neighborNums, params.efSearch, params.efConstructor);
        }
    }

    CAISS_FUNCTION_CHECK_STATUS    // 如果是precision达不到要求，则返回警告信息
    CAISS_FUNCTION_END
}


CAISS_RET_TYPE HnswProc::search(void *info, const CAISS_SEARCH_TYPE searchType, const unsigned int topK) {
    CAISS_FUNCTION_BEGIN

    CAISS_ASSERT_NOT_NULL(info)
    CAISS_CHECK_MODE_ENABLE(CAISS_MODE_PROCESS)

    /* 将信息清空 */
    this->result_.clear();

    CAISS_BOOL isGet = CAISS_FALSE;
    if (CAISS_SEARCH_WORD == searchType || CAISS_LOOP_WORD == searchType) {
        ret = searchInLruCache((const char *) info, searchType, topK, isGet);    // 如果查询的是单词，则先进入cache中获取
        CAISS_FUNCTION_CHECK_STATUS
    }

    if (CAISS_FALSE == isGet) {    // 如果在cache中没找到，将之前查询的距离和单词都删除
        this->result_words_.clear();
        this->result_distance_.clear();
        ret = innerSearchResult(info, searchType, topK);
    }

    CAISS_FUNCTION_CHECK_STATUS

    this->last_topK_ = topK;    // 查询完毕之后，记录当前的topK信息
    this->last_search_type_ = searchType;
    if (searchType == CAISS_SEARCH_WORD || searchType == CAISS_LOOP_WORD) {
        this->lru_cache_.put(std::string((char *)info), this->result_);
    }

    CAISS_FUNCTION_END
}


CAISS_RET_TYPE HnswProc::insert(CAISS_FLOAT *node, const char *index, CAISS_INSERT_TYPE insertType) {
    CAISS_FUNCTION_BEGIN
    CAISS_ASSERT_NOT_NULL(node)
    CAISS_ASSERT_NOT_NULL(index)
    auto ptr = HnswProc::getHnswSingleton();
    CAISS_ASSERT_NOT_NULL(ptr)

    CAISS_CHECK_MODE_ENABLE(CAISS_MODE_PROCESS)

    unsigned int curCount = ptr->cur_element_count_;
    if (curCount >= ptr->max_elements_) {
        return CAISS_RET_MODEL_SIZE;    // 超过模型的最大尺寸了
    }

    std::vector<CAISS_FLOAT> vec;
    for (int i = 0; i < this->dim_; i++) {
        vec.push_back(node[i]);
    }

    ret = normalizeNode(vec, this->dim_);
    CAISS_FUNCTION_CHECK_STATUS

    switch (insertType) {
        case CAISS_INSERT_OVERWRITE:
            ret = insertByOverwrite(vec.data(), curCount, index);
            break;
        case CAISS_INSERT_DISCARD:
            ret = insertByDiscard(vec.data(), curCount, index);
            break;
        default:
            ret = CAISS_RET_PARAM;
            break;
    }

    CAISS_FUNCTION_CHECK_STATUS

    this->last_topK_ = 0;    // 如果插入成功，则重新记录topK信息
    CAISS_FUNCTION_END
}


CAISS_RET_TYPE HnswProc::save(const char *modelPath) {
    CAISS_FUNCTION_BEGIN
    auto ptr = HnswProc::getHnswSingleton();
    CAISS_ASSERT_NOT_NULL(ptr)

    std::string path;
    if (nullptr == modelPath) {
        path = this->model_path_;    // 如果传入的值为空，则保存当前的模型
    } else {
        path = isAnnSuffix(modelPath) ? string(modelPath) : (string(modelPath) + MODEL_SUFFIX);
    }

    remove(path.c_str());    // 如果有的话，就删除
    ptr->saveIndex(path);

    CAISS_FUNCTION_END
}


CAISS_RET_TYPE HnswProc::getResultSize(unsigned int &size) {
    CAISS_FUNCTION_BEGIN
    CAISS_CHECK_MODE_ENABLE(CAISS_MODE_PROCESS)

    size = this->result_.size();

    CAISS_FUNCTION_END
}


CAISS_RET_TYPE HnswProc::getResult(char *result, unsigned int size) {
    CAISS_FUNCTION_BEGIN
    CAISS_ASSERT_NOT_NULL(result)
    CAISS_CHECK_MODE_ENABLE(CAISS_MODE_PROCESS)

    memset(result, 0, size);
    memcpy(result, this->result_.data(), this->result_.size());

    CAISS_FUNCTION_END
}


CAISS_RET_TYPE HnswProc::ignore(const char *label) {
    CAISS_FUNCTION_BEGIN

    CAISS_ASSERT_NOT_NULL(label)
    // todo 逻辑待实现

    CAISS_FUNCTION_END
}


/************************ 以下是本Proc类内部函数 ************************/
/**
 * 读取文件中信息，并存至datas中
 * @param datas
 * @return
 */
CAISS_RET_TYPE HnswProc::loadDatas(const char *dataPath, vector<CaissDataNode> &datas) {
    CAISS_FUNCTION_BEGIN
    CAISS_ASSERT_NOT_NULL(dataPath);

    std::ifstream in(dataPath);
    if (!in) {
        return CAISS_RET_PATH;
    }

    std::string line;
    while (getline(in, line)) {
        if (0 == line.length()) {
            continue;    // 排除空格的情况
        }

        CaissDataNode dataNode;
        ret = RapidJsonProc::parseInputData(line.data(), dataNode);
        CAISS_FUNCTION_CHECK_STATUS

        ret = normalizeNode(dataNode.node, this->dim_);    // 在normalizeNode函数内部，判断是否需要归一化
        CAISS_FUNCTION_CHECK_STATUS

        datas.push_back(dataNode);
    }

    in.close();
    CAISS_FUNCTION_END
}


CAISS_RET_TYPE HnswProc::trainModel(std::vector<CaissDataNode> &datas, const unsigned int showSpan) {
    CAISS_FUNCTION_BEGIN
    auto ptr = HnswProc::getHnswSingleton();
    CAISS_ASSERT_NOT_NULL(ptr)

    unsigned int size = datas.size();
    for (unsigned int i = 0; i < size; i++) {
        ret = insertByOverwrite(datas[i].node.data(), i, (char *)datas[i].index.c_str());
        CAISS_FUNCTION_CHECK_STATUS

        if (showSpan != 0 && i % showSpan == 0) {
            printf("[caiss] train [%d] node, total size is [%d]. \n", i, (int)datas.size());
        }
    }

    remove(this->model_path_.c_str());
    ptr->saveIndex(std::string(this->model_path_));
    CAISS_FUNCTION_END
}


CAISS_RET_TYPE HnswProc::buildResult(const CAISS_FLOAT *query, const CAISS_SEARCH_TYPE searchType,
                                     std::priority_queue<std::pair<CAISS_FLOAT, labeltype>> &predResult) {
    CAISS_FUNCTION_BEGIN
    CAISS_ASSERT_NOT_NULL(query)
    auto ptr = HnswProc::getHnswSingleton();
    CAISS_ASSERT_NOT_NULL(ptr);

    std::list<CaissResultDetail> detailsList;
    while (!predResult.empty()) {
        CaissResultDetail detail;
        auto cur = predResult.top();
        detail.node = ptr->getDataByLabel<CAISS_FLOAT>(cur.second);
        detail.distance = cur.first;
        detail.index = cur.second;
        detail.label = ptr->index_lookup_.left.find(cur.second)->second;
        detailsList.push_front(detail);
        predResult.pop();

        this->result_words_.push_front(detail.label);    // 保存label（词语）信息
        this->result_distance_.push_front(detail.distance);    // 保存对应的距离信息
    }

    std::string type;
    if (CAISS_SEARCH_QUERY == searchType || CAISS_SEARCH_WORD == searchType) {
        type = "ann_search";
    } else {
        type = "force_loop";
    }

    ret = RapidJsonProc::buildSearchResult(detailsList, this->distance_type_, this->result_, type);
    CAISS_FUNCTION_CHECK_STATUS

    CAISS_FUNCTION_END
}


CAISS_RET_TYPE HnswProc::loadModel(const char *modelPath) {
    CAISS_FUNCTION_BEGIN

    CAISS_ASSERT_NOT_NULL(modelPath)
    CAISS_ASSERT_NOT_NULL(this->distance_ptr_)

    HnswProc::createHnswSingleton(this->distance_ptr_, this->model_path_);    // 读取模型的时候，使用的获取方式
    this->normalize_ = HnswProc::getHnswSingleton()->normalize_;    // 保存模型的时候，会写入是否被标准化的信息

    CAISS_FUNCTION_END
}


CAISS_RET_TYPE HnswProc::createDistancePtr(CAISS_DIST_FUNC distFunc) {
    CAISS_FUNCTION_BEGIN

    CAISS_DELETE_PTR(this->distance_ptr_)    // 先删除，确保不会出现重复new的情况
    switch (this->distance_type_) {
        case CAISS_DISTANCE_EUC :
            this->distance_ptr_ = new L2Space(this->dim_);
            break;
        case CAISS_DISTANCE_INNER:
            this->distance_ptr_ = new InnerProductSpace(this->dim_);
            break;
        case CAISS_DISTANCE_EDITION:
            this->distance_ptr_ = new EditionProductSpace(this->dim_);
            this->distance_ptr_->set_dist_func((DISTFUNC<float>)distFunc);
            break;
        default:
            break;
    }

    CAISS_FUNCTION_END
}


CAISS_RET_TYPE HnswProc::searchInLruCache(const char *word, const CAISS_SEARCH_TYPE searchType, const unsigned int topK,
                                          CAISS_BOOL &isGet) {
    CAISS_FUNCTION_BEGIN
    CAISS_ASSERT_NOT_NULL(word)

    if (topK == last_topK_ && searchType == last_search_type_) {    // 查询的还是上次的topK，并且查詢的方式还是一致的话
        std::string&& result = lru_cache_.get(std::string(word));
        if (!result.empty()) {
            isGet = CAISS_TRUE;    // 如果有值，直接给result赋值
            this->result_ = std::move(result);
            cout << "find word in cache - " << word << endl;
        } else {
            cout << "not find word in cache - " << word << endl;
            isGet = CAISS_FALSE;
        }
    } else {
        isGet = CAISS_FALSE;
        lru_cache_.clear();    // 如果topK有变动，或者有信息插入的话，清空缓存信息
    }

    CAISS_FUNCTION_END
}


/**
 * 训练模型的时候，使用的构建方式（static成员函数）
 * @param distance_ptr
 * @param maxDataSize
 * @param normalize
 * @return
 */
CAISS_RET_TYPE HnswProc::createHnswSingleton(SpaceInterface<CAISS_FLOAT>* distance_ptr, unsigned int maxDataSize, CAISS_BOOL normalize,
                                              const unsigned int maxIndexSize, const unsigned int maxNeighbor, const unsigned int efSearch, const unsigned int efConstruction) {
    CAISS_FUNCTION_BEGIN

    if (nullptr == HnswProc::hnsw_alg_ptr_) {
        HnswProc::hnsw_lock_.writeLock();
        if (nullptr == HnswProc::hnsw_alg_ptr_) {
            HnswProc::hnsw_alg_ptr_ = new HierarchicalNSW<CAISS_FLOAT>(distance_ptr, maxDataSize, normalize, maxIndexSize, maxNeighbor, efSearch, efConstruction);
        }
        HnswProc::hnsw_lock_.writeUnlock();
    }

    CAISS_FUNCTION_END
}

/**
 * 加载模型的时候，使用的构建方式（static成员函数）
 * @param distance_ptr
 * @param modelPath
 * @return
 */
CAISS_RET_TYPE HnswProc::createHnswSingleton(SpaceInterface<CAISS_FLOAT> *distance_ptr, const std::string &modelPath) {
    CAISS_FUNCTION_BEGIN

    if (nullptr == HnswProc::hnsw_alg_ptr_) {
        HnswProc::hnsw_lock_.writeLock();
        if (nullptr == HnswProc::hnsw_alg_ptr_) {
            // 这里是static函数信息，只能通过传递值下来的方式实现
            HnswProc::hnsw_alg_ptr_ = new HierarchicalNSW<CAISS_FLOAT>(distance_ptr, modelPath);
        }
        HnswProc::hnsw_lock_.writeUnlock();
    }

    CAISS_FUNCTION_END
}


CAISS_RET_TYPE HnswProc::destroyHnswSingleton() {
    CAISS_FUNCTION_BEGIN

    HnswProc::hnsw_lock_.writeLock();
    CAISS_DELETE_PTR(HnswProc::hnsw_alg_ptr_);
    HnswProc::hnsw_lock_.writeUnlock();

    CAISS_FUNCTION_END
}



HierarchicalNSW<CAISS_FLOAT> *HnswProc::getHnswSingleton() {
    return HnswProc::hnsw_alg_ptr_;
}


CAISS_RET_TYPE HnswProc::insertByOverwrite(CAISS_FLOAT *node, unsigned int label, const char *index) {
    CAISS_FUNCTION_BEGIN

    CAISS_ASSERT_NOT_NULL(node)    // 传入的信息，已经是normalize后的信息了
    CAISS_ASSERT_NOT_NULL(index)
    auto ptr = HnswProc::getHnswSingleton();
    CAISS_ASSERT_NOT_NULL(ptr);

    if (-1 == ptr->findWordLabel(index)) {
        // 返回-1，表示没找到对应的信息，如果不存在，则插入内容
        ret = ptr->addPoint(node, label, index);
    } else {
        // 如果被插入过了，则覆盖之前的内容，覆盖的时候，不需要考虑label的值，因为在里面，可以通过index获取
        ret = ptr->overwriteNode(node, index);
    }
    CAISS_FUNCTION_CHECK_STATUS

    CAISS_FUNCTION_END
}


CAISS_RET_TYPE HnswProc::insertByDiscard(CAISS_FLOAT *node, unsigned int label, const char *index) {
    CAISS_FUNCTION_BEGIN

    CAISS_ASSERT_NOT_NULL(node)
    CAISS_ASSERT_NOT_NULL(index)
    auto ptr = HnswProc::getHnswSingleton();
    CAISS_ASSERT_NOT_NULL(ptr)

    bool bret = ptr->findWordLabel(index);
    if (!bret) {
        // 如果不存在，则直接添加；如果存在，则不进入此逻辑，直接返回
        ret = ptr->addPoint(node, label, index);
        CAISS_FUNCTION_CHECK_STATUS
    }

    CAISS_FUNCTION_END
}


/**
 * 内部真实查询信息的时候，使用的函数。可以确保不用进入process状态，也可以查询
 * @param info
 * @param searchType
 * @param topK
 * @return
 */
CAISS_RET_TYPE HnswProc::innerSearchResult(void *info, CAISS_SEARCH_TYPE searchType, const unsigned int topK) {
    CAISS_FUNCTION_BEGIN

    CAISS_ASSERT_NOT_NULL(info)
    auto ptr = HnswProc::getHnswSingleton();
    CAISS_ASSERT_NOT_NULL(ptr)

    std::vector<CAISS_FLOAT> vec;
    vec.reserve(this->dim_);

    switch (searchType) {
        case CAISS_SEARCH_QUERY:
        case CAISS_LOOP_QUERY: {    // 如果传入的是query信息的话
            for (int i = 0; i < this->dim_; i++) {
                vec.push_back(*((CAISS_FLOAT *)info + i));
            }
            ret = normalizeNode(vec, this->dim_);    // 前面将信息转成query的形式
            break;
        }
        case CAISS_SEARCH_WORD:
        case CAISS_LOOP_WORD: {    // 过传入的是word信息的话
            int label = ptr->findWordLabel((const char *)info);
            if (-1 != label) {
                vec = ptr->getDataByLabel<CAISS_FLOAT>(label);    // 找到word的情况，这种情况下，不需要做normalize。因为存入的时候，已经设定好了
            } else {
                ret = CAISS_RET_NO_WORD;    // 没有找到word的情况
            }
            break;
        }
        default:
            ret = CAISS_RET_PARAM;
            break;
    }

    CAISS_FUNCTION_CHECK_STATUS

    auto *query = (CAISS_FLOAT *)vec.data();

    std::priority_queue<std::pair<CAISS_FLOAT, labeltype>> result;
    if (CAISS_SEARCH_QUERY == searchType || CAISS_SEARCH_WORD == searchType)  {
        result = ptr->searchKnn((void *)query, topK);
    } else {
        result = ptr->forceLoop((void *)query, topK);
    }

    ret = buildResult(query, searchType, result);
    CAISS_FUNCTION_CHECK_STATUS

    CAISS_FUNCTION_END;
}


CAISS_RET_TYPE HnswProc::checkModelPrecisionEnable(const float targetPrecision, const unsigned int fastRank, const unsigned int realRank,
                                                   const vector<CaissDataNode> &datas, float &calcPrecision) {
    CAISS_FUNCTION_BEGIN
    auto ptr = HnswProc::getHnswSingleton();
    CAISS_ASSERT_NOT_NULL(ptr)

    unsigned int suitableTimes = 0;
    unsigned int calcTimes = min((int)datas.size(), 1000);    // 最多1000次比较
    for (unsigned int i = 0; i < calcTimes; i++) {
        auto fastResult = ptr->searchKnn((void *)datas[i].node.data(), fastRank);    // 记住，fastResult是倒叙的
        auto realResult = ptr->forceLoop((void *)datas[i].node.data(), realRank);
        float fastFarDistance = fastResult.top().first;
        float realFarDistance = realResult.top().first;

        if (abs(fastFarDistance - realFarDistance) < 0.00001f) {    // 这里近似小于
            suitableTimes++;
        }
    }

    calcPrecision = (float)suitableTimes / (float)calcTimes;
    ret = (calcPrecision >= targetPrecision) ? CAISS_RET_OK : CAISS_RET_WARNING;
    CAISS_FUNCTION_CHECK_STATUS

    CAISS_FUNCTION_END
}


