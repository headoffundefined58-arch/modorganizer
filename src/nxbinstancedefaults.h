#pragma once

/*
 * MO2 core upgrade - self-healing instance defaults for games whose data root
 * IS the game root (NARUTO X BORUTO Ultimate Ninja STORM CONNECTIONS).
 *
 * A stock MO2 "create instance" wizard knows nothing about such a game: it
 * writes no VFS configuration at all, so every mod falls back to Bethesda
 * assumptions and the save folder is never mapped into the profile.  Worse,
 * the wizard happily lets the user put the instance's base directory INSIDE
 * the game folder; mods/ then lives under the virtual data root and USVFS has
 * to map a child onto its own parent.
 *
 * This header makes the core repair itself:
 *
 *   - setManagedGame() calls nxbEnsureInstanceDefaults() on EVERY start, so a
 *     missing/blank section is refilled.  Users can still override any value;
 *     only empty keys are touched.  Bethesda instances are never matched and
 *     therefore never modified.
 *   - CreateInstanceDialog::finish() calls it right after writing a new ini,
 *     so a freshly created NXB instance is correct from the first launch.
 *
 * Keep the values in sync with config/nxb-instance.ini.example and
 * tools/finalize_nxb_instance.py.
 */

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QString>
#include <QVariant>
#include <QtGlobal>

#include "iplugingame.h"

// Matches by game name first, then by the game's own files (so a renamed
// plugin still works).  Bethesda games never match either test.
inline bool nxbIsRootDataGame(MOBase::IPluginGame* game, const QDir& gameRoot)
{
  if (game == nullptr) {
    return false;
  }
  if (game->gameName().contains("NARUTO X BORUTO", Qt::CaseInsensitive)) {
    return true;
  }
  return QFileInfo::exists(gameRoot.filePath("NSUNSC.exe")) &&
         QDir(gameRoot.filePath("data/launch")).exists();
}

// Returns true when at least one key was (re)written.
inline bool nxbEnsureInstanceDefaults(const QString& iniPath,
                                      MOBase::IPluginGame* game,
                                      const QDir& gameRoot)
{
  if (iniPath.isEmpty() || !nxbIsRootDataGame(game, gameRoot)) {
    return false;
  }

  QSettings ini(iniPath, QSettings::IniFormat);
  if (ini.status() != QSettings::NoError) {
    return false;
  }

  bool changed = false;
  auto setIfEmpty = [&](const QString& key, const QVariant& value) {
    if (!ini.contains(key) || ini.value(key).toString().trimmed().isEmpty()) {
      ini.setValue(key, value);
      changed = true;
    }
  };
  auto setIfMissing = [&](const QString& key, const QVariant& value) {
    if (!ini.contains(key)) {
      ini.setValue(key, value);
      changed = true;
    }
  };

  // The virtual Data root is the game root, not a "Data" subfolder.
  setIfEmpty("VFSCore/data_root_override", "${GAME_ROOT}");

  // Root/ packages are handled by the projection itself (Root/data_win32 and
  // Root/data go to the layer, Root/*.dll and friends go to the game root), so
  // a blanket Root -> game root mapping would map every such mod twice.
  if (!ini.contains("VFSRootMappings/size")) {
    ini.beginWriteArray("VFSRootMappings");
    ini.endArray();
    changed = true;
  }

  // Migration: build 8500d21 briefly pointed the layer at <game>/data, based on
  // the wrong assumption that the engine reads loose files there. Empirically it
  // does not - data/ has lower priority than the CPKs, so mods mapped there
  // never apply (the user played fine with the data_win32 layer and lost mods
  // the moment it switched to data). Heal instances back to data_win32.
  {
    const QString layerKey = "VFSContentLayout/layer";
    const QString bareKey  = "VFSContentLayout/bare_destination";

    const QString layer = ini.value(layerKey).toString().trimmed();
    const bool pointsAtPlainData =
        layer.endsWith("/data", Qt::CaseInsensitive) &&
        !layer.endsWith("data_win32", Qt::CaseInsensitive);
    if (pointsAtPlainData) {
      ini.setValue(layerKey, "${GAME_ROOT}/data_win32");
      changed = true;
    }
    const QString bare = ini.value(bareKey).toString().trimmed();
    if (bare.endsWith("/data/spc", Qt::CaseInsensitive)) {
      ini.setValue(bareKey, QString(bare).replace(
          QString("/data/spc"), QString("/data_win32/spc")));
      changed = true;
    }
  }

  // Loose community packages (top-level *.xfbin) belong where the game's
  // mod loader reads them; structured data/ mods keep the native map.
  // Virtual overlay layer first: without these three keys the instance falls
  // back to the old "bare mods go to spc" behaviour, which is what made the
  // settings dialog show only bare_destination.
  setIfEmpty("VFSContentLayout/layer", "${GAME_ROOT}/data_win32");
  setIfEmpty("VFSContentLayout/routing", "nxb_win32_routing.json");
  setIfEmpty("VFSContentLayout/infer_nested", "true");
  // Materialize the layer with hard links as well: the game then sees mod
  // files even if USVFS injection were unavailable, and direct launches work
  // too. Links are purged via the manifest on game exit / MO2 start.
  setIfEmpty("VFSContentLayout/deploy", "hardlink");

  // Bare destination stays as the fallback for assets the routing table does
  // not know (old mods), never as the primary mechanism.
  setIfEmpty("VFSContentLayout/bare_destination",
             "${GAME_ROOT}/data_win32/spc");
  setIfEmpty("VFSContentLayout/bare_patterns", "*.xfbin;*.dds;*.png;*.tex");
  setIfEmpty("VFSContentLayout/layout_markers",
             "data_win32;data;moddingapi;Root;engine");

  // Saves: ask the game plugin, never hardcode a RUNE/Steam path in the core.
  const QString saves = game->savesDirectory().absolutePath();
  if (!saves.isEmpty()) {
    setIfEmpty("VFSGameProfile/save_source", saves);
  }
  setIfEmpty("VFSGameProfile/save_pattern", "stormc.s");
  setIfMissing("VFSGameProfile/save_per_profile", true);
  setIfMissing("VFSGameProfile/seed_existing_saves", true);

  // The generic Bethesda path rows have no meaning for this game.
  setIfEmpty("VFSUIPaths/hidden_fields",
             "downloads;mods;cache;profiles;overwrite;baseDirHint;writableHint");

  // Content panel catalog (plain JSON next to the ini).
  setIfMissing("VFSEntityPanel/enabled", true);
  setIfEmpty("VFSEntityPanel/catalog", "nxb_entities.json");

  // Per-profile saves must be on for LocalSavegames to map <profile>/saves.
  setIfMissing("Settings/profile_local_saves", true);

  if (changed) {
    ini.sync();
  }
  return changed;
}

// A new instance must never put mods/ inside the game folder for a root-data
// game.  Returns the corrected base directory (unchanged for other games).
// Where an instance's heavy data (mods/, downloads/, profiles/, overwrite/)
// should live. The wizard default is AppData, i.e. the system drive, which is
// the one place a modded setup must avoid: thousands of small files there are
// slow to enumerate and fill up the profile disk. Preference: the drive MO2
// itself lives on, then the game's drive; an empty result means "keep the
// caller's default" (only when both are on the system drive).
inline QString nxbPreferredBaseDirectory(const QDir& gameRoot,
                                         const QString& instanceName)
{
  const QString name =
      instanceName.trimmed().isEmpty() ? QStringLiteral("instance")
                                       : instanceName.trimmed();
  const QString systemDrive =
      qEnvironmentVariable("SystemDrive", "C:").toUpper();

  auto usableDrive = [&](const QString& path) {
    const QString native = QDir::toNativeSeparators(path);
    if (native.size() < 2 || native.at(1) != QChar(':')) {
      return false;
    }
    return native.left(2).toUpper() != systemDrive;
  };

  const QString appDir = QCoreApplication::applicationDirPath();
  if (usableDrive(appDir)) {
    return QDir::toNativeSeparators(
        QDir(appDir).filePath("instances/" + name));
  }

  const QString gameDir = gameRoot.absolutePath();
  if (usableDrive(gameDir)) {
    return QDir::toNativeSeparators(
        QDir(gameDir.left(2) + "/MO2-Instances").filePath(name));
  }

  return {};
}

inline QString nxbSafeBaseDirectory(MOBase::IPluginGame* game,
                                    const QDir& gameRoot,
                                    const QString& requestedBase,
                                    const QString& fallbackBase)
{
  if (!nxbIsRootDataGame(game, gameRoot) || requestedBase.isEmpty()) {
    return requestedBase;
  }
  const QString gamePath =
      QDir::cleanPath(QDir::toNativeSeparators(gameRoot.absolutePath()));
  const QString base =
      QDir::cleanPath(QDir::toNativeSeparators(QDir(requestedBase).absolutePath()));
  if (base.compare(gamePath, Qt::CaseInsensitive) == 0 ||
      base.startsWith(gamePath + QDir::separator(), Qt::CaseInsensitive)) {
    return fallbackBase;
  }
  return requestedBase;
}
