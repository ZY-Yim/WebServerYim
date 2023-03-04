#ifndef LST_TIMER
#define LST_TIMER

#include <iostream>
#include <time.h>
#include <netinet/in.h>

using namespace std;

// 需要先声明，否则下面的结构体找不到该类
class util_timer;

// 用户数据
struct client_data
{
    // 客户端地址
    sockaddr_in address;
    int sockfd;
    // 定时器
    util_timer* timer;
};

// 定时器类
class util_timer
{
public:
    util_timer():prev(NULL), next(NULL){};

public:
    // 超时事件,绝对事件
    time_t expire;
    // 回调函数
    void (*cb_func)(client_data*);
    // 处理的客户数据
    client_data* user_data;
    util_timer* prev;   // 前一个定时器
    util_timer* next;   // 后一个定时器
};

// 定时器链表
class sort_lst_timer
{
public:
    sort_lst_timer() : head(NULL), tail(NULL){}
    // 销毁所有定时器
    ~sort_lst_timer(){
        util_timer* tmp = head;
        while(tmp){
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }
    // 讲目标定时器添加到链表
    void add_timer(util_timer* timer){
        if(!timer) return;
        if(!head){
            head = tail = timer;
            return;
        }
        // 如果目标定时器的超时时间小于当前链表中所有定时器的超时时间
        // 则把他插入到链表头部,作为新的头节点
        // 否则调用重载函数,把他插入到合适的位置
        if(timer->expire < head->expire){
            timer->next = head;
            head->prev = timer;
            head = timer;
            return;
        }
        add_timer(timer, head);
    }
    // 定时器事件发生变化，时间延长，往链表后面移动
    void adjust_timer(util_timer* timer){
        if(!timer) return;
        util_timer* tmp = timer->next;
        // 如果被调整的定时器刚好在尾部或者下一个定时器的超时值依然大于该定时器调整之后的
        if(!tmp || (tmp->expire > timer->expire)){
            return;
        }
        // 如果是头节点，则将其重新取出并插入
        if(timer == head){
            head = head->next;
            head->prev = NULL;
            timer->next = NULL;
            add_timer(timer);
        }
        // 如果不是头节点，将其取出并重新插入
        else{
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            // 并不需要从头开始
            add_timer(timer, timer->next);
        }
    }
    // 删除目标定时器
    void del_timer(util_timer* timer){
        if(!timer) return;
        // 如果只有一个定时器
        if((head == timer) && (tail == timer)){
            delete timer;
            head = NULL;
            tail = NULL;
            return;
        }
        // 如果至少有两个定时器，且为头节点，头节点重置为头节点的next节点
        if(head == timer){
            head = head->next;
            head->prev = head;
            delete timer;
            return;
        }
        // 如果至少有两个定时器，且为尾节点，尾节点重置为尾节点的prev节点
        if(tail == timer){
            tail = tail->prev;
            tail->next = NULL;
            delete timer;
            return;
        }
        // 中间,删除
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
        return;
    }
    // 处理链表上的到期任务
    void tick(){
        if(!head) return;
        // cout << "time tick" << endl;
        time_t cur_time = time(NULL);
        util_timer* tmp = head;
        while(tmp){
            // 直到找到一个未到期的任务
            if(cur_time < tmp->expire){
                break;
            }
            tmp->cb_func(tmp->user_data);
            head = head->next;
            if(head) head->prev = NULL;
            delete tmp;
            tmp = head;
        }
    }

private:
    // 重载函数
    // 将目标定时器插入到合适的位置
    void add_timer(util_timer* timer, util_timer* lst_head){
        util_timer* pre = lst_head;
        util_timer* tmp = pre->next;

        // 遍历lst_head之后的节点
        // 找到一个超时时间大于目标定时器的节点,插入到该节点之前
        while(tmp){
            if(timer->expire < tmp->expire)
            {
                pre->next = timer;
                timer->next = tmp;
                tmp->prev = timer;
                timer->prev = pre;
                return;
            }
            pre = tmp;
            tmp = pre->next;
        }
        // 还没找到,插入到尾部
        if(!tmp){
            pre->next = timer;
            timer->prev = pre;
            timer->next = NULL;
            tail = timer;
        }
    }   

private:
    util_timer* head;
    util_timer* tail;
};

#endif