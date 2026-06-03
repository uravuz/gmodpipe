#include "GarrysMod/Lua/Interface.h"
#include <string>
#include <unordered_map>
#include <cstring>
#include <cstdio>

#ifdef _WIN32
	#include <windows.h>

	using pipe_t = HANDLE;
	static const pipe_t INVALID_PIPE = INVALID_HANDLE_VALUE;

	static bool pipe_valid(pipe_t p) { return p != INVALID_HANDLE_VALUE; }
	static void pipe_close_handle(pipe_t p) { CloseHandle(p); }
	static std::string pipe_fullname(const char* name)
	{
		return std::string("\\\\.\\pipe\\") + name;
	}
#else
	#include <sys/socket.h>
	#include <sys/un.h>
	#include <sys/select.h>
	#include <fcntl.h>
	#include <unistd.h>
	#include <errno.h>

	using pipe_t = int;
	static const pipe_t INVALID_PIPE = -1;

	static bool pipe_valid(pipe_t p) { return p >= 0; }
	static void pipe_close_handle(pipe_t p) { close(p); }
	static std::string pipe_fullname(const char* name)
	{
		return std::string("/tmp/") + name;
	}
#endif

using namespace GarrysMod::Lua;

static int s_nextId = 1;
static std::unordered_map<int, pipe_t> s_pipes;

static int RegisterPipe(pipe_t p)
{
	int id = s_nextId++;
	s_pipes[id] = p;
	return id;
}

static pipe_t GetPipe(int id)
{
	auto it = s_pipes.find(id);
	if (it == s_pipes.end()) return INVALID_PIPE;
	return it->second;
}

static void RemovePipe(int id)
{
	s_pipes.erase(id);
}

LUA_FUNCTION(Pipe_CreateServer)
{
	const char* name = LUA->CheckString(1);
	std::string fullName = pipe_fullname(name);

#ifdef _WIN32
	HANDLE pipe = CreateNamedPipeA(
		fullName.c_str(),
		PIPE_ACCESS_DUPLEX,
		PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT,
		1, 4096, 4096, 0, NULL
	);

	if (!pipe_valid(pipe))
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "CreateNamedPipe failed: %lu", GetLastError());
		LUA->PushNil(); LUA->PushString(buf);
		return 2;
	}
#else
	int pipe = socket(AF_UNIX, SOCK_STREAM, 0);
	if (!pipe_valid(pipe))
	{
		LUA->PushNil(); LUA->PushString("socket() failed");
		return 2;
	}

	unlink(fullName.c_str());

	struct sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, fullName.c_str(), sizeof(addr.sun_path) - 1);

	if (bind(pipe, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
		close(pipe);
		LUA->PushNil(); LUA->PushString("bind() failed");
		return 2;
	}

	if (listen(pipe, 1) < 0)
	{
		close(pipe);
		LUA->PushNil(); LUA->PushString("listen() failed");
		return 2;
	}

	int flags = fcntl(pipe, F_GETFL, 0);
	fcntl(pipe, F_SETFL, flags | O_NONBLOCK);
#endif

	LUA->PushNumber(RegisterPipe(pipe));
	return 1;
}

LUA_FUNCTION(Pipe_WaitForClient)
{
	int id      = (int)LUA->CheckNumber(1);
	int timeout = (int)LUA->CheckNumber(2);

	pipe_t handle = GetPipe(id);
	if (!pipe_valid(handle))
	{
		LUA->PushNil(); LUA->PushString("Invalid pipe handle");
		return 2;
	}

#ifdef _WIN32
	DWORD mode = PIPE_READMODE_MESSAGE | PIPE_WAIT;
	SetNamedPipeHandleState(handle, &mode, NULL, NULL);

	OVERLAPPED ov = {};
	ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

	BOOL connected = ConnectNamedPipe(handle, &ov);
	DWORD err = GetLastError();
	bool ok = false;

	if (connected || err == ERROR_PIPE_CONNECTED)
	{
		ok = true;
	}
	else if (err == ERROR_IO_PENDING)
	{
		DWORD wait = WaitForSingleObject(ov.hEvent, (DWORD)timeout);
		if (wait == WAIT_OBJECT_0)
		{
			DWORD dummy;
			ok = GetOverlappedResult(handle, &ov, &dummy, FALSE) != 0;
		}
	}

	CloseHandle(ov.hEvent);

	mode = PIPE_READMODE_MESSAGE | PIPE_NOWAIT;
	SetNamedPipeHandleState(handle, &mode, NULL, NULL);

	if (!ok)
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "WaitForClient failed or timed out: %lu", GetLastError());
		LUA->PushNil(); LUA->PushString(buf);
		return 2;
	}
#else
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(handle, &fds);

	struct timeval tv;
	tv.tv_sec  = timeout / 1000;
	tv.tv_usec = (timeout % 1000) * 1000;

	int ret = select(handle + 1, &fds, NULL, NULL, &tv);
	if (ret <= 0)
	{
		LUA->PushNil();
		LUA->PushString(ret == 0 ? "WaitForClient timed out" : "select() failed");
		return 2;
	}

	int client = accept(handle, NULL, NULL);
	if (client < 0)
	{
		LUA->PushNil(); LUA->PushString("accept() failed");
		return 2;
	}

	int flags = fcntl(client, F_GETFL, 0);
	fcntl(client, F_SETFL, flags | O_NONBLOCK);

	close(handle);
	s_pipes[id] = client;
#endif

	LUA->PushBool(true);
	return 1;
}

LUA_FUNCTION(Pipe_Connect)
{
	const char* name = LUA->CheckString(1);
	std::string fullName = pipe_fullname(name);

#ifdef _WIN32
	if (!WaitNamedPipeA(fullName.c_str(), 5000))
	{
		char buf[128];
		snprintf(buf, sizeof(buf), "WaitNamedPipe failed: %lu", GetLastError());
		LUA->PushNil(); LUA->PushString(buf);
		return 2;
	}

	HANDLE hPipe = CreateFileA(
		fullName.c_str(),
		GENERIC_READ | GENERIC_WRITE,
		0, NULL, OPEN_EXISTING,
		FILE_FLAG_OVERLAPPED, NULL
	);

	if (!pipe_valid(hPipe))
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "CreateFile failed: %lu", GetLastError());
		LUA->PushNil(); LUA->PushString(buf);
		return 2;
	}

	DWORD mode = PIPE_READMODE_MESSAGE;
	SetNamedPipeHandleState(hPipe, &mode, NULL, NULL);
#else
	int hPipe = socket(AF_UNIX, SOCK_STREAM, 0);
	if (!pipe_valid(hPipe))
	{
		LUA->PushNil(); LUA->PushString("socket() failed");
		return 2;
	}

	struct sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, fullName.c_str(), sizeof(addr.sun_path) - 1);

	if (connect(hPipe, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
		close(hPipe);
		char buf[64];
		snprintf(buf, sizeof(buf), "connect() failed: %s", strerror(errno));
		LUA->PushNil(); LUA->PushString(buf);
		return 2;
	}

	int flags = fcntl(hPipe, F_GETFL, 0);
	fcntl(hPipe, F_SETFL, flags | O_NONBLOCK);
#endif

	LUA->PushNumber(RegisterPipe(hPipe));
	return 1;
}

LUA_FUNCTION(Pipe_Write)
{
	int id = (int)LUA->CheckNumber(1);
    size_t len = 0;
    const char* data = LUA->CheckString(2);
    len = strlen(data);

	pipe_t handle = GetPipe(id);
	if (!pipe_valid(handle))
	{
		LUA->PushNil(); LUA->PushString("Invalid pipe handle");
		return 2;
	}

#ifdef _WIN32
	OVERLAPPED ov = {};
	ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

	DWORD written = 0;
	BOOL ok = WriteFile(handle, data, (DWORD)len, &written, &ov);
	if (!ok && GetLastError() == ERROR_IO_PENDING)
	{
		WaitForSingleObject(ov.hEvent, INFINITE);
		ok = GetOverlappedResult(handle, &ov, &written, FALSE);
	}
	CloseHandle(ov.hEvent);

	if (!ok)
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "WriteFile failed: %lu", GetLastError());
		LUA->PushNil(); LUA->PushString(buf);
		return 2;
	}

	LUA->PushNumber((double)written);
#else
	ssize_t written = send(handle, data, len, 0);
	if (written < 0)
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "send() failed: %s", strerror(errno));
		LUA->PushNil(); LUA->PushString(buf);
		return 2;
	}

	LUA->PushNumber((double)written);
#endif
	return 1;
}

LUA_FUNCTION(Pipe_Read)
{
	int id       = (int)LUA->CheckNumber(1);
	int maxBytes = (int)LUA->CheckNumber(2);

	pipe_t handle = GetPipe(id);
	if (!pipe_valid(handle))
	{
		LUA->PushNil(); LUA->PushString("Invalid pipe handle");
		return 2;
	}

	std::string buf(maxBytes, '\0');

#ifdef _WIN32
	OVERLAPPED ov = {};
	ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

	DWORD read = 0;
	BOOL ok  = ReadFile(handle, &buf[0], (DWORD)maxBytes, &read, &ov);
	DWORD err = GetLastError();

	if (!ok)
	{
		if (err == ERROR_IO_PENDING)
		{
			DWORD available = 0;
			if (!PeekNamedPipe(handle, NULL, 0, NULL, &available, NULL) || available == 0)
			{
				CancelIo(handle);
				CloseHandle(ov.hEvent);
				LUA->PushNil();
				return 1;
			}
			WaitForSingleObject(ov.hEvent, INFINITE);
			ok  = GetOverlappedResult(handle, &ov, &read, FALSE);
			err = GetLastError();
		}

		if (!ok && err != ERROR_MORE_DATA)
		{
			CloseHandle(ov.hEvent);
			if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED)
			{
				LUA->PushNil(); LUA->PushString("Pipe disconnected");
				return 2;
			}
			char errbuf[64];
			snprintf(errbuf, sizeof(errbuf), "ReadFile failed: %lu", err);
			LUA->PushNil(); LUA->PushString(errbuf);
			return 2;
		}
	}

	CloseHandle(ov.hEvent);
	buf.resize(read);
	LUA->PushString(buf.c_str(), (int)read);
#else
	ssize_t read = recv(handle, &buf[0], (size_t)maxBytes, 0);
	if (read < 0)
	{
		if (errno == EAGAIN || errno == EWOULDBLOCK)
		{
			LUA->PushNil();
			return 1;
		}
		char errbuf[64];
		snprintf(errbuf, sizeof(errbuf), "recv() failed: %s", strerror(errno));
		LUA->PushNil(); LUA->PushString(errbuf);
		return 2;
	}
	if (read == 0)
	{
		LUA->PushNil(); LUA->PushString("Pipe disconnected");
		return 2;
	}

	LUA->PushString(buf.c_str(), (int)read);
#endif
	return 1;
}

LUA_FUNCTION(Pipe_Peek)
{
	int id = (int)LUA->CheckNumber(1);

	pipe_t handle = GetPipe(id);
	if (!pipe_valid(handle))
	{
		LUA->PushNil(); LUA->PushString("Invalid pipe handle");
		return 2;
	}

#ifdef _WIN32
	DWORD available = 0;
	if (!PeekNamedPipe(handle, NULL, 0, NULL, &available, NULL))
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "PeekNamedPipe failed: %lu", GetLastError());
		LUA->PushNil(); LUA->PushString(buf);
		return 2;
	}
	LUA->PushNumber((double)available);
#else
	int available = 0;
	if (ioctl(handle, FIONREAD, &available) < 0)
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "ioctl(FIONREAD) failed: %s", strerror(errno));
		LUA->PushNil(); LUA->PushString(buf);
		return 2;
	}
	LUA->PushNumber((double)available);
#endif
	return 1;
}

LUA_FUNCTION(Pipe_Disconnect)
{
	int id = (int)LUA->CheckNumber(1);

	pipe_t handle = GetPipe(id);
	if (!pipe_valid(handle))
	{
		LUA->PushNil(); LUA->PushString("Invalid pipe handle");
		return 2;
	}

#ifdef _WIN32
	if (!DisconnectNamedPipe(handle))
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "DisconnectNamedPipe failed: %lu", GetLastError());
		LUA->PushNil(); LUA->PushString(buf);
		return 2;
	}
#else
	if (shutdown(handle, SHUT_RDWR) < 0)
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "shutdown() failed: %s", strerror(errno));
		LUA->PushNil(); LUA->PushString(buf);
		return 2;
	}
#endif

	LUA->PushBool(true);
	return 1;
}

LUA_FUNCTION(Pipe_Close)
{
	int id = (int)LUA->CheckNumber(1);

	pipe_t handle = GetPipe(id);
	if (!pipe_valid(handle))
	{
		LUA->PushNil(); LUA->PushString("Invalid pipe handle");
		return 2;
	}

	pipe_close_handle(handle);
	RemovePipe(id);

	LUA->PushBool(true);
	return 1;
}

GMOD_MODULE_OPEN()
{
	LUA->PushSpecial(SPECIAL_GLOB);
	LUA->CreateTable();

	LUA->PushString("CreateServer");  LUA->PushCFunction(Pipe_CreateServer);  LUA->SetTable(-3);
	LUA->PushString("WaitForClient"); LUA->PushCFunction(Pipe_WaitForClient); LUA->SetTable(-3);
	LUA->PushString("Connect");       LUA->PushCFunction(Pipe_Connect);       LUA->SetTable(-3);
	LUA->PushString("Write");         LUA->PushCFunction(Pipe_Write);         LUA->SetTable(-3);
	LUA->PushString("Read");          LUA->PushCFunction(Pipe_Read);          LUA->SetTable(-3);
	LUA->PushString("Peek");          LUA->PushCFunction(Pipe_Peek);          LUA->SetTable(-3);
	LUA->PushString("Disconnect");    LUA->PushCFunction(Pipe_Disconnect);    LUA->SetTable(-3);
	LUA->PushString("Close");         LUA->PushCFunction(Pipe_Close);         LUA->SetTable(-3);

	LUA->SetField(-2, "Pipe");
	LUA->Pop();
	return 0;
}

GMOD_MODULE_CLOSE()
{
	for (auto& kv : s_pipes)
		pipe_close_handle(kv.second);
	s_pipes.clear();
	return 0;
}