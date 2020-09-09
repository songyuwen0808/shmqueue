#include <string.h>
#include <cstdlib>
#include <stdio.h>
#include <sys/shm.h>
#include <cmath>
#include "shmmqueue.h"

namespace shmmqueue
{
CMessageQueue::CMessageQueue(char *curr_addr, const eQueueModel& module, key_t shm_key, int shm_id, size_t size, const enShmModule &shm_model)
{
    _shm_ptr = (void*) curr_addr;
    _data_ptr = curr_addr;
    _mem_trunk = (stMemTrunk*)curr_addr;
    if (shm_model == enShmModule::SHM_INIT) {
        // if shm == SHM_RESUME, can not init following param
        // otherwise data already saved in shm will lost
        _mem_trunk->_begin_pos = 0;
        _mem_trunk->_end_pos = 0;
        _mem_trunk->_shm_key = shm_key;
        _mem_trunk->_shm_id = shm_id;
        _mem_trunk->_size = (unsigned int)size;
        _mem_trunk->_queue_model = module;
    } 
    
    _data_ptr += sizeof(stMemTrunk);
    init_lock();
}

CMessageQueue::~CMessageQueue()
{
    if(_mem_trunk) {
        destory_share_mem(_shm_ptr,_mem_trunk->_shm_key);
        _mem_trunk->~stMemTrunk();
    }

    if (_read_lock) {
        delete _read_lock;
        _read_lock = NULL;
    }

    if (_write_lock) {
        delete _write_lock;
        _write_lock = NULL;
    }
}

int CMessageQueue::write_shm(char* message, size_t length)
{
    if (!message || length <= 0) {
        return (int) eQueueErrorCode::QUEUE_PARAM_ERROR;
    }

    CSafeShmWlock tm_lock;
    // judege whether or not need lock write
    if (is_write_lock() && _write_lock) {
        tm_lock.init_lock(_write_lock);
    }

    // whether or not the shm have the enough space to put current data
    int free_size = get_free_size();
    if (free_size <= 0) {
        return (int)eQueueErrorCode::QUEUE_NO_SPACE;
    }

    // not enough space
    if ((length + sizeof(size_t)) > free_size) {
        return (int)eQueueErrorCode::QUEUE_NO_SPACE;
    }

    size_t in_len = length;
    char* tmp_addr = (char *)(&in_len);

    // write data length before real data, use this length when reading data for more accurate reading
    // as we may write data seperately to the tail and the head, write byte by byte
    unsigned int tmp_end_pos = _mem_trunk->_end_pos;
    for (size_t i = 0; i < sizeof(in_len); i++) {
        _data_ptr[tmp_end_pos] = tmp_addr[i];
        // prevent index overflow
        tmp_end_pos = (tmp_end_pos + 1) & (_mem_trunk->_size - 1);
    }

    // as we may write data seperately, calculate the first part of data size
    unsigned int first_part_len = MIN(in_len, _mem_trunk->_size - tmp_end_pos);
    memcpy((void *) (&_data_ptr[tmp_end_pos]), (const void *)message, (size_t) first_part_len);
    
    // if there are still some rest data, write from the beginning of the shm
    size_t second_part_len = length - first_part_len;
    if(second_part_len > 0) {
        /* then put the rest (if any) at the beginning of the shm */
        memcpy(&_data_ptr[0], message + first_part_len, second_part_len);
    }

    // make sure we have already write data into shm
    // otherwise, the read process may read incomplete data
    __WRITE_BARRIER__;

    // update end pos
    _mem_trunk->_end_pos = (tmp_end_pos + in_len) & (_mem_trunk->_size -1);
    return (int)eQueueErrorCode::QUEUE_OK;
}

int CMessageQueue::read_shm(std::string& out_res)
{
    CSafeShmWlock tm_lock;
    // judege whether or not need lock read
    if (is_read_lock() && _read_lock) {
        tm_lock.init_lock(_read_lock);
    }

    int data_size = get_data_size();
    if (data_size <= 0) {
        return (int) eQueueErrorCode::QUEUE_NO_MESSAGE;
    }

    // if current data (saved in shm) is smaller then sizeof(size_t), there are not invalid data to be read
    if (data_size <= (int)sizeof(size_t)) {
        std::cout << "[file:" << __FILE__
            << "][line:" << __LINE__
            << "] invalid data size[" << data_size
            << "] < " << sizeof(size_t)
            << std::endl;
        _mem_trunk->_begin_pos = _mem_trunk->_end_pos;
        return (int)eQueueErrorCode::QUEUE_DATA_SEQUENCE_ERROR;
    }

    // as we may write data seperately to the tail and the head, read out_len byte by byte
    size_t out_len = 0;
    char* tmp_addr = (char*)&out_len;
    unsigned int tmp_begin_pos = _mem_trunk->_begin_pos;
    for (size_t i = 0; i < sizeof(size_t); i++) {
        tmp_addr[i] = _data_ptr[tmp_begin_pos];
        tmp_begin_pos = (tmp_begin_pos + 1) & (_mem_trunk->_size -1);
    }

    // check whether or not the out_len is valid
    if (out_len > (int)(data_size - sizeof(size_t)) || out_len < 0) {
        printf("[%s:%d] read_msg_head usOutLength illegal,usOutLength: %d,data_size %d \n", __FILE__, __LINE__, out_len, data_size);
        _mem_trunk->_begin_pos = _mem_trunk->_end_pos;
        return (int)eQueueErrorCode::QUEUE_DATA_SEQUENCE_ERROR;
    }

    // read data from shm according to out_len, also consider the case of data seperation
    unsigned int first_part_len = MIN(out_len, _mem_trunk->_size - tmp_begin_pos);
    out_res.assign(&_data_ptr[tmp_begin_pos], first_part_len);
    unsigned int second_part_len = out_len - first_part_len;
    if(second_part_len > 0) {
        out_res.insert(out_res.length(), _data_ptr, second_part_len);
    }

    // update begin pos after ensure data read finish
    __WRITE_BARRIER__;
    _mem_trunk->_begin_pos = (tmp_begin_pos + out_len) & (_mem_trunk->_size -1);
    return out_len;
}

int CMessageQueue::read_msg_head(std::string& out_res)
{
    CSafeShmRlock tm_lock;
    // judege whether or not need lock read
    if (is_read_lock() && _read_lock) {
        tm_lock.init_lock(_read_lock);
    }

    int data_size = get_data_size();
    if (data_size <= 0) {
        return (int) eQueueErrorCode::QUEUE_NO_MESSAGE;
    }

    // if current data (saved in shm) is smaller then sizeof(size_t), there are not invalid data to be read
    if (data_size <= (int) sizeof(size_t)) {
        std::cout << "[file:" << __FILE__
            << "][line:" << __LINE__
            << "] invalid data size[" << data_size
            << "] < " << sizeof(size_t)
            << std::endl;
        return (int) eQueueErrorCode::QUEUE_DATA_SEQUENCE_ERROR;
    }

    // as we may write data seperately to the tail and the head, read out_len byte by byte
    size_t out_len = 0;
    char* tmp_addr = (char *) &out_len;
    unsigned int tmp_begin_pos = _mem_trunk->_begin_pos;
    for (size_t i = 0; i < sizeof(size_t); i++) {
        tmp_addr[i] = _data_ptr[tmp_begin_pos];
        tmp_begin_pos = (tmp_begin_pos + 1) & (_mem_trunk->_size -1);
    }

    // check whether or not the out_len is valid
    if (out_len > (int) (data_size - sizeof(size_t)) || out_len < 0) {
        printf("[%s:%d] read_msg_head usOutLength illegal,usOutLength: %d,data_size %d \n",
               __FILE__, __LINE__, out_len, data_size);
        print_head();
        return (int)eQueueErrorCode::QUEUE_DATA_SEQUENCE_ERROR;
    }

    // read data from shm according to out_len, also consider the case of data seperation
    unsigned int first_part_len = MIN(out_len, _mem_trunk->_size  - tmp_begin_pos);
    out_res.assign(&_data_ptr[tmp_begin_pos], first_part_len);
    unsigned int second_part_len = out_len - first_part_len;
    if(second_part_len > 0) {
        out_res.insert(out_res.length(), _data_ptr, second_part_len);
    }
    
    // don't update begin pos as this interface
    return out_len;
}

int CMessageQueue::del_msg_head()
{
    CSafeShmWlock tm_lock;
    // judege whether or not need lock read
    if (is_read_lock() && _read_lock) {
        tm_lock.init_lock(_read_lock);
    }

    int data_size = get_data_size();
    if (data_size <= 0) {
        return (int) eQueueErrorCode::QUEUE_NO_MESSAGE;
    }

    // if current data (saved in shm) is smaller then sizeof(size_t), there are not invalid data to be read
    if (data_size <= (int)sizeof(size_t)) {
        std::cout << "[file:" << __FILE__
            << "][line:" << __LINE__
            << "] invalid data size[" << data_size
            << "] < " << sizeof(size_t)
            << std::endl;
        _mem_trunk->_begin_pos = _mem_trunk->_end_pos;
        return (int) eQueueErrorCode::QUEUE_DATA_SEQUENCE_ERROR;
    }

    // as we may write data seperately to the tail and the head, read out_len byte by byte
    size_t out_len = 0;
    char* tmp_addr = (char*) &out_len;
    unsigned int tmp_begin_pos = _mem_trunk->_begin_pos;
    for (size_t i = 0; i < sizeof(size_t); i++) {
        tmp_addr[i] = _data_ptr[tmp_begin_pos];
        tmp_begin_pos = (tmp_begin_pos + 1)  & (_mem_trunk->_size -1);
    }

    // check whether or not the out_len is valid
    if (out_len > (int) (data_size - sizeof(size_t)) || out_len < 0) {
        printf("[%s:%d] read_msg_head usOutLength illegal,usOutLength: %d,data_size %d \n", __FILE__, __LINE__, out_len, data_size);
        print_head();
        _mem_trunk->_begin_pos = _mem_trunk->_end_pos;
        return (int) eQueueErrorCode::QUEUE_DATA_SEQUENCE_ERROR;
    }

    // update begin pos
    _mem_trunk->_begin_pos = (tmp_begin_pos + out_len) & (_mem_trunk->_size -1);
    return out_len;
}

char* CMessageQueue::create_share_mem(key_t shm_key, long shm_size, enShmModule &shm_model, int& shm_id)
{
    if (shm_key < 0) {
        printf("[%s:%d] create_share_mem failed. [key %d]errno:%s \n", __FILE__, __LINE__, shm_key,strerror(errno));
        exit(-1);
    }

    size_t tmp_shm_size = (size_t) shm_size;
    printf("Try to malloc share memory of %d bytes... \n", tmp_shm_size);
    shm_id = shmget(shm_key, tmp_shm_size, IPC_CREAT | IPC_EXCL | 0666);
    if (shm_id < 0) {
        if (errno != EEXIST) {
            printf("[%s:%d] Alloc share memory failed, [iKey:%d] , size:%d , error:%s \n",
                   __FILE__, __LINE__, shm_key, tmp_shm_size, strerror(errno));
            exit(-1);
        }

        printf("Same shm seg [key= %d] exist, now try to attach it... \n", shm_key);
        shm_id = shmget(shm_key, tmp_shm_size, IPC_CREAT | 0666);
        if (shm_id < 0) {
            printf("Attach to share memory [key= %d,shmId %d] failed,maybe the size of share memory changed,%s .now try to touch it again \n", shm_key, shm_id, strerror(errno));
            //先获取之前的shmId
            shm_id = shmget(shm_key, 0, 0666);
            if (shm_id < 0) {
                printf("[%s:%d] Fatel error, touch to shm [key= %d,shmId %d] failed, %s.\n", __FILE__, __LINE__, shm_key, shm_id,strerror(errno));
                exit(-1);
            } else {
                //先删除之前的share memory
                printf("First remove the exist share memory [key= %d,shmId %d] \n", shm_key,shm_id);
                if (shmctl(shm_id, IPC_RMID, NULL)) {
                    printf("[%s:%d] Remove share memory [key= %d,shmId %d] failed, %s \n", __FILE__, __LINE__, shm_key,shm_id,strerror(errno));
                    exit(-1);
                }
                //重新创建
                shm_id = shmget(shm_key, tmp_shm_size, IPC_CREAT | IPC_EXCL | 0666);
                if (shm_id < 0) {
                    printf("[%s:%d] Fatal error, alloc share memory [key= %d,shmId %d] failed, %s \n", __FILE__, __LINE__, shm_key,shm_id,strerror(errno));
                    exit(-1);
                }
            }
        } else {
            shm_model = enShmModule::SHM_RESUME;
            printf("Attach to share memory [key= %d,shmId %d] succeed.\n",shm_key,shm_id);
        }
    } else {
        shm_model = enShmModule::SHM_INIT;
    }

    printf("Successfully alloced share memory block,[key= %d,shmId %d] size = %d \n", shm_key, shm_id, tmp_shm_size);
    char* shm_addr = (char *) shmat(shm_id, NULL, 0);

    if ((void *) -1 == shm_addr) {
        printf("[%s:%d] create share memory failed, shmat failed, [key= %d,shmId %d], error = %s. \n", __FILE__, __LINE__,shm_key, shm_id, strerror(errno));
        exit(0);
    }

    return shm_addr;
}

int CMessageQueue::destory_share_mem(const void* shm_addr,key_t shm_key)
{
    int shm_id = -1;

    if (shm_key < 0) {
        printf("[%s:%d] Error in ftok, %s. \n", __FILE__, __LINE__, strerror(errno));
        return -1;
    }
    printf("Touch to share memory [key = %d]... \n", shm_key);
    shm_id = shmget(shm_key, 0, 0666);
    if (shm_id < 0) {
        printf("[%s:%d] Error, touch to shm [key= %d,shmId %d] failed, %s \n", __FILE__, __LINE__, shm_key, shm_id, strerror(errno));
        return -1;
    } else {
        printf("Now disconnect the exist share memory [key= %d,shmId %d] \n",  shm_key, shm_id);
        if(shmdt(shm_addr)){
            printf("[%s:%d] Disconnect share memory [key= %d,shmId %d] failed, %s \n", __FILE__, __LINE__,shm_key, shm_id,strerror(errno));
        } else{
            printf("Disconnect the exist share memory [key= %d,shmId %d] succeed \n", shm_key, shm_id);
        }
        printf("Now remove the exist share memory [key= %d,shmId %d] \n", shm_key, shm_id);
        if (shmctl(shm_id, IPC_RMID, NULL)) {
            printf("[%s:%d] Remove share memory [key= %d,shmId %d] failed, %s \n", __FILE__, __LINE__, shm_key, shm_id,strerror(errno));
            return -1;
        } else{
            printf("Remove shared memory [key= %d,shmId %d] succeed. \n", shm_id, shm_key);
        }
    }
    return 0;
}

CMessageQueue *CMessageQueue::create_instance(key_t shm_key, size_t queue_size, eQueueModel queue_model)
{
    if (queue_size <= 0) {
        return NULL;
    }

    queue_size = is_power_of_2(queue_size) ? queue_size : round_up_pow_of_2(queue_size);
    if (queue_size <= 0) {
        return NULL;
    }
    enShmModule shmModule;
    int shm_id = 0;
    char * shm_addr = CMessageQueue::create_share_mem(shm_key, queue_size + sizeof(stMemTrunk), shmModule, shm_id);
    CMessageQueue *messageQueue = new CMessageQueue(shm_addr, queue_model, shm_key,shm_id, queue_size, shmModule);
    messageQueue->print_head();
    return messageQueue;
}
}
