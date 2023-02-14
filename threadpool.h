#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
// 线程同步机制的包装类
#include "locker.h"

// 线程池类，定义为模板类
// 半同步 半反应堆模式
template< typename T >
class threadpool
{
public:
    /*
     * thread_number 线程池中线程的数量，
     * max_requests 请求队列中最多允许的，等待的请求的数量
     */
    threadpool( int thread_number = 8, int max_requests = 10000 );
    ~threadpool();
    // 往请求队列中添加任务
    bool append( T* request );

private:
    // 工作线程运行的函数，不断从工作队列中取出任务并执行
    static void* worker( void* arg );
    void run();

private:
    int m_thread_number;        // 线程数
    int m_max_requests;         // 最大请求数
    pthread_t* m_threads;       // 线程池数组，大小为m_thread_number
    std::list< T* > m_workqueue;    // 请求队列
    locker m_queuelocker;       // 保护请求队列的互斥锁
    sem m_queuestat;            // 是否有任务需要处理
    bool m_stop;                // 是否结束线程
};

// 构造函数
template< typename T >
threadpool< T >::threadpool( int thread_number, int max_requests ) : 
        m_thread_number( thread_number ), m_max_requests( max_requests ), m_stop( false ), m_threads( NULL )
{
    if( ( thread_number <= 0 ) || ( max_requests <= 0 ) )
    {
        throw std::exception();
    }
    // create threadpool arrays
    m_threads = new pthread_t[ m_thread_number ];
    if( ! m_threads )
    {
        throw std::exception();
    }
    // create thread_number threads, 设置为脱离线程
    for ( int i = 0; i < thread_number; ++i )
    {
        printf( "create the %dth thread\n", i );
        // 创建成功返回0
        if( pthread_create( m_threads + i, NULL, worker, this ) != 0 )
        {
            delete [] m_threads;
            throw std::exception();
        }
        //pthread_join: 线程结束后等待其他线程调用pthread_join，不释放资源
        //pthread_detach：线程结束后直接释放资源，不可以等待它结束
        // 调用成功返回0
        if( pthread_detach( m_threads[i] ) )
        {
            delete [] m_threads;
            throw std::exception();
        }
    }
}

// 析构函数
template< typename T >
threadpool< T >::~threadpool()
{
    delete [] m_threads;
    m_stop = true;
}

// 往请求队列中添加任务
template< typename T >
bool threadpool< T >::append( T* request )
{   
    // 操作工作队列一定要加锁，因为他被所有线程共享
    m_queuelocker.lock();
    if ( m_workqueue.size() > m_max_requests )
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back( request );
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template< typename T >
void* threadpool< T >::worker( void* arg )
{
    threadpool* pool = ( threadpool* )arg;
    pool->run();
    return pool;
}

// 竞争获取请求处理
template< typename T >
void threadpool< T >::run()
{
    while ( ! m_stop )
    {
        m_queuestat.wait();
        m_queuelocker.lock();
        if ( m_workqueue.empty() )
        {
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if ( ! request )
        {
            continue;
        }
        request->process();
    }
}

#endif
