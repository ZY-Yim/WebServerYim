# 基于Linux的轻量级Web服务器

## 项目描述
在Linux环境下基于C++语言开发轻量级的多线程Web服务器，服务器支持一定数量的客户端连接并及时响应，返回客户端请求的图片和视频等资源。

## 主要工作
•	使用Epoll边沿触发的IO多路复用+非阻塞IO

•	使用同步IO模拟Proactor事件处理模式

•	使用多线程充分发挥多核CPU的优势，实现了固定线程数的半同步/半反应堆模式的线程池

•	使用基于升序链表的定时器处理超时连接

•	使用有限状态机解析HTTP请求报文，支持GET和POST方法

•	使用MySQL数据库和数据库池，实现客户端注册和登录功能

## 原代码存在的问题
1. 传输大文件时，m_iv结构体不会自动偏移

> 每次调用writev之后进行偏移

2. listenfd为ET触发模式，但在main里没有循环读取

> listenfd设置为LT触发，addfd增加一个参数Trigger



<!-- * 解析了HTTP的get、post请求，支持长短连接
* mime设计为单例模式
* 线程的工作分配为：
    * 主线程负责等待epoll中的事件，并把到来的事件放进任务队列，在每次循环的结束剔除超时请求和被置为删除的时间结点
    * 工作线程阻塞在条件变量的等待中，新任务到来后，某一工作线程会被唤醒，执行具体的IO操作和计算任务，如果需要继续监听，会添加到epoll中  

* 锁的使用有两处：
    * 第一处是任务队列的添加和取操作，都需要加锁，并配合条件变量，跨越了多个线程。
    * 第二处是定时器结点的添加和删除，需要加锁，主线程和工作线程都要操作定时器队列。 -->



