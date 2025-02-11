#pragma once
#ifndef THREADPOOL_H
#define THREADPOOL_H
#include<vector>
#include<memory>
#include<queue>
#include<atomic>
#include<mutex>
#include<thread>
#include<iostream>
#include<functional>
#include<condition_variable>
#include<map>
#include<future>

const int TASK_MAX_THRESHOLD = 2;//1024;//  任务数量最大上限
const int Thread_Size_Threshold = 200;
const int THREAD_MAX_IDLE_TIME = 10;//单位：秒  

//线程池支持的模式
enum class PoolMode
{
	MODE_FIXED,//固定数量的线程
	MODE_CACHED,//线程数量可动态增加
};
//线程类
class Thread
{
public:
	//线程函数对象类型
	using ThreadFunc = std::function<void(int)>;

	//线程构造
	Thread(ThreadFunc func)
	{
		func_ = func;
		threadId_ = generateId;
		generateId++;
	}
	//线程析构
	~Thread() = default;
	//启动线程
	void start()
	{
		//创建一个线程来执行线程函数
		std::thread t(func_, threadId_);//线程类的构造函数要求对线程函数传入的参数放在后面
		t.detach();
	}
	//获取线程Id
	int getId()const
	{
		return threadId_;
	}
private:
	ThreadFunc func_;
	static int generateId;//
	int threadId_;//线程Id
};

int Thread::generateId = 0;

//线程池
class ThreadPool
{
public:
	//线程池构造
	ThreadPool():initThreadSize_(0),
		taskSize_(0),
		taskQueMaxThresHold_(TASK_MAX_THRESHOLD),
		poolMode_(PoolMode::MODE_FIXED),
		isPoolRunning_(false),//  线程池运行状态初始为否
		idleThreadSize_(0),
		currentThreadSize_(0),
		threadSizeThreshold_(Thread_Size_Threshold)
	{}
	//线程池析构
	~ThreadPool()
	{
		isPoolRunning_ = false;
		std::unique_lock<std::mutex> lck(taskQueMtx_);
		notEmpty_.notify_all();
		exitCond_.wait(lck, [&]()->bool {return threads_.size() == 0; });
	}

	//启动线程池
	void start(int initThreadSize = std::thread::hardware_concurrency())
	{
		//设置线程池运行状态
		isPoolRunning_ = true;

		initThreadSize_ = initThreadSize;
		currentThreadSize_ = initThreadSize;

		for (int i = 0; i < initThreadSize_; i++)
		{	//这里的线程类是我们自定义，真正的线程在线程类的start成员方法中创建
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, move(ptr));
		}
		for (int i = 0; i < initThreadSize_; i++)
		{
			threads_[i]->start();//需要去执行一个线程函数
			idleThreadSize_++;//  记录初始空闲线程数量
		}
	}

	//线程池工作模式
	void setMode(PoolMode mode)
	{
		if (checkRuningState())
			return;
		poolMode_ = mode;
	}

	//设置任务队列线程数量上限阈值
	void setTaskQueMaxThresHold(int threshold)
	{
		if (checkRuningState())
			return;
		taskQueMaxThresHold_ = threshold;
	}

	//设置线程cached模式下线程阈值
	void setThreadSizeThreshold(int threshold)
	{
		if (checkRuningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED)
		{
			threadSizeThreshold_ = threshold;
		}
	}

	//给线程池提交任务
	//使用可变参模板编程，让submitTask可以接受任意的任务函数和任意数量的参数
	//Result submitTask(std::shared_ptr<Task> sp);
	template<typename Func,typename... Args>
	auto submitTask(Func&& func, Args... args)->std::future<decltype(func(args...))>
	{
		//打包任务，放入任务队列
		using RType = decltype(func(args...));//获取类型
		auto task = std::make_shared<std::packaged_task<RType()>>(//创建了一个packaged_task对象来打包一个返回值是RType，不带参数的一个函数对象
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...));//然后通过Bind把函数对象的参数绑定上去
		std::future<RType>result = task->get_future();

		//获取锁
		std::unique_lock<std::mutex> lck(taskQueMtx_);
		//线程的通信  等待任务队列有空余  任务队列满了的话 就不能新增任务到任务队列
		//等待一秒钟，若任务队列仍然是满的，就输出提交任务失败提示，然后退出提交
		if (!notFull_.wait_for(lck, std::chrono::seconds(1), [&]()->bool {return taskQue_.size() < (size_t)taskQueMaxThresHold_; }))
		{
			std::cerr << "taskQue_ is full,submit task failed" << std::endl;
			//任务队列满了，无法提交，返回一个零值
			auto task = std::make_shared<std::packaged_task<RType()>>(
				[]() {return RType(); }
			);
			(*task)();
			return task->get_future();
		}

		//任务队列有空余，将任务放入任务队列
		//taskQue_.emplace(sp);
		//using Task = std::function<void()>;由于不知道任务的返回值和参数是什么，所以初始化的任务队列都设为空，
											// 然后再传入任务的时候，在外面套一个lambda表达式
		taskQue_.emplace([task]() {(*task)(); });
		taskSize_++;
		//因为新放了任务，任务队列肯定不空，在notEmpty上通知线程
		notEmpty_.notify_all();

		//cached模式：任务处理比较紧急 场景：小而快的任务  需要根据任务数量和空闲线程数量 判断是否需要创建新线程
		if (taskSize_ > idleThreadSize_ && poolMode_ == PoolMode::MODE_CACHED && currentThreadSize_ < threadSizeThreshold_)
		{
			//创建新的线程
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			std::cout << ">>> create new thread..." << std::endl;
			int threadId = ptr->getId();
			threads_.emplace(threadId, move(ptr));
			//启动新创建的线程
			threads_[threadId]->start();
			//修改线程数量相关的变量
			currentThreadSize_++;
			idleThreadSize_++;
		}
		//返回任务的result对象
		return result;
	}

	//不允许拷贝构造和重载赋值
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	//声明线程函数
	void threadFunc(int threadid)
	{
		auto lastTime = std::chrono::high_resolution_clock().now();//线程开始时间
		//线程不断地从任务队列里面消费任务，直到为空
		//等待所有任务执行完，线程池才可以回收所有线程资源
		for (;;)
		{
			Task task;
			{
				//先获取锁
				std::unique_lock<std::mutex> lck(taskQueMtx_);
				std::cout << "tid" << std::this_thread::get_id() << "尝试获取任务..." << std::endl;

				//cached模式下，有可能已经创建了许多的线程，且空闲时间超过了60s,这部分线程应该结束并回收掉（超过initThreadSize_的部分）
				//  当前时间 - 线程上次执行完任务的时间 > 60s
				//每一秒钟返回一次  怎么区分：超时时间？ 还有任务待执行？
				while (taskQue_.size() == 0)
				{
					if (!isPoolRunning_)
					{
						threads_.erase(threadid);
						std::cout << "thread id:" << std::this_thread::get_id() << "exit!" << std::endl;
						exitCond_.notify_all();
						return;
					}
					if (poolMode_ == PoolMode::MODE_CACHED)
					{
						if (std::cv_status::timeout == notEmpty_.wait_for(lck, std::chrono::seconds(1)))
						{
							auto nowTime = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(nowTime - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME && currentThreadSize_ > initThreadSize_)
							{
								//开始回收当前线程
								//记录线程数量的相关值修改
								//线程列表中删除相应线程
								threads_.erase(threadid);
								currentThreadSize_--;
								idleThreadSize_--;
								std::cout << "thread id:" << std::this_thread::get_id() << "exit!" << std::endl;
								return;
							}
						}
					}
					else
					{
						//等待notEmpty条件
						notEmpty_.wait(lck);
					}
					//线程池要结束，回收线程资源
				/*	if (!isPoolRunning_)
					{
						threads_.erase(threadid);
						std::cout << "thread id:" << std::this_thread::get_id() << "exit!" << std::endl;
						exitCond_.notify_all();
						return;
					}
				*/
				}
				idleThreadSize_--;//  空闲线程数量减少
				std::cout << "tid" << std::this_thread::get_id() << "获取任务成功..." << std::endl;
				//从任务队列中取出一个任务
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;

				//如果任务队列不为空 通知其他线程取任务
				if (taskQue_.size() > 0)
				{
					notEmpty_.notify_all();
				}
				//取出一个任务，任务队列肯定不满
				notFull_.notify_all();

			}//出作用域，锁释放  保证能够多个线程运行任务
			if (task != nullptr)
			{
				//#1 线程执行任务   #2 获取线程返回值
				//task->run();
				task();
			}
			idleThreadSize_++;//  空闲线程数量增加
			lastTime = std::chrono::high_resolution_clock().now();//更新线程执行完任务的时间
		}
	}
	bool checkRuningState()const
	{
		return isPoolRunning_;
	}
private:
	std::map<int, std::unique_ptr<Thread>> threads_;//线程列表
	int initThreadSize_;  //初始的线程数量
	std::atomic_int idleThreadSize_;//  记录空闲线程的数量
	std::atomic_int currentThreadSize_;//记录当前线程数量

	//Task任务 现在就是一个函数对象
	using Task = std::function<void()>;
	std::queue<Task> taskQue_;  //任务队列
	std::atomic_int taskSize_;  //任务数量   线程安全
	int threadSizeThreshold_;//  线程数量上限
	int taskQueMaxThresHold_;  //任务数量最大阈值

	std::mutex taskQueMtx_;  //保证任务队列的线程安全
	std::condition_variable notFull_;  //任务队列不满
	std::condition_variable notEmpty_;  //任务队列不空
	std::condition_variable exitCond_;  //线程池析构时，各线程通信

	PoolMode poolMode_;  //线程池工作模式
	bool isPoolRunning_;  //表示线程池是否启动

};
#endif