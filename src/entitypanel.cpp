#include "entitypanel.h"

#include <algorithm>

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
#include <QSignalBlocker>
#include <QSize>
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

// Upper bound on how many files a single mod contributes: a mod with 20k files
// is a packaging accident, and scanning it would block the UI for no benefit.
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
  connect(m_tree, &QTreeWidget::itemExpanded, this,
          [this](QTreeWidgetItem* item) { populateChildren(item); });

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
    m_columns = QStringList{QString(), QString::fromUtf8("\xE7\x8A\xB6\xE6\x80\x81")};
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
      header->resizeSection(0, 300);  // entity name
    }
    if (m_columns.size() > 1) {
      header->resizeSection(1, 120);  // status
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
      for (const QString& pattern : entry.patterns) {
        QRegularExpression re(QRegularExpression::wildcardToRegularExpression(pattern));
        re.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
        entry.regexes.append(re);
      }
      // Bucket key: the literal head of the first pattern. "5tyvbod1*" -> "5ty";
      // a pattern that starts with a wildcard gets no key and is always tested.
      entry.matchKey.clear();
      for (const QString& pattern : entry.patterns) {
        QString literal;
        for (const QChar c : pattern) {
          if (c == QLatin1Char('*') || c == QLatin1Char('?') ||
              c == QLatin1Char('[')) {
            break;
          }
          literal.append(c);
        }
        if (literal.size() >= 3) {
          entry.matchKey = literal.left(3).toLower();
          break;
        }
      }

      if (!entry.id.isEmpty() && !entry.name.isEmpty()) {
        group.entries.append(entry);
      }
    }
    if (!group.entries.isEmpty() && !group.id.isEmpty()) {
      m_groups.append(group);
    }
  }
  if (m_groups.isEmpty()) {
    return false;
  }
  buildMatchIndex();
  return true;
}

void EntityPanel::buildMatchIndex()
{
  m_flat.clear();
  m_matchIndex.clear();
  m_matchAll.clear();

  int flat = 0;
  for (int g = 0; g < m_groups.size(); ++g) {
    const EntityGroup& group = m_groups.at(g);
    for (int e = 0; e < group.entries.size(); ++e, ++flat) {
      m_flat.append(qMakePair(g, e));
      const EntityEntry& entry = group.entries.at(e);
      if (entry.matchKey.isEmpty()) {
        m_matchAll.append(flat);
      } else {
        m_matchIndex[entry.matchKey].append(flat);
      }
    }
  }
}

void EntityPanel::showEvent(QShowEvent* event)
{
  QWidget::showEvent(event);
  if (m_dirty) {
    m_dirty = false;
    refresh();
  }
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
  if (!isVisible()) {
    // After the game exits MO2 refreshes the whole directory structure and the
    // panel is not even on screen; rebuilding it then is pure waste. Rebuild
    // once when the tab is actually shown (showEvent).
    m_dirty = true;
    return;
  }
  if (m_refreshing) {
    // m_refreshing: a mod state change can schedule a refresh while one is
    // still running (the mod list emits once per toggled mod). Rebuilding the
    // tree from inside a rebuild would delete items that are still being
    // walked, so the nested call is simply dropped.
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

  // One pass over the files, not one pass per entity. The previous version ran
  // QDir::match() for every (entity, file) pair, and QDir::match compiles a
  // wildcard regex on each call: 468 entities x ~1000 files meant roughly half
  // a million regex compilations per rebuild. That is what made the window
  // freeze for seconds after the game exited (MO2 refreshes the whole tree
  // then). Now the patterns are compiled once at load time and a file is only
  // tested against the entities sharing its first three characters.
  QVector<QHash<int, int>> counts(m_flat.size());
  QVector<QHash<int, QStringList>> samples(m_flat.size());

  for (int mi = 0; mi < perMod.size(); ++mi) {
    const ModFiles& mod = perMod.at(mi);
    for (const QString& fileName : mod.files) {
      const QString key = fileName.left(3).toLower();

      auto test = [&](int flat) {
        const QPair<int, int>& ref = m_flat.at(flat);
        const EntityEntry& entity = m_groups.at(ref.first).entries.at(ref.second);
        for (const QRegularExpression& re : entity.regexes) {
          if (re.match(fileName).hasMatch()) {
            ++counts[flat][mi];
            QStringList& list = samples[flat][mi];
            if (list.size() < 3) {
              list.append(fileName);
            }
            return;
          }
        }
      };

      for (const int flat : m_matchAll) {
        test(flat);
      }
      const auto bucket = m_matchIndex.constFind(key);
      if (bucket != m_matchIndex.constEnd()) {
        for (const int flat : bucket.value()) {
          test(flat);
        }
      }
    }
  }

  QHash<QString, QVector<EntityModHit>> hits;
  m_entityHits.clear();
  m_modToEntity.clear();
  int flat = 0;
  for (const EntityGroup& group : m_groups) {
    for (const EntityEntry& entity : group.entries) {
      QVector<EntityModHit> contributions;
      QList<int> modIndexes = counts.at(flat).keys();
      std::sort(modIndexes.begin(), modIndexes.end());  // stable child order
      for (const int mi : modIndexes) {
        contributions.append(EntityModHit{perMod.at(mi).name,
                                          counts.at(flat).value(mi),
                                          samples.at(flat).value(mi)});
      }
      if (!contributions.isEmpty()) {
        hits.insert(entity.id, contributions);
        m_entityHits.insert(entity.id, contributions);
        for (const EntityModHit& hit : contributions) {
          if (!m_modToEntity.contains(hit.modName)) {
            m_modToEntity.insert(hit.modName, entity.id);
          }
        }
      }
      ++flat;
    }
  }

  const int sortColumn = m_tree->header() != nullptr ? m_tree->sortColumn() : 0;
  const Qt::SortOrder sortOrder =
      m_tree->header() != nullptr ? m_tree->header()->sortIndicatorOrder()
                                  : Qt::AscendingOrder;

  // Keep the user's selection across the rebuild: enabling a mod must not swap
  // the selection (and, through the panel -> mod list hop, the mod list's
  // selection) out from under them.
  const QStringList keepSelected = selectedEntityIds();

  m_refreshing = true;
  {
    // Blocking the tree removes the itemSelectionChanged burst that the
    // rebuild would otherwise emit while the items are being deleted.
    const QSignalBlocker blockTree(m_tree);

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
        // 忍者 / 舞台 are independent blocks, not ordinary rows: span the header
        // across the columns, tint it and give it height so the two sections
        // read as separate groups at a glance.
        groupItem->setText(0, QString("%1   %2 / %3")
                                  .arg(group.name)
                                  .arg(groupModified)
                                  .arg(group.entries.size()));
        groupItem->setFirstColumnSpanned(true);
        groupItem->setBackground(0, QBrush(QColor(0xE8, 0xEE, 0xF7)));
        groupItem->setSizeHint(0, QSize(0, 30));
      }
    }

    m_tree->setSortingEnabled(true);
    m_tree->sortItems(sortColumn, sortOrder);
    applyFilter(m_filter->text());
    restoreEntitySelection(keepSelected);
  }
  m_refreshing = false;
}

void EntityPanel::selectEntitiesForMods(const QStringList& modNames)
{
  if (!m_available || m_refreshing || m_tree == nullptr || modNames.isEmpty()) {
    return;
  }
  if (!isVisible()) {
    // Nothing on screen to select; when the tab is shown the tree is rebuilt
    // anyway and the user's next click re-triggers this.
    return;
  }

  QStringList wanted;
  for (const QString& modName : modNames) {
    const QString entityId = m_modToEntity.value(modName);
    if (!entityId.isEmpty() && !wanted.contains(entityId)) {
      wanted.append(entityId);
    }
  }
  if (wanted.isEmpty()) {
    // The selected mod overrides nothing in the catalog: leave the panel as it
    // is instead of clearing it, so browsing unmodded mods is not jarring.
    return;
  }

  // Blocked: changing the selection programmatically must not look like a user
  // click (the panel no longer drives the mod list, but the itemSelectionChanged
  // burst below would still cost time for no reason).
  const QSignalBlocker blocker(m_tree);
  m_tree->clearSelection();
  QTreeWidgetItem* first = nullptr;
  for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
    QTreeWidgetItem* group = m_tree->topLevelItem(g);
    for (int e = 0; e < group->childCount(); ++e) {
      QTreeWidgetItem* item = group->child(e);
      if (!wanted.contains(item->data(0, kRoleEntityId).toString())) {
        continue;
      }
      group->setExpanded(true);
      item->setSelected(true);
      if (first == nullptr) {
        first = item;
      }
    }
  }
  if (first != nullptr) {
    // setCurrentItem only; no scrollToItem - the jump is what was removed.
    m_tree->setCurrentItem(first);
  }
}

void EntityPanel::populateChildren(QTreeWidgetItem* item)
{
  if (item == nullptr || item->childCount() > 0) {
    return;
  }
  const QString entityId = item->data(0, kRoleEntityId).toString();
  if (entityId.isEmpty()) {
    return;
  }
  const QVector<EntityModHit> contributions = m_entityHits.value(entityId);
  if (contributions.size() < 2) {
    return;
  }
  const QSignalBlocker blocker(m_tree);
  for (const EntityModHit& hit : contributions) {
    auto* child = new EntityItem(item);
    child->setText(0, hit.modName);
    child->setText(1, QString("%1: %2")
                          .arg(label("files", QString::fromUtf8("\xE5\x8C\xB9\xE9\x85\x8D\xE6\x96\x87\xE4\xBB\xB6")))
                          .arg(hit.fileCount));
    child->setData(0, kRoleMod, hit.modName);
    child->setToolTip(0, hit.sampleFiles.join("\n"));
  }
}

QStringList EntityPanel::selectedEntityIds() const
{
  QStringList ids;
  if (m_tree == nullptr) {
    return ids;
  }
  for (QTreeWidgetItem* item : m_tree->selectedItems()) {
    QTreeWidgetItem* row = (item->parent() != nullptr) ? item->parent() : item;
    const QString id = row->data(0, kRoleEntityId).toString();
    if (!id.isEmpty() && !ids.contains(id)) {
      ids.append(id);
    }
  }
  return ids;
}

void EntityPanel::restoreEntitySelection(const QStringList& entityIds)
{
  if (m_tree == nullptr || entityIds.isEmpty()) {
    return;
  }
  QTreeWidgetItem* firstItem = nullptr;
  for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
    QTreeWidgetItem* group = m_tree->topLevelItem(g);
    for (int e = 0; e < group->childCount(); ++e) {
      QTreeWidgetItem* item = group->child(e);
      if (!entityIds.contains(item->data(0, kRoleEntityId).toString())) {
        continue;
      }
      group->setExpanded(true);
      item->setSelected(true);
      if (firstItem == nullptr) {
        firstItem = item;
      }
    }
  }
  if (firstItem != nullptr) {
    m_tree->setCurrentItem(firstItem);
  }
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
                         item->text(1).contains(needle, Qt::CaseInsensitive);
      item->setHidden(!match);
      if (match) {
        ++visible;
      }
    }
    group->setHidden(visible == 0 && !needle.isEmpty());
  }
}
