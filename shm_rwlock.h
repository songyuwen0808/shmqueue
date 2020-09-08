#ifndef SHMQUEUE_SHM_RWLOCK_H
#define SHMQUEUE_SHM_RWLOCK_H

#include <sys/sem.h>
#include <sys/types.h>

namespace shmmqueue
{
class CShmRWlock
{
public:
    // 构造函数
    CShmRWlock();
    //构造函数.
    CShmRWlock(key_t iKey);
    //读锁
    int r_lock() const;
    //释放读锁
    int r_unlock() const;
    /**
     * TryRlock
     * @return  true lock ok,false lock failed
     */
    bool try_r_lock() const;
    //写锁
    int w_lock() const;
    //释放写锁
    int w_unlock() const;
    /**
    * TryRlock
    * @return  true lock ok,false lock failed
    */
    bool try_w_lock() const;
    //lock block until lock sucess
    int lock() const;
    //Unlock
    int unlock() const;
    /**
     * trylock
     * @return true lock ok,false lock failed
     */
    bool try_lock() const;
    //get sem key
    key_t get_key() const;
    //get sem id
    int get_id() const;

private:
    //初始化
    void init(key_t iKey);
protected:
    int _sem_id;
    key_t _sem_key;
};

class CSafeShmRlock
{
public:
    CSafeShmRlock() : _p_lock(NULL) {}
    CSafeShmRlock(CShmRWlock *pLock) : _p_lock(pLock)
    {
        if (_p_lock != NULL) {
            _p_lock->r_lock();
        }
    }

    void init_lock(CShmRWlock *pLock)
    {
        _p_lock = pLock;
        _p_lock->r_lock();
    }

    ~CSafeShmRlock()
    {
        _p_lock->r_unlock();
    }
private:
    CShmRWlock *_p_lock;
};

class CSafeShmWlock
{
public:
    CSafeShmWlock() : _p_lock(NULL)
    {

    }
    CSafeShmWlock(CShmRWlock *pLock) : _p_lock(pLock)
    {
        _p_lock->w_lock();
    }

    void init_lock(CShmRWlock *pLock)
    {
        _p_lock = pLock;
        _p_lock->w_lock();
    }


    ~CSafeShmWlock()
    {
        if (_p_lock != NULL)
        {
            _p_lock->w_unlock();
        }
    }
private:
    CShmRWlock *_p_lock;
};
}
#endif //SHMQUEUE_SHM_RWLOCK_H
