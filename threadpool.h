  #ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <list>
#include <exception>
#include "locker.h"
#include <cstdio>

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
    
    int m_thread_number;  //线程的数量
    pthread_t * m_threads;
    int m_max_requests;
    std::list<T*> m_workqueue;
    locker m_queuelocker;
    sem m_queuestat;
    bool m_stop;

};

template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests) :
    m_thread_number(thread_number), m_max_requests(max_requests), 
    m_stop(false), m_threads(NULL) {
        if ((thread_number <= 0) || (max_requests <= 0)){
            throw std::exception();
        }

        //创建线程池
        m_threads = new pthread_t[m_thread_number]; 
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

            if (pthread_detach(m_threads[i])){
                delete [] m_threads;
                throw std::exception();
            }
        }
    }

template<typename T>
threadpool<T>::~threadpool(){
    delete [] m_threads;
    m_stop = true; 
}

template<typename T>
bool threadpool<T>::append(T* request){
    m_queuelocker.lock(); 
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
void* threadpool<T>::worker(void* arg) {
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