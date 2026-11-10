#pragma once

/*
 * MO2 core upgrade - instance-scoped game profile.
 *
 * This file is part of ModOrganizer.exe itself, NOT a plugin.
 *
 * Bethesda games all look the same to MO2: one Data folder, saves under
 * Documents/My Games, loose files dropped into Data. Other games do not.
 * This header lets each *instance* declare the parts of a game that differ:
 *
 *   [VFSContentLayout]  where unstructured ("bare") mod files belong
 *   [VFSGameProfile]    where the game's saves live and how they are isolated
 *
 * Example for NARUTO X BORUTO Ultimate Ninja STORM CONNECTIONS (RUNE build):
 *
 *   [VFSContentLayout]
 *   bare_destination=${GAME_ROOT}/data_win32/spc
 *   bare_patterns=*.xfbin;*.dds;*.tex.xfbin
 *   layout_markers=data_win32;data;moddingapi;Root;engine
 *
 *   [VFSGameProfile]
 *   save_source=${PUBLIC_DOCUMENTS}/Steam/RUNE/1020790/remote
 *   save_pattern=stormc.s
 *   save_per_profile=true
 *   seed_existing_saves=true
 *
 * Placeholders: ${GAME_ROOT}, ${DATA_DIR}, ${PUBLIC_DOCUMENTS},
 * ${DOCUMENTS}, ${APPDATA}, ${LOCALAPPDATA}, ${USERPROFILE}. A relative
 * path is resolved against the game root. Instances without these sections
 * get an empty config, in which case fileMapping(), the Data tab and the
 * Saves tab behave exactly like an unpatched MO2 (zero impact on Skyrim).
 */

#include "instancemanager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <mutex>

struct GameProfileConfig
{
  QString bareDestination;   // absolute; empty disables bare-mod normalization
  QStringList barePatterns;  // file names that make a mod "bare"
  QStringList layoutMarkers; // directories that mean "already laid out"
  QString saveSource;        // absolute; the game's real save directory
  QString savePattern;       // e.g. "stormc.s", "*.sav"
  bool savePerProfile = false;
  bool seedSaves      = false;

  bool isEmpty() const
  {
    return bareDestination.isEmpty() && saveSource.isEmpty();
  }
};

inline QStringList gpSplitList(const QString& raw)
{
  QStringList result;
  for (const QString& part : raw.split(';')) {
    const QString value = part.trimmed();
    if (!value.isEmpty()) {
      result.append(value);
    }
  }
  return result;
}

inline QString gpEnv(const char* name)
{
  return QString::fromLocal8Bit(qgetenv(name));
}

inline QString gpExpandPath(const QString& raw, const QDir& gameRoot,
                            const QDir& dataDir)
{
  QString path = raw.trimmed();
  path.replace("${GAME_ROOT}", gameRoot.absolutePath());
  path.replace("${DATA_DIR}", dataDir.absolutePath());
  path.replace("${PUBLIC_DOCUMENTS}", gpEnv("PUBLIC") + "/Documents");
  path.replace("${USERPROFILE}", gpEnv("USERPROFILE"));
  path.replace("${LOCALAPPDATA}", gpEnv("LOCALAPPDATA"));
  path.replace("${APPDATA}", gpEnv("APPDATA"));
  path.replace("${DOCUMENTS}",
               gpEnv("USERPROFILE") + "/Documents");
  if (!QDir::isAbsolutePath(path)) {
    path = gameRoot.filePath(path);
  }
  return QDir::toNativeSeparators(QDir::cleanPath(path));
}

inline QString gameProfileIniPath()
{
  const auto instance = InstanceManager::singleton().currentInstance();
  return instance ? instance->iniPath() : QString();
}

inline GameProfileConfig gameProfileLoadConfig(const QDir& gameRoot,
                                               const QDir& dataDir)
{
  GameProfileConfig config;

  const QString iniPath = gameProfileIniPath();
  if (iniPath.isEmpty()) {
    return config;
  }

  QSettings ini(iniPath, QSettings::IniFormat);
  if (ini.status() != QSettings::NoError) {
    return config;
  }

  const QString bare =
      ini.value("VFSContentLayout/bare_destination").toString().trimmed();
  if (!bare.isEmpty()) {
    config.bareDestination = gpExpandPath(bare, gameRoot, dataDir);
  }
  config.barePatterns = gpSplitList(
      ini.value("VFSContentLayout/bare_patterns", "*.xfbin").toString());
  config.layoutMarkers = gpSplitList(
      ini.value("VFSContentLayout/layout_markers",
                "data_win32;data;moddingapi;Root;engine")
          .toString());

  const QString save =
      ini.value("VFSGameProfile/save_source").toString().trimmed();
  if (!save.isEmpty()) {
    config.saveSource = gpExpandPath(save, gameRoot, dataDir);
  }
  config.savePattern =
      ini.value("VFSGameProfile/save_pattern", "*").toString().trimmed();
  config.savePerProfile =
      ini.value("VFSGameProfile/save_per_profile", false).toBool();
  config.seedSaves =
      ini.value("VFSGameProfile/seed_existing_saves", false).toBool();

  return config;
}

namespace gpCacheDetail
{
inline std::mutex& mutex()
{
  static std::mutex m;
  return m;
}
inline GameProfileConfig& config()
{
  static GameProfileConfig c;
  return c;
}
inline bool& loaded()
{
  static bool b = false;
  return b;
}
}  // namespace gpCacheDetail

// A snapshot is returned by value so worker threads never hold a reference
// into the cache while the settings dialog refreshes it.
inline GameProfileConfig gameProfileSnapshot(const QDir& gameRoot,
                                             const QDir& dataDir)
{
  std::lock_guard<std::mutex> lock(gpCacheDetail::mutex());
  if (!gpCacheDetail::loaded()) {
    gpCacheDetail::config() = gameProfileLoadConfig(gameRoot, dataDir);
    gpCacheDetail::loaded() = true;
  }
  return gpCacheDetail::config();
}

inline void gameProfileInvalidateCache()
{
  std::lock_guard<std::mutex> lock(gpCacheDetail::mutex());
  gpCacheDetail::loaded() = false;
}

// True when a mod contains loose game assets but no game-layout directory.
// Such a mod is what a user gets when a community package is unzipped
// straight into mods/<name>/ (e.g. a bare 3ksnbod1.xfbin).
inline bool gameProfileIsBareMod(const QString& modPath,
                                 const GameProfileConfig& config)
{
  if (config.bareDestination.isEmpty() || config.barePatterns.isEmpty()) {
    return false;
  }
  const QDir dir(modPath);
  if (!dir.exists()) {
    return false;
  }
  for (const QString& marker : config.layoutMarkers) {
    if (dir.exists(marker)) {
      return false;
    }
  }
  for (const QFileInfo& info : dir.entryInfoList(QDir::Files)) {
    for (const QString& pattern : config.barePatterns) {
      if (QDir::match(pattern, info.fileName())) {
        return true;
      }
    }
  }
  return false;
}

// Copy the game's existing saves into the current profile once, so enabling
// per-profile saves never makes the player's progress "disappear". Only the
// MO2 profile directory is written to; the game directory is never touched.
inline void gameProfileSeedSaves(const QString& profileSaves,
                                 const GameProfileConfig& config)
{
  QDir target(profileSaves);
  if (!target.exists() && !QDir().mkpath(profileSaves)) {
    return;
  }
  if (!config.seedSaves || config.saveSource.isEmpty() ||
      !target.entryList(QDir::Files).isEmpty()) {
    return;
  }
  const QDir source(config.saveSource);
  if (!source.exists()) {
    return;
  }
  const QStringList patterns = gpSplitList(config.savePattern);
  for (const QFileInfo& info : source.entryInfoList(QDir::Files)) {
    for (const QString& pattern : patterns) {
      if (QDir::match(pattern, info.fileName())) {
        QFile::copy(info.absoluteFilePath(), target.filePath(info.fileName()));
        break;
      }
    }
  }
}
