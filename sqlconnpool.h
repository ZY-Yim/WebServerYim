#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_
#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include <stdlib.h>
#include <pthread.h>
#include "locker.h"

using namespace std;

class sqlconnpool
{
public:
    // 获取数据库连接
    MYSQL* get_connection();
    // 释放连接
    bool release_connection(MYSQL* conn);
    // 获取连接
    int get_free_conn();
    // 销毁所有连接
    void destroy_pool();

    // 单例模式,本身是安全的
    static sqlconnpool *get_instance();
    
    // 初始化
    void init(string url, string user, string password, string data_base_name, int port, unsigned int max_conn);
    
    sqlconnpool();
    ~sqlconnpool(); 

private:
    string m_url;
    string m_user;
    string m_password;
    string m_data_base_name;
    int m_port;

    locker lock;    // 对连接池进行处理 需要加锁
    sem reverse;
    list<MYSQL*> conn_list;

    unsigned int m_max_conn;
    unsigned int m_free_conn;
    unsigned int m_busy_conn;
};

#endif