// multi_trace.h
#ifndef MULTI_TRACE_H
#define MULTI_TRACE_H

#include <QObject>
#include <QList>
#include <QPair>
#include <QMutex>
#include <QThreadPool>
#include "trace.h"

// Запрос на трассировку одного соединения
struct ConnectionRequest {
    int fromPadId;
    int toPadId;
    GridPoint start;
    GridPoint end;
    int priority; // для сортировки
};

// Результат трассировки одного соединения
struct RoutingResult {
    int connIndex;   // индекс в исходном списке запросов
    int fromPadId;
    int toPadId;
    bool success;
    QList<GridPoint> path;
    int layerUsed;   // слой, на котором проложена трасса (можно использовать первый слой пути)
};

class MultiThreadedRouter : public QObject
{
    Q_OBJECT

public:
    MultiThreadedRouter(GridCell*** grid, int boardWidth, int boardHeight, int layerCount,
                        const QList<ConnectionRequest>& requests, QObject* parent = nullptr);
    ~MultiThreadedRouter();

    void setLayerThreadCount(int count); // устанавливает максимальное количество потоков
    void startRouting();
mutable QMutex mutex;
    QList<RoutingResult> getSuccessfulResults() const;
    QList<QPair<int,int>> getFailedConnections() const; // пары fromPadId,toPadId

signals:
    void progressChanged(int percent);
    void routingComplete();
    void errorOccurred(const QString& message);

public slots:
    void addResult(const RoutingResult& result); // вызывается из задач

private:
    GridCell*** grid;           // глобальная сетка
    int boardWidth;
    int boardHeight;
    int layerCount;
    QList<ConnectionRequest> requests;
          // для синхронизации доступа к результатам и глобальной сетке
    QThreadPool threadPool;
    bool isRunning;
    int totalRequests;
    int completedRequests;
    QList<RoutingResult> successfulResults;
    QList<QPair<int,int>> failedConnections;
    QList<int> failedIndices;   // индексы неудачных запросов
};

#endif // MULTI_TRACE_H
