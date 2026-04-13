#include "service_host.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

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
		{ const_cast<LPWSTR>(L""), ServiceMain },
		{				 nullptr,	   nullptr }
	};

	if (!StartServiceCtrlDispatcherW(serviceTable))
		return static_cast<int>(GetLastError());
	return 0;
}

void WINAPI ServiceHost::ServiceMain(DWORD argc, LPWSTR* argv)
{
	s_StatusHandle = RegisterServiceCtrlHandlerW(L"", ServiceCtrlHandler);
	if (!s_StatusHandle)
		return;

	s_Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	UpdateStatus(SERVICE_START_PENDING, NO_ERROR, 3'000);

	s_hStopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	if (!s_hStopEvent)
	{
		UpdateStatus(SERVICE_STOPPED, GetLastError(), 0);
		return;
	}

	auto args	 = std::span(argv, argc) | std::views::drop(1);
	auto cmdLine = BuildCommandLine({ args.begin(), args.end() });
	if (cmdLine.empty())
	{
		UpdateStatus(SERVICE_STOPPED, ERROR_BAD_ARGUMENTS, 0);
		return;
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

	UpdateStatus(SERVICE_RUNNING, NO_ERROR, 0);

	std::array<HANDLE, 2> handles{ s_hStopEvent.get(), s_hChildProcess.get() };
	DWORD				  waitResult = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);

	if (waitResult == WAIT_OBJECT_0)
	{
		GenerateConsoleCtrlEvent(CTRL_C_EVENT, pi.dwProcessId);
		if (WaitForSingleObject(s_hChildProcess.get(), 15'000) != WAIT_OBJECT_0)
			TerminateProcess(s_hChildProcess.get(), 0);
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

std::wstring ServiceHost::BuildCommandLine(std::span<LPWSTR> args)
{
	std::wstring cmd;
	for (auto arg : args)
	{
		if (!cmd.empty())
			cmd += L" ";
		if (std::wstring_view(arg).find(L' ') != std::wstring_view::npos)
		{
			cmd += L'"';
			cmd += arg;
			cmd += L'"';
		}
		else
		{
			cmd += arg;
		}
	}
	return cmd;
}

std::expected<PROCESS_INFORMATION, DWORD> ServiceHost::LaunchChild(std::wstring_view cmdLine)
{
	STARTUPINFOW si{ };
	si.dwFlags	   = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;

	PROCESS_INFORMATION pi{};
	if (CreateProcessW(nullptr, const_cast<LPWSTR>(cmdLine.data()), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi))
		return pi;

	return std::unexpected(GetLastError());
}
