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

  // Keep Root Builder packages working (they are still valid mods here),
  // while the core itself maps data_win32/ and loose files natively.
  if (!ini.contains("VFSRootMappings/size")) {
    ini.beginWriteArray("VFSRootMappings");
    ini.setArrayIndex(0);
    ini.setValue("source", "Root");
    ini.setValue("destination", "${GAME_ROOT}");
    ini.endArray();
    changed = true;
  }

  // Loose community packages (top-level *.xfbin) belong where the game's
  // mod loader reads them; structured data_win32/ mods keep the native map.
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
