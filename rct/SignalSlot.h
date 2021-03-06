#ifndef SIGNALSLOT_H
#define SIGNALSLOT_H

#include <map>
#include <mutex>
#include <assert.h>
#include "EventLoop.h"

template<typename Signature>
class Signal
{
public:
    typedef unsigned int Key;

    Signal() : id(0) { }
    ~Signal() { }

    template<typename Call>
    Key connect(Call&& call)
    {
        std::lock_guard<std::mutex> locker(mutex);
        connections.insert(std::make_pair(++id, std::forward<Call>(call)));
        return id;
    }

    template<size_t Value, typename Call, typename std::enable_if<Value == EventLoop::Async, int>::type = 0>
    Key connect(Call&& call)
    {
        std::lock_guard<std::mutex> locker(mutex);
        connections.insert(std::make_pair(++id, SignatureWrapper(std::forward<Call>(call))));
        return id;
    }

    // this connection type will std::move all the call arguments so if this type is used
    // then no other connections may be used on the same signal
    template<size_t Value, typename Call, typename std::enable_if<Value == EventLoop::Move, int>::type = 0>
    Key connect(Call&& call)
    {
        std::lock_guard<std::mutex> locker(mutex);
        assert(connections.empty());
        connections.insert(std::make_pair(++id, SignatureMoveWrapper(std::forward<Call>(call))));
        return id;
    }

    bool disconnect(Key key)
    {
        std::lock_guard<std::mutex> locker(mutex);
        return connections.erase(key) == 1;
    }

    // ignore result_type for now
    template<typename... Args>
    void operator()(Args&&... args)
    {
        std::lock_guard<std::mutex> locker(mutex);
        for (auto& connection : connections) {
            connection.second(std::forward<Args>(args)...);
        }
    }

    template<typename... Args>
    void operator()(const Args&... args)
    {
        std::lock_guard<std::mutex> locker(mutex);
        for (auto& connection : connections) {
            connection.second(std::forward<const Args>(args)...);
        }
    }

    template<typename... Args>
    void operator()()
    {
        std::lock_guard<std::mutex> locker(mutex);
        for (auto& connection : connections) {
            connection.second();
        }
    }

private:
    class SignatureWrapper
    {
    public:
        SignatureWrapper(Signature&& signature)
            : loop(EventLoop::eventLoop()), call(std::move(signature))
        {
        }

        template<typename... Args>
        void operator()(Args&&... args)
        {
            EventLoop::SharedPtr l;
            if ((l = loop.lock())) {
                l->post(call, std::forward<Args>(args)...);
            }
        }

        EventLoop::WeakPtr loop;
        Signature call;
    };

    class SignatureMoveWrapper
    {
    public:
        SignatureMoveWrapper(Signature&& signature)
            : loop(EventLoop::eventLoop()), call(std::move(signature))
        {
        }

        template<typename... Args>
        void operator()(Args&&... args)
        {
            EventLoop::SharedPtr l;
            if ((l = loop.lock())) {
                l->postMove(call, std::forward<Args>(args)...);
            }
        }

        EventLoop::WeakPtr loop;
        Signature call;
    };

private:
    Key id;
    std::mutex mutex;
    std::map<Key, Signature> connections;
};

#endif
