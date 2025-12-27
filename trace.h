#ifndef TRACE_H
#define TRACE_H

#include <QList>
#include <QPoint>
#include <QColor>
#include <QSet>

// Типы ячеек сетки
enum CellType {
    CELL_EMPTY,
    CELL_OBSTACLE,
    CELL_PAD,
    CELL_TRACE,
    CELL_VIA
};

// Структура ячейки сетки
struct GridCell {
    CellType type;
    int layer;
    int padId; // если это площадка
    int traceId; // если это трасса
    QColor color;
};

// Простая структура для хранения точек сетки
struct GridPoint {
    int x;
    int y;
    int layer;

    GridPoint() : x(0), y(0), layer(0) {}
    GridPoint(int x_, int y_, int layer_ = 0) : x(x_), y(y_), layer(layer_) {}

    bool operator==(const GridPoint& other) const {
        return x == other.x && y == other.y && layer == other.layer;
    }

    bool operator<(const GridPoint& other) const {
        if (layer != other.layer) return layer < other.layer;
        if (y != other.y) return y < other.y;
        return x < other.x;
    }
};

// Структура для узла в алгоритме Хейса
struct HayesNode {
    GridPoint point;
    int cost;
    int heuristic;
    HayesNode* parent;

    HayesNode(const GridPoint& p, int c, int h, HayesNode* par = nullptr)
        : point(p), cost(c), heuristic(h), parent(par) {}

    int totalCost() const { return cost + heuristic; }
};

// Класс для сравнения узлов в очереди приоритетов
struct CompareHayesNode {
    bool operator()(const HayesNode* a, const HayesNode* b) const {
        return a->totalCost() > b->totalCost();
    }
};

class PathFinder
{
public:
    PathFinder();

    // Основная функция поиска пути (многослойная трассировка Хейса)
    QList<GridPoint> findPath(const GridPoint& start, const GridPoint& end,
                             GridCell*** grid, int boardWidth, int boardHeight, int totalLayers,
                             int fromPadId, int toPadId);  // ДВА параметра вместо одного

    // Проверка возможности размещения трассы
    bool canPlaceTrace(int x, int y, int layer, GridCell*** grid,
                      int boardWidth, int boardHeight, int fromPadId, int toPadId);  // ДВА параметра

    // Получение стоимости перехода между ячейками
    int getTransitionCost(const GridPoint& from, const GridPoint& to,
                         GridCell*** grid, int fromPadId, int toPadId);

    // Эвристическая функция (Манхэттенское расстояние с учетом слоев)
    int heuristic(const GridPoint& a, const GridPoint& b);

    // Получение соседей для алгоритма Хейса
    QList<GridPoint> getNeighbors(const GridPoint& point, GridCell*** grid,
                                 int boardWidth, int boardHeight, int totalLayers,
                                 int fromPadId, int toPadId);

private:
    // Направления для поиска (4-связность в плоскости + переходы между слоями)
    static const int dx[4];
    static const int dy[4];
};

#endif // TRACE_H
