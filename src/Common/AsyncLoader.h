#pragma once

#include <condition_variable>
#include <exception>
#include <memory>
#include <map>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <boost/noncopyable.hpp>
#include <base/defines.h>
#include <base/types.h>
#include <Common/CurrentMetrics.h>
#include <Common/ThreadPool.h>
#include <Common/ErrorCodes.h>
#include <Common/Exception.h>
#include <Common/noexcept_scope.h>
#include <Common/setThreadName.h>

namespace DB
{

class LoadJob;
using LoadJobPtr = std::shared_ptr<LoadJob>;
using LoadJobSet = std::unordered_set<LoadJobPtr>;
class LoadTask;
class AsyncLoader;

namespace ErrorCodes
{
    extern const int ASYNC_LOAD_SCHEDULE_FAILED;
    extern const int ASYNC_LOAD_FAILED;
    extern const int ASYNC_LOAD_CANCELED;
    extern const int ASYNC_LOAD_DEPENDENCY_FAILED;
}

enum class LoadStatus
{
    PENDING, // Load is not finished yet
    SUCCESS, // Load was successful
    FAILED   // Load failed or canceled
};

class LoadJob : private boost::noncopyable
{
public:
    template <class Func>
    LoadJob(LoadJobSet && dependencies_, const String & name_, Func && func_)
        : dependencies(std::move(dependencies_))
        , name(name_)
        , func(std::forward<Func>(func_))
    {}

    LoadStatus status() const
    {
        std::unique_lock lock{mutex};
        return !is_finished ? LoadStatus::PENDING : (exception ? LoadStatus::FAILED : LoadStatus::SUCCESS);
    }

    void wait() const
    {
        std::unique_lock lock{mutex};
        waiters++;
        finished.wait(lock, [this] { return is_finished; });
        waiters--;
        if (exception)
            std::rethrow_exception(exception);
    }

    void waitNoThrow() const
    {
        std::unique_lock lock{mutex};
        waiters++;
        finished.wait(lock, [this] { return is_finished; });
        waiters--;
    }

    size_t waiters_count() const
    {
        std::unique_lock lock{mutex};
        return waiters;
    }

    const LoadJobSet dependencies; // jobs to be done before this one (with ownership), it is `const` to make creation of cycles hard
    const String name;
    std::atomic<ssize_t> priority{0};

private:
    friend class AsyncLoader;

    void setSuccess()
    {
        std::unique_lock lock{mutex};
        is_finished = true;
        if (waiters > 0)
            finished.notify_all();
    }

    void setFailure(const std::exception_ptr & ptr)
    {
        std::unique_lock lock{mutex};
        is_finished = true;
        exception = ptr;
        if (waiters > 0)
            finished.notify_all();
    }

    std::function<void(const LoadJob & self)> func;

    mutable std::mutex mutex;
    mutable std::condition_variable finished;
    mutable size_t waiters = 0;
    bool is_finished = false;
    std::exception_ptr exception;
};

template <class Func>
LoadJobPtr makeLoadJob(LoadJobSet && dependencies, const String & name, Func && func)
{
    return std::make_shared<LoadJob>(std::move(dependencies), name, std::forward<Func>(func));
}

// TODO(serxa): write good comment
class AsyncLoader : private boost::noncopyable
{
private:
    // Key of a pending job in ready queue
    struct ReadyKey
    {
        ssize_t priority;
        UInt64 ready_seqno;

        bool operator<(const ReadyKey & rhs) const
        {
            if (priority == rhs.priority)
                return ready_seqno < rhs.ready_seqno;
            return priority > rhs.priority;
        }
    };

    // Scheduling information for a pending job
    struct Info
    {
        ssize_t priority = 0;
        size_t dependencies_left = 0;
        UInt64 ready_seqno = 0; // zero means that job is not in ready queue
        LoadJobSet dependent_jobs;

        ReadyKey key() const
        {
            return {priority, ready_seqno};
        }
    };

public:
    using Metric = CurrentMetrics::Metric;

    // Helper class that removes all not started jobs in destructor and wait all executing jobs to finish
    class Task
    {
    public:
        Task()
            : loader(nullptr)
        {}

        Task(AsyncLoader * loader_, LoadJobSet && jobs_)
            : loader(loader_)
            , jobs(std::move(jobs_))
        {}

        Task(const Task & o) = delete;
        Task & operator=(const Task & o) = delete;

        Task(Task && o) noexcept
            : loader(std::exchange(o.loader, nullptr))
            , jobs(std::move(o.jobs))
        {}

        Task & operator=(Task && o) noexcept
        {
            loader = std::exchange(o.loader, nullptr);
            jobs = std::move(o.jobs);
            return *this;
        }

        void merge(Task && o)
        {
            if (!loader)
            {
                *this = std::move(o);
            }
            else
            {
                chassert(loader == o.loader);
                jobs.merge(o.jobs);
                o.loader = nullptr;
            }
        }

        ~Task()
        {
            remove();
        }

        void remove()
        {
            if (loader)
            {
                loader->remove(jobs);
                detach();
            }
        }

        // Do not track jobs in this task
        void detach()
        {
            loader = nullptr;
            jobs.clear();
        }

    private:
        AsyncLoader * loader;
        LoadJobSet jobs;
    };

    AsyncLoader(Metric metric_threads, Metric metric_active_threads, size_t max_threads_)
        : max_threads(max_threads_)
        , pool(metric_threads, metric_active_threads, max_threads)
    {}

    // WARNING: all Task instances returned by `schedule()` should be destructed before AsyncLoader
    ~AsyncLoader()
    {
        stop();
    }

    // Start workers to execute scheduled load jobs
    void start()
    {
        std::unique_lock lock{mutex};
        is_running = true;
        for (size_t i = 0; workers < max_threads && i < ready_queue.size(); i++)
            spawn(lock);
    }

    // Wait for all load jobs to finish, including all new jobs. So at first take care to stop adding new jobs.
    void wait()
    {
        pool.wait();
    }

    // Wait for currently executing jobs to finish, but do not run any other pending jobs.
    // Not finished jobs are left in pending state:
    //  - they can be resumed by calling start() again;
    //  - or canceled using ~Task() or remove() later.
    void stop()
    {
        {
            std::unique_lock lock{mutex};
            is_running = false;
            // NOTE: there is no need to notify because workers never wait
        }
        pool.wait();
    }

    [[nodiscard]] Task schedule(LoadJobSet && jobs, ssize_t priority = 0)
    {
        std::unique_lock lock{mutex};

        // Sanity checks
        for (const auto & job : jobs)
        {
            if (job->status() != LoadStatus::PENDING)
                throw Exception(ErrorCodes::ASYNC_LOAD_SCHEDULE_FAILED, "Trying to schedule already finished load job '{}'", job->name);
            if (scheduled_jobs.contains(job))
                throw Exception(ErrorCodes::ASYNC_LOAD_SCHEDULE_FAILED, "Load job '{}' has been already scheduled", job->name);
        }

        // Ensure scheduled_jobs graph will have no cycles. The only way to get a cycle is to add a cycle, assuming old jobs cannot reference new ones.
        checkCycle(jobs, lock);

        // We do not want any exception to be throws after this point, because the following code is not exception-safe
        DENY_ALLOCATIONS_IN_SCOPE;

        // Schedule all incoming jobs
        for (const auto & job : jobs)
        {
            NOEXCEPT_SCOPE({
                ALLOW_ALLOCATIONS_IN_SCOPE;
                scheduled_jobs.emplace(job, Info{.priority = priority});
            });
            job->priority.store(priority); // Set user-facing priority
        }

        // Process incoming dependencies
        for (const auto & job : jobs)
        {
            Info & info = scheduled_jobs.find(job)->second;
            for (const auto & dep : job->dependencies)
            {
                // Register every dependency on scheduled job with back-link to dependent job
                if (auto dep_info = scheduled_jobs.find(dep); dep_info != scheduled_jobs.end())
                {
                    NOEXCEPT_SCOPE({
                        ALLOW_ALLOCATIONS_IN_SCOPE;
                        dep_info->second.dependent_jobs.insert(job);
                    });
                    info.dependencies_left++;
                }
                else
                {
                    // TODO(serxa): check status: (1) pending: it is wrong - throw? (2) success: good - no dep. (3) failed: propagate failure!
                }

                // Priority inheritance: prioritize deps to have at least given `priority` to avoid priority inversion
                prioritize(dep, priority, lock);
            }

            // Place jobs w/o dependencies to ready queue
            if (info.dependencies_left == 0)
                enqueue(info, job, lock);
        }

        return Task(this, std::move(jobs));
    }

    // Increase priority of a job and all its dependencies recursively
    void prioritize(const LoadJobPtr & job, ssize_t new_priority)
    {
        DENY_ALLOCATIONS_IN_SCOPE;
        std::unique_lock lock{mutex};
        prioritize(job, new_priority, lock);
    }

    // Remove finished jobs, cancel scheduled jobs, wait for executing jobs to finish and remove them
    void remove(const LoadJobSet & jobs)
    {
        DENY_ALLOCATIONS_IN_SCOPE;
        std::unique_lock lock{mutex};
        for (const auto & job : jobs)
        {
            if (auto it = finished_jobs.find(job); it != finished_jobs.end()) // Job is already finished
            {
                finished_jobs.erase(it);
            }
            else if (auto info = scheduled_jobs.find(job); info != scheduled_jobs.end())
            {
                if (info->second.dependencies_left > 0) // Job is not ready yet
                    canceled(job, lock);
                else if (info->second.ready_seqno) // Job is enqueued in ready queue
                {
                    ready_queue.erase(info->second.key());
                    info->second.ready_seqno = 0;
                    canceled(job, lock);
                }
                else // Job is currently executing
                {
                    lock.unlock();
                    job->waitNoThrow(); // Wait for job to finish
                    lock.lock();
                }
                finished_jobs.erase(job);
            }
        }
    }

private:
    void checkCycle(const LoadJobSet & jobs, std::unique_lock<std::mutex> & lock)
    {
        LoadJobSet left = jobs;
        LoadJobSet visited;
        visited.reserve(left.size());
        while (!left.empty())
        {
            LoadJobPtr job = *left.begin();
            checkCycleImpl(job, left, visited, lock);
        }
    }

    String checkCycleImpl(const LoadJobPtr & job, LoadJobSet & left, LoadJobSet & visited, std::unique_lock<std::mutex> & lock)
    {
        if (!left.contains(job))
            return {}; // Do not consider external dependencies and already processed jobs
        if (auto [_, inserted] = visited.insert(job); !inserted)
        {
            visited.erase(job); // Mark where cycle ends
            return job->name;
        }
        for (const auto & dep : job->dependencies)
        {
            if (auto chain = checkCycleImpl(dep, left, visited, lock); !chain.empty())
            {
                if (!visited.contains(job)) // Check for cycle end
                    throw Exception(ErrorCodes::ASYNC_LOAD_SCHEDULE_FAILED, "Load job dependency cycle detected: {} -> {}", job->name, chain);
                else
                    return fmt::format("{} -> {}", job->name, chain); // chain is not a cycle yet -- continue building
            }
        }
        left.erase(job);
        return {};
    }

    void canceled(const LoadJobPtr & job, std::unique_lock<std::mutex> & lock)
    {
        std::exception_ptr e;
        NOEXCEPT_SCOPE({
            ALLOW_ALLOCATIONS_IN_SCOPE;
            e = std::make_exception_ptr(
                Exception(ErrorCodes::ASYNC_LOAD_CANCELED,
                    "Load job '{}' canceled",
                    job->name));
        });
        failed(job, e, lock);
    }

    void loaded(const LoadJobPtr & job, std::unique_lock<std::mutex> & lock)
    {
        // Notify waiters
        job->setSuccess();

        // Update dependent jobs and enqueue if ready
        chassert(scheduled_jobs.contains(job)); // Job was pending
        for (const auto & dep : scheduled_jobs[job].dependent_jobs)
        {
            chassert(scheduled_jobs.contains(dep)); // All depended jobs must be pending
            Info & dep_info = scheduled_jobs[dep];
            if (--dep_info.dependencies_left == 0)
                enqueue(dep_info, dep, lock);
        }

        finish(job, lock);
    }

    void failed(const LoadJobPtr & job, std::exception_ptr exception_from_job, std::unique_lock<std::mutex> & lock)
    {
        // Notify waiters
        job->setFailure(exception_from_job);

        // Recurse into all dependent jobs
        chassert(scheduled_jobs.contains(job)); // Job was pending
        Info & info = scheduled_jobs[job];
        LoadJobSet dependent;
        dependent.swap(info.dependent_jobs); // To avoid container modification during recursion
        for (const auto & dep : dependent)
        {
            std::exception_ptr e;
            NOEXCEPT_SCOPE({
                ALLOW_ALLOCATIONS_IN_SCOPE;
                e = std::make_exception_ptr(
                    Exception(ErrorCodes::ASYNC_LOAD_DEPENDENCY_FAILED,
                        "Load job '{}' -> {}",
                        dep->name,
                        getExceptionMessage(exception_from_job, /* with_stack_trace = */ false)));
            });
            failed(dep, e, lock);
        }

        // Clean dependency graph edges
        for (const auto & dep : job->dependencies)
            if (auto dep_info = scheduled_jobs.find(dep); dep_info != scheduled_jobs.end())
                dep_info->second.dependent_jobs.erase(job);

        // Job became finished
        finish(job, lock);
    }

    void finish(const LoadJobPtr & job, std::unique_lock<std::mutex> &)
    {
        scheduled_jobs.erase(job);
        NOEXCEPT_SCOPE({
            ALLOW_ALLOCATIONS_IN_SCOPE;
            finished_jobs.insert(job);
        });
    }

    void prioritize(const LoadJobPtr & job, ssize_t new_priority, std::unique_lock<std::mutex> & lock)
    {
        if (auto info = scheduled_jobs.find(job); info != scheduled_jobs.end())
        {
            if (info->second.priority >= new_priority)
                return; // Never lower priority

            // Update priority and push job forward through ready queue if needed
            if (info->second.ready_seqno)
                ready_queue.erase(info->second.key());
            info->second.priority = new_priority;
            job->priority.store(new_priority); // Set user-facing priority (may affect executing jobs)
            if (info->second.ready_seqno)
            {
                NOEXCEPT_SCOPE({
                    ALLOW_ALLOCATIONS_IN_SCOPE;
                    ready_queue.emplace(info->second.key(), job);
                });
            }

            // Recurse into dependencies
            for (const auto & dep : job->dependencies)
                prioritize(dep, new_priority, lock);
        }
    }

    void enqueue(Info & info, const LoadJobPtr & job, std::unique_lock<std::mutex> & lock)
    {
        chassert(info.dependencies_left == 0);
        chassert(info.ready_seqno == 0);
        info.ready_seqno = ++last_ready_seqno;
        NOEXCEPT_SCOPE({
            ALLOW_ALLOCATIONS_IN_SCOPE;
            ready_queue.emplace(info.key(), job);
        });

        if (is_running && workers < max_threads) // TODO(serxa): Can we make max_thread changeable in runtime?
            spawn(lock);
    }

    void spawn(std::unique_lock<std::mutex> &)
    {
        workers++;
        NOEXCEPT_SCOPE({
            ALLOW_ALLOCATIONS_IN_SCOPE;
            pool.scheduleOrThrowOnError([this] { worker(); });
        });
    }

    void worker()
    {
        DENY_ALLOCATIONS_IN_SCOPE;

        LoadJobPtr job;
        std::exception_ptr exception_from_job;
        while (true)
        {
            /// This is inside the loop to also reset previous thread names set inside the jobs
            setThreadName("AsyncLoader");

            {
                std::unique_lock lock{mutex};

                // Handle just executed job
                if (exception_from_job)
                    failed(job, exception_from_job, lock);
                else if (job)
                    loaded(job, lock);

                if (!is_running)
                    return;

                if (ready_queue.empty())
                {
                    workers--;
                    return;
                }

                // Take next job to be executed from the ready queue
                auto it = ready_queue.begin();
                job = it->second;
                ready_queue.erase(it);
                scheduled_jobs.find(job)->second.ready_seqno = 0; // This job is no longer in the ready queue
            }

            try
            {
                ALLOW_ALLOCATIONS_IN_SCOPE;
                job->func(*job);
                exception_from_job = {};
            }
            catch (...)
            {
                tryLogCurrentException(__PRETTY_FUNCTION__);
                NOEXCEPT_SCOPE({
                    ALLOW_ALLOCATIONS_IN_SCOPE;
                    exception_from_job = std::make_exception_ptr(
                        Exception(ErrorCodes::ASYNC_LOAD_FAILED,
                            "Load job '{}' failed: {}",
                            job->name,
                            getCurrentExceptionMessage(/* with_stacktrace = */ true)));
                });
            }
        }
    }

    std::mutex mutex;
    bool is_running = false;

    // TODO(serxa): add metrics for number of jobs in every state

    // Full set of scheduled pending jobs along with scheduling info
    std::unordered_map<LoadJobPtr, Info> scheduled_jobs;

    // Subset of scheduled pending jobs with resolved dependencies (waiting for a thread to be executed)
    // Represent a queue of jobs in order of decreasing priority and FIFO for jobs with equal priorities
    std::map<ReadyKey, LoadJobPtr> ready_queue;

    // Set of finished jobs (for introspection only, until job is removed)
    LoadJobSet finished_jobs;

    // Increasing counter for ReadyKey assignment (to preserve FIFO order of jobs with equal priority)
    UInt64 last_ready_seqno = 0;

    // For executing jobs. Note that we avoid using an internal queue of the pool to be able to prioritize jobs
    size_t max_threads;
    size_t workers = 0;
    ThreadPool pool;
};

}
