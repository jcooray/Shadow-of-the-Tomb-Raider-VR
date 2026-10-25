#pragma once
#include <string>

namespace modlog {

// Opens <game-dir>\vrmod.log next to this DLL. Safe to call once from the init thread.
void Init();
void Close();

// Thread-safe. Appends a timestamped line.
void Line(const std::string& message);

} // namespace modlog
