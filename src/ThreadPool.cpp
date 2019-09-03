#include "ThreadPool.h"
#include "Thread.h"

#include <vector>
#include <atomic>
#include <condition_variable>
#include <algorithm>

// 线程池数据结构体
struct ThreadPoolStructure
{
	using size_type = ThreadPool::size_type;
	using TaskQueue = Queue<ThreadPool::TaskPair>;
	std::vector<std::unique_ptr<Thread>> threadTable;		// 工作线程表
	std::shared_ptr<TaskQueue> taskQueue;					// 任务队列
	std::function<void(bool, Thread::ThreadID)> callback;	// 回调函数子
	std::thread thread;										// 线程体
	std::mutex mutex;										// 互斥元
	std::condition_variable signal;							// 条件变量
	std::atomic_bool closed;								// 关闭标记
	std::atomic<size_type> maxThreads;						// 最大线程数
	std::atomic<size_type> freeThreads;						// 空闲线程数
	//std::atomic_int timeSlice;
	ThreadPoolStructure()
	{
		taskQueue = std::make_shared<TaskQueue>();
	}
};

// 线程池构造函数
ThreadPool::ThreadPool(size_type threads, size_type maxThreads)
{
	/* shared_ptr需要维护引用计数，若调用构造函数（即先以new运算符创建对象，再传递给shared_ptr），
	一共申请两次内存，先申请对象内存，再申请控制块内存，对象内存和控制块内存不连续。
	而使用make_shared方法只申请一次内存，对象内存和控制块内存在一起。 */
	data = std::make_shared<ThreadPoolStructure>();

	setClosed(data, false);	// 设置线程未关闭
	//setTimeSlice(timeSlice);
	setMaxThreads(maxThreads);	// 设置最大线程数量
	// 保证工作线程数量不超过最大线程数量
	threads = std::min(threads, maxThreads);

	/* 工作线程主动于任务队列获取任务，获取失败时回调通知线程池，
	空闲线程数量加一，若未增加之前，空闲线程数量为零，则唤醒阻塞的管理线程 */
	data->callback = [data = data](bool free, Thread::ThreadID)
	{
		if (free && ++data->freeThreads == 0x01)
			data->signal.notify_one();
	};

	data->threadTable.reserve(threads);	// 预分配内存空间，但是不初始化内存，即未调用构造函数
	// 初始化线程并放入工作线程表
	for (decltype(threads) counter = 0; counter < threads; ++counter)
		data->threadTable.push_back(std::make_unique<Thread>(data->taskQueue, data->callback));
	data->freeThreads = data->threadTable.size();	// 设置空闲线程数量
	data->thread = std::thread(ThreadPool::execute, data);	// 创建thread，执行ThreadPool类的execute函数
}

// 线程池析构函数
ThreadPool::~ThreadPool()
{
	// 若数据非空则销毁线程，否则不销毁，以支持移动语义
	if (data)
		destroy();
}

// 获取硬件设备并发运行的最大线程数量
ThreadPool::size_type ThreadPool::getConcurrency()
{
	return std::thread::hardware_concurrency();
}

//// 设置管理器轮询时间片
//bool ThreadPool::setTimeSlice(size_type timeSlice)
//{
//	if (timeSlice < 0)
//		return false;
//	data->timeSlice = timeSlice;
//	return true;
//}
//
//// 获取管理器轮询时间片
//ThreadPool::size_type ThreadPool::getTimeSlice() const
//{
//	return data->timeSlice;
//}

// 设置工作线程的最大数量
void ThreadPool::setMaxThreads(size_type maxThreads)
{
	data->maxThreads = maxThreads > 0 ? maxThreads : 0x01;
}

// 获取最大工作线程数量
ThreadPool::size_type ThreadPool::getMaxThreads() const
{
	return data->maxThreads;
}

// 设置工作线程数量
bool ThreadPool::setThreads(size_type threads)
{
	// 保证工作线程数量不超过上限
	if (threads > getMaxThreads())
		return false;
	// 增加工作线程
	if (auto number = threads - data->threadTable.size(); 
		number > 0)
	{
		std::unique_lock<std::mutex> locker(data->mutex);	// 死锁
		data->threadTable.reserve(threads);	// 增加工作线程表容量
		// 向工作线程表添加工作线程
		for (decltype(number) counter = 0; counter < number; ++counter)
			data->threadTable.push_back(std::make_unique<Thread>(data->taskQueue, data->callback));
		locker.unlock();

		data->freeThreads += number;
		// 如果未添加工作线程之前，无空闲工作线程，唤醒或许阻塞的管理线程
		if (data->freeThreads == number)
			data->signal.notify_one();
		return true;
	}
	// 减少工作线程（未制定策略）
	else if (number < 0)
	{
		return false;
	}
	return false;
}

// 获取工作线程数量
ThreadPool::size_type ThreadPool::getThreads() const
{
	//std::lock_guard<std::mutex> locker(data->mutex);	// 死锁
	return data->threadTable.size();
}

// 获取空闲工作线程数量
ThreadPool::size_type ThreadPool::getFreeThreads() const
{
	return data->freeThreads;
}

// 获取任务队列的任务数
ThreadPool::size_type ThreadPool::getTasks() const
{
	return data->taskQueue->size();
}

// 向任务队列添加单任务
void ThreadPool::pushTask(functor process, functor callback)
{
	using std::move;
	data->taskQueue->push(std::make_pair(move(process), move(callback)));
	// 如果未添加任务之前，任务队列为空，唤醒或许阻塞的管理线程
	if (data->taskQueue->size() == 0x01)
		data->signal.notify_one();
}

// 向任务队列添加单任务
void ThreadPool::pushTask(TaskPair&& task)
{
	data->taskQueue->push(std::move(task));
	if (data->taskQueue->size() == 0x01)
		data->signal.notify_one();
}

// 向任务队列批量添加任务
void ThreadPool::pushTask(std::list<TaskPair>& tasks)
{
	auto size = tasks.size();
	data->taskQueue->push(tasks);
	if (data->taskQueue->size() == size)
		data->signal.notify_one();
}

// 销毁线程池
void ThreadPool::destroy()
{
	// 若已经销毁线程池，忽略以下步骤
	if (getClosed(data))
		return;
	setClosed(data, true);	// 设置线程池为关闭状态，即销毁状态

	data->thread.detach();	// 分离线程池管理线程
	data->signal.notify_one();	// 唤醒阻塞的管理线程
	//std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

//bool ThreadPool::getTask(std::shared_ptr<Thread> thread)
//{
//	std::unique_lock<std::mutex> locker(data->tasks->mutex());
//	if (!data->tasks->empty())	// 任务队列非空
//	{
//		thread->configure(std::move(data->tasks->front()));	// 为工作线程配置新任务
//		data->tasks->pop();	// 任务队列弹出已经配置的任务
//		return true;
//	}
//	locker.unlock();
//
//	// 任务队列为空，空闲线程数量加一，若未增加之前，空闲线程数量为零，则唤醒阻塞的管理线程
//	if (++data->freeThreads == 0x01)
//		data->signal.notify_one();
//	return false;
//}

// 管理线程主函数
void ThreadPool::execute(data_type data)
{
	/* 创建std::unique_lock互斥锁，指定延迟锁定策略，用于互斥访问工作线程表
	由于std::unique_lock析构之时，自动释放锁，因此无需手动释放 */
	using lock_type = std::unique_lock<std::mutex>;
	using std::defer_lock;
	lock_type threadLocker(data->mutex, defer_lock);
	// 创建互斥锁，延迟锁定任务队列互斥元，用于互斥访问任务队列
	lock_type taskLocker(data->taskQueue->mutex(), defer_lock);

	// 定义Lambda函数，用于销毁工作线程
	auto destroy = [&data]
	{
		//std::lock_guard<std::mutex> locker(data->mutex);
		// 遍历工作线程表，销毁所有工作线程
		//for each (auto& thread in data->threads)
		//	thread->destroy();
		//for (auto it = data->threads.cbegin(); it != data->threads.cend(); ++it)
		//	it->get()->destroy();
		for (auto& thread : data->threadTable)
			thread->destroy();
	};

	while (!getClosed(data))	// 管理线程退出通道
	{
		threadLocker.lock();	// 锁定线程互斥锁
		// 无空闲工作线程
		if (!data->freeThreads)
		{
			/* 再次锁定线程锁，阻塞管理线程，等待条件变量的唤醒信号，
			直到空闲线程数量非零或者关闭管理线程，释放一次线程锁 */
			data->signal.wait(threadLocker, 
				[&data] {return data->freeThreads || getClosed(data); });
			// 若管理线程为关闭状态，退出循环，结束线程
			if (getClosed(data))
				break;
		}

		// 遍历工作线程表，给空闲工作线程分配任务
		for (auto it = data->threadTable.begin(); 
			it != data->threadTable.end() && data->freeThreads && !getClosed(data); ++it)
		{
			if (auto& thread = *it; thread->free())	// 若工作线程处于空闲状态
			{
				taskLocker.lock();	// 锁定任务队列互斥锁
				/* 若任务队列为空，阻塞管理线程，等待条件变量的唤醒信号，
				并且唤醒后任务队列应残留任务，未被其他工作线程取走，否则再次阻塞线程 */
				while (data->taskQueue->empty())
				{
					/* 再次锁定任务锁，阻塞管理线程，等待条件变量的唤醒信号，
					直到任务队列非空或者关闭管理线程，释放一次任务锁 */
					data->signal.wait(taskLocker, 
						[&data] {return !data->taskQueue->empty() || getClosed(data); });
					if (getClosed(data))
					{
						destroy();	// 管理线程结束之时，销毁工作线程
						return;
					}
				}

				if (thread->configure(data->taskQueue->front())		// 为工作线程分配新任务
					&& thread->start())	// 唤醒阻塞中的工作线程
				{
					data->taskQueue->pop();	// 任务队列弹出已经配置的任务
					--data->freeThreads;	// 空闲工作线程数量减一
				}
				taskLocker.unlock();	// 释放任务队列互斥锁
			}
		}
		threadLocker.unlock();	// 释放线程互斥锁
	}
	destroy();	// 管理线程结束之时，销毁工作线程
}

// 设置线程池的关闭状态，用于初始线程池状态和关闭线程池
inline void ThreadPool::setClosed(data_type& data, bool closed)
{
	data->closed = closed;
}

// 获取线程池的关闭状态
inline bool ThreadPool::getClosed(const data_type& data)
{
	return data->closed;
}
