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

const int TASK_MAX_THRESHOLD = 2;//1024;//  ���������������
const int Thread_Size_Threshold = 200;
const int THREAD_MAX_IDLE_TIME = 10;//��λ����  

//�̳߳�֧�ֵ�ģʽ
enum class PoolMode
{
	MODE_FIXED,//�̶��������߳�
	MODE_CACHED,//�߳������ɶ�̬����
};
//�߳���
class Thread
{
public:
	//�̺߳�����������
	using ThreadFunc = std::function<void(int)>;

	//�̹߳���
	Thread(ThreadFunc func)
	{
		func_ = func;
		threadId_ = generateId;
		generateId++;
	}
	//�߳�����
	~Thread() = default;
	//�����߳�
	void start()
	{
		//����һ���߳���ִ���̺߳���
		std::thread t(func_, threadId_);//�߳���Ĺ��캯��Ҫ����̺߳�������Ĳ������ں���
		t.detach();
	}
	//��ȡ�߳�Id
	int getId()const
	{
		return threadId_;
	}
private:
	ThreadFunc func_;
	static int generateId;//
	int threadId_;//�߳�Id
};

int Thread::generateId = 0;

//�̳߳�
class ThreadPool
{
public:
	//�̳߳ع���
	ThreadPool():initThreadSize_(0),
		taskSize_(0),
		taskQueMaxThresHold_(TASK_MAX_THRESHOLD),
		poolMode_(PoolMode::MODE_FIXED),
		isPoolRunning_(false),//  �̳߳�����״̬��ʼΪ��
		idleThreadSize_(0),
		currentThreadSize_(0),
		threadSizeThreshold_(Thread_Size_Threshold)
	{}
	//�̳߳�����
	~ThreadPool()
	{
		isPoolRunning_ = false;
		std::unique_lock<std::mutex> lck(taskQueMtx_);
		notEmpty_.notify_all();
		exitCond_.wait(lck, [&]()->bool {return threads_.size() == 0; });
	}

	//�����̳߳�
	void start(int initThreadSize = std::thread::hardware_concurrency())
	{
		//�����̳߳�����״̬
		isPoolRunning_ = true;

		initThreadSize_ = initThreadSize;
		currentThreadSize_ = initThreadSize;

		for (int i = 0; i < initThreadSize_; i++)
		{	//������߳����������Զ��壬�������߳����߳����start��Ա�����д���
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, move(ptr));
		}
		for (int i = 0; i < initThreadSize_; i++)
		{
			threads_[i]->start();//��Ҫȥִ��һ���̺߳���
			idleThreadSize_++;//  ��¼��ʼ�����߳�����
		}
	}

	//�̳߳ع���ģʽ
	void setMode(PoolMode mode)
	{
		if (checkRuningState())
			return;
		poolMode_ = mode;
	}

	//������������߳�����������ֵ
	void setTaskQueMaxThresHold(int threshold)
	{
		if (checkRuningState())
			return;
		taskQueMaxThresHold_ = threshold;
	}

	//�����߳�cachedģʽ���߳���ֵ
	void setThreadSizeThreshold(int threshold)
	{
		if (checkRuningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED)
		{
			threadSizeThreshold_ = threshold;
		}
	}

	//���̳߳��ύ����
	//ʹ�ÿɱ��ģ���̣���submitTask���Խ�������������������������Ĳ���
	//Result submitTask(std::shared_ptr<Task> sp);
	template<typename Func,typename... Args>
	auto submitTask(Func&& func, Args... args)->std::future<decltype(func(args...))>
	{
		//������񣬷����������
		using RType = decltype(func(args...));//��ȡ����
		auto task = std::make_shared<std::packaged_task<RType()>>(//������һ��packaged_task���������һ������ֵ��RType������������һ����������
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...));//Ȼ��ͨ��Bind�Ѻ�������Ĳ�������ȥ
		std::future<RType>result = task->get_future();

		//��ȡ��
		std::unique_lock<std::mutex> lck(taskQueMtx_);
		//�̵߳�ͨ��  �ȴ���������п���  ����������˵Ļ� �Ͳ������������������
		//�ȴ�һ���ӣ������������Ȼ�����ģ�������ύ����ʧ����ʾ��Ȼ���˳��ύ
		if (!notFull_.wait_for(lck, std::chrono::seconds(1), [&]()->bool {return taskQue_.size() < (size_t)taskQueMaxThresHold_; }))
		{
			std::cerr << "taskQue_ is full,submit task failed" << std::endl;
			//����������ˣ��޷��ύ������һ����ֵ
			auto task = std::make_shared<std::packaged_task<RType()>>(
				[]() {return RType(); }
			);
			(*task)();
			return task->get_future();
		}

		//��������п��࣬����������������
		//taskQue_.emplace(sp);
		//using Task = std::function<void()>;���ڲ�֪������ķ���ֵ�Ͳ�����ʲô�����Գ�ʼ����������ж���Ϊ�գ�
											// Ȼ���ٴ��������ʱ����������һ��lambda���ʽ
		taskQue_.emplace([task]() {(*task)(); });
		taskSize_++;
		//��Ϊ�·�������������п϶����գ���notEmpty��֪ͨ�߳�
		notEmpty_.notify_all();

		//cachedģʽ��������ȽϽ��� ������С���������  ��Ҫ�������������Ϳ����߳����� �ж��Ƿ���Ҫ�������߳�
		if (taskSize_ > idleThreadSize_ && poolMode_ == PoolMode::MODE_CACHED && currentThreadSize_ < threadSizeThreshold_)
		{
			//�����µ��߳�
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			std::cout << ">>> create new thread..." << std::endl;
			int threadId = ptr->getId();
			threads_.emplace(threadId, move(ptr));
			//�����´������߳�
			threads_[threadId]->start();
			//�޸��߳�������صı���
			currentThreadSize_++;
			idleThreadSize_++;
		}
		//���������result����
		return result;
	}

	//����������������ظ�ֵ
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	//�����̺߳���
	void threadFunc(int threadid)
	{
		auto lastTime = std::chrono::high_resolution_clock().now();//�߳̿�ʼʱ��
		//�̲߳��ϵش��������������������ֱ��Ϊ��
		//�ȴ���������ִ���꣬�̳߳زſ��Ի��������߳���Դ
		for (;;)
		{
			Task task;
			{
				//�Ȼ�ȡ��
				std::unique_lock<std::mutex> lck(taskQueMtx_);
				std::cout << "tid" << std::this_thread::get_id() << "���Ի�ȡ����..." << std::endl;

				//cachedģʽ�£��п����Ѿ������������̣߳��ҿ���ʱ�䳬����60s,�ⲿ���߳�Ӧ�ý��������յ�������initThreadSize_�Ĳ��֣�
				//  ��ǰʱ�� - �߳��ϴ�ִ���������ʱ�� > 60s
				//ÿһ���ӷ���һ��  ��ô���֣���ʱʱ�䣿 ���������ִ�У�
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
								//��ʼ���յ�ǰ�߳�
								//��¼�߳����������ֵ�޸�
								//�߳��б���ɾ����Ӧ�߳�
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
						//�ȴ�notEmpty����
						notEmpty_.wait(lck);
					}
					//�̳߳�Ҫ�����������߳���Դ
				/*	if (!isPoolRunning_)
					{
						threads_.erase(threadid);
						std::cout << "thread id:" << std::this_thread::get_id() << "exit!" << std::endl;
						exitCond_.notify_all();
						return;
					}
				*/
				}
				idleThreadSize_--;//  �����߳���������
				std::cout << "tid" << std::this_thread::get_id() << "��ȡ����ɹ�..." << std::endl;
				//�����������ȡ��һ������
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;

				//���������в�Ϊ�� ֪ͨ�����߳�ȡ����
				if (taskQue_.size() > 0)
				{
					notEmpty_.notify_all();
				}
				//ȡ��һ������������п϶�����
				notFull_.notify_all();

			}//�����������ͷ�  ��֤�ܹ�����߳���������
			if (task != nullptr)
			{
				//#1 �߳�ִ������   #2 ��ȡ�̷߳���ֵ
				//task->run();
				task();
			}
			idleThreadSize_++;//  �����߳���������
			lastTime = std::chrono::high_resolution_clock().now();//�����߳�ִ���������ʱ��
		}
	}
	bool checkRuningState()const
	{
		return isPoolRunning_;
	}
private:
	std::map<int, std::unique_ptr<Thread>> threads_;//�߳��б�
	int initThreadSize_;  //��ʼ���߳�����
	std::atomic_int idleThreadSize_;//  ��¼�����̵߳�����
	std::atomic_int currentThreadSize_;//��¼��ǰ�߳�����

	//Task���� ���ھ���һ����������
	using Task = std::function<void()>;
	std::queue<Task> taskQue_;  //�������
	std::atomic_int taskSize_;  //��������   �̰߳�ȫ
	int threadSizeThreshold_;//  �߳���������
	int taskQueMaxThresHold_;  //�������������ֵ

	std::mutex taskQueMtx_;  //��֤������е��̰߳�ȫ
	std::condition_variable notFull_;  //������в���
	std::condition_variable notEmpty_;  //������в���
	std::condition_variable exitCond_;  //�̳߳�����ʱ�����߳�ͨ��

	PoolMode poolMode_;  //�̳߳ع���ģʽ
	bool isPoolRunning_;  //��ʾ�̳߳��Ƿ�����

};
#endif