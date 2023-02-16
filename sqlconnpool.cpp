#include <mysql/mysql.h>
#include "sqlconnpool.h"
#include "locker.h"

using namespace std;

sqlconnpool::sqlconnpool(){
    this->m_busy_conn = 0;
    this->m_free_conn = 0;
}

// 获取单例
sqlconnpool* sqlconnpool::get_instance(){
    static sqlconnpool connpool;
    return &connpool;
}

void sqlconnpool::init(string url, string user, string password, string data_base_name, int port, unsigned int max_conn){
    this->m_url = url;
    this->m_user = user;
    this->m_password = password;
    this->m_data_base_name = data_base_name;
    this->m_port = port;
    this->m_max_conn = max_conn;

    // 加锁
    lock.lock();
    for(int i = 0; i < m_max_conn; ++i){
        MYSQL* conn = NULL;
        conn = mysql_init(conn);
        if(conn == NULL){
            exit(1);
        }
        conn = mysql_real_connect(conn, m_url.c_str(), m_user.c_str(), m_password.c_str(), m_data_base_name.c_str(), port, NULL, 0);
        // cout << "1234" << endl;
        if(conn == NULL){
            // cout << "123" << endl;
            exit(1);
        }
        // cout << "123" << endl;
        conn_list.push_back(conn);
        ++m_free_conn;
    }

    reverse = sem(m_free_conn);
    this->m_max_conn = m_free_conn;
    lock.unlock();
}

// 有请求时,获取一个连接
MYSQL* sqlconnpool::get_connection(){
    MYSQL* conn = NULL;
    if(conn_list.size() == 0){
        return NULL;
    }
    reverse.wait();
    lock.lock();
    conn = conn_list.front();
    conn_list.pop_front();
    --m_free_conn;
    ++m_busy_conn;
    lock.unlock();
    return conn;
}

// 释放当前连接
bool sqlconnpool::release_connection(MYSQL* conn){
    // cout << "release" << endl;
    if(conn == NULL) return false;
    lock.lock();
    conn_list.push_back(conn);
    ++m_free_conn;
    --m_busy_conn;
    lock.unlock();
    reverse.post();
    return true;
}

// 销毁连接池
void sqlconnpool::destroy_pool(){
    lock.lock();
    if(conn_list.size() > 0){
        list<MYSQL*>::iterator it;
        for(it = conn_list.begin(); it != conn_list.end(); ++it){
            MYSQL* conn = *it;
            mysql_close(conn);
        }
        m_busy_conn = 0;
        m_free_conn = 0;
        conn_list.clear();
        lock.unlock();
    }
    lock.unlock();
}

// 返回当前空闲的连接数
int sqlconnpool::get_free_conn(){
    return this->m_free_conn;
}

sqlconnpool::~sqlconnpool(){
    destroy_pool();
}
