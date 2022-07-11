#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include "locker.h" 
#include "threadpool.h"
#include <signal.h>
#include "http_conn.h"
#include <assert.h>


#define MAX_FD 65535  // 最大文件描述符个数。
#define MAX_EVENTS_NUM 10000   //最大监听数量

#define FD_LIMIT 65535
#define MAX_EVENT_NUMBER 1024
#define TIMESLOT 5

static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;

/*
模拟的是preactor模式，主线程负责所有的IO操作，工作线程只负责逻辑业务。
当监听到读事件的时候，读出来，然后将读到的内容封装成一个任务类。
交给线程池，插入线程队列。然后线程池调用运行。
*/

//添加信号捕捉
void addsig(int sig, void (handler)(int)){
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);  //暂时设为阻塞
    sigaction(sig, &sa, NULL); 
}

//收到信号，设置信号处理函数，发送给监听
void sig_handler( int sig ) {
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}


void cb_func( http_conn* user_data ) {
    epoll_ctl( epollfd, EPOLL_CTL_DEL, user_data->getfd(), 0 );
    assert( user_data );
    close( user_data->getfd() );
    printf( "close fd %d\n", user_data->getfd() );
}

void timer_handler() {
    // 定时处理任务，实际上就是调用tick()函数
    timer_lst.tick();
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}

//添加文件描述符进epoll
//从epoll中删除文件描述符
//修改文件描述符
extern void addfd(int epollfd, int fd, bool one_shot);
extern void removefd(int epollfd, int fd);
extern void modfd(int epollfd, int fd, int ev);
extern int setnonblocking(int fd);


int main(int argc, char* argv[]){

    if (argc <= 1) {
        //至少传一个参数
        printf("按照如下格式运行：%s port_number\n", basename(argv[0]));  //参数是带路径的名称，使用basename获取程序名称
        exit(-1);
    }

    //获取端口号
    int port = atoi(argv[1]); //字符串转int
    
    //对SIGPIE信号进行处理
    addsig(SIGPIPE, SIG_IGN);  //对于终止信号，进行忽略。 防止客户端终止终止服务端 https://blog.csdn.net/weixin_36750623/article/details/91370604

    //创建线程池，初始化线程池。
    threadpool<http_conn> * pool = NULL;  //线程池装的是工作线程，可以处理数据响应请求。
    try{
        pool = new threadpool<http_conn>;
    } catch(...){
        exit(-1);
    }

    //创建数组，保存所有客户端信息。
    http_conn * users = new http_conn[ MAX_FD];
    //检测到客户端来了就增加数组。
    
    //创建监听套接字
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    //判错

    //设置端口复用。对文件描述符操作
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //绑定
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    bind(listenfd, (struct sockaddr*)&address, sizeof(address));

    //监听
    listen(listenfd, 5);

    //使用epoll多路复用
    //创建epoll对象
    epoll_event events[MAX_EVENTS_NUM];
    int epollfd = epoll_create(5);

    //将监听的文件描述符添加进epoll
    addfd(epollfd, listenfd, false);  //允许多个线程同对监听文件描述符，不需要添加oneshot
    http_conn::m_epollfd = epollfd;

    //upadate:创建管道
    int piperet = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( piperet != -1 );
    setnonblocking( pipefd[1] );
    addfd( epollfd, pipefd[0], true);  //设置了单次命中

    //设置信号处理函数
    addsig(SIGALRM, sig_handler);
    addsig(SIGTERM, sig_handler);
    bool stop_server = false;

    //初始化检测标志，和定时
    bool timeout = false;
    alarm(TIMESLOT);  // 定时,5秒后产生SIGALARM信号

    while ( !stop_server ) {
        //主线程循环检测有没有事件发生
        int num = epoll_wait(epollfd, events, MAX_EVENTS_NUM, -1);
        if (num < 0 && errno != EINTR ){  //产生信号会中断，
            // printf("epoll failure\n");
            break;  //中断了
        } 

        //循环遍历事件数组
        for (int i = 0; i < num; i++){
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd){
                //有客户端连接进来
                
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlen);
                //需要判断是不是连接成功
                // printf("有新连接连接进来 fd %d\n", connfd);

                if (http_conn::m_user_count >= MAX_FD){
                    //目前连接满了
                    //给客户端写一个信息，服务器内部正忙。
                    close(connfd);
                    continue;
                }

                //将客户端信息放进数组。直接替换
                users[connfd].init(connfd, client_address);
                
                //创建个定时器，设置回调函数和超时事件，绑定到用户上，并加入链接中。
                util_timer* timer = new util_timer;
                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
                time_t cur = time( NULL );
                timer->expire = cur + 3 * TIMESLOT;
                users[connfd].timer = timer;
                timer_lst.add_timer( timer );

            } else if ( ( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN ) ) {
                //有信号了，处理信号
                int sig;
                char signals[1024];
                int recvret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if (recvret == -1 || recvret == 0){
                    continue;
                } else {
                    for (int i = 0; i < recvret; ++i){
                        switch ( signals[i] ){
                            case SIGALRM: {
                                //延迟处理，因为IO优先级更高
                                timeout = true;
                                break;
                            }
                            case SIGTERM: {
                                stop_server = true;
                            }
                        }
                    }
                }
            } else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                //对方异常断开或者错误等事件
                util_timer* timer = users[sockfd].timer; //为删除做准备
                if (timer){
                    timer_lst.del_timer(timer);
                }
                users[sockfd].close_conn();

            } else if (events[i].events & EPOLLIN){  //考察读
                
                if (users[sockfd].read()){
                    util_timer* timer = users[sockfd].timer; //为删除做准备
                    // 如果某个客户端上有数据可读，则我们要调整该连接对应的定时器，以延迟该连接被关闭的时间。
                    if( timer ) {
                        time_t cur = time( NULL );
                        timer->expire = cur + 3 * TIMESLOT;
                        printf( "adjust timer once\n" );
                        timer_lst.adjust_timer( timer );
                    }
                    //一次性都读完
                    pool->append(users + sockfd);  //将http_conn指针传入工作线程，线程池。
                } else {
                    util_timer* timer = users[sockfd].timer; //为删除做准备
                    if (timer){
                        timer_lst.del_timer(timer);
                    }
                    users[sockfd].close_conn();
                }
            } else if (events[i].events & EPOLLOUT){
                
                if (!users[sockfd].write()) {  //成功了就完事了。connnection closed 就会主动关闭连接
                    // printf("****发送成功******\n");
                    users[sockfd].close_conn();
                }
            }
        }
        //处理定时事件
        if( timeout ) {
            timer_handler();
            timeout = false;
        }
    }

    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete [] users;
    delete pool;

    return 0;
}