#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

struct sockaddr_un { unsigned short sun_family; char sun_path[108]; };
#define SOCK_PATH "C:\\Temp\\afunix_test.sock"
#define BIG_BUF 65536
#define NUM_RECV 64
#define NUM_SEND 64

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
    CreateDirectoryA("C:\\Temp", NULL);

    SOCKET srv = WSASocketA(AF_UNIX, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path)-1);
    DeleteFileA(SOCK_PATH);
    bind(srv, (struct sockaddr*)&addr, sizeof(addr));
    listen(srv, 1);

    SOCKET cli = WSASocketA(AF_UNIX, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    connect(cli, (struct sockaddr*)&addr, sizeof(addr));
    SOCKET conn = accept(srv, NULL, NULL);
    printf("[+] connected\n"); fflush(stdout);

    // Post 64 pending receives on conn — don't drain
    char *rbuf = calloc(4096, 1);
    OVERLAPPED rov[NUM_RECV] = {0};
    WSABUF rwb[NUM_RECV];
    HANDLE rev[NUM_RECV];
    for (int i = 0; i < NUM_RECV; i++) {
        rev[i] = CreateEvent(NULL, TRUE, FALSE, NULL);
        rov[i].hEvent = rev[i];
        rwb[i].buf = rbuf;
        rwb[i].len = 4096;
        DWORD recvd = 0, flags = 0;
        WSARecv(conn, &rwb[i], 1, &recvd, &flags, &rov[i], NULL);
    }
    printf("[+] %d pending recvs posted\n", NUM_RECV); fflush(stdout);

    // Post 64 large sends on cli — back up the queue
    char *sbuf = calloc(BIG_BUF, 1);
    memset(sbuf, 'A', BIG_BUF);
    OVERLAPPED sov[NUM_SEND] = {0};
    WSABUF swb[NUM_SEND];
    HANDLE sev[NUM_SEND];
    for (int i = 0; i < NUM_SEND; i++) {
        sev[i] = CreateEvent(NULL, TRUE, FALSE, NULL);
        sov[i].hEvent = sev[i];
        swb[i].buf = sbuf;
        swb[i].len = BIG_BUF;
        DWORD sent = 0;
        WSASend(cli, &swb[i], 1, &sent, 0, &sov[i], NULL);
    }
    printf("[+] %d pending sends posted\n", NUM_SEND); fflush(stdout);

    // Staggered cancel burst — one at a time
    printf("[*] cancelling receives one by one\n"); fflush(stdout);
    for (int i = 0; i < NUM_RECV; i++) {
        CancelIoEx((HANDLE)conn, &rov[i]);
        Sleep(0);
    }
    printf("[*] cancel burst done\n"); fflush(stdout);

    Sleep(5000);
    closesocket(conn);
    closesocket(cli);
    closesocket(srv);
    DeleteFileA(SOCK_PATH);
    WSACleanup();
    return 0;
}
