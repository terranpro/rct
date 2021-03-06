#include "rct/ThreadPool.h"
#include "rct/Thread.h"
#include <algorithm>
#include <assert.h>
#if defined (OS_FreeBSD) || defined (OS_NetBSD) || defined (OS_OpenBSD)
#   include <sys/types.h>
#   include <sys/sysctl.h>
#elif defined (OS_Linux)
#   include <unistd.h>
#elif defined (OS_Darwin)
#   include <sys/param.h>
#   include <sys/sysctl.h>
#endif

using std::shared_ptr;

ThreadPool* ThreadPool::sInstance = 0;

class ThreadPoolThread : public Thread
{
public:
    ThreadPoolThread(ThreadPool* pool);
    ThreadPoolThread(const std::shared_ptr<ThreadPool::Job> &job);

    void stop();

protected:
    virtual void run();

private:
    std::shared_ptr<ThreadPool::Job> mJob;
    ThreadPool* mPool;
    bool mStopped;
};

ThreadPoolThread::ThreadPoolThread(ThreadPool* pool)
    : mPool(pool), mStopped(false)
{
    setAutoDelete(false);
}

ThreadPoolThread::ThreadPoolThread(const std::shared_ptr<ThreadPool::Job> &job)
    : mJob(job), mPool(0), mStopped(false)
{
    setAutoDelete(false);
}

void ThreadPoolThread::stop()
{
    std::lock_guard<std::mutex> lock(mPool->mMutex);
    mStopped = true;
    mPool->mCond.notify_all();
}

void ThreadPoolThread::run()
{
    if (mJob) {
        mJob->mMutex.lock();
        mJob->run();
        mJob->mMutex.unlock();
        return;
    }
    bool first = true;
    for (;;) {
        std::unique_lock<std::mutex> lock(mPool->mMutex);
        if (!first) {
            --mPool->mBusyThreads;
        } else {
            first = false;
        }
        while (mPool->mJobs.empty() && !mStopped)
            mPool->mCond.wait(lock);
        if (mStopped)
            break;
        std::deque<std::shared_ptr<ThreadPool::Job> >::iterator item = mPool->mJobs.begin();
        assert(item != mPool->mJobs.end());
        std::shared_ptr<ThreadPool::Job> job = *item;
        mPool->mJobs.erase(item);
        {
            std::lock_guard<std::mutex> joblock(job->mMutex);
            job->mState = ThreadPool::Job::Running;
        }
        ++mPool->mBusyThreads;
        lock.unlock();
        job->run();
        {
            std::lock_guard<std::mutex> joblock(job->mMutex);
            job->mState = ThreadPool::Job::Finished;
        }
    }
}

ThreadPool::ThreadPool(int concurrentJobs)
    : mConcurrentJobs(concurrentJobs), mBusyThreads(0)
{
    if (!sInstance)
        sInstance = this;
    for (int i = 0; i < mConcurrentJobs; ++i) {
        mThreads.push_back(new ThreadPoolThread(this));
        mThreads.back()->start();
    }
}

ThreadPool::~ThreadPool()
{
    if (sInstance == this)
        sInstance = 0;
    std::unique_lock<std::mutex> lock(mMutex);
    mJobs.clear();
    lock.unlock();
    for (List<ThreadPoolThread*>::iterator it = mThreads.begin();
         it != mThreads.end(); ++it) {
        ThreadPoolThread* t = *it;
        t->stop();
        t->join();
        delete t;
    }
}

void ThreadPool::setConcurrentJobs(int concurrentJobs)
{
    if (concurrentJobs == mConcurrentJobs)
        return;
    if (concurrentJobs > mConcurrentJobs) {
        std::lock_guard<std::mutex> lock(mMutex);
        for (int i = mConcurrentJobs; i < concurrentJobs; ++i) {
            mThreads.push_back(new ThreadPoolThread(this));
            mThreads.back()->start();
        }
        mConcurrentJobs = concurrentJobs;
    } else {
        std::unique_lock<std::mutex> lock(mMutex);
        for (int i = mConcurrentJobs; i > concurrentJobs; --i) {
            ThreadPoolThread* t = mThreads.back();
            mThreads.pop_back();
            lock.unlock();
            t->stop();
            t->join();
            lock.lock();
            delete t;
        }
        mConcurrentJobs = concurrentJobs;
    }
}

bool ThreadPool::jobLessThan(const std::shared_ptr<Job> &l, const std::shared_ptr<Job> &r)
{
    return static_cast<unsigned>(l->mPriority) > static_cast<unsigned>(r->mPriority);
}

void ThreadPool::start(const std::shared_ptr<Job> &job, int priority)
{
    job->mPriority = priority;
    if (priority == Guaranteed) {
        ThreadPoolThread *t = new ThreadPoolThread(job);
        t->start();
        return;
    }

    std::lock_guard<std::mutex> lock(mMutex);
    if (mJobs.empty()) {
        mJobs.push_back(job);
    } else {
        if (mJobs.at(mJobs.size() - 1)->mPriority >= priority) {
            mJobs.push_back(job);
        } else if (mJobs.at(0)->mPriority < priority) {
            mJobs.push_front(job);
        } else {
            mJobs.push_back(job);
            std::sort(mJobs.begin(), mJobs.end(), jobLessThan);
        }
    }
    mCond.notify_one();
}

bool ThreadPool::remove(const std::shared_ptr<Job> &job)
{
    std::lock_guard<std::mutex> lock(mMutex);
    std::deque<std::shared_ptr<Job> >::iterator it = std::find(mJobs.begin(), mJobs.end(), job);
    if (it == mJobs.end())
        return false;
    mJobs.erase(it);
    return true;
}

int ThreadPool::idealThreadCount()
{
#if defined (OS_FreeBSD) || defined (OS_NetBSD) || defined (OS_OpenBSD)
    int cores;
    size_t len = sizeof(cores);
    int mib[2];
    mib[0] = CTL_HW;
    mib[1] = HW_NCPU;
    if (sysctl(mib, 2, &cores, &len, NULL, 0) != 0)
        return 1;
    return cores;
#elif defined (OS_Linux)
    return (int)sysconf(_SC_NPROCESSORS_ONLN);
#elif defined (OS_Darwin)
    int cores;
    size_t len = sizeof(cores);
    int mib[2] = { CTL_HW, HW_AVAILCPU };
    if (sysctl(mib, 2, &cores, &len, NULL, 0)) {
        mib[1] = HW_NCPU;
        if (sysctl(mib, 2, &cores, &len, NULL, 0))
            return 1;
    }
    return cores;
#else
#   warning idealthreadcount not implemented on this platform
    return 1;
#endif
}

ThreadPool* ThreadPool::instance()
{
    if (!sInstance)
        sInstance = new ThreadPool(idealThreadCount());
    return sInstance;
}

ThreadPool::Job::Job()
    : mPriority(0), mState(NotStarted)
{
}

void ThreadPool::clearBackLog()
{
    std::lock_guard<std::mutex> lock(mMutex);
    mJobs.clear();
}
