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
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <functional>

class QLineEdit;
class QPushButton;
class QTimer;
class QTreeWidget;
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

  // Scroll to / select the first entity touched by `modName` (mod list -> panel).
  void focusEntityForMod(const QString& modName);

  // Called when the user selects an entity, with the mod that owns it (may be
  // empty for a pristine entry). Used to drive the mod list (panel -> mod list).
  void setModActivatedHandler(std::function<void(const QString&)> handler);

private:
  bool loadCatalog();
  void applyFilter(const QString& text);
  QString label(const QString& key, const QString& fallback) const;
  void currentEntityChanged();

  // QPointer: an instance switch rebuilds OrganizerCore, which nulls this out
  // instead of leaving a dangling pointer behind.
  QPointer<QObject> m_core;
  QLineEdit* m_filter = nullptr;
  QPushButton* m_refreshButton = nullptr;
  QTreeWidget* m_tree = nullptr;
  QTimer* m_debounce = nullptr;

  bool m_available = false;
  QJsonObject m_labels;
  QStringList m_columns;
  QVector<EntityGroup> m_groups;

  // entity id -> mods currently overriding it; entity id -> first mod (for
  // mod-list -> panel scrolling)
  QHash<QString, QStringList> m_entityMods;
  QHash<QString, QString> m_modToEntity;
  std::function<void(const QString&)> m_onModActivated;
};
