#include "entitypanel.h"

#include "gameprofileconfig.h"
#include "organizercore.h"
#include "profile.h"
#include "settings.h"

#include <QBrush>
#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace
{
struct ModFiles
{
  QString name;
  QStringList files;
};

const int kMaxFilesPerMod = 20000;
}  // namespace

EntityPanel::EntityPanel(OrganizerCore* core, QWidget* parent)
    : QWidget(parent), m_core(core)
{
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(6, 6, 6, 0);
  root->setSpacing(4);

  auto* bar = new QHBoxLayout();
  bar->setSpacing(4);
  m_filter = new QLineEdit(this);
  m_filter->setClearButtonEnabled(true);
  m_refreshButton = new QPushButton(this);
  bar->addWidget(m_filter, 1);
  bar->addWidget(m_refreshButton);
  root->addLayout(bar);

  m_summary = new QLabel(this);
  root->addWidget(m_summary);

  m_tree = new QTreeWidget(this);
  m_tree->setAlternatingRowColors(true);
  m_tree->setUniformRowHeights(true);
  m_tree->setRootIsDecorated(true);
  m_tree->setSortingEnabled(false);
  root->addWidget(m_tree, 1);

  m_debounce = new QTimer(this);
  m_debounce->setSingleShot(true);
  m_debounce->setInterval(250);

  connect(m_debounce, &QTimer::timeout, this, [this]() { refresh(); });
  connect(m_refreshButton, &QPushButton::clicked, this, [this]() { refresh(); });
  connect(m_filter, &QLineEdit::textChanged, this,
          [this](const QString& text) { applyFilter(text); });

  m_available = loadCatalog();
  if (!m_available) {
    return;
  }

  m_filter->setPlaceholderText(label("filter", "Filter..."));
  m_refreshButton->setText(label("refresh", "Refresh"));
  if (m_columns.isEmpty()) {
    m_columns = QStringList{"Name", "Status", "Mods", "Code"};
  }
  m_tree->setColumnCount(m_columns.size());
  m_tree->setHeaderLabels(m_columns);
  if (QHeaderView* header = m_tree->header()) {
    header->setSectionResizeMode(0, QHeaderView::Stretch);
  }
  refresh();
}

QString EntityPanel::label(const QString& key, const QString& fallback) const
{
  const QString value = m_labels.value(key).toString();
  return value.isEmpty() ? fallback : value;
}

bool EntityPanel::loadCatalog()
{
  const QString iniPath = gameProfileIniPath();
  if (iniPath.isEmpty()) {
    return false;
  }

  QSettings ini(iniPath, QSettings::IniFormat);
  if (!ini.childGroups().contains(QString("VFSEntityPanel"))) {
    return false;
  }
  if (!ini.value("VFSEntityPanel/enabled", false).toBool()) {
    return false;
  }

  QString catalogPath =
      ini.value("VFSEntityPanel/catalog", "nxb_entities.json").toString();
  if (!QDir::isAbsolutePath(catalogPath)) {
    catalogPath = QFileInfo(iniPath).absolutePath() + "/" + catalogPath;
  }

  QFile file(catalogPath);
  if (!file.open(QIODevice::ReadOnly)) {
    return false;
  }
  QJsonParseError error;
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !doc.isObject()) {
    return false;
  }

  const QJsonObject root = doc.object();
  m_labels = root.value("labels").toObject();
  for (const QJsonValue& column : root.value("columns").toArray()) {
    m_columns.append(column.toString());
  }

  for (const QJsonValue& groupValue : root.value("groups").toArray()) {
    const QJsonObject groupObject = groupValue.toObject();
    EntityGroup group;
    group.id   = groupObject.value("id").toString();
    group.name = groupObject.value("name").toString();
    for (const QJsonValue& entityValue : groupObject.value("entities").toArray()) {
      const QJsonObject entityObject = entityValue.toObject();
      EntityEntry entry;
      entry.id   = entityObject.value("id").toString();
      entry.name = entityObject.value("name").toString();
      for (const QJsonValue& pattern : entityObject.value("patterns").toArray()) {
        entry.patterns.append(pattern.toString());
      }
      if (entry.patterns.isEmpty()) {
        entry.patterns.append(entry.id + "*");
      }
      if (!entry.id.isEmpty() && !entry.name.isEmpty()) {
        group.entries.append(entry);
      }
    }
    if (!group.entries.isEmpty() && !group.id.isEmpty()) {
      m_groups.append(group);
    }
  }
  return !m_groups.isEmpty();
}

void EntityPanel::scheduleRefresh()
{
  if (m_available) {
    m_debounce->start();
  }
}

void EntityPanel::refresh()
{
  if (!m_available || m_core == nullptr) {
    return;
  }

  // MO2 writes the enabled state in <profile>/modlist.txt as "+Name" /
  // "-Name". Reading it keeps this panel independent of the model internals
  // and never touches the mod list itself.
  QStringList enabledMods;
  if (const Profile* profile = m_core->currentProfile()) {
    QFile modlist(profile->getModlistFileName());
    if (modlist.open(QIODevice::ReadOnly)) {
      while (!modlist.atEnd()) {
        QString line = QString::fromUtf8(modlist.readLine()).trimmed();
        line.remove(QChar(0xFEFF));
        if (line.startsWith(QLatin1Char('+'))) {
          const QString name = line.mid(1).trimmed();
          if (!name.isEmpty()) {
            enabledMods.append(name);
          }
        }
      }
    }
  }

  const QString modsPath = m_core->settings().paths().mods();
  QVector<ModFiles> perMod;
  for (const QString& modName : enabledMods) {
    const QString modDir = modsPath + "/" + modName;
    if (!QDir(modDir).exists()) {
      continue;
    }
    ModFiles entry;
    entry.name = modName;
    QDirIterator it(modDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && entry.files.size() < kMaxFilesPerMod) {
      it.next();
      entry.files.append(it.fileName());
    }
    perMod.append(entry);
  }

  QHash<QString, QVector<EntityModHit>> hits;
  for (const EntityGroup& group : m_groups) {
    for (const EntityEntry& entity : group.entries) {
      QVector<EntityModHit> contributions;
      for (const ModFiles& mod : perMod) {
        int count = 0;
        QStringList samples;
        for (const QString& fileName : mod.files) {
          bool matched = false;
          for (const QString& pattern : entity.patterns) {
            if (QDir::match(pattern, fileName)) {
              matched = true;
              break;
            }
          }
          if (matched) {
            ++count;
            if (samples.size() < 3) {
              samples.append(fileName);
            }
          }
        }
        if (count > 0) {
          contributions.append(EntityModHit{mod.name, count, samples});
        }
      }
      if (!contributions.isEmpty()) {
        hits.insert(group.id + "/" + entity.id, contributions);
      }
    }
  }

  m_tree->clear();
  int total = 0;
  int modified = 0;

  for (const EntityGroup& group : m_groups) {
    QTreeWidgetItem* groupItem = new QTreeWidgetItem(m_tree);
    groupItem->setText(0, group.name);
    QFont groupFont = groupItem->font(0);
    groupFont.setBold(true);
    groupItem->setFont(0, groupFont);
    groupItem->setExpanded(true);

    int groupModified = 0;
    for (const EntityEntry& entity : group.entries) {
      ++total;
      QTreeWidgetItem* item = new QTreeWidgetItem(groupItem);
      item->setText(0, entity.name);
      item->setText(3, entity.id);

      const QVector<EntityModHit> contributions =
          hits.value(group.id + "/" + entity.id);
      if (contributions.isEmpty()) {
        item->setText(1, label("original", "Original"));
        continue;
      }

      ++modified;
      ++groupModified;
      QStringList names;
      for (const EntityModHit& hit : contributions) {
        names.append(hit.modName);
      }
      item->setText(2, names.join(", "));

      if (contributions.size() == 1) {
        item->setText(1, label("modified", "Modified"));
        item->setForeground(1, QBrush(QColor(255, 183, 77)));
      } else {
        item->setText(1, QString("%1 (%2)")
                             .arg(label("multi", "Multiple mods"))
                             .arg(contributions.size()));
        item->setForeground(1, QBrush(QColor(239, 83, 80)));
        QFont statusFont = item->font(1);
        statusFont.setBold(true);
        item->setFont(1, statusFont);
      }

      // One child per contributing mod: multi-mod overrides stay readable.
      for (const EntityModHit& hit : contributions) {
        QTreeWidgetItem* child = new QTreeWidgetItem(item);
        child->setText(0, hit.modName);
        child->setText(1, label("fromMod", "From"));
        child->setText(2, QString("%1: %2")
                              .arg(label("files", "Files"))
                              .arg(hit.fileCount));
        child->setText(3, hit.sampleFiles.join(", "));
      }
    }
    if (m_tree->columnCount() > 1) {
      groupItem->setText(1, QString("%1 / %2")
                                .arg(groupModified)
                                .arg(group.entries.size()));
    }
  }

  QString summary = label("summary", "{total} entries, {modified} modified");
  summary.replace("{total}", QString::number(total));
  summary.replace("{modified}", QString::number(modified));
  m_summary->setText(summary);

  applyFilter(m_filter->text());
}

void EntityPanel::applyFilter(const QString& text)
{
  if (m_tree == nullptr) {
    return;
  }
  const QString needle = text.trimmed();
  for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
    QTreeWidgetItem* group = m_tree->topLevelItem(g);
    int visible = 0;
    for (int e = 0; e < group->childCount(); ++e) {
      QTreeWidgetItem* item = group->child(e);
      const bool match =
          needle.isEmpty() ||
          item->text(0).contains(needle, Qt::CaseInsensitive) ||
          item->text(2).contains(needle, Qt::CaseInsensitive) ||
          item->text(3).contains(needle, Qt::CaseInsensitive);
      item->setHidden(!match);
      if (match) {
        ++visible;
      }
    }
    group->setHidden(visible == 0 && !needle.isEmpty());
  }
}
