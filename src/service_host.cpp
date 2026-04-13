#include "service_host.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>
#include <fstream>

void HandleDeleter::operator()(HANDLE h) const noexcept
{
	if (h && h != INVALID_HANDLE_VALUE)
		CloseHandle(h);
}

int ServiceHost::Run(int argc, wchar_t* /*argv*/[])
{
	if (argc < 2)
		return 1;

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

	s_Status.dwServiceType		= SERVICE_WIN32_OWN_PROCESS;
	s_Status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
	UpdateStatus(SERVICE_START_PENDING, NO_ERROR, 3'000);

	s_hStopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	if (!s_hStopEvent)
	{
		UpdateStatus(SERVICE_STOPPED, GetLastError(), 0);
		return;
	}

	if (argc < 2)
	{
		UpdateStatus(SERVICE_STOPPED, ERROR_BAD_ARGUMENTS, 0);
		return;
	}

	std::wstring cmdLine;
	for (DWORD i = 1; i < argc; ++i)
	{
		if (i > 1)
			cmdLine += L" ";

		std::wstring arg = argv[i];
		if (arg.size() >= 2 && arg.front() == L'"' && arg.back() == L'"')
			arg = arg.substr(1, arg.size() - 2);

		cmdLine += arg;
	}

	auto launchResult = LaunchChild(cmdLine);
	if (!launchResult)
	{
		UpdateStatus(SERVICE_STOPPED, launchResult.error(), 0);
		return;
	}

	auto& pi = *launchResult;
	s_hChildProcess.reset(pi.hProcess);
	UniqueHandle hThread(pi.hThread);

	if (s_hChildProcess)
	{
		s_hJob = CreateJobAndAssignProcess(s_hChildProcess.get());

		UpdateStatus(SERVICE_RUNNING, NO_ERROR, 0);

		DWORD exitCode = 0;
		if (WaitForSingleObject(s_hChildProcess.get(), 5'000) == WAIT_OBJECT_0)
		{
			GetExitCodeProcess(s_hChildProcess.get(), &exitCode);
			std::wofstream log(L"C:\\svchost_debug.log", std::ios::app);
			log << L"Child process exited early with code: " << exitCode << std::endl;
			UpdateStatus(SERVICE_STOPPED, exitCode, 0);
			return;
		}

		std::array<HANDLE, 2> handles{ s_hStopEvent.get(), s_hChildProcess.get() };
		DWORD				  waitResult = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);

		if (waitResult == WAIT_OBJECT_0)
		{
			if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, pi.dwProcessId))
				TerminateProcess(s_hChildProcess.get(), 1);
			else
			{
				DWORD waitChild = WaitForSingleObject(s_hChildProcess.get(), 10'000);
				if (waitChild != WAIT_OBJECT_0)
				{
					TerminateProcess(s_hChildProcess.get(), 1);
					WaitForSingleObject(s_hChildProcess.get(), 2'000);
				}
			}
		}

		if (s_hJob)
			s_hJob.reset();

		s_hChildProcess.reset();
	}

	UpdateStatus(SERVICE_STOPPED, NO_ERROR, 0);
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

std::expected<PROCESS_INFORMATION, DWORD> ServiceHost::LaunchChild(std::wstring_view cmdLine)
{
	std::wofstream log(L"C:\\svchost_debug.log", std::ios::app);
	log << L"LaunchChild: " << cmdLine << std::endl;
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

	std::wstring workingDir;
	size_t		 lastSlash = exePath.find_last_of(L"\\/");
	if (lastSlash != std::wstring::npos)
		workingDir = exePath.substr(0, lastSlash);

	STARTUPINFOW si{};
	si.dwFlags	   = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;

	PROCESS_INFORMATION pi{};
	BOOL				success = CreateProcessW(
		   nullptr,
		   cmd.data(),
		   nullptr,
		   nullptr,
		   FALSE,
		   CREATE_NEW_CONSOLE,
		   nullptr,
		   workingDir.empty() ? nullptr : workingDir.c_str(),
		   &si,
		   &pi
	   );
	if (!success)
	{
		DWORD err = GetLastError();
		log << L"CreateProcess failed, error: " << err << std::endl;
		return std::unexpected(err);
	}
	log << L"Process created, PID: " << pi.dwProcessId << std::endl;
	return pi;
	return std::unexpected(GetLastError());
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
