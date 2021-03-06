/*
	http_business.cpp
	parse http request and send back a .html file
*/

#include "http_business.h"
#include "public_func.h"

namespace mj{
	const char* ok_200_title = "OK";
	const char* error_400_title = "Bad Request";
	const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
	const char* error_403_title = "Forbidden";
	const char* error_403_form = "You do not have permission to get file from this server.\n";
	const char* error_404_title = "Not Found";
	const char* error_404_form = "The requested file was not found on this server.\n";
	const char* error_500_title = "Internal Error";
	const char* error_500_form = "There was an unusual problem serving the requested file.\n";
	const char* doc_root = "/var/www/html";


	int http_business::http_user_count = 0;
	int http_business::http_epollfd = -1;

	void http_business::close_conn( bool real_close )
	{
		if( real_close && ( http_sockfd != -1 ) )
		{
		    removefd( http_epollfd, http_sockfd );
		    http_sockfd = -1;
		    http_user_count--;
		}
	}

	void http_business::init( int sockfd, const sockaddr_in& addr )
	{
		http_sockfd = sockfd;
		http_address = addr;
		
		addfd( http_epollfd, sockfd, true );
		http_user_count++;

		init();
	}

	void http_business::init()
	{
		http_check_state = CHECK_STATE_REQUESTLINE;
		http_keep_alive = false;

		http_method = GET;
		http_url = 0;
		http_version = 0;
		http_content_length = 0;
		http_host = 0;
		http_start_line = 0;
		http_checked_idx = 0;
		http_read_idx = 0;
		http_write_idx = 0;
		memset( http_read_buf, '\0', READ_BUFFER_SIZE );
		memset( http_write_buf, '\0', WRITE_BUFFER_SIZE );
		memset( http_real_file, '\0', FILENAME_LEN );
	}

	http_business::LINE_STATUS http_business::parse_line()
	{
		char temp;
		for ( ; http_checked_idx < http_read_idx; ++http_checked_idx )
		{
		    temp = http_read_buf[ http_checked_idx ];
		    if ( temp == '\r' )
		    {
		        if ( ( http_checked_idx + 1 ) == http_read_idx )
		        {
		            return LINE_OPEN;
		        }
		        else if ( http_read_buf[ http_checked_idx + 1 ] == '\n' )
		        {
		            http_read_buf[ http_checked_idx++ ] = '\0';
		            http_read_buf[ http_checked_idx++ ] = '\0';
		            return LINE_OK;
		        }

		        return LINE_BAD;
		    }
		    else if( temp == '\n' )
		    {
		        if( ( http_checked_idx > 1 ) && ( http_read_buf[ http_checked_idx - 1 ] == '\r' ) )
		        {
		            http_read_buf[ http_checked_idx-1 ] = '\0';
		            http_read_buf[ http_checked_idx++ ] = '\0';
		            return LINE_OK;
		        }
		        return LINE_BAD;
		    }
		}

		return LINE_OPEN;
	}

	bool http_business::read()
	{
		if( http_read_idx >= READ_BUFFER_SIZE )
		{
		    return false;
		}

		int bytes_read = 0;
		while( true )
		{
		    bytes_read = recv( http_sockfd, http_read_buf + http_read_idx, READ_BUFFER_SIZE - http_read_idx, 0 );
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
		        return false;
		    }

		    http_read_idx += bytes_read;
		}
		return true;
	}

	http_business::HTTP_CODE http_business::parse_request_line( char* text )
	{
		http_url = strpbrk( text, " \t" );
		if ( ! http_url )
		{
		    return BAD_REQUEST;
		}
		*http_url++ = '\0';

		char* method = text;
		if ( strcasecmp( method, "GET" ) == 0 )
		{
		    http_method = GET;
		}
		else
		{
		    return BAD_REQUEST;
		}

		http_url += strspn( http_url, " \t" );
		http_version = strpbrk( http_url, " \t" );
		if ( ! http_version )
		{
		    return BAD_REQUEST;
		}
		*http_version++ = '\0';
		http_version += strspn( http_version, " \t" );
		if ( strcasecmp( http_version, "HTTP/1.1" ) != 0 )
		{
		    return BAD_REQUEST;
		}

		if ( strncasecmp( http_url, "http://", 7 ) == 0 )
		{
		    http_url += 7;
		    http_url = strchr( http_url, '/' );
		}

		if ( ! http_url || http_url[ 0 ] != '/' )
		{
		    return BAD_REQUEST;
		}

		http_check_state = CHECK_STATE_HEADER;
		return INCOMPLETE_REQUEST;
	}

	http_business::HTTP_CODE http_business::parse_headers( char* text )
	{
		if( text[ 0 ] == '\0' )
		{
		    if ( http_method == HEAD )
		    {
		        return GET_REQUEST;
		    }

		    if ( http_content_length != 0 )
		    {
		        http_check_state = CHECK_STATE_CONTENT;
		        return INCOMPLETE_REQUEST;
		    }

		    return GET_REQUEST;
		}
		else if ( strncasecmp( text, "Connection:", 11 ) == 0 )
		{
		    text += 11;
		    text += strspn( text, " \t" );
		    if ( strcasecmp( text, "keep-alive" ) == 0 )
		    {
		        http_keep_alive = true;
		    }
		}
		else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 )
		{
		    text += 15;
		    text += strspn( text, " \t" );
		    http_content_length = atol( text );
		}
		else if ( strncasecmp( text, "Host:", 5 ) == 0 )
		{
		    text += 5;
		    text += strspn( text, " \t" );
		    http_host = text;
		}
		else
		{
		    //printf( "unknow header %s\n", text );
		}

		return INCOMPLETE_REQUEST;

	}

	http_business::HTTP_CODE http_business::parse_content( char* text )
	{
		if ( http_read_idx >= ( http_content_length + http_checked_idx ) )
		{
		    text[ http_content_length ] = '\0';
		    return GET_REQUEST;
		}

		return INCOMPLETE_REQUEST;
	}

	http_business::HTTP_CODE http_business::process_read()
	{
		LINE_STATUS line_status = LINE_OK;
		HTTP_CODE ret = INCOMPLETE_REQUEST;
		char* text = 0;

		while ( ( ( http_check_state == CHECK_STATE_CONTENT ) && ( line_status == LINE_OK  ) )
		            || ( ( line_status = parse_line() ) == LINE_OK ) )
		{
		    text = get_line();
		    http_start_line = http_checked_idx;
		    //printf( "got one http line: %s\n", text );

		    switch ( http_check_state )
		    {
		        case CHECK_STATE_REQUESTLINE:
		        {
		            ret = parse_request_line( text );
		            if ( ret == BAD_REQUEST )
		            {
		                return BAD_REQUEST;
		            }
		            break;
		        }
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

		return INCOMPLETE_REQUEST;
	}

	http_business::HTTP_CODE http_business::do_request()
	{
		strcpy( http_real_file, doc_root );
		int len = strlen( doc_root );
		strncpy( http_real_file + len, http_url, FILENAME_LEN - len - 1 );
		if ( stat( http_real_file, &http_file_stat ) < 0 )
		{
		    return NO_RESOURCE;
		}

		if ( ! ( http_file_stat.st_mode & S_IROTH ) )
		{
		    return FORBIDDEN_REQUEST;
		}

		if ( S_ISDIR( http_file_stat.st_mode ) )
		{
		    return BAD_REQUEST;
		}

		int fd = open( http_real_file, O_RDONLY );
		http_file_address = ( char* )mmap( 0, http_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
		close( fd );
		return FILE_REQUEST;
	}

	void http_business::unmap()
	{
		if( http_file_address )
		{
		    munmap( http_file_address, http_file_stat.st_size );
		    http_file_address = 0;
		}
	}

	bool http_business::write()
	{
		int temp = 0;
		int bytes_have_send = 0;
		int bytes_to_send = http_write_idx;
		if ( bytes_to_send == 0 )
		{
		    modfd( http_epollfd, http_sockfd, EPOLLIN );
		    init();
		    return true;
		}

		while( 1 )
		{
		    temp = writev( http_sockfd, http_iv, http_iv_count );
		    if ( temp <= -1 )
		    {
		        if( errno == EAGAIN )
		        {
		            modfd( http_epollfd, http_sockfd, EPOLLOUT );
		            return true;
		        }
		        unmap();
		        return false;
		    }

		    bytes_to_send -= temp;
		    bytes_have_send += temp;
		    if ( bytes_to_send <= bytes_have_send )
		    {
		        unmap();
		        if( http_keep_alive )
		        {
		            init();
		            modfd( http_epollfd, http_sockfd, EPOLLIN );
		            return true;
		        }
		        else
		        {
		            modfd( http_epollfd, http_sockfd, EPOLLIN );
		            return false;
		        } 
		    }
		}
	}

	bool http_business::add_response( const char* format, ... )
	{
		if( http_write_idx >= WRITE_BUFFER_SIZE )
		{
		    return false;
		}
		va_list arg_list;
		va_start( arg_list, format );
		int len = vsnprintf( http_write_buf + http_write_idx, WRITE_BUFFER_SIZE - 1 - http_write_idx, format, arg_list );
		if( len >= ( WRITE_BUFFER_SIZE - 1 - http_write_idx ) )
		{
		    return false;
		}
		http_write_idx += len;
		va_end( arg_list );
		return true;
	}

	bool http_business::add_status_line( int status, const char* title )
	{
		return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
	}

	bool http_business::add_headers( int content_len )
	{
		add_content_length( content_len );
		add_linger();
		add_blank_line();
	}

	bool http_business::add_content_length( int content_len )
	{
		return add_response( "Content-Length: %d\r\n", content_len );
	}

	bool http_business::add_linger()
	{
		return add_response( "Connection: %s\r\n", ( http_keep_alive == true ) ? "keep-alive" : "close" );
	}

	bool http_business::add_blank_line()
	{
		return add_response( "%s", "\r\n" );
	}

	bool http_business::add_content( const char* content )
	{
		return add_response( "%s", content );
	}

	bool http_business::process_write( HTTP_CODE ret )
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
		        if ( http_file_stat.st_size != 0 )
		        {
		            add_headers( http_file_stat.st_size );
		            http_iv[ 0 ].iov_base = http_write_buf;
		            http_iv[ 0 ].iov_len = http_write_idx;
		            http_iv[ 1 ].iov_base = http_file_address;
		            http_iv[ 1 ].iov_len = http_file_stat.st_size;
		            http_iv_count = 2;
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
		    }
		    default:
		    {
		        return false;
		    }
		}

		http_iv[ 0 ].iov_base = http_write_buf;
		http_iv[ 0 ].iov_len = http_write_idx;
		http_iv_count = 1;
		return true;
	}

	void http_business::process()
	{
		HTTP_CODE read_ret = process_read();
		if ( read_ret == INCOMPLETE_REQUEST )
		{
		    modfd( http_epollfd, http_sockfd, EPOLLIN );
		    return;
		}

		bool write_ret = process_write( read_ret );
		if ( ! write_ret )
		{
		    close_conn();
		}

		modfd( http_epollfd, http_sockfd, EPOLLOUT );
	}
}

