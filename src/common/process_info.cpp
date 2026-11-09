// SPDX-License-Identifier: MIT
#include "pml/process_info.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>

namespace pml {

namespace {

std::string readWholeFile(const std::string &path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return {};
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string basenameOf(const std::string &path, bool alsoBackslash)
{
	size_t pos = path.find_last_of(alsoBackslash ? "/\\" : "/");
	return pos == std::string::npos ? path : path.substr(pos + 1);
}

bool isWineExecutable(const std::string &base)
{
	// wine, wine64, wine-preloader, wine64-preloader, wine-staging, ...
	return base.rfind("wine", 0) == 0 && base.find("server") == std::string::npos;
}

} // namespace

namespace {

// Fields of /proc/<pid>/stat that we need: the state (field 3) and starttime (field 22).
struct ProcStat
{
	char state = 0;
	uint64_t startTicks = 0;
};

bool readProcStat(int pid, ProcStat *out)
{
	std::string stat = readWholeFile("/proc/" + std::to_string(pid) + "/stat");
	if (stat.empty())
		return false;

	// The comm field (2) may contain spaces and parentheses: fields resume after the last ')'.
	size_t close = stat.rfind(')');
	if (close == std::string::npos)
		return false;

	std::istringstream fields(stat.substr(close + 1));
	std::string token;
	// Token 0 after ')' is field 3 (state); starttime is field 22, i.e. token 19.
	for (int i = 0; i <= 19; i++)
	{
		if (!(fields >> token))
			return false;
		if (i == 0)
			out->state = token.empty() ? 0 : token[0];
	}

	char *end = nullptr;
	errno = 0;
	unsigned long long value = std::strtoull(token.c_str(), &end, 10);
	if (errno != 0 || end == token.c_str())
		return false;
	out->startTicks = value;
	return true;
}

} // namespace

uint64_t readProcessStartTicks(int pid)
{
	ProcStat st;
	return readProcStat(pid, &st) ? st.startTicks : 0;
}

std::string readExePath(int pid)
{
	char buffer[4096];
	std::string link = "/proc/" + std::to_string(pid) + "/exe";
	ssize_t len = ::readlink(link.c_str(), buffer, sizeof(buffer) - 1);
	if (len <= 0)
		return {};
	buffer[len] = '\0';
	return buffer;
}

std::string readArgv0(int pid)
{
	std::string cmdline = readWholeFile("/proc/" + std::to_string(pid) + "/cmdline");
	if (cmdline.empty())
		return {};
	// Arguments are NUL-separated; argv[0] ends at the first NUL.
	return std::string(cmdline.c_str());
}

std::string detectProcessName(int pid)
{
	std::string exe = readExePath(pid);
	std::string base = basenameOf(exe, false);

	if (base.empty() || isWineExecutable(base))
	{
		std::string argv0 = readArgv0(pid);
		std::string winBase = basenameOf(argv0, true);
		if (!winBase.empty())
			return winBase;
	}

	if (base.empty())
	{
		std::string comm = readWholeFile("/proc/" + std::to_string(pid) + "/comm");
		while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r'))
			comm.pop_back();
		return comm;
	}

	return base;
}

bool processAlive(int pid, uint64_t startTicks)
{
	if (pid <= 0)
		return false;
	if (::kill(pid, 0) != 0 && errno == ESRCH)
		return false;
	ProcStat st;
	if (!readProcStat(pid, &st) || st.startTicks == 0)
		return false;
	// A zombie (Z) or dead (X) process has exited; only its parent has not reaped it yet.
	if (st.state == 'Z' || st.state == 'X')
		return false;
	return startTicks == 0 || st.startTicks == startTicks;
}

uint32_t currentThreadId()
{
	return uint32_t(::syscall(SYS_gettid));
}

} // namespace pml
