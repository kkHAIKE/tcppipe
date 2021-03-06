#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>

#include <WinSock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include <vector>

#define PIPE_URL _T("\\\\.\\pipe\\com_1")
#define BUFSIZE 2048

enum OverType {
	OverType_pipe_unknow = 0,
	OverType_pipe_conn,
	OverType_pipe_send,
	OverType_pipe_recv,
	OverType_sock_send,
	OverType_sock_recv,
	OverType_pipe_pass,
	OverType_sock_pass,
};

class BASEINST {
public:
	BASEINST() : sz(0) {}

	OVERLAPPED over;
	OverType type;
	BYTE buf[BUFSIZE];
	DWORD sz;
};

class PIPEINST: public BASEINST {
public:
	PIPEINST() {
		over.hEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
	}
	~PIPEINST() {
		CloseHandle(over.hEvent);
	}
};

class SOCKINST: public BASEINST {
public:
	SOCKINST() {
		over.hEvent = WSACreateEvent();
		wbuf.len = BUFSIZE;
		wbuf.buf = (char*)buf;
	}
	~SOCKINST() {
		WSACloseEvent(over.hEvent);
	}
	WSABUF wbuf;
};

int main(int argc, char *argv[])
{
	// wsa init
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);

	// socket init
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	// parse argv for remote tcp address
	SOCKADDR_IN addr;
	addr.sin_family = AF_INET;
	inet_pton(AF_INET, argv[1], &addr.sin_addr);
	addr.sin_port = htons(atoi(argv[2]));

	// tcp connect
	int ret = connect(s, (SOCKADDR*)&addr, sizeof(addr));
	if (ret < 0) {
		printf("connect failed\n");
	}

	// create local named pipe
	HANDLE hp = CreateNamedPipe(PIPE_URL,
		PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
		1, BUFSIZE, BUFSIZE, 0, NULL);
	if (hp == INVALID_HANDLE_VALUE) {
		printf("CreateNamedPipe failed\n");
		return -1;
	}

	std::vector<HANDLE> events;
	std::vector<OVERLAPPED*> overlaps;

	/////////////////////////

	PIPEINST *conn = new PIPEINST();
	conn->type = OverType_pipe_conn;

	// wait client connected
	BOOL ok = ConnectNamedPipe(hp, &conn->over);
	if (ok) {
		// overlapped mode must be FALSE
		printf("ConnectNamedPipe failed\n");
		return -1;
	}
	switch (GetLastError()) {
	case ERROR_PIPE_CONNECTED:
		// client connected before, so set event manual
		SetEvent(conn->over.hEvent);
		conn->type = OverType_pipe_pass;
	case ERROR_IO_PENDING:
		// add wait array
		events.push_back(conn->over.hEvent);
		overlaps.push_back(&conn->over);
		break;

	default:
		printf("ConnectNamedPipe failed with %d.\n", GetLastError());
		return -1;
	}

	/////////////////////////

	SOCKINST *rec = new SOCKINST();
	rec->type = OverType_sock_recv;

	DWORD flag = 0;
	// make first tcp recv pending 
	ret = WSARecv(s, &rec->wbuf, 1, &rec->sz, &flag, &rec->over, NULL);
	if ((ret == SOCKET_ERROR) && (WSA_IO_PENDING == WSAGetLastError())) {
		events.push_back(rec->over.hEvent);
		overlaps.push_back(&rec->over);
	} else {
		printf("WSARecv failed %d\n", WSAGetLastError());
		return -1;
	}

	/////////////////////////

	while (TRUE) {
		// wait event array
		DWORD wait = WaitForMultipleObjects(events.size(), &events[0], FALSE, INFINITE);
		DWORD idx = wait - WAIT_OBJECT_0;
		if (idx >= events.size()) {
			printf("WaitForMultipleObjects failed\n");
			return -1;
		}

		BASEINST *base = reinterpret_cast<BASEINST*>(overlaps[idx]);
		DWORD sz = 0, flag = 0;
		if (base->type >= OverType_pipe_pass) {
			// pass type is to assist nooverlop state
			ok = TRUE;
			sz = base->sz;
		} else if (base->type >= OverType_sock_send) {
			ok = WSAGetOverlappedResult(s, &base->over, &sz, FALSE, &flag);
		} else if (base->type >= OverType_pipe_conn) {
			ok = GetOverlappedResult(hp, &base->over, &sz, FALSE);
		} else {
			printf("error type\n");
			return -1;
		}
		if (!ok) {
			printf("GetOverlappedResult failed\n");
			return -1;
		}

		printf("got %d sz %d\n", base->type, sz);

		switch (base->type) {
		case OverType_pipe_conn:
		case OverType_pipe_recv:
		case OverType_pipe_pass:
			if (sz > 0) {
				SOCKINST *tmp = new SOCKINST();
				tmp->type = OverType_sock_send;
				memcpy(tmp->buf, base->buf, sz);
				tmp->wbuf.len = sz;

				int ret = WSASend(s, &tmp->wbuf, 1, &tmp->sz, 0, &tmp->over, NULL);
				if (ret == 0) {
					// send ok immediately
					delete tmp;
				} else if ((ret == SOCKET_ERROR) && (WSA_IO_PENDING == WSAGetLastError())) {
					events.push_back(tmp->over.hEvent);
					overlaps.push_back(&tmp->over);
				} else {
					printf("WSASend failed \n");
					return -1;
				}
			}

			base->type = OverType_pipe_recv;
			base->sz = 0;
			ok = ReadFile(hp, base->buf, BUFSIZE, &base->sz, &base->over);
			if (ok && base->sz > 0) {
				// read ok immediately
				SetEvent(base->over.hEvent);
				base->type = OverType_pipe_pass;
			} else if (!ok && (ERROR_IO_PENDING == GetLastError())) {
				// ignore
			} else {
				printf("ReadFile failed \n");
				return -1;
			}
			break;
		case OverType_sock_recv:
		case OverType_sock_pass:
			if (sz > 0) {
				PIPEINST *tmp = new PIPEINST();
				tmp->type = OverType_pipe_send;
				memcpy(tmp->buf, base->buf, sz);

				BOOL ok = WriteFile(hp, tmp->buf, sz, &tmp->sz, &tmp->over);
				if (ok) {
					delete tmp;
				} else if (!ok && (ERROR_IO_PENDING == GetLastError())) {
					events.push_back(tmp->over.hEvent);
					overlaps.push_back(&tmp->over);
				} else {
					printf("WriteFile failed \n");
					return -1;
				}
			}
			{
				SOCKINST *inst = reinterpret_cast<SOCKINST*>(base);
				inst->type = OverType_sock_recv;
				inst->sz = 0;
				inst->wbuf.len = BUFSIZE;

				DWORD flag = 0;
				int ret = WSARecv(s, &inst->wbuf, 1, &inst->sz, &flag, &inst->over, NULL);
				if (ret == 0 && base->sz > 0) {
					WSASetEvent(base->over.hEvent);
					base->type = OverType_sock_pass;
				} else if ((ret == SOCKET_ERROR) && (WSA_IO_PENDING == WSAGetLastError())) {
					// ignore
				} else {
					printf("WSARecv failed \n");
					return -1;
				}
				break;
			}
		case OverType_pipe_send:
		case OverType_sock_send:
			// clear send inst
			events.erase(events.begin() + idx);
			overlaps.erase(overlaps.begin() + idx);

			if (base->type == OverType_pipe_send) {
				delete reinterpret_cast<PIPEINST*>(base);
			} else {
				delete reinterpret_cast<SOCKINST*>(base);
			}
		}
	}
	
	return 0;
}