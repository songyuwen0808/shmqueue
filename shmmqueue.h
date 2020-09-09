#ifndef __SHM_QUEUE_H__
#define __SHM_QUEUE_H__

#include <iostream>
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
     * @brief (only) constructor
     * @param char* curr_addr : head ptr of shm
     * @param const eQueueModel& module : single/multi read & write type
     * @param key_t shm_key : a unique identifier for SHM
     * @param int shm_id : segment identifier
     * @param size_t siz : size of shm
     * @param const enShmModule &shm_model: create shm type
     * @return :
     */
    CMessageQueue(char* curr_addr, const eQueueModel& module, key_t shm_key, int shm_id, size_t size, const enShmModule &shm_model);
public:
    ~CMessageQueue();
    /**
     * default (copy) construct is forbidden
     */
    CMessageQueue(const CMessageQueue &) = delete;
    CMessageQueue(CMessageQueue &&) = delete;
    CMessageQueue& operator=(const CMessageQueue &) = delete;

// interface of init & release
public:
    /**
     * @brief create shm
     * @param key_t shm_key : a unique identifier for SHM
     * @param long shm_size : size of shm
     * @param enShmModule &shm_model : create shm type
     * @param int& shm_id : segment identifier
     * @return : head ptr of shm
     */
    static char *create_share_mem(key_t shm_key, long shm_size, enShmModule &shm_model, int& shm_id);
    /**
     * @brief destroy shm
     * @param key_t shm_key : a unique identifier for SHM
     * @param long shm_size : size of shm
     * @param enShmModule &shm_model : create shm type
     * @param int& shm_id : segment identifier
     * @return : head ptr of shm
     */
    static int destory_share_mem(const void* shm_addr,key_t shm_key);
    /**
     * @brief create shm manager instance
     * @param key_t shm_key : a unique identifier for SHM
     * @param size_t queue_size : size of shm requested, if queue_size != 2^n, will change queue_size to the nearest 2^n bigger than queue_size
     * @param eQueueModel queue_model : single/multi read & write type
     * @return : shm manager instance
     */
    static CMessageQueue* create_instance(key_t shm_key, size_t queue_size, eQueueModel queue_model = eQueueModel::ONE_READ_ONE_WRITE);

// interface of read and write
public:
    /**
     * @brief wirte message to shm
     * @param char* message : head ptr of the message needs to be wirtten to shm
     * @param size_t length : length of message
     * @return : 0 is returned on success, negtive number on error(see details: eQueueErrorCode)
     */
    int write_shm(char *message, size_t length);
    /**
     * @brief read message from shm, and delete from shm after success
     * @param std::string& out_res : message get from shm
     * @return : 0 or positive number is returned on success, which means the real length of the message
     */
    int read_shm(std::string& out_res);
    /**
     * @brief read first message from shm, but not delete from shm
     * @param std::string& out_res : message get from shm
     * @return : 0 or positive number is returned on success, which means the real length of the message
     */
    int read_msg_head(std::string& out_res);
    /**
     * @brief delete first message from shm
     * @param
     * @return : 0 or positive number is returned on success, which means the real length of the message
     */
    int del_msg_head();
    /**
     * @brief used for debug, print the head of shm(no lock, maybe not correct where exists multi read/write process)
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
private:
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
