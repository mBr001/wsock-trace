/*
 * A small and simple drop-in tracer for most normal Winsock calls.
 * Works best for MSVC since the stack-walking code requires the program's
 * PDB symbol-file to be present. And unfortunately MinGW/CygWin doesn't
 * produce PDB-symbols.
 *
 * Usage (MSVC):
 *   link with wsock_trace.lib instead of the system's ws32_2.lib. Thus
 *   most normal Winsock calls are traced on entry and exit.
 *
 * Usage (MinGW/CygWin):
 *   link with libwsock_trace.a instead of the system's libws32_2.a.
 *   I.e. copy it to a directory in $(LIBRARY_PATH) and use '-lwsock_trace'
 *   to link. The Makefile.MinGW already does the copying to $(MINGW32)/lib.
 *
 * Ignore warnings like:
 *   foo.obj : warning LNK4049: locally defined symbol _closesocket@4
 *             imported in bar().
 */

#define IN_WSOCK_TRACE_C

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <assert.h>
#include <limits.h>
#include <ctype.h>
#include <math.h>

#include "common.h"
#include "bfd_gcc.h"
#include "in_addr.h"
#include "init.h"
#include "dump.h"
#include "stkwalk.h"
#include "wsock_trace_lua.h"
#include "wsock_trace.h"

/* This used to be set in the Makefiles to '-DWSOCK_TRACE_DLL="..."'.
 * It was not so reliable. Hence hardcode it in DllMain() below.
 */
const char *wsock_trace_dll_name = NULL;

/* Keep track of number of calls to WSAStartup() and WSACleanup().
 */
int volatile cleaned_up = 0;
int volatile startup_count = 0;

static BOOL exclude_this = FALSE;

static const char *get_caller (ULONG_PTR ret_addr, ULONG_PTR ebp);
static const char *get_timestamp (void);

#if defined(USE_BFD) || defined(__clang__)
  static void test_get_caller (const void *from);
#endif

#if defined(WIN64) || defined(_WIN64)
  #define SOCK_RC_TYPE SOCKET
#else
  #define SOCK_RC_TYPE unsigned
#endif

/*
 * All 'p_function' pointers below are checked before use with this
 * macro. 'init_ptr()' makes sure 'wsock_trace_init()' is called once
 * and 'p_function' is not NULL.
*/
#if defined(USE_DETOURS)   /* \todo */
  #define INIT_PTR(ptr)    /* */
#else
  #define INIT_PTR(ptr) init_ptr ((const void**)&ptr, #ptr)
#endif

/*
 * There is some difference between some Winsock prototypes in MS-SDK's
 * versus MinGW headers. This is to fit the 'const struct timeval*'
 * parameter in 'select()' etc.
 */
#if defined(__MINGW32__)
  #define CONST_PTIMEVAL   const PTIMEVAL
#else
  #define CONST_PTIMEVAL   const struct timeval *
#endif

/*
 * More hacks for string parameters to 'WSAConnectByNameA()'. According to
 * MSDN:
 *   https://msdn.microsoft.com/en-us/library/windows/desktop/ms741557(v=vs.85).aspx
 *
 * they want an 'LPSTR' for 'node_name' and 'service_name'. Hence so does MinGW.
 * But the <winsock2.h> in the WindowsKit wants an 'LPCSTR'.
 *
 * Funny enough, 'WSAConnectByNameW()' doesn't want a 'const wide-string'.
 */
#if defined(_MSC_VER) && defined(_CRT_BEGIN_C_HEADER)
  #define CONST_LPSTR  LPCSTR
#else
  #define CONST_LPSTR  LPSTR    /* non-const 'char*' as per MSDN */
#endif

/*
 * A WSTRACE() macro for the WinSock calls we support.
 * This macro is used like 'WSTRACE ("WSAStartup (%u.%u) --> %s", args).'
 * Note:
 *   Do NOT add a trailing ".~0\n"; it's done in this macro.
 *
 * If "g_cfg.trace_caller == 0" or "WSAStartup" is in the
 * "g_cfg.exclude_list[]", the '!exclude_list_get("WSAStartup...")'
 * returns FALSE.
 */
#define WSTRACE(fmt, ...)                                   \
  do {                                                      \
    exclude_this = TRUE;                                    \
    if (g_cfg.trace_level > 0 && !exclude_list_get(fmt)) {  \
       exclude_this = FALSE;                                \
       wstrace_printf (TRUE, "~1* ~3%s~5%s: ~1",            \
                       get_timestamp(),                     \
                       get_caller (GET_RET_ADDR(),          \
                                   get_EBP()) );            \
       wstrace_printf (FALSE, fmt ".~0\n", ## __VA_ARGS__); \
    }                                                       \
  } while (0)


#if defined(__GNUC__) || defined(__clang__)
  #define GET_RET_ADDR()  (ULONG_PTR)__builtin_return_address (0)
#else
  #define GET_RET_ADDR()  0
#endif

#if (defined(_MSC_VER) && defined(_M_X64)) || \
    (defined(__GNUC__) && !defined(__i386__))
  #define get_EBP() 0

#elif defined(_MSC_VER) && defined(_X86_)
  __declspec(naked) static ULONG_PTR get_EBP (void)
  {
    __asm mov eax, ebp
    __asm ret
  }

#elif defined(__GNUC__)
  extern __inline__ ULONG_PTR get_EBP (void)
  {
    ULONG_PTR ebp;
    __asm__ __volatile__ ("movl %%ebp,%k0" : "=r" (ebp) : );
    return (ebp);
  }

#elif defined(__WATCOMC__)
  extern ULONG_PTR get_EBP (void);
  #pragma aux  get_EBP = \
          "mov eax, ebp" \
          modify [eax];

#else
  #error "Unsupported compiler."
#endif

static fd_set *last_rd_fd = NULL;
static fd_set *last_wr_fd = NULL;
static fd_set *last_ex_fd = NULL;

static void wstrace_printf (BOOL first_line,
                            _Printf_format_string_ const char *fmt, ...) ATTR_PRINTF (2,3);

typedef SOCKET  (WINAPI *func_socket) (int family, int type, int protocol);
typedef SOCKET  (WINAPI *func_accept) (SOCKET s, struct sockaddr *addr, int *addr_len);
typedef int     (WINAPI *func_bind) (SOCKET s, const struct sockaddr *addr, int addr_len);
typedef int     (WINAPI *func_shutdown) (SOCKET s, int how);
typedef int     (WINAPI *func_closesocket) (SOCKET s);
typedef int     (WINAPI *func_connect) (SOCKET s, const struct sockaddr *addr, int addr_len);
typedef int     (WINAPI *func_ioctlsocket) (SOCKET s, long opt, u_long *arg);
typedef int     (WINAPI *func_select) (int nfds, fd_set *rd_fd, fd_set *wr_fd, fd_set *ex_fd, CONST_PTIMEVAL timeout);
typedef int     (WINAPI *func_listen) (SOCKET s, int backlog);
typedef int     (WINAPI *func_recv) (SOCKET s, char *buf, int buf_len, int flags);
typedef int     (WINAPI *func_recvfrom) (SOCKET s, char *buf, int buf_len, int flags, struct sockaddr *from, int *from_len);
typedef int     (WINAPI *func_send) (SOCKET s, const char *buf, int buf_len, int flags);
typedef int     (WINAPI *func_sendto) (SOCKET s, const char *buf, int buf_len, int flags, const struct sockaddr *to, int to_len);
typedef int     (WINAPI *func_setsockopt) (SOCKET s, int level, int opt, const char *opt_val, int opt_len);
typedef int     (WINAPI *func_getsockopt) (SOCKET s, int level, int opt, char *opt_val, int *opt_len);
typedef int     (WINAPI *func_gethostname) (char *buf, int buf_len);
typedef int     (WINAPI *func_getpeername) (SOCKET s, struct sockaddr *name, int *namelen);
typedef int     (WINAPI *func_getsockname) (SOCKET s, struct sockaddr *name, int *namelen);
typedef u_short (WINAPI *func_htons) (u_short x);
typedef u_short (WINAPI *func_ntohs) (u_short x);
typedef u_long  (WINAPI *func_htonl) (u_long x);
typedef u_long  (WINAPI *func_ntohl) (u_long x);
typedef u_long  (WINAPI *func_inet_addr) (const char *addr);
typedef char *  (WINAPI *func_inet_ntoa) (struct in_addr addr);

typedef struct servent  *(WINAPI *func_getservbyport) (int port, const char *proto);
typedef struct servent  *(WINAPI *func_getservbyname) (const char *serv, const char *proto);
typedef struct hostent  *(WINAPI *func_gethostbyname) (const char *name);
typedef struct hostent  *(WINAPI *func_gethostbyaddr) (const char *addr, int len, int type);
typedef struct protoent *(WINAPI *func_getprotobynumber) (int num);
typedef struct protoent *(WINAPI *func_getprotobyname) (const char *name);

typedef int (WINAPI *func_getnameinfo) (const struct sockaddr *sa, socklen_t sa_len,
                                        char *buf, DWORD buf_size, char *serv_buf,
                                        DWORD serv_buf_size, int flags);

typedef int (WINAPI *func_getaddrinfo) (const char *host_name, const char *serv_name,
                                        const struct addrinfo *hints, struct addrinfo **res);

typedef void (WINAPI *func_freeaddrinfo) (struct addrinfo *ai);

typedef char *    (WINAPI *func_gai_strerrorA) (int err);
typedef wchar_t * (WINAPI *func_gai_strerrorW) (int err);

typedef int  (WINAPI *func_WSAStartup) (WORD version, LPWSADATA data);
typedef int  (WINAPI *func_WSACleanup) (void);
typedef int  (WINAPI *func_WSAGetLastError) (void);
typedef void (WINAPI *func_WSASetLastError) (int err);
typedef int  (WINAPI *func_WSAIoctl) (SOCKET s, DWORD code, VOID *vals, DWORD size_in,
                                      VOID *out_buf, DWORD out_size, DWORD *size_ret,
                                      WSAOVERLAPPED *ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE func);

typedef WSAEVENT (WINAPI *func_WSACreateEvent) (void);
typedef BOOL     (WINAPI *func_WSACloseEvent) (WSAEVENT);
typedef BOOL     (WINAPI *func_WSASetEvent) (WSAEVENT);
typedef BOOL     (WINAPI *func_WSAResetEvent) (WSAEVENT);
typedef int      (WINAPI *func_WSAEventSelect) (SOCKET s, WSAEVENT hnd, long net_ev);
typedef int      (WINAPI *func_WSAAsyncSelect) (SOCKET s, HWND wnd, unsigned int msg, long net_ev);

typedef int      (WINAPI *func_WSARecv) (SOCKET s, WSABUF *bufs, DWORD num_bufs, DWORD *num_bytes,
                                         DWORD *flags, WSAOVERLAPPED *ov,
                                         LPWSAOVERLAPPED_COMPLETION_ROUTINE func);


typedef int      (WINAPI *func_WSARecvFrom) (SOCKET s, WSABUF *bufs, DWORD num_bufs, DWORD *num_bytes,
                                             DWORD *flags, struct sockaddr *from, INT *from_len,
                                             WSAOVERLAPPED *ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE func);

typedef int      (WINAPI *func_WSARecvDisconnect) (SOCKET s, WSABUF *disconnect_data);
typedef int      (WINAPI *func_WSARecvEx) (SOCKET s, char *buf, int buf_len, int *flags);

typedef int      (WINAPI *func_WSASend) (SOCKET s, WSABUF *bufs, DWORD num_bufs, DWORD *num_bytes,
                                         DWORD flags, WSAOVERLAPPED *ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE func);

typedef int      (WINAPI *func_WSASendTo) (SOCKET s, WSABUF *bufs, DWORD num_bufs, DWORD *num_bytes,
                                           DWORD flags, const struct sockaddr *to, int to_len,
                                           WSAOVERLAPPED *ow, LPWSAOVERLAPPED_COMPLETION_ROUTINE func);

typedef int      (WINAPI *func_WSAConnect) (SOCKET s, const struct sockaddr *name, int namelen,
                                            WSABUF *caller_data, WSABUF *callee_data, QOS *SQOS,
                                            QOS *GQOS);


typedef BOOL   (WINAPI *func_WSAConnectByList) (SOCKET               s,
                                                SOCKET_ADDRESS_LIST *socket_addr_list,
                                                DWORD               *local_addr_len,
                                                SOCKADDR            *local_addr,
                                                DWORD               *remote_addr_len,
                                                SOCKADDR            *remote_addr,
                                                CONST_PTIMEVAL       timeout,
                                                WSAOVERLAPPED       *reserved);

typedef BOOL   (WINAPI *func_WSAConnectByNameA) (SOCKET              s,
                                                 CONST_LPSTR         node_name,
                                                 CONST_LPSTR         service_name,
                                                 DWORD              *local_addr_len,
                                                 SOCKADDR           *local_addr,
                                                 DWORD              *remote_addr_len,
                                                 SOCKADDR           *remote_addr,
                                                 CONST_PTIMEVAL      timeout,
                                                 WSAOVERLAPPED      *reserved);

typedef BOOL   (WINAPI *func_WSAConnectByNameW) (SOCKET              s,
                                                 LPWSTR              node_name,
                                                 LPWSTR              service_name,
                                                 DWORD              *local_addr_len,
                                                 SOCKADDR           *local_addr,
                                                 DWORD              *remote_addr_len,
                                                 SOCKADDR           *remote_addr,
                                                 CONST_PTIMEVAL      timeout,
                                                 WSAOVERLAPPED      *reserved);

typedef int    (WINAPI *func___WSAFDIsSet) (SOCKET s, fd_set *);

typedef SOCKET (WINAPI *func_WSASocketA) (int af, int type, int protocol,
                                          WSAPROTOCOL_INFOA *protocol_info,
                                          GROUP grp, DWORD dwFlags);

typedef SOCKET (WINAPI *func_WSASocketW) (int af, int type, int protocol,
                                          WSAPROTOCOL_INFOW *protocol_info,
                                          GROUP grp, DWORD dwFlags);

typedef int (WINAPI *func_WSADuplicateSocketA) (SOCKET, DWORD process_id,
                                                WSAPROTOCOL_INFOA *protocol_info);

typedef int (WINAPI *func_WSADuplicateSocketW) (SOCKET, DWORD process_id,
                                                WSAPROTOCOL_INFOW *protocol_info);

typedef INT (WINAPI *func_WSAAddressToStringA) (SOCKADDR          *address,
                                                DWORD              address_len,
                                                WSAPROTOCOL_INFOA *protocol_info,
                                                char              *result_string,
                                                DWORD             *result_string_len);

typedef INT (WINAPI *func_WSAAddressToStringW) (SOCKADDR          *address,
                                                DWORD              address_len,
                                                WSAPROTOCOL_INFOW *protocol_info,
                                                wchar_t           *result_string,
                                                DWORD             *result_string_len);

typedef INT (WINAPI *func_WSAStringToAddressA) (char              *addressStr,
                                                INT                addressFamily,
                                                WSAPROTOCOL_INFOA *protocolInfo,
                                                SOCKADDR          *address,
                                                INT               *addressLength);

typedef INT (WINAPI *func_WSAStringToAddressW) (wchar_t           *addressStr,
                                                INT                addressFamily,
                                                WSAPROTOCOL_INFOW *protocolInfo,
                                                SOCKADDR          *address,
                                                INT               *addressLength);

typedef BOOL (WINAPI *func_WSAGetOverlappedResult) (SOCKET s, WSAOVERLAPPED *ov, DWORD *transfered,
                                                    BOOL wait, DWORD *flags);

typedef int (WINAPI *func_WSAEnumNetworkEvents) (SOCKET s, WSAEVENT ev, WSANETWORKEVENTS *events);

typedef int (WINAPI *func_WSAEnumProtocolsA) (int *protocols, WSAPROTOCOL_INFOA *proto_info, DWORD *buf_len);
typedef int (WINAPI *func_WSAEnumProtocolsW) (int *protocols, WSAPROTOCOL_INFOW *proto_info, DWORD *buf_len);

typedef DWORD (WINAPI *func_WSAWaitForMultipleEvents) (DWORD           num_ev,
                                                       const WSAEVENT *ev,
                                                       BOOL            wait_all,
                                                       DWORD           timeout,
                                                       BOOL            alertable);

typedef DWORD (WINAPI *func_WaitForMultipleObjectsEx) (DWORD         num_ev,
                                                       const HANDLE *hnd,
                                                       BOOL          wait_all,
                                                       DWORD         timeout,
                                                       BOOL          alertable);

typedef int (WINAPI *func_WSACancelBlockingCall) (void);

typedef int (WINAPI *func_WSCGetProviderPath) (GUID    *provider_id,
                                               wchar_t *provider_dll_path,
                                               int     *provider_dll_path_len,
                                               int     *error);
/*
 * Windows-Vista functions.
 */
typedef INT   (WINAPI *func_inet_pton) (int family, const char *string, void *res);
typedef PCSTR (WINAPI *func_inet_ntop) (int family, const void *addr, char *string, size_t string_size);
typedef int   (WINAPI *func_WSAPoll) (WSAPOLLFD *fd_array, ULONG fds, int timeout);

/*
 * In ntdll.dll
 */
typedef USHORT (WINAPI *func_RtlCaptureStackBackTrace) (ULONG  frames_to_skip,
                                                        ULONG  frames_to_capture,
                                                        void **frames,
                                                        ULONG *trace_hash);

/*
 * All function pointers are file global.
 */
static func_WSAStartup               p_WSAStartup = NULL;
static func_WSACleanup               p_WSACleanup = NULL;
static func_WSAGetLastError          p_WSAGetLastError = NULL;
static func_WSASetLastError          p_WSASetLastError = NULL;
static func_WSASocketA               p_WSASocketA = NULL;
static func_WSASocketW               p_WSASocketW = NULL;
static func_WSADuplicateSocketA      p_WSADuplicateSocketA = NULL;
static func_WSADuplicateSocketW      p_WSADuplicateSocketW = NULL;
static func_WSAIoctl                 p_WSAIoctl = NULL;
static func_WSACreateEvent           p_WSACreateEvent = NULL;
static func_WSASetEvent              p_WSASetEvent = NULL;
static func_WSACloseEvent            p_WSACloseEvent = NULL;
static func_WSAResetEvent            p_WSAResetEvent = NULL;
static func_WSAEventSelect           p_WSAEventSelect = NULL;
static func_WSAAsyncSelect           p_WSAAsyncSelect = NULL;
static func_WSAAddressToStringA      p_WSAAddressToStringA = NULL;
static func_WSAAddressToStringW      p_WSAAddressToStringW = NULL;
static func_WSAStringToAddressA      p_WSAStringToAddressA = NULL;
static func_WSAStringToAddressW      p_WSAStringToAddressW = NULL;
static func_WSAPoll                  p_WSAPoll = NULL;
static func___WSAFDIsSet             p___WSAFDIsSet = NULL;
static func_accept                   p_accept = NULL;
static func_bind                     p_bind = NULL;
static func_closesocket              p_closesocket = NULL;
static func_connect                  p_connect = NULL;
static func_ioctlsocket              p_ioctlsocket = NULL;
static func_select                   p_select = NULL;
static func_gethostname              p_gethostname = NULL;
static func_listen                   p_listen = NULL;
static func_recv                     p_recv = NULL;
static func_recvfrom                 p_recvfrom = NULL;
static func_send                     p_send = NULL;
static func_sendto                   p_sendto = NULL;
static func_setsockopt               p_setsockopt = NULL;
static func_getsockopt               p_getsockopt = NULL;
static func_shutdown                 p_shutdown = NULL;
static func_socket                   p_socket = NULL;
static func_getservbyport            p_getservbyport = NULL;
static func_getservbyname            p_getservbyname = NULL;
static func_gethostbyname            p_gethostbyname = NULL;
static func_gethostbyaddr            p_gethostbyaddr = NULL;
static func_htons                    p_htons = NULL;
static func_ntohs                    p_ntohs = NULL;
static func_htonl                    p_htonl = NULL;
static func_ntohl                    p_ntohl = NULL;
static func_inet_addr                p_inet_addr = NULL;
static func_inet_ntoa                p_inet_ntoa = NULL;
static func_getpeername              p_getpeername = NULL;
static func_getsockname              p_getsockname = NULL;
static func_getprotobynumber         p_getprotobynumber = NULL;
static func_getprotobyname           p_getprotobyname = NULL;
static func_getnameinfo              p_getnameinfo = NULL;
static func_getaddrinfo              p_getaddrinfo = NULL;
static func_freeaddrinfo             p_freeaddrinfo = NULL;
static func_inet_pton                p_inet_pton = NULL;
static func_inet_ntop                p_inet_ntop = NULL;
static func_WSARecv                  p_WSARecv = NULL;
static func_WSARecvEx                p_WSARecvEx = NULL;
static func_WSARecvFrom              p_WSARecvFrom = NULL;
static func_WSARecvDisconnect        p_WSARecvDisconnect = NULL;
static func_WSASend                  p_WSASend = NULL;
static func_WSASendTo                p_WSASendTo = NULL;
static func_WSAConnect               p_WSAConnect = NULL;
static func_WSAConnectByNameA        p_WSAConnectByNameA = NULL;
static func_WSAConnectByNameW        p_WSAConnectByNameW = NULL;
static func_WSAConnectByList         p_WSAConnectByList = NULL;
static func_WSAGetOverlappedResult   p_WSAGetOverlappedResult = NULL;
static func_WSAEnumNetworkEvents     p_WSAEnumNetworkEvents = NULL;
static func_WSAEnumProtocolsA        p_WSAEnumProtocolsA = NULL;
static func_WSAEnumProtocolsW        p_WSAEnumProtocolsW = NULL;
static func_WSAWaitForMultipleEvents p_WSAWaitForMultipleEvents = NULL;
static func_WSACancelBlockingCall    p_WSACancelBlockingCall = NULL;
static func_WSCGetProviderPath       p_WSCGetProviderPath = NULL;
static func_WaitForMultipleObjectsEx p_WaitForMultipleObjectsEx = NULL;

static func_RtlCaptureStackBackTrace p_RtlCaptureStackBackTrace = NULL;

#if defined(__MINGW32__)
  static func_gai_strerrorA          p_gai_strerrorA = NULL;
  static func_gai_strerrorW          p_gai_strerrorW = NULL;
#endif

#define ADD_VALUE(opt,dll,func)   { opt, NULL, dll, #func, (void**)&p_##func }

static struct LoadTable dyn_funcs [] = {
              ADD_VALUE (0, "ws2_32.dll", WSAStartup),
              ADD_VALUE (0, "ws2_32.dll", WSACleanup),
              ADD_VALUE (0, "ws2_32.dll", WSAGetLastError),
              ADD_VALUE (0, "ws2_32.dll", WSASetLastError),
              ADD_VALUE (0, "ws2_32.dll", WSASocketA),
              ADD_VALUE (0, "ws2_32.dll", WSASocketW),
              ADD_VALUE (0, "ws2_32.dll", WSAIoctl),
              ADD_VALUE (0, "ws2_32.dll", WSACreateEvent),
              ADD_VALUE (0, "ws2_32.dll", WSACloseEvent),
              ADD_VALUE (0, "ws2_32.dll", WSAResetEvent),
              ADD_VALUE (0, "ws2_32.dll", WSASetEvent),
              ADD_VALUE (0, "ws2_32.dll", WSAEventSelect),
              ADD_VALUE (0, "ws2_32.dll", WSAAsyncSelect),
              ADD_VALUE (0, "ws2_32.dll", WSAAddressToStringA),
              ADD_VALUE (0, "ws2_32.dll", WSAAddressToStringW),
              ADD_VALUE (0, "ws2_32.dll", WSAStringToAddressA),
              ADD_VALUE (0, "ws2_32.dll", WSAStringToAddressW),
              ADD_VALUE (0, "ws2_32.dll", WSADuplicateSocketA),
              ADD_VALUE (0, "ws2_32.dll", WSADuplicateSocketW),
              ADD_VALUE (0, "ws2_32.dll", __WSAFDIsSet),
              ADD_VALUE (0, "ws2_32.dll", WSARecv),
              ADD_VALUE (0, "ws2_32.dll", WSARecvDisconnect),
              ADD_VALUE (0, "ws2_32.dll", WSARecvFrom),
              ADD_VALUE (1, "Mswsock.dll",WSARecvEx),
              ADD_VALUE (0, "ws2_32.dll", WSASend),
              ADD_VALUE (0, "ws2_32.dll", WSASendTo),
              ADD_VALUE (0, "ws2_32.dll", WSAConnect),
              ADD_VALUE (1, "ws2_32.dll", WSAConnectByList),   /* Windows Vista+ */
              ADD_VALUE (1, "ws2_32.dll", WSAConnectByNameA),  /* Windows Vista+ */
              ADD_VALUE (1, "ws2_32.dll", WSAConnectByNameW),  /* Windows Vista+ */
              ADD_VALUE (1, "ws2_32.dll", WSAPoll),
              ADD_VALUE (0, "ws2_32.dll", WSAGetOverlappedResult),
              ADD_VALUE (0, "ws2_32.dll", WSAEnumNetworkEvents),
              ADD_VALUE (1, "ws2_32.dll", WSAEnumProtocolsA),
              ADD_VALUE (1, "ws2_32.dll", WSAEnumProtocolsW),
              ADD_VALUE (0, "ws2_32.dll", WSACancelBlockingCall),
              ADD_VALUE (1, "ws2_32.dll", WSAWaitForMultipleEvents),
              ADD_VALUE (1, "ws2_32.dll", WSCGetProviderPath),
              ADD_VALUE (0, "ws2_32.dll", accept),
              ADD_VALUE (0, "ws2_32.dll", bind),
              ADD_VALUE (0, "ws2_32.dll", closesocket),
              ADD_VALUE (0, "ws2_32.dll", connect),
              ADD_VALUE (0, "ws2_32.dll", ioctlsocket),
              ADD_VALUE (0, "ws2_32.dll", select),
              ADD_VALUE (0, "ws2_32.dll", listen),
              ADD_VALUE (0, "ws2_32.dll", recv),
              ADD_VALUE (0, "ws2_32.dll", recvfrom),
              ADD_VALUE (0, "ws2_32.dll", send),
              ADD_VALUE (0, "ws2_32.dll", sendto),
              ADD_VALUE (0, "ws2_32.dll", setsockopt),
              ADD_VALUE (0, "ws2_32.dll", getsockopt),
              ADD_VALUE (0, "ws2_32.dll", shutdown),
              ADD_VALUE (0, "ws2_32.dll", socket),
              ADD_VALUE (0, "ws2_32.dll", getservbyport),
              ADD_VALUE (0, "ws2_32.dll", getservbyname),
              ADD_VALUE (0, "ws2_32.dll", gethostbyname),
              ADD_VALUE (0, "ws2_32.dll", gethostbyaddr),
              ADD_VALUE (0, "ws2_32.dll", gethostname),
              ADD_VALUE (0, "ws2_32.dll", htons),
              ADD_VALUE (0, "ws2_32.dll", ntohs),
              ADD_VALUE (0, "ws2_32.dll", htonl),
              ADD_VALUE (0, "ws2_32.dll", ntohl),
              ADD_VALUE (0, "ws2_32.dll", inet_addr),
              ADD_VALUE (0, "ws2_32.dll", inet_ntoa),
              ADD_VALUE (0, "ws2_32.dll", getpeername),
              ADD_VALUE (0, "ws2_32.dll", getsockname),
              ADD_VALUE (0, "ws2_32.dll", getprotobynumber),
              ADD_VALUE (0, "ws2_32.dll", getprotobyname),
              ADD_VALUE (0, "ws2_32.dll", getnameinfo),
              ADD_VALUE (0, "ws2_32.dll", getaddrinfo),
              ADD_VALUE (0, "ws2_32.dll", freeaddrinfo),
              ADD_VALUE (1, "ws2_32.dll", inet_pton),
              ADD_VALUE (1, "ws2_32.dll", inet_ntop),
              ADD_VALUE (0, "ntdll.dll",  RtlCaptureStackBackTrace),
           // ADD_VALUE (1, "kernel32.dll", WaitForMultipleObjectsEx),
#if defined(__MINGW32__)
              ADD_VALUE (1, "ws2_32.dll", gai_strerrorA),
              ADD_VALUE (1, "ws2_32.dll", gai_strerrorW),
#endif

#if 0  /* to-do? Allthough 'WSASendMsg()' seems to be an 'extension-function'
        * accessible only (?) via the 'WSAID_WSASENDMSG' GUID, it is present in
        * libws2_32.a in some MinGW distros
        */
              ADD_VALUE (1, "ws2_32.dll", WSASendMsg),
#endif
            };

void load_ws2_funcs (void)
{
  load_dynamic_table (dyn_funcs, DIM(dyn_funcs));

  if (p_RtlCaptureStackBackTrace == NULL)
      g_cfg.trace_caller = 0;

  if (p_inet_pton == NULL)
      p_inet_pton = (func_inet_pton) inet_pton;

  if (p_inet_ntop == NULL)
      p_inet_ntop = (func_inet_ntop) inet_ntop;
}

struct LoadTable *find_ws2_func_by_name (const char *func)
{
  return find_dynamic_table (dyn_funcs, DIM(dyn_funcs), func);
}

static void wstrace_printf (BOOL first_line, const char *fmt, ...)
{
  DWORD   err = GetLastError();   /* save error status */
  va_list args;

  va_start (args, fmt);

  if (first_line)
  {
    /* If stdout or stderr is redirected, we cannot get the cursor column.
     * So just wrap if told to do so. Also add an extra newline if writing
     * to a trace-file.
     */
    BOOL add_nl = (g_cfg.start_new_line && g_cfg.trace_file_device &&
                   (get_column() > 0 || g_cfg.stdout_redirected));

    if (add_nl || g_cfg.trace_file_okay)
       trace_putc ('\n');

    trace_indent (g_cfg.trace_indent);
  }
  else if (!first_line && !g_cfg.compact)
  {
    trace_putc ('\n');
    trace_indent (g_cfg.trace_indent+2);
  }

#if 0
  if (first_line && g_cfg.trace_time_format != TS_NONE)
  {
    trace_printf ("~3* %s: ~1", get_timestamp());
    fmt += 4;  /* step over the "~1* " we're called with */
  }
#endif

  trace_vprintf (fmt, args);

  SetLastError (err);  /* restore error status */
  va_end (args);
}

/*
 * Save and restore WSA error-state:
 *   pop = 0: WSAGetLastError()
 *   pop = 1: WSASetLastError()
 */
int WSAError_save_restore (int pop)
{
  static int err = 0;

  if (pop)
       (*p_WSASetLastError) (err);
  else err = (*p_WSAGetLastError)();
  return (err);
}

static const char *get_error (SOCK_RC_TYPE rc)
{
  if (rc != 0)
  {
    /* Longest result:
     *   "WSAECANCELLED: A call to WSALookupServiceEnd was made while this call was still processing. The call has been canceled (10103)" = 127 chars.
     */
    static char buf[150];
    const char *ret;
    int   err = WSAERROR_PUSH();

    ret = ws_strerror (err, buf, sizeof(buf));
    WSAERROR_POP();
    return (ret);
  }
  return ("No error");
}

/*
 * WSAAddressToStringA() returns the address AND the port in the 'buf'.
 * Like: 127.0.0.1:1234
 */
const char *sockaddr_str (const struct sockaddr *sa, const int *sa_len)
{
  static char buf [MAX_IP6_SZ+MAX_PORT_SZ+1];
  DWORD  size = sizeof(buf);
  DWORD  len  = sa_len ? *(DWORD*)sa_len : (DWORD)sizeof(*sa);

  WSAERROR_PUSH();
  if ((*p_WSAAddressToStringA)((SOCKADDR*)sa, len, NULL, buf, &size))
     strcpy (buf, "??");
  WSAERROR_POP();
  return (buf);
}

/*
 * Don't call the above 'WSAAddressToStringA()' for AF_INET/AF_INET6 addresses.
 * We do it ourself using the below sockaddr_str_port().
 */
const char *sockaddr_str2 (const struct sockaddr *sa, const int *sa_len)
{
  const char *p = sockaddr_str_port (sa, sa_len);

  if (!p)
     return sockaddr_str (sa, sa_len);
  return (p);
}

/*
 * This returns the address AND the port in the 'buf'.
 * Like: "127.0.0.1:1234"  for an AF_INET sockaddr. And
 *       "[0F::80::]:1234" for an AF_INET6 sockaddr.
 */
const char *sockaddr_str_port (const struct sockaddr *sa, const int *sa_len)
{
  const struct sockaddr_in  *sa4 = (const struct sockaddr_in*) sa;
  const struct sockaddr_in6 *sa6 = (const struct sockaddr_in6*) sa;
  static char buf [MAX_IP6_SZ+MAX_PORT_SZ+3];
  char       *end;

  if (!sa4)
     return ("<NULL>");

  if (sa4->sin_family == AF_INET)
  {
    snprintf (buf, sizeof(buf), "%u.%u.%u.%u:%d",
              sa4->sin_addr.S_un.S_un_b.s_b1,
              sa4->sin_addr.S_un.S_un_b.s_b2,
              sa4->sin_addr.S_un.S_un_b.s_b3,
              sa4->sin_addr.S_un.S_un_b.s_b4,
              swap16(sa4->sin_port));
    return (buf);
  }
  if (sa4->sin_family == AF_INET6)
  {
    buf[0] = '[';
    wsock_trace_inet_ntop6 ((const u_char*)&sa6->sin6_addr, buf+1, sizeof(buf)-1);
    end = strchr (buf, '\0');
    *end++ = ']';
    *end++ = ':';
    _itoa (swap16(sa6->sin6_port), end, 10);
    return (buf);
  }
  return (NULL);
}

static const char *inet_ntop2 (const char *addr, int family)
{
  static char buf [MAX_IP6_SZ+1];
  PCSTR  rc;

  WSAERROR_PUSH();
  rc  = (*p_inet_ntop) (family, (void*)addr, buf, sizeof(buf));

  if (!rc)
     strcpy (buf, "??");

  WSAERROR_POP();
  return (buf);
}

static __inline const char *uint_ptr_hexval (UINT_PTR val, char *buf)
{
  int i, j;

  buf[0] = '0';
  buf[1] = 'x';
  for (i = 0, j = 1+2*sizeof(val); i < 4*sizeof(val); i += 2, j--)
  {
    static const char hex_chars[] = "0123456789ABCDEF";
    unsigned idx = val % 16;

    val >>= 4;
    buf[j] = hex_chars [idx];
//  printf ("val: 0x%08lX, idx: %u, buf[%d]: %c\n", val, idx, j, buf[j]);
  }
  return (buf);
}

static const char *ptr_or_error (const void *ptr)
{
  static char buf [30];

  if (!ptr)
     return get_error (-1);

  memset (&buf, '\0', sizeof(buf));
  return uint_ptr_hexval ((UINT_PTR)ptr, buf);
}

static const char *socket_or_error (SOCK_RC_TYPE rc)
{
  static char buf [10];

  if (rc == INVALID_SOCKET || rc == SOCKET_ERROR)
     return get_error (rc);
  return _itoa ((int)rc, buf, 10);
}

/*
 * The actual Winsock functions we trace.
 */

EXPORT int WINAPI WSAStartup (WORD ver, WSADATA *data)
{
  int rc;

  INIT_PTR (p_WSAStartup);
  rc = (*p_WSAStartup) (ver, data);

  if (startup_count < INT_MAX)
     startup_count++;
  cleaned_up = 0;

  ENTER_CRIT();
  WSTRACE ("WSAStartup (%u.%u) --> %s",
           loBYTE(data->wVersion), hiBYTE(data->wVersion), get_error(rc));

  LUA_HOOK (rc, WSAStartup(ver,data));
  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI WSACleanup (void)
{
  int rc;

  INIT_PTR (p_WSACleanup);
  rc = (*p_WSACleanup)();

  ENTER_CRIT();
  WSTRACE ("WSACleanup() --> %s", get_error(rc));

  if (startup_count > 0)
  {
    startup_count--;
    cleaned_up = (startup_count == 0);
  }
  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI WSAGetLastError (void)
{
  int rc;

  INIT_PTR (p_WSAGetLastError);
  rc = (*p_WSAGetLastError)();

  ENTER_CRIT();
  WSTRACE ("WSAGetLastError() --> %s", get_error(rc));
  LEAVE_CRIT();
  return (rc);
}

EXPORT void WINAPI WSASetLastError (int err)
{
  INIT_PTR (p_WSASetLastError);
  (*p_WSASetLastError)(err);

  ENTER_CRIT();
  WSTRACE ("WSASetLastError (%d=%s)", err, get_error(err));
  LEAVE_CRIT();
}

EXPORT SOCKET WINAPI WSASocketA (int af, int type, int protocol,
                                 WSAPROTOCOL_INFOA *proto_info,
                                 GROUP group, DWORD flags)
{
  SOCKET rc;

  INIT_PTR (p_WSASocketA);
  rc = (*p_WSASocketA) (af, type, protocol, proto_info, group, flags);

  ENTER_CRIT();

  WSTRACE ("WSASocketA (%s, %s, %s, 0x%p, %d, %s) --> %s",
           socket_family(af), socket_type(type), protocol_name(protocol),
           proto_info, group, wsasocket_flags_decode(flags),
           socket_or_error(rc));

  if (!exclude_this && g_cfg.dump_wsaprotocol_info)
     dump_wsaprotocol_info ('A', proto_info, p_WSCGetProviderPath);

  LEAVE_CRIT();
  return (rc);
}

EXPORT SOCKET WINAPI WSASocketW (int af, int type, int protocol,
                                 WSAPROTOCOL_INFOW *proto_info,
                                 GROUP group, DWORD flags)
{
  SOCKET rc;

  INIT_PTR (p_WSASocketW);
  rc = (*p_WSASocketW) (af, type, protocol, proto_info, group, flags);

  ENTER_CRIT();

  WSTRACE ("WSASocketW (%s, %s, %s, 0x%p, %d, %s) --> %s",
           socket_family(af), socket_type(type), protocol_name(protocol),
           proto_info, group, wsasocket_flags_decode(flags),
           socket_or_error(rc));

  if (!exclude_this && g_cfg.dump_wsaprotocol_info)
     dump_wsaprotocol_info ('W', proto_info, p_WSCGetProviderPath);

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI WSADuplicateSocketA (SOCKET s, DWORD process_id, WSAPROTOCOL_INFOA *proto_info)
{
  int rc;

  INIT_PTR (p_WSADuplicateSocketA);
  rc = (*p_WSADuplicateSocketA) (s, process_id, proto_info);

  ENTER_CRIT();

  WSTRACE ("WSADuplicateSocketA (%u, proc-ID %lu, ...) --> %s",
           SOCKET_CAST(s), process_id, get_error(rc));

  if (!exclude_this && g_cfg.dump_wsaprotocol_info)
     dump_wsaprotocol_info ('A', proto_info, p_WSCGetProviderPath);

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI WSADuplicateSocketW (SOCKET s, DWORD process_id, WSAPROTOCOL_INFOW *proto_info)
{
  int rc;

  INIT_PTR (p_WSADuplicateSocketW);
  rc = (*p_WSADuplicateSocketW) (s, process_id, proto_info);

  ENTER_CRIT();

  WSTRACE ("WSADuplicateSocketW (%u, proc-ID %lu, ...) --> %s",
            SOCKET_CAST(s), process_id, get_error(rc));

  if (!exclude_this && g_cfg.dump_wsaprotocol_info)
     dump_wsaprotocol_info ('W', proto_info, p_WSCGetProviderPath);

  LEAVE_CRIT();
  return (rc);
}

EXPORT INT WINAPI WSAAddressToStringA (SOCKADDR          *address,
                                       DWORD              address_len,
                                       WSAPROTOCOL_INFOA *proto_info,
                                       char              *result_string,
                                       DWORD             *result_string_len)
{
  INT rc;

  INIT_PTR (p_WSAAddressToStringA);
  rc = (*p_WSAAddressToStringA) (address, address_len, proto_info,
                                 result_string, result_string_len);
  ENTER_CRIT();

  WSTRACE ("WSAAddressToStringA(). --> %s", rc == 0 ? result_string : get_error(rc));

  if (!exclude_this && g_cfg.dump_wsaprotocol_info)
     dump_wsaprotocol_info ('A', proto_info, p_WSCGetProviderPath);

  LEAVE_CRIT();
  return (rc);
}

EXPORT INT WINAPI WSAAddressToStringW (SOCKADDR          *address,
                                       DWORD              address_len,
                                       WSAPROTOCOL_INFOW *proto_info,
                                       wchar_t           *result_string,
                                       DWORD             *result_string_len)
{
  INT rc;

  INIT_PTR (p_WSAAddressToStringW);
  rc = (*p_WSAAddressToStringW) (address, address_len, proto_info,
                                 result_string, result_string_len);
  ENTER_CRIT();

  if (rc == 0)
       WSTRACE ("WSAAddressToStringW(). --> %" WCHAR_FMT, result_string);
  else WSTRACE ("WSAAddressToStringW(). --> %s", get_error(rc));

  if (!exclude_this && g_cfg.dump_wsaprotocol_info)
     dump_wsaprotocol_info ('W', proto_info, p_WSCGetProviderPath);

  LEAVE_CRIT();
  return (rc);
}

EXPORT INT WINAPI WSAStringToAddressA (char              *addressStr,
                                       INT                addressFamily,
                                       WSAPROTOCOL_INFOA *protocolInfo,
                                       SOCKADDR          *address,
                                       INT               *addressLength)
{
  INT rc;

  INIT_PTR (p_WSAStringToAddressA);
  rc = (*p_WSAStringToAddressA) (addressStr, addressFamily, protocolInfo, address, addressLength);
  ENTER_CRIT();

  if (rc == 0)
       WSTRACE ("WSAStringToAddressA(). --> ok");
  else WSTRACE ("WSAStringToAddressA(). --> %s", get_error(rc));

  LEAVE_CRIT();
  return (rc);
}

EXPORT INT WINAPI WSAStringToAddressW (wchar_t           *addressStr,
                                       INT                addressFamily,
                                       WSAPROTOCOL_INFOW *protocolInfo,
                                       SOCKADDR          *address,
                                       INT               *addressLength)
{
  INT rc;

  INIT_PTR (p_WSAStringToAddressW);
  rc = (*p_WSAStringToAddressW) (addressStr, addressFamily, protocolInfo, address, addressLength);
  ENTER_CRIT();

  if (rc == 0)
       WSTRACE ("WSAStringToAddressW(). --> ok");
  else WSTRACE ("WSAStringToAddressW(). --> %s", get_error(rc));

  LEAVE_CRIT();
  return (rc);
}


EXPORT int WINAPI WSAIoctl (SOCKET s, DWORD code, VOID *vals, DWORD size_in,
                            VOID *out_buf, DWORD out_size, DWORD *size_ret,
                            WSAOVERLAPPED *ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE func)
{
  const char *in_out = "";
  int   rc;

  INIT_PTR (p_WSAIoctl);
  rc = (*p_WSAIoctl) (s, code, vals, size_in, out_buf, out_size, size_ret, ov, func);

  ENTER_CRIT();

  if (code & IOC_INOUT)
    in_out = " (RW)";
  else if (code & IOC_OUT)
    in_out = " (R)";
  else if (code & IOC_IN)
    in_out = " (W)";
  else if (code & IOC_VOID)
    in_out = " (N)";

  WSTRACE ("WSAIoctl (%u, %s%s, ...) --> %s",
           SOCKET_CAST(s), get_sio_name(code), in_out, socket_or_error(rc));

  /* Dump known extension functions by GUID.
   */
  if (g_cfg.trace_level > 0 && code == SIO_GET_EXTENSION_FUNCTION_POINTER &&
      size_in == sizeof(GUID) && out_size == sizeof(void*))
     dump_extension_funcs (vals, out_buf);

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI WSAConnect (SOCKET s, const struct sockaddr *name, int namelen,
                              WSABUF *caller_data, WSABUF *callee_data, QOS *SQOS, QOS *GQOS)
{
  int rc;

  INIT_PTR (p_WSAConnect);
  rc = (*p_WSAConnect) (s, name, namelen, caller_data, callee_data, SQOS, GQOS);

  ENTER_CRIT();

  WSTRACE ("WSAConnect (%u, %s, 0x%p, 0x%p, ...) --> %s",
           SOCKET_CAST(s), sockaddr_str2(name,&namelen),
           caller_data, callee_data, socket_or_error(rc));

#if 0
  if (!exclude_this && g_cfg.dump_data)
   {
     dump_wsabuf (caller_data, 1);
     dump_wsabuf (callee_data, 1);
   }
#endif

  LEAVE_CRIT();
  return (rc);
}

EXPORT BOOL WINAPI WSAConnectByNameA (SOCKET         s,
                                      CONST_LPSTR    node_name,
                                      CONST_LPSTR    service_name,
                                      DWORD         *local_addr_len,
                                      SOCKADDR      *local_addr,
                                      DWORD         *remote_addr_len,
                                      SOCKADDR      *remote_addr,
                                      CONST_PTIMEVAL tv,
                                      WSAOVERLAPPED *reserved)
{
  BOOL rc;
  char tv_buf[30];

  INIT_PTR (p_WSAConnectByNameA);
  rc = (*p_WSAConnectByNameA) (s, node_name, service_name, local_addr_len, local_addr,
                               remote_addr_len, remote_addr, tv, reserved);

  ENTER_CRIT();

  exclude_this = (g_cfg.trace_level == 0 || exclude_list_get("WSAConnectByNameA"));
  if (!exclude_this)
  {
    if (!tv)
         strcpy (tv_buf, "unspec");
    else snprintf (tv_buf, sizeof(tv_buf), "tv=%ld.%06lds", tv->tv_sec, tv->tv_usec);

    WSTRACE ("WSAConnectByNameA (%u, %s, %s, %s, ...) --> %s",
             SOCKET_CAST(s), node_name, service_name, tv_buf, get_error(rc));
  }
  LEAVE_CRIT();
  return (rc);
}

EXPORT BOOL WINAPI WSAConnectByNameW (SOCKET         s,
                                      LPWSTR        node_name,
                                      LPWSTR        service_name,
                                      DWORD         *local_addr_len,
                                      SOCKADDR      *local_addr,
                                      DWORD         *remote_addr_len,
                                      SOCKADDR      *remote_addr,
                                      CONST_PTIMEVAL tv,
                                      WSAOVERLAPPED *reserved)
{
  BOOL rc;
  char tv_buf[30];

  INIT_PTR (p_WSAConnectByNameW);
  rc = (*p_WSAConnectByNameW) (s, node_name, service_name, local_addr_len, local_addr,
                               remote_addr_len, remote_addr, tv, reserved);

  ENTER_CRIT();

  exclude_this = (g_cfg.trace_level == 0 || exclude_list_get("WSAConnectByNameW"));
  if (!exclude_this)
  {
    if (!tv)
         strcpy (tv_buf, "unspec");
    else snprintf (tv_buf, sizeof(tv_buf), "tv=%ld.%06lds", tv->tv_sec, tv->tv_usec);

    WSTRACE ("WSAConnectByNameW (%u, %" WCHAR_FMT ", %" WCHAR_FMT ", %s, ...) --> %s",
             SOCKET_CAST(s), node_name, service_name, tv_buf, get_error(rc));
  }
  LEAVE_CRIT();
  return (rc);
}

EXPORT BOOL WINAPI WSAConnectByList (SOCKET               s,
                                     SOCKET_ADDRESS_LIST *socket_addr_list,
                                     DWORD               *local_addr_len,
                                     SOCKADDR            *local_addr,
                                     DWORD               *remote_addr_len,
                                     SOCKADDR            *remote_addr,
                                     CONST_PTIMEVAL       tv,
                                     WSAOVERLAPPED       *reserved)
{
  BOOL rc;
  char tv_buf[30];

  INIT_PTR (p_WSAConnectByList);
  rc = (*p_WSAConnectByList) (s, socket_addr_list, local_addr_len, local_addr,
                              remote_addr_len, remote_addr, tv, reserved);

  ENTER_CRIT();

  exclude_this = (g_cfg.trace_level == 0 || exclude_list_get("WSAConnectByList"));
  if (!exclude_this)
  {
    if (!tv)
         strcpy (tv_buf, "unspec");
    else snprintf (tv_buf, sizeof(tv_buf), "tv=%ld.%06lds", tv->tv_sec, tv->tv_usec);

    WSTRACE ("WSAConnectByList (%u, %s, ...) --> %s", SOCKET_CAST(s), tv_buf, get_error(rc));
  }
  LEAVE_CRIT();
  return (rc);
}

EXPORT WSAEVENT WINAPI WSACreateEvent (void)
{
  WSAEVENT ev;

  INIT_PTR (p_WSACreateEvent);
  ev = (*p_WSACreateEvent)();

  ENTER_CRIT();

  WSTRACE ("WSACreateEvent() --> 0x%" ADDR_FMT, ADDR_CAST(ev));

  LEAVE_CRIT();
  return (ev);
}

EXPORT BOOL WINAPI WSASetEvent (WSAEVENT ev)
{
  BOOL rc;

  INIT_PTR (p_WSASetEvent);
  rc = (*p_WSASetEvent) (ev);

  ENTER_CRIT();

  WSTRACE ("WSASetEvent (0x%" ADDR_FMT ") -> %s", ADDR_CAST(ev), get_error(rc));

  LEAVE_CRIT();
  return (rc);
}

EXPORT BOOL WINAPI WSACloseEvent (WSAEVENT ev)
{
  BOOL rc;

  INIT_PTR (p_WSACloseEvent);
  rc = (*p_WSACloseEvent) (ev);

  ENTER_CRIT();

  WSTRACE ("WSACloseEvent (0x%" ADDR_FMT ") -> %s", ADDR_CAST(ev), get_error(rc));

  LEAVE_CRIT();
  return (rc);
}

EXPORT BOOL WINAPI WSAResetEvent (WSAEVENT ev)
{
  BOOL rc;

  INIT_PTR (p_WSAResetEvent);
  rc = (*p_WSAResetEvent) (ev);

  ENTER_CRIT();

  WSTRACE ("WSAResetEvent (0x%" ADDR_FMT ") -> %s", ADDR_CAST(ev), get_error(rc));

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI WSAEventSelect (SOCKET s, WSAEVENT ev, long net_ev)
{
  int rc;

  INIT_PTR (p_WSAEventSelect);

  rc = (*p_WSAEventSelect) (s, ev, net_ev);

  ENTER_CRIT();

  WSTRACE ("WSAEventSelect (%u, 0x%" ADDR_FMT ", %s) -> %s",
           SOCKET_CAST(s), ADDR_CAST(ev), event_bits_decode(net_ev), get_error(rc));

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI WSAAsyncSelect (SOCKET s, HWND wnd, unsigned int msg, long net_ev)
{
  int rc;

  INIT_PTR (p_WSAAsyncSelect);
  rc = (*p_WSAAsyncSelect) (s, wnd, msg, net_ev);

  ENTER_CRIT();

  WSTRACE ("WSAAsyncSelect (%u, 0x%" ADDR_FMT ", %u, %s) -> %s",
           SOCKET_CAST(s), ADDR_CAST(wnd), msg, event_bits_decode(net_ev), get_error(rc));

  LEAVE_CRIT();
  return (rc);
}

#if !defined(_MSC_VER)
EXPORT
#endif

int WINAPI __WSAFDIsSet (SOCKET s, fd_set *fd)
{
  int     rc;
  unsigned _s = (unsigned) s;

  INIT_PTR (p___WSAFDIsSet);
  rc = (*p___WSAFDIsSet) (s, fd);

  ENTER_CRIT();

  if (fd == last_rd_fd)
       WSTRACE ("FD_ISSET (%u, \"rd fd_set\") --> %d", _s, rc);
  else if (fd == last_wr_fd)
       WSTRACE ("FD_ISSET (%u, \"wr fd_set\") --> %d", _s, rc);
  else if (fd == last_ex_fd)
       WSTRACE ("FD_ISSET (%u, \"ex fd_set\") --> %d", _s, rc);
  else WSTRACE ("FD_ISSET (%u, 0x%p) --> %d", _s, fd, rc);

  LEAVE_CRIT();
  return (rc);
}

/*
 * Since the MS SDK headers lacks an dllexport on this, this function is just
 * added to the imp-lib. Called from data.c.
 */
int raw__WSAFDIsSet (SOCKET s, fd_set *fd)
{
  return __WSAFDIsSet (s, fd);
}

EXPORT SOCKET WINAPI accept (SOCKET s, struct sockaddr *addr, int *addr_len)
{
  SOCKET rc;

  INIT_PTR (p_accept);
  rc = (*p_accept) (s, addr, addr_len);

  ENTER_CRIT();

  WSTRACE ("accept (%u, %s) --> %s",
           SOCKET_CAST(s), sockaddr_str2(addr,addr_len), socket_or_error(rc));

  if (!exclude_this && g_cfg.geoip_enable)
     dump_countries_sockaddr (addr);

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI bind (SOCKET s, const struct sockaddr *addr, int addr_len)
{
  int rc;

  INIT_PTR (p_bind);
  rc = (*p_bind) (s, addr, addr_len);

  ENTER_CRIT();

  WSTRACE ("bind (%u, %s) --> %s", SOCKET_CAST(s), sockaddr_str2(addr,&addr_len), get_error(rc));

  if (!exclude_this && g_cfg.geoip_enable)
     dump_countries_sockaddr (addr);

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI closesocket (SOCKET s)
{
  int rc;

  INIT_PTR (p_closesocket);
  rc = (*p_closesocket) (s);

  ENTER_CRIT();

  WSTRACE ("closesocket (%u) --> %s",  SOCKET_CAST(s), get_error(rc));

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI connect (SOCKET s, const struct sockaddr *addr, int addr_len)
{
  /*
   * todo: we may need to record (in ioctlsocket() below) if socket is non-blocking.
   *       It seems the WSAGetLastError() is not reliably returned on a non-blocking socket.
   */
  const struct sockaddr_in *sa = (const struct sockaddr_in*)addr;
  int   rc;

  INIT_PTR (p_connect);
  ENTER_CRIT();

  rc = (*p_connect) (s, addr, addr_len);

  WSTRACE ("connect (%u, %s, fam %s) --> %s",
           SOCKET_CAST(s), sockaddr_str2(addr, &addr_len),
           socket_family(sa->sin_family), get_error(rc));

  if (!exclude_this && g_cfg.geoip_enable)
     dump_countries_sockaddr (addr);

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI ioctlsocket (SOCKET s, long opt, u_long *argp)
{
  char arg[10] = "?";
  int  rc;

  INIT_PTR (p_ioctlsocket);
  rc = (*p_ioctlsocket) (s, opt, argp);

  ENTER_CRIT();

  if (argp)
     _itoa (*argp, arg, 10);

  WSTRACE ("ioctlsocket (%u, %s, %s) --> %s",
           SOCKET_CAST(s), ioctlsocket_cmd_name(opt), arg, get_error(rc));

  LEAVE_CRIT();
  return (rc);
}

#define FD_INPUT   "fd_input  ->"
#define FD_OUTPUT  "fd_output ->"

EXPORT int WINAPI select (int nfds, fd_set *rd_fd, fd_set *wr_fd, fd_set *ex_fd, CONST_PTIMEVAL tv)
{
  fd_set *rd_copy = NULL;
  fd_set *wr_copy = NULL;
  fd_set *ex_copy = NULL;
  char    tv_buf [50];
  int     rc;

  INIT_PTR (p_select);
  ENTER_CRIT();

  exclude_this = (g_cfg.trace_level == 0 || exclude_list_get("select"));

  if (!exclude_this)
  {
    if (!tv)
         strcpy (tv_buf, "unspec");
    else snprintf (tv_buf, sizeof(tv_buf), "tv=%ld.%06lds", tv->tv_sec, tv->tv_usec);

    if (g_cfg.dump_select)
    {
      rd_copy = copy_fd_set (rd_fd);
      wr_copy = copy_fd_set (wr_fd);
      ex_copy = copy_fd_set (ex_fd);
    }
  }

  rc = (*p_select) (nfds, rd_fd, wr_fd, ex_fd, tv);

  /* Remember last 'fd_set' for printing their types in FD_ISSET().
   */
  last_rd_fd = rd_fd;
  last_wr_fd = wr_fd;
  last_ex_fd = ex_fd;

  if (!exclude_this)
  {
    char buf [20];

    WSTRACE ("select (n=%d, %s, %s, %s, {%s}) --> (rc=%d) %s",
             nfds,
             rd_fd ? "rd" : "NULL",
             wr_fd ? "wr" : "NULL",
             ex_fd ? "ex" : "NULL",
             tv_buf, rc, rc > 0 ? _itoa(rc,buf,10) : get_error(rc));

    if (g_cfg.dump_select)
    {
      trace_indent (g_cfg.trace_indent+2);
      trace_puts ("~4" FD_INPUT);
      dump_select (rd_copy, wr_copy, ex_copy, g_cfg.trace_indent + 1 + sizeof(FD_OUTPUT));

      if (rd_copy)
         free (rd_copy);
      if (wr_copy)
         free (wr_copy);
      if (ex_copy)
         free (ex_copy);

      trace_indent (g_cfg.trace_indent+2);
      trace_puts (FD_OUTPUT);
      dump_select (rd_fd, wr_fd, ex_fd, g_cfg.trace_indent + 1 + sizeof(FD_OUTPUT));
      trace_puts ("~0");
    }
  }

  if (g_cfg.select_delay)
     SleepEx (g_cfg.select_delay, FALSE);

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI gethostname (char *buf, int buf_len)
{
  int rc;

  INIT_PTR (p_gethostname);
  rc = (*p_gethostname) (buf, buf_len);

  ENTER_CRIT();

  WSTRACE ("gethostname (->%.*s) --> %s", buf_len, buf, get_error(rc));

  LEAVE_CRIT();
 return (rc);
}

EXPORT int WINAPI listen (SOCKET s, int backlog)
{
  int rc;

  INIT_PTR (p_listen);
  rc = (*p_listen) (s, backlog);

  ENTER_CRIT();

  WSTRACE ("listen (%u, %d) --> %s",  SOCKET_CAST(s), backlog, get_error(rc));

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI recv (SOCKET s, char *buf, int buf_len, int flags)
{
  int rc;

  INIT_PTR (p_recv);
  rc = (*p_recv) (s, buf, buf_len, flags);

  ENTER_CRIT();

  exclude_this = (g_cfg.trace_level == 0 || exclude_list_get("recv"));

  if (rc >= 0)
  {
    if (flags & MSG_PEEK)
         g_cfg.counts.recv_peeked += rc;
    else g_cfg.counts.recv_bytes  += rc;
  }
  else
    g_cfg.counts.recv_errors++;

  if (!exclude_this)
  {
    char res[100];

    if (rc >= 0)
        sprintf (res, "%d bytes", rc);
    else strcpy (res, get_error(rc));

    WSTRACE ("recv (%u, 0x%p, %d, %s) --> %s",
             SOCKET_CAST(s), buf, buf_len, socket_flags(flags), res);

    if (rc > 0 && g_cfg.dump_data)
       dump_data (buf, rc);
  }

  if (g_cfg.recv_delay)
     SleepEx (g_cfg.recv_delay, FALSE);

  if (g_cfg.pcap.enable)
     write_pcap_packet (s, buf, buf_len, FALSE);

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI recvfrom (SOCKET s, char *buf, int buf_len, int flags, struct sockaddr *from, int *from_len)
{
  int rc;

  INIT_PTR (p_recvfrom);
  rc = (*p_recvfrom) (s, buf, buf_len, flags, from, from_len);

  ENTER_CRIT();

  exclude_this = (g_cfg.trace_level == 0 || exclude_list_get("recvfrom"));

  if (rc >= 0)
  {
    if (flags & MSG_PEEK)
         g_cfg.counts.recv_peeked += rc;
    else g_cfg.counts.recv_bytes  += rc;
  }
  else
    g_cfg.counts.recv_errors++;

  if (!exclude_this)
  {
    char res[100];

    if (rc >= 0)
       sprintf (res, "%d bytes", rc);
    else
    {
      strcpy (res, get_error(rc));
      if (rc == WSAEWOULDBLOCK)
         g_cfg.counts.recv_EWOULDBLOCK++;
    }

    WSTRACE ("recvfrom (%u, 0x%p, %d, %s, %s) --> %s",
             SOCKET_CAST(s), buf, buf_len, socket_flags(flags),
             sockaddr_str2(from,from_len), res);

    if (rc > 0 && g_cfg.dump_data)
       dump_data (buf, rc);

    if (g_cfg.geoip_enable)
       dump_countries_sockaddr (from);
  }

  if (g_cfg.recv_delay)
     SleepEx (g_cfg.recv_delay, FALSE);

  if (g_cfg.pcap.enable)
     write_pcap_packet (s, buf, buf_len, FALSE);

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI send (SOCKET s, const char *buf, int buf_len, int flags)
{
  int rc;

  INIT_PTR (p_send);
  rc = (*p_send) (s, buf, buf_len, flags);

  ENTER_CRIT();

  exclude_this = (g_cfg.trace_level == 0 || exclude_list_get("send"));

  if (rc >= 0)
       g_cfg.counts.send_bytes += rc;
  else g_cfg.counts.send_errors++;

  if (!exclude_this)
  {
    char res[100];

    if (rc >= 0)
         sprintf (res, "%d bytes", rc);
    else strcpy (res, get_error(rc));

    WSTRACE ("send (%u, 0x%p, %d, %s) --> %s",
             SOCKET_CAST(s), buf, buf_len, socket_flags(flags), res);

    if (g_cfg.dump_data)
       dump_data (buf, buf_len);
  }

  if (g_cfg.send_delay)
     SleepEx (g_cfg.send_delay, FALSE);

  if (g_cfg.pcap.enable)
     write_pcap_packet (s, buf, buf_len, TRUE);

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI sendto (SOCKET s, const char *buf, int buf_len, int flags, const struct sockaddr *to, int to_len)
{
  int rc;

  INIT_PTR (p_sendto);
  rc = (*p_sendto) (s, buf, buf_len, flags, to, to_len);

  ENTER_CRIT();

  exclude_this = (g_cfg.trace_level == 0 || exclude_list_get("sendto"));

  if (rc >= 0)
       g_cfg.counts.send_bytes += rc;
  else g_cfg.counts.send_errors++;

  if (!exclude_this)
  {
    char res[100];

    if (rc >= 0)
         sprintf (res, "%d bytes", rc);
    else strcpy (res, get_error(rc));

    WSTRACE ("sendto (%u, 0x%p, %d, %s, %s) --> %s",
             SOCKET_CAST(s), buf, buf_len, socket_flags(flags),
             sockaddr_str2(to,&to_len), res);

    if (g_cfg.dump_data)
       dump_data (buf, buf_len);

    if (g_cfg.geoip_enable)
       dump_countries_sockaddr (to);
  }

  if (g_cfg.send_delay)
     SleepEx (g_cfg.send_delay, FALSE);

  if (g_cfg.pcap.enable)
     write_pcap_packet (s, buf, buf_len, TRUE);

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI WSARecv (SOCKET s, WSABUF *bufs, DWORD num_bufs, DWORD *num_bytes,
                           DWORD *flags, WSAOVERLAPPED *ov,
                           LPWSAOVERLAPPED_COMPLETION_ROUTINE func)
{
  int rc;

  INIT_PTR (p_WSARecv);
  rc = (*p_WSARecv) (s, bufs, num_bufs, num_bytes, flags, ov, func);

  ENTER_CRIT();

  exclude_this = (g_cfg.trace_level == 0 || exclude_list_get("WSARecv"));

  if (rc == NO_ERROR)
       g_cfg.counts.recv_bytes += bufs->len * num_bufs;
  else g_cfg.counts.recv_errors++;

  if (!exclude_this)
  {
    char        res[100];
    const char *flg = flags ? socket_flags(*flags) : "NULL";

    if (rc == SOCKET_ERROR)
         strcpy (res, get_error(rc));
    else strcpy (res, "<Pending>");

    WSTRACE ("WSARecv (%u, 0x%p, %lu, %lu, <%s>, 0x%p, 0x%p) --> %s",
             SOCKET_CAST(s), bufs, num_bufs, *num_bytes, flg, ov, func, res);

    if (g_cfg.dump_data)
       dump_wsabuf (bufs, num_bufs);
  }

  if (g_cfg.recv_delay)
     SleepEx (g_cfg.recv_delay, FALSE);

  if (g_cfg.pcap.enable)
     write_pcap_packetv (s, bufs, num_bufs, FALSE);

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI WSARecvFrom (SOCKET s, WSABUF *bufs, DWORD num_bufs, DWORD *num_bytes,
                               DWORD *flags, struct sockaddr *from, INT *from_len,
                               WSAOVERLAPPED *ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE func)
{
  int rc;

  INIT_PTR (p_WSARecvFrom);
  rc = (*p_WSARecvFrom) (s, bufs, num_bufs, num_bytes, flags, from, from_len, ov, func);

  ENTER_CRIT();

  exclude_this = (g_cfg.trace_level == 0 || exclude_list_get("WSARecvFrom"));

  if (rc >= 0)
       g_cfg.counts.recv_bytes += rc;
  else g_cfg.counts.recv_errors++;

  if (!exclude_this)
  {
    char        res[100];
    char        nbytes[20];
    const char *flg = flags ? socket_flags(*flags) : "NULL";

    if (num_bytes)
         _itoa (*num_bytes, nbytes, 10);
    else strcpy (nbytes, "??");

    if (rc == SOCKET_ERROR)
         strcpy (res, get_error(rc));
    else strcpy (res, "<Pending>");

    WSTRACE ("WSARecvFrom (%u, 0x%p, %lu, %s, <%s>, %s, 0x%p, 0x%p) --> %s",
             SOCKET_CAST(s), bufs, num_bufs, nbytes, flg,
             sockaddr_str2(from,from_len), ov, func, res);

    if (rc > 0 && g_cfg.dump_data)
       dump_wsabuf (bufs, num_bufs);

    if (g_cfg.geoip_enable)
       dump_countries_sockaddr (from);
  }

  if (g_cfg.recv_delay)
     SleepEx (g_cfg.recv_delay, FALSE);

  if (g_cfg.pcap.enable)
     write_pcap_packetv (s, bufs, num_bufs, FALSE);

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI WSARecvEx (SOCKET s, char *buf, int buf_len, int *flags)
{
  int rc;

  INIT_PTR (p_WSARecvEx);
  rc = (*p_WSARecvEx) (s, buf, buf_len, flags);

  ENTER_CRIT();

  exclude_this = (g_cfg.trace_level == 0 || exclude_list_get("WSARecvEx"));

  if (rc >= 0)
       g_cfg.counts.recv_bytes += rc;
  else g_cfg.counts.recv_errors++;

  if (!exclude_this)
  {
    char res[100];
    const char *flg = flags ? socket_flags(*flags) : "NULL";

    if (rc == SOCKET_ERROR)
         strcpy (res, get_error(rc));
    else sprintf (res, "%d bytes", rc);

    WSTRACE ("WSARecvEx (%u, 0x%p, %d, <%s>) --> %s",
             SOCKET_CAST(s), buf, buf_len, flg, res);

    if (rc > 0 && g_cfg.dump_data)
       dump_data (buf, rc);
  }

  if (g_cfg.recv_delay)
     SleepEx (g_cfg.recv_delay, FALSE);

  if (g_cfg.pcap.enable)
     write_pcap_packet (s, buf, buf_len, FALSE);

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI WSARecvDisconnect (SOCKET s, WSABUF *disconnect_data)
{
  int rc;

  INIT_PTR (p_WSARecvDisconnect);
  rc = (*p_WSARecvDisconnect) (s, disconnect_data);

  ENTER_CRIT();

  WSTRACE ("WSARecvDisconnect (%u, 0x%p) --> %s",
           SOCKET_CAST(s), disconnect_data, get_error(rc));

  if (!exclude_this && rc == NO_ERROR && g_cfg.dump_data)
     dump_data (disconnect_data->buf, disconnect_data->len);

  if (g_cfg.recv_delay)
     SleepEx (g_cfg.recv_delay, FALSE);

  LEAVE_CRIT();
  return (rc);
}

/*
 * Count the number of bytes in an array of 'WSABUF' structures.
 */
static int count_wsabuf (const WSABUF *bufs, DWORD num_bufs)
{
  int i, rc = 0;

  for (i = 0; i < (int)num_bufs && bufs; i++, bufs++)
      rc += bufs->len;
  return (rc);
}

EXPORT int WINAPI WSASend (SOCKET s, WSABUF *bufs, DWORD num_bufs, DWORD *num_bytes,
                           DWORD flags, WSAOVERLAPPED *ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE func)
{
  int rc;

  INIT_PTR (p_WSASend);
  rc = (*p_WSASend) (s, bufs, num_bufs, num_bytes, flags, ov, func);

  ENTER_CRIT();

  if (rc == SOCKET_ERROR)
       g_cfg.counts.send_errors++;
  else g_cfg.counts.send_bytes += count_wsabuf (bufs, num_bufs);

  exclude_this = (g_cfg.trace_level == 0 || exclude_list_get("WSASend"));

  if (!exclude_this)
  {
    char res[100];
    char nbytes[20];

    if (num_bytes)
         _itoa (*num_bytes, nbytes, 10);
    else strcpy (nbytes, "??");

    if (rc == SOCKET_ERROR)
         strcpy (res, get_error(rc));
    else strcpy (res, "<Pending>");

    WSTRACE ("WSASend (%u, 0x%p, %lu, %s, <%s>, 0x%p, 0x%p) --> %s",
             SOCKET_CAST(s), bufs, num_bufs, nbytes,
             socket_flags(flags), ov, func, res);

    if (g_cfg.dump_data)
       dump_wsabuf (bufs, num_bufs);
  }

  if (g_cfg.send_delay)
     SleepEx (g_cfg.send_delay, FALSE);

  if (g_cfg.pcap.enable)
     write_pcap_packetv (s, bufs, num_bufs, TRUE);

  LEAVE_CRIT();
  return (rc);
}


EXPORT int WINAPI WSASendTo (SOCKET s, WSABUF *bufs, DWORD num_bufs, DWORD *num_bytes,
                             DWORD flags, const struct sockaddr *to, int to_len,
                             WSAOVERLAPPED *ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE func)
{
  int rc;

  INIT_PTR (p_WSASendTo);
  rc = (*p_WSASendTo) (s, bufs, num_bufs, num_bytes, flags, to, to_len, ov, func);

  ENTER_CRIT();

  if (rc == SOCKET_ERROR)
       g_cfg.counts.send_errors++;
  else g_cfg.counts.send_bytes += count_wsabuf (bufs, num_bufs);

  exclude_this = (g_cfg.trace_level == 0 || exclude_list_get("WSASendTo"));

  if (!exclude_this)
  {
    char res[100];
    char nbytes[20];

    if (num_bytes)
         _itoa (*num_bytes, nbytes, 10);
    else strcpy (nbytes, "??");

    if (rc == SOCKET_ERROR)
         strcpy (res, get_error(rc));
    else strcpy (res, "<Pending>");

    WSTRACE ("WSASendTo (%u, 0x%p, %lu, %s, <%s>, %s, 0x%p, 0x%p) --> %s",
             SOCKET_CAST(s), bufs, num_bufs, nbytes, socket_flags(flags),
             sockaddr_str2(to,&to_len), ov, func, res);

    if (g_cfg.dump_data)
       dump_wsabuf (bufs, num_bufs);

    if (g_cfg.geoip_enable)
       dump_countries_sockaddr (to);
  }

  if (g_cfg.send_delay)
     SleepEx (g_cfg.send_delay, FALSE);

  if (g_cfg.pcap.enable)
     write_pcap_packetv (s, bufs, num_bufs, TRUE);

  LEAVE_CRIT();
  return (rc);
}

EXPORT BOOL WINAPI WSAGetOverlappedResult (SOCKET s, WSAOVERLAPPED *ov, DWORD *transfered,
                                           BOOL wait, DWORD *flags)
{
  BOOL rc;
  char  xfer[10]  = "<N/A>";
  const char *flg = "<N/A>";

  INIT_PTR (p_WSAGetOverlappedResult);
  rc = (*p_WSAGetOverlappedResult) (s, ov, transfered, wait, flags);

  ENTER_CRIT();

  if (transfered)
     _itoa (*transfered, xfer, 10);
  if (flags)
     flg = wsasocket_flags_decode (*flags);

  WSTRACE ("WSAGetOverlappedResult (%u, 0x%p, %s, %d, %s) --> %s",
           SOCKET_CAST(s), ov, xfer, wait, flg, get_error(rc));

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI WSAEnumNetworkEvents (SOCKET s, WSAEVENT ev, WSANETWORKEVENTS *events)
{
  WSANETWORKEVENTS in_events;
  int rc, do_it = (g_cfg.trace_level > 0 && g_cfg.dump_wsanetwork_events);

  if (do_it && events)
       memcpy (&in_events, events, sizeof(in_events));
  else memset (&in_events, '\0', sizeof(in_events));

  INIT_PTR (p_WSAEnumNetworkEvents);
  rc = (*p_WSAEnumNetworkEvents) (s, ev, events);

  ENTER_CRIT();

  WSTRACE ("WSAEnumNetworkEvents (%u, 0x%" ADDR_FMT ", 0x%" ADDR_FMT ") --> %s",
           SOCKET_CAST(s), ADDR_CAST(ev), ADDR_CAST(events), get_error(rc));

  if (rc == 0 && !exclude_this && do_it)
     dump_events (&in_events, events);

  LEAVE_CRIT();
  return (rc);
}

/*
 * This function is what the command "netsh WinSock Show Catalog" uses.
 */
EXPORT int WINAPI WSAEnumProtocolsA (int *protocols, WSAPROTOCOL_INFOA *proto_info, DWORD *buf_len)
{
  char buf[50], *p = buf;
  int  i, rc, do_it = (g_cfg.trace_level > 0 && g_cfg.dump_wsaprotocol_info);

  INIT_PTR (p_WSAEnumProtocolsA);
  rc = (*p_WSAEnumProtocolsA) (protocols, proto_info, buf_len);

  ENTER_CRIT();

  if (do_it)
  {
    if (rc > 0)
         snprintf (buf, sizeof(buf), "num: %d, size: %lu", rc, *buf_len);
    else p = (char*) get_error (rc);
  }

  WSTRACE ("WSAEnumProtocolsA() --> %s", p);

  if (do_it && rc != SOCKET_ERROR && rc > 0 && !exclude_this)
  {
    for (i = 0; i < rc; i++)
    {
      trace_indent (g_cfg.trace_indent+2);
      trace_printf ("~1Provider Entry # %d:\n", i);
      dump_wsaprotocol_info ('A', proto_info + i, p_WSCGetProviderPath);
    }
  }

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI WSAEnumProtocolsW (int *protocols, WSAPROTOCOL_INFOW *proto_info, DWORD *buf_len)
{
  char buf[50], *p = buf;
  int  i, rc, do_it = (g_cfg.trace_level > 0 && g_cfg.dump_wsaprotocol_info);

  INIT_PTR (p_WSAEnumProtocolsW);
  rc = (*p_WSAEnumProtocolsW) (protocols, proto_info, buf_len);

  ENTER_CRIT();

  if (do_it)
  {
    if (rc > 0)
         snprintf (buf, sizeof(buf), "num: %d, size: %lu", rc, *buf_len);
    else p = (char*) get_error (rc);
  }

  WSTRACE ("WSAEnumProtocolsW() --> %s", p);

  if (do_it && rc != SOCKET_ERROR && rc > 0 && !exclude_this)
  {
    for (i = 0; i < rc; i++)
    {
      trace_indent (g_cfg.trace_indent+2);
      trace_printf ("~1Winsock Catalog Provider Entry #%d\n", i);
      dump_wsaprotocol_info ('W', proto_info + i, p_WSCGetProviderPath);
    }
  }

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI WSACancelBlockingCall (void)
{
  int rc;

  INIT_PTR (p_WSACancelBlockingCall);
  rc = (*p_WSACancelBlockingCall)();

  ENTER_CRIT();

  WSTRACE ("WSACancelBlockingCall() --> %s", get_error(rc));

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI WSAPoll (LPWSAPOLLFD fd_array, ULONG fds, int timeout)
{
  int        rc;
  WSAPOLLFD *fd_in = NULL;

  INIT_PTR (p_WSAPoll);

  if (!p_WSAPoll)
     return (0);

  ENTER_CRIT();

  exclude_this = (g_cfg.trace_level == 0 || exclude_list_get("WSAPoll"));

  if (!exclude_this && fd_array)
  {
    size_t size = fds * sizeof(*fd_in);

    fd_in = alloca (size);
    memcpy (fd_in, fd_array, size);
  }

  rc = (*p_WSAPoll) (fd_array, fds, timeout);

  if (!exclude_this)
  {
    char tbuf[20];

    if (timeout > 0)
         snprintf (tbuf, sizeof(tbuf), "%d ms", timeout);
    else if (timeout == 0)
         strcpy (tbuf, "return imm.");
    else strcpy (tbuf, "wait indef.");

    WSTRACE ("WSAPoll (0x%" ADDR_FMT ", %lu, %s) -> %s",
             ADDR_CAST(fd_array), fds, tbuf, socket_or_error(rc));

    trace_indent (g_cfg.trace_indent+2);
    trace_puts ("~4" FD_INPUT " ");
    if (fd_in)
         dump_wsapollfd (fd_in, fds, g_cfg.trace_indent + 2 + sizeof(FD_INPUT));
    else trace_puts ("None!\n");

    trace_indent (g_cfg.trace_indent+2);
    trace_puts (FD_OUTPUT " ");
    if (fd_array)
         dump_wsapollfd (fd_array, fds, g_cfg.trace_indent + 2 + sizeof(FD_OUTPUT));
    else trace_puts ("None!\n");
    trace_puts ("~0");
  }

  if (g_cfg.poll_delay)
     SleepEx (g_cfg.poll_delay, FALSE);

  LEAVE_CRIT();

  return (rc);
}

/* \todo:
 */
#if 0
EXPORT DWORD WaitForMultipleObjectsEx (DWORD         num_ev,
                                       const HANDLE *hnd,
                                       BOOL          wait_all,
                                       DWORD         timeout,
                                       BOOL          alertable)
{
}
#endif

EXPORT DWORD WINAPI WSAWaitForMultipleEvents (DWORD           num_ev,
                                              const WSAEVENT *ev,
                                              BOOL            wait_all,
                                              DWORD           timeout,
                                              BOOL            alertable)
{
  DWORD rc;

  if (p_WSAWaitForMultipleEvents == NULL)
  {
    /* This should maybe call '(*p_WaitForMultipleObjectsEx)()' when finished.
     */
    rc = WaitForMultipleObjectsEx (num_ev, (const HANDLE*)ev, wait_all, timeout, alertable);
  }
  else
  {
    INIT_PTR (p_WSAWaitForMultipleEvents);
    rc = (*p_WSAWaitForMultipleEvents) (num_ev, ev, wait_all, timeout, alertable);
  }

  ENTER_CRIT();

  exclude_this = (g_cfg.trace_level == 0 || exclude_list_get("WSAWaitForMultipleEvents"));

  if (!exclude_this)
  {
    char  extra[20] = "";
    char  time[20]  = "WSA_INFINITE";
    const char *err = "Unknown";

    if (rc == WSA_WAIT_FAILED)
         err = get_error (rc);
    else if (rc == WSA_WAIT_IO_COMPLETION)
         err = "WSA_WAIT_IO_COMPLETION";
    else if (rc == WSA_WAIT_TIMEOUT)
         err = "WSA_WAIT_TIMEOUT";
    else if (rc >= WSA_WAIT_EVENT_0 && rc < (WSA_WAIT_EVENT_0 + num_ev))
    {
      err = "WSA_WAIT_EVENT_0";
      if (wait_all)
           strcpy (extra, ", all");
      else snprintf (extra, sizeof(extra), ", %lu", rc-WSA_WAIT_EVENT_0);
    }

    if (timeout != WSA_INFINITE)
       snprintf (time, sizeof(time), "%lu ms", timeout);

    WSTRACE ("WSAWaitForMultipleEvents (%lu, 0x%p, %s, %s, %sALERTABLE) --> %s%s",
             num_ev, ev, wait_all ? "TRUE" : "FALSE", time, alertable ? "" : "not ", err, extra);
  }

  LEAVE_CRIT();
  return (rc);
}


EXPORT int WINAPI setsockopt (SOCKET s, int level, int opt, const char *opt_val, int opt_len)
{
  int rc;

  INIT_PTR (p_setsockopt);
  rc = (*p_setsockopt) (s, level, opt, opt_val, opt_len);

  ENTER_CRIT();

  WSTRACE ("setsockopt (%u, %s, %s, %s, %d) --> %s",
           SOCKET_CAST(s), socklevel_name(level), sockopt_name(level,opt),
           sockopt_value(opt_val,opt_len), opt_len,
           get_error(rc));

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI getsockopt (SOCKET s, int level, int opt, char *opt_val, int *opt_len)
{
  int rc;

  INIT_PTR (p_getsockopt);
  rc = (*p_getsockopt) (s, level, opt, opt_val, opt_len);

  ENTER_CRIT();

  WSTRACE ("getsockopt (%u, %s, %s, %s, %d) --> %s",
           SOCKET_CAST(s), socklevel_name(level), sockopt_name(level,opt),
           sockopt_value(opt_val, opt_len ? *opt_len : 0),
           opt_len ? *opt_len : 0, get_error(rc));

#if 0  /* \todo */
  if (level == SOL_SOCKET && !exclude_this && g_cfg.dump_wsaprotocol_info)
  {
    if (opt == SO_PROTOCOL_INFOA)
       dump_wsaprotocol_info ('A', opt_val, p_WSCGetProviderPath);
    else if (opt == SO_PROTOCOL_INFOW)
       dump_wsaprotocol_info ('W', opt_val, p_WSCGetProviderPath);
  }
#endif

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI shutdown (SOCKET s, int how)
{
  int rc;

  INIT_PTR (p_shutdown);
  rc = (*p_shutdown) (s, how);

  ENTER_CRIT();

  WSTRACE ("shutdown (%u, %d) --> %s",  SOCKET_CAST(s), how, get_error(rc));

  LEAVE_CRIT();
  return (rc);
}

EXPORT SOCKET WINAPI socket (int family, int type, int protocol)
{
  SOCKET rc;

  INIT_PTR (p_socket);
  rc = (*p_socket) (family, type, protocol);

  ENTER_CRIT();

  WSTRACE ("socket (%s, %s, %s) --> %s",
           socket_family(family), socket_type(type), protocol_name(protocol),
           socket_or_error(rc));

  LEAVE_CRIT();
  return (rc);
}

EXPORT struct servent *WINAPI getservbyport (int port, const char *proto)
{
  struct servent *rc;

  INIT_PTR (p_getservbyport);
  rc = (*p_getservbyport) (port, proto);

  ENTER_CRIT();

  WSTRACE ("getservbyport (%d, \"%s\") --> %s",
           swap16(port), proto, ptr_or_error(rc));

  if (rc && !exclude_this && g_cfg.dump_servent)
     dump_servent (rc);

  LEAVE_CRIT();
  return (rc);
}

EXPORT struct servent *WINAPI getservbyname (const char *serv, const char *proto)
{
  struct servent *rc;

  INIT_PTR (p_getservbyname);
  rc = (*p_getservbyname) (serv, proto);

  ENTER_CRIT();

  WSTRACE ("getservbyname (\"%s\", \"%s\") --> %s",
           serv, proto, ptr_or_error(rc));

  if (rc && !exclude_this && g_cfg.dump_servent)
     dump_servent (rc);

  LEAVE_CRIT();
  return (rc);
}

EXPORT struct hostent *WINAPI gethostbyname (const char *name)
{
  struct hostent *rc;

  INIT_PTR (p_gethostbyname);
  rc = (*p_gethostbyname) (name);

  ENTER_CRIT();

  WSTRACE ("gethostbyname (\"%s\") --> %s", name, ptr_or_error(rc));

  if (rc && !exclude_this && g_cfg.dump_hostent)
     dump_hostent (rc);

  if (rc && !exclude_this && g_cfg.geoip_enable)
     dump_countries (rc->h_addrtype, (const char**)rc->h_addr_list);

  LEAVE_CRIT();
  return (rc);
}

EXPORT struct hostent *WINAPI gethostbyaddr (const char *addr, int len, int type)
{
  struct hostent *rc;

  INIT_PTR (p_gethostbyaddr);
  rc = (*p_gethostbyaddr) (addr, len, type);

  ENTER_CRIT();

#if defined(USE_BFD)
  // test_get_caller (&gethostbyaddr);
#endif

  WSTRACE ("gethostbyaddr (%s, %d, %s) --> %s",
           inet_ntop2(addr,type), len, socket_family(type), ptr_or_error(rc));

#if defined(__clang__)
  // test_get_caller (&gethostbyaddr);
#endif

  if (!exclude_this)
  {
    if (rc && g_cfg.dump_hostent)
       dump_hostent (rc);

    if (g_cfg.geoip_enable)
    {
      if (rc)
         dump_countries (rc->h_addrtype, (const char**)rc->h_addr_list);
      else
      {
        const char *a[2];

        a[0] = addr;
        a[1] = NULL;
        dump_countries (type, a);
      }
    }
  }

  LEAVE_CRIT();
  return (rc);
}

EXPORT u_short WINAPI htons (u_short x)
{
  u_short rc;

  INIT_PTR (p_htons);
  rc = (*p_htons) (x);

  ENTER_CRIT();
  WSTRACE ("htons (%u) --> %u", x, rc);
  LEAVE_CRIT();
  return (rc);
}

EXPORT u_short WINAPI ntohs (u_short x)
{
  u_short rc;

  INIT_PTR (p_ntohs);
  rc = (*p_ntohs) (x);

  ENTER_CRIT();
  WSTRACE ("ntohs (%u) --> %u", x, rc);
  LEAVE_CRIT();
  return (rc);
}

EXPORT u_long WINAPI htonl (u_long x)
{
  u_long rc;

  INIT_PTR (p_htonl);
  rc = (*p_htonl) (x);

  ENTER_CRIT();
  WSTRACE ("htonl (%lu) --> %lu", x, rc);
  LEAVE_CRIT();
  return (rc);
}

EXPORT u_long WINAPI ntohl (u_long x)
{
  u_long rc;

  INIT_PTR (p_ntohl);
  rc = (*p_ntohl) (x);

  ENTER_CRIT();
  WSTRACE ("ntohl (%lu) --> %lu", x, rc);
  LEAVE_CRIT();
  return (rc);
}

EXPORT u_long WINAPI inet_addr (const char *addr)
{
  u_long rc;

  INIT_PTR (p_inet_addr);
  rc = (*p_inet_addr) (addr);

  ENTER_CRIT();
  WSTRACE ("inet_addr (\"%s\") -> %lu", addr, rc);
  LEAVE_CRIT();
  return (rc);
}

EXPORT char * WINAPI inet_ntoa (struct in_addr addr)
{
  char *rc;

  INIT_PTR (p_inet_ntoa);
  rc = (*p_inet_ntoa) (addr);

  ENTER_CRIT();
  WSTRACE ("inet_ntoa (%u.%u.%u.%u) --> %s",
           addr.S_un.S_un_b.s_b1,
           addr.S_un.S_un_b.s_b2,
           addr.S_un.S_un_b.s_b3,
           addr.S_un.S_un_b.s_b4, rc);

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI getpeername (SOCKET s, struct sockaddr *name, int *name_len)
{
  int rc;

  INIT_PTR (p_getpeername);
  rc = (*p_getpeername) (s, name, name_len);

  ENTER_CRIT();

  WSTRACE ("getpeername (%u, %s) --> %s",
           SOCKET_CAST(s), sockaddr_str2(name,name_len), get_error(rc));

  if (!exclude_this && g_cfg.geoip_enable)
     dump_countries_sockaddr (name);

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI getsockname (SOCKET s, struct sockaddr *name, int *name_len)
{
  int rc;

  INIT_PTR (p_getsockname);
  rc = (*p_getsockname) (s, name, name_len);

  ENTER_CRIT();

  WSTRACE ("getsockname (%u, %s) --> %s",
           SOCKET_CAST(s), sockaddr_str2(name,name_len), get_error(rc));

  if (!exclude_this && g_cfg.geoip_enable)
     dump_countries_sockaddr (name);

  LEAVE_CRIT();
  return (rc);
}

EXPORT struct protoent * WINAPI getprotobynumber (int num)
{
  struct protoent *rc;

  INIT_PTR (p_getprotobynumber);
  rc = (*p_getprotobynumber) (num);

  ENTER_CRIT();

  WSTRACE ("getprotobynumber (%d) --> %s", num, ptr_or_error(rc));

  if (rc && !exclude_this && g_cfg.dump_protoent)
     dump_protoent (rc);

  LEAVE_CRIT();
  return (rc);
}

EXPORT struct protoent * WINAPI getprotobyname (const char *name)
{
  struct protoent *rc;

  INIT_PTR (p_getprotobyname);
  rc = (*p_getprotobyname) (name);

  ENTER_CRIT();

  WSTRACE ("getprotobyname (\"%s\") --> %s", name, ptr_or_error(rc));

  if (rc && !exclude_this && g_cfg.dump_protoent)
     dump_protoent (rc);

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI getnameinfo (const struct sockaddr *sa, socklen_t sa_len,
                               char *host, DWORD host_size, char *serv_buf,
                               DWORD serv_buf_size, int flags)
{
  int rc;

  INIT_PTR (p_getnameinfo);
  rc = (*p_getnameinfo) (sa, sa_len, host, host_size, serv_buf, serv_buf_size, flags);

  ENTER_CRIT();

  WSTRACE ("getnameinfo (%s, ..., %s) --> %s",
           sockaddr_str2(sa,&sa_len), getnameinfo_flags_decode(flags), get_error(rc));

  if (!exclude_this)
  {
    if (rc == 0 && g_cfg.dump_nameinfo)
       dump_nameinfo (host, serv_buf, flags);

    if (g_cfg.geoip_enable)
       dump_countries_sockaddr (sa);
  }

  LEAVE_CRIT();
  return (rc);
}

EXPORT int WINAPI getaddrinfo (const char *host_name, const char *serv_name,
                               const struct addrinfo *hints, struct addrinfo **res)
{
  int rc;

  INIT_PTR (p_getaddrinfo);

  ENTER_CRIT();

  rc = (*p_getaddrinfo) (host_name, serv_name, hints, res);

#if 0
  if (rc != 0 && g_cfg.idna_enable && g_cfg.idna_helper && !IDNA_is_ASCII(host_name))
  {
    char   buf [MAX_HOST_LEN] = "?";
    size_t size;

    _strlcpy (buf, host_name, sizeof(buf));
    size = strlen (buf);
    if (IDNA_convert_to_ACE(buf,&size))
    {
      host_name = buf;
      rc = (*p_getaddrinfo) (host_name, serv_name, hints, res);
    }
  }
#endif

  WSTRACE ("getaddrinfo (%s, %s, <hints>, ...) --> %s\n"
           "%*shints: %s",
           host_name, serv_name, get_error(rc),
           g_cfg.trace_indent+4, "",
           hints ? get_addrinfo_hint(hints,g_cfg.trace_indent+3+sizeof("hints: ")) : "<none>");

  if (rc == 0 && *res && !exclude_this)
  {
    if (g_cfg.dump_data)
       dump_addrinfo (*res);

    if (g_cfg.geoip_enable)
       dump_countries_addrinfo (*res);
  }

  LEAVE_CRIT();
  return (rc);
}

EXPORT void WINAPI freeaddrinfo (struct addrinfo *ai)
{
  INIT_PTR (p_freeaddrinfo);
  (*p_freeaddrinfo) (ai);

  ENTER_CRIT();
  WSTRACE ("freeaddrinfo (0x%" ADDR_FMT ")", ADDR_CAST(ai));
  LEAVE_CRIT();
}

#if defined(__MINGW32__)
char * gai_strerrorA (int err)
{
  char *rc;

  INIT_PTR (p_gai_strerrorA);
  rc = (*p_gai_strerrorA) (err);

  ENTER_CRIT();
  WSTRACE ("gai_strerrorA (%d) -> %s", err, rc);
  LEAVE_CRIT();
  return (rc);
}

wchar_t * gai_strerrorW (int err)
{
  wchar_t *rc;

  INIT_PTR (p_gai_strerrorW);
  rc = (*p_gai_strerrorW) (err);

  ENTER_CRIT();
  WSTRACE ("gai_strerrorW (%d) -> %" WCHAR_FMT, err, rc);
  LEAVE_CRIT();
  return (rc);
}
#endif  /* __MINGW32__ */

#if (defined(__MINGW32__) && !defined(__MINGW64_VERSION_MAJOR)) || defined(__WATCOMC__)
  #define ADDRINFOW  void *
  #define PADDRINFOW void *
#endif

#define UNIMPLEMENTED() FATAL ("Call to unimplemented function %s().\n", __FUNCTION__)

EXPORT VOID WINAPI FreeAddrInfoW (PADDRINFOW addr_info)
{
  UNIMPLEMENTED();
}

EXPORT INT WINAPI GetAddrInfoW (PCWSTR           node_name,
                                PCWSTR           service_name,
                                const ADDRINFOW *hints,
                                PADDRINFOW      *result)
{
  UNIMPLEMENTED();
  return (-1);
}


EXPORT INT WINAPI GetNameInfoW (const SOCKADDR *sockaddr,
                                socklen_t       sockaddr_len,
                                PWCHAR          node_buf,
                                DWORD           node_buf_size,
                                PWCHAR          service_buf,
                                DWORD           service_buf_size,
                                INT             flags)
{
  UNIMPLEMENTED();
  return (-1);
}

/****************** Internal utility functions **********************************/

static const char *get_timestamp (void)
{
  static LARGE_INTEGER last = { S64_SUFFIX(0) };
  static char          buf [40];
  SYSTEMTIME           now;
  LARGE_INTEGER        ticks;
  int64                clocks;
  double               msec;

  switch (g_cfg.trace_time_format)
  {
    case TS_RELATIVE:
    case TS_DELTA:
         if (last.QuadPart == 0ULL)
            last.QuadPart = g_cfg.start_ticks;

         QueryPerformanceCounter (&ticks);
         if (g_cfg.trace_time_format == TS_RELATIVE)
              clocks = (int64) (ticks.QuadPart - g_cfg.start_ticks);
         else clocks = (int64) (ticks.QuadPart - last.QuadPart);

         last = ticks;
         msec = (double)clocks / ((double)g_cfg.clocks_per_usec * 1000.0);

#if defined(__CYGWIN__) || defined(__WATCOMC__)  /* Doesn't seems to have 'fmodl()' */
         sprintf (buf, "%.3f msec: ", msec);
#else
         {
           int         dec = (int) fmodl (msec, 1000.0);
           const char *sec = qword_str ((unsigned __int64) (msec/1000.0));

           sprintf (buf, "%s.%03d sec: ", sec, dec);
         }
#endif
         return (buf);

    case TS_ABSOLUTE:
         GetLocalTime (&now);
         sprintf (buf, "%02u:%02u:%02u: ", now.wHour, now.wMinute, now.wSecond);
         return (buf);

    case TS_NONE:
         return ("");
  }
  return (NULL);
}

static const char *get_caller (ULONG_PTR ret_addr, ULONG_PTR ebp)
{
  static int reentry = 0;
  char  *ret = NULL;

  if (reentry++)
  {
    ret = "get_caller() reentry. Breaking out.";
    g_cfg.reentries++;
  }
  else if (g_cfg.callee_level == 0)
  {
    ret = "~1";
  }
  else
  {
    CONTEXT ctx;
    HANDLE  thr = GetCurrentThread();

#if !defined(USE_BFD)       /* I.e. MSVC/clang-cl/Watcom, not gcc */
    void   *frames [10];
    USHORT  num_frames;

    WSAERROR_PUSH();
    memset (frames, '\0', sizeof(frames));
    num_frames = (*p_RtlCaptureStackBackTrace) (0, DIM(frames), frames, NULL);
    if (num_frames <= 2)
    {
      ret = "No stack";

#if defined(__clang__)
     /*
      * The flag '-Oy-' turns off frame pointer omission.
      */
      TRACE (2, "RtlCaptureStackBackTrace(): %d; add '-Oy-' to your CFLAGS.\n", num_frames);

#elif defined(_MSC_VER)
     /*
      * The MSVC flag '-Ox' (maximum optimizations) breaks the assumption for
      * 'RtlCaptureStackBackTrace()'. Hence warn strongly against it.
      */
      TRACE (2, "RtlCaptureStackBackTrace(): %d; Do not use '-Ox' in your CFLAGS.\n", num_frames);
#endif
      goto quit;
    }

#if !defined(__GNUC__)
    /*
     * For MSVC/clang-cl/Watcom (USE_BFD undefined), the passed 'ret_addr' is
     * always 0. We have to get it from 'frames[2]'.
     */
    ret_addr = (ULONG_PTR) frames [2];
#endif

#else
    WSAERROR_PUSH();
#endif

    /* We don't need a CONTEXT_FULL; only EIP+EBP (or RIP+RBP for x64). We want the caller's
     * address of a traced function (e.g. select()). Since we're called twice, that address
     * (for MSVC/PDB files) should be at frames[2]. For gcc, the RtlCaptureStackBackTrace()
     * doesn't work. I've had to use __builtin_return_addres(0) (='ret_addr').
     */
#ifdef _WIN64
    ctx.Rip = ret_addr;
    ctx.Rbp = ebp;       /* = 0 */
#else
    ctx.Eip = ret_addr;
    ctx.Ebp = ebp;
#endif

    ret = StackWalkShow (thr, &ctx);

#if !defined(USE_BFD)
    if (g_cfg.callee_level > 1 && num_frames > 2 && frames[3])
    {
      char *a, *b;

#ifdef _WIN64
      ctx.Rip = (ULONG_PTR) frames [3];
#else
      ctx.Eip = (ULONG_PTR) frames [3];
#endif

      a = strdup (ret);
      b = strdup (StackWalkShow (thr, &ctx));
      ret = malloc (strlen(a)+strlen(b)+50);
      sprintf (ret, "%s\n               %s", a, b);
    }
#endif

    WSAERROR_POP();
  }

quit:
  reentry--;
  return (ret);
}

#if defined(USE_BFD)  /* MinGW implied */

extern ULONG_PTR _image_base__;

#define FILL_STACK_ADDRESS(X) \
        stack_addr[X] = (ULONG_PTR) __builtin_return_address (X)

static void test_get_caller (const void *from)
{
  ULONG_PTR stack_addr [3];
  void     *frames [5];
  int       i, num;
  char      buf[100];

  memset (frames, '\0', sizeof(frames));
  num = (*p_RtlCaptureStackBackTrace) (0, DIM(frames), frames, NULL);

  FILL_STACK_ADDRESS (0);
  FILL_STACK_ADDRESS (1);
  FILL_STACK_ADDRESS (2);

  TRACE (4, "_image_base__: 0x%" ADDR_FMT ", from: 0x%" ADDR_FMT ", delta: 0x%" ADDR_FMT ".\n",
            _image_base__, ADDR_CAST(from), ADDR_CAST(from - _image_base__));

  for (i = 0; i < num; i++)
      TRACE (1, " frames[%d]: 0x%" ADDR_FMT "\n", i, ADDR_CAST(frames[i]));

#if 1
  for (i = 0; i < DIM(stack_addr); i++)
      TRACE (1, " stack_addr[%d]: 0x%" ADDR_FMT "\n", i, ADDR_CAST(stack_addr[i]));
#endif

  BFD_get_function_name (stack_addr[0], buf, sizeof(buf));
  TRACE (1, "BFD_get_function_name(stack_addr[0]): %s\n", buf);

  BFD_get_function_name (stack_addr[1], buf, sizeof(buf));
  TRACE (1, "BFD_get_function_name(stack_addr[1]): %s\n", buf);

  BFD_get_function_name (stack_addr[2], buf, sizeof(buf));
  TRACE (1, "BFD_get_function_name(stack_addr[2]): %s\n", buf);

  BFD_get_function_name (10 + (ULONG_PTR)from, buf, sizeof(buf));
  TRACE (1, "BFD_get_function_name(from): %s, frames[0] - from: 0x%02lX\n",
            buf, (DWORD) (ADDR_CAST(frames[0]) - ADDR_CAST(from)));
  exit (0);
}

#elif defined(__clang__)

#include <imagehlp.h>

static void test_get_caller (const void *from)
{
  CONTEXT     ctx;
  HANDLE      thr = GetCurrentThread();
  void       *frames [5];
  const char *ret;
  int         i, num;

  memset (frames, '\0', sizeof(frames));
  num = (*p_RtlCaptureStackBackTrace) (1, DIM(frames), frames, NULL);

  for (i = 0; i < num; i++)
  {
    ret = "<none>";
    if (i == 0)
    {
      ctx.Eip = (DWORD) frames[i];
      ctx.Ebp = 0;
      ret = StackWalkShow (thr, &ctx);
    }
    TRACE (1, "frames[%d]: 0x%" ADDR_FMT ", ret: %s\n", i, ADDR_CAST(frames[i]), ret);
  }
  ARGSUSED (from);

#if 0
  void *stk_p = NULL;
  STACKFRAME64 *stk;

  memset (&ctx, '\0', sizeof(ctx));
  ctx.ContextFlags = CONTEXT_FULL;
  GetThreadContext (thr, &ctx);
  ctx.Eip = (DWORD) from;
  ctx.Ebp = 0;
  ret = StackWalkShow2 (thr, &ctx, &stk_p);
  TRACE (1, "from: 0x%" ADDR_FMT ", ret: %s\n", ADDR_CAST(from), ret);

  stk = (STACKFRAME64*) stk_p;
  ctx.Eip = stk->AddrPC.Offset;
  ctx.Ebp = 0;
  ret = StackWalkShow2 (thr, &ctx, &stk_p);
  TRACE (1, "from: 0x%" ADDR_FMT ", ret: %s\n", ADDR_CAST(from), ret);
#endif
}
#endif  /* USE_BFD */

#include "wsock_trace.rc"

static const char *set_dll_name (void)
{
#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__) || defined(__ia64__)
  #define X_SUFFIX "_x64"
#else
  #define X_SUFFIX ""
#endif

  return (RC_BASENAME X_SUFFIX ".dll");
}


BOOL WINAPI DllMain (HINSTANCE hinstDLL, DWORD dwReason, LPVOID lpvReserved)
{
  char  note[30] = "";
  DWORD tid;

  if (dwReason == DLL_PROCESS_ATTACH)
     wsock_trace_dll_name = set_dll_name();

  if (ws_trace_base && hinstDLL == ws_trace_base)
     snprintf (note, sizeof(note), " (%s)", wsock_trace_dll_name);

  switch (dwReason)
  {
    case DLL_PROCESS_ATTACH:
         crtdbg_init();
         wsock_trace_init();
#if 0
         smartlist_add (thread_list, GetCurrentThreadId());
#endif
         break;

    case DLL_PROCESS_DETACH:
         wsock_trace_exit();
         crtdbg_exit();
         break;

    case DLL_THREAD_ATTACH:
         tid = GetCurrentThreadId();
         g_cfg.counts.dll_attach++;
         TRACE (3, "  DLL_THREAD_ATTACH. hinstDLL: 0x%" ADDR_FMT "%s, thr-id: %lu.\n",
                ADDR_CAST(hinstDLL), note, tid);

         /* \todo:
          *   Add this 'tid' as a new thread to a 'smartlist_t' and call 'print_thread_times()'
          *   for it when 'DLL_PROCESS_DETACH' is received.
          */
         break;

    case DLL_THREAD_DETACH:
         tid = GetCurrentThreadId();
         g_cfg.counts.dll_detach++;
         TRACE (3, "  DLL_THREAD_DETACH. hinstDLL: 0x%" ADDR_FMT "%s, thr-id: %lu.\n",
               ADDR_CAST(hinstDLL), note, tid);
         if (g_cfg.trace_level >= 3)
         {
           extern void print_thread_times (HANDLE thread);
           HANDLE hnd = OpenThread (THREAD_QUERY_INFORMATION, FALSE, tid);

           print_thread_times (hnd);
         }
         /* \todo:
          *   Instead of calling 'print_thread_times()' here, add this 'tid' as
          *   dying thread and call 'print_thread_times()' for all threads (alive or dead) when
          *   'DLL_PROCESS_DETACH' is received.
          */
         break;
  }

  ARGSUSED (lpvReserved);
  return (TRUE);
}


