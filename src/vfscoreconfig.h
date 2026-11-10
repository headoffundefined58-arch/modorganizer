#pragma once

/*
 * MO2 core upgrade - instance-scoped, spec-driven VFS destinations.
 *
 * This file is part of ModOrganizer.exe itself, NOT a plugin. It lets a single
 * MO2 *instance* declare additional virtual destinations for mod files, so
 * games whose mods do not live in one "Data" folder can still be managed with
 * MO2's native enable/disable, priority, conflict and overwrite machinery.
 *
 * Configuration lives in the current instance's ModOrganizer.ini:
 *
 *   [VFSCore]
 *   data_root_override=${GAME_ROOT}          ; optional, replaces dataDirectory()
 *
 *   [VFSRootMappings]
 *   size=2
 *   1\source=Root
 *   1\destination=${GAME_ROOT}
 *   2\source=bin
 *   2\destination=${GAME_ROOT}/bin/x64
 *
 * Placeholders: ${GAME_ROOT} = game->gameDirectory(), ${DATA_DIR} =
 * game->dataDirectory(). A relative destination is resolved against the game
 * root. A "source" is always relative to a mod folder and must never escape
 * it. Instances without these sections produce an empty config, in which case
 * fileMapping() and DirectoryRefresher::refresh() behave byte-for-byte like
 * an unpatched MO2 (zero impact on Skyrim and other Bethesda instances).
 */

#include "instancemanager.h"

#include <QDir>
#include <QSettings>
#include <QString>
#include <QVector>

#include <mutex>

struct VFSCoreRule
{
  QString source;       // relative to each mod folder, never absolute
  QString destination;  // absolute path after placeholder expansion
};

struct VFSCoreConfig
{
  QString dataRootOverride;  // empty -> keep the game plugin's dataDirectory()
  QVector<VFSCoreRule> rules;

  bool isEmpty() const
  {
    return dataRootOverride.isEmpty() && rules.isEmpty();
  }
};

inline QString vfsCoreExpandPath(const QString& raw, const QDir& gameRoot,
                                 const QDir& dataDir)
{
  QString path = raw.trimmed();
  path.replace("${GAME_ROOT}", gameRoot.absolutePath());
  path.replace("${DATA_DIR}", dataDir.absolutePath());
  if (!QDir::isAbsolutePath(path)) {
    path = gameRoot.filePath(path);
  }
  return QDir::toNativeSeparators(QDir::cleanPath(path));
}

inline VFSCoreConfig vfsCoreLoadConfig(const QDir& gameRoot, const QDir& dataDir)
{
  VFSCoreConfig config;

  const auto instance = InstanceManager::singleton().currentInstance();
  if (!instance) {
    return config;
  }

  QSettings ini(instance->iniPath(), QSettings::IniFormat);
  if (ini.status() != QSettings::NoError) {
    return config;
  }

  const QString override =
      ini.value("VFSCore/data_root_override").toString().trimmed();
  if (!override.isEmpty()) {
    config.dataRootOverride = vfsCoreExpandPath(override, gameRoot, dataDir);
  }

  const int count = ini.beginReadArray("VFSRootMappings");
  for (int i = 0; i < count; ++i) {
    ini.setArrayIndex(i);
    const QString source      = ini.value("source").toString().trimmed();
    const QString destination = ini.value("destination").toString().trimmed();
    if (source.isEmpty() || destination.isEmpty()) {
      continue;
    }
    // A rule must never point outside its own mod folder.
    if (QDir::isAbsolutePath(source) || source.contains("..")) {
      continue;
    }
    VFSCoreRule rule;
    rule.source      = QDir::fromNativeSeparators(source);
    rule.destination = vfsCoreExpandPath(destination, gameRoot, dataDir);
    config.rules.push_back(rule);
  }
  ini.endArray();

  return config;
}

// Read on the main thread (warmed up by OrganizerCore::setManagedGame),
// then shared read-only with the directory refresher worker thread. The
// settings dialog invalidates the cache after editing the game profile.
namespace vfsCoreCacheDetail
{
inline std::mutex& mutex()
{
  static std::mutex m;
  return m;
}
inline VFSCoreConfig& config()
{
  static VFSCoreConfig c;
  return c;
}
inline bool& loaded()
{
  static bool b = false;
  return b;
}
}  // namespace vfsCoreCacheDetail

inline VFSCoreConfig vfsCoreCachedConfig(const QDir& gameRoot,
                                         const QDir& dataDir)
{
  std::lock_guard<std::mutex> lock(vfsCoreCacheDetail::mutex());
  if (!vfsCoreCacheDetail::loaded()) {
    vfsCoreCacheDetail::config() = vfsCoreLoadConfig(gameRoot, dataDir);
    vfsCoreCacheDetail::loaded() = true;
  }
  return vfsCoreCacheDetail::config();
}

inline void vfsCoreInvalidateCache()
{
  std::lock_guard<std::mutex> lock(vfsCoreCacheDetail::mutex());
  vfsCoreCacheDetail::loaded() = false;
}
