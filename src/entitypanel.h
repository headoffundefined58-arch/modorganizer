#pragma once

/*
 * MO2 core upgrade - generic "vanilla entity" panel.
 *
 * Part of ModOrganizer.exe itself, NOT a plugin. It shows a baseline catalog of
 * the game's original entities (characters, stages, ...) and marks the ones
 * currently overridden by enabled mods. Strictly read-only: it never changes a
 * mod's state, priority or any setting.
 *
 * Instance opt-in (ini next to the instance):
 *   [VFSEntityPanel]
 *   enabled=true
 *   catalog=nxb_entities.json
 *
 * The catalog is UTF-8 JSON:
 *   { "labels": {...}, "columns": [...],
 *     "groups": [ { "id":..., "name":...,
 *                   "entities": [ { "id":..., "name":...,
 *                                   "patterns": ["?knn*"] } ] } ] }
 *
 * Instances without the section never create the tab (zero impact on Skyrim).
 */

#include <QHash>
#include <QJsonObject>
#include <QPair>
#include <QFont>
#include <QPointer>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QLineEdit;
class QPushButton;
class QShowEvent;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class OrganizerCore;

struct EntityModHit
{
  QString modName;
  int fileCount = 0;
  QStringList sampleFiles;
};

struct EntityEntry
{
  QString id;
  QString name;
  QStringList patterns;

  // Wildcard patterns compiled once at load time. QDir::match() recompiles a
  // regular expression on every call, and the naive loop called it once per
  // (entity, file) pair - ~500k compilations per rebuild, seconds of frozen UI
  // every time a mod state changed or the game exited.
  QVector<QRegularExpression> regexes;
  // First three literal characters of the pattern (lower case), used as a
  // bucket key so a file is only tested against plausible entities.
  QString matchKey;
};

struct EntityGroup
{
  QString id;
  QString name;
  QVector<EntityEntry> entries;
};

class EntityPanel : public QWidget
{
public:
  explicit EntityPanel(OrganizerCore* core, QWidget* parent = nullptr);
  ~EntityPanel() override;

  // False when the instance has no [VFSEntityPanel] section or the catalog
  // could not be read; the caller hides the tab in that case.
  bool available() const { return m_available; }

  void refresh();
  void scheduleRefresh();

  // Select (not scroll to) the entities the given mods override. This is the
  // one-way link from the mod list to the panel: picking a mod shows its ninja
  // or stage on the right. Deliberately does NOT scroll, and there is no
  // reverse direction - both used to fight with the mod list's own selection.
  void selectEntitiesForMods(const QStringList& modNames);

protected:
  void showEvent(QShowEvent* event) override;

private:
  bool loadCatalog();
  void buildMatchIndex();
  void setActiveGroup(int index);
  void refreshGroupButtons();
  QFont boldButtonFont() const;
  QFont normalButtonFont() const;
  void positionHeaderSwitch();
  void applyFilter(const QString& text);
  QString label(const QString& key, const QString& fallback) const;

  // Entity ids of the currently selected rows (child rows map to their parent).
  QStringList selectedEntityIds() const;
  void restoreEntitySelection(const QStringList& entityIds);

  // Rows for "which mods touch this entity" are created only when the branch is
  // expanded: one ninja edited by ten mods used to add ten child rows during
  // every rebuild, which made the panel crawl and sometimes crash.
  void populateChildren(QTreeWidgetItem* item);

  // QPointer: an instance switch rebuilds OrganizerCore, which nulls this out
  // instead of leaving a dangling pointer behind.
  QPointer<QObject> m_core;
  QLineEdit* m_filter = nullptr;
  QPushButton* m_refreshButton = nullptr;
  QTreeWidget* m_tree = nullptr;
  QTimer* m_debounce = nullptr;
  // The ninja / stage switcher lives INSIDE the tree header, over the blank
  // name section, to the left of the status header (user requirement).
  QWidget* m_headerSwitch = nullptr;
  QVector<QPushButton*> m_groupButtons;

  // Which catalog group (ninja / stage) the flat list shows. Persisted in the
  // instance ini so the panel reopens where the user left it.
  int m_activeGroup = 0;
  QString m_iniPath;

  bool m_available = false;
  QJsonObject m_labels;
  QStringList m_columns;
  QVector<EntityGroup> m_groups;

  // Flattened (group, entry) positions in catalog order. m_matchIndex buckets
  // them by the pattern's first three characters; m_matchAll holds the ones
  // without a usable literal prefix (patterns starting with a wildcard), which
  // have to be tested for every file.
  QVector<QPair<int, int>> m_flat;
  QHash<QString, QVector<int>> m_matchIndex;
  QVector<int> m_matchAll;

  // entity id -> contributing mods, kept so children can be built lazily
  QHash<QString, QVector<EntityModHit>> m_entityHits;

  // mod name -> first entity it overrides (for mod list -> panel selection)
  QHash<QString, QString> m_modToEntity;

  // True while refresh() rebuilds the tree: item pointers are invalid then.
  bool m_refreshing = false;

  // A rebuild requested while the tab was hidden is deferred until it is shown.
  bool m_dirty = false;
};
