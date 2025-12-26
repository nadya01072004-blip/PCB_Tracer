#ifndef MAINWINDOW_H
#define MAINWINDOW_H

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

namespace Ui {
class MainWindow;
}

// Типы ячеек сетки
enum CellType {
    CELL_EMPTY,
    CELL_OBSTACLE,
    CELL_PAD,
    CELL_TRACE,
    CELL_VIA
};

// Структура для представления контактной площадки
struct Pad {
    int id;
    int x;
    int y;
    QString name;
    QColor color;
    QList<int> connections; // ID подключенных площадок
};

// Структура для представления связи
struct Connection {
    int fromPadId;
    int toPadId;
    bool routed;
    int layer;
    QGraphicsLineItem* visualLine; // Визуализация связи
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

    GridPoint() : x(0), y(0) {}
    GridPoint(int x_, int y_) : x(x_), y(y_) {}

    bool operator==(const GridPoint& other) const {
        return x == other.x && y == other.y;
    }
};

class CustomGraphicsScene : public QGraphicsScene
{
    Q_OBJECT
public:
    explicit CustomGraphicsScene(QObject *parent = nullptr) : QGraphicsScene(parent) {}

signals:
    void cellClicked(int x, int y);

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent *event);
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onCellClicked(int x, int y);

    // Обработчики кнопок
    void onSetObstacle();
    void onSetPad();
    void onSetConnection();
    void onRoute();
    void onClearTraces();
    void onRemoveObstacle();
    void onRemovePad();
    void onLayerCountChanged(int count);
    void onBoardSizeChanged(); // Для изменения размера платы

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

    // Параметры платы
    int gridSize = 30;
    int boardWidth = 10; // Теперь настраиваемые параметры
    int boardHeight = 15;
    int layerCount = 4;
    int currentLayer = 0;

    // Режимы работы
    enum Mode { MODE_NONE, MODE_OBSTACLE, MODE_PAD, MODE_CONNECTION };
    Mode currentMode = MODE_NONE;

    // Данные
    QList<Pad> pads;
    QList<Connection> connections;
    GridCell*** grid; // 3D массив: [layer][y][x]

    // Визуализация
    QGraphicsRectItem*** cells;
    int nextPadId = 1;
    int selectedPadId = -1;

    // Цвета для слоев
    QList<QColor> layerColors = {
        QColor(255, 0, 0),    // Красный - слой 1
        QColor(0, 255, 0),    // Зеленый - слой 2
        QColor(0, 0, 255),    // Синий - слой 3
        QColor(255, 255, 0),  // Желтый - слой 4
        QColor(255, 0, 255),  // Пурпурный - слой 5
        QColor(0, 255, 255),  // Голубой - слой 6
        QColor(255, 165, 0),  // Оранжевый - слой 7
        QColor(128, 0, 128)   // Фиолетовый - слой 8
    };

    // Инициализация
    void initGrid();
    void clearGrid();
    void drawGrid();
    void updateCellDisplay(int x, int y, int layer = -1);
    void drawTraceLine(const GridPoint& from, const GridPoint& to, int layer);
    void drawConnectionLine(int padId1, int padId2); // Рисуем визуальную линию связи
    void updateConnectionLines(); // Обновляем все линии связей

    // Алгоритм трассировки
    QList<GridPoint> findPath(const GridPoint& start, const GridPoint& end, int layer);
    bool canPlaceTrace(int x, int y, int layer);
    void placeVia(int x, int y);

    // Вспомогательные методы
    Pad* getPadById(int id);
    int getPadAt(int x, int y);
    bool hasPadAt(int x, int y);
    void updatePadConnections();

    // Создание элементов UI для слоев
    void createLayerRadioButtons();
};

#endif // MAINWINDOW_H
