// SPDX-License-Identifier: MIT
// Helpers for the CLI tests: spawn processes with redirected output, wait with timeouts, read files.
#pragma once

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

namespace pmltest {

inline void sleepMs(unsigned ms)
{
	timespec ts = { time_t(ms / 1000), long(ms % 1000) * 1000000L };
	nanosleep(&ts, nullptr);
}

inline std::string readFile(const std::string &path)
{
	std::ifstream f(path, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

inline std::vector<std::string> splitLines(const std::string &text)
{
	std::vector<std::string> lines;
	std::string cur;
	for (char c : text)
	{
		if (c == '\n')
		{
			lines.push_back(cur);
			cur.clear();
		}
		else if (c != '\r')
			cur += c;
	}
	if (!cur.empty())
		lines.push_back(cur);
	return lines;
}

inline std::vector<std::string> splitCsv(const std::string &line)
{
	std::vector<std::string> cols;
	std::string cur;
	for (char c : line)
	{
		if (c == ',')
		{
			cols.push_back(cur);
			cur.clear();
		}
		else
			cur += c;
	}
	cols.push_back(cur);
	return cols;
}

// A child process with stdout/stderr redirected into files.
struct Proc
{
	pid_t pid = -1;
	std::string outPath, errPath;
	int status = 0;
	bool reaped = false;

	std::string out() const { return readFile(outPath); }
	std::string err() const { return readFile(errPath); }

	// Exit code, 128+signal when killed. Waits up to timeoutMs, then SIGKILLs and sets *timedOut.
	int wait(unsigned timeoutMs, bool *timedOut = nullptr)
	{
		if (timedOut)
			*timedOut = false;
		if (reaped)
			return decode();
		for (unsigned waited = 0;; waited += 10)
		{
			pid_t r = ::waitpid(pid, &status, WNOHANG);
			if (r == pid)
			{
				reaped = true;
				return decode();
			}
			if (waited >= timeoutMs)
			{
				::kill(pid, SIGKILL);
				::waitpid(pid, &status, 0);
				reaped = true;
				if (timedOut)
					*timedOut = true;
				return decode();
			}
			sleepMs(10);
		}
	}

	bool running()
	{
		if (reaped)
			return false;
		pid_t r = ::waitpid(pid, &status, WNOHANG);
		if (r == pid)
		{
			reaped = true;
			return false;
		}
		return true;
	}

	void signal(int sig) const { ::kill(pid, sig); }

	~Proc()
	{
		if (!reaped && pid > 0)
		{
			// SIGTERM first: the fake producer and the CLI clean up after themselves.
			::kill(pid, SIGTERM);
			bool timedOut = false;
			wait(2000, &timedOut);
		}
	}

private:
	int decode() const { return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status); }
};

// Polls `path` until it contains `text`.
inline bool waitForText(const std::string &path, const std::string &text, unsigned timeoutMs)
{
	for (unsigned waited = 0; waited <= timeoutMs; waited += 10)
	{
		if (readFile(path).find(text) != std::string::npos)
			return true;
		sleepMs(10);
	}
	return false;
}

// Children start with the stop signals at their defaults and nothing blocked, whatever the test runner had
// (a ctest started with & or under nohup ignores SIGINT/SIGQUIT or SIGHUP, and the collector keeps an ignored
// signal ignored). A test about an inherited disposition names the one signal whose disposition is passed on.
inline int gSpawnKeepsSignal = 0;

inline bool spawn(Proc &p, const std::string &exe, const std::vector<std::string> &args, const std::string &logDir,
                  const std::string &tag, const std::string &cwd = "",
                  const std::vector<std::string> &extraEnv = {})
{
	p.outPath = logDir + "/" + tag + ".out";
	p.errPath = logDir + "/" + tag + ".err";
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 1, p.outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	posix_spawn_file_actions_addopen(&fa, 2, p.errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (!cwd.empty())
		posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());

	std::vector<std::string> argStore;
	argStore.push_back(exe);
	argStore.insert(argStore.end(), args.begin(), args.end());
	std::vector<char *> argv;
	for (auto &a : argStore)
		argv.push_back(a.data());
	argv.push_back(nullptr);

	std::vector<std::string> envStore;
	for (char **e = environ; *e; ++e)
		envStore.push_back(*e);
	envStore.insert(envStore.end(), extraEnv.begin(), extraEnv.end());
	std::vector<char *> envp;
	for (auto &e : envStore)
		envp.push_back(e.data());
	envp.push_back(nullptr);

	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	{
		sigset_t defaults, none;
		sigemptyset(&defaults);
		for (int sig : { SIGINT, SIGTERM, SIGHUP, SIGQUIT })
			if (sig != gSpawnKeepsSignal)
				sigaddset(&defaults, sig);
		sigemptyset(&none);
		posix_spawnattr_setsigdefault(&attr, &defaults);
		posix_spawnattr_setsigmask(&attr, &none);
		posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
	}
	int rc = posix_spawn(&p.pid, exe.c_str(), &fa, &attr, argv.data(), envp.data());
	posix_spawnattr_destroy(&attr);
	posix_spawn_file_actions_destroy(&fa);
	return rc == 0;
}

} // namespace pmltest
