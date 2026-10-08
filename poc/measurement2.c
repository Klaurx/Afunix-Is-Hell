#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "Ws2_32.lib")

struct sockaddr_un {
    unsigned short sun_family;
    char sun_path[108];
};

#define SOCK_PATH "C:\\Temp\\afunix_test.sock"
#define BIG_BUF 65536
#define NUM_RECV 64
#define NUM_SEND 64

int main(void)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("[-] WSAStartup failed\n");
        return 1;
    }

    CreateDirectoryA("C:\\Temp", NULL);
    DeleteFileA(SOCK_PATH);

    SOCKET srv = WSASocketA(AF_UNIX, SOCK_STREAM, 0, NULL, 0,
                            WSA_FLAG_OVERLAPPED);
    if (srv == INVALID_SOCKET) {
        printf("[-] server socket failed: %lu\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("[-] bind failed: %lu\n", WSAGetLastError());
        closesocket(srv);
        WSACleanup();
        return 1;
    }

    if (listen(srv, 1) == SOCKET_ERROR) {
        printf("[-] listen failed: %lu\n", WSAGetLastError());
        closesocket(srv);
        DeleteFileA(SOCK_PATH);
        WSACleanup();
        return 1;
    }

    SOCKET cli = WSASocketA(AF_UNIX, SOCK_STREAM, 0, NULL, 0,
                            WSA_FLAG_OVERLAPPED);
    if (cli == INVALID_SOCKET) {
        printf("[-] client socket failed: %lu\n", WSAGetLastError());
        closesocket(srv);
        DeleteFileA(SOCK_PATH);
        WSACleanup();
        return 1;
    }

    if (connect(cli, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("[-] connect failed: %lu\n", WSAGetLastError());
        closesocket(cli);
        closesocket(srv);
        DeleteFileA(SOCK_PATH);
        WSACleanup();
        return 1;
    }

    SOCKET conn = accept(srv, NULL, NULL);
    if (conn == INVALID_SOCKET) {
        printf("[-] accept failed: %lu\n", WSAGetLastError());
        closesocket(cli);
        closesocket(srv);
        DeleteFileA(SOCK_PATH);
        WSACleanup();
        return 1;
    }

    printf("[+] connected\n");
    fflush(stdout);

    char *rbuf = calloc(4096, 1);
    char *sbuf = calloc(BIG_BUF, 1);

    if (!rbuf || !sbuf) {
        printf("[-] allocation failed\n");
        return 1;
    }

    memset(sbuf, 'A', BIG_BUF);

    OVERLAPPED rov[NUM_RECV] = {0};
    WSABUF rwb[NUM_RECV];
    HANDLE rev[NUM_RECV];

    for (int i = 0; i < NUM_RECV; i++) {
        rev[i] = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!rev[i]) {
            printf("[-] CreateEvent receive %d failed: %lu\n",
                   i, GetLastError());
            return 1;
        }

        rov[i].hEvent = rev[i];
        rwb[i].buf = rbuf;
        rwb[i].len = 4096;

        DWORD recvd = 0;
        DWORD flags = 0;

        int rc = WSARecv(conn, &rwb[i], 1, &recvd, &flags,
                         &rov[i], NULL);

        if (rc == SOCKET_ERROR) {
            DWORD err = WSAGetLastError();
            if (err != WSA_IO_PENDING) {
                printf("[-] WSARecv %d failed: %lu\n", i, err);
            }
        }
    }

    printf("[+] %d pending recvs posted\n", NUM_RECV);
    fflush(stdout);

    OVERLAPPED sov[NUM_SEND] = {0};
    WSABUF swb[NUM_SEND];
    HANDLE sev[NUM_SEND];

    for (int i = 0; i < NUM_SEND; i++) {
        sev[i] = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!sev[i]) {
            printf("[-] CreateEvent send %d failed: %lu\n",
                   i, GetLastError());
            return 1;
        }

        sov[i].hEvent = sev[i];
        swb[i].buf = sbuf;
        swb[i].len = BIG_BUF;

        DWORD sent = 0;

        int rc = WSASend(cli, &swb[i], 1, &sent, 0,
                         &sov[i], NULL);

        if (rc == SOCKET_ERROR) {
            DWORD err = WSAGetLastError();
            if (err != WSA_IO_PENDING) {
                printf("[-] WSASend %d failed: %lu\n", i, err);
            }
        }
    }

    printf("[+] %d pending sends posted\n", NUM_SEND);
    fflush(stdout);

    printf("[*] cancelling receives one by one\n");
    fflush(stdout);

    for (int i = 0; i < NUM_RECV; i++) {
        BOOL ok = CancelIoEx((HANDLE)conn, &rov[i]);
        DWORD err = ok ? ERROR_SUCCESS : GetLastError();

        printf("[*] cancel recv %d: ok=%d err=%lu\n",
               i, ok, err);
        fflush(stdout);

        Sleep(0);
    }

    printf("[+] cancel burst done\n");
    printf("[*] keeping sockets and process alive\n");
    printf("[*] measurement2 is now waiting for kernel activity\n");
    fflush(stdout);

    for (;;) {
        Sleep(1000);
    }

    return 0;
}
