#include "trace.h"
#include <queue>
#include <vector>
#include <unordered_map>
#include <climits>
#include <QDebug>

// Направления для поиска (4-связность)
const int PathFinder::dx[4] = {1, -1, 0, 0};
const int PathFinder::dy[4] = {0, 0, 1, -1};

PathFinder::PathFinder()
{
}

// Вспомогательная функция для вычисления хэша GridPoint
size_t hashGridPoint(const GridPoint& p) {
    return ((static_cast<size_t>(p.layer) * 1000 + static_cast<size_t>(p.y)) * 1000 + static_cast<size_t>(p.x));
}

// Структура для сравнения GridPoint
struct GridPointHash {
    size_t operator()(const GridPoint& p) const {
        return hashGridPoint(p);
    }
};

struct GridPointEqual {
    bool operator()(const GridPoint& a, const GridPoint& b) const {
        return a.x == b.x && a.y == b.y && a.layer == b.layer;
    }
};

QList<GridPoint> PathFinder::findPath(const GridPoint& start, const GridPoint& end,
                                     GridCell*** grid, int boardWidth, int boardHeight, int totalLayers,
                                     int fromPadId, int toPadId)
{
    qDebug() << "=== НАЧАЛО ПОИСКА ПУТИ ===";
    qDebug() << "От: (" << start.x << "," << start.y << "," << start.layer << ")";
    qDebug() << "До: (" << end.x << "," << end.y << "," << end.layer << ")";
    qDebug() << "FromPadID:" << fromPadId << "ToPadID:" << toPadId;
    qDebug() << "Размер платы:" << boardWidth << "x" << boardHeight;
    qDebug() << "Слоев:" << totalLayers;

    // Проверяем границы
    if (start.x < 0 || start.x >= boardWidth || start.y < 0 || start.y >= boardHeight ||
        start.layer < 0 || start.layer >= totalLayers) {
        qDebug() << "ОШИБКА: Стартовая точка вне границ!";
        return QList<GridPoint>();
    }

    if (end.x < 0 || end.x >= boardWidth || end.y < 0 || end.y >= boardHeight ||
        end.layer < 0 || end.layer >= totalLayers) {
        qDebug() << "ОШИБКА: Конечная точка вне границ!";
        return QList<GridPoint>();
    }

    // Специальная обработка для стартовой и конечной точек (площадок)
    GridCell& startCell = grid[start.layer][start.y][start.x];
    GridCell& endCell = grid[end.layer][end.y][end.x];

    qDebug() << "Стартовая ячейка - тип:" << startCell.type << "padId:" << startCell.padId;
    qDebug() << "Конечная ячейка - тип:" << endCell.type << "padId:" << endCell.padId;

    // Стартовая точка должна быть площадкой
    if (startCell.type != CELL_PAD) {
        qDebug() << "ОШИБКА: Стартовая точка не является площадкой!";
        return QList<GridPoint>();
    }

    // Конечная точка должна быть площадкой
    if (endCell.type != CELL_PAD) {
        qDebug() << "ОШИБКА: Конечная точка не является площадкой!";
        return QList<GridPoint>();
    }

    // ВАЖНО: Используем fromPadId как идентификатор трассы для этого соединения
    // Но разрешаем доступ к обеим площадкам (fromPadId и toPadId)
    int traceId = fromPadId;

    // Алгоритм A* для многослойной трассировки
    std::priority_queue<HayesNode*, std::vector<HayesNode*>, CompareHayesNode> openSet;

    // Используем unordered_map для gScore и cameFrom
    std::unordered_map<GridPoint, int, GridPointHash, GridPointEqual> gScore;
    std::unordered_map<GridPoint, GridPoint, GridPointHash, GridPointEqual> cameFrom;

    // Инициализация начального узла
    gScore[start] = 0;
    HayesNode* startNode = new HayesNode(start, 0, heuristic(start, end));
    openSet.push(startNode);

    // Для отслеживания всех созданных узлов
    std::vector<HayesNode*> allNodes;
    allNodes.push_back(startNode);

    int iterations = 0;
    const int MAX_ITERATIONS = 10000; // Защита от бесконечного цикла

    while (!openSet.empty() && iterations < MAX_ITERATIONS) {
        iterations++;

        HayesNode* current = openSet.top();
        openSet.pop();

        GridPoint currentPoint = current->point;

        // Если достигли конечной точки (другой площадки)
        if (currentPoint.x == end.x && currentPoint.y == end.y) {
            qDebug() << "НАЙДЕН ПУТЬ! Итераций:" << iterations;

            // Восстанавливаем путь
            QList<GridPoint> path;
            GridPoint node = currentPoint;

            // Восстанавливаем путь от конца к началу
            path.prepend(node);
            while (!(node.x == start.x && node.y == start.y && node.layer == start.layer)) {
                auto it = cameFrom.find(node);
                if (it != cameFrom.end()) {
                    node = it->second;
                    path.prepend(node);
                } else {
                    qDebug() << "ОШИБКА: Не удалось восстановить путь!";
                    break;
                }
            }

            qDebug() << "Путь восстановлен, длина:" << path.size();

            // Очистка памяти
            for (HayesNode* n : allNodes) {
                delete n;
            }

            qDebug() << "=== ПОИСК ЗАВЕРШЕН УСПЕШНО ===";
            return path;
        }

        // Получаем соседей
        QList<GridPoint> neighbors = getNeighbors(currentPoint, grid, boardWidth, boardHeight,
                                                 totalLayers, fromPadId, toPadId);

        for (const GridPoint& neighbor : neighbors) {
            int tentativeGScore = gScore[currentPoint] +
                                getTransitionCost(currentPoint, neighbor, grid, fromPadId, toPadId);

            auto it = gScore.find(neighbor);
            if (it == gScore.end() || tentativeGScore < it->second) {
                // Этот путь лучше, чем предыдущий
                cameFrom[neighbor] = currentPoint;
                gScore[neighbor] = tentativeGScore;

                HayesNode* neighborNode = new HayesNode(neighbor, tentativeGScore,
                                                      heuristic(neighbor, end), current);
                openSet.push(neighborNode);
                allNodes.push_back(neighborNode);
            }
        }
    }

    if (iterations >= MAX_ITERATIONS) {
        qDebug() << "ПРЕРВАНО: Превышено максимальное количество итераций!";
    } else {
        qDebug() << "Путь не найден. Итераций:" << iterations;
        qDebug() << "Размер openSet:" << openSet.size();
        qDebug() << "Размер gScore:" << gScore.size();
    }

    // Очистка памяти
    for (HayesNode* n : allNodes) {
        delete n;
    }

    qDebug() << "=== ПОИСК ЗАВЕРШЕН БЕЗ УСПЕХА ===";
    return QList<GridPoint>();
}

bool PathFinder::canPlaceTrace(int x, int y, int layer, GridCell*** grid,
                              int boardWidth, int boardHeight, int fromPadId, int toPadId)
{
    // Проверяем границы
    if (x < 0 || x >= boardWidth || y < 0 || y >= boardHeight ||
        layer < 0 || layer >= boardHeight) {
        return false;
    }

    GridCell& cell = grid[layer][y][x];

    // Для площадок: разрешаем доступ ТОЛЬКО к площадкам этого соединения
    if (cell.type == CELL_PAD) {
        return (cell.padId == fromPadId || cell.padId == toPadId);
    }

    // Для остальных типов ячеек
    switch (cell.type) {
    case CELL_EMPTY:
    case CELL_VIA:
        return true;

    case CELL_TRACE:
        // Можно пройти через трассу, только если она принадлежит тому же соединению
        // Используем fromPadId как идентификатор трассы
        return (cell.traceId == fromPadId || cell.traceId == -1);

    case CELL_OBSTACLE:
        return false;

    default:
        return false;
    }
}

int PathFinder::getTransitionCost(const GridPoint& from, const GridPoint& to,
                                 GridCell*** grid, int fromPadId, int toPadId)
{
    int baseCost = 10;

    GridCell& toCell = grid[to.layer][to.y][to.x];

    // Дополнительная стоимость за смену слоя (переход через via)
    if (from.layer != to.layer) {
        baseCost += 30; // Высокая стоимость за переход между слоями
    }

    // Дополнительная стоимость за проход через via
    if (toCell.type == CELL_VIA) {
        baseCost += 10;
    }

    // Дешевле идти по пустым ячейкам
    if (toCell.type == CELL_EMPTY) {
        baseCost -= 2;
    }

    // Еще дешевле идти по тем же трассам
    if (toCell.type == CELL_TRACE && toCell.traceId == fromPadId) {
        baseCost -= 5;
    }

    return baseCost;
}

int PathFinder::heuristic(const GridPoint& a, const GridPoint& b)
{
    // Манхэттенское расстояние + стоимость смены слоев
    int manhattan = abs(a.x - b.x) + abs(a.y - b.y);
    int layerDiff = abs(a.layer - b.layer) * 30; // Умножаем на 30, т.к. смена слоя дорогая

    return manhattan * 10 + layerDiff;
}

QList<GridPoint> PathFinder::getNeighbors(const GridPoint& point, GridCell*** grid,
                                         int boardWidth, int boardHeight, int totalLayers,
                                         int fromPadId, int toPadId)
{
    QList<GridPoint> neighbors;

    // Соседи в той же плоскости (4 направления)
    for (int i = 0; i < 4; i++) {
        int nx = point.x + dx[i];
        int ny = point.y + dy[i];

        if (nx >= 0 && nx < boardWidth && ny >= 0 && ny < boardHeight) {
            if (canPlaceTrace(nx, ny, point.layer, grid, boardWidth, boardHeight, fromPadId, toPadId)) {
                neighbors.append(GridPoint(nx, ny, point.layer));
            }
        }
    }

    // Соседи на других слоях (вертикальные переходы через via)
    // Разрешаем переходы на все слои
    for (int layer = 0; layer < totalLayers; layer++) {
        if (layer != point.layer) {
            // Проверяем, можно ли перейти на этот слой
            if (canPlaceTrace(point.x, point.y, layer, grid, boardWidth, boardHeight, fromPadId, toPadId)) {
                neighbors.append(GridPoint(point.x, point.y, layer));
            }
        }
    }

    return neighbors;
}
