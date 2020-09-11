#ifndef __SHM_RWLOCK_H__
#define __SHM_RWLOCK_H__

#include <sys/sem.h>
#include <sys/types.h>
#include <cstddef>
#include <cerrno>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <sys/ipc.h>

/*
struct sembuf {
    short semnum; -----  信号量集合中的信号量编号，0代表第1个信号量,1代表第二.....

    short val;    -----  若val>0进行V操作信号量值加val，表示进程释放控制的资源
                         若val<0进行P操作信号量值减val，若(semval-val)<0（semval为该信号量值），则调用进程阻塞，直到资源可用；若设置IPC_NOWAIT不会睡眠，
                         进程直接返回EAGAIN错误
                         若val==0时阻塞等待信号量为0，调用进程进入睡眠状态，直到信号值为0；若设置IPC_NOWAIT，进程不会睡眠，直接返回EAGAIN错误

    short flag;   -----  0 设置信号量的默认操作
                         IPC_NOWAIT 设置信号量操作不等待
                         SEM_UNDO  选项会让内核记录一个与调用进程相关的UNDO记录，如果该进程崩溃，则根据这个进程的UNDO记录自动恢复相应信号量的计数值
};
*/

namespace shmmqueue
{
// as "man semctl" page said:
// semctl() performs the control operation specified by cmd on the semaphore set identified by semid,
// or on the semnum-th semaphore of that set.  (The semaphores in a set are numbered starting at 0.)
// This function has three or four arguments, depending on cmd.
// When there are four, the fourth has the type union semun.  The calling program must define this union as follows:
union semun {
    int              val;    /* Value for SETVAL */
    struct semid_ds *buf;    /* Buffer for IPC_STAT, IPC_SET */
    unsigned short  *array;  /* Array for GETALL, SETALL */
    struct seminfo  *__buf;  /* Buffer for IPC_INFO (Linux-specific) */
};

class CShmRWlock
{
public:
    CShmRWlock() : _sem_id(-1), _sem_key(-1) {}
    CShmRWlock(key_t key)
    {
        init(key);
    }

    /**
     * @brief : try to lock read until success
     * @param :
     * @return : 0 is returned on success, otherwise return -1 with errno indicating the error
     */
	int r_lock() const
	{
		struct sembuf sops[2] = {{0, 0, SEM_UNDO}, {1, 1, SEM_UNDO}};
		size_t nsops = 2;
		int ret = -1;

		do {
			ret = semop(_sem_id, sops, nsops);
		} while ((ret == -1) && (errno == EINTR));

		return ret;
	}

    /**
     * @brief : try to unlock read until success
     * @param :
     * @return : 0 is returned on success, otherwise return -1 with errno indicating the error
     */
	int r_unlock() const
	{
		struct sembuf sops[1] = {{1, -1, SEM_UNDO}};
		size_t nsops = 1;

		int ret = -1;
		do {
			ret = semop(_sem_id, &sops[0], nsops);

		} while ((ret == -1) && (errno == EINTR));

		return ret;
	}

    /**
     * @brief : try to lock read once
     * @param :
     * @return : true is returned on success, otherwise return false with errno indicating the error
     */
	bool try_r_lock() const
	{
		struct sembuf sops[2] = {{0, 0, SEM_UNDO | IPC_NOWAIT}, {1, 1, SEM_UNDO | IPC_NOWAIT}};
		size_t nsops = 2;

		int iRet = semop(_sem_id, &sops[0], nsops);
		if (iRet == -1) {
			if (errno == EAGAIN) {
				//无法获得锁
				return false;
			}
			else {
				throw std::runtime_error("semop error : " + std::string(strerror(errno)));
			}
		}
		return true;
	}

    /**
     * @brief : try to lock write until success
     * @param :
     * @return : 0 is returned on success, otherwise return -1 with errno indicating the error
     */
	int w_lock() const
	{
		struct sembuf sops[3] = {{0, 0, SEM_UNDO}, {1, 0, SEM_UNDO}, {0, 1, SEM_UNDO}};
		size_t nsops = 3;

		int ret = -1;

		do {
			ret = semop(_sem_id, &sops[0], nsops);

		}
		while ((ret == -1) && (errno == EINTR));

		return ret;
	}

    /**
     * @brief : try to unlock write until success
     * @param :
     * @return : 0 is returned on success, otherwise return -1 with errno indicating the error
     */
	int w_unlock() const
	{
		struct sembuf sops[1] = {{0, -1, SEM_UNDO}};
		size_t nsops = 1;

		int ret = -1;

		do {
			ret = semop(_sem_id, &sops[0], nsops);

		}
		while ((ret == -1) && (errno == EINTR));

		return ret;
	}

    /**
     * @brief : try to unlock write once
     * @param :
     * @return : 0 is returned on success, otherwise return -1 with errno indicating the error
     */
	bool try_w_lock() const
	{
		struct sembuf sops[3] = {{0, 0, SEM_UNDO | IPC_NOWAIT},
								 {1, 0, SEM_UNDO | IPC_NOWAIT},
								 {0, 1, SEM_UNDO | IPC_NOWAIT}};

		size_t nsops = 3;
		int iRet = semop(_sem_id, &sops[0], nsops);
		if (iRet == -1) {
			if (errno == EAGAIN) {
				return false;
			}
			else {
				throw std::runtime_error("semop error : " + std::string(strerror(errno)));
			}
		}

		return true;
	}

    /**
     * @brief : get sem key
     * @param :
     * @return : sem key
     */
	key_t get_key() const
	{
		return _sem_key;
	}

    /**
     * @brief : get sem id
     * @param :
     * @return : sem id
     */
	int get_id() const
	{
		return _sem_id;
	}

private:
    /**
     * @brief : init sem
     * @param key_t key : sem key
     * @return
     */
	int init(key_t key)
	{
		int sem_id = -1;
		union semun arg;
		// 2 sem, one for read, and one for write
		u_short array[2] = {0, 0};
		// create new set of sem
		if ((sem_id = semget(key, 2, IPC_CREAT | IPC_EXCL | 0666)) != -1) {
			arg.array = &array[0];

			// set semval for all semaphores of the set arg
			if (semctl(sem_id, 0, SETALL, arg) == -1) {
				return -1;
			}
		} else {
			// judge whether or not the sem set is already exists
			if (errno != EEXIST) {
				return -1;
			}

			// connect to the exists sem set
			if ((sem_id = semget(key, 2, 0666)) == -1) {
				return -1;
			}
		}

		_sem_key = key;
		_sem_id = sem_id;
	}
protected:
    int _sem_id;
    key_t _sem_key;
};

// easier way to lock read
class CSafeShmRlock
{
public:
    CSafeShmRlock(CShmRWlock *lock, bool is_lock)
    {
        _p_lock = NULL;
        if (is_lock && NULL != lock) {
            _p_lock = lock;
            _p_lock->r_lock();
        }
    }

    ~CSafeShmRlock()
    {
        if (NULL != _p_lock) {
            _p_lock->r_unlock();
        }
    }
private:
    CShmRWlock *_p_lock;
};

// easier way to lock write
class CSafeShmWlock
{
public:
    CSafeShmWlock(CShmRWlock* lock, bool is_lock)
    {
        _p_lock = NULL;
        if (is_lock && NULL != lock) {
            _p_lock = lock;
            _p_lock->w_lock();
        }
    }

    ~CSafeShmWlock()
    {
        if (_p_lock != NULL) {
            _p_lock->w_unlock();
        }
    }
private:
    CShmRWlock* _p_lock;
};
}
#endif // __SHM_RWLOCK_H__
