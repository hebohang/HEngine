#pragma once

#include <queue>
#include <mutex>

namespace HEngine
{
    // refer to https://github.com/jpvanoosten/LearningDirectX12/blob/main/DX12Lib/inc/dx12lib/ThreadSafeQueue.h
    // license: MIT
    template<typename T>
    class ThreadSafeQueue
    {
    public:
        ThreadSafeQueue();
        ThreadSafeQueue(const ThreadSafeQueue& inQueue);

        /**
         * Push a value into the back of the queue.
         */
        void Push(T value);

        /**
         * Try to pop a value from the front of the queue.
         * @returns false if the queue is empty.
         */
        bool TryPop(T& value);

        /**
         * Check to see if there are any items in the queue.
         */
        bool Empty() const;

        /**
         * Retrieve the number of items in the queue.
         */
        size_t Size() const;

    private:
        std::queue<T> mQueue;
        mutable std::mutex mMutex;
    };

    template<typename T>
    ThreadSafeQueue<T>::ThreadSafeQueue()
    {}

    template<typename T>
    ThreadSafeQueue<T>::ThreadSafeQueue(const ThreadSafeQueue<T>& inQueue)
    {
        std::lock_guard<std::mutex> lock(inQueue.mMutex);
        mQueue = inQueue.mQueue;
    }

    template<typename T>
    void ThreadSafeQueue<T>::Push(T value)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mQueue.push(std::move(value));
    }

    template<typename T>
    bool ThreadSafeQueue<T>::TryPop(T& value)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mQueue.empty())
            return false;

        value = mQueue.front();
        mQueue.pop();

        return true;
    }

    template<typename T>
    bool ThreadSafeQueue<T>::Empty() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mQueue.empty();
    }

    template<typename T>
    size_t ThreadSafeQueue<T>::Size() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mQueue.size();
    }
}
