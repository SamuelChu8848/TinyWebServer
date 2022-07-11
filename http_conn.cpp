#include "http_conn.h"

// 网站的根目录
const char* doc_root = "/root/newcoder/webserver/resourses";

int http_conn:: m_epollfd = -1; //所有socket上的事件，都被注册到同一个epoll中
int http_conn::m_user_count = 0;  //统计用户数量

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";


//设置文件描述符非阻塞
int setnonblocking(int fd){
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
    return old_flag;
}

//向epoll中添加需要监听的文件描述符
void addfd(int epollfd, int fd, bool one_shot){
    epoll_event event;  //epoll ctl用
    // event.events = EPOLLIN | EPOLLRDHUP;  //判断写入，异常断开会发送一个EPOLIN EPOLLRDHUP，可以直接判断异常断开。
    event.events = EPOLLIN  | EPOLLRDHUP;  //| EPOLLET
    event.data.fd = fd;

    if (one_shot){
        event.events | EPOLLONESHOT;  //一次命中
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //用的边沿触发。需要设置非阻塞的。
    setnonblocking(fd);

}


//移除
void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}


//修改文件描述符状态，重置socket上EPOLLONESHOT事件，确保下次可读仍能够触发
void modfd(int epollfd, int fd, int ev){
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event); 
}

int http_conn::getfd(){
    return this->m_sockfd;
}

//初始化连接
void http_conn::init(int sockfd, const sockaddr_in & addr){
    m_sockfd = sockfd;
    m_address = addr;

    //设置sockfd的端口复用。
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //添加到epoll
    addfd(m_epollfd, sockfd, true);
    m_user_count++;  //总用户数+1。

    init(); //初始化解析客户端请求的状态设置
}

//初始化解析客户端请求的状态设置
void http_conn::init(){

    bytes_to_send = 0;
    bytes_have_send = 0;  //发送标志
    
    m_check_state = CHECK_STATE_REQUESTLINE; //初始化状态为解析请求首行
    m_linger = false;  //默认不保持链接  Connection : keep-alive保持连接

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;  //响应体长度初始化
    m_host = 0;

    m_checked_index = 0;  //解析的字符位置
    m_start_line = 0; //解析的行的索引
    m_read_idx = 0; // 读的
    m_write_idx = 0;
    
    bzero(m_read_buf, READ_BUFFER_SIZE);  //读缓冲置为0
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);

}

//关闭连接
void http_conn::close_conn(){
    if (m_sockfd != -1){
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;  //总数量-1。
    }
}

//读数据
//循环读取
bool http_conn::read(){
    if (m_read_idx >= READ_BUFFER_SIZE){
        return false;
    }

    //读取到的字节
    int bytes_read = 0;
    while (true) {  //循环的读，循环的存。每次从上一次存储
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1){
            if (errno == EAGAIN || errno == EWOULDBLOCK){
                //没有数据了，退出循环
                break;
            }
            return false;
        } else if (bytes_read == 0){
            //对方关闭链接
            return false;
        } 
        m_read_idx += bytes_read;
    }
    // printf("读到的数据：%s\n", m_read_buf);
    

    return true;
}


//主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read(){  //解析HTTP请求，里面交给具体的某一个
    LINE_STATUS line_status = LINE_OK;  //定义一个初始状态
    HTTP_CODE ret = NO_REQUEST;
    char * text = 0; 
    while ( ((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
    || (line_status = parse_line()) == LINE_OK) {  //解析一行数据，解析成功, 注：先将\0\0截断\r\n，然后用getline读取到text里，然后更新行开头。
        //没定位到新行，就不会乱读，说明没有传输完成。
        //解析到一行数据或者解析到请求体，也是完成的数据。
        //07.01 先用parse_line修改换成，定位新行，然后读取本行数据，将新行位置更新。然后解析
        //获取一行数据
        text = getline();
        m_start_line = m_checked_index;   //更新行开头到\0\0下一位，方便下次读取。
        printf("got 1 http line : %s \n", text);

        switch(m_check_state){
            case CHECK_STATE_REQUESTLINE:{
                // printf("解析请求行ing\n");
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST){  //语法错误直接结束
                    return BAD_REQUEST;
                    
                } 
                break;  //正常解析
            }
            case CHECK_STATE_HEADER: {
                // printf("解析请求头ing\n");
                // printf("传入字符串%s\n", text);
                ret = parse_headers(text);
                // printf("出来后主状态机%d 返回值为%d\n", m_check_state, ret);
                if (ret == BAD_REQUEST){
                    return BAD_REQUEST;  
                } else if (ret == GET_REQUEST){  //如果得到完整的请求。  
                    return do_request();  //完成请求头的解析，解析具体的请求信息。
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                // printf("解析请求体ing\n");
                ret = parse_content(text);
                if (ret == GET_REQUEST){
                    return do_request();
                }
                line_status = LINE_OPEN;  //如果上面没有成功返回，行数据不完整
                break;
            }
            default: {
                return INTERNAL_ERROR;  //内部错误
            }
        }
        // printf("over~\n");
    }
    return NO_REQUEST;
}

//解析HTTP请求行，获得请求方法，目标url，HTTP版本
http_conn::HTTP_CODE http_conn::parse_request_line(char * text) { //解析HTTP请求首行
    //GET /index.html  HTTP/1.1
    m_url = strpbrk(text, " \t");  //找到tab，
    if (!m_url){
        // printf("-------%s %s\n", m_url, text);
        return BAD_REQUEST;  
    }
    //GET\0/index.html  HTTP/1.1
    *m_url++ = '\0';
    char* method = text;
    if (strcasecmp(method, "GET") == 0){  //忽略大小写比较
        m_method = GET;
    } else {
        // printf("-------%d\n", 2);
        return BAD_REQUEST;
    }
    // /index.html  HTTP/1.1
    // 继续定位tab
    m_version = strpbrk(m_url, " \t");
    if (!m_version){
        // printf("-------%d\n", 3);
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    if (strcasecmp(m_version, "HTTP/1.0") != 0 && strcasecmp(m_version, "HTTP/1.1") != 0){
        // printf("-------%d\n", 4);
        return BAD_REQUEST;
    }
    //处理url
    if (strncasecmp(m_url, "http://", 7) == 0){
        m_url += 7;
        //获取请求的页面文件
        m_url = strchr(m_url, '/');  //在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
    }
    if (!m_url || m_url[0] != '/'){
        // printf("-------%d\n", 5);
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER;  //转而进入请求头的解析
    return NO_REQUEST;


}

http_conn::HTTP_CODE http_conn::parse_headers(char * text) { //解析HTTP请求头
    // 遇到空行，表示头部字段解析完毕
    // printf("* 请求头\n");
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );  //07.01 第一个不是\t的字符下标。
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } else {
        // printf( "oop! unknow header %s\n", text );
    }
    return NO_REQUEST;
}


// 没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content( char* text ) {
    if ( m_read_idx >= ( m_content_length + m_checked_index ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//解析一行，判断依据就是/r/n
//0702注释，一次解析到\r\n处就停止。
http_conn::LINE_STATUS http_conn::parse_line(){ //解析某一行
    char temp;
    for ( ; m_checked_index < m_read_idx; ++m_checked_index){
        //遍历一行数据
        temp = m_read_buf[m_checked_index];
        if (temp == '\r'){
            if ((m_checked_index + 1) == m_read_idx){  //  \r后面就没有了，只读到这里，所以就不完整
                return LINE_OPEN;
            } else if (m_read_buf[m_checked_index + 1] == '\n'){
                m_read_buf[m_checked_index++] = '\0';    //发现了\r\n，就把\r变成字符串结束符
                m_read_buf[m_checked_index++] = '\0';    //发现了\r\n，就把\n变成字符串结束符
                return LINE_OK;
            }
            return LINE_BAD;  //出现\r不是没有读完，且不是\r\n成双出现，就是语法有问题。
        } else if (temp == '\n') {  //防止错开\r导致漏算
            if ((m_checked_index > 1) && (m_read_buf[m_checked_index - 1] == '\r')){
                m_read_buf[m_checked_index - 1] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;  //没出现\r和\n，那么就是读取不完全 
}

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request(){

    // "/home/nowcoder/webserver/resources"
    strcpy( m_real_file, doc_root );   //把路径的完整信息放进去
    int len = strlen( doc_root );
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    // stat用法https://blog.csdn.net/qq_40839779/article/details/82789217
    if ( stat( m_real_file, &m_file_stat ) < 0 ) {  //同时把文件信息房子啊m_file stat上
        return NO_RESOURCE;
    }

    //判断访问权限
    if (!(m_file_stat.st_mode & S_IROTH)){
        return FORBIDDEN_REQUEST;   //通过检查其他用户可读权限，返回用户没有足够的权限读取
    }
    
    //判断是不是目录 S_ISDIR ()函数的作用是判断一个路径是不是目录
    if(S_ISDIR(m_file_stat.st_mode)){
        return BAD_REQUEST;
    }

    //以只读方式打开文件,打开文件映射到某个内存中，然后在内存中，再发送。
    int fd = open(m_real_file, O_RDONLY);

    //创建内存映射
    m_file_address = (char *) mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;  //获取到文件
}

//对映射的内存释放掉
void http_conn::unmap(){
    if (m_file_address){
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

//整个处理的请求就结束了


bool http_conn::write(){
    // printf("\n-----调用write往套接字写------\n");
    int temp = 0;
    
    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }
    // printf("待写入字节大小: %d\n", bytes_to_send);
    while(1) {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);  //https://www.likecs.com/show-204564457.html

        // printf("本轮写的返回值%d\n", temp);
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_have_send >= m_iv[0].iov_len)  //一把没写完，为下次接着写做准备
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        if (bytes_to_send <= 0)
        {
            // 没有数据要发送了
            // printf("发完了，待发送大小为%d\n", bytes_to_send);
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger) {  //长连接全部初始化，模拟新连接
                init();
                return true;
            } else {    //没有长连接，无需如此麻烦
                return false;
            }
        }

    }    
    return true;
}


// 往写缓冲中写入待发送的数据
//va_list 可变参数列表 https://blog.csdn.net/weixin_40332490/article/details/105306188 初步了解
//用法 https://blog.csdn.net/qq_35905572/article/details/110160137?spm=1001.2101.3001.6650.1&utm_medium=distribute.pc_relevant.none-task-blog-2%7Edefault%7EBlogCommendFromBaidu%7Edefault-1-110160137-blog-105306188.pc_relevant_vip_default&depth_1-utm_source=distribute.pc_relevant.none-task-blog-2%7Edefault%7EBlogCommendFromBaidu%7Edefault-1-110160137-blog-105306188.pc_relevant_vip_default&utm_relevant_index=2
// vsprintf用法 https://www.runoob.com/cprogramming/c-function-vsprintf.html
//vnsprintf https://baike.baidu.com/item/_vsnprintf/5395011?fr=aladdin
bool http_conn::add_response( const char* format, ... ) {
    if( m_write_idx >= WRITE_BUFFER_SIZE ) {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

bool http_conn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger(){
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line() {
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

bool http_conn::add_content( const char* content ) {  //添加空行
    return add_response( "%s", content );
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;  //缓冲区内容，响应行 响应头
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;  //响应体  将两个不连续的缓存连续发送
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;

            bytes_to_send = m_write_idx + m_file_stat.st_size;  //总共的大小
            // printf("准备响应内容成功\n");
            return true;
        default:
            return false;
    }
    //如果不是200，只发送响应行和响应头  
    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    // printf("准备响应内容成功\n");
    return true;
}

//处理http请求的入口函数
void http_conn::process(){
    //解析请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST){
        //请求不完整，继续监听
        // printf("请求不完整，解析时返回NO_REQUEST\n");
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return ; //线程结束，变成空闲
    }

    // printf("开始准备响应内容\n");

    //生成响应
    bool write_ret = process_write(read_ret);
    if (!write_ret){
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT) ;  //设定为检测可写  
}