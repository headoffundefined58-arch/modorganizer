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
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVector>
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

  // ---- virtual overlay layer -------------------------------------------
  // When layerRoot is set every enabled mod is *projected* into it instead of
  // being mapped onto the game's Data directory. This is how a game whose
  // engine reads loose files from <game>/data_win32 gets a mod stack that
  // behaves like a normal MO2 Data tree: same relative path = same file, so
  // priorities, conflicts and overwrite keep their usual meaning.
  bool hardlinkDeploy = false;  // deploy=hardlink: also materialize the layer
                                // with hard links (USVFS-independent fallback)

  QString layerRoot;         // absolute; the engine's loose root, e.g. <game>/data
  QString gameRoot;          // absolute; <game>, for payload that must sit there
  QString routingPath;       // absolute; JSON {"<file name>": "<sub dir>"}
  bool inferNested = false;  // strip a single wrapper folder around the payload
  QHash<QString, QString> routing;  // lower-case file name -> sub dir ("" = root)
  QSet<QString> routingDirs;        // every sub dir the table can produce
  QSet<QString> routingTopDirs;     // first segment of every dir above (spc, ui, ...)

  bool layered() const { return !layerRoot.isEmpty(); }

  bool deployHardlinks() const { return layered() && hardlinkDeploy; }

  bool isEmpty() const
  {
    return bareDestination.isEmpty() && saveSource.isEmpty() && layerRoot.isEmpty();
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

// [VFSUIPaths] hidden_fields lists generic path rows this game has no
// use for. Purely presentational: an absent or empty value shows every
// row, so Bethesda instances are completely unaffected.
inline QStringList gameProfileHiddenPathFields()
{
  const QString iniPath = gameProfileIniPath();
  if (iniPath.isEmpty()) {
    return {};
  }
  QSettings ini(iniPath, QSettings::IniFormat);
  return gpSplitList(ini.value("VFSUIPaths/hidden_fields").toString());
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

  // ---- virtual overlay layer ----
  const QString layer =
      ini.value("VFSContentLayout/layer").toString().trimmed();
  config.gameRoot = QDir::toNativeSeparators(gameRoot.absolutePath());
  if (!layer.isEmpty()) {
    config.layerRoot = gpExpandPath(layer, gameRoot, dataDir);
    // Defensive: a layer that does not exist makes USVFS drop every mapping for
    // the instance ("<layer> doesn't exist" in the usvfs log) and the game then
    // runs completely unmodded with no visible error. The only repair that is
    // safe here is towards data_win32 - the loose root this engine family
    // actually reads (data/ has LOWER priority than the CPKs, so mods landed
    // there never apply). With CREATETARGET the mapping creates a missing
    // data_win32 by itself, so a missing folder is not fatal any more.
    if (!QDir(config.layerRoot).exists()) {
      const QString candidate = QDir::toNativeSeparators(
          gameRoot.absoluteFilePath("data_win32"));
      if (QDir(candidate).exists()) {
        config.layerRoot = candidate;
      }
    }
    config.inferNested =
        ini.value("VFSContentLayout/infer_nested", true).toBool();

    const QString routing =
        ini.value("VFSContentLayout/routing").toString().trimmed();
    if (!routing.isEmpty()) {
      QString routingFile = routing;
      if (!QDir::isAbsolutePath(routingFile)) {
        routingFile = QFileInfo(iniPath).absolutePath() + "/" + routingFile;
      }
      config.routingPath = QDir::toNativeSeparators(QDir::cleanPath(routingFile));

      QFile table(config.routingPath);
      if (table.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(table.readAll());
        if (doc.isObject()) {
          const QJsonObject object = doc.object();
          for (auto it = object.begin(); it != object.end(); ++it) {
            const QString dir = it.value().toString();
            config.routing.insert(it.key().toLower(), dir);
            config.routingDirs.insert(dir);
            if (!dir.isEmpty()) {
              // "ui/flash/OTHER/name_tlp/chi" -> "ui": a mod that keeps its
              // payload under ui/ or message/ is already layer-relative.
              config.routingTopDirs.insert(dir.section('/', 0, 0));
            }
          }
        }
      }
    }
  }

  config.hardlinkDeploy =
      ini.value("VFSContentLayout/deploy").toString().trimmed()
          .compare("hardlink", Qt::CaseInsensitive) == 0;

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

// One (source, destination) pair handed to the VFS. A directory entry makes the
// whole subtree visible at `destination`; a file entry places a single loose
// file, which is how a mod that keeps its payload in the mod root gets routed
// into the category the engine looks at.
struct GameProfileMapping
{
  QString source;
  QString destination;
  bool isDirectory = true;
};

// The legacy bare destination expressed relative to the layer root, e.g.
// <game>/data_win32/spc -> "spc". Files the routing table does not know about
// keep going there, so mods built for the old scheme keep working unchanged.
inline QString gpLayerLegacySubdir(const GameProfileConfig& config)
{
  if (config.layerRoot.isEmpty() || config.bareDestination.isEmpty()) {
    return {};
  }
  const QString layer = QDir::cleanPath(config.layerRoot);
  const QString bare  = QDir::cleanPath(config.bareDestination);
  if (bare == layer) {
    return {};
  }
  if (bare.startsWith(layer + "/")) {
    return bare.mid(layer.size() + 1);
  }
  return {};
}

inline QString gpLayerPath(const GameProfileConfig& config, const QString& sub,
                           const QString& name = QString())
{
  QString path = QDir::cleanPath(config.layerRoot);
  if (!sub.isEmpty()) {
    path += "/" + sub;
  }
  if (!name.isEmpty()) {
    path += "/" + name;
  }
  return QDir::toNativeSeparators(path);
}

// Files that are actual game payload. meta.ini is MO2's own metadata and must
// not count as content: a package that is "meta.ini + one wrapper folder" is a
// nested mod, not a flat one, and meta.ini must never be projected into the
// game directory.
inline QStringList gpPayloadFiles(const QDir& dir)
{
  QStringList result;
  const QFileInfoList entries = dir.entryInfoList(QDir::Files);
  for (const QFileInfo& info : entries) {
    if (info.fileName().compare("meta.ini", Qt::CaseInsensitive) != 0) {
      result.append(info.fileName());
    }
  }
  return result;
}

// Loader payload that must sit next to the executable, never under the loose
// data root: a mod whose only content is "dinput8.dll" or an "*.ini" would
// otherwise be projected into <layer>/spc and do nothing. The routing table has
// no entry for these, and their extension is unambiguous, so it is safe to use
// the game root for them.
inline bool gpIsRootPayload(const QString& fileName)
{
  static const QSet<QString> kSuffixes{
      "dll", "exe", "asi", "ini", "bat", "cmd", "lnk"};
  return kSuffixes.contains(QFileInfo(fileName).suffix().toLower());
}

// Project a directory that holds loose payload (no data_win32/ or Root/ inside).
// Each file is routed by name through the CPK-derived table, which is what lets
// an unzipped community package land exactly where the engine reads it.
inline void gpProjectLoose(const QDir& dir, const GameProfileConfig& config,
                           QVector<GameProfileMapping>& out)
{
  const QString legacy      = gpLayerLegacySubdir(config);
  const QStringList allFiles = gpPayloadFiles(dir);
  const QStringList subdirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

  // Split loader payload (dll/exe/ini ...) away from asset payload: it belongs
  // in the game root, and it must not make the whole directory look "mixed".
  QStringList files;
  int rootPayload = 0;
  for (const QString& name : allFiles) {
    if (!config.gameRoot.isEmpty() && gpIsRootPayload(name) &&
        config.routing.value(name.toLower()).isEmpty()) {
      out.append({QDir::toNativeSeparators(dir.filePath(name)),
                  QDir::toNativeSeparators(config.gameRoot + "/" + name), false});
      ++rootPayload;
    } else {
      files.append(name);
    }
  }

  QMap<QString, QStringList> byCategory;
  for (const QString& name : files) {
    QString category = config.routing.value(name.toLower());
    if (category.isEmpty()) {
      category = legacy;  // unknown asset: behave like the old bare scheme
    }
    byCategory[category].append(name);
  }

  bool subdirNeedsRouting = false;
  for (const QString& sub : subdirs) {
    if (config.routingTopDirs.contains(sub) || config.routingDirs.contains(sub)) {
      subdirNeedsRouting = true;
    }
  }

  if (byCategory.size() <= 1 && !subdirNeedsRouting && files.size() <= 4096 &&
      rootPayload == 0) {
    // One mapping for the whole directory. Cheap, and the resulting relative
    // paths are identical to a "data_win32/..." mod, so MO2 shows the conflict.
    const QString category =
        byCategory.isEmpty() ? legacy : byCategory.firstKey();
    out.append({QDir::toNativeSeparators(dir.absolutePath()),
                gpLayerPath(config, category), true});
    return;
  }

  for (auto it = byCategory.begin(); it != byCategory.end(); ++it) {
    for (const QString& name : it.value()) {
      out.append({QDir::toNativeSeparators(dir.filePath(name)),
                  gpLayerPath(config, it.key(), name), false});
    }
  }

  for (const QString& sub : subdirs) {
    if (config.routingTopDirs.contains(sub) || config.routingDirs.contains(sub)) {
      // The folder already names a real category (sound/, ui/, effect/ ...):
      // keep its structure.
      out.append({QDir::toNativeSeparators(dir.filePath(sub)),
                  gpLayerPath(config, sub), true});
      continue;
    }

    // Otherwise this is a wrapper folder left over from unzipping. If its
    // payload is flat, project the files individually so they land in the real
    // category instead of under spc/<wrapper name>/.
    const QDir subDir(dir.filePath(sub));
    const QStringList subFiles = gpPayloadFiles(subDir);
    const QStringList subDirs =
        subDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (subDirs.isEmpty() && !subFiles.isEmpty() && subFiles.size() <= 256) {
      for (const QString& name : subFiles) {
        QString category = config.routing.value(name.toLower());
        if (category.isEmpty()) {
          category = legacy;
        }
        out.append({QDir::toNativeSeparators(subDir.filePath(name)),
                    gpLayerPath(config, category, name), false});
      }
      continue;
    }

    const QString destSub = legacy.isEmpty() ? sub : legacy + "/" + sub;
    out.append({QDir::toNativeSeparators(dir.filePath(sub)),
                gpLayerPath(config, destSub), true});
  }
}

// Turn any mod layout into the layer-relative mapping set. Layouts handled, in
// order: data_win32/ payload, Root/ payload (Root Builder compatible), a single
// wrapper folder around either of those (and two levels deep), and finally a
// loose payload at the mod root. Returns an empty vector when the instance has
// no layer configured, so unpatched behaviour is preserved bit for bit.
inline QVector<GameProfileMapping> gameProfileModMappings(
    const QString& modPath, const GameProfileConfig& config)
{
  QVector<GameProfileMapping> out;
  if (!config.layered()) {
    return out;
  }
  QDir dir(modPath);
  if (!dir.exists()) {
    return out;
  }

  auto tryLayout = [&](const QDir& base) -> bool {
    if (base.exists("data_win32")) {
      out.append({QDir::toNativeSeparators(base.filePath("data_win32")),
                  gpLayerPath(config, QString()), true});
      return true;
    }
    if (base.exists("Root")) {
      const QDir root(base.filePath("Root"));
      if (root.exists("data_win32")) {
        out.append({QDir::toNativeSeparators(root.filePath("data_win32")),
                    gpLayerPath(config, QString()), true});
      } else if (root.exists("data")) {
        out.append({QDir::toNativeSeparators(root.filePath("data")),
                    gpLayerPath(config, QString()), true});
      } else {
        // Root/ that is not a data tree is loader payload (dll/exe/ini): Root
        // Builder's whole purpose, done by the core instead of by copying.
        out.append({QDir::toNativeSeparators(root.absolutePath()),
                    QDir::toNativeSeparators(config.gameRoot), true});
      }
      return true;
    }
    return false;
  };

  if (tryLayout(dir)) {
    return out;
  }

  const QStringList files = gpPayloadFiles(dir);
  const QStringList dirs  = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

  // A mod that already splits its payload into real categories (spc/, sound/,
  // ui/, effect/ ...) is layer-relative by construction. Map those folders as
  // they are, without applying the wrapper heuristic - otherwise "spc/" would
  // be mistaken for a wrapper folder and re-routed file by file.
  bool hasCategoryDir = false;
  for (const QString& d : dirs) {
    if (config.routingTopDirs.contains(d) || config.routingDirs.contains(d)) {
      hasCategoryDir = true;
      break;
    }
  }
  if (hasCategoryDir) {
    const QString legacy = gpLayerLegacySubdir(config);
    for (const QString& d : dirs) {
      const bool isCategory =
          config.routingTopDirs.contains(d) || config.routingDirs.contains(d);
      const QString destSub =
          isCategory ? d : (legacy.isEmpty() ? d : legacy + "/" + d);
      out.append({QDir::toNativeSeparators(dir.filePath(d)),
                  gpLayerPath(config, destSub), true});
    }
    for (const QString& name : files) {
      if (!config.gameRoot.isEmpty() && gpIsRootPayload(name) &&
          config.routing.value(name.toLower()).isEmpty()) {
        out.append({QDir::toNativeSeparators(dir.filePath(name)),
                    QDir::toNativeSeparators(config.gameRoot + "/" + name),
                    false});
        continue;
      }
      QString category = config.routing.value(name.toLower());
      if (category.isEmpty()) {
        category = legacy;
      }
      out.append({QDir::toNativeSeparators(dir.filePath(name)),
                  gpLayerPath(config, category, name), false});
    }
    return out;
  }

  if (config.inferNested && files.isEmpty() && dirs.size() == 1) {
    const QDir wrapper(dir.filePath(dirs.first()));
    if (tryLayout(wrapper)) {
      return out;
    }
    const QStringList files2 = gpPayloadFiles(wrapper);
    const QStringList dirs2 =
        wrapper.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (files2.isEmpty() && dirs2.size() == 1) {
      // Two wrapper levels is what real archives usually need; deeper than that
      // is almost always an author mistake, so stop and treat it as loose.
      const QDir inner(wrapper.filePath(dirs2.first()));
      if (tryLayout(inner)) {
        return out;
      }
      gpProjectLoose(inner, config, out);
      return out;
    }
    gpProjectLoose(wrapper, config, out);
    return out;
  }

  gpProjectLoose(dir, config, out);
  return out;
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
