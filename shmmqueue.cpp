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
        // _mem_trunk = new (_queue_addr) stMemTrunk();
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
    //修改共享内存写锁
    if (is_write_lock() && _write_lock) {
        tm_lock.init_lock(_write_lock);
    }

    // 首先判断是否队列已满
    int free_size = get_free_size();
    if (free_size <= 0) {
        return (int)eQueueErrorCode::QUEUE_NO_SPACE;
    }

    //空间不足
    if ((length + sizeof(size_t)) > free_size) {
        return (int)eQueueErrorCode::QUEUE_NO_SPACE;
    }

    size_t in_len = length;
    // BYTE *pTempDst = _queue_addr;
    char* tmp_addr = (char *) (&in_len);

    //写入的时候我们在数据头插上数据的长度,方便准确取数据,每次写入一个字节可能会分散在队列的头和尾
    unsigned int tmp_end_pos = _mem_trunk->_end_pos;
    for (size_t i = 0; i < sizeof(in_len); i++) {
        _data_ptr[tmp_end_pos] = tmp_addr[i];  // 拷贝 Code 的长度
        tmp_end_pos = (tmp_end_pos + 1) & (_mem_trunk->_size - 1);  // % 用于防止 Code 结尾的 idx 超出 codequeue
    }

    unsigned int first_part_len = MIN(in_len, _mem_trunk->_size - tmp_end_pos);
    memcpy((void *) (&_data_ptr[tmp_end_pos]), (const void *) message, (size_t) first_part_len);
    size_t second_part_len = length - first_part_len;
    if(second_part_len > 0)
    {
        /* then put the rest (if any) at the beginning of the buffer */
        memcpy(&_data_ptr[0], message + first_part_len, second_part_len);
    }

    /*
    * Ensure that we add the bytes to the kfifo -before-
    * we update the fifo->in index.
    * 数据写入完成修改m_iEnd，保证读端不会读到写入一半的数据
    */
    __WRITE_BARRIER__;
    _mem_trunk->_end_pos = (tmp_end_pos + in_len) & (_mem_trunk->_size -1);
    // std::cout << "SendMessage: " << m_stMemTrunk->m_iBegin << "-" << m_stMemTrunk->m_iEnd << " = " << message << std::endl;
    return (int)eQueueErrorCode::QUEUE_OK;
}

int CMessageQueue::read_shm(std::string& out_res)
{
    CSafeShmWlock tm_lock;
    //修改共享内存写锁
    if (is_read_lock() && _read_lock) {
        tm_lock.init_lock(_read_lock);
    }

    int data_size = get_data_size();
    if (data_size <= 0) {
        return (int) eQueueErrorCode::QUEUE_NO_MESSAGE;
    }

    // BYTE *pTempSrc = _queue_addr;
    // 如果数据的最大长度不到sizeof(MESS_SIZE_TYPE)（存入数据时在数据头插入了数据的长度,长度本身）
    if (data_size <= (int) sizeof(size_t)) {
        printf("[%s:%d] read_msg_head data len illegal,free_data_size %d \n", __FILE__, __LINE__, data_size);
        print_head();
        _mem_trunk->_begin_pos = _mem_trunk->_end_pos;
        return (int)eQueueErrorCode::QUEUE_DATA_SEQUENCE_ERROR;
    }

    size_t out_len = 0;
    char* tmp_addr = (char *) &out_len;   // 数据拷贝的目的地址
    unsigned int tmp_begin_pos = _mem_trunk->_begin_pos;
    //取出数据的长度
    for (size_t i = 0; i < sizeof(size_t); i++) {
        tmp_addr[i] = _data_ptr[tmp_begin_pos];
        tmp_begin_pos = (tmp_begin_pos + 1) & (_mem_trunk->_size -1);
    }

    // 将数据长度回传
    //取出的数据的长度实际有的数据长度，非法
    if (out_len > (int) (data_size - sizeof(size_t)) || out_len < 0) {
        printf("[%s:%d] read_msg_head usOutLength illegal,usOutLength: %d,free_data_size %d \n", __FILE__, __LINE__, out_len, data_size);
        print_head();
        _mem_trunk->_begin_pos = _mem_trunk->_end_pos;
        return (int)eQueueErrorCode::QUEUE_DATA_SEQUENCE_ERROR;
    }


    unsigned int first_part_len = MIN(out_len, _mem_trunk->_size  - tmp_begin_pos);
    out_res.assign(&_data_ptr[tmp_begin_pos], first_part_len);
    unsigned int second_part_len = out_len - first_part_len;
    if(second_part_len > 0) {
        out_res.insert(out_res.length(), _data_ptr, second_part_len);
    }

    __WRITE_BARRIER__;
    _mem_trunk->_begin_pos = (tmp_begin_pos + out_len) & (_mem_trunk->_size -1);
    return out_len;
}

/**
  *函数名          : PeekHeadCode
  *功能描述        : 查看共享内存管道（不改变读写索引）
  * Error code: -1 invalid para; -2 not enough; -3 data crashed
**/
int CMessageQueue::read_msg_head(std::string& out_res)
{
    CSafeShmRlock tm_lock;
    //修改共享内存写锁
    if (is_read_lock() && _read_lock) {
        tm_lock.init_lock(_read_lock);
    }

    int free_data_size = get_data_size();
    if (free_data_size <= 0) {
        return (int) eQueueErrorCode::QUEUE_NO_MESSAGE;
    }

    // BYTE *pTempSrc = _queue_addr;
    // 如果数据的最大长度不到sizeof(MESS_SIZE_TYPE)（存入数据时在数据头插入了数据的长度,长度本身）
    if (free_data_size <= (int) sizeof(size_t)) {
        printf("[%s:%d] read_msg_head data len illegal,free_data_size %d \n", __FILE__, __LINE__, free_data_size);
        print_head();
        return (int) eQueueErrorCode::QUEUE_DATA_SEQUENCE_ERROR;
    }

    size_t out_len = 0;
    char* tmp_addr = (char *) &out_len;   // 数据拷贝的目的地址
    unsigned int tmp_begin_pos = _mem_trunk->_begin_pos;
    //取出数据的长度
    for (size_t i = 0; i < sizeof(size_t); i++) {
        tmp_addr[i] = _data_ptr[tmp_begin_pos];
        tmp_begin_pos = (tmp_begin_pos + 1) & (_mem_trunk->_size -1);
    }

    // 将数据长度回传
    //取出的数据的长度实际有的数据长度，非法
    if (out_len > (int) (free_data_size - sizeof(size_t)) || out_len < 0) {
        printf("[%s:%d] read_msg_head usOutLength illegal,usOutLength: %d,free_data_size %d \n",
               __FILE__, __LINE__, out_len, free_data_size);
        print_head();
        return (int) eQueueErrorCode::QUEUE_DATA_SEQUENCE_ERROR;
    }

    // tmp_addr = &out_code[0];  // 设置接收 Code 的地址

    unsigned int tmp_idx = tmp_begin_pos & (_mem_trunk->_size - 1);
    unsigned int first_part_len = MIN(out_len, _mem_trunk->_size  - tmp_idx);
    out_res.assign(&_data_ptr[tmp_begin_pos], first_part_len);
    unsigned int second_part_len = out_len - first_part_len;
    if(second_part_len > 0) {
        out_res.insert(out_res.length(), _data_ptr, second_part_len);
    }
    return out_len;
}

/**
  *函数名          : GetOneCode
  *功能描述        : 从指定位置iCodeOffset获取指定长度nCodeLength数据
  * */
int CMessageQueue::del_msg_head()
{
    CSafeShmWlock tm_lock;
    //修改共享内存写锁
    if (is_read_lock() && _read_lock) {
        tm_lock.init_lock(_read_lock);
    }

    int free_data_size = get_data_size();
    if (free_data_size <= 0) {
        return (int) eQueueErrorCode::QUEUE_NO_MESSAGE;
    }

    // BYTE *pTempSrc = _queue_addr;
    // 如果数据的最大长度不到sizeof(MESS_SIZE_TYPE)（存入数据时在数据头插入了数据的长度,长度本身）
    if (free_data_size <= (int)sizeof(size_t)) {
        printf("[%s:%d] read_msg_head data len illegal,free_data_size %d \n", __FILE__, __LINE__, free_data_size);
        print_head();
        _mem_trunk->_begin_pos = _mem_trunk->_end_pos;
        return (int) eQueueErrorCode::QUEUE_DATA_SEQUENCE_ERROR;
    }

    size_t out_len = 0;
    char* tmp_addr = (char*) &out_len;   // 数据拷贝的目的地址
    unsigned int tmp_begin_pos = _mem_trunk->_begin_pos;
    //取出数据的长度
    for (size_t i = 0; i < sizeof(size_t); i++) {
        tmp_addr[i] = _data_ptr[tmp_begin_pos];
        tmp_begin_pos = (tmp_begin_pos + 1)  & (_mem_trunk->_size -1);
    }

    // 将数据长度回传
    //取出的数据的长度实际有的数据长度，非法
    if (out_len > (int) (free_data_size - sizeof(size_t)) || out_len < 0) {
        printf("[%s:%d] read_msg_head usOutLength illegal,usOutLength: %d,free_data_size %d \n", __FILE__, __LINE__, out_len, free_data_size);
        print_head();
        _mem_trunk->_begin_pos = _mem_trunk->_end_pos;
        return (int) eQueueErrorCode::QUEUE_DATA_SEQUENCE_ERROR;
    }

    _mem_trunk->_begin_pos = (tmp_begin_pos + out_len) & (_mem_trunk->_size -1);
    return out_len;
}

void CMessageQueue::print_head()
{
    printf("Mem trunk address 0x%p,shmkey %d ,shmid %d, size %d, begin %d, end %d, queue module %d \n",
           _mem_trunk,
           _mem_trunk->_shm_key,
           _mem_trunk->_shm_id,
           _mem_trunk->_size,
           _mem_trunk->_begin_pos,
           _mem_trunk->_end_pos,
           _mem_trunk->_queue_model);
}

//获取空闲区大小
unsigned int CMessageQueue::get_free_size()
{
    //长度应该减去预留部分长度8，保证首尾不会相接
    return get_queue_length() - get_data_size() - EXTRA_BYTE;
}

//获取数据长度
unsigned int CMessageQueue::get_data_size()
{
    //第一次写数据前
    if (_mem_trunk->_begin_pos == _mem_trunk->_end_pos) {
        return 0;
    }
        //数据在两头
    else if (_mem_trunk->_begin_pos > _mem_trunk->_end_pos) {

        return  (unsigned int)(_mem_trunk->_end_pos + _mem_trunk->_size  - _mem_trunk->_begin_pos);
    }
    else   //数据在中间
    {
        return _mem_trunk->_end_pos - _mem_trunk->_begin_pos;
    }
}

unsigned int CMessageQueue::get_queue_length()
{
    return (unsigned int) _mem_trunk->_size;
}

void CMessageQueue::init_lock()
{
    if (is_read_lock()) {
        _read_lock = new CShmRWlock((key_t) (_mem_trunk->_shm_key + 1));
    }

    if (is_write_lock()) {
        _write_lock = new CShmRWlock((key_t) (_mem_trunk->_shm_key + 2));
    }
}

bool CMessageQueue::is_read_lock()
{
    return (_mem_trunk->_queue_model == eQueueModel::MUL_READ_MUL_WRITE ||
        _mem_trunk->_queue_model == eQueueModel::MUL_READ_ONE_WRITE);
}

bool CMessageQueue::is_write_lock()
{
    return (_mem_trunk->_queue_model == eQueueModel::MUL_READ_MUL_WRITE ||
        _mem_trunk->_queue_model == eQueueModel::ONE_READ_MUL_WRITE);
}

/**
  *函数名          : CreateShareMem
  *功能描述        : 创建共享内存块
  *参数			 :  iKey：共享内存块唯一标识key vSize：大小
  *返回值         ： 共享内存块地址
**/
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

/************************************************
  函数名          : DestroyShareMem
  功能描述        : 销毁共享内存块
  参数			:  iKey：共享内存块唯一标识key
  返回值         : 成功0 错误：错误码
************************************************/
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

bool CMessageQueue::is_power_of_2(size_t size) {
    if(size < 1) {
        return false;//2的次幂一定大于0
    }
    return ((size & (size -1)) == 0);
}


int CMessageQueue::fls(size_t size) {
    int position = 0;
    int i = 0;
    if (0 != size) {
        for (i = (size >> 1), position = 0; i != 0; ++position)
            i >>= 1;
    } else {
        position = -1;
    }

    return position + 1;
}

size_t CMessageQueue::round_up_pow_of_2(size_t size) {
    return 1UL << fls(size - 1);
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

