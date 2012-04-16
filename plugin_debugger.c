/**********************************************************************
 * plugin_debugger.c	- Language-independent parts of debugger
 *
 * Copyright (c) 2004-2012 EnterpriseDB Corporation. All Rights Reserved.
 *
 * Licensed under the Artistic License, see 
 *		http://www.opensource.org/licenses/artistic-license.php
 * for full details
 *
 **********************************************************************/

#include "postgres.h"

#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>

#ifdef WIN32
	#include<winsock2.h>
#else
	#include <netinet/in.h>
	#include <sys/socket.h>
	#include <arpa/inet.h>
#endif

#include "nodes/pg_list.h"
#include "lib/dllist.h"
#include "lib/stringinfo.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "parser/parser.h"
#include "parser/parse_func.h"
#include "globalbp.h"
#include "storage/proc.h"							/* For MyProc		   */
#include "storage/procarray.h"						/* For BackendPidGetProc */
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "miscadmin.h"

#include "pldebugger.h"

/*
 * Let the PG module loader know that we are compiled against
 * the right version of the PG header files
 */

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

#define GET_STR(textp) DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(textp)))

#define	TARGET_PROTO_VERSION	"1.0"

/**********************************************************************
 * Type and structure definitions
 **********************************************************************/

/* 
 * eConnectType
 *
 *	This enum defines the different ways that we can connect to the 
 *  debugger proxy.  
 *
 *		CONNECT_AS_SERVER means that we create a socket, bind an address to
 *		to that socket, send a NOTICE to our client application, and wait for
 *		a debugger proxy to attach to us.  That's what happens when	your 
 *		client application sets a local breakpoint and can handle the 
 *		NOTICE that we send.
 *
 *		CONNECT_AS_CLIENT means that a proxy has already created a socket
 *		and is waiting for a target (that's us) to connect to it. We do
 *		this kind of connection stuff when a debugger client sets a global
 *		breakpoint and we happen to blunder into that breakpoint.
 *
 *		CONNECT_UNKNOWN indicates a problem, we shouldn't ever see this.
 */

typedef enum
{
	CONNECT_AS_SERVER, 	/* Open a server socket and wait for a proxy to connect to us	*/
	CONNECT_AS_CLIENT,	/* Connect to a waiting proxy (global breakpoints do this)		*/
	CONNECT_UNKNOWN		/* Must already be connected 									*/
} eConnectType;

/**********************************************************************
 * Local (static) variables
 **********************************************************************/


per_session_ctx_t per_session_ctx;

errorHandlerCtx client_lost;

/**********************************************************************
 * Function declarations
 **********************************************************************/

void _PG_init( void );				/* initialize this module when we are dynamically loaded	*/
void _PG_fini( void );				/* shutdown this module when we are dynamically unloaded	*/

/**********************************************************************
 * Local (hidden) function prototypes
 **********************************************************************/

//static char       ** fetchArgNames( PLpgSQL_function * func, int * nameCount );
static uint32 		 resolveHostName( const char * hostName );
static bool 		 getBool( int channel );
static char        * getNString( int channel );
static void 		 sendString( int channel, char * src );
static void        * writen( int peer, void * src, size_t len );
static bool 		 connectAsServer( void );
static bool 		 connectAsClient( Breakpoint * breakpoint );
static bool 		 handle_socket_error(void);
static bool 		 parseBreakpoint( Oid * funcOID, int * lineNumber, char * breakpointString );
static bool 		 addLocalBreakpoint( Oid funcOID, int lineNo );
static void			 reserveBreakpoints( void );

/**********************************************************************
 * Function definitions
 **********************************************************************/

void _PG_init( void )
{
	plpgsql_debugger_init();

	reserveBreakpoints();
}

/*
 * CREATE OR REPLACE FUNCTION pldbg_oid_debug( functionOID OID ) RETURNS INTEGER AS 'pldbg_oid_debug' LANGUAGE C;
 */

Datum pldbg_oid_debug(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pldbg_oid_debug);

Datum pldbg_oid_debug(PG_FUNCTION_ARGS)
{
	Oid			funcOid;
	HeapTuple	tuple;
	Oid			userid;

	if(( funcOid = PG_GETARG_OID( 0 )) == InvalidOid )
		ereport( ERROR, ( errcode( ERRCODE_UNDEFINED_FUNCTION ), errmsg( "no target specified" )));

	/* get the owner of the function */
	tuple = SearchSysCache(PROCOID,
				   ObjectIdGetDatum(funcOid),
				   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u",
			 funcOid);
	userid = ((Form_pg_proc) GETSTRUCT(tuple))->proowner;
	ReleaseSysCache(tuple);

	if( !superuser() && (GetUserId() != userid))
		ereport( ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg( "must be owner or superuser to create a breakpoint" )));

	addLocalBreakpoint( funcOid, -1 );

	PG_RETURN_INT32( 0 );
}

/*
 * ---------------------------------------------------------------------
 * readn()
 *
 *	This function reads exactly 'len' bytes from the given socket or it 
 *	throws an error.  readn() will hang until the proper number of bytes 
 *	have been read (or an error occurs).
 *
 *	Note: dst must point to a buffer large enough to hold at least 'len' 
 *	bytes.  readn() returns dst (for convenience).
 */

static void * readn( int peer, void * dst, size_t len )
{
	size_t	bytesRemaining = len;
	char  * buffer         = (char *)dst;

	while( bytesRemaining > 0 )
	{
		ssize_t bytesRead = recv( peer, buffer, bytesRemaining, 0 );

		if( bytesRead <= 0 && errno != EINTR )
			handle_socket_error();

		bytesRemaining -= bytesRead;
		buffer         += bytesRead;
	}

	return( dst );
}

/*
 * ---------------------------------------------------------------------
 * readUInt32()
 *
 *	Reads a 32-bit unsigned value from the server (and returns it in the host's
 *	byte ordering)
 */

static uint32 readUInt32( int channel )
{
	uint32	netVal;

	readn( channel, &netVal, sizeof( netVal ));

	return( ntohl( netVal ));
}

/*
 * ---------------------------------------------------------------------
 * dbg_read_str()
 *
 *	This function reads a counted string from the given stream
 *	Returns a palloc'd, null-terminated string.
 *
 *	NOTE: the server-side of the debugger uses this function to read a 
 *		  string from the client side
 */

char *dbg_read_str( void )
{
	uint32 len;
	char *dst;
	int sock = per_session_ctx.client_r;

	len = readUInt32( sock );

	dst = palloc(len + 1);
	readn( sock, dst, len );
	
	dst[len] = '\0';
	return dst;
}

/*
 * ---------------------------------------------------------------------
 * writen()
 *
 *	This function writes exactly 'len' bytes to the given socket or it 
 *  	throws an error.  writen() will hang until the proper number of bytes
 *	have been written (or an error occurs).
 */

static void * writen( int peer, void * src, size_t len )
{
	size_t	bytesRemaining = len;
	char  * buffer         = (char *)src;

	while( bytesRemaining > 0 )
	{
		ssize_t bytesWritten;

		if(( bytesWritten = send( peer, buffer, bytesRemaining, 0 )) <= 0 )
			handle_socket_error();
		
		bytesRemaining -= bytesWritten;
		buffer         += bytesWritten;
	}

	return( src );
}


/*
 * ---------------------------------------------------------------------
 * sendUInt32()
 *
 *	This function sends a uint32 value (val) to the debugger server.
 */

static void sendUInt32( int channel, uint32 val )
{
	uint32	netVal = htonl( val );

	writen( channel, &netVal, sizeof( netVal ));
}

/*******************************************************************************
 * getBool()
 *
 *	getBool() retreives a boolean value (TRUE or FALSE) from the server.  We
 *  call this function after we ask the server to do something that returns a
 *  boolean result (like deleting a breakpoint or depositing a new value).
 */

static bool getBool( int channel )
{
	char * str;
	bool   result;

	str = getNString( channel );

	if( str[0] == 't' )
		result = TRUE;
	else
		result = FALSE;

	pfree( str );

	return( result );
}

/*******************************************************************************
 * sendString()
 *
 *	This function sends a string value (src) to the debugger server.  'src' 
 *	should point to a null-termianted string.  We send the length of the string 
 *	(as a 32-bit unsigned integer), then the bytes that make up the string - we
 *	don't send the null-terminator.
 */

static void sendString( int channel, char * src )
{
	size_t	len = strlen( src );

	sendUInt32( channel, len );
	writen( channel, src, len );
}

/******************************************************************************
 * getNstring()
 *
 *	This function is the opposite of sendString() - it reads a string from the 
 *	debugger server.  The server sends the length of the string and then the
 *	bytes that make up the string (minus the null-terminator).  We palloc() 
 *	enough space to hold the entire string (including the null-terminator) and
 *	return a pointer to that space (after, of course, reading the string from
 *	the server and tacking on the null-terminator).
 */

static char * getNString( int channel )
{
	uint32 len = readUInt32( channel );

	if( len == 0 )
		return( NULL );
	else
	{
		char * result = palloc( len + 1 );

		readn( channel, result, len );

		result[len] = '\0';

		return( result );
	}
}


/*******************************************************************************
 * resolveHostName()
 *
 *	Given the name of a host (hostName), this function returns the IP address
 *	of that host (or 0 if the name does not resolve to an address).
 *
 *	FIXME: this function should probably be a bit more flexibile.
 */

#ifndef INADDR_NONE
#define INADDR_NONE ((unsigned long int) -1)    /* For Solaris */
#endif

static uint32 resolveHostName( const char * hostName )
{
    struct hostent * hostDesc;
    uint32           hostAddress;

    if(( hostDesc = gethostbyname( hostName )))
		hostAddress = ((struct in_addr *)hostDesc->h_addr )->s_addr;
    else
		hostAddress = inet_addr( hostName );

    if(( hostAddress == -1 ) || ( hostAddress == INADDR_NONE ))
		return( 0 );
	else
		return( hostAddress );
}

/*
 * ---------------------------------------------------------------------
 * dbg_send()
 *
 *	This function writes a formatted, counted string to the
 *	given stream.  The argument list for this function is identical to
 *	the argument list for the fprintf() function - you provide a socket,
 *	a format string, and then some number of arguments whose meanings 
 *	are defined by the format string.
 *
 *	NOTE:  the server-side of the debugger uses this function to send
 *		   data to the client side.  If the connection drops, dbg_send()
 *		   will longjmp() back to the debugger top-level so that the 
 *		   server-side can respond properly.
 */

void dbg_send( const char *fmt, ... )
{
	StringInfoData	result;
	char		   *data;
	size_t			remaining;
	int				sock = per_session_ctx.client_w;
	
	if( !sock )
		return;

	initStringInfo(&result);

	for (;;)
	{
		va_list	args;
		bool	success;

		va_start(args, fmt);
		success = appendStringInfoVA(&result, fmt, args);
		va_end(args);

		if (success)
			break;

		enlargeStringInfo(&result, result.maxlen);
	}

	data = result.data;
	remaining = strlen(data);

	sendUInt32(sock, remaining);

	while( remaining > 0 )
	{
		int written = send( sock, data, remaining, 0 );

		if(written < 0)
		{	
			handle_socket_error();
			continue;
		}

		remaining -= written;
		data      += written;
	}

	pfree(result.data);
}

/*
 * ---------------------------------------------------------------------
 * findSource()
 *
 *	This function locates and returns a pointer to a null-terminated string
 *	that contains the source code for the given function (identified by its
 *	OID).
 *
 *	In addition to returning a pointer to the requested source code, this
 *	function sets *tup to point to a HeapTuple (that you must release when 
 *	you are finished with it).
 */

char * findSource( Oid oid, HeapTuple * tup )
{
	bool	isNull;

	*tup = SearchSysCache( PROCOID, ObjectIdGetDatum( oid ), 0, 0, 0 );

	if(!HeapTupleIsValid( *tup ))
		elog( ERROR, "pldebugger: cache lookup for proc %u failed", oid );

	return( DatumGetCString( DirectFunctionCall1( textout, SysCacheGetAttr( PROCOID, *tup, Anum_pg_proc_prosrc, &isNull ))));
}


/*
 * ---------------------------------------------------------------------
 * attach_to_proxy()
 *
 *	This function creates a connection to the debugger client (via the
 *  proxy process). attach_to_proxy() will hang the PostgreSQL backend
 *  until the debugger client completes the connection.
 *
 *	We start by asking the TCP/IP stack to allocate an unused port, then we 
 *	extract the port number from the resulting socket, send the port number to
 *	the client application (by raising a NOTICE), and finally, we wait for the
 *	client to connect.
 *
 *	We assume that the client application knows the IP address of the PostgreSQL
 *	backend process - if that turns out to be a poor assumption, we can include 
 *	the IP address in the notification string that we send to the client application.
 */

bool attach_to_proxy( Breakpoint * breakpoint )
{
	bool			result;
	errorHandlerCtx	save;

    if( per_session_ctx.client_w )
	{
		/* We're already connected to a live proxy, just go home */
		return( TRUE );
	}

	if( breakpoint == NULL )
	{
		/* 
		 * No breakpoint - that implies that we're 'stepping into'.
		 * We had better already have a connection to a proxy here
		 * (how could we be 'stepping into' if we aren't connected
		 * to a proxy?)
		 */
		return( FALSE );
	}

	/*
	 * When a networking error is detected, we longjmp() to the client_lost
	 * error handler - that normally points to a location inside of dbg_newstmt()
	 * but we want to handle any network errors that arise while we are 
	 * setting up a link to the proxy.  So, we save the original client_lost
	 * error handler context and push our own context on to the stack.
	 */

	save = client_lost;
	
	if( sigsetjmp( client_lost.m_savepoint, 1 ) != 0 )
	{
		client_lost = save;
		return( FALSE );
	}

    if( breakpoint->data.proxyPort == -1 )
	{
		/*
		 * proxyPort == -1 implies that this is a local breakpoint,
		 * create a server socket and wait for the proxy to contact
		 * us.
		 */
		result = connectAsServer();
	}
	else
	{
		/*
		 * proxyPort != -1 implies that this is a global breakpoint,
		 * a debugger proxy is already waiting for us at the given
		 * port (on this host), connect to that proxy.
		 */

		result = connectAsClient( breakpoint );
	}

	/*
	 * Now restore the original error handler context so that
	 * dbg_newstmt() can handle any future network errors.
	 */

	client_lost = save;
	return( result );
}

/*
 * ---------------------------------------------------------------------
 * connectAsServer()
 *
 *	This function creates a socket, asks the TCP/IP stack to bind it to
 *	an unused port, and then waits for a debugger proxy to connect to
 *	that port.  We send a NOTICE to our client process (on the other 
 *	end of the fe/be connection) to let the client know that it should
 *	fire up a debugger and attach to that port (the NOTICE includes 
 *	the port number)
 */

static bool connectAsServer( void )
{
	int	 				sockfd       = socket( AF_INET, SOCK_STREAM, 0 );
	struct sockaddr_in 	srv_addr     = {0};
	struct sockaddr_in  cli_addr     = {0};
	socklen_t			srv_addr_len = sizeof( srv_addr );
	socklen_t			cli_addr_len = sizeof(cli_addr);
	int	 				client_sock;
	int					reuse_addr_flag = 1;
#ifdef WIN32
	WORD                wVersionRequested;
	WSADATA             wsaData;
	int                 err;
	u_long              blockingMode = 0;
#endif

	/* Ask the TCP/IP stack for an unused port */
	srv_addr.sin_family      = AF_INET;
	srv_addr.sin_port        = htons( 0 );
	srv_addr.sin_addr.s_addr = htonl( INADDR_ANY );

#ifdef WIN32

	wVersionRequested = MAKEWORD( 2, 2 );
 
	err = WSAStartup( wVersionRequested, &wsaData );
	if ( err != 0 )
	{
		/* Tell the user that we could not find a usable 
		 * WinSock DLL.                                  
		 */
		return 0;
	}

	/* Confirm that the WinSock DLL supports 2.2.
	 * Note that if the DLL supports versions greater
	 * than 2.2 in addition to 2.2, it will still return
	 * 2.2 in wVersion since that is the version we
	 * requested.
	 */

	if ( LOBYTE( wsaData.wVersion ) != 2 ||HIBYTE( wsaData.wVersion ) != 2 )
	{
		/* Tell the user that we could not find a usable
		 * WinSock DLL.
		 */
		WSACleanup( );
		return 0;
	}
#endif

	setsockopt( sockfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse_addr_flag, sizeof( reuse_addr_flag ));

	/* Bind a listener socket to that port */
	if( bind( sockfd, (struct sockaddr *)&srv_addr, sizeof( srv_addr )) < 0 )
	{
		elog( COMMERROR, "pl_debugger - can't bind server port, errno %d", errno );
		return( FALSE );
	}

	/* Get the port number selected by the TCP/IP stack */
	getsockname( sockfd, (struct sockaddr *)&srv_addr, &srv_addr_len );

	/* Get ready to wait for a client */
	listen( sockfd, 2 );
		
#ifdef WIN32
	ioctlsocket( sockfd, FIONBIO,  &blockingMode );
#endif

	/* Notify the client application that a debugger is waiting on this port. */
	elog( NOTICE, "PLDBGBREAK:%d", ntohs( srv_addr.sin_port ));

	while( TRUE )
	{
		uint32	proxyPID;
		PGPROC *proxyOff;
		PGPROC *proxyProc;
		char   *proxyProtoVersion;
			
		/* and wait for the debugger client to attach to us */
		if(( client_sock = accept( sockfd, (struct sockaddr *)&cli_addr, &cli_addr_len )) < 0 )
		{
			per_session_ctx.client_w = per_session_ctx.client_r = 0;
			per_session_ctx.client_port = 0;
			return( FALSE );
		}
		else
		{
#ifdef WIN32
			u_long blockingMode1 = 0;

			ioctlsocket( client_sock, FIONBIO,  &blockingMode1 );
#endif
			
			per_session_ctx.client_w = client_sock;
			per_session_ctx.client_r = client_sock;
			per_session_ctx.client_port = 0;
		}

		/* Now authenticate the proxy */
		proxyPID = readUInt32( client_sock );
		readn( client_sock, &proxyOff, sizeof( proxyOff ));
		proxyProc = BackendPidGetProc(proxyPID);
		
		if (proxyProc == NULL || proxyProc != proxyOff)
		{
			/* This doesn't look like a valid proxy - he didn't send us the right info */
			ereport(LOG, (ERRCODE_CONNECTION_FAILURE, 
						  errmsg( "invalid debugger connection credentials")));
			dbg_send( "%s", "f" );
#ifdef WIN32
			closesocket( client_sock );
#else
			close( client_sock );
#endif
			per_session_ctx.client_w = per_session_ctx.client_r = 0;
			per_session_ctx.client_port = 0;
			continue;
		}

			
		/* 
		 * This looks like a valid proxy, let's use this connection
		 *
		 * FIXME: we may want to ensure that proxyProc->roleId corresponds
		 *		  to a superuser too
		 */
		dbg_send( "%s", "t" );
		
		/*
		 * The proxy now sends it's protocol version and we
		 * reply with ours
		 */
		proxyProtoVersion = dbg_read_str();
		pfree(proxyProtoVersion);
		dbg_send( "%s", TARGET_PROTO_VERSION );
		
		return( TRUE );
	}
}

/*
 * ---------------------------------------------------------------------
 * connectAsClient()
 *
 *	This function connects to a waiting proxy process over the given
 *  port. We got the port number from a global breakpoint (the proxy
 *	stores it's port number in the breakpoint so we'll know how to 
 *	find that proxy).
 */

static bool connectAsClient( Breakpoint * breakpoint )
{
	int					 proxySocket;
	struct 	sockaddr_in  proxyAddress = {0};
	char               * proxyProtoVersion;

	if(( proxySocket = socket( AF_INET, SOCK_STREAM, 0 )) < 0 )
	{
		ereport( COMMERROR, (errcode(ERRCODE_CONNECTION_FAILURE), errmsg( "debugger server can't create socket, errno %d", errno )));
		return( FALSE );
	}

	proxyAddress.sin_family 	  = AF_INET;
	proxyAddress.sin_addr.s_addr = resolveHostName( "127.0.0.1" );
	proxyAddress.sin_port        = htons( breakpoint->data.proxyPort );
	
	if( connect( proxySocket, (struct sockaddr *)&proxyAddress, sizeof( proxyAddress )) < 0 )
	{
		ereport( DEBUG1, (errcode(ERRCODE_CONNECTION_FAILURE), errmsg( "debugger could not connect to debug proxy" )));
		return( FALSE );
	}

	sendUInt32( proxySocket, MyProc->pid );
	writen( proxySocket, &MyProc, sizeof( MyProc ));

	if( !getBool( proxySocket ))
	{
		ereport( COMMERROR, (errcode(ERRCODE_CONNECTION_FAILURE), errmsg( "debugger proxy refused authentication" )));
	}

	/*
	 * Now exchange version information with the target - for now,
	 * we don't actually do anything with the version information,
	 * but as soon as we make a change to the protocol, we'll need
	 * to know the right patois.
	 */

	sendString( proxySocket, TARGET_PROTO_VERSION );

	proxyProtoVersion = getNString( proxySocket );
	
	pfree( proxyProtoVersion );

	per_session_ctx.client_w = proxySocket;
	per_session_ctx.client_r = proxySocket;
	per_session_ctx.client_port = breakpoint->data.proxyPort;

	BreakpointBusySession( breakpoint->data.proxyPid );

	return( TRUE );
}

/*
 * ---------------------------------------------------------------------
 * parseBreakpoint()
 *
 *	Given a string that formatted like "funcOID:linenumber", 
 *	this function parses out the components and returns them to the 
 *	caller.  If the string is well-formatted, this function returns 
 *	TRUE, otherwise, we return FALSE.
 */

static bool parseBreakpoint( Oid * funcOID, int * lineNumber, char * breakpointString )
{
	int a, b;
	int n;

	n = sscanf(breakpointString, "%d:%d", &a, &b);
	if (n == 2)
	{
		*funcOID = a;
		*lineNumber = b;
	}
	else
		return false;

	return( TRUE );
}

/*
 * ---------------------------------------------------------------------
 * addLocalBreakpoint()
 *
 *	This function adds a local breakpoint for the given function and 
 *	line number
 */

static bool addLocalBreakpoint( Oid funcOID, int lineNo )
{
	Breakpoint breakpoint;
	
	breakpoint.key.databaseId = MyProc->databaseId;
	breakpoint.key.functionId = funcOID;
	breakpoint.key.lineNumber = lineNo;
	breakpoint.key.targetPid  = MyProc->pid;
	breakpoint.data.isTmp     = FALSE;
	breakpoint.data.proxyPort = -1;
	breakpoint.data.proxyPid  = -1;

	return( BreakpointInsert( BP_LOCAL, &breakpoint.key, &breakpoint.data ));
}

/*
 * ---------------------------------------------------------------------
 * setBreakpoint()
 *
 *	The debugger client can set a local breakpoint at a given 
 *	function/procedure and line number by calling	this function
 *  (through the debugger proxy process).
 */

void setBreakpoint( char * command )
{
	/* 
	 *  Format is 'b funcOID:lineNumber'
	 */
	int			  lineNo;
	Oid			  funcOID;

	if( parseBreakpoint( &funcOID, &lineNo, command + 2 ))
	{
		if( addLocalBreakpoint( funcOID, lineNo ))
			dbg_send( "%s", "t" );
		else
			dbg_send( "%s", "f" );
	}
	else
	{
		dbg_send( "%s", "f" );
	}
}

/*
 * ---------------------------------------------------------------------
 * clearBreakpoint()
 *
 *	This function deletes the breakpoint at the package,
 *	function/procedure, and line number indicated by the
 *	given command.
 *
 *	For now, we maintain our own private list of breakpoints -
 *	later, we'll use the same list managed by the CREATE/
 *	DROP BREAKPOINT commands.
 */

void clearBreakpoint( char * command )
{
	/* 
	 *  Format is 'f funcOID:lineNumber'
	 */
	int			  lineNo;
	Oid			  funcOID;

	if( parseBreakpoint( &funcOID, &lineNo, command + 2 ))
	{
		Breakpoint breakpoint;
	
		breakpoint.key.databaseId = MyProc->databaseId;
		breakpoint.key.functionId = funcOID;
		breakpoint.key.lineNumber = lineNo;
		breakpoint.key.targetPid  = MyProc->pid;

		if( BreakpointDelete( BP_LOCAL, &breakpoint.key ))
			dbg_send( "t" );
		else
			dbg_send( "f" );
	}
	else
	{
		dbg_send( "f" ); 
	}
}

bool breakAtThisLine( Breakpoint ** dst, eBreakpointScope * scope, Oid funcOid, int lineNumber )
{
	BreakpointKey		key;

	key.databaseId = MyProc->databaseId;
	key.functionId = funcOid;
    key.lineNumber = lineNumber;

	if( per_session_ctx.step_into_next_func )
	{
		*dst   = NULL;
		*scope = BP_LOCAL;
		return( TRUE );
	}

	/*
	 *  We conduct 3 searches here.  
	 *	
	 *	First, we look for a global breakpoint at this line, targeting our
	 *  specific backend process.
	 *
	 *  Next, we look for a global breakpoint (at this line) that does
	 *  not target a specific backend process.
	 *
	 *	Finally, we look for a local breakpoint at this line (implicitly 
	 *  targeting our specific backend process).
	 *
	 *	NOTE:  We must do the local-breakpoint search last because, when the
	 *		   proxy attaches to our process, it marks all of its global
	 *		   breakpoints as busy (so other potential targets will ignore
	 *		   those breakpoints) and we copy all of those global breakpoints
	 *		   into our local breakpoint hash.  If the debugger client exits
	 *		   and the user starts another debugger session, we want to see the
	 *		   new breakpoints instead of our obsolete local breakpoints (we
	 *		   don't have a good way to detect that the proxy has disconnected
	 *		   until it's inconvenient - we have to read-from or write-to the
	 *		   proxy before we can tell that it's died).
	 */

	key.targetPid = MyProc->pid;		/* Search for a global breakpoint targeted at our process ID */
  
	if((( *dst = BreakpointLookup( BP_GLOBAL, &key )) != NULL ) && ((*dst)->data.busy == FALSE ))
	{
		*scope = BP_GLOBAL;
		return( TRUE );
	}

	key.targetPid = -1;					/* Search for a global breakpoint targeted at any process ID */

	if((( *dst = BreakpointLookup( BP_GLOBAL, &key )) != NULL ) && ((*dst)->data.busy == FALSE ))
	{
		*scope = BP_GLOBAL;
		return( TRUE );
	}

	key.targetPid = MyProc->pid;	 	/* Search for a local breakpoint (targeted at our process ID) */

	if(( *dst = BreakpointLookup( BP_LOCAL, &key )) != NULL )
	{
		*scope = BP_LOCAL;
		return( TRUE );
	}

	return( FALSE );
}   

bool breakpointsForFunction( Oid funcOid )
{
	if( BreakpointOnId( BP_LOCAL, funcOid ) || BreakpointOnId( BP_GLOBAL, funcOid ))
		return( TRUE );
	else
		return( FALSE );

}

/* ---------------------------------------------------------------------
 * handle_socket_error()
 *
 * when invoked after a socket operation it would check socket operation's
 * last error status and invoke siglongjmp incase the error is fatal. 
 */
static bool handle_socket_error(void)
{
	int		err;
	bool	abort = TRUE;

#ifdef WIN32
		err = WSAGetLastError();
			
		switch(err) 
		{
		
			case WSAEINTR:
			case WSAEBADF:
			case WSAEACCES:
			case WSAEFAULT:
			case WSAEINVAL:
			case WSAEMFILE:
				
			/*
			 * Windows Sockets definitions of regular Berkeley error constants
			 */
			case WSAEWOULDBLOCK:
			case WSAEINPROGRESS:
			case WSAEALREADY:
			case WSAENOTSOCK:
			case WSAEDESTADDRREQ:
			case WSAEMSGSIZE:
			case WSAEPROTOTYPE:
			case WSAENOPROTOOPT:
			case WSAEPROTONOSUPPORT:
			case WSAESOCKTNOSUPPORT:
			case WSAEOPNOTSUPP:
			case WSAEPFNOSUPPORT:
			case WSAEAFNOSUPPORT:
			case WSAEADDRINUSE:
			case WSAEADDRNOTAVAIL:
			case WSAENOBUFS:
			case WSAEISCONN:
			case WSAENOTCONN:
			case WSAETOOMANYREFS:
			case WSAETIMEDOUT:
			case WSAELOOP:
			case WSAENAMETOOLONG:
			case WSAEHOSTUNREACH:
			case WSAENOTEMPTY:
			case WSAEPROCLIM:
			case WSAEUSERS:
			case WSAEDQUOT:
			case WSAESTALE:
			case WSAEREMOTE:
				  
			/*
			 *	Extended Windows Sockets error constant definitions
			 */
			case WSASYSNOTREADY:
			case WSAVERNOTSUPPORTED:
			case WSANOTINITIALISED:
			case WSAEDISCON:
			case WSAENOMORE:
			case WSAECANCELLED:
			case WSAEINVALIDPROCTABLE:
			case WSAEINVALIDPROVIDER:
			case WSAEPROVIDERFAILEDINIT:
			case WSASYSCALLFAILURE:
			case WSASERVICE_NOT_FOUND:
			case WSATYPE_NOT_FOUND:
			case WSA_E_NO_MORE:
			case WSA_E_CANCELLED:
			case WSAEREFUSED:
				break;

			/*
			 *	Server should shut down its socket on these errors.
			 */		
			case WSAENETDOWN:
			case WSAENETUNREACH:
			case WSAENETRESET:
			case WSAECONNABORTED:
			case WSAESHUTDOWN:
			case WSAEHOSTDOWN:
			case WSAECONNREFUSED:
			case WSAECONNRESET:		
				abort = TRUE;
				break;
			
			default:
				;
		}

		if(abort)
		{
			LPVOID lpMsgBuf;
			FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,	NULL,err,
					   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),(LPTSTR) &lpMsgBuf,0, NULL );
		
			elog(COMMERROR,"%s", (char *)lpMsgBuf);		
			LocalFree(lpMsgBuf);
			
			siglongjmp(client_lost.m_savepoint, 1);
		}
		
#else	
		
		err = errno;
		switch(err) 
		{
			case EINTR:
			case ECONNREFUSED:
			case EPIPE:
			case ENOTCONN:
				abort =	TRUE;
				break;
				
			case ENOTSOCK:
			case EAGAIN:
			case EFAULT:
			case ENOMEM:
			case EINVAL:
			default:		
				break;
		}
		
		if(abort)
		{
			if(( err ) && ( err != EPIPE ))
				elog(COMMERROR, "%s", strerror(err)); 

			siglongjmp(client_lost.m_savepoint, 1);	
		}		
			
		errno = err;
#endif
		
	return abort;
}

////////////////////////////////////////////////////////////////////////////////


/*-------------------------------------------------------------------------------------
 * The shared hash table for global breakpoints. It is protected by 
 * breakpointLock
 *-------------------------------------------------------------------------------------
 */
static LWLockId  breakpointLock;
static HTAB    * globalBreakpoints = NULL;
static HTAB    * localBreakpoints  = NULL;

/*-------------------------------------------------------------------------------------
 * The size of Breakpoints is determined by globalBreakpointCount (should be a GUC)
 *-------------------------------------------------------------------------------------
 */
static int		globalBreakpointCount = 20;
static Size		breakpoint_hash_size;
static Size		breakcount_hash_size;

/*-------------------------------------------------------------------------------------
 * Another shared hash table which tracks number of breakpoints created
 * against each entity.
 *
 * It is also protected by breakpointLock, thus making operations on Breakpoints
 * BreakCounts atomic.
 *-------------------------------------------------------------------------------------
 */
static HTAB *globalBreakCounts;
static HTAB *localBreakCounts;

typedef struct BreakCountKey
{
	Oid			databaseId;
#if INCLUDE_PACKAGE_SUPPORT
	Oid		   	packageId;		/* Not used, but included to match BreakpointKey so casts work as expected */
#endif
    Oid			functionId;
} BreakCountKey;

typedef struct BreakCount
{
	BreakCountKey	key;
	int				count;
} BreakCount;

/*-------------------------------------------------------------------------------------
 * Prototypes for functions which operate on GlobalBreakCounts.
 *-------------------------------------------------------------------------------------
 */
static void initGlobalBreakpoints(int size);
static void initLocalBreakpoints(void);
static void initLocalBreakCounts(void);

static void   breakCountInsert(eBreakpointScope scope, BreakCountKey *key);
static void   breakCountDelete(eBreakpointScope scope, BreakCountKey *key);
static int    breakCountLookup(eBreakpointScope scope, BreakCountKey *key, bool *found);
static HTAB * getBreakpointHash(eBreakpointScope scope);
static HTAB * getBreakCountHash(eBreakpointScope scope);

static void reserveBreakpoints( void )
{
	breakpoint_hash_size = hash_estimate_size(globalBreakpointCount, sizeof(Breakpoint));
	breakcount_hash_size = hash_estimate_size(globalBreakpointCount, sizeof(BreakCount));

	RequestAddinShmemSpace( add_size( breakpoint_hash_size, breakcount_hash_size ));
	RequestAddinLWLocks( 1 );
}

static void
initializeHashTables(void)
{
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	initGlobalBreakpoints(globalBreakpointCount);

	LWLockRelease(AddinShmemInitLock);

	initLocalBreakpoints();
	initLocalBreakCounts();
}

static void
initLocalBreakpoints(void)
{
	HASHCTL	ctl = {0};

	ctl.keysize   = sizeof(BreakpointKey);
	ctl.entrysize = sizeof(Breakpoint);
	ctl.hash      = tag_hash;

	localBreakpoints = hash_create("Local Breakpoints", 128, &ctl, HASH_ELEM | HASH_FUNCTION);
}

static void
initGlobalBreakpoints(int tableEntries)
{
	bool   	  		found;
	LWLockId	   *lockId;

	if(( lockId = ((LWLockId *)ShmemInitStruct( "Global Breakpoint LockId", sizeof( LWLockId ), &found ))) == NULL )
		elog(ERROR, "out of shared memory");
	else
	{
		HASHCTL breakpointCtl = {0};
		HASHCTL breakcountCtl = {0};

		/*
		 * Request a LWLock, store the ID in breakpointLock and store the ID
		 * in shared memory so other processes can find it later.
		 */
		if (!found)
		    *lockId = breakpointLock = LWLockAssign();
		else
			breakpointLock = *lockId;

		/*
		 * Now create a shared-memory hash to hold our global breakpoints
		 */
		breakpointCtl.keysize   = sizeof(BreakpointKey);
		breakpointCtl.entrysize = sizeof(Breakpoint);
		breakpointCtl.hash 	  	= tag_hash;

		globalBreakpoints = ShmemInitHash("Global Breakpoints Table", tableEntries, tableEntries, &breakpointCtl, HASH_ELEM | HASH_FUNCTION);

		if (!globalBreakpoints)
			elog(FATAL, "could not initialize global breakpoints hash table");

		/*
		 * And create a shared-memory hash to hold our global breakpoint counts
		 */
		breakcountCtl.keysize   = sizeof(BreakCountKey);
		breakcountCtl.entrysize = sizeof(BreakCount);
		breakcountCtl.hash    	= tag_hash;

		globalBreakCounts = ShmemInitHash("Global BreakCounts Table", tableEntries, tableEntries, &breakcountCtl, HASH_ELEM | HASH_FUNCTION);

		if (!globalBreakCounts)
			elog(FATAL, "could not initialize global breakpoints count hash table");
	}
}

/* ---------------------------------------------------------
 * acquireLock()
 *
 *	This function waits for a lightweight lock that protects
 *  the breakpoint and breakcount hash tables at the given
 *	scope.  If scope is BP_GLOBAL, this function locks
 * 	breakpointLock. If scope is BP_LOCAL, this function
 *	doesn't lock anything because local breakpoints are,
 *	well, local (clever naming convention, huh?)
 */

static void
acquireLock(eBreakpointScope scope, LWLockMode mode)
{
	if( localBreakpoints == NULL )
		initializeHashTables();

	if (scope == BP_GLOBAL)
		LWLockAcquire(breakpointLock, mode);
}

/* ---------------------------------------------------------
 * releaseLock()
 *
 *	This function releases the lightweight lock that protects
 *  the breakpoint and breakcount hash tables at the given
 *	scope.  If scope is BP_GLOBAL, this function releases
 * 	breakpointLock. If scope is BP_LOCAL, this function
 *	doesn't do anything because local breakpoints are not
 *  protected by a lwlock.
 */

static void
releaseLock(eBreakpointScope scope)
{
	if (scope == BP_GLOBAL)
		LWLockRelease(breakpointLock);
}

/* ---------------------------------------------------------
 * BreakpointLookup()
 *
 * lookup the given global breakpoint hash key. Returns an instance
 * of Breakpoint structure
 */
Breakpoint *
BreakpointLookup(eBreakpointScope scope, BreakpointKey *key)
{
	Breakpoint	*entry;
	bool		 found;

	acquireLock(scope, LW_SHARED);
	entry = (Breakpoint *) hash_search( getBreakpointHash(scope), (void *) key, HASH_FIND, &found);
	releaseLock(scope);

	return entry;
}

/* ---------------------------------------------------------
 * BreakpointOnId()
 *
 * This is where we see the real advantage of the existence of BreakCounts.
 * It returns true if there is a global breakpoint on the given id, false
 * otherwise. The hash key of Global breakpoints table is a composition of Oid
 * and lineno. Therefore lookups on the basis of Oid only are not possible.
 * With this function however callers can determine whether a breakpoint is
 * marked on the given entity id with the cost of one lookup only.
 *
 * The check is made by looking up id in BreakCounts.
 */
bool
BreakpointOnId(eBreakpointScope scope, Oid funcOid)
{
	bool			found = false;
	BreakCountKey	key;

	key.databaseId = MyProc->databaseId;
	key.functionId = funcOid;

	acquireLock(scope, LW_SHARED);
	breakCountLookup(scope, &key, &found);
	releaseLock(scope);

	return found;
}

/* ---------------------------------------------------------
 * BreakpointInsert()
 *
 * inserts the global breakpoint (brkpnt) in the global breakpoints
 * hash table against the supplied key.
 */
bool
BreakpointInsert(eBreakpointScope scope, BreakpointKey *key, BreakpointData *data)
{
	Breakpoint	*entry;
	bool		 found;
	
	acquireLock(scope, LW_EXCLUSIVE);

	entry = (Breakpoint *) hash_search(getBreakpointHash(scope), (void *)key, HASH_ENTER, &found);

	if(found)
	{
		releaseLock(scope);
		return FALSE;
	}

	entry->data      = *data;
	entry->data.busy = FALSE;		/* Assume this breakpoint has not been nabbed by a target */

	
	/* register this insert in the count hash table*/
	breakCountInsert(scope, ((BreakCountKey *)key));

	releaseLock(scope);

	return( TRUE );
}

/* ---------------------------------------------------------
 * BreakpointInsertOrUpdate()
 *
 * inserts the global breakpoint (brkpnt) in the global breakpoints
 * hash table against the supplied key.
 */

bool
BreakpointInsertOrUpdate(eBreakpointScope scope, BreakpointKey *key, BreakpointData *data)
{
	Breakpoint	*entry;
	bool		 found;
	
	acquireLock(scope, LW_EXCLUSIVE);

	entry = (Breakpoint *) hash_search(getBreakpointHash(scope), (void *)key, HASH_ENTER, &found);

	if(found)
	{
		entry->data = *data;
		releaseLock(scope);
		return FALSE;
	}

	entry->data      = *data;
	entry->data.busy = FALSE;		/* Assume this breakpoint has not been nabbed by a target */

	
	/* register this insert in the count hash table*/
	breakCountInsert(scope, ((BreakCountKey *)key));
	
	releaseLock(scope);

	return( TRUE );
}

/* ---------------------------------------------------------
 * BreakpointBusySession()
 *
 * This function marks all breakpoints that belong to the given
 * proxy (identified by pid) as 'busy'. When a potential target
 * runs into a busy breakpoint, that means that the breakpoint
 * has already been hit by some other target and that other 
 * target is engaged in a conversation with the proxy (in other
 * words, the debugger proxy and debugger client are busy).
 *
 * We also copy all global breakpoints for the given proxy
 * to the local breakpoints list - that way, the target that's
 * actually interacting with the debugger client will continue
 * to hit those breakpoints until the target process ends.
 * 
 * When that debugging session ends, the debugger proxy calls
 * BreakpointFreeSession() to let other potential targets know
 * that the proxy can handle another target.
 *
 * FIXME: it might make more sense to simply move all of the
 *		  global breakpoints into the local hash instead, then
 *		  the debugger client would have to recreate all of
 *		  it's global breakpoints before waiting for another
 *		  target.
 */

void 
BreakpointBusySession(int pid)
{
	HASH_SEQ_STATUS status;
	Breakpoint	   *entry;

	acquireLock(BP_GLOBAL, LW_EXCLUSIVE);

	hash_seq_init(&status, getBreakpointHash(BP_GLOBAL));

	while((entry = (Breakpoint *) hash_seq_search(&status)))
	{
		if( entry->data.proxyPid == pid )
		{
			Breakpoint 	localCopy = *entry;

			entry->data.busy = TRUE;
			
			/* 
			 * Now copy the global breakpoint into the
			 * local breakpoint hash so that the target
			 * process will hit it again (other processes 
			 * will ignore it)
			 */

			localCopy.key.targetPid = MyProc->pid;

			BreakpointInsertOrUpdate(BP_LOCAL, &localCopy.key, &localCopy.data );
		}
	}

	releaseLock(BP_GLOBAL);
}

/* ---------------------------------------------------------
 * BreakpointFreeSession()
 *
 * This function marks all of the breakpoints that belong to 
 * the given proxy (identified by pid) as 'available'.  
 *
 * See the header comment for BreakpointBusySession() for
 * more information
 */

void
BreakpointFreeSession(int pid)
{
	HASH_SEQ_STATUS status;
	Breakpoint	   *entry;

	acquireLock(BP_GLOBAL, LW_EXCLUSIVE);

	hash_seq_init(&status, getBreakpointHash(BP_GLOBAL));

	while((entry = (Breakpoint *) hash_seq_search(&status)))
	{
		if( entry->data.proxyPid == pid )
			entry->data.busy = FALSE;
	}

	releaseLock(BP_GLOBAL);
}
/* ------------------------------------------------------------
 * BreakpointDelete()
 *
 * delete the given key from the global breakpoints hash table.
 */
bool 
BreakpointDelete(eBreakpointScope scope, BreakpointKey *key)
{
	Breakpoint	*entry;
	
	acquireLock(scope, LW_EXCLUSIVE);

	entry = (Breakpoint *) hash_search(getBreakpointHash(scope), (void *) key, HASH_REMOVE, NULL);

	if (entry)
 		breakCountDelete(scope, ((BreakCountKey *)key));
	
	releaseLock(scope);

	if(entry == NULL)
		return( FALSE );
	else
		return( TRUE );
}

/* ------------------------------------------------------------
 * BreakpointGetList()
 *
 *	This function returns an iterator (*scan) to the caller.
 *	The caller can use this iterator to scan through the 
 *	given hash table (either global or local).  The caller
 *  must call BreakpointReleaseList() when finished.
 */

void
BreakpointGetList(eBreakpointScope scope, HASH_SEQ_STATUS * scan)
{
	acquireLock(scope, LW_SHARED);
	hash_seq_init(scan, getBreakpointHash(scope));
}

/* ------------------------------------------------------------
 * BreakpointReleaseList()
 *
 *	This function releases the iterator lock returned by an 
 *	earlier call to BreakpointGetList().
 */

void 
BreakpointReleaseList(eBreakpointScope scope)
{
	releaseLock(scope);
}

/* ------------------------------------------------------------
 * BreakpointShowAll()
 *
 * sequentially traverse the Global breakpoints hash table and 
 * display all the break points via elog(INFO, ...)
 *
 * Note: The display format is still not decided.
 */

void
BreakpointShowAll(eBreakpointScope scope)
{
	HASH_SEQ_STATUS status;
	Breakpoint	   *entry;
	BreakCount     *count;
	
	acquireLock(scope, LW_SHARED);
	
	hash_seq_init(&status, getBreakpointHash(scope));

	elog(INFO, "BreakpointShowAll - %s", scope == BP_GLOBAL ? "global" : "local" );

	while((entry = (Breakpoint *) hash_seq_search(&status)))
	{
		elog(INFO, "Database(%d) function(%d) lineNumber(%d) targetPid(%d) proxyPort(%d) proxyPid(%d) busy(%c) tmp(%c)",
			 entry->key.databaseId,
			 entry->key.functionId,
			 entry->key.lineNumber,
			 entry->key.targetPid,
			 entry->data.proxyPort,
			 entry->data.proxyPid,
			 entry->data.busy ? 'T' : 'F',
			 entry->data.isTmp ? 'T' : 'F' );
	}

	elog(INFO, "BreakpointCounts" );

	hash_seq_init(&status, getBreakCountHash(scope));

	while((count = (BreakCount *) hash_seq_search(&status)))
	{
		elog(INFO, "Database(%d) function(%d) count(%d)",
			 count->key.databaseId,
			 count->key.functionId,
			 count->count );
	}
	releaseLock( scope );
}

/* ------------------------------------------------------------
 * BreakpointCleanupProc()
 *
 * sequentially traverse the Global breakpoints hash table and 
 * delete any breakpoints for the given process (identified by
 * its process ID).
 */

void BreakpointCleanupProc(int pid)
{
	HASH_SEQ_STATUS status;
	Breakpoint	   *entry;

	/*
	 * NOTE: we don't care about local breakpoints here, only
	 * global breakpoints
	 */

	acquireLock(BP_GLOBAL, LW_SHARED);
	
	hash_seq_init(&status, getBreakpointHash(BP_GLOBAL));

	while((entry = (Breakpoint *) hash_seq_search(&status)))
	{	
		if( entry->data.proxyPid == pid )
		{
			entry = (Breakpoint *) hash_search(getBreakpointHash(BP_GLOBAL), &entry->key, HASH_REMOVE, NULL);

			breakCountDelete(BP_GLOBAL, ((BreakCountKey *)&entry->key));
		}
	}

	releaseLock(BP_GLOBAL);
}

/* ==========================================================================
 * Function definitions for BreakCounts hash table
 *
 * Note: All the underneath functions assume that the caller has taken care
 * of all concurrency issues and thus does not do any locking
 * ==========================================================================
 */

static void
initLocalBreakCounts(void)
{
	HASHCTL ctl = {0};

	ctl.keysize   = sizeof(BreakCountKey);
	ctl.entrysize = sizeof(BreakCount);
	ctl.hash 	  = tag_hash;

	localBreakCounts = hash_create("Local Breakpoint Count Table",
								   32,
								  &ctl,
								  HASH_ELEM | HASH_FUNCTION );

	if (!globalBreakCounts)
		elog(FATAL, "could not initialize global breakpoints count hash table");
}

/* ---------------------------------------------------------
 * breakCountInsert()
 *
 * should be invoked when a breakpoint is added in Breakpoints
 */
void
breakCountInsert(eBreakpointScope scope, BreakCountKey *key)
{
	BreakCount *entry;
	bool		found;
	
	entry = hash_search(getBreakCountHash(scope), key, HASH_ENTER, &found);
	
	if (found)
		entry->count++;
	else
		entry->count = 1;
}

/* ---------------------------------------------------------
 * breakCountDelete()
 *
 * should be invoked when a breakpoint is removed from Breakpoints
 */
void
breakCountDelete(eBreakpointScope scope, BreakCountKey *key)
{
	BreakCount		*entry;
	
	entry = hash_search(getBreakCountHash(scope), key, HASH_FIND, NULL);
	
	if (entry)
	{
		entry->count--;
		
		/* remove entry only if entry->count is zero */
		if (entry->count == 0 )
			hash_search(getBreakCountHash(scope), key, HASH_REMOVE, NULL);
	}
		
}

/* ---------------------------------------------------------
 * breakCountLookup()
 *
 */
static int
breakCountLookup(eBreakpointScope scope, BreakCountKey *key, bool *found)
{
	BreakCount		*entry;
	
	entry = hash_search(getBreakCountHash(scope), key, HASH_FIND, found);
	
	if (entry)
		return entry->count;
		
	return -1;
}

/* ---------------------------------------------------------
 * getBreakpointHash()
 *
 *	Returns a pointer to the global or local breakpoint hash,
 *	depending on the given scope.
 */

static HTAB *
getBreakpointHash(eBreakpointScope scope )
{
	if( localBreakpoints == NULL )
		initializeHashTables();

	if (scope == BP_GLOBAL)
		return globalBreakpoints;
	else
		return localBreakpoints;
}

/* ---------------------------------------------------------
 * getBreakCountHash()
 *
 *	Returns a pointer to the global or local breakcount hash,
 *	depending on the given scope.
 */

static HTAB *
getBreakCountHash(eBreakpointScope scope)
{
	if( localBreakCounts == NULL )
		initializeHashTables();

	if (scope == BP_GLOBAL)
		return globalBreakCounts;
	else
		return localBreakCounts;
}
