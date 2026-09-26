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

namespace
{
	// Installed while broadcasting CTRL_C to the child's console so the host
	// itself does not die from the event it just generated.
	BOOL WINAPI IgnoreCtrlHandler(DWORD /*ctrl_type*/)
	{
		return TRUE;
	}
}

static std::wstring Utf8ToWide(std::string_view utf8)
{
	if (utf8.empty())
		return L"";

	int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	if (len <= 0)
		return L"";

	std::wstring wide(len, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), len);
	return wide;
}

static void ReadPipeToLogger(HANDLE hPipe, std::wstring_view prefix)
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

				Logger::get().info(L"{}: {}", prefix, Utf8ToWide(lineBuffer));
				lineBuffer.clear();
				start = end + 1;
			}
		}
	}

	if (!lineBuffer.empty())
		Logger::get().info(L"{}: {}", prefix, Utf8ToWide(lineBuffer));
}

int ServiceHost::Run(int argc, wchar_t* /*argv*/[])
{
	if (argc < 2)
		return 1;

	SERVICE_TABLE_ENTRYW serviceTable[] = {
		{ const_cast<LPWSTR>(L"SvcHost"), _serviceMain },
		{						nullptr,	   nullptr }
	};

	if (!StartServiceCtrlDispatcherW(serviceTable))
		return static_cast<int>(GetLastError());

	return 0;
}

void WINAPI ServiceHost::_serviceMain(DWORD argc, LPWSTR* argv)
{
	_status_handle = RegisterServiceCtrlHandlerW(L"SvcHost", _serviceCtrlHandler);
	if (!_status_handle)
		return;

	auto cmd_line = _commandLine(argc, argv);
	if (cmd_line.empty())
	{
		_updateStatus(SERVICE_STOPPED, ERROR_BAD_ARGUMENTS, 0);
		Logger::get().error(L"cmd_line empty ERROR_BAD_ARGUMENTS service STOPPED!");
		return;
	}

	if (cmd_line.front() == L'"')
	{
		size_t endQuote = cmd_line.find(L'"', 1);
		if (endQuote != std::wstring::npos)
			_exe_path = cmd_line.substr(1, endQuote - 1);
	}
	else
	{
		size_t spacePos = cmd_line.find(L' ');
		if (spacePos != std::wstring::npos)
			_exe_path = cmd_line.substr(0, spacePos);
		else
			_exe_path = cmd_line;
	}

	_exe_name = _exe_path.filename().wstring();

	_working_dir = _exe_path.parent_path();

	Logger::get().setLogFile(_working_dir / (_exe_name + L".log"));

	Logger::get().info(L"_serviceMain Run");
	Logger::get().info(L"_launchChild cmd[{}].", cmd_line);
	Logger::get().info(L"_launchChild _exe_path[{}].", _exe_path.wstring());
	Logger::get().info(L"_launchChild _working_dir[{}].", _working_dir.wstring());

	_status.dwServiceType	   = SERVICE_WIN32_OWN_PROCESS;
	_status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
	_updateStatus(SERVICE_START_PENDING, NO_ERROR, 3'000);

	_h_stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	if (!_h_stop_event)
	{
		_updateStatus(SERVICE_STOPPED, GetLastError(), 0);
		Logger::get().warning(L"_h_stop_event service STOPPED!");
		return;
	}

	auto launchResult = _launchChild(cmd_line);
	if (!launchResult)
	{
		_updateStatus(SERVICE_STOPPED, launchResult.error(), 0);
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
		_h_job = _createJobAndAssignProcess(res.hProcess.get());

		_updateStatus(SERVICE_RUNNING, NO_ERROR, 0);
		Logger::get().info(L"Service RUNNING! PID: {}", res.dwProcessId);

		std::array<HANDLE, 2> handles{ _h_stop_event.get(), res.hProcess.get() };
		DWORD				  waitResult = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);

		if (waitResult == WAIT_OBJECT_0)
		{
			Logger::get().info(L"Stop signal received. Asking the child to exit gracefully...");

			bool childExited = false;

			// The child owns a hidden console (CREATE_NEW_CONSOLE): attach to it
			// and broadcast CTRL_C so the child can clean up (e.g. restore DNS).
			if (AttachConsole(res.dwProcessId))
			{
				SetConsoleCtrlHandler(IgnoreCtrlHandler, TRUE);
				GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
				SetConsoleCtrlHandler(IgnoreCtrlHandler, FALSE);
				FreeConsole();

				constexpr DWORD grace_ms = 15'000;
				childExited				 = WaitForSingleObject(res.hProcess.get(), grace_ms) == WAIT_OBJECT_0;

				if (childExited)
					Logger::get().info(L"Child exited gracefully after CTRL_C.");
				else
					Logger::get().warning(L"Child did not exit within {} ms after CTRL_C.", grace_ms);
			}
			else
			{
				Logger::get().warning(L"AttachConsole failed, error: {}", GetLastError());
			}

			if (!childExited)
			{
				Logger::get().info(L"Forcing child process termination...");

				if (!TerminateProcess(res.hProcess.get(), 1))
				{
					DWORD err = GetLastError();
					Logger::get().error(L"TerminateProcess failed, error: {}", err);
				}
				else if (WaitForSingleObject(res.hProcess.get(), 5'000) == WAIT_OBJECT_0)
					Logger::get().info(L"Child process terminated successfully.");
				else
					Logger::get().warning(L"Child process did not terminate in time.");
			}
		}

		if (_h_job)
		{
			_h_job.reset();
			Logger::get().info(L"Job object closed. Any remaining processes in job are terminated.");
		}

		res.hProcess.reset();
	}

	_updateStatus(SERVICE_STOPPED, NO_ERROR, 0);
	Logger::get().info(L"Service STOPPED!");
}

void WINAPI ServiceHost::_serviceCtrlHandler(DWORD ctrl) noexcept
{
	if (ctrl == SERVICE_CONTROL_STOP)
	{
		_updateStatus(SERVICE_STOP_PENDING, NO_ERROR, 3'000);
		SetEvent(_h_stop_event.get());
	}
}

void ServiceHost::_updateStatus(DWORD state, DWORD exitCode, DWORD waitHint) noexcept
{
	static DWORD checkpoint = 1;
	_status.dwCurrentState	= state;
	_status.dwWin32ExitCode = exitCode;
	_status.dwWaitHint		= waitHint;
	_status.dwCheckPoint	= (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
	SetServiceStatus(_status_handle, &_status);
}

std::expected<LaunchResult, DWORD> ServiceHost::_launchChild(std::wstring_view cmd_line)
{
	std::wstring cmd(cmd_line);

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

	BOOL success = CreateProcessW(
		nullptr,
		cmd.data(),
		nullptr,
		nullptr,
		TRUE,					// bInheritHandles
		CREATE_NEW_CONSOLE,		// own hidden console, so CTRL_C_EVENT reaches the child on stop
		nullptr,				// lpEnvironment
		_working_dir.wstring().c_str(),
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

UniqueHandle ServiceHost::_createJobAndAssignProcess(HANDLE hProcess)
{
	UniqueHandle h_job(CreateJobObjectW(nullptr, nullptr));
	if (!h_job)
		return nullptr;

	JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
	jeli.BasicLimitInformation.LimitFlags	  = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!SetInformationJobObject(h_job.get(), JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)))
		return nullptr;

	if (!AssignProcessToJobObject(h_job.get(), hProcess))
		return nullptr;

	return h_job;
}

std::wstring ServiceHost::_commandLine(DWORD argc, LPWSTR* argv)
{
	std::wstring cmd_line;

	if (argc < 2)
	{
		LPWSTR	pFullCmd   = GetCommandLineW();
		int		local_argc = 0;
		LPWSTR* local_argv = CommandLineToArgvW(pFullCmd, &local_argc);
		if (local_argv && local_argc >= 2)
		{
			for (int i = 1; i < local_argc; ++i)
			{
				if (i > 1)
					cmd_line += L" ";

				std::wstring arg = local_argv[i];
				if (arg.find(L' ') != std::wstring::npos)
				{
					cmd_line += L'"';
					cmd_line += arg;
					cmd_line += L'"';
				}
				else
					cmd_line += arg;
			}

			LocalFree(local_argv);
		}
	}
	else
	{
		for (DWORD i = 1; i < argc; ++i)
		{
			if (i > 1)
				cmd_line += L" ";

			std::wstring arg = argv[i];
			if (arg.size() >= 2 && arg.front() == L'"' && arg.back() == L'"')
				arg = arg.substr(1, arg.size() - 2);

			cmd_line += arg;
		}
	}

	return cmd_line;
}
