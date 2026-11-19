// Kindle one-time steps at start-up (called from the patched main.cpp right
// after the settings load). Each runs once, guarded by a marker file in the
// working folder (/mnt/us/crosspoint on the Kindle):
//
// .kindle-library-3  Rebuild the Library index once, so an index made by an
//                    earlier build picks up the current folder exclusions
//                    (Kindle system and jailbreak folders, dot files; see
//                    tools/build.py). Bump the number when they change.
// .kindle-defaults-1 Switch both page-turn gestures to "tap and swipe" and the
//                    reader menu to a tap. CrossPoint's defaults (swipe only)
//                    suit button devices. Later changes in Settings are kept.
//                    Page taps work even if the settings are reset later: the
//                    build patches gestureAllowsTap() to always allow them.

#include <cstdio>

#include "CrossPointSettings.h"
#include "LibraryBuilder.h"

static bool firstTime(const char* marker, const char* note) {
  if (FILE* f = std::fopen(marker, "r")) {
    std::fclose(f);
    return false;
  }
  if (FILE* f = std::fopen(marker, "w")) {
    std::fputs(note, f);
    std::fclose(f);
  }
  return true;
}

void kindleApplySettingsDefaultsOnce() {
  // The Library's Kindle exclusions changed (system folders instead of all
  // .txt/.md files): rebuild an index made by an earlier build.
  if (firstTime(".kindle-library-3", "Library rebuilt without Kindle system and jailbreak folders\n")) {
    library::markLibraryIndexDirty();
    std::fprintf(stderr, "[kindle] library index marked for rebuild\n");
  }
  static const char* const kMarker = ".kindle-defaults-1";  // in /mnt/us/crosspoint
  if (FILE* f = std::fopen(kMarker, "r")) {
    std::fclose(f);
    return;
  }
  SETTINGS.pageTurnGesture = CrossPointSettings::TAP_AND_SWIPE;
  SETTINGS.previousPageGesture = CrossPointSettings::TAP_AND_SWIPE;
  SETTINGS.showReaderMenu = CrossPointSettings::READER_MENU_TAP;
  SETTINGS.saveToFile();
  if (FILE* f = std::fopen(kMarker, "w")) {
    std::fputs("CrossPoint Kindle defaults applied (tap page turns)\n", f);
    std::fclose(f);
  }
  std::fprintf(stderr, "[kindle] applied Kindle default settings (tap page turns)\n");
}
