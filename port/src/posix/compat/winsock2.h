/* winsock2.h for the Linux and Switch builds: BSD sockets under the Winsock
 * names netplay/np_net.c uses. */
#ifndef OT_COMPAT_WINSOCK2_H
#define OT_COMPAT_WINSOCK2_H
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)
#define closesocket(s) close(s)
#define WSAGetLastError() (errno)
typedef struct WSAData { unsigned short wVersion; } WSADATA;
#ifndef MAKEWORD
#define MAKEWORD(a, b) ((unsigned short)(((unsigned char)(a)) | ((unsigned short)((unsigned char)(b))) << 8))
#endif
/* Starts the network service where the host needs it (the Switch); 0 = ready. */
int WSAStartup(unsigned short version, WSADATA *data);
#endif
