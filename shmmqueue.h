#ifndef __SHM_QUEUE_H__
#define __SHM_QUEUE_H__

#include <iostream>
#include <string.h>
#include <cstdlib>
#include <stdio.h>
#include <sys/shm.h>
#include <cmath>
#include "shm_rwlock.h"


#define EXTRA_BYTE 8
#define CACHELINE_SIZE 64
//Modify the alignment rules to avoid false sharing
#define CACHELINE_ALIGN  __attribute__((aligned(CACHELINE_SIZE)))

#ifndef MIN
#define MIN(a, b) (a) < (b) ? (a) : (b)
#endif

//cache line 大小
#define  CACHE_LINE_SIZE 64
// memory barrier
#define __MEM_BARRIER  __asm__ __volatile__("mfence":::"memory")
// memory read barrier
#define __READ_BARRIER__  __asm__ __volatile__("lfence":::"memory")
// memory write barrier
#define __WRITE_BARRIER__  __asm__ __volatile__("sfence":::"memory")

namespace shmmqueue
{
// read and write type
enum class eQueueModel: unsigned char
{
    // one process read, one process write
    ONE_READ_ONE_WRITE,
    // one process read, multi process write
    ONE_READ_MUL_WRITE,
    // multi process read, one process write
    MUL_READ_ONE_WRITE,
    // multi process read, multi process write
    MUL_READ_MUL_WRITE,
};

// error code of create shm
enum class eQueueErrorCode: int
{
    // everything is fine
    QUEUE_OK = 0,
    // param error
    QUEUE_PARAM_ERROR = -1,
    // message queue has no space
    QUEUE_NO_SPACE = -2,
    // message queue has no message    
    QUEUE_NO_MESSAGE = -3,
    // message queue the message sequence error
    QUEUE_DATA_SEQUENCE_ERROR = -4,
};

// create shm type
enum class enShmModule: unsigned char
{
    // first time, shm need to be initialized
    SHM_INIT,
    // not first time, shm can't initialize
    SHM_RESUME,
};

// shm controller
class CACHELINE_ALIGN CMessageQueue
{
private:
    /**
     * @brief : (only) constructor
     * @param char* curr_addr : head ptr of shm
     * @param const eQueueModel& module : single/multi read & write type
     * @param key_t shm_key : a unique identifier for SHM
     * @param int shm_id : segment identifier
     * @param size_t siz : size of shm
     * @param const enShmModule &shm_model: create shm type
     * @return :
     */
    CMessageQueue(char *curr_addr, const eQueueModel& module, key_t shm_key,
                    int shm_id, size_t size, const enShmModule &shm_model)
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

        std::cout << "shm_model = " << (int)shm_model << std::endl;
        
        _data_ptr += sizeof(stMemTrunk);
        init_lock();
    }
public:
    ~CMessageQueue()
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
    /**
     * default (copy) construct is forbidden
     */
    CMessageQueue(const CMessageQueue &) = delete;
    CMessageQueue(CMessageQueue &&) = delete;
    CMessageQueue& operator=(const CMessageQueue &) = delete;

// interface of init & release
public:
    /**
     * @brief : create shm
     * @param key_t shm_key : a unique identifier for SHM
     * @param long shm_size : size of shm
     * @param enShmModule &shm_model : create shm type
     * @param int& shm_id : segment identifier
     * @return : head ptr of shm
     */
    static char* create_share_mem(key_t shm_key, long shm_size, enShmModule &shm_model, int& shm_id)
    {
        if (shm_key < 0) {
            std::cout << "[file:" << __FILE__
                << "][line:" << __LINE__
                << "] invalid shm_key[" << shm_key << "]"
                << std::endl;
            return NULL;
        }

        size_t tmp_shm_size = (size_t)shm_size;
        std::cout << "[file:" << __FILE__
            << "][line:" << __LINE__
            << "] try to shmget share memory of [" << tmp_shm_size << "] bytes"
            << std::endl;
        shm_id = shmget(shm_key, tmp_shm_size, IPC_CREAT | IPC_EXCL | 0666);
        if (shm_id < 0) {
            if (errno != EEXIST) {
                std::cout << "[file:" << __FILE__
                    << "][line:" << __LINE__
                    << "] shmget share memory failed, shm_key = [" << shm_key
                    << "] size = [" << tmp_shm_size
                    << "], error = [" << strerror(errno) << "]"
                    << std::endl;
                return NULL;
            }

            std::cout << "[file:" << __FILE__
                << "][line:" << __LINE__
                << "] shm exist, shm_key = [" << shm_key
                << "], now try to attach it"
                << std::endl;
            shm_id = shmget(shm_key, tmp_shm_size, IPC_CREAT | 0666);
            if (shm_id < 0) {
                std::cout << "[file:" << __FILE__
                    << "][line:" << __LINE__
                    << "] attach to share memory failed, shm_key = [" << shm_key
                    << "], shm_id = [" << shm_id
                    << "], size = [" << tmp_shm_size
                    << "], maybe size of shm changed, please check by cmd [ipcs -m]"
                    << ", if you want to release shm before, plese run cmd [ipcrm -m shmid] to release"
                    << std::endl;
                //先获取之前的shmId
                shm_id = shmget(shm_key, 0, 0666);
                return NULL;
            }
            std::cout << "[file:" << __FILE__
                << "][line:" << __LINE__
                << "] attach shm success, shm_key = [" << shm_key
                << "], shm_id = " << shm_id
                << "], size = " << tmp_shm_size
                << std::endl;
            shm_model = enShmModule::SHM_RESUME;
        } else {
            std::cout << "[file:" << __FILE__
                << "][line:" << __LINE__
                << "]  shmget shm success, shm_key = [" << shm_key
                << "], shm_id = " << shm_id
                << "], size = " << tmp_shm_size
                << std::endl;
            shm_model = enShmModule::SHM_INIT;
        }

        char* shm_addr = (char *)shmat(shm_id, NULL, 0);
        if ((void *) -1 == shm_addr) {
            std::cout << "[file:" << __FILE__
                << "][line:" << __LINE__
                << "]  shmat shm failed, shm_key = [" << shm_key
                << "], shm_id = [" << shm_id
                << "], size = [" << tmp_shm_size
                << "], error = [" << strerror(errno) << "]"
                << std::endl;
            return NULL;
        }

        return shm_addr;
    }
    /**
     * @brief : destroy shm
     * @param key_t shm_key : a unique identifier for SHM
     * @param long shm_size : size of shm
     * @param enShmModule &shm_model : create shm type
     * @param int& shm_id : segment identifier
     * @return : head ptr of shm
     */
    int destory_share_mem(const void* shm_addr,key_t shm_key)
    {
        std::cout << "[file:" << __FILE__
            << "][line:" << __LINE__
            << "] start to destory shm shm_key = [" << shm_key << "]"
            << std::endl;
        int shm_id = -1;
        if (shm_key < 0) {
            std::cout << "[file:" << __FILE__
                << "][line:" << __LINE__
                << "] invalid shm_key = [" << shm_key << "]"
                << std::endl;
            return -1;
        }

        shm_id = shmget(shm_key, 0, 0666);
        if (shm_id < 0) {
            std::cout << "[file:" << __FILE__
                << "][line:" << __LINE__
                << "] invalid shm_key = [" << shm_key
                << "], shm_id = [" << shm_id
                << "], error = [" << strerror(errno) << "]"
                << std::endl;
            return -1;
        }

        if (shmdt(shm_addr)) {
            std::cout << "[file:" << __FILE__
                << "][line:" << __LINE__
                << "] disconnect share memory failed, shm_key = [" << shm_key
                << "], shm_id = [" << shm_id
                << "], error = [" << strerror(errno)
                << "]" << std::endl;
        }
        std::cout << "[file:" << __FILE__
            << "][line:" << __LINE__
            << "] disconnect share memory succeed, shm_key = [" << shm_key
            << "], shm_id = [" << shm_id
            << "]" << std::endl;

        std::cout << "[file:" << __FILE__
            << "][line:" << __LINE__
            << "] now remove the exist share memory, shm_key = [" << shm_key
            << "], shm_id = [" << shm_id
            << "]" << std::endl;

        if (shmctl(shm_id, IPC_RMID, NULL)) {
            std::cout << "[file:" << __FILE__
                << "][line:" << __LINE__
                << "] remove share memory failed, shm_key = [" << shm_key
                << "], shm_id = [" << shm_id
                << "], error = [" << strerror(errno)
                << "]" << std::endl;
            return -1;
        }

        std::cout << "[file:" << __FILE__
            << "][line:" << __LINE__
            << "] remove share memory succeed, shm_key = [" << shm_key
            << "], shm_id = [" << shm_id
            << "], error = [" << strerror(errno)
            << "]" << std::endl;

        return 0;
    }
    /**
     * @brief : create shm manager instance
     * @param key_t shm_key : a unique identifier for SHM
     * @param size_t queue_size : size of shm requested, if queue_size != 2^n, will change queue_size to the nearest 2^n bigger than queue_size
     * @param eQueueModel queue_model : single/multi read & write type
     * @return : shm manager instance
     */
    static CMessageQueue *create_instance(key_t shm_key, size_t queue_size, eQueueModel queue_model)
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

// interface of read and write
public:
    /**
     * @brief : wirte message to shm
     * @param char* message : head ptr of the message needs to be wirtten to shm
     * @param size_t length : length of message
     * @return : 0 is returned on success, negtive number on error(see details: eQueueErrorCode)
     */
    int write_shm(char* message, size_t length)
    {
        if (!message || length <= 0) {
            return (int) eQueueErrorCode::QUEUE_PARAM_ERROR;
        }

        // judege whether or not need lock write
        CSafeShmWlock tm_lock(_write_lock, is_write_lock());

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

    /**
     * @brief : read message from shm, and delete from shm after success
     * @param std::string& out_res : message get from shm
     * @return : 0 or positive number is returned on success, which means the real length of the message
     */
    int read_shm(std::string& out_res)
    {
        // judege whether or not need lock write
        CSafeShmWlock tm_lock(_write_lock, is_read_lock());

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
            std::cout << "[file:" << __FILE__
                << "][line:" << __LINE__
                << "] invalid out_len[" << out_len
                << "] is larger than current data size[" << data_size << "]"
                << std::endl;
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
    /**
     * @brief : read first message from shm, but not delete from shm
     * @param std::string& out_res : message get from shm
     * @return : 0 or positive number is returned on success, which means the real length of the message
     */
    int read_msg_head(std::string& out_res)
    {
        // judege whether or not need lock read
        CSafeShmRlock tm_lock(_read_lock, is_read_lock());

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
            return (int)eQueueErrorCode::QUEUE_DATA_SEQUENCE_ERROR;
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
        if (out_len > (int)(data_size - sizeof(size_t)) || out_len < 0) {
            std::cout << "[file:" << __FILE__
                << "][line:" << __LINE__
                << "] invalid out_len[" << out_len
                << "] current data size[" << data_size - sizeof(size_t) << "]"
                << std::endl;
            return (int)eQueueErrorCode::QUEUE_DATA_SEQUENCE_ERROR;
        }

        // read data from shm according to out_len, also consider the case of data seperation
        unsigned int first_part_len = MIN(out_len, _mem_trunk->_size - tmp_begin_pos);
        out_res.assign(&_data_ptr[tmp_begin_pos], first_part_len);
        unsigned int second_part_len = out_len - first_part_len;
        if (second_part_len > 0) {
            out_res.insert(out_res.length(), _data_ptr, second_part_len);
        }

        // don't update begin pos as this interface
        return out_len;
    }
    /**
     * @brief : delete first message from shm
     * @param
     * @return : 0 or positive number is returned on success, which means the real length of the message
     */
    int del_msg_head()
    {
        // judege whether or not need lock write
        CSafeShmWlock tm_lock(_write_lock, is_write_lock());

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
            std::cout << "[file:" << __FILE__
                << "][line:" << __LINE__
                << "] invalid out_len[" << out_len
                << "] current data size[" << data_size - sizeof(size_t) << "]"
                << std::endl;
            _mem_trunk->_begin_pos = _mem_trunk->_end_pos;
            return (int) eQueueErrorCode::QUEUE_DATA_SEQUENCE_ERROR;
        }

        // update begin pos
        _mem_trunk->_begin_pos = (tmp_begin_pos + out_len) & (_mem_trunk->_size -1);
        return out_len;
    }
    /**
     * @brief : used for debug, print the head of shm(no lock, maybe not correct where exists multi read/write process)
     * @param
     * @return
     */
    void print_head()
    {
        std::cout << "shm head ptr = " << (void*)_mem_trunk
            << ", shmkey = " << _mem_trunk->_shm_key
            << ", shmid = " << _mem_trunk->_shm_id
            << ", total size = " << _mem_trunk->_size
            << ", begin pos = " << _mem_trunk->_begin_pos
            << ", end pos = " << _mem_trunk->_end_pos
            << ", queue module = " << static_cast<int>(_mem_trunk->_queue_model)
            << std::endl;
    }

// some inner interface
public:
    // get free size for wirte
    inline unsigned int get_free_size()
    {
        return get_queue_length() - get_data_size() - EXTRA_BYTE;
    }

    // get data size which has already written to shm
    inline unsigned int get_data_size()
    {
        if (_mem_trunk->_begin_pos == _mem_trunk->_end_pos) {
            // shm is empyt
            return 0;
        } else if (_mem_trunk->_begin_pos > _mem_trunk->_end_pos) {
            // data is seperately to the tail and the head
            return (unsigned int)(_mem_trunk->_end_pos + _mem_trunk->_size  - _mem_trunk->_begin_pos);
        } else {
            // data is around the center
            return _mem_trunk->_end_pos - _mem_trunk->_begin_pos;
        }
    }

    // get total size of shm
    inline unsigned int get_queue_length()
    {
        return (unsigned int)_mem_trunk->_size;
    }

    // init lock
    inline void init_lock()
    {
        if (is_read_lock()) {
            _read_lock = new CShmRWlock((key_t) (_mem_trunk->_shm_key + 1));
        }

        if (is_write_lock()) {
            _write_lock = new CShmRWlock((key_t) (_mem_trunk->_shm_key + 2));
        }
    }

    // whether locks are required when read data
    inline bool is_read_lock()
    {
        return _mem_trunk->_queue_model == eQueueModel::MUL_READ_MUL_WRITE ||
            _mem_trunk->_queue_model == eQueueModel::MUL_READ_ONE_WRITE;
    }

    // whether locks are required when write data
    inline bool is_write_lock()
    {
        return (_mem_trunk->_queue_model == eQueueModel::MUL_READ_MUL_WRITE ||
            _mem_trunk->_queue_model == eQueueModel::ONE_READ_MUL_WRITE);
    }
    // whether shm size is 2^n
    static bool is_power_of_2(size_t size)
    {
        if(size < 1) {
            return false;
        }
        return (size & (size -1)) == 0;
    }

    // get the nearest 2^n number
    static size_t round_up_pow_of_2(size_t size)
    {
        return 1UL << fls(size - 1);
    }

    static int fls(size_t size)
    {
        int position = 0;
        int i = 0;
        if (0 != size) {
            for (i = (size >> 1), position = 0; i != 0; ++position) {
                i >>= 1;
            }
        } else {
            position = -1;
        }

        return position + 1;
    }

private:
    struct CACHELINE_ALIGN stMemTrunk
    {
        /**
         * 1) use 'int' type to denote the begin/end pos, CPU can ensure atomic read/write operation of basic type 
              beside float/double/long, so A process will not read incomplete value writing by B process
           2) cache has no practical meaning, just to put value in the different CPU cache line, to prevent false sharing
         */

        // begin pos to read from shm
        volatile unsigned int _begin_pos;
        char __cache_padding1__[CACHE_LINE_SIZE];
        // end pos to read from shm
        // if _begin_pos == _end_pos, means no data saved in shm
        volatile unsigned int _end_pos;
        char __cache_padding2__[CACHE_LINE_SIZE];
        // unique identifier for SHM
        int _shm_key;
        char __cache_padding3__[CACHE_LINE_SIZE];
        
        // size of shm
        unsigned int _size;
        char __cache_padding4__[CACHE_LINE_SIZE];
        
        // segment identifier
        int _shm_id;
        char __cache_padding5__[CACHE_LINE_SIZE];
        
        // single/multi read & write type
        eQueueModel _queue_model;
    };
private:
    // shm header
    stMemTrunk* _mem_trunk;
    // read lock
    CShmRWlock* _read_lock;
    // write lock
    CShmRWlock* _write_lock;
    // data head ptr
    char* _data_ptr;
    // shm head ptr
    void * _shm_ptr;
};
}
#endif /* __SHM_QUEUE_H__ */
