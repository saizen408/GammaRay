/*
  This file is part of GammaRay, the Qt application inspection and
  manipulation tool.

  Copyright (C) 2010-2011 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com
  Author: Kevin Funk <kevin.funk@kdab.com>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "statemachineviewer.h"
#include "ui_statemachineviewer.h"

#include "statemachinewatcher.h"
#include "gvgraph/gvgraph.h"
#include "gvgraph/gvgraphitems.h"

#include <transitionmodel.h>
#include <statemodel.h>
#include <probeinterface.h>
#include <objecttypefilterproxymodel.h>

#include <QAbstractTransition>
#include <QDebug>
#include <QHBoxLayout>
#include <QPainter>
#include <QList>
#include <QGraphicsView>
#include <QScrollBar>
#include <QTextBrowser>
#include <QFinalState>
#include <QStateMachine>
#include <QWheelEvent>

#include <QtPlugin>

using namespace GammaRay;

template<class T>
static qreal relativePosition(QList<T> list, T t) {
  const int index = list.indexOf(t);
  Q_ASSERT(index != -1);
  return (index+1.0) / list.size();
}

StateMachineView::StateMachineView(QWidget *parent)
  : QGraphicsView(parent)
{
}

StateMachineView::StateMachineView(QGraphicsScene *scene, QWidget *parent)
  : QGraphicsView(scene, parent)
{
}

void StateMachineView::zoomBy(qreal scaleFactor)
{
  scale(scaleFactor, scaleFactor);
}

void StateMachineView::wheelEvent(QWheelEvent *event)
{
  const qreal step = qAbs(event->delta() / 100.0);
  const qreal zoomFactor = event->delta() >= 0 ? step : 1.0f / step ;
  zoomBy(zoomFactor);
  event->accept();
}

StateMachineViewer::StateMachineViewer(ProbeInterface *probe, QWidget *parent)
  : QWidget(parent),
    m_ui(new Ui::StateMachineViewer),
    m_graph(new GVGraph("State Machine")),
    m_stateModel(new StateModel(this)),
    m_transitionModel(new TransitionModel(this)),
    m_filteredState(0),
    m_maximumDepth(0),
    m_selectedStateMachine(0),
    m_font(QFont("Helvetica [Cronxy]", 6)),
    m_stateMachineWatcher(new StateMachineWatcher(this))
{
  m_ui->setupUi(this);

  m_graph->setFont(m_font);

  m_lastConfigurations.resize(5);

  m_ui->graphicsView->setDragMode(QGraphicsView::ScrollHandDrag);
  m_ui->graphicsView->setScene(new QGraphicsScene(this));
  m_ui->graphicsView->setRenderHint(QPainter::Antialiasing);

  ObjectTypeFilterProxyModel<QStateMachine> *stateMachineFilter =
    new ObjectTypeFilterProxyModel<QStateMachine>(this);
  stateMachineFilter->setSourceModel(probe->objectListModel());
  m_stateMachineModel = stateMachineFilter;
  m_ui->stateMachinesView->setModel(m_stateMachineModel);
  m_ui->stateMachinesView->resize(0, 0);
  connect(m_ui->stateMachinesView, SIGNAL(clicked(QModelIndex)),
          SLOT(handleMachineClicked(QModelIndex)));

  m_ui->singleStateMachineView->setModel(m_stateModel);
  connect(m_ui->singleStateMachineView, SIGNAL(clicked(QModelIndex)),
          SLOT(handleStateClicked(QModelIndex)));

  connect(m_ui->depthSpinBox, SIGNAL(valueChanged(int)), SLOT(handleDepthChanged(int)));

  connect(m_stateMachineWatcher, SIGNAL(stateEntered(QAbstractState*)),
          SLOT(handleStatesChanged()));
  connect(m_stateMachineWatcher, SIGNAL(stateExited(QAbstractState*)),
          SLOT(handleStatesChanged()));
  connect(m_stateMachineWatcher, SIGNAL(transitionTriggered(QAbstractTransition*)),
          SLOT(handleTransitionTriggered(QAbstractTransition*)));

  setMaximumDepth(3);
}

void StateMachineViewer::clearGraph()
{
  m_graph->clear();

  m_graphItemMap.clear();
  m_nodeItemMap.clear();
  m_edgeItemMap.clear();

  m_stateGraphIdMap.clear();
  m_stateNodeIdMap.clear();
  m_transitionEdgeIdMap.clear();
}

void StateMachineViewer::repopulateGraph()
{
  qDebug() << Q_FUNC_INFO;

  clearGraph();

  for (int i = 0; i < m_stateModel->rowCount(); ++i) {
    const QModelIndex index = m_stateModel->index(i, 0);
    QObject* stateObject = index.data(StateModel::StateObjectRole).value<QObject*>();
    QAbstractState *state = qobject_cast<QAbstractState*>(stateObject);
    if (!state) {
      continue;
    }

    addState(state);
  }
}

QStateMachine *StateMachineViewer::selectedStateMachine() const
{
  return m_stateModel->stateMachine();
}

void StateMachineViewer::selectStateMachine(QStateMachine *machine)
{
  if (!machine) {
    qWarning() << Q_FUNC_INFO << "Warning: Null parameter";
    return;
  }

  m_stateModel->setStateMachine(machine);

  m_lastConfigurations.clear();
  m_lastTransitions.clear();

  setFilteredState(machine);
  m_stateMachineWatcher->setWatchedStateMachine(machine);
}

int treeDepth(QAbstractState *ascendant, QAbstractState *obj)
{
  if (!Util::descendantOf(ascendant, obj)) {
    return -1;
  }

  int depth = 0;
  QAbstractState *parent = obj->parentState();
  while (parent) {
    ++depth;
    parent = parent->parentState();
  }
  return depth;
}

bool StateMachineViewer::mayAddState(QAbstractState *state)
{
  if (!state)
    return false;

  if (m_stateNodeIdMap.contains(state)) {
    return false;
  }

  if (m_filteredState) {
    if (m_filteredState != state && !Util::descendantOf(m_filteredState, state)) {
      return false;
    }
  }

  if (m_maximumDepth > 0) {
    if (::treeDepth(m_filteredState, state) > m_maximumDepth) {
      return false;
    }
  }

  return true;
}

void StateMachineViewer::setFilteredState(QAbstractState *state)
{
  if (m_filteredState == state) {
    return;
  }

  showMessage(QString("Setting filter on: %1").arg(Util::displayString(state)));
  m_filteredState = state;
  repopulateGraph();
  repopulateView();
}

void StateMachineViewer::setMaximumDepth(int depth)
{
  if (m_maximumDepth == depth) {
    return;
  }

  showMessage(QString("Showing states until a depth of %1").arg(depth));
  m_maximumDepth = depth;
  repopulateGraph();
  repopulateView();
  m_ui->depthSpinBox->setValue(depth);
}

void StateMachineViewer::handleMachineClicked(const QModelIndex &index)
{
  QObject *stateMachineObject = index.data(ObjectModel::ObjectRole).value<QObject*>();
  QStateMachine *stateMachine = qobject_cast<QStateMachine*>(stateMachineObject);
  Q_ASSERT(stateMachine);

  selectStateMachine(stateMachine);
}

void StateMachineViewer::handleStateClicked(const QModelIndex &index)
{
  qDebug() << Q_FUNC_INFO;

  QObject *stateObject = index.data(ObjectModel::ObjectRole).value<QObject*>();
  Q_ASSERT(stateObject);
  QAbstractState *state = qobject_cast<QAbstractState*>(stateObject);
  Q_ASSERT(state);
  setFilteredState(state);
}

void StateMachineViewer::handleDepthChanged(int depth)
{
  setMaximumDepth(depth);
}

void StateMachineViewer::showMessage(const QString &message)
{
  // update log
  QPlainTextEdit *plainTextEdit = m_ui->plainTextEdit;
  plainTextEdit->appendPlainText(message);

  // auto-scroll hack
  QScrollBar *sb = plainTextEdit->verticalScrollBar();
  sb->setValue(sb->maximum());
}

void StateMachineViewer::handleTransitionTriggered(QAbstractTransition *transition)
{
  showMessage(tr("Transition triggered: %1").arg(Util::displayString(transition)));

  m_lastTransitions.enqueue(transition);
  updateTransitionItems();
}

void StateMachineViewer::updateTransitionItems()
{
  // restore default color
  Q_FOREACH (QGraphicsItem *item, m_ui->graphicsView->scene()->items()) {
    GVEdgeItem *edgeItem = qgraphicsitem_cast<GVEdgeItem*>(item);
    if (edgeItem) {
      edgeItem->setPen(QPen());
    }
  }

  // set color based on recent usage
  Q_FOREACH (QAbstractTransition *t, m_lastTransitions.entries()) {
    EdgeId id = m_transitionEdgeIdMap.value(t);
    GVEdgeItem *edgeItem = m_edgeItemMap[id];
    if (!edgeItem) {
      continue;
    }

    QColor color(Qt::red);
    color.setAlphaF(relativePosition(m_lastTransitions.entries(), t));
    QPen pen(Qt::DashLine);
    pen.setWidth(2);
    pen.setColor(color);
    edgeItem->setPen(pen);
  }
}

void StateMachineViewer::handleStatesChanged()
{
  StateMachineConfiguration config = m_stateModel->stateMachine()->configuration();
  if (m_lastConfigurations.size() > 0 && m_lastConfigurations.tail() == config) {
    return;
  }

  Q_FOREACH (QAbstractState *state, config) {
    showMessage(tr("State entered: %1").arg(Util::displayString(state)));
  }

  m_lastConfigurations.enqueue(config);
  updateStateItems();
}

void StateMachineViewer::updateStateItems()
{
  // initialize
  Q_FOREACH (QAbstractState *state, m_stateNodeIdMap.keys()) {
    NodeId id = m_stateNodeIdMap.value(state);
    GVNodeItem *nodeItem = m_nodeItemMap[id];
    if (qobject_cast<QFinalState*>(state)) {
      QPen pen(Qt::black, 3, Qt::SolidLine);
      nodeItem->setPen(pen);
    } else if (qobject_cast<QStateMachine*>(state)) {
      nodeItem->setBrush(Qt::gray);
    } else {
      nodeItem->setBrush(Qt::white);
    }
  }

  // color recent configurations based on last usage
  // note that each configuration has the same color saturation atm
  Q_FOREACH (const StateMachineConfiguration &config, m_lastConfigurations.entries()) {
    const qreal alpha = relativePosition(m_lastConfigurations.entries(), config);
    Q_FOREACH (QAbstractState *state, config) {
      NodeId id = m_stateNodeIdMap.value(state);
      GVNodeItem *nodeItem = m_nodeItemMap[id];
      if (!nodeItem) {
        continue;
      }

      QColor color(Qt::red);
      color.setAlphaF(alpha);
      QBrush brush = nodeItem->brush();
      brush.setColor(color);
      nodeItem->setBrush(brush);
    }
  }
}

void StateMachineViewer::addState(QAbstractState *state)
{
  if (!mayAddState(state)) {
    return;
  }

  QAbstractState *parentState = state->parentState();
  if (parentState) {
    addState(parentState); // be sure that parent is added first
  }

  GraphId parentGraphId = m_stateGraphIdMap.value(parentState);
  GraphId graphId;
  NodeId nodeId;
  if (parentState && parentGraphId) {
    graphId = m_graph->addGraph(Util::displayString(state), parentGraphId);
    nodeId = m_graph->addNode(Util::displayString(state), graphId);
    Q_ASSERT(m_graph->addEdge(m_stateNodeIdMap[parentState], nodeId,
                              QString("%1 -> %2").
                                arg(Util::displayString(parentState)).
                                arg(Util::displayString(state))));
  } else {
    graphId = m_graph->addGraph(Util::displayString(state));
    nodeId = m_graph->addNode(Util::displayString(state), graphId);
  }
  Q_ASSERT(graphId);
  Q_ASSERT(nodeId);
  m_stateGraphIdMap.insert(state, graphId);
  m_stateNodeIdMap.insert(state, nodeId);

  // add transitions
  Q_FOREACH (QAbstractTransition *transition, state->findChildren<QAbstractTransition*>()) {
    addTransition(transition);
  }

  // recursive call to add children
  Q_FOREACH (QAbstractState* child, state->findChildren<QAbstractState*>()) {
    addState(child);
  }
}

void StateMachineViewer::addTransition(QAbstractTransition *transition)
{
  qDebug() << Q_FUNC_INFO << transition << transition->sourceState() << transition->targetStates();

  QState *sourceState = transition->sourceState();
  QAbstractState *targetState = transition->targetState();
  addState(sourceState);
  addState(targetState);

  NodeId sourceStateId = m_stateNodeIdMap.value(sourceState);
  NodeId targetStateId = m_stateNodeIdMap.value(targetState);
  if (!sourceStateId || !targetStateId) {
    return;
  }

  EdgeId id = m_graph->addEdge(sourceStateId, targetStateId, Util::displayString(transition));
  Q_ASSERT(id);
  m_transitionEdgeIdMap.insert(transition, id);
}

void StateMachineViewer::clearView()
{
  m_ui->graphicsView->scene()->clear();
}

void StateMachineViewer::repopulateView()
{
  qDebug() << Q_FUNC_INFO;

  clearView();

  m_graph->applyLayout();

  QGraphicsScene *scene = m_ui->graphicsView->scene();
  Q_FOREACH (const GVNodePair &nodePair, m_graph->gvNodes()) {
    const NodeId &id = nodePair.first;
    const GVNode &node = nodePair.second;
    GVNodeItem *item = new GVNodeItem(node);
    scene->addItem(item);
    m_nodeItemMap.insert(id, item);
  }

  Q_FOREACH (const GVEdgePair &edgePair, m_graph->gvEdges()) {
    const EdgeId &id = edgePair.first;
    const GVEdge &edge = edgePair.second;
    GVEdgeItem *item = new GVEdgeItem(edge);
    scene->addItem(item);
    m_edgeItemMap.insert(id, item);
  }

// FIXME: Currently broken since bounding rect calculation fails
#if 0
  Q_FOREACH (const GVSubGraphPair &graphPair, m_graph->gvSubGraphs()) {
    const GraphId &id = graphPair.first;
    const GVSubGraph &graph = graphPair.second;
    GVGraphItem *item = new GVGraphItem(graph);
    scene->addItem(item);
    m_graphItemMap.insert(id, item);
  }
#endif

  updateStateItems();
  updateTransitionItems();

  // correctly set the scene rect
  scene->setSceneRect(scene->itemsBoundingRect());
}

Q_EXPORT_PLUGIN(StateMachineViewerFactory)

#include "statemachineviewer.moc"
