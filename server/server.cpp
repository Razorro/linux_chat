#include	"../util.h"
#include	"../const.h"
#include	"../msg_util.h"
#include	"../msg_buffer.h"

#include	"./db.h"
#include	"./service.h"

#include	<sys/epoll.h>
#include	<sys/time.h>

#include	<map>
#include	<string>
#include	<fstream>
#include	<iostream>
using namespace std;

/*
 *Accept a connection socket and add events to epoll
 *@return 
 	accepted socket id if success
	-1 if fail
 * */
int do_accept(int listen_fd, int epoll_fd)
{
	int conn_fd;

	/* accept a connection socket */
	if ( (conn_fd = accept(listen_fd, NULL, NULL)) < 0)
		return -1;

	if ( Util::make_socket_unblocking(conn_fd) < 0) 
	{
		close(conn_fd);
		return -1;
	}

	/* add events to epoll */
	epoll_event ev;
	Util::create_event(&ev, conn_fd, EPOLLIN);

	if ( epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev) < 0)
	{
		close(conn_fd);
		return -1;
	}

	return conn_fd;
}

void close_sock(int sock_fd, map<int, Msg_buffer*> & map_sock_msgbuff, DB & db)
{
	/* destroy socket */
	close(sock_fd);
						
	delete map_sock_msgbuff[sock_fd];
	map_sock_msgbuff.erase(sock_fd);

	/* log out */
	string name;
	name = db.map_login_sock_name[sock_fd];
	db.map_login_name_sock.erase(name);

	db.map_login_sock_name.erase(sock_fd);
}

/*
* do service for the request message fields,
* because echo may be a broadcast, 
* so echo socket and echo message fields are vector.
*@param request_sockfd in
*@param	request_fields in
	request socket and message fields from client
*@param v_echo_sockfd out
*@param v_echo_fields out
	echo sockets and message fields generated by the service
*@param db in, out
	database, it will be used by service
*@return
	this function is always success
*/
void do_service(const int request_sockfd, const Msg_field & request_fields, 
			vector<int> & v_echo_sockfd, vector<Msg_field> & v_echo_fields, 
			DB & db)
{
	if ( strcmp(request_fields.msg_type, CONST::MSG_TYPE_REGIST) == 0)
	{   
		/* client request registing service */
		if ( Service::regist(request_sockfd, request_fields, v_echo_sockfd, v_echo_fields, db) == 0)
			puts("注册成功");
		else
			puts("注册失败!");
	}
	else if ( strcmp(request_fields.msg_type, CONST::MSG_TYPE_LOGIN) == 0)
	{ 
		/* client request logining service */
		if ( Service::login(request_sockfd, request_fields, v_echo_sockfd, v_echo_fields, db) == 0)
			puts("登录成功");
		else
			puts("登录失败!");
	}
	else if ( strcmp(request_fields.msg_type, CONST::MSG_TYPE_LOGOUT) == 0)
	{
		/* client logout service */
		Service::logout(request_sockfd, request_fields, v_echo_sockfd, v_echo_fields, db);
	}
	else if ( strcmp(request_fields.msg_type, CONST::MSG_TYPE_FRIENDLIST) == 0)
	{
		/* client request getting friend list service */
		Service::get_friendlist(request_sockfd, request_fields, v_echo_sockfd, v_echo_fields, db);
	}
	else if ( strcmp(request_fields.msg_type, CONST::MSG_TYPE_ADDOK) == 0)
	{
		/* client agree with adding friend service */
		Service::add_friend(request_sockfd, request_fields, v_echo_sockfd, v_echo_fields, db);
	}
	else if ( strcmp(request_fields.msg_type, CONST::MSG_TYPE_MSG) == 0 ||
			  strcmp(request_fields.msg_type, CONST::MSG_TYPE_FB) == 0  ||
			  strcmp(request_fields.msg_type, CONST::MSG_TYPE_FI) == 0  ||
			  strcmp(request_fields.msg_type, CONST::MSG_TYPE_FE) == 0  ||
			  strcmp(request_fields.msg_type, CONST::MSG_TYPE_FOK) == 0 ||
			  strcmp(request_fields.msg_type, CONST::MSG_TYPE_FNO) == 0 ||
			  strcmp(request_fields.msg_type, CONST::MSG_TYPE_ADDFRIEND) == 0 ||
			  strcmp(request_fields.msg_type, CONST::MSG_TYPE_ADDNO) == 0 )
	{ 
		/* client request transpondering message service */
		Service::transponder(request_sockfd, request_fields, v_echo_sockfd, v_echo_fields, db);
	}
}

double diff_time(const timeval & b, const timeval & e)
{
	/* return different time in seconds */
	return (1000000*(e.tv_sec - b.tv_sec) + (e.tv_usec - b.tv_usec)) * 1.0 / 1000000;
}

/*
* echo all message to corresponding socket,
* and open write listening for this socket.
*@return
	this function is always success
*/
void echo_msg(const vector<int> & v_echo_sockfd, const vector<Msg_field> & v_echo_fields, 
			map<int, Msg_buffer*> & map_sock_msgbuff, int epoll_fd)
{
	for (int i = 0; i < v_echo_sockfd.size(); ++ i)
	{
		char echo_msg[CONST::MSG_SIZE];
		int  echo_msg_len;
		int  echo_sockfd;

		echo_sockfd = v_echo_sockfd[i];
		echo_msg_len = Msg_util::packing(&v_echo_fields[i], echo_msg);
		if ( map_sock_msgbuff[echo_sockfd]->push_a_msg(echo_msg, echo_msg_len) == 0)
			Util::update_event(epoll_fd, echo_sockfd, EPOLLOUT);
	}
}

/*
*@return
	this function is always success
*/
void offline_broadcast(const int request_sockfd, DB & db,
			map<int, Msg_buffer*> & map_sock_msgbuff, int epoll_fd)
{
	Msg_field request_fields(CONST::MSG_TYPE_LOGOUT, (char*)"", (char*)"", (char*)"");
	vector<int> v_echo_sockfd;
	vector<Msg_field> v_echo_fields;

	do_service(request_sockfd, request_fields, v_echo_sockfd, v_echo_fields, db);
	echo_msg(v_echo_sockfd, v_echo_fields, map_sock_msgbuff, epoll_fd);
}

void wait_event(int epoll_fd, int listen_fd)
{
	/* data structures will be used in some events */
	map<int, Msg_buffer*> map_sock_msgbuff; /* create a message buffer when accept a new connection,
											   destroy a message buffer when close a existed connection */
	
	DB db; /* database */
	
	if ( db.load_rigested_user_table(CONST::REGISTED_FILE) < 0)
	{
		printf("open registed file fail\n");
		return;
	}
	if ( db.load_friendship_table(CONST::FRIENDSHIP_FILE) < 0)
	{
		printf("open friendship file fail\n");
		return;
	}
	/* data structures finish */
	
	/* waiting for events */
	int n, i;
	epoll_event events[CONST::MAX_EVENTS];

	while (1)
	{
		if ( (n = epoll_wait(epoll_fd, events, CONST::MAX_EVENTS, -1)) < 0)
			Util::err_quit("epoll_wait call fail.");

		for (i = 0; i < n; ++ i)
		{
			int  request_sockfd = events[i].data.fd;
			bool is_close = false;

			if (request_sockfd == listen_fd)
			{
				/* main function used to accepting client connection
				   and creating a messge buffer for new accepted client connection */
				int conn_fd;
				if ( (conn_fd = do_accept(listen_fd, epoll_fd)) > 0)
				{
					map_sock_msgbuff[conn_fd] = new Msg_buffer(conn_fd, 
												CONST::MSG_QUEUE_SIZE, 
												CONST::MSG_N_BYTE_OF_LENGTH);
				}
			}
			else 
			{
				if (events[i].events & EPOLLIN)
				{
					/* read all request message from socket,
					   for each request message, 
					     firstly unpacking this message, 
					     secondly call do_service and get echo message fields,
					     at last write all echo message to corresponding socket */
					int ret;
					if ( (ret = map_sock_msgbuff[request_sockfd]->read_all()) > 0)
					{
						char request_msg[CONST::MSG_SIZE];
						int  request_msg_len;

						while ( (request_msg_len = map_sock_msgbuff[request_sockfd]->pop_a_msg(request_msg)) > 0)
						{	
							Msg_field request_fields;
							vector<int> v_echo_sockfd;
							vector<Msg_field> v_echo_fields;

							if ( Msg_util::unpacking(request_msg, request_msg_len, &request_fields))
							{
								do_service(request_sockfd, request_fields, v_echo_sockfd, v_echo_fields, db);
							}
							else
							{
								v_echo_sockfd.push_back(request_sockfd);
								Msg_field echo_fields("ERR", (char*)"", (char*)"", "消息格式错误");
								v_echo_fields.push_back(echo_fields);
							}

							echo_msg(v_echo_sockfd, v_echo_fields, map_sock_msgbuff, epoll_fd);
						}
					}
					else if (ret == 0)
					{
						printf("client close\n");
						is_close = true;
					}
					else
					{
						Util::err_sys("read socket error");
						is_close = true;
					}
				}
				if (events[i].events & EPOLLOUT)
				{
					Msg_buffer* p_msg_buffer;
					p_msg_buffer = map_sock_msgbuff[request_sockfd];

					int ret;
					if ( (ret = p_msg_buffer->write_all()) > 0)
						Util::update_event(epoll_fd, request_sockfd, EPOLLIN);
					else if (ret < 0)
						is_close = true;
					else
						; /* socket writing operation blocked,
						     do nothing and just wait this socket 
							 writing operation ready again,
							 then write rest bytes from message buffer to socket. */
				}
			}

			if (is_close)
			{
				offline_broadcast(request_sockfd, db, map_sock_msgbuff, epoll_fd);
				close_sock(request_sockfd, map_sock_msgbuff, db);
			}
		}/* for each IO request */
	}/* while listening */
}

int main()
{
	int listen_fd;
	
	/* create listening socket */
	if ( (listen_fd = Util::create_socket(NULL, CONST::PORT_NO)) < 0)
		Util::err_quit("create listening socket fail.");

	if ( Util::make_socket_unblocking(listen_fd) < 0)
		Util::err_quit("make listening socket unblocking fail.");

	listen(listen_fd, 100);
	
	/* create epoll with lt */
	int epoll_fd;
	if ( (epoll_fd = epoll_create(1)) < 0)
		Util::err_quit("create epoll fail.");

	/* add listening socket to epoll */
	epoll_event ev;
	Util::create_event(&ev, listen_fd, EPOLLIN);

	if ( epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0)
		Util::err_quit("add listening socket to epoll fail.");

	/* wait event */
	wait_event(epoll_fd, listen_fd);

	return 0;
}