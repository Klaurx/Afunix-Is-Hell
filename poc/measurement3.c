#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sockaddr_un {
    unsigned short sun_family;
    char sun_path[108];
};

#define SOCK_PATH "C:\\Temp\\afunix_test.sock"
#define RECV_SIZE 4096
#define SEND_SIZE 65536
#define INITIAL_RECVS 64
#define INITIAL_SENDS 64
#define ACTIVE_RECVS 32
#define TRAFFIC_THREADS 2

static SOCKET g_conn = INVALID_SOCKET;
static SOCKET g_cli  = INVALID_SOCKET;
static volatile LONG g_running = 1;

static DWORD WINAPI sender_thread(LPVOID arg)
{
    (void)arg;

    char *buf = (char *)calloc(SEND_SIZE, 1);
    if (!buf)
        return 1;

    memset(buf, 'B', SEND_SIZE);

    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        DWORD sent = 0;

        int rc = WSASend(
            g_cli,
            &(WSABUF){ (ULONG)SEND_SIZE, buf },
            1,
            &sent,
            0,
            NULL,
            NULL
        );

        if (rc == SOCKET_ERROR) {
            DWORD err = WSAGetLastError();

            if (err != WSAEWOULDBLOCK &&
                err != WSA_IO_PENDING &&
                err != WSAENOBUFS) {
                Sleep(1);
            }
        }

        Sleep(1);
    }

    free(buf);
    return 0;
}

static DWORD WINAPI receiver_thread(LPVOID arg)
{
    (void)arg;

    char *buf = (char *)calloc(RECV_SIZE, 1);
    if (!buf)
        return 1;

    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        OVERLAPPED ov = {0};
        WSABUF wb = {
            RECV_SIZE,
            buf
        };

        HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!ev)
            break;

        ov.hEvent = ev;

        DWORD recvd = 0;
        DWORD flags = 0;

        int rc = WSARecv(
            g_conn,
            &wb,
            1,
            &recvd,
            &flags,
            &ov,
            NULL
        );

        if (rc == SOCKET_ERROR) {
            DWORD err = WSAGetLastError();

            if (err == WSA_IO_PENDING) {

                WaitForSingleObject(ev, 10);
            }
        }


        CancelIoEx((HANDLE)g_conn, &ov);

        CloseHandle(ev);
        Sleep(1);
    }

    free(buf);
    return 0;
}

int main(void)
{
    WSADATA wsa;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("[-] WSAStartup failed\n");
        return 1;
    }

    CreateDirectoryA("C:\\Temp", NULL);
    DeleteFileA(SOCK_PATH);

    SOCKET srv = WSASocketA(
        AF_UNIX,
        SOCK_STREAM,
        0,
        NULL,
        0,
        WSA_FLAG_OVERLAPPED
    );

    if (srv == INVALID_SOCKET) {
        printf("[-] server socket failed: %u\n",
               (unsigned)WSAGetLastError());
        WSACleanup();
        return 1;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;

    strncpy(
        addr.sun_path,
        SOCK_PATH,
        sizeof(addr.sun_path) - 1
    );

    if (bind(
            srv,
            (struct sockaddr *)&addr,
            sizeof(addr)) == SOCKET_ERROR) {

        printf("[-] bind failed: %u\n",
               (unsigned)WSAGetLastError());

        closesocket(srv);
        WSACleanup();
        return 1;
    }

    if (listen(srv, 1) == SOCKET_ERROR) {
        printf("[-] listen failed: %u\n",
               (unsigned)WSAGetLastError());

        closesocket(srv);
        DeleteFileA(SOCK_PATH);
        WSACleanup();
        return 1;
    }

    g_cli = WSASocketA(
        AF_UNIX,
        SOCK_STREAM,
        0,
        NULL,
        0,
        WSA_FLAG_OVERLAPPED
    );

    if (g_cli == INVALID_SOCKET) {
        printf("[-] client socket failed: %u\n",
               (unsigned)WSAGetLastError());

        closesocket(srv);
        DeleteFileA(SOCK_PATH);
        WSACleanup();
        return 1;
    }

    if (connect(
            g_cli,
            (struct sockaddr *)&addr,
            sizeof(addr)) == SOCKET_ERROR) {

        printf("[-] connect failed: %u\n",
               (unsigned)WSAGetLastError());

        closesocket(g_cli);
        closesocket(srv);
        DeleteFileA(SOCK_PATH);
        WSACleanup();
        return 1;
    }

    g_conn = accept(srv, NULL, NULL);

    if (g_conn == INVALID_SOCKET) {
        printf("[-] accept failed: %u\n",
               (unsigned)WSAGetLastError());

        closesocket(g_cli);
        closesocket(srv);
        DeleteFileA(SOCK_PATH);
        WSACleanup();
        return 1;
    }

    printf("[+] connected\n");
    fflush(stdout);


    char *rbuf = (char *)calloc(RECV_SIZE, 1);

    if (!rbuf) {
        printf("[-] receive buffer allocation failed\n");
        return 1;
    }

    OVERLAPPED rov[INITIAL_RECVS] = {0};
    WSABUF rwb[INITIAL_RECVS];
    HANDLE rev[INITIAL_RECVS];

    for (int i = 0; i < INITIAL_RECVS; i++) {
        rev[i] = CreateEventA(NULL, TRUE, FALSE, NULL);

        if (!rev[i]) {
            printf("[-] receive event %d failed\n", i);
            return 1;
        }

        rov[i].hEvent = rev[i];

        rwb[i].buf = rbuf;
        rwb[i].len = RECV_SIZE;

        DWORD recvd = 0;
        DWORD flags = 0;

        int rc = WSARecv(
            g_conn,
            &rwb[i],
            1,
            &recvd,
            &flags,
            &rov[i],
            NULL
        );

        if (rc == SOCKET_ERROR) {
            DWORD err = WSAGetLastError();

            if (err != WSA_IO_PENDING) {
                printf(
                    "[-] initial WSARecv %d failed: %u\n",
                    i,
                    (unsigned)err
                );
            }
        }
    }

    printf("[+] %d pending recvs posted\n", INITIAL_RECVS);
    fflush(stdout);


    char *sbuf = (char *)calloc(SEND_SIZE, 1);

    if (!sbuf) {
        printf("[-] send buffer allocation failed\n");
        return 1;
    }

    memset(sbuf, 'A', SEND_SIZE);

    OVERLAPPED sov[INITIAL_SENDS] = {0};
    WSABUF swb[INITIAL_SENDS];
    HANDLE sev[INITIAL_SENDS];

    for (int i = 0; i < INITIAL_SENDS; i++) {
        sev[i] = CreateEventA(NULL, TRUE, FALSE, NULL);

        if (!sev[i]) {
            printf("[-] send event %d failed\n", i);
            return 1;
        }

        sov[i].hEvent = sev[i];

        swb[i].buf = sbuf;
        swb[i].len = SEND_SIZE;

        DWORD sent = 0;

        int rc = WSASend(
            g_cli,
            &swb[i],
            1,
            &sent,
            0,
            &sov[i],
            NULL
        );

        if (rc == SOCKET_ERROR) {
            DWORD err = WSAGetLastError();

            if (err != WSA_IO_PENDING) {
                printf(
                    "[-] initial WSASend %d failed: %u\n",
                    i,
                    (unsigned)err
                );
            }
        }
    }

    printf("[+] %d pending sends posted\n", INITIAL_SENDS);
    fflush(stdout);


    printf("[*] cancelling initial receives one by one\n");
    fflush(stdout);

    for (int i = 0; i < INITIAL_RECVS; i++) {
        BOOL ok = CancelIoEx(
            (HANDLE)g_conn,
            &rov[i]
        );

        DWORD err = ok ? ERROR_SUCCESS : GetLastError();

        printf(
            "[*] cancel initial recv %d: ok=%d err=%u\n",
            i,
            ok,
            (unsigned)err
        );

        fflush(stdout);
        Sleep(0);
    }

    printf("[+] initial cancel burst done\n");
    fflush(stdout);

    printf("[*] starting continued AF_UNIX activity\n");
    printf("[*] measurement3 is now active\n");
    fflush(stdout);

    HANDLE threads[TRAFFIC_THREADS];

    threads[0] = CreateThread(
        NULL,
        0,
        sender_thread,
        NULL,
        0,
        NULL
    );

    threads[1] = CreateThread(
        NULL,
        0,
        receiver_thread,
        NULL,
        0,
        NULL
    );

    if (!threads[0] || !threads[1]) {
        printf("[-] failed to start traffic threads\n");
        return 1;
    }

    printf("[+] continued traffic threads running\n");
    fflush(stdout);


    for (;;) {
        Sleep(1000);
    }

    return 0;
}
