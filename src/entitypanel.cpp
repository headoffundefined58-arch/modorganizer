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

// Rank stored in Qt::UserRole of the status column: 0 = several mods,
// 1 = one mod, 2 = pristine. Sorting by that column therefore puts the
// modified entries first (ascending) or last (descending).
const int kRankMulti = 0;
const int kRankModified = 1;
const int kRankOriginal = 2;

const int kRoleRank = Qt::UserRole + 1;
const int kRoleMod = Qt::UserRole + 2;
const int kRoleEntityId = Qt::UserRole + 3;

// sortItems() also reorders top-level items. Sorting by status compared the
// group headers' "0 / 109" against "16 / 359" as plain text, which pushed the
// stage group above the ninja group. GroupItem ignores the sort column and
// keeps the catalog's group order.
const int kRoleGroupOrder = Qt::UserRole + 4;

class GroupItem : public QTreeWidgetItem
{
public:
  explicit GroupItem(QTreeWidget* parent, int order) : QTreeWidgetItem(parent)
  {
    setData(0, kRoleGroupOrder, order);
  }

  bool operator<(const QTreeWidgetItem& other) const override
  {
    const QVariant mine   = data(0, kRoleGroupOrder);
    const QVariant theirs = other.data(0, kRoleGroupOrder);
    if (mine.isValid() && theirs.isValid()) {
      // QTreeWidget implements descending order by evaluating "other < this",
      // which would flip the fixed group order whenever the user clicks a
      // header twice. Compensate so both directions keep the catalog order.
      QTreeWidget* tree = treeWidget();
      const bool descending =
          tree != nullptr && tree->header() != nullptr &&
          tree->header()->sortIndicatorOrder() == Qt::DescendingOrder;
      return descending ? mine.toInt() > theirs.toInt()
                        : mine.toInt() < theirs.toInt();
    }
    return QTreeWidgetItem::operator<(other);
  }
};

// Lets the status column sort by rank instead of by its localized text.
class EntityItem : public QTreeWidgetItem
{
public:
  explicit EntityItem(QTreeWidget* parent) : QTreeWidgetItem(parent) {}
  explicit EntityItem(QTreeWidgetItem* parent) : QTreeWidgetItem(parent) {}

  bool operator<(const QTreeWidgetItem& other) const override
  {
    QTreeWidget* tree = treeWidget();
    const int column  = tree != nullptr ? tree->sortColumn() : 0;
    const QVariant mine = data(column, kRoleRank);
    const QVariant theirs = other.data(column, kRoleRank);
    if (mine.isValid() && theirs.isValid()) {
      return mine.toInt() < theirs.toInt();
    }
    return QTreeWidgetItem::operator<(other);
  }
};
}  // namespace

EntityPanel::EntityPanel(OrganizerCore* core, QWidget* parent)
    : QWidget(parent), m_core(core)
{
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(2);

  m_tree = new QTreeWidget(this);
  m_tree->setAlternatingRowColors(true);
  m_tree->setUniformRowHeights(true);
  m_tree->setRootIsDecorated(true);
  m_tree->setSortingEnabled(true);
  // Ctrl / Shift click and rubber-band selection: selecting several entities
  // must be able to activate several mods at once.
  m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
  root->addWidget(m_tree, 1);

  // Filter lives at the bottom right, mirroring MO2's own filter bars.
  auto* bottom = new QHBoxLayout();
  bottom->setContentsMargins(0, 0, 0, 0);
  bottom->setSpacing(4);
  bottom->addStretch(1);
  m_filter = new QLineEdit(this);
  m_filter->setClearButtonEnabled(true);
  m_filter->setMinimumWidth(160);
  m_filter->setMaximumWidth(260);
  m_refreshButton = new QPushButton(this);
  bottom->addWidget(m_filter);
  bottom->addWidget(m_refreshButton);
  root->addLayout(bottom);

  m_debounce = new QTimer(this);
  m_debounce->setSingleShot(true);
  m_debounce->setInterval(250);

  connect(m_debounce, &QTimer::timeout, this, [this]() { refresh(); });
  connect(m_refreshButton, &QPushButton::clicked, this, [this]() { refresh(); });
  connect(m_filter, &QLineEdit::textChanged, this,
          [this](const QString& text) { applyFilter(text); });
  connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
          [this]() { currentEntityChanged(); });

  m_available = loadCatalog();
  if (!m_available) {
    return;
  }

  // Fallbacks are ASCII-only UTF-8 byte escapes: MSVC has no /utf-8 flag,
  // so a raw CJK literal would depend on the runner code page. The JSON
  // catalog normally supplies these Simplified Chinese strings anyway.
  m_filter->setPlaceholderText(label("filter", QString::fromUtf8("\xE7\xAD\x9B\xE9\x80\x89\xE5\xBF\x8D\xE8\x80\x85\xE6\x88\x96\xE8\x88\x9E\xE5\x8F\xB0\xE2\x80\xA6")));
  m_refreshButton->setText(label("refresh", QString::fromUtf8("\xE5\x88\xB7\xE6\x96\xB0")));
  if (m_columns.isEmpty()) {
    m_columns = QStringList{QString::fromUtf8("\xE5\x90\x8D\xE7\xA7\xB0"), QString::fromUtf8("\xE7\x8A\xB6\xE6\x80\x81"),
                        QString::fromUtf8("\xE8\xB5\x84\xE6\xBA\x90\xE4\xBB\xA3\xE7\xA0\x81")};
  }
  m_tree->setColumnCount(m_columns.size());
  m_tree->setHeaderLabels(m_columns);
  if (QHeaderView* header = m_tree->header()) {
    // QHeaderView::Stretch makes a section impossible for the user to drag.
    // With column 0 stretched, only the last column had a draggable edge,
    // and long official Traditional Chinese names were cut off.  Make every
    // column interactive and give them sensible starting widths.
    for (int i = 0; i < m_columns.size(); ++i) {
      header->setSectionResizeMode(i, QHeaderView::Interactive);
    }
    header->setMinimumSectionSize(60);
    if (m_columns.size() > 0) {
      header->resizeSection(0, 280);  // name
    }
    if (m_columns.size() > 1) {
      header->resizeSection(1, 110);  // status
    }
    if (m_columns.size() > 2) {
      header->resizeSection(2, 180);  // resource code
    }
    header->setSectionsMovable(true);
    header->setSortIndicator(0, Qt::AscendingOrder);
  }
  refresh();
}

EntityPanel::~EntityPanel()
{
  if (m_debounce != nullptr) {
    m_debounce->stop();
  }
}

void EntityPanel::setModActivatedHandler(std::function<void(const QStringList&)> handler)
{
  m_onModActivated = std::move(handler);
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
  OrganizerCore* core = qobject_cast<OrganizerCore*>(m_core.data());
  if (!m_available || core == nullptr) {
    return;
  }

  // MO2 writes the enabled state in <profile>/modlist.txt as "+Name" / "-Name".
  // Reading it keeps this panel independent of the model internals and never
  // touches the mod list itself.
  QStringList enabledMods;
  if (const Profile* profile = core->currentProfile()) {
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

  const QString modsPath = core->settings().paths().mods();
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
  m_entityMods.clear();
  m_modToEntity.clear();
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
        hits.insert(entity.id, contributions);
        QStringList names;
        for (const EntityModHit& hit : contributions) {
          names.append(hit.modName);
          if (!m_modToEntity.contains(hit.modName)) {
            m_modToEntity.insert(hit.modName, entity.id);
          }
        }
        m_entityMods.insert(entity.id, names);
      }
    }
  }

  const int sortColumn = m_tree->header() != nullptr ? m_tree->sortColumn() : 0;
  const Qt::SortOrder sortOrder =
      m_tree->header() != nullptr ? m_tree->header()->sortIndicatorOrder()
                                  : Qt::AscendingOrder;
  m_tree->setSortingEnabled(false);
  m_tree->clear();

  int groupOrder = 0;
  for (const EntityGroup& group : m_groups) {
    auto* groupItem = new GroupItem(m_tree, groupOrder++);
    groupItem->setText(0, group.name);
    QFont groupFont = groupItem->font(0);
    groupFont.setBold(true);
    groupItem->setFont(0, groupFont);
    groupItem->setExpanded(true);

    int groupModified = 0;
    for (const EntityEntry& entity : group.entries) {
      auto* item = new EntityItem(groupItem);
      item->setText(0, entity.name);
      item->setText(2, entity.id);
      item->setData(0, kRoleEntityId, entity.id);

      const QVector<EntityModHit> contributions = hits.value(entity.id);
      if (contributions.isEmpty()) {
        item->setText(1, label("original", QString::fromUtf8("\xE5\x8E\x9F\xE7\x89\x88")));
        item->setData(1, kRoleRank, kRankOriginal);
        continue;
      }

      ++groupModified;
      if (contributions.size() == 1) {
        item->setText(1, label("modified", QString::fromUtf8("\xE5\xB7\xB2\xE4\xBF\xAE\xE6\x94\xB9")));
        item->setData(1, kRoleRank, kRankModified);
        item->setForeground(1, QBrush(QColor(255, 183, 77)));
      } else {
        item->setText(1, QString("%1 (%2)")
                             .arg(label("multi", QString::fromUtf8("\xE5\xA4\x9A\xE4\xB8\xAA\x20\x4D\x6F\x64")))
                             .arg(contributions.size()));
        item->setData(1, kRoleRank, kRankMulti);
        item->setForeground(1, QBrush(QColor(239, 83, 80)));
        QFont statusFont = item->font(1);
        statusFont.setBold(true);
        item->setFont(1, statusFont);
      }
      item->setData(0, kRoleMod, contributions.first().modName);

      // One child per contributing mod so multi-mod overrides stay readable.
      for (const EntityModHit& hit : contributions) {
        auto* child = new EntityItem(item);
        child->setText(0, hit.modName);
        child->setText(1, label("fromMod", QString::fromUtf8("\xE6\x9D\xA5\xE8\x87\xAA")));
        child->setText(2, QString("%1: %2")
                              .arg(label("files", QString::fromUtf8("\xE5\x8C\xB9\xE9\x85\x8D\xE6\x96\x87\xE4\xBB\xB6")))
                              .arg(hit.fileCount));
        child->setData(0, kRoleMod, hit.modName);
        child->setToolTip(2, hit.sampleFiles.join("\n"));
      }
    }
    if (m_tree->columnCount() > 1) {
      groupItem->setText(1, QString("%1 / %2")
                                .arg(groupModified)
                                .arg(group.entries.size()));
    }
  }

  m_tree->setSortingEnabled(true);
  m_tree->sortItems(sortColumn, sortOrder);
  applyFilter(m_filter->text());
}

void EntityPanel::focusEntityForMod(const QString& modName)
{
  focusEntitiesForMods(QStringList{modName});
}

void EntityPanel::focusEntitiesForMods(const QStringList& modNames)
{
  if (!m_available || m_selectionSync || modNames.isEmpty()) {
    return;
  }

  // m_modToEntity holds the first entity each mod contributes to, so one mod
  // maps to one entity. De-duplicate so N selected mods select N entities.
  QStringList wanted;
  for (const QString& modName : modNames) {
    const QString entityId = m_modToEntity.value(modName);
    if (!entityId.isEmpty() && !wanted.contains(entityId)) {
      wanted.append(entityId);
    }
  }
  if (wanted.isEmpty()) {
    return;
  }

  // Changing the tree selection emits itemSelectionChanged; the guard stops
  // that from driving the mod list straight back into this function.
  m_selectionSync = true;
  m_tree->clearSelection();
  QTreeWidgetItem* firstItem = nullptr;
  for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
    QTreeWidgetItem* group = m_tree->topLevelItem(g);
    for (int e = 0; e < group->childCount(); ++e) {
      QTreeWidgetItem* item = group->child(e);
      if (!wanted.contains(item->data(0, kRoleEntityId).toString())) {
        continue;
      }
      group->setExpanded(true);
      item->setHidden(false);
      item->setSelected(true);
      if (firstItem == nullptr) {
        firstItem = item;
      }
    }
  }
  if (firstItem != nullptr) {
    m_tree->setCurrentItem(firstItem);
    m_tree->scrollToItem(firstItem, QAbstractItemView::PositionAtCenter);
  }
  m_selectionSync = false;
}

void EntityPanel::currentEntityChanged()
{
  if (!m_available || m_selectionSync || !m_onModActivated) {
    return;
  }

  // Gather every selected entity row (and expanded per-mod child rows), then
  // hand the de-duplicated mod batch to the mod list.
  QStringList mods;
  for (QTreeWidgetItem* item : m_tree->selectedItems()) {
    const QString mod = item->data(0, kRoleMod).toString();
    if (!mod.isEmpty() && !mods.contains(mod)) {
      mods.append(mod);
    }
  }
  if (mods.isEmpty()) {
    return;
  }

  m_selectionSync = true;
  m_onModActivated(mods);
  m_selectionSync = false;
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
      const bool match = needle.isEmpty() ||
                         item->text(0).contains(needle, Qt::CaseInsensitive) ||
                         item->text(1).contains(needle, Qt::CaseInsensitive) ||
                         item->text(2).contains(needle, Qt::CaseInsensitive);
      item->setHidden(!match);
      if (match) {
        ++visible;
      }
    }
    group->setHidden(visible == 0 && !needle.isEmpty());
  }
}
