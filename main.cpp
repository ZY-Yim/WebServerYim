#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "locker.h"
#include "threadpool.h"
#include "sqlconnpool.h"
#include "sqlconnRAII.h"
#include "http_conn.h"
#include "lst_timer.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000
#define TIMESLOT 5             //最小超时单位

extern int addfd(int epollfd, int fd, bool one_shot);
extern int removefd( int epollfd, int fd );
extern int setnonblocking( int fd );

static int epollfd = 0;
static int pipefd[2];
// 定时器链表
static sort_lst_timer lst_timer;

// 信号处理函数
void sig_handler(int sig){
    // 保留原来的errno，在函数最后恢复，保证函数的可重入性
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

// 设置信号处理函数
void addsig( int sig, void( handler )(int), bool restart = true )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    if( restart )
    {
        sa.sa_flags |= SA_RESTART;
    }
    // 在信号集中设置所有的信号
    sigfillset( &sa.sa_mask );
    // 设置信号处理函数
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

// 定时处理任务
void timer_hander(){
    lst_timer.tick();
    // 5s产生一个alarm信号
    alarm(TIMESLOT);
}

// 回调函数,删除非活动连接在socket上的事件，并关闭
void cb_func(client_data* user_data){
    cout << "delete sockfd timer" << endl;
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}

void show_error( int connfd, const char* info )
{
    printf( "%s", info );
    send( connfd, info, strlen( info ), 0 );
    close( connfd );
}


/*只负责I/O读写*/
int main( int argc, char* argv[] )
{
    if( argc <= 2 )
    {
        printf( "usage: %s ip_address port_number\n", basename( argv[0] ) );
        return 1;
    }
    const char* ip = argv[1];
    int port = atoi( argv[2] );

    // cout << "123" << endl;
    // 创建sql数据库连接池 
    sqlconnpool* connpool = sqlconnpool::get_instance();
    connpool->init("localhost", "yim", "123456", "WebDB", 3306, 8);
    // cout << "123" << endl;

    // 创建线程池
    threadpool< http_conn >* pool = NULL;
    try
    {
        // 线程池中有一个指向数据库连接池的指针
        pool = new threadpool< http_conn >(connpool);
    }
    catch( ... )
    {
        return 1;
    }
    // 预先为每个可能的客户连接分配一个http_conn对象
    http_conn* users = new http_conn[ MAX_FD ];
    assert( users );
    int user_count = 0;

    // 预先初始化定时器
    client_data* users_timer = new client_data[MAX_FD];

    // 创建监听socket
    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    assert( listenfd >= 0 );
    
    // // SO_LINGER控制close系统调用关闭TCP连接时的行为
    // // 默认情况下,若有数据待发送，TCP把剩余数据发送
    // struct linger tmp = { 1, 0 };   // 异常终止连接，丢弃发送缓冲区中的数据，发送rst报文
    // // 成功返回0，失败返回-1
    // setsockopt( listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof( tmp ) );

    int ret = 0;
    // 设置ip
    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;
    inet_pton( AF_INET, ip, &address.sin_addr );
    address.sin_port = htons( port );

    // 绑定ip-port
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    assert( ret >= 0 );

    ret = listen( listenfd, 5 );
    assert( ret >= 0 );

    // 创建内核时间表
    epoll_event events[ MAX_EVENT_NUMBER ];
    epollfd = epoll_create( 5 );
    assert( epollfd != -1 );
    // 监听socket注册到内核事件表
    addfd( epollfd, listenfd, false );
    // 静态的，初始化
    http_conn::m_epollfd = epollfd;

    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    // 注册管道pipefd[0]上的可读事件
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false);

    // 设置信号处理函数
    // 忽略SIGPIPE信号
    addsig(SIGPIPE, SIG_IGN);   // 往读端被关闭的管道或者socket中写数据
    addsig(SIGALRM, sig_handler, false);    // 实时闹钟
    addsig(SIGTERM, sig_handler, false);    // 终止进程

    bool timeout = false;
    // 开始先alarm
    alarm(TIMESLOT);

    bool stop_server = false;
    while(!stop_server)
    {
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        if ( ( number < 0 ) && ( errno != EINTR ) )
        {
            printf( "epoll failure\n" );
            break;
        }

        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;
            if( sockfd == listenfd )
            {
                cout << "new client" << endl;
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                if ( connfd < 0 )
                {
                    printf( "errno is: %d\n", errno );
                    continue;
                }
                if( http_conn::m_user_count >= MAX_FD )
                {
                    show_error( connfd, "Internal server busy" );
                    continue;
                }
                // 初始化客户端连接
                users[connfd].init( connfd, client_address );
                
                // 初始化client_data
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                util_timer* timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur_time = time(NULL);
                timer->expire = cur_time + 3 * TIMESLOT;
                users_timer[connfd].timer = timer;
                lst_timer.add_timer(timer);                 
            }
            else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) )
            {
                // 如果有异常，直接关闭客户连接
                // users[sockfd].close_conn();
                // 首先调用回调函数，删除注册的socket并且关闭socket，然后移除定时器
                util_timer* timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);
                if(timer) 
                    lst_timer.del_timer(timer);
            }
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)){
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if(ret == -1){
                    continue;
                }
                else if(ret == 0){
                    continue;
                }
                else{
                    for(int i = 0; i < ret; ++i){
                        switch (signals[i])
                        {
                        case SIGALRM:
                            // 说明超时了，接下来要处理
                            timeout = true;
                            cout << "ALARM" << endl;
                            break;
                        case SIGTERM:
                            // kill该进程
                            cout << "TERM" << endl;
                            stop_server = true;
                            break;
                        default:
                            break;
                        }
                    }
                }
            }
            else if( events[i].events & EPOLLIN )
            {
                util_timer* timer = users_timer[sockfd].timer;
                // 根据读的结果，决定是讲任务加入到线程池，还是关闭连接
                if( users[sockfd].read() )
                {
                    pool->append( users + sockfd );
                    
                    // 有数据传输，定时器延后3个TIMESLOT
                    if(timer){
                        time_t cur_time = time(NULL);
                        timer->expire = cur_time + 3 * TIMESLOT;
                        lst_timer.adjust_timer(timer);
                    }
                }
                else
                {
                    // users[sockfd].close_conn();
                    // 关闭客户连接，删除注册的socket事件，关闭sockfd，删除定时器
                    timer->cb_func(&users_timer[sockfd]);
                    if(timer){
                        lst_timer.del_timer(timer);
                    }
                }
            }
            else if( events[i].events & EPOLLOUT )
            {
                // 根据写的结果，决定是否关闭连接
                // 如果write为true表示keep-alive
                // if( !users[sockfd].write() )
                // {
                //     users[sockfd].close_conn();
                // }
                util_timer* timer = users_timer[sockfd].timer;
                if(users[sockfd].write()){
                    // 有数据传输，定时器延后3个TIMESLOT
                    if(timer){
                        time_t cur_time = time(NULL);
                        timer->expire = cur_time + 3 * TIMESLOT;
                        lst_timer.adjust_timer(timer);
                    }
                }
                else{
                    // 关闭客户连接，删除注册的socket事件，关闭sockfd，删除定时器
                    timer->cb_func(&users_timer[sockfd]);
                    if(timer){
                        lst_timer.del_timer(timer);
                    }
                }
            }
        }
        if(timeout){
            // 处理超时，tick一下，然后再次alarm
            timer_hander();
            timeout = false;
        }
    }

    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    cout << "close done!" << endl;
    return 0;
}
