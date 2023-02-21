/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2022 Olive Team

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

***/

#include "project.h"

#include <QDir>
#include <QFileInfo>

#include "common/qtutils.h"
#include "common/xmlutils.h"
#include "core.h"
#include "dialog/progress/progress.h"
#include "node/factory.h"
#include "node/serializeddata.h"
#include "render/diskmanager.h"
#include "window/mainwindow/mainwindow.h"

namespace olive {

#define super QObject

const QString Project::kItemMimeType = QStringLiteral("application/x-oliveprojectitemdata");

Project::Project() :
  is_modified_(false),
  autorecovery_saved_(true)
{
  // Generate UUID for this project
  RegenerateUuid();

  // Folder root for project
  root_ = new Folder();
  root_->setParent(this);
  root_->SetLabel(tr("Root"));
  AddDefaultNode(root_);

  // Adds a color manager "node" to this project so that it synchronizes
  color_manager_ = new ColorManager();
  color_manager_->setParent(this);
  AddDefaultNode(color_manager_);

  // Same with project settings
  settings_ = new ProjectSettingsNode();
  settings_->setParent(this);
  AddDefaultNode(settings_);

  connect(color_manager(), &ColorManager::ValueChanged,
          this, &Project::ColorManagerValueChanged);
}

Project::~Project()
{
  Clear();
}

void Project::Clear()
{
  // By deleting the last nodes first, we assume that nodes that are most important are deleted last
  // (e.g. Project's ColorManager or ProjectSettingsNode.
  for (auto it=node_children_.cbegin(); it!=node_children_.cend(); it++) {
    (*it)->SetCachesEnabled(false);
  }

  while (!node_children_.isEmpty()) {
    delete node_children_.last();
  }
}

void Project::Load(QXmlStreamReader *reader)
{
  SerializedData data;

  while (XMLReadNextStartElement(reader)) {
    if (reader->name() == QStringLiteral("uuid")) {

      this->SetUuid(QUuid::fromString(reader->readElementText()));

    } else if (reader->name() == QStringLiteral("nodes")) {

      while (XMLReadNextStartElement(reader)) {
        if (reader->name() == QStringLiteral("node")) {
          bool is_root = false;
          bool is_cm = false;
          bool is_settings = false;
          QString id;

          {
            XMLAttributeLoop(reader, attr) {
              if (attr.name() == QStringLiteral("id")) {
                id = attr.value().toString();
              } else if (attr.name() == QStringLiteral("root") && attr.value() == QStringLiteral("1")) {
                is_root = true;
              } else if (attr.name() == QStringLiteral("cm") && attr.value() == QStringLiteral("1")) {
                is_cm = true;
              } else if (attr.name() == QStringLiteral("settings") && attr.value() == QStringLiteral("1")) {
                is_settings = true;
              }
            }
          }

          if (id.isEmpty()) {
            qWarning() << "Failed to load node with empty ID";
            reader->skipCurrentElement();
          } else {
            Node* node;

            if (is_root) {
              node = this->root();
            } else if (is_cm) {
              node = this->color_manager();
            } else if (is_settings) {
              node = this->settings();
            } else {
              node = NodeFactory::CreateFromID(id);
            }

            if (!node) {
              qWarning() << "Failed to find node with ID" << id;
              reader->skipCurrentElement();
            } else {
              // Disable cache while node is being loaded (we'll re-enable it later)
              node->SetCachesEnabled(false);

              node->Load(reader, &data);

              node->setParent(this);
            }
          }
        } else {
          reader->skipCurrentElement();
        }
      }

    } else {

      // Skip this
      reader->skipCurrentElement();

    }
  }

  for (auto it = this->nodes().cbegin(); it != this->nodes().cend(); it++){
    (*it)->PostLoadEvent(&data);
  }

  foreach (const SerializedData::SerializedConnection& con, data.desired_connections) {
    if (Node *out = data.node_ptrs.value(con.output_node)) {
      Node::ConnectEdge(out, con.input);
    }
  }

  foreach (const SerializedData::BlockLink& l, data.block_links) {
    Node *a = l.block;
    Node *b = data.node_ptrs.value(l.link);

    Node::Link(a, b);
  }

  // Re-enable caches and resolve tracks
  for (Node *n : this->nodes()) {
    n->SetCachesEnabled(true);
  }
}

void Project::Save(QXmlStreamWriter *writer) const
{
  writer->writeAttribute(QStringLiteral("version"), QString::number(230220));

  writer->writeTextElement(QStringLiteral("uuid"), this->GetUuid().toString());

  writer->writeStartElement(QStringLiteral("nodes"));

  foreach (Node* node, this->nodes()) {
    writer->writeStartElement(QStringLiteral("node"));

    if (node == this->root()) {
      writer->writeAttribute(QStringLiteral("root"), QStringLiteral("1"));
    } else if (node == this->color_manager()) {
      writer->writeAttribute(QStringLiteral("cm"), QStringLiteral("1"));
    } else if (node == this->settings()) {
      writer->writeAttribute(QStringLiteral("settings"), QStringLiteral("1"));
    }

    writer->writeAttribute(QStringLiteral("id"), node->id());

    node->Save(writer);

    writer->writeEndElement(); // node
  }

  writer->writeEndElement(); // nodes
}

int Project::GetNumberOfContextsNodeIsIn(Node *node, bool except_itself) const
{
  int count = 0;

  foreach (Node *ctx, node_children_) {
    if (ctx->ContextContainsNode(node) && (!except_itself || ctx != node)) {
      count++;
    }
  }

  return count;
}

void Project::childEvent(QChildEvent *event)
{
  super::childEvent(event);

  Node* node = dynamic_cast<Node*>(event->child());

  if (node) {
    if (event->type() == QEvent::ChildAdded) {

      node_children_.append(node);

      // Connect signals
      connect(node, &Node::InputConnected, this, &Project::InputConnected, Qt::DirectConnection);
      connect(node, &Node::InputDisconnected, this, &Project::InputDisconnected, Qt::DirectConnection);
      connect(node, &Node::ValueChanged, this, &Project::ValueChanged, Qt::DirectConnection);
      connect(node, &Node::InputValueHintChanged, this, &Project::InputValueHintChanged, Qt::DirectConnection);

      if (NodeGroup *group = dynamic_cast<NodeGroup*>(node)) {
        connect(group, &NodeGroup::InputPassthroughAdded, this, &Project::GroupAddedInputPassthrough, Qt::DirectConnection);
        connect(group, &NodeGroup::InputPassthroughRemoved, this, &Project::GroupRemovedInputPassthrough, Qt::DirectConnection);
        connect(group, &NodeGroup::OutputPassthroughChanged, this, &Project::GroupChangedOutputPassthrough, Qt::DirectConnection);
      }

      emit NodeAdded(node);
      emit node->AddedToGraph(this);

      // Emit input connections
      for (auto it=node->input_connections().cbegin(); it!=node->input_connections().cend(); it++) {
        if (nodes().contains(it->second)) {
          emit InputConnected(it->second, it->first);
        }
      }

      // Emit output connections
      for (auto it=node->output_connections().cbegin(); it!=node->output_connections().cend(); it++) {
        if (nodes().contains(it->second.node())) {
          emit InputConnected(it->first, it->second);
        }
      }

    } else if (event->type() == QEvent::ChildRemoved) {

      node_children_.removeOne(node);

      // Disconnect signals
      disconnect(node, &Node::InputConnected, this, &Project::InputConnected);
      disconnect(node, &Node::InputDisconnected, this, &Project::InputDisconnected);
      disconnect(node, &Node::ValueChanged, this, &Project::ValueChanged);
      disconnect(node, &Node::InputValueHintChanged, this, &Project::InputValueHintChanged);

      if (NodeGroup *group = dynamic_cast<NodeGroup*>(node)) {
        disconnect(group, &NodeGroup::InputPassthroughAdded, this, &Project::GroupAddedInputPassthrough);
        disconnect(group, &NodeGroup::InputPassthroughRemoved, this, &Project::GroupRemovedInputPassthrough);
        disconnect(group, &NodeGroup::OutputPassthroughChanged, this, &Project::GroupChangedOutputPassthrough);
      }

      emit NodeRemoved(node);
      emit node->RemovedFromGraph(this);

      // Remove from any contexts
      foreach (Node *context, node_children_) {
        context->RemoveNodeFromContext(node);
      }
    }
  }
}

QString Project::name() const
{
  if (filename_.isEmpty()) {
    return tr("(untitled)");
  } else {
    return QFileInfo(filename_).completeBaseName();
  }
}

const QString &Project::filename() const
{
  return filename_;
}

QString Project::pretty_filename() const
{
  QString fn = filename();

  if (fn.isEmpty()) {
    return tr("(untitled)");
  } else {
    return fn;
  }
}

void Project::set_filename(const QString &s)
{
  filename_ = s;

#ifdef Q_OS_WINDOWS
  // Prevents filenames
  filename_.replace('/', '\\');
#endif

  emit NameChanged();
}

void Project::set_modified(bool e)
{
  is_modified_ = e;
  set_autorecovery_saved(!e);

  emit ModifiedChanged(is_modified_);
}

bool Project::has_autorecovery_been_saved() const
{
  return autorecovery_saved_;
}

void Project::set_autorecovery_saved(bool e)
{
  autorecovery_saved_ = e;
}

bool Project::is_new() const
{
  return !is_modified_ && filename_.isEmpty();
}

QString Project::get_cache_alongside_project_path() const
{
  if (!filename_.isEmpty()) {
    // Non-translated string so the path doesn't change if the language does
    return QFileInfo(filename_).dir().filePath(QStringLiteral("cache"));
  }
  return QString();
}

QString Project::cache_path() const
{
  ProjectSettingsNode::CacheSetting setting = settings_->GetCacheSetting();

  switch (setting) {
  case ProjectSettingsNode::kCacheUseDefaultLocation:
    break;
  case ProjectSettingsNode::kCacheCustomPath:
  {
    QString cache_path = settings_->GetCustomCachePath();
    if (cache_path.isEmpty()) {
      return cache_path;
    }
    break;
  }
  case ProjectSettingsNode::kCacheStoreAlongsideProject:
  {
    QString alongside = get_cache_alongside_project_path();
    if (!alongside.isEmpty()) {
      return alongside;
    }
    break;
  }
  }

  return DiskManager::instance()->GetDefaultCachePath();
}

void Project::RegenerateUuid()
{
  uuid_ = QUuid::createUuid();
}

Project *Project::GetProjectFromObject(const QObject *o)
{
  return QtUtils::GetParentOfType<Project>(o);
}

void Project::ColorManagerValueChanged(const NodeInput &input, const TimeRange &range)
{
  Q_UNUSED(input)
  Q_UNUSED(range)

  QVector<Footage*> footage = root()->ListChildrenOfType<Footage>();

  foreach (Footage* item, footage) {
    item->InvalidateAll(QString());
  }
}

}