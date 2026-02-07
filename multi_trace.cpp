#include "multi_trace.h"
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <QTimer>
#include <QMetaObject>

// ================= ThreadSafeGrid =================

ThreadSafeGrid::ThreadSafeGrid(GridCell*** grid, int width, int height, int layers)
    : grid(nullptr), boardWidth(width), boardHeight(height), totalLayers(layers)
{
    // Проверка параметров
    if (!grid || width <= 0 || height <= 0 || layers <= 0) {
        qDebug() << "Ошибка: неверные параметры для ThreadSafeGrid";
        layerMutexes = nullptr;
        return;
    }

    this->grid = grid;
    layerMutexes = new QMutex*[static_cast<size_t>(totalLayers)];
    for (int l = 0; l < totalLayers; l++) {
        layerMutexes[l] = new QMutex();
    }
}

ThreadSafeGrid::~ThreadSafeGrid() {
    for (int l = 0; l < totalLayers; l++) {
        delete layerMutexes[l];
    }
    delete[] layerMutexes;
}

GridCell ThreadSafeGrid::readCell(int x, int y, int layer) const {
    if (x < 0 || x >= boardWidth || y < 0 || y >= boardHeight ||
        layer < 0 || layer >= totalLayers) {
        return GridCell{CELL_EMPTY, layer, -1, -1, Qt::white};
    }

    QMutexLocker locker(layerMutexes[layer]);
    return grid[layer][y][x];
}

bool ThreadSafeGrid::writeCell(int x, int y, int layer, const GridCell& newValue) {
    if (x < 0 || x >= boardWidth || y < 0 || y >= boardHeight ||
        layer < 0 || layer >= totalLayers) {
        return false;
    }

    QMutexLocker locker(layerMutexes[layer]);
    grid[layer][y][x] = newValue;
    return true;
}

bool ThreadSafeGrid::tryPlaceTrace(const QList<GridPoint>& path, int traceId) {
    if (path.isEmpty()) return false;

    // Проверяем возможность размещения
    // Используем -1 для fromPadId и toPadId, так как эта проверка уже выполнена
    if (!canPlacePath(path, traceId, traceId)) {
        return false;
    }

    // Размещаем трассу (атомарно для всей трассы)
    QVector<QMutexLocker*> lockers;

    // Сначала блокируем все необходимые ячейки
    for (const GridPoint& point : path) {
        if (point.layer < 0 || point.layer >= totalLayers) {
            // Разблокируем всё и выходим
            for (auto locker : lockers) delete locker;
            return false;
        }
        lockers.append(new QMutexLocker(layerMutexes[point.layer]));
    }

    // Размещаем трассу
    for (int i = 0; i < path.size(); i++) {
        const GridPoint& point = path[i];
        GridCell& cell = grid[point.layer][point.y][point.x];

        // Пропускаем площадки
        if (cell.type == CELL_PAD) continue;

        // Определяем, является ли это VIA
        bool isVia = false;
        if (i > 0) {
            const GridPoint& prev = path[i - 1];
            if (prev.x == point.x && prev.y == point.y && prev.layer != point.layer) {
                isVia = true;
            }
        }

        if (isVia) {
            cell.type = CELL_VIA;
        } else {
            cell.type = CELL_TRACE;
        }
        cell.traceId = traceId;
    }

    // Очищаем локеры
    for (auto locker : lockers) delete locker;
    return true;
}

bool ThreadSafeGrid::canPlacePath(const QList<GridPoint>& path, int fromPadId, int toPadId) const {
    QSet<QString> checkedCells;  // Для предотвращения повторных проверок

    for (const GridPoint& point : path) {
        QString cellKey = QString("%1-%2-%3").arg(point.x).arg(point.y).arg(point.layer);
        if (checkedCells.contains(cellKey)) continue;
        checkedCells.insert(cellKey);

        // Проверяем границы
        if (point.x < 0 || point.x >= boardWidth ||
            point.y < 0 || point.y >= boardHeight ||
            point.layer < 0 || point.layer >= totalLayers) {
            return false;
        }

        QMutexLocker locker(layerMutexes[point.layer]);
        const GridCell& cell = grid[point.layer][point.y][point.x];

        // Для площадок - только наши площадки
        if (cell.type == CELL_PAD) {
            if (cell.padId != fromPadId && cell.padId != toPadId) {
                return false;
            }
            continue;
        }

        // Препятствия
        if (cell.type == CELL_OBSTACLE) {
            return false;
        }

        // Трассы и VIA - только если свободны или наши
        if (cell.type == CELL_TRACE || cell.type == CELL_VIA) {
            if (cell.traceId != -1 && cell.traceId != fromPadId) {
                return false;
            }
        }
    }

    return true;
}

QMutex* ThreadSafeGrid::getLayerMutex(int layer) const {
    if (layer < 0 || layer >= totalLayers) return nullptr;
    return layerMutexes[layer];
}

int ThreadSafeGrid::getLayerOccupancy(int layer) const {
    if (layer < 0 || layer >= totalLayers) return 0;

    QMutex* mutex = layerMutexes[layer];
    if (mutex) {
        QMutexLocker locker(mutex);
        int occupied = 0;
        for (int y = 0; y < boardHeight; y++) {
            for (int x = 0; x < boardWidth; x++) {
                if (grid[layer][y][x].type == CELL_TRACE ||
                    grid[layer][y][x].type == CELL_VIA) {
                    occupied++;
                }
            }
        }
        return occupied;
    }
    return 0;
}

int ThreadSafeGrid::getLayerDifficulty(int layer) const {
    if (layer < 0 || layer >= totalLayers) return 1000; // Высокая сложность

    int occupancy = getLayerOccupancy(layer);

    // Сложность = занятость + штраф за препятствия
    int obstacleCount = 0;
    {
        QMutexLocker locker(layerMutexes[layer]);
        for (int y = 0; y < boardHeight; y++) {
            for (int x = 0; x < boardWidth; x++) {
                if (grid[layer][y][x].type == CELL_OBSTACLE) {
                    obstacleCount++;
                }
            }
        }
    }

    return occupancy * 10 + obstacleCount * 5;
}

// ================= LayerRoutingThread =================

LayerRoutingThread::LayerRoutingThread(int layerId,
                                       ThreadSafeGrid* grid,
                                       PathFinder* pathFinder,
                                       QObject* parent)
    : QThread(parent), layerId(layerId), grid(grid), pathFinder(pathFinder),
      running(false), stats(layerId)
{
}

LayerRoutingThread::~LayerRoutingThread() {
    stop();
    wait();
}

void LayerRoutingThread::setConnections(const QList<ConnectionRequest>& connections) {
    QMutexLocker locker(&connectionsMutex);
    this->connections = connections;
    stats.connectionsAssigned = connections.size();
}

void LayerRoutingThread::stop() {
    running.store(false);
}

LayerStats LayerRoutingThread::getStats() const {
    QMutexLocker locker(&connectionsMutex);
    return stats;
}

bool LayerRoutingThread::isIdle() const {
    QMutexLocker locker(&connectionsMutex);
    return connections.isEmpty();
}

void LayerRoutingThread::run() {
    running.store(true);

    qDebug() << "Layer thread" << layerId << "started";

    while (running.load()) {
        // Берем следующее соединение
        ConnectionRequest conn;
        {
            QMutexLocker locker(&connectionsMutex);
            if (connections.isEmpty()) {
                msleep(10);
                continue;
            }
            conn = connections.takeFirst();
        }

        // Ищем путь
        RoutingResult result = findPathForConnection(conn);

        if (result.success) {
            emit pathFound(result);
            stats.connectionsCompleted++;
        } else {
            emit routingFailed(conn.fromPadId, conn.toPadId);
        }

        // Проверяем, не пуста ли очередь
        {
            QMutexLocker locker(&connectionsMutex);
            if (connections.isEmpty()) {
                emit threadIdle(layerId);
            }
        }

        // Небольшая пауза для балансировки нагрузки
        msleep(1);
    }

    running.store(false);
    qDebug() << "Layer thread" << layerId << "finished";
}

RoutingResult LayerRoutingThread::findPathForConnection(const ConnectionRequest& conn) {
    RoutingResult result;
    result.fromPadId = conn.fromPadId;
    result.toPadId = conn.toPadId;
    result.success = false;
    result.layerUsed = -1;
    result.connIndex = -1;

    // Пытаемся найти путь на текущем слое
    GridPoint start(conn.start.x, conn.start.y, layerId);
    GridPoint end(conn.end.x, conn.end.y, layerId);

    // Проверяем, доступны ли точки на этом слое
    GridCell startCell = grid->readCell(start.x, start.y, layerId);
    GridCell endCell = grid->readCell(end.x, end.y, layerId);

    if (startCell.type != CELL_PAD || endCell.type != CELL_PAD) {
        return result;
    }

    // Ищем путь
    GridCell*** rawGrid = grid->getGrid();
    QList<GridPoint> path = pathFinder->findPath(start, end, rawGrid,
                                                grid->getWidth(), grid->getHeight(),
                                                grid->getTotalLayers(),
                                                conn.fromPadId, conn.toPadId);

    if (!path.isEmpty()) {
        result.path = path;
        result.success = true;
        result.layerUsed = layerId;
    }

    return result;
}

QList<GridPoint> LayerRoutingThread::findPathSingleLayer(const GridPoint& start,
                                                        const GridPoint& end,
                                                        int fromPadId, int toPadId) {
    return pathFinder->findPath(start, end, grid->getGrid(),
                               grid->getWidth(), grid->getHeight(),
                               grid->getTotalLayers(),
                               fromPadId, toPadId);
}

// ================= PlacementManager =================

PlacementManager::PlacementManager(ThreadSafeGrid* grid, QObject* parent)
    : QObject(parent), grid(grid), maxRetries(3)
{
}

void PlacementManager::onPathFound(const RoutingResult& result) {
    if (!result.success) return;

    bool placed = tryPlacePath(result.path, result.fromPadId);

    if (placed) {
        emit placementCompleted(result);
    } else {
        // Планируем повторную попытку
        scheduleRetry(result);
    }
}

void PlacementManager::scheduleRetry(const RoutingResult& failedResult) {
    QString key = QString("%1-%2").arg(failedResult.fromPadId).arg(failedResult.toPadId);

    RetryInfo info;
    info.result = failedResult;
    info.retryCount = 1;
    info.nextRetryTime = QDateTime::currentDateTime().addMSecs(100); // 100ms задержка

    {
        QMutexLocker locker(&placementMutex);
        retryQueue[key] = info;
    }

    // Запускаем обработку очереди
    QTimer::singleShot(100, this, &PlacementManager::processRetryQueue);
}

bool PlacementManager::tryPlacePath(const QList<GridPoint>& path, int traceId) {
    return grid->tryPlaceTrace(path, traceId);
}

void PlacementManager::processRetryQueue() {
    QMutexLocker locker(&placementMutex);

    QDateTime now = QDateTime::currentDateTime();
    QList<QString> toRemove;

    for (auto it = retryQueue.begin(); it != retryQueue.end(); ++it) {
        if (it.value().nextRetryTime <= now) {
            RetryInfo& info = it.value();

            if (info.retryCount >= maxRetries) {
                // Превышено количество попыток
                emit placementFailed(info.result.fromPadId, info.result.toPadId);
                toRemove.append(it.key());
            } else {
                // Пробуем ещё раз
                bool placed = tryPlacePath(info.result.path, info.result.fromPadId);

                if (placed) {
                    emit placementCompleted(info.result);
                    toRemove.append(it.key());
                } else {
                    // Увеличиваем счетчик и планируем следующую попытку
                    info.retryCount++;
                    info.nextRetryTime = now.addMSecs(info.retryCount * 200); // Экспоненциальная задержка
                    emit retryRequested(info.result);
                }
            }
        }
    }

    // Удаляем обработанные
    for (const QString& key : toRemove) {
        retryQueue.remove(key);
    }

    // Если очередь не пуста, планируем следующую обработку
    if (!retryQueue.isEmpty()) {
        QTimer::singleShot(50, this, &PlacementManager::processRetryQueue);
    }
}

// ================= MultiThreadedRouter =================

MultiThreadedRouter::MultiThreadedRouter(GridCell*** grid,
                                       int boardWidth, int boardHeight, int layerCount,
                                       const QList<ConnectionRequest>& connections,
                                       QObject* parent)
    : QObject(parent),
      boardWidth(boardWidth), boardHeight(boardHeight), layerCount(layerCount),
      threadsPerLayer(1),
      safeGrid(new ThreadSafeGrid(grid, boardWidth, boardHeight, layerCount)),
      allConnections(connections),
      activeThreads(0),
      totalProcessed(0),
      totalConnections(connections.size()),
      pathFinder(new PathFinder()),
      placementManager(nullptr)
{
    // Настройка thread pool для размещения
    placementThreadPool.setMaxThreadCount(QThread::idealThreadCount());
}

MultiThreadedRouter::~MultiThreadedRouter() {
    stopRouting();
    delete safeGrid;
    delete pathFinder;
    if (placementManager) {
        delete placementManager;
    }
}

void MultiThreadedRouter::setLayerThreadCount(int threadsPerLayer) {
    this->threadsPerLayer = qMax(1, threadsPerLayer);
}

void MultiThreadedRouter::setPlacementThreads(int count) {
    count = qMax(1, count);
    placementThreadPool.setMaxThreadCount(count);
}

void MultiThreadedRouter::startRouting() {
    qDebug() << "Starting multi-threaded routing with" << layerCount << "layers";

    // Инициализация
    initializeThreads();
    distributeConnections();

    // Подключаем сигналы
    connectSignals();

    // Запуск потоков
    for (LayerRoutingThread* thread : layerThreads) {
        thread->start();
        activeThreads++;
    }

    // Создаем менеджер размещения
    placementManager = new PlacementManager(safeGrid, this);
    connect(placementManager, &PlacementManager::placementCompleted,
            this, &MultiThreadedRouter::onPlacementCompleted);
    connect(placementManager, &PlacementManager::placementFailed,
            this, &MultiThreadedRouter::onPlacementFailed);

    qDebug() << "Routing started with" << layerThreads.size() << "layer threads";
}

void MultiThreadedRouter::stopRouting() {
    // Останавливаем все потоки
    for (LayerRoutingThread* thread : layerThreads) {
        thread->stop();
    }

    // Ждем завершения
    for (LayerRoutingThread* thread : layerThreads) {
        thread->wait();
    }

    cleanup();
}

bool MultiThreadedRouter::isRunning() const {
    return activeThreads.load() > 0;
}

QList<RoutingResult> MultiThreadedRouter::getSuccessfulResults() {
    QMutexLocker locker(&resultsMutex);
    return successfulResults;
}

QList<QPair<int, int>> MultiThreadedRouter::getFailedConnections() {
    QMutexLocker locker(&resultsMutex);
    return failedConnections;
}

QMap<int, LayerStats> MultiThreadedRouter::getLayerStatistics() const {
    QMap<int, LayerStats> stats;
    for (LayerRoutingThread* thread : layerThreads) {
        LayerStats threadStats = thread->getStats();
        stats[threadStats.layerId] = threadStats;
    }
    return stats;
}

void MultiThreadedRouter::onLayerThreadIdle(int layerId) {
    qDebug() << "Layer" << layerId << "is idle, redistributing load...";

    // Находим самый загруженный слой
    int mostBusyLayer = -1;
    int maxQueueSize = 0;

    for (LayerRoutingThread* thread : layerThreads) {
        if (!thread->isIdle()) {
            LayerStats stats = thread->getStats();
            if (stats.connectionsAssigned - stats.connectionsCompleted > maxQueueSize) {
                maxQueueSize = stats.connectionsAssigned - stats.connectionsCompleted;
                mostBusyLayer = stats.layerId;
            }
        }
    }

    if (mostBusyLayer != -1 && mostBusyLayer != layerId) {
        redistributeLoad(mostBusyLayer, layerId);
    }
}

void MultiThreadedRouter::onPlacementCompleted(const RoutingResult& result) {
    QMutexLocker locker(&resultsMutex);
    successfulResults.append(result);

    totalProcessed++;

    // Обновляем прогресс
    int percent = (totalProcessed.load() * 100) / qMax(1, allConnections.size());
    emit progressChanged(percent);

    // Проверяем, все ли завершено
    checkCompletion();
}

void MultiThreadedRouter::onPlacementFailed(int fromPadId, int toPadId) {
    QMutexLocker locker(&resultsMutex);
    failedConnections.append(qMakePair(fromPadId, toPadId));

    totalProcessed++;

    // Проверяем, все ли завершено
    checkCompletion();
}

void MultiThreadedRouter::onPathFoundFromThread(const RoutingResult& result) {
    // Передаем результат менеджеру размещения
    if (placementManager) {
        placementManager->onPathFound(result);
    }
}

void MultiThreadedRouter::onRoutingFailedFromThread(int fromPadId, int toPadId) {
    QMutexLocker locker(&resultsMutex);
    failedConnections.append(qMakePair(fromPadId, toPadId));

    totalProcessed++;

    // Обновляем прогресс
    int percent = (totalProcessed * 100) / qMax(1, allConnections.size());
    emit progressChanged(percent);

    // Проверяем завершение
    checkCompletion();
}

void MultiThreadedRouter::checkCompletion() {
    QMutexLocker locker(&resultsMutex);
    int completed = successfulResults.size() + failedConnections.size();

    if (completed >= allConnections.size()) {
        bool allThreadsStopped = true;
        for (LayerRoutingThread* thread : layerThreads) {
            if (thread && thread->isRunning()) {
                allThreadsStopped = false;
                break;
            }
        }

        if (allThreadsStopped) {
            emit routingComplete();
        }
    }
}

void MultiThreadedRouter::connectSignals() {
    for (LayerRoutingThread* thread : layerThreads) {
        if (thread) {
            // Используем старый синтаксис для надежности
            connect(thread, SIGNAL(pathFound(RoutingResult)),
                    this, SLOT(onPathFoundFromThread(RoutingResult)));
            connect(thread, SIGNAL(routingFailed(int, int)),
                    this, SLOT(onRoutingFailedFromThread(int, int)));
            connect(thread, SIGNAL(threadIdle(int)),
                    this, SLOT(onLayerThreadIdle(int)));
        }
    }
}

void MultiThreadedRouter::initializeThreads() {
    cleanup(); // Очищаем старые потоки

    // Создаем потоки для каждого слоя
    for (int layer = 0; layer < layerCount; layer++) {
        PathFinder* threadPathFinder = new PathFinder();
        LayerRoutingThread* thread = new LayerRoutingThread(layer, safeGrid, threadPathFinder, this);
        layerThreads.append(thread);
    }

    qDebug() << "Initialized" << layerThreads.size() << "threads";
}

void MultiThreadedRouter::distributeConnections() {
    // Группируем соединения по слоям
    QVector<QList<ConnectionRequest>> layerConnections(layerCount);

    for (const ConnectionRequest& conn : allConnections) {
        int bestLayer = selectBestLayerForConnection(conn);
        if (bestLayer >= 0 && bestLayer < layerCount) {
            ConnectionRequest prioritizedConn = conn;
            prioritizedConn.priority = calculateConnectionPriority(conn);
            layerConnections[bestLayer].append(prioritizedConn);
        }
    }

    // Сортируем соединения по приоритету (меньше = выше приоритет)
    for (int l = 0; l < layerCount; l++) {
        std::sort(layerConnections[l].begin(), layerConnections[l].end(),
                  [](const ConnectionRequest& a, const ConnectionRequest& b) {
                      return a.priority < b.priority;
                  });

        // Распределяем по потокам
        if (l < layerThreads.size()) {
            layerThreads[l]->setConnections(layerConnections[l]);
        }
    }

    qDebug() << "Connections distributed:";
    for (int l = 0; l < layerCount; l++) {
        qDebug() << "  Layer" << l << ":" << layerConnections[l].size() << "connections";
    }
}

void MultiThreadedRouter::redistributeLoad(int fromLayer, int toLayer) {
    // Перераспределяем часть соединений с загруженного слоя на свободный
    qDebug() << "Redistributing load from layer" << fromLayer << "to layer" << toLayer;
}

void MultiThreadedRouter::cleanup() {
    for (LayerRoutingThread* thread : layerThreads) {
        if (thread) {
            thread->stop();
            thread->wait();
            delete thread;
        }
    }
    layerThreads.clear();

    if (placementManager) {
        delete placementManager;
        placementManager = nullptr;
    }
}

int MultiThreadedRouter::calculateConnectionPriority(const ConnectionRequest& conn) const {
    // Приоритет = расстояние + сложность начального слоя
    int distance = abs(conn.start.x - conn.end.x) + abs(conn.start.y - conn.end.y);
    int startLayerDifficulty = safeGrid->getLayerDifficulty(conn.start.layer);

    return distance + startLayerDifficulty;
}

int MultiThreadedRouter::selectBestLayerForConnection(const ConnectionRequest& conn) const {
    // Выбираем лучший слой для трассировки
    // 1. Проверяем, на каких слоях есть обе площадки
    QList<int> availableLayers;

    for (int layer = 0; layer < layerCount; layer++) {
        GridCell startCell = safeGrid->readCell(conn.start.x, conn.start.y, layer);
        GridCell endCell = safeGrid->readCell(conn.end.x, conn.end.y, layer);

        if (startCell.type == CELL_PAD && startCell.padId == conn.fromPadId &&
            endCell.type == CELL_PAD && endCell.padId == conn.toPadId) {
            availableLayers.append(layer);
        }
    }

    if (availableLayers.isEmpty()) {
        return conn.start.layer; // По умолчанию
    }

    // 2. Выбираем слой с наименьшей сложностью
    int bestLayer = availableLayers.first();
    int bestDifficulty = safeGrid->getLayerDifficulty(bestLayer);

    for (int i = 1; i < availableLayers.size(); i++) {
        int layer = availableLayers[i];
        int difficulty = safeGrid->getLayerDifficulty(layer);

        if (difficulty < bestDifficulty) {
            bestLayer = layer;
            bestDifficulty = difficulty;
        }
    }

    return bestLayer;
}
