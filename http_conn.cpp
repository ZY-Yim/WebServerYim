#include "http_conn.h"

// 定义http响应的状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站根目录
const char* doc_root = "/home/yim/WorkSpace/WebServerYim/resources";

// 将文件描述符设置为非阻塞的
int setnonblocking( int fd )
{
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd( int epollfd, int fd, bool one_shot )
{
    epoll_event event;
    event.data.fd = fd;
    // 数据可读 | ET触发 | TCP连接被对方关闭，或者对方关闭了写操作
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if( one_shot )
    {
        event.events |= EPOLLONESHOT;
    }
    // 操作内核事件表
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking( fd );
}

void removefd( int epollfd, int fd )
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close( fd );
}

// 修改为EPOLLONESHOT
void modfd( int epollfd, int fd, int ev )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

// 关闭连接
void http_conn::close_conn( bool real_close )
{
    if( real_close && ( m_sockfd != -1 ) )
    {
        //modfd( m_epollfd, m_sockfd, EPOLLIN );
        removefd( m_epollfd, m_sockfd );
        m_sockfd = -1;
        m_user_count--; // 关闭连接，客户端数量-1
    }
}

// 初始化连接
void http_conn::init( int sockfd, const sockaddr_in& addr )
{
    m_sockfd = sockfd;
    m_address = addr;
    int error = 0;
    socklen_t len = sizeof( error );
    getsockopt( m_sockfd, SOL_SOCKET, SO_ERROR, &error, &len );
    int reuse = 1;
    setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    addfd( m_epollfd, sockfd, true );
    m_user_count++;

    init();
}

void http_conn::init()
{
    mysql = NULL;
    
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset( m_read_buf, '\0', READ_BUFFER_SIZE );
    memset( m_write_buf, '\0', WRITE_BUFFER_SIZE );
    memset( m_real_file, '\0', FILENAME_LEN );
}

// 从状态机,解析一行内容
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    /**
     * m_read_idx 指向buffer中客户数据尾部的下一字节
     * m_checked_idx 指向当前正在分析的字节
    */
    for ( ; m_checked_idx < m_read_idx; ++m_checked_idx )
    {
        temp = m_read_buf[ m_checked_idx ];
        // 如果当前是回车，可能读到一个完整的行
        if ( temp == '\r' )
        {
            // 如果读到最后一个字节，这次没有读到一个完整的行
            if ( ( m_checked_idx + 1 ) == m_read_idx )
            {
                return LINE_OPEN;
            }
            // 如果下一个字节的'\n',说明读到一个完整的行
            else if ( m_read_buf[ m_checked_idx + 1 ] == '\n' )
            {
                // '\r\n'替换为'\0'，返回LINE_OK
                m_read_buf[ m_checked_idx++ ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            // 否则，HTTP请求存在语法问题
            return LINE_BAD;
        }
        // 如果当前是'\n'，说明也有可能读到一个完整的行
        // 刚好'\r'在末尾和'\n'分开
        else if( temp == '\n' )
        {
            if( ( m_checked_idx > 1 ) && ( m_read_buf[ m_checked_idx - 1 ] == '\r' ) )
            {
                m_read_buf[ m_checked_idx-1 ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }

    return LINE_OPEN;
}

// 循环读取
bool http_conn::read()
{
    if( m_read_idx >= READ_BUFFER_SIZE )
    {
        return false;
    }

    int bytes_read = 0;
    // 循环读取，直到遇到EAGAIN错误
    // ET模式
    while( true )
    {
        bytes_read = recv( m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0 );
        if ( bytes_read == -1 )
        {
            if( errno == EAGAIN || errno == EWOULDBLOCK )
            {
                break;
            }
            return false;
        }
        else if ( bytes_read == 0 )
        {
            // 客户端关闭了连接？
            return false;
        }

        m_read_idx += bytes_read;
    }
    return true;
}

// 解析请求行，获取请求方法，目标url，http版本号
http_conn::HTTP_CODE http_conn::parse_request_line( char* text )
{
    /**
     * GET http://www.baidu.com/index.html HTTP/1.1
    */

    // 检索字符串 str1 中第一个匹配字符串 str2 中字符的字符，不包含空结束字符
    m_url = strpbrk( text, " \t" );     // m_url = " http://www.baidu.com/index.html HTTP/1.1"
    // 如果请求行中没有空白字符或者'\t'字符，则请求有问题
    if ( ! m_url )
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';        // 去掉空格 m_url = "http://www.baidu.com/index.html HTTP/1.1"

    // 因为设置为'\0',text就等于"GET"
    // char* 字符串 以 “\0”结尾。
    char* method = text;    // text = "GET"
    // 若参数s1和s2字符串相等返回0，s1大于s2则返回大于0的值，s1小于s2则返回小于0的值
    // 忽视大小写
    if ( strcasecmp( method, "GET" ) == 0 )
    {
        m_method = GET;
    }
    else if( strcasecmp( method, "POST" ) == 0 )
    {
        m_method = POST;
    }
    else
    {
        return BAD_REQUEST;
    }

    // 该函数返回 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_url += strspn( m_url, " \t" );        // m_url = "http://www.baidu.com/index.html HTTP/1.1"
    m_version = strpbrk( m_url, " \t" );    // m_version = " HTTP/1.1"
    if ( ! m_version )
    {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';                        // m_version = "HTTP/1.1", m_url = "http://www.baidu.com/index.html"
    m_version += strspn( m_version, " \t" );    // m_version = "HTTP/1.1"
    if ( strcasecmp( m_version, "HTTP/1.1" ) != 0 )
    {
        return BAD_REQUEST;
    }

    if ( strncasecmp( m_url, "http://", 7 ) == 0 )  // m_url = "http://www.baidu.com/index.html"
    {           
        m_url += 7;                                 // m_url = "www.baidu.com/index.html"
        // 该函数返回在字符串 str 中第一次出现字符 c 的位置，如果未找到该字符则返回 NULL。
        m_url = strchr( m_url, '/' );               // m_url = "/index.html"
    }   

    if ( ! m_url || m_url[ 0 ] != '/' )
    {
        return BAD_REQUEST;
    }
    // 当url为/时,返回默认文件
    if(strlen(m_url) == 1)
        strcat(m_url, "judge.html");    // m_url = "/judge.html"

    // 状态转移
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析头部信息
http_conn::HTTP_CODE http_conn::parse_headers( char* text )
{
    // 遇到空行,解析完毕
    if( text[ 0 ] == '\0' )
    {
        // 如果有消息体,还需要读取m_content_length字节的消息体,状态转移到CHECK_STATE_CONTENT
        if ( m_method == HEAD )
        {
            return GET_REQUEST;
        }

        if ( m_content_length != 0 )
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }

        return GET_REQUEST;
    }
    // 处理Connection头部字段
    else if ( strncasecmp( text, "Connection:", 11 ) == 0 )
    {
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 )
        {
            m_linger = true;
        }
    }
    // 处理Content-Length字段
    else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 )
    {
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol( text );
    }
    // 处理Host字段
    else if ( strncasecmp( text, "Host:", 5 ) == 0 )
    {
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    }
    else
    {
        printf( "oop! unknow header %s\n", text );
    }

    return NO_REQUEST;

}

// 并没有真正解析http请求的消息体,只是判断是否被完整读入
// 修改之后，获取post内容
http_conn::HTTP_CODE http_conn::parse_content( char* text )
{
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        // POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }

    return NO_REQUEST;
}

// 主状态机
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    while ( ( ( m_check_state == CHECK_STATE_CONTENT ) && ( line_status == LINE_OK  ) )
                || ( ( line_status = parse_line() ) == LINE_OK ) )
    {
        text = get_line();
        m_start_line = m_checked_idx;       // 下一行真实位置
        printf( "got 1 http line: %s\n", text );

        switch ( m_check_state )
        {
            // 第一个状态 分析请求行
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line( text );
                if ( ret == BAD_REQUEST )
                {
                    return BAD_REQUEST;
                }
                break;
            }
            // 第二个状态 分析头部字段
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers( text );
                if ( ret == BAD_REQUEST )
                {
                    return BAD_REQUEST;
                }
                else if ( ret == GET_REQUEST )
                {
                    return do_request();
                }
                break;
            }
            // 第三个状态,分析主体内容
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content( text );
                if ( ret == GET_REQUEST )
                {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }

    return NO_REQUEST;
}

// 当得到一个完整、正确的http请求时，就分析目标文件的属性，
// 如果目标文件存在,对所有用户可读,且不是目录,则使用mmap将其映射到内存地址m_file_address
// 并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy( m_real_file, doc_root );    // m_real_file = "/var/WebServer/html"
    int len = strlen( doc_root );
    // 返回最后一次出现'/'的位置
    const char *p = strrchr(m_url, '/');    // m_url = "/judge.html"  p = "/judge.html"

    // 处理POST方法
    if(m_method == POST && (*(p + 1) == '2' || *(p + 1) == '3')){
        // 提取用户名和密码
        // user=123&password=123
        // 找到分割符的位置，下标
        int idx = m_string.find('&');
        string name(m_string.begin() + 5, m_string.begin() + idx);
        string password(m_string.begin() + idx + 10, m_string.end());
        // cout << name << password << endl;

        // 判断登录还是注册
        // '2'是登录
        if(*(p + 1) == '2'){
            strcpy(m_url, "/welcome.html");
        }
        // '3'是注册
        // 注册之后跳转到登录页面
        else{
            strcpy(m_url, "/log.html");
        }
    }
    // '0'跳转注册界面
    if(*(p + 1) == '0'){
        // // C库函数
        // char *m_url_real = (char *)malloc(sizeof(char) * 200);
        // strcpy(m_url_real, "/register.html");
        // strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        // free(m_url_real);
        string m_url_real = "/register.html";
        strncpy(m_real_file + len, m_url_real.c_str(), m_url_real.size());
    }
    // '1'跳转登录界面
    else if(*(p + 1) == '1'){
        string m_url_real = "/log.html";
        strncpy(m_real_file + len, m_url_real.c_str(), m_url_real.size());
    }
    else if(*(p + 1) == '5'){
        string m_url_real = "/picture.html";
        strncpy(m_real_file + len, m_url_real.c_str(), m_url_real.size());
    }
    else if(*(p + 1) == '6'){
        string m_url_real = "/video.html";
        strncpy(m_real_file + len, m_url_real.c_str(), m_url_real.size());
    }
    else if(*(p + 1) == '7'){
        string m_url_real = "/fans.html";
        strncpy(m_real_file + len, m_url_real.c_str(), m_url_real.size());
    }
    else
        strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    
    // 判断资源是否存在
    if ( stat( m_real_file, &m_file_stat ) < 0 )
    {
        return NO_RESOURCE;
    }
    // S_IROTH     00004       其他用户具可读取权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) )
    {
        return FORBIDDEN_REQUEST;
    }
    // 是目录
    if ( S_ISDIR( m_file_stat.st_mode ) )
    {
        return BAD_REQUEST;
    }

    int fd = open( m_real_file, O_RDONLY );
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST;
}

// 对内存映射块执行munmap操作
void http_conn::unmap()
{
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

// 写http响应
bool http_conn::write()
{
    int temp = 0;
    int bytes_have_send = 0;
    int bytes_to_send = m_write_idx;
    if ( bytes_to_send == 0 )
    {
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        init();
        return true;
    }

    while( 1 )
    {
        temp = writev( m_sockfd, m_iv, m_iv_count );
        if ( temp <= -1 )
        {
            // 如果tcp写缓冲没有空间,则等待下一轮EPOLLOUT事件
            // 虽然在此期间,服务器没法接受到同一个客户的下一个请求,单可以保持连接的完整性
            if( errno == EAGAIN )
            {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }

        bytes_to_send -= temp;
        bytes_have_send += temp;
        if ( bytes_to_send <= bytes_have_send )
        {
            // 发送http响应成功,根据http请求中的Connection字段决定是否立即关闭连接
            unmap();
            if( m_linger )
            {
                init();
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return true;
            }
            else
            {
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return false;
            } 
        }
    }
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response( const char* format, ... )
{
    if( m_write_idx >= WRITE_BUFFER_SIZE )
    {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) )
    {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

bool http_conn::add_status_line( int status, const char* title )
{
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers( int content_len )
{
    add_content_length( content_len );
    add_linger();
    add_blank_line();
    return true;
}

bool http_conn::add_content_length( int content_len )
{
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

// 根据服务器处理http请求的结果,决定返回客户端的数据
bool http_conn::process_write( HTTP_CODE ret )
{
    switch ( ret )
    {
        case INTERNAL_ERROR:
        {
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) )
            {
                return false;
            }
            break;
        }
        case BAD_REQUEST:
        {
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) )
            {
                return false;
            }
            break;
        }
        case NO_RESOURCE:
        {
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) )
            {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            add_status_line( 403, error_403_title );
            add_headers( strlen( error_403_form ) );
            if ( ! add_content( error_403_form ) )
            {
                return false;
            }
            break;
        }
        case FILE_REQUEST:
        {
            add_status_line( 200, ok_200_title );
            if ( m_file_stat.st_size != 0 )
            {
                add_headers( m_file_stat.st_size );
                m_iv[ 0 ].iov_base = m_write_buf;
                m_iv[ 0 ].iov_len = m_write_idx;
                m_iv[ 1 ].iov_base = m_file_address;
                m_iv[ 1 ].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                return true;
            }
            else
            {
                const char* ok_string = "<html><body></body></html>";
                add_headers( strlen( ok_string ) );
                if ( ! add_content( ok_string ) )
                {
                    return false;
                }
            }
	    break;
        }
        default:
        {
            return false;
        }
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

// 由线程池中的工作线程调用,处理http请求的入口函数
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if ( read_ret == NO_REQUEST )
    {
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        return;
    }

    bool write_ret = process_write( read_ret );
    if ( ! write_ret )
    {
        close_conn();
    }

    modfd( m_epollfd, m_sockfd, EPOLLOUT );
}

