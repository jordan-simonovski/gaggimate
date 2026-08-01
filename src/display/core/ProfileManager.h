#pragma once
#ifndef PROFILEMANAGER_H
#define PROFILEMANAGER_H
#include "PluginManager.h"
#include <FS.h>
#include <display/core/Settings.h>
#include <display/core/utils.h>
#include <display/models/profile.h>

class ProfileManager {
  public:
    ProfileManager(fs::FS *fs, String dir, Settings &settings, PluginManager *plugin_manager);

    void setup();
    std::vector<String> listProfiles();
    bool loadProfile(const String &uuid, Profile &outProfile);
    // reselect=false skips the post-write reload/reselect. Only the grind-flush
    // path uses it: that path saves `selectedProfile` itself, so reloading would
    // blank the shared object (phases included) while another task may be copying
    // it into a BrewProcess.
    bool saveProfile(Profile &profile, bool reselect = true);
    bool deleteProfile(const String &uuid);
    bool profileExists(const String &uuid);
    void selectProfile(const String &uuid);
    Profile &getSelectedProfile();
    bool loadSelectedProfile(Profile &outProfile);
    std::vector<String> getFavoritedProfiles(bool validate = false);

    void addFavoritedProfile(String id);
    void removeFavoritedProfile(String id);

    // Grind level / dose live on the profile but are edited one encoder click at
    // a time from the brew screen. Persisting each click would rewrite the
    // profile JSON per step, so changes mark the profile dirty and are flushed
    // once the user stops turning (or immediately, before anything reloads the
    // profile from disk).
    void markSelectedDirty() { _dirtyAt = millis(); }
    void flushSelected(bool force = false);

  private:
    static constexpr unsigned long DIRTY_DEBOUNCE_MS = 2000;
    unsigned long _dirtyAt = 0; // 0 = clean
    Profile selectedProfile{};
    PluginManager *_plugin_manager;
    Settings &_settings;
    fs::FS *_fs;
    String _dir;
    bool ensureDirectory() const;
    String profilePath(const String &uuid) const;
    void migrate(const std::vector<String> &existingProfiles);
};

#endif // PROFILEMANAGER_H
