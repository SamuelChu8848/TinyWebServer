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
    sigfillset(&sa.sa_mask); 
    sigaction(sig, &sa, NULL); 
}

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
    timer_lst.tick();
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
        printf("按照如下格式运行：%s port_number\n", basename(argv[0])); 
        exit(-1);
    }

    int port = atoi(argv[1]); 
    addsig(SIGPIPE, SIG_IGN);  //对于终止信号，进行忽略。 防止客户端终止终止服务端 https://blog.csdn.net/weixin_36750623/article/details/91370604

    threadpool<http_conn> * pool = NULL;
    try{
        pool = new threadpool<http_conn>;
    } catch(...){
        exit(-1);
    }

    http_conn * users = new http_conn[ MAX_FD];
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //绑定
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    listen(listenfd, 5);

    epoll_event events[MAX_EVENTS_NUM];
    int epollfd = epoll_create(5);
    addfd(epollfd, listenfd, false);  
    http_conn::m_epollfd = epollfd;

    //upadate:创建管道
    int piperet = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( piperet != -1 );
    setnonblocking( pipefd[1] );
    addfd( epollfd, pipefd[0], true); 

    //设置信号处理函数
    addsig(SIGALRM, sig_handler);
    addsig(SIGTERM, sig_handler);
    bool stop_server = false;

    bool timeout = false;
    alarm(TIMESLOT);  

    while ( !stop_server ) {
        //主线程循环检测有没有事件发生
        int num = epoll_wait(epollfd, events, MAX_EVENTS_NUM, -1);
        if (num < 0 && errno != EINTR ){  
            break; 
        } 

        //循环遍历事件数组
        for (int i = 0; i < num; i++){
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd){
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlen);
                if (http_conn::m_user_count >= MAX_FD){
                    close(connfd);
                    continue;
                }

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
                //处理信号
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
                util_timer* timer = users[sockfd].timer; 
                if (timer){
                    timer_lst.del_timer(timer);
                }
                users[sockfd].close_conn();
            } else if (events[i].events & EPOLLIN){ 
                
                if (users[sockfd].read()){
                    util_timer* timer = users[sockfd].timer;
                    if( timer ) {
                        time_t cur = time( NULL );
                        timer->expire = cur + 3 * TIMESLOT;
                        printf( "adjust timer once\n" );
                        timer_lst.adjust_timer( timer );
                    }
                    pool->append(users + sockfd);  //将http_conn指针传入工作线程，线程池。
                } else {
                    util_timer* timer = users[sockfd].timer; 
                    if (timer){
                        timer_lst.del_timer(timer);
                    }
                    users[sockfd].close_conn();
                }
            } else if (events[i].events & EPOLLOUT){
                if (!users[sockfd].write()) {  
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