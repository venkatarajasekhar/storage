#ifndef STORAGE_COND_H_
#define STORAGE_COND_H_

#include <pthread.h>
#include <assert.h>
#include "mutex.h"
#include "utime.h"

namespace storage
{

class Cond
{
private:
	pthread_cond_t cond_;
	Mutex *waiter_mutex_;	

	// no copying allowed
	Cond(const Cond &);
	void operator=(Cond &c);

public:
	Cond() : waiter_mutex_(NULL)
	{
		int r = pthread_cond_init(&cond_, NULL);
		assert(r == 0);
	}

	~Cond()
	{
		int r = pthread_cond_destroy(&cond_);
		assert(r == 0);
	}

	int32_t Wait(Mutex &mutex)
	{
		assert(waiter_mutex_ == NULL || waiter_mutex_ == &mutex);
		waiter_mutex_ = &mutex;

		int r = pthread_cond_wait(&cond_, &mutex.m_);
		return (int32_t)r;
	}

	int32_t WaitUtil(Mutex &mutex, UTime time)
	{
		assert(waiter_mutex_ == NULL || waiter_mutex_ == &mutex);

		waiter_mutex_ = &mutex;

		struct timespec ts;
		time.ToTimeSpec(&ts);

		int r = pthread_cond_timedwait(&cond_, &mutex.m_, &ts);

		return (int32_t)r;
	}

	int32_t Signal()
	{
		int r = pthread_cond_signal(&cond_);

		return (int32_t)r;
	}

	int32_t SignalAll()
	{
		int r = pthread_cond_broadcast(&cond_);
		
		return r;
	}

}; // end of class Cond

}

#endif
