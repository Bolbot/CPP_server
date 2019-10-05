#ifndef __MULTITHREADING_H__
#define __MULTITHREADING_H__

#include <atomic>
#include <exception>
#include <thread>
#include <future>
#include <queue>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>

#include <iostream>

/*class interrupt_flag final
{
private:
	std::atomic<bool> flag{ false };

public:
	void set()
	{
		flag.store(true, std::memory_order_release);
	}
	bool is_set() const
	{
		return flag.load(std::memory_order_acquire);
	}
};

class thread_interrupted : public std::exception
{};

void interruption_point();

class interruptible_thread final
{
private:
	std::thread thread;
	static thread_local interrupt_flag this_thread_interrupt_flag;
	interrupt_flag *interrupt_flag_ptr;
	friend void interruption_point();
public:
	template <typename Function, typename ... Arguments>
	interruptible_thread(Function function, Arguments ... arguments)
	{
		std::promise<interrupt_flag *> promise;
		thread = std::thread([&promise, &function, &arguments...] ()
		{
			promise.set_value(&interruptible_thread::this_thread_interrupt_flag);
			try
			{
				function(arguments...);
			}
			catch (const thread_interrupted &)
			{}
		});
		interrupt_flag_ptr = promise.get_future().get();
	}

	bool joinable() const
	{
		return thread.joinable();
	}
	void join()
	{
		thread.join();
	}
	void detach()
	{
		thread.detach();
	}
	void interrupt()
	{
		if (interrupt_flag_ptr)
			interrupt_flag_ptr->set();
	}
};
*/

template <typename T>
class mt_safe_queue final
{
private:
	std::queue<std::shared_ptr<T>> queue;
	mutable std::mutex mutex;
	std::condition_variable condv;
public:
	mt_safe_queue() = default;
	mt_safe_queue(const mt_safe_queue &) = delete;
	mt_safe_queue &operator=(const mt_safe_queue &) = delete;

	void push(T &&element)
	{
		std::shared_ptr<T> pointer = std::make_shared<T>(std::move(element));

		std::lock_guard<std::mutex> lock(mutex);
		queue.push(std::move(pointer));
	}

	bool try_pop(T &dest)
	{
		std::lock_guard<std::mutex> lock(mutex);

		if (queue.empty())
			return false;

		dest = std::move(*queue.front());
		queue.pop();
		return true;
	}

	std::shared_ptr<T> try_pop()
	{
		std::lock_guard<std::mutex> lock(mutex);

		if (queue.empty())
			return nullptr;

		std::shared_ptr<T> pointer = std::move(queue.front());
		queue.pop();
		return pointer;
	}

	void wait_and_pop(T &dest)
	{
		std::unique_lock<T> lock(mutex);
		if (queue.empty())
			condv.wait(lock, [this]() { return !queue.empty(); });

		dest = std::move(*queue.front());
		queue.pop();
	}

	std::shared_ptr<T> wait_and_pop()
	{
		std::unique_lock<T> lock(mutex);
		if (queue.empty())
			condv.wait(lock, [this]() { return !queue.empty(); });

		std::shared_ptr<T> pointer = std::move(queue.front());
		queue.pop();
		return pointer;
	}

	bool empty() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return queue.empty();
	}
};

template <typename T>
class stealing_queue final
{
private:
	std::deque<std::shared_ptr<T>> deque;
	mutable std::mutex mutex;
	std::condition_variable condv;
public:
	stealing_queue() = default;
	stealing_queue(const stealing_queue &) = delete;
	stealing_queue &operator=(const stealing_queue &) = delete;

	void push(T &&element)
	{
		std::shared_ptr<T> ptr = std::make_shared<T>(std::move(element));

		std::lock_guard<std::mutex> lock(mutex);
		deque.push_front(std::move(ptr));
		condv.notify_one();
	}

	bool try_pop(T &dest)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (deque.empty())
			return false;

		dest = std::move(*deque.front());
		deque.pop_front();
		return true;
	}
	std::shared_ptr<T> try_pop()
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (deque.empty())
			return nullptr;

		std::shared_ptr<T> ptr{ std::move(deque.front()) };
		deque.pop_front();
		return ptr;
	}
	void wait_and_pop(T &dest)
	{
		std::unique_lock<std::mutex> lock(mutex);
		if (deque.empty())
			condv.wait(lock, [this]() { return !deque.empty(); });

		dest = std::move(*deque.front());
		deque.pop_front();
	}
	std::shared_ptr<T> wait_and_pop()
	{
		std::unique_lock<std::mutex> lock(mutex);
		if (deque.empty())
			condv.wait(lock, [this]() { return !deque.empty(); });

		std::shared_ptr<T> ptr = std::move(*deque.front());
		deque.pop_front();
		return ptr;
	}

	bool try_steal(T &dest)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (deque.empty())
			return false;

		dest = std::move(*deque.back());
		deque.pop_back();
		return true;
	}
	std::shared_ptr<T> try_steal()
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (deque.empty())
			return nullptr;

		std::shared_ptr<T> ptr = std::move(deque.back());
		deque.pop_back();
		return ptr;
	}

	bool empty() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return deque.empty();
	}	
};

class thread_joiner final
{
private:
	std::vector<std::thread> &threads;
public:
	explicit thread_joiner(std::vector<std::thread> &t): threads{ t }
	{}
	~thread_joiner()
	{
		for (auto &i: threads)
			if (i.joinable())
				i.join();
	}
};

class thread_pool final
{
private:
	class moveable_task final
	{
	private:
		struct base_impl
		{
			virtual ~base_impl(){}
			virtual void call() = 0;
		};

		std::unique_ptr<base_impl> implementation;

		template <typename Function>
		struct curr_impl: public base_impl
		{
			Function function;
			curr_impl(Function f) : function{ std::move(f) }
			{}
			void call() override
			{
				function();
			}
		};
	public:
		moveable_task() = default;
		template <typename Function>
		moveable_task(Function function) : implementation{ new curr_impl<Function>(std::move(function)) }
		{}
		moveable_task(const moveable_task &) = delete;
		moveable_task &operator=(const moveable_task &) = delete;
		moveable_task(moveable_task &&other) : implementation{ std::move(other.implementation) }
		{}
		moveable_task &operator=(moveable_task &&other)
		{
			if (&other != this)
				implementation = std::move(other.implementation);
			return *this;
		}

		void operator()()
		{
			if (implementation)
				implementation->call();
		}
	};

	std::atomic<bool> terminate_flag;
	mt_safe_queue<moveable_task> common_tasks_queue;
	std::vector<std::unique_ptr<stealing_queue<moveable_task>>> task_queues;
	static thread_local stealing_queue<moveable_task> *local_tasks_queue;
	static thread_local size_t thread_index;

	std::vector<std::thread> threads;
	thread_joiner joiner_of_pool_threads;

	bool try_steal(moveable_task &dest)
	{
		for (size_t i = 0; i != task_queues.size(); ++i)
		{
			size_t index = (thread_index + i + 1) % task_queues.size();

			if (task_queues[index]->try_steal(dest))
				return true;
		}

		return false;
	}

	void working_loop(size_t index)
	{
		thread_index = index;
		local_tasks_queue = task_queues[thread_index].get();

		while (!terminate_flag.load(std::memory_order_acquire))
		{
			moveable_task task;

			if ((local_tasks_queue && local_tasks_queue->try_pop(task)) || common_tasks_queue.try_pop(task) || try_steal(task))
				task();
			else
				std::this_thread::yield();
		}
	}

public:
	thread_pool() : terminate_flag{ false },  task_queues(std::thread::hardware_concurrency()),
			threads(std::thread::hardware_concurrency()), joiner_of_pool_threads{ threads }
	{
		try
		{
			for (auto &i: task_queues)
				i.reset(new stealing_queue<moveable_task>);

			for (size_t i = 0; i != threads.size(); ++i)
				threads[i] = std::thread(&thread_pool::working_loop, this, i);
		}
		catch (...)
		{
			terminate_flag.store(true, std::memory_order_release);
		}
	}
	~thread_pool()
	{
		terminate_flag.store(true, std::memory_order_release);
	}

	template <typename Function, typename Argument>
	void enqueue_task(Function function, Argument &&argument)
	{
		std::cout << typeid(function).name() << " accepting " << typeid(argument).name() << "\n";

		moveable_task task{ std::bind(function, std::move(argument)) };
		if (local_tasks_queue)
			local_tasks_queue->push(std::move(task));
		else
			common_tasks_queue.push(std::move(task));
	}
};

extern std::unique_ptr<thread_pool> worker_threads;

void initialize_thread_pool();

void terminate_thread_pool();

#endif
