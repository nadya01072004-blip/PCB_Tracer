#ifndef MULTI_TRACE_H
#define MULTI_TRACE_H

#include "trace.h"
#include <QObject>
#include <QThread>
#include <QList>
#include <QMutex>
#include <QAtomicInt>
#include <QFuture>
#include <QtConcurrent>
#include <QVector>
#include <QSet>
#include <QDateTime>

struct RoutingResult {
    int fromPadId;
    int toPadId;
    QList<GridPoint> path;
    bool success;
    int layerUsed;  // Основной слой, на котором проложена трасса
    int connIndex;  // Индекс соединения

    RoutingResult() : success(false), layerUsed(-1), connIndex(-1) {}
};

struct ConnectionRequest {
    int fromPadId;
    int toPadId;
    GridPoint start;
    GridPoint end;
    int priority;  // Приоритет (меньше = выше)
};

struct LayerStats {
    int layerId;
    int connectionsAssigned;
    int connectionsCompleted;
    int cellsOccupied;

    LayerStats(int id = 0) : layerId(id),
                           connectionsAssigned(0),
                           connectionsCompleted(0),
                           cellsOccupied(0) {}
};

// Класс для безопасного доступа к сетке из разных потоков
class ThreadSafeGrid {
private:
    GridCell*** grid;
    int boardWidth;
    int boardHeight;
    int totalLayers;
    QMutex** layerMutexes;  // Отдельный мьютекс для каждого слоя

public:
    ThreadSafeGrid(GridCell*** grid, int width, int height, int layers);
    ~ThreadSafeGrid();

    GridCell*** getGrid() const { return grid; }

    // Безопасный доступ к ячейке
    GridCell readCell(int x, int y, int layer) const;
    bool writeCell(int x, int y, int layer, const GridCell& newValue);

    // Попытка размещения трассы (атомарная операция)
    bool tryPlaceTrace(const QList<GridPoint>& path, int traceId);

    // Проверка возможности размещения
    bool canPlacePath(const QList<GridPoint>& path, int fromPadId, int toPadId) const;

    // Получение мьютекса для слоя
    QMutex* getLayerMutex(int layer) const;

    // Статистика
    int getLayerOccupancy(int layer) const;
    int getLayerDifficulty(int layer) const;  // "Сложность" слоя

    int getWidth() const { return boardWidth; }
    int getHeight() const { return boardHeight; }
    int getTotalLayers() const { return totalLayers; }
};

// Поток для трассировки на конкретном слое
class LayerRoutingThread : public QThread {
    Q_OBJECT

public:
    LayerRoutingThread(int layerId,
                      ThreadSafeGrid* grid,
                      PathFinder* pathFinder,
                      QObject* parent = nullptr);
    ~LayerRoutingThread();

    void setConnections(const QList<ConnectionRequest>& connections);
    void stop();

    // Статистика
    LayerStats getStats() const;
    bool isIdle() const;

signals:
    void pathFound(const RoutingResult& result);
    void routingFailed(int fromPadId, int toPadId);
    void threadIdle(int layerId);
    void routingComplete();

protected:
    void run() override;

private:
    int layerId;
    ThreadSafeGrid* grid;
    PathFinder* pathFinder;
    QList<ConnectionRequest> connections;
    mutable QMutex connectionsMutex;
    QAtomicInt running;
    LayerStats stats;

    // Внутренние методы
    RoutingResult findPathForConnection(const ConnectionRequest& conn);
    QList<GridPoint> findPathSingleLayer(const GridPoint& start, const GridPoint& end,
                                        int fromPadId, int toPadId);
};

// Менеджер размещения найденных трасс
class PlacementManager : public QObject {
    Q_OBJECT

public:
    PlacementManager(ThreadSafeGrid* grid, QObject* parent = nullptr);

    void setMaxRetries(int retries) { maxRetries = retries; }

public slots:
    void onPathFound(const RoutingResult& result);
    void scheduleRetry(const RoutingResult& failedResult);

signals:
    void placementCompleted(const RoutingResult& result);
    void placementFailed(int fromPadId, int toPadId);
    void retryRequested(const RoutingResult& result);

private:
    ThreadSafeGrid* grid;
    QMutex placementMutex;
    int maxRetries;

    struct RetryInfo {
        RoutingResult result;
        int retryCount;
        QDateTime nextRetryTime;
    };

    QMap<QString, RetryInfo> retryQueue;  // key = "fromId-toId"

    bool tryPlacePath(const QList<GridPoint>& path, int traceId);
    void processRetryQueue();
};

// Главный координатор многопоточной трассировки
class MultiThreadedRouter : public QObject {
    Q_OBJECT

public:
    MultiThreadedRouter(GridCell*** grid,
                       int boardWidth, int boardHeight, int layerCount,
                       const QList<ConnectionRequest>& connections,
                       QObject* parent = nullptr);
    ~MultiThreadedRouter();

    void setLayerThreadCount(int threadsPerLayer = 1);
    void setPlacementThreads(int count = 1);

    void startRouting();
    void stopRouting();
    bool isRunning() const;

    // Результаты
    QList<RoutingResult> getSuccessfulResults();
    QList<QPair<int, int>> getFailedConnections();
    QMap<int, LayerStats> getLayerStatistics() const;

signals:
    void progressChanged(int percent);
    void routingComplete();
    void errorOccurred(const QString& message);

private slots:
    void onLayerThreadIdle(int layerId);
    void onPlacementCompleted(const RoutingResult& result);
    void onPlacementFailed(int fromPadId, int toPadId);

    void onPathFoundFromThread(const RoutingResult& result);
       void onRoutingFailedFromThread(int fromPadId, int toPadId);

       void connectSignals();

private:
       PathFinder* pathFinder;
       PlacementManager* placementManager;
       int totalConnections;

       // Добавьте этот метод
       void checkCompletion();

    // Конфигурация
    int boardWidth;
    int boardHeight;
    int layerCount;
    int threadsPerLayer;

    // Данные
    ThreadSafeGrid* safeGrid;
    QList<ConnectionRequest> allConnections;
    QList<RoutingResult> successfulResults;
    QList<QPair<int, int>> failedConnections;

    // Потоки
    QVector<LayerRoutingThread*> layerThreads;
    QVector<PlacementManager*> placementManagers;
    QThreadPool placementThreadPool;

    // Синхронизация
    QMutex resultsMutex;
    QAtomicInt activeThreads;
    QAtomicInt totalProcessed;

    // Внутренние методы
    void initializeThreads();
    void distributeConnections();
    void redistributeLoad(int fromLayer, int toLayer);
    void cleanup();

    // Вспомогательные
    int calculateConnectionPriority(const ConnectionRequest& conn) const;
    int selectBestLayerForConnection(const ConnectionRequest& conn) const;
};

#endif // MULTI_TRACE_H
