#pragma once

/*
 * MO2 core upgrade - generic "vanilla entity" panel.
 *
 * This is part of ModOrganizer.exe itself, NOT a plugin. It shows a baseline
 * catalog of the game's original entities (characters, locations, ...) and
 * marks the ones currently overridden by enabled mods. It is strictly
 * read-only: it never changes a mod's state, priority or any setting.
 *
 * Everything is data-driven. An instance opts in with:
 *
 *   [VFSEntityPanel]
 *   enabled=true
 *   catalog=nxb_entities.json
 *
 * The catalog is a UTF-8 JSON file next to that instance's ini:
 *   { "labels": {...}, "columns": [...],
 *     "groups": [ { "id":..., "name":...,
 *                   "entities": [ { "id":..., "name":...,
 *                                   "patterns": ["?knn*"] } ] } ] }
 *
 * Instances without the section never create the tab (zero impact on Skyrim
 * and every other game). Patterns are QDir glob patterns matched against the
 * file names inside each enabled mod folder.
 */

#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QLabel;
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
  // could not be read; the caller removes the tab in that case.
  bool available() const { return m_available; }

  void refresh();
  void scheduleRefresh();

private:
  bool loadCatalog();
  void applyFilter(const QString& text);
  QString label(const QString& key, const QString& fallback) const;

  // QPointer: an instance switch rebuilds OrganizerCore, which nulls this
  // out instead of leaving a dangling pointer behind.
  QPointer<QObject> m_core;
  QLineEdit* m_filter = nullptr;
  QPushButton* m_refreshButton = nullptr;
  QLabel* m_summary = nullptr;
  QTreeWidget* m_tree = nullptr;
  QTimer* m_debounce = nullptr;

  bool m_available = false;
  QJsonObject m_labels;
  QStringList m_columns;
  QVector<EntityGroup> m_groups;
};
