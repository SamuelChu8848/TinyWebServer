  #ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <list>
#include <exception>
#include "locker.h"
#include <cstdio>


//找一个线程处理任务，定义成模板类，为了代码的复用
//不管哪种任务都能复用

template<typename T>
class threadpool{

public:

    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T* request);

private:
    static void * worker(void * arg);
    void run();


private:
    //线程的数量
    int m_thread_number;

    //线程池数组，大小为m_thread_number
    //动态的创建
    pthread_t * m_threads;

    //请求队列，装任务，请求队列等待处理的请求数量
    int m_max_requests;

    //请求队列
    std::list<T*> m_workqueue;

    //互斥锁，因为请求队列所有线程共享，所以需要互斥锁
    locker m_queuelocker;

    //信号量，判断是否有任务需要处理
    sem m_queuestat;

    //是否结束线程
    bool m_stop;

};

template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests) :
    m_thread_number(thread_number), m_max_requests(max_requests), 
    m_stop(false), m_threads(NULL) {
        //两个大小的有效性

        //printf("初始化线程池ing...\n");
        if ((thread_number <= 0) || (max_requests <= 0)){
            throw std::exception();
        }

        //创建线程池
        m_threads = new pthread_t[m_thread_number];  //数组，注意析构销毁
        if(!m_threads){
            throw std::exception();
        }

        //创建线程并设置线程分离，用完自动销毁
        for (int i = 0; i < m_thread_number; i++){  //1、参数1指向pthread_t*  2、worker函数需要是静态函数，规定，线程的回调函数必须是静态函数。
            printf("create the %dth thread\n", i);
            if (pthread_create(m_threads + i, NULL, worker, this) != 0){
                delete [] m_threads;
                throw std::exception();
            }  

            //设置线程分离
            if (pthread_detach(m_threads[i])){
                //不等于0
                delete [] m_threads;
                throw std::exception();
            }
        }

    }

template<typename T>
threadpool<T>::~threadpool(){
    delete [] m_threads;
    m_stop = true;  //线程根据这个值判断要不要结束
}

template<typename T>
bool threadpool<T>::append(T* request){

    m_queuelocker.lock();   //？？？为什么可以不用初始化锁
    if (m_workqueue.size() > m_max_requests){
        m_queuelocker.unlock();
        return false;
    }

    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();  //信号量增加
    return true;
}



template<typename T>
void* threadpool<T>::worker(void* arg){
    threadpool* pool = (threadpool *) arg;
    pool->run();
    return pool;
}

template<typename T>
void threadpool<T>::run(){
    while(!m_stop){
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }

        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        if (!request){
            continue;
        }

        request->process();  //线程类做任务。
    }
}



#endif