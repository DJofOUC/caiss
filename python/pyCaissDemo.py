import json
from python.pyCaiss import *

# LIB_PATH = 'libCaiss.dylib'
# MODEL_PATH = 'bert_71290words_768dim.caiss'

LIB_PATH = r'/Users/chunel/Documents/code/cpp/caiss/python/libCaiss.dylib'
MODEL_PATH = r'/Users/chunel/Documents/code/cpp/models/bert_71290words_768dim.caiss'
MAX_THREAD_SIZE = 1
DIM = 768
WORD = 'water'
TOP_K = 5
SEARCH_TYPE = CAISS_SEARCH_QUERY


def demo():
    caiss = PyCaiss(LIB_PATH, MAX_THREAD_SIZE, CAISS_ALGO_HNSW, CAISS_MANAGE_SYNC)

    handle = c_void_p(0)
    ret = caiss.create_handle(handle)
    if 0 != ret:
        return

    ret = caiss.init(handle, CAISS_MODE_PROCESS, CAISS_DISTANCE_INNER, DIM, MODEL_PATH)
    if 0 != ret:
        return

    vec_list = []
    for i in range(0, DIM):
        vec_list.append(i)

    ret, result = caiss.sync_search(handle, vec_list, SEARCH_TYPE, TOP_K, 0)
    print(result)

    caiss.destroy(handle)
    return

if __name__ == '__main__':
    demo()
