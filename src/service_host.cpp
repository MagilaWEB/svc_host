#include "logger.hpp"
#include "service_host.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>
#include <fstream>
#include <thread>

void HandleDeleter::operator()(HANDLE h) const noexcept
{
	if (h && h != INVALID_HANDLE_VALUE)
		CloseHandle(h);
}

static std::wstring Utf8ToWide(std::string_view utf8)
{
	if (utf8.empty())
		return L"";
	int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
	if (len <= 0)
		return L"";
	std::wstring wide(len, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), wide.data(), len);
	return wide;
}

static void ReadPipeToLogger(HANDLE hPipe, const std::wstring& prefix)
{
	char		buffer[4'096];
	DWORD		bytesRead;
	std::string lineBuffer;
	while (ReadFile(hPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0)
	{
		buffer[bytesRead] = '\0';
		std::string_view chunk(buffer, bytesRead);
		size_t			 start = 0;
		while (start < chunk.size())
		{
			size_t end = chunk.find('\n', start);
			if (end == std::string_view::npos)
			{
				lineBuffer.append(chunk.substr(start));
				break;
			}
			else
			{
				lineBuffer.append(chunk.substr(start, end - start));
				if (!lineBuffer.empty() && lineBuffer.back() == '\r')
					lineBuffer.pop_back();
				std::wstring wline = Utf8ToWide(lineBuffer);
				Logger::get().info(L"{}: {}", prefix, wline);
				lineBuffer.clear();
				start = end + 1;
			}
		}
	}
	if (!lineBuffer.empty())
	{
		std::wstring wline = Utf8ToWide(lineBuffer);
		Logger::get().info(L"{}: {}", prefix, wline);
	}
}

int ServiceHost::Run(int argc, wchar_t* /*argv*/[])
{
	if (argc < 2)
		return 1;

	Logger::get().info(L"Service Run");

	SERVICE_TABLE_ENTRYW serviceTable[] = {
		{ const_cast<LPWSTR>(L"SvcHost"), ServiceMain },
		{						nullptr,	   nullptr }
	};

	if (!StartServiceCtrlDispatcherW(serviceTable))
		return static_cast<int>(GetLastError());

	return 0;
}

void WINAPI ServiceHost::ServiceMain(DWORD argc, LPWSTR* argv)
{
	s_StatusHandle = RegisterServiceCtrlHandlerW(L"SvcHost", ServiceCtrlHandler);
	if (!s_StatusHandle)
		return;

	Logger::get().info(L"ServiceMain Run");

	s_Status.dwServiceType		= SERVICE_WIN32_OWN_PROCESS;
	s_Status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
	UpdateStatus(SERVICE_START_PENDING, NO_ERROR, 3'000);

	s_hStopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	if (!s_hStopEvent)
	{
		UpdateStatus(SERVICE_STOPPED, GetLastError(), 0);
		Logger::get().warning(L"s_hStopEvent service STOPPED!");
		return;
	}

	std::wstring cmdLine;

	if (argc < 2)
	{
		LPWSTR	pFullCmd  = GetCommandLineW();
		int		localArgc = 0;
		LPWSTR* localArgv = CommandLineToArgvW(pFullCmd, &localArgc);
		if (localArgv && localArgc >= 2)
		{
			for (int i = 1; i < localArgc; ++i)
			{
				if (i > 1)
					cmdLine += L" ";

				std::wstring arg = localArgv[i];
				if (arg.find(L' ') != std::wstring::npos)
				{
					cmdLine += L'"';
					cmdLine += arg;
					cmdLine += L'"';
				}
				else
					cmdLine += arg;
			}

			LocalFree(localArgv);
		}
	}
	else
	{
		for (DWORD i = 1; i < argc; ++i)
		{
			if (i > 1)
				cmdLine += L" ";

			std::wstring arg = argv[i];
			if (arg.size() >= 2 && arg.front() == L'"' && arg.back() == L'"')
				arg = arg.substr(1, arg.size() - 2);

			cmdLine += arg;
		}
	}

	if (cmdLine.empty())
	{
		UpdateStatus(SERVICE_STOPPED, ERROR_BAD_ARGUMENTS, 0);
		Logger::get().error(L"cmdLine empty ERROR_BAD_ARGUMENTS service STOPPED!");
		return;
	}

	Logger::get().info(L"cmdLine [{}]", cmdLine);

	auto launchResult = LaunchChild(cmdLine);
	if (!launchResult)
	{
		UpdateStatus(SERVICE_STOPPED, launchResult.error(), 0);
		Logger::get().error(L"LaunchChild failed, error [{}], service STOPPED!", launchResult.error());
		return;
	}

	auto& res = *launchResult;

	std::thread stdoutThread(ReadPipeToLogger, res.hStdoutRead.release(), L"[STDOUT]");
	std::thread stderrThread(ReadPipeToLogger, res.hStderrRead.release(), L"[STDERR]");

	stdoutThread.detach();
	stderrThread.detach();

	if (res.hProcess)
	{
		s_hJob = CreateJobAndAssignProcess(res.hProcess.get());

		UpdateStatus(SERVICE_RUNNING, NO_ERROR, 0);
		Logger::get().info(L"Service RUNNING! PID: {}", res.dwProcessId);

		std::array<HANDLE, 2> handles{ s_hStopEvent.get(), res.hProcess.get() };
		DWORD				  waitResult = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);

		if (waitResult == WAIT_OBJECT_0)
		{
			Logger::get().info(L"Stop signal received. Terminating child process...");

			if (!TerminateProcess(res.hProcess.get(), 1))
			{
				DWORD err = GetLastError();
				Logger::get().error(L"TerminateProcess failed, error: {}", err);
			}
			else
			{
				DWORD waitChild = WaitForSingleObject(res.hProcess.get(), 5'000);

				if (waitChild == WAIT_OBJECT_0)
					Logger::get().info(L"Child process terminated successfully.");
				else
					Logger::get().warning(L"Child process did not terminate in time.");
			}
		}

		if (s_hJob)
		{
			s_hJob.reset();
			Logger::get().info(L"Job object closed. Any remaining processes in job are terminated.");
		}

		res.hProcess.reset();
	}

	UpdateStatus(SERVICE_STOPPED, NO_ERROR, 0);
	Logger::get().info(L"Service STOPPED!");
}

void WINAPI ServiceHost::ServiceCtrlHandler(DWORD ctrl) noexcept
{
	if (ctrl == SERVICE_CONTROL_STOP)
	{
		UpdateStatus(SERVICE_STOP_PENDING, NO_ERROR, 3'000);
		SetEvent(s_hStopEvent.get());
	}
}

void ServiceHost::UpdateStatus(DWORD state, DWORD exitCode, DWORD waitHint) noexcept
{
	static DWORD checkpoint	 = 1;
	s_Status.dwCurrentState	 = state;
	s_Status.dwWin32ExitCode = exitCode;
	s_Status.dwWaitHint		 = waitHint;
	s_Status.dwCheckPoint	 = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
	SetServiceStatus(s_StatusHandle, &s_Status);
}

std::expected<LaunchResult, DWORD> ServiceHost::LaunchChild(std::wstring_view cmdLine)
{
	std::wstring cmd(cmdLine);
	std::wstring exePath;

	if (cmd.front() == L'"')
	{
		size_t endQuote = cmd.find(L'"', 1);
		if (endQuote != std::wstring::npos)
			exePath = cmd.substr(1, endQuote - 1);
	}
	else
	{
		size_t spacePos = cmd.find(L' ');
		if (spacePos != std::wstring::npos)
			exePath = cmd.substr(0, spacePos);
		else
			exePath = cmd;
	}

	Logger::get().info(L"LaunchChild exePath[{}].", exePath);
	Logger::get().info(L"LaunchChild cmd[{}].", cmd);

	std::wstring workingDir;
	size_t		 lastSlash = exePath.find_last_of(L"\\/");
	if (lastSlash != std::wstring::npos)
		workingDir = exePath.substr(0, lastSlash);

	Logger::get().info(L"LaunchChild workingDir[{}].", workingDir);

	SECURITY_ATTRIBUTES sa			= { sizeof(sa), nullptr, TRUE };
	HANDLE				hStdoutRead = nullptr, hStdoutWrite = nullptr;
	HANDLE				hStderrRead = nullptr, hStderrWrite = nullptr;

	if (!CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0))
		return std::unexpected(GetLastError());
	if (!CreatePipe(&hStderrRead, &hStderrWrite, &sa, 0))
	{
		CloseHandle(hStdoutRead);
		CloseHandle(hStdoutWrite);
		return std::unexpected(GetLastError());
	}

	SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(hStderrRead, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW si{};
	si.cb		   = sizeof(si);
	si.dwFlags	   = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	si.hStdOutput  = hStdoutWrite;
	si.hStdError   = hStderrWrite;
	si.hStdInput   = nullptr;

	PROCESS_INFORMATION pi{};
	LPWSTR				lpWorkingDir = workingDir.empty() ? nullptr : workingDir.data();

	BOOL success = CreateProcessW(
		nullptr,
		cmd.data(),
		nullptr,
		nullptr,
		TRUE,		// bInheritHandles
		0,			// dwCreationFlags
		nullptr,	// lpEnvironment
		lpWorkingDir,
		&si,
		&pi
	);

	CloseHandle(hStdoutWrite);
	CloseHandle(hStderrWrite);

	if (!success)
	{
		DWORD err = GetLastError();
		CloseHandle(hStdoutRead);
		CloseHandle(hStderrRead);
		return std::unexpected(err);
	}

	LaunchResult result;
	result.hProcess.reset(pi.hProcess);
	result.hThread.reset(pi.hThread);
	result.hStdoutRead.reset(hStdoutRead);
	result.hStderrRead.reset(hStderrRead);
	result.dwProcessId = pi.dwProcessId;

	return result;
}

UniqueHandle ServiceHost::CreateJobAndAssignProcess(HANDLE hProcess)
{
	UniqueHandle hJob(CreateJobObjectW(nullptr, nullptr));
	if (!hJob)
		return nullptr;

	JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
	jeli.BasicLimitInformation.LimitFlags	  = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!SetInformationJobObject(hJob.get(), JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)))
		return nullptr;

	if (!AssignProcessToJobObject(hJob.get(), hProcess))
		return nullptr;

	return hJob;
}
