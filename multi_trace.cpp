// multi_trace.cpp
#include "multi_trace.h"
#include "trace.h"
#include <QThread>
#include <QDebug>

// Вспомогательная задача для трассировки одного соединения
class RoutingTask : public QRunnable
{
public:
    RoutingTask(MultiThreadedRouter* router, int index, const ConnectionRequest& req,
                GridCell*** globalGrid, int boardWidth, int boardHeight, int layerCount)
        : router(router), index(index), req(req), globalGrid(globalGrid),
          boardWidth(boardWidth), boardHeight(boardHeight), layerCount(layerCount)
    {
        setAutoDelete(true);
    }

    void run() override
    {
        // 1. Копируем глобальную сетку
        GridCell*** localGrid = copyGrid(globalGrid, boardWidth, boardHeight, layerCount);
        if (!localGrid) {
            RoutingResult result;
            result.connIndex = index;
            result.fromPadId = req.fromPadId;
            result.toPadId = req.toPadId;
            result.success = false;
            router->addResult(result);
            return;
        }

        // 2. Ищем путь
        PathFinder pathFinder;
        QList<GridPoint> path = pathFinder.findPath(req.start, req.end, localGrid,
                                                    boardWidth, boardHeight, layerCount,
                                                    req.fromPadId, req.toPadId);
        RoutingResult result;
        result.connIndex = index;
        result.fromPadId = req.fromPadId;
        result.toPadId = req.toPadId;
        result.success = !path.isEmpty();
        if (result.success) {
            result.path = path;
            result.layerUsed = path.first().layer;
        }

        // 3. Если путь найден, пытаемся закоммитить в глобальную сетку
        if (result.success) {
            QMutexLocker locker(&router->mutex); // используем мьютекс роутера
            // Проверяем, что все ячейки пути свободны в глобальной сетке (кроме своих)
            bool canCommit = true;
            for (const GridPoint& p : path) {
                // Пропускаем площадки
                if (globalGrid[p.layer][p.y][p.x].type == CELL_PAD)
                    continue;
                // Проверяем, что ячейка не занята другой трассой
                if (globalGrid[p.layer][p.y][p.x].type == CELL_TRACE ||
                    globalGrid[p.layer][p.y][p.x].type == CELL_VIA) {
                    if (globalGrid[p.layer][p.y][p.x].traceId != req.fromPadId) {
                        canCommit = false;
                        break;
                    }
                }
                // Также проверяем препятствия (хотя они уже должны быть в копии)
                if (globalGrid[p.layer][p.y][p.x].type == CELL_OBSTACLE) {
                    canCommit = false;
                    break;
                }
            }

            if (canCommit) {
                // Коммитим: обновляем глобальную сетку
                for (int i = 0; i < path.size(); ++i) {
                    const GridPoint& p = path[i];
                    if (globalGrid[p.layer][p.y][p.x].type == CELL_PAD)
                        continue;
                    bool isVia = (i > 0 && path[i-1].x == p.x && path[i-1].y == p.y &&
                                  path[i-1].layer != p.layer);
                    if (isVia) {
                        globalGrid[p.layer][p.y][p.x].type = CELL_VIA;
                    } else {
                        globalGrid[p.layer][p.y][p.x].type = CELL_TRACE;
                    }
                    globalGrid[p.layer][p.y][p.x].traceId = req.fromPadId;
                }
                // Успешно закоммитили
            } else {
                result.success = false; // конфликт, не можем разместить
            }
        }

        // 4. Сообщаем результат
        router->addResult(result);

        // 5. Освобождаем локальную сетку
        freeGrid(localGrid, boardWidth, boardHeight, layerCount);
    }

private:
    MultiThreadedRouter* router;
    int index;
    ConnectionRequest req;
    GridCell*** globalGrid;
    int boardWidth;
    int boardHeight;
    int layerCount;

    static GridCell*** copyGrid(GridCell*** src, int w, int h, int layers)
    {
        GridCell*** dst = new GridCell**[layers];
        for (int l = 0; l < layers; ++l) {
            dst[l] = new GridCell*[h];
            for (int y = 0; y < h; ++y) {
                dst[l][y] = new GridCell[w];
                for (int x = 0; x < w; ++x) {
                    dst[l][y][x] = src[l][y][x];
                }
            }
        }
        return dst;
    }

    static void freeGrid(GridCell*** grid, int w, int h, int layers)
    {
        if (!grid) return;
        for (int l = 0; l < layers; ++l) {
            if (grid[l]) {
                for (int y = 0; y < h; ++y) {
                    delete[] grid[l][y];
                }
                delete[] grid[l];
            }
        }
        delete[] grid;
    }
};

// ------------------- MultiThreadedRouter -------------------

MultiThreadedRouter::MultiThreadedRouter(GridCell*** grid, int boardWidth, int boardHeight,
                                         int layerCount, const QList<ConnectionRequest>& requests,
                                         QObject* parent)
    : QObject(parent), grid(grid), boardWidth(boardWidth), boardHeight(boardHeight),
      layerCount(layerCount), requests(requests), isRunning(false), totalRequests(0), completedRequests(0)
{
    threadPool.setMaxThreadCount(QThread::idealThreadCount());
}

MultiThreadedRouter::~MultiThreadedRouter()
{
    threadPool.waitForDone();
}

void MultiThreadedRouter::setLayerThreadCount(int count)
{
    if (count > 0)
        threadPool.setMaxThreadCount(count);
}

void MultiThreadedRouter::startRouting()
{
    if (isRunning) return;
    isRunning = true;
    completedRequests = 0;
    successfulResults.clear();
    failedConnections.clear();
    failedIndices.clear();

    totalRequests = requests.size();

    for (int i = 0; i < requests.size(); ++i) {
        RoutingTask* task = new RoutingTask(this, i, requests[i], grid,
                                            boardWidth, boardHeight, layerCount);
        threadPool.start(task);
    }
}

void MultiThreadedRouter::addResult(const RoutingResult& result)
{
    QMutexLocker locker(&mutex);
    if (result.success) {
        successfulResults.append(result);
    } else {
        failedConnections.append(qMakePair(result.fromPadId, result.toPadId));
        failedIndices.append(result.connIndex);
    }
    completedRequests++;
    int percent = (completedRequests * 100) / totalRequests;
    emit progressChanged(percent);
    if (completedRequests == totalRequests) {
        isRunning = false;
        emit routingComplete();
    }
}

QList<RoutingResult> MultiThreadedRouter::getSuccessfulResults() const
{
    QMutexLocker locker(&mutex);
    return successfulResults;
}

QList<QPair<int,int>> MultiThreadedRouter::getFailedConnections() const
{
    QMutexLocker locker(&mutex);
    return failedConnections;
}
