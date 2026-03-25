#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#ifdef _OPENMP
#include <omp.h>
#endif

#include <QScreen>
#include <QGuiApplication>
#include <QMainWindow>
#include <QGraphicsScene>
#include <QGraphicsRectItem>
#include <QGraphicsLineItem>
#include <QList>
#include <QColor>
#include <QMap>
#include <QGraphicsSceneMouseEvent>
#include <QRadioButton>
#include <QButtonGroup>
#include <QDialog>
#include <QSpinBox>
#include <QElapsedTimer>
#include <QThreadPool>
#include <QRunnable>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QMutex>
#include <QMutexLocker>
#include "trace.h"
#include "helpdialog.h"
#include "multi_trace.h"

namespace Ui {
class MainWindow;
}

struct Pad {
    int id;
    int x;
    int y;
    QString name;
    QColor color;
    QList<int> connections;
};

struct Connection {
    int fromPadId;
    int toPadId;
    bool routed;
    int layer;
    QGraphicsLineItem* visualLine;
};

struct TraceLineInfo {
    QGraphicsLineItem* line;
    int layer;
};

class CustomGraphicsScene : public QGraphicsScene
{
    Q_OBJECT
public:
    explicit CustomGraphicsScene(QObject *parent = nullptr) : QGraphicsScene(parent) {}

signals:
    void cellClicked(int x, int y);

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override;
};

class MultiThreadedRouter;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onHelpClicked();
    void onCellClicked(int x, int y);
    void showFailedConnections();
    void restoreFailedConnectionLines();
    void onSetObstacle();
    void onSetPad();
    void onSetConnection();
    void onRoute();
    void onClearTraces();
    void onRemoveObstacle();
    void onRemovePad();
    void onLayerCountChanged(int count);
    void onBoardSizeChanged();
    void onAutoFill();
    void onRoutingMethodChanged();
    void retryFailedConnections();
    // Слоты для многопоточной трассировки
    void onMultiThreadRoutingProgress(int percent);
    void onMultiThreadRoutingComplete();
    void onMultiThreadRoutingError(const QString& message);

    // Режимы работы
    void setModeObstacle();
    void setModePad();
    void setModeConnection();

    // Обработчик переключения слоев
    void onLayerRadioButtonClicked();

private:
    Ui::MainWindow *ui;
    CustomGraphicsScene *scene;
    QButtonGroup *layerButtonGroup;
    PathFinder pathFinder;
    QList<TraceLineInfo> traceLinesInfo;
    QElapsedTimer routeTimer;

    bool multiThreadRoutingInProgress = false;

    void showRoutingResults(int successCount, int totalCount, int failedCount,
                               const QMap<int, int>& tracesPerLayer);

    // Многопоточные компоненты
    QThreadPool threadPool;
    QList<QFutureWatcher<QList<GridPoint>>*> futureWatchers;
    MultiThreadedRouter* multiThreadRouter;

    // Параметры платы
    int gridSize = 30;
    int boardWidth = 20;
    int boardHeight = 20;
    int layerCount = 2;
    int currentLayer = 0;
    QList<int> failedConnections;

    // Режимы работы
    enum Mode { MODE_NONE, MODE_OBSTACLE, MODE_PAD, MODE_CONNECTION };
    Mode currentMode = MODE_NONE;

    // Метод трассировки
    enum RoutingMethod { SINGLE_THREADED, MULTI_THREADED };
    RoutingMethod currentRoutingMethod = SINGLE_THREADED;

    // Для многопоточной трассировки
    QMutex gridMutex;

    // Для безопасного доступа к сетке
    struct SafeGridAccess {
        GridCell*** grid;
        int boardWidth;
        int boardHeight;
        int layerCount;
        QMutex* mutex;
    } safeGrid;

    // Данные
    QList<Pad> pads;
    QList<Connection> connections;

    // Сетка и визуализация
    GridCell*** grid;
    QGraphicsRectItem*** cells;

    // Идентификаторы
    int nextPadId = 1;
    int selectedPadId = -1;

    // Цвета для слоев
    QList<QColor> layerColors = {
        QColor(255, 0, 0),
        QColor(0, 255, 0),
        QColor(0, 0, 255),
        QColor(255, 255, 0),
        QColor(255, 0, 255),
        QColor(0, 255, 255),
        QColor(255, 165, 0),
        QColor(128, 0, 128)
    };

    // Инициализация и отрисовка
    void initGrid();
    void clearGrid();
    void drawGrid();
    void updateCellDisplay(int x, int y, int layer = -1);
    void drawTraceLine(const GridPoint& from, const GridPoint& to, int layer);
    void drawConnectionLine(int padId1, int padId2);
    void updateConnectionLines();
    void updateTraceLinesForCurrentLayer();

    // Создание элементов UI для слоев
    void createLayerRadioButtons();

    // Авто-заполнение
    void autoFillBoard();
    QList<GridPoint> generateObstacle(int size, int type);

    // Вспомогательные методы для работы с данными
    Pad* getPadById(int id);
    int getPadAt(int x, int y);
    bool hasPadAt(int x, int y);
    void updatePadConnections();
    QColor getLayerColor(int layer, bool isActiveLayer);
    void removeConnectionLines();

    // Методы для работы с трассировкой (однопоточная)
    QList<GridPoint> findPath(const GridPoint& start, const GridPoint& end, int fromPadId, int toPadId);
    void placeVia(int x, int y);
    bool canPlaceTrace(int x, int y, int layer, int fromPadId, int toPadId);

    // Алгоритмы трассировки
    void performSingleThreadedRouting();
    void performMultiThreadedRouting();

    // Методы многопоточной обработки
    void processMultiThreadedResults(const QList<RoutingResult>& results, const QList<Connection>& connectionsToRoute);
    void processRoutedPath(const QList<GridPoint>& path, Connection& conn, Pad* fromPad);
    QList<GridPoint> routeConnection(Pad* fromPad, Pad* toPad);
    void drawTraceLineMultiThreaded(const GridPoint& from, const GridPoint& to, int layer);
    int findLayerForPad(int x, int y);
    void placeTraceSafely(const QList<GridPoint>& path, int fromPadId, int toPadId);
    void processRoutingResultsImproved(const QList<RoutingResult>& results, const QList<Connection>& connectionsToRoute);
    QList<RoutingResult> routeConnectionsParallel(const QList<Connection>& connectionsToRoute);
    void processRoutingResults(const QList<RoutingResult>& results);
    void routeSingleConnection(Connection& conn);

    // Вспомогательные методы для многопоточной трассировки
    int countLayerTransitions(const QList<GridPoint>& path);
    int estimateConnectionComplexity(int x1, int y1, int x2, int y2);
    QList<GridPoint> adjustPathAroundConflict(const QList<GridPoint>& originalPath,
                                             int conflictIndex,
                                             int fromPadId, int toPadId);
    RoutingResult routeSingleConnectionAsync(int connIndex, const Connection& conn);
    bool canPlacePathInTempGrid(const QList<GridPoint>& path,
                               GridCell*** tempGrid,
                               int fromPadId, int toPadId);
    void placePathInTempGrid(const QList<GridPoint>& path,
                            GridCell*** tempGrid,
                            int traceId);
    void copyTempGridToMain(GridCell*** tempGrid);

    // Вспомогательные методы для временной сетки
    GridCell*** createTempGrid();
    void freeTempGrid(GridCell*** tempGrid);

    // Методы для подготовки данных для многопоточной трассировки
    QList<ConnectionRequest> prepareConnectionRequests();
    void applyRoutingResults(const QList<RoutingResult>& results);

    // Метод выбора оптимального слоя
    int findOptimalLayer(int x, int y);
};


class BoardSizeDialog : public QDialog
{
    Q_OBJECT
public:
    explicit BoardSizeDialog(QWidget *parent = nullptr, int currentWidth = 10, int currentHeight = 10);
    int getWidth() const { return widthSpinBox->value(); }
    int getHeight() const { return heightSpinBox->value(); }

private:
    QSpinBox *widthSpinBox;
    QSpinBox *heightSpinBox;
};

#endif // MAINWINDOW_H
