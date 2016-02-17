#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

//#include "asm.h"
//#include "processor.h"

#include "errno.h"
#include "gccore.h"
#include "semaphore.h"
#include "network.h"
#include "debug_if.h"


#undef	DB_DEBUG_FLAG


#ifdef	DB_DEBUG_FLAG
s32 db_debug_flag = 0;

extern int net_print_init(const char *rhost, unsigned short port);

extern int net_print_string( const char* file, int line, const char* format, ...);

extern int net_print_binary( int format, const void* binary, int len);

#   define	DEBUG_PRINT(x) \
	if ( db_debug_flag) net_print_string x
#   define	DEBUG_BINARY(x) \
	if ( db_debug_flag) net_print_binary x
#else
#   define	DEBUG_PRINT(x)
#   define	DEBUG_BINARY(x)
#endif

static struct dbginterface netif_device;

//static 
void *__db_tcpip_helper = NULL;

  /* this variable can be put into DebutHelper_t */
//static 
lwp_t __db_helper_thread = (lwp_t)NULL ;

enum {
	HELPER_CODE_NONE = 0,
	HELPER_CODE_OPEN,
	HELPER_CODE_CLOSE,
	HELPER_CODE_READ,
	HELPER_CODE_WRITE,
	HELPER_CODE_WAIT,
	HELPER_CODE_STOP,
	HELPER_CODE_END
};

  /* statistics information for debugging purpose */
s32 __db_req_count[ HELPER_CODE_END ] = {0};
s32 __db_ack_count[ HELPER_CODE_END ] = {0};
u32 __db_read_bytes = 0;
u32 __db_write_bytes = 0;

typedef	struct {
	void	*pointer;
	int	intval;
} HelperParam_t;

typedef struct {
	  /* request descriptions */
	volatile s32 code;
	HelperParam_t *param;     
	//HelperParam_t param[5];     

	s32 retvalue; 

	  /*
	   * Here we assume that the runtime core of gdb will 
	   * talk to the helper thread in single threaded mode.
	   *
  	   * we will use the following semaphore to synchronize
	   * the gdb-runtime-core and the helper thread.
	   *
	   * if multi-talkings, we have to modify this structure
	   * to add a mutext, change the request to a queue, and
	   * change the way exchange the return-value.
	   */
	sem_t requestReadySema;
	sem_t answerReadySema;

	s32	listensock;
	s32	clientsock;
	lwp_t	thread_handle;
} DebugHelper_t;

#if	0
static s32 setNonblocking(s32 fd)
{
    s32 flags;

    /* Fixme: O_NONBLOCK is defined but broken on SunOS 4.1.x and AIX 3.2.5. */
    if (-1 == (flags = net_fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return net_fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}     
#endif


static s32 helper_setup_socket( u16 port)
{
	s32 sock;
	int ret;
	//u32	clientlen;
	struct sockaddr_in server;
	struct sockaddr_in client;
	
	//clientlen = sizeof(client);

	sock = net_socket (AF_INET, SOCK_STREAM, IPPROTO_IP);

	if (sock == INVALID_SOCKET) {
		printf( "setup():INVALID_SOCKET\n");
		return -1;
	}

	memset (&server, 0, sizeof (server));
	memset (&client, 0, sizeof (client));

	server.sin_family = AF_INET;
	server.sin_port = htons (port);
	server.sin_addr.s_addr = INADDR_ANY;
	ret = net_bind (sock, (struct sockaddr *) &server, sizeof (server));
		
	if ( ret ) {
		printf( "net_bind():INVALID_SOCKET\n");
		net_close( sock); 
		return -1;
	}

	if ( (ret = net_listen( sock, 1)) ) {
		printf( "net_listen():INVALID_SOCKET\n");
		net_close( sock);
		return -1;
	}

#if	0
	ret = setNonblocking(sock);
	printf( "setNonblocking(%d) returns=%d.\n", sock, ret);
#endif
	return sock;
}

static
DebugHelper_t *DebugHelper_init( s32 mode, u16 port)
{
	s32	sock;
	DebugHelper_t *helper;

	sock = helper_setup_socket( port);
	if ( sock < 0 ) {
		return NULL;
	}
	helper = (DebugHelper_t*)malloc( sizeof( DebugHelper_t));
	if (helper==NULL){
		return helper;
	}

	memset(helper, 0, sizeof( DebugHelper_t));

	LWP_SemInit( &helper->requestReadySema, 0, 5);
	LWP_SemInit( &helper->answerReadySema, 0, 5);

	helper->code = HELPER_CODE_NONE;

	helper->listensock = sock;
	helper->clientsock = -1;

	helper->thread_handle = (lwp_t)NULL;

	DEBUG_PRINT((NULL,0, "helper object created, listensock=%d.\n", helper->listensock));
	return helper;
}

static s32 DebugHelper_request( DebugHelper_t *helper, s32 code, s32 nargs, HelperParam_t *param )
{
	if ( code <= HELPER_CODE_NONE || code >= HELPER_CODE_END ) {
		return 0;
	}
	__db_req_count[ code ] ++;
	DEBUG_PRINT((NULL,0,"DebugHelper_request(%p), code=%d.\n", helper, code));

	  /* nargs is not used */
	helper->param = param;
	//if ( nargs > 0 ) {
	//	memcpy( helper->param, param, nargs * sizeof( HelperParam_t));
	//}

	helper->code = code;

 	  /* notify the helper thread */
	LWP_SemPost( helper->requestReadySema);

	  /* wait be notified until the helper thread complete the request */
	LWP_SemWait( helper->answerReadySema);

	return	helper->retvalue;
}

static
s32 helper_opentcpip( DebugHelper_t *helper)
{
	struct sockaddr_in client;
	u32 clientlen = sizeof(client);

	if ( helper->listensock >=0 && helper->clientsock<0 ) {

		memset (&client, 0, sizeof (client));
	
		DEBUG_PRINT(( NULL,0, "helper_opentcpip(%p), try net_accept(), listensock=%d.\n", helper, helper->listensock));

                helper->clientsock = net_accept (helper->listensock, (struct sockaddr *) &client, &clientlen);

		DEBUG_PRINT(( NULL,0, "helper_opentcpip(%p), clientsock=%d.\n", helper, helper->clientsock));

		return helper->clientsock ;
	}
	return -1;

}

static 
s32 helper_closetcpip( DebugHelper_t *helper)
{
	net_close( helper->clientsock);
	helper->clientsock = -1;

 	return 0;
}

static 
s32 helper_waittcpip( DebugHelper_t *helper)
{
 	return 0;
}

static 
s32 helper_readtcpip( DebugHelper_t *helper)
{
	void *buffer = helper->param[0].pointer;
	s32 size = helper->param[1].intval;

	s32 ret = net_read( helper->clientsock, buffer, size);
	return ret;
}

static 
s32 helper_writetcpip( DebugHelper_t *helper)
{
	void *buffer = helper->param[0].pointer;
	s32 size = helper->param[1].intval;

	s32 ret = net_write( helper->clientsock, buffer, size);
	
	return ret;
}


static
void *DebugHelper_run( void *args)
{
	DebugHelper_t *helper = (DebugHelper_t*) args;
	s32 code = HELPER_CODE_NONE;
	s32 ret;

	DEBUG_PRINT((NULL,0,"Helper(%p) thread up and running ...\n", helper));

	while (1) {

		 /* wait be notified by gdb-runtime-core */
		LWP_SemWait( helper->requestReadySema);

		code=helper->code;

		DEBUG_PRINT((NULL,0,"Helper(%p) wakeup with code:%d\n", helper, helper->code));
		if ( code<= HELPER_CODE_NONE || code>= HELPER_CODE_END ) {
			continue;
		}
		__db_ack_count[ code]++;

		DEBUG_PRINT((NULL,0,"Helper(%p) working on code:%d\n", helper, code));

		switch( code) {
		case	HELPER_CODE_OPEN:
			ret = helper_opentcpip( helper);
			break;
		case	HELPER_CODE_CLOSE:
			ret = helper_closetcpip( helper);
			break;
		case	HELPER_CODE_READ:
			ret = helper_readtcpip( helper);
			break;
		case	HELPER_CODE_WRITE:
			ret = helper_writetcpip( helper);
			break;
		case	HELPER_CODE_WAIT:
			ret = helper_waittcpip( helper);
			break;
		case	HELPER_CODE_STOP:
			ret = 0;
			break;
		default:
			ret = 0;
			break;
		}
		helper->retvalue = ret;

		DEBUG_PRINT((NULL,0,"Helper(%p) completed with ret-code:%d\n", helper, ret));

		  /* just be safe */
		helper->code = HELPER_CODE_NONE;

		  /* notify the gdb-runtime-core */
		LWP_SemPost( helper->answerReadySema);

		if ( code==HELPER_CODE_STOP) {
			break ;
		}
	}
	return NULL;
}

static
void DebugHelper_start( DebugHelper_t *helper)
{
	LWP_CreateThread( &__db_helper_thread,	/* thread handle */ 
			DebugHelper_run,	/* code */ 
			helper,		/* arg pointer for thread */
			NULL,		/* stack base */ 
			16*1024,	/* stack size */
			50		/* thread priority */ );
}


static int open_helper_tcpip(struct dbginterface *device)
{
	if ( __db_tcpip_helper && device->fhndl<0 ) {
		device->fhndl = DebugHelper_request( __db_tcpip_helper, HELPER_CODE_OPEN, 0, NULL);
	}
	if ( device->fhndl < 0 ){
		return -1;
	}
	return 0;
}

static int close_helper_tcpip(struct dbginterface *device)
{
	if ( __db_tcpip_helper && device->fhndl>=0 ) {
		DebugHelper_request( __db_tcpip_helper, HELPER_CODE_CLOSE, 0, NULL);
		device->fhndl = -1;
	}
	return 0;
}

static int wait_helper_tcpip(struct dbginterface *device)
{
	if ( __db_tcpip_helper && device->fhndl>=0 ) {
		DebugHelper_request( __db_tcpip_helper, HELPER_CODE_WAIT, 0, NULL);
	}
	return 0;
}

static int read_helper_tcpip(struct dbginterface *device,void *buffer,int size)
{
	HelperParam_t param[2];
	int ret = 0;

	param[0].pointer = buffer;
	param[1].intval  = size;

	if ( __db_tcpip_helper && device->fhndl>=0 ) {
		ret = DebugHelper_request( __db_tcpip_helper, HELPER_CODE_READ, 2, param);
		if ( ret>0 ) {
			__db_read_bytes += ret;
		}
		return ret;
	}
	return ret;
}

static int write_helper_tcpip(struct dbginterface *device,const void *buffer,int size)
{
	HelperParam_t param[2];
	int ret = 0;

	param[0].pointer = (void*)buffer;
	param[1].intval  = size;

	if ( __db_tcpip_helper && device->fhndl>=0 ) {
		ret = DebugHelper_request( __db_tcpip_helper, HELPER_CODE_WRITE, 2, param);
		if ( ret>0 ) {
			__db_write_bytes += ret;
		}
		return ret;
	}
	return ret;
}

void helper_tcpip_preinit(s32 type, s32 port)
{
	DebugHelper_t *helper = DebugHelper_init( type, port);

	if ( helper != NULL ) {
		DebugHelper_start( helper) ;
		__db_tcpip_helper = helper;
	}
}

struct dbginterface* helper_tcpip_init( s32 port)
{
	if ( __db_tcpip_helper==NULL ) {
		return NULL;
	}
	netif_device.fhndl = -1;
	netif_device.wait = wait_helper_tcpip;
	netif_device.open = open_helper_tcpip;
	netif_device.close = close_helper_tcpip;
	netif_device.read = read_helper_tcpip;
	netif_device.write = write_helper_tcpip;

	return &netif_device;
}

