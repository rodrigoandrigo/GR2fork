#pragma once

#include <string>

namespace Core::Uwp {
bool StopRequested();
bool PauseRequested();
void SetGuestReady(bool ready);
void ReportError(std::string message);
void FailGuestSession(std::string message);
void ClearStorage();
std::string PrepareBootStorage(std::string executable);
} // namespace Core::Uwp
