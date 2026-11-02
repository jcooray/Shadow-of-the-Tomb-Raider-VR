#pragma once

#include <string>

namespace vrmod {

// Update notice (1.2): once per start, in the background, the mod asks GitHub's public API for the latest release of
// the mod ([update] check=1, the default; the overlay's Support page switches it off). Nothing is sent but the request
// itself; nothing is downloaded or run. A newer release is shown in the overlay with the start of its release notes.
void UpdateCheckStart();
// A newer release than this build: its version ("1.2.0") and the "What's new" part of its notes as plain text.
bool UpdateAvailable(std::string* version, std::string* notes);
// For the overlay's Support page: "up to date", "1.2.0 available", "checking...", "off", "couldn't reach GitHub".
std::string UpdateStatusText();

}  // namespace vrmod
