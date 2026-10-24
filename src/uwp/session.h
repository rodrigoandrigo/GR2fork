#pragma once

#include <string>
#include <stop_token>

namespace Core::Uwp {
bool StopRequested();
bool PauseRequested();
std::stop_token GuestStopToken();
void RegisterGuestWorker(std::stop_source source);
void StopGuestWorkers();
void SetGuestReady(bool ready);
void ReportError(std::string message);
void FailGuestSession(std::string message);
void ClearStorage();
std::string PrepareBootStorage(std::string executable);
} // namespace Core::Uwp
