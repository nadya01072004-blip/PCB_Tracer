#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "trace.h"
#include <QGraphicsRectItem>
#include <QGraphicsLineItem>
#include <QMessageBox>
#include <QInputDialog>
#include <QBrush>
#include <QPen>
#include <cmath>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSizePolicy>
#include <algorithm>
#include <QDebug>
#include <QDialogButtonBox>
#include <QTime>
#include <QStyle>
#include <QFormLayout>
#include <QScreen>
#include <QGuiApplication>
#include <QDesktopWidget>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QFutureSynchronizer>
#include <QTextBrowser>
#include "routingtask.h"

void CustomGraphicsScene::mousePressEvent(QGraphicsSceneMouseEvent *event)
{
    QPointF scenePos = event->scenePos();
    int x = static_cast<int>(scenePos.x()) / 30;
    int y = static_cast<int>(scenePos.y()) / 30;
    emit cellClicked(x, y);
    QGraphicsScene::mousePressEvent(event);
}

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
        ui(new Ui::MainWindow),
        scene(nullptr),
        layerButtonGroup(new QButtonGroup(this)),
        grid(nullptr),
        cells(nullptr)
{
    ui->setupUi(this);

    threadPool.setMaxThreadCount(QThread::idealThreadCount());

    currentRoutingMethod = SINGLE_THREADED; // По умолчанию однопоточный
        ui->singleThreadRadio->setChecked(true);

        threadPool.setMaxThreadCount(QThread::idealThreadCount() / 2); // Используем половину доступных потоков

    // ОПТИМИЗАЦИЯ: Настраиваем пул потоков
       int idealThreads = QThread::idealThreadCount();
       int maxThreads = qMin(4, qMax(2, idealThreads)); // 2-4 потока максимум
       threadPool.setMaxThreadCount(maxThreads);
       threadPool.setExpiryTimeout(10000); // 10 секунд таймаут для потоков

       qDebug() << "Пул потоков настроен на" << maxThreads << "потоков";

    // Инициализируем safeGrid (пока grid еще nullptr, обновим после initGrid)
    safeGrid.grid = nullptr;
    safeGrid.boardWidth = boardWidth;
    safeGrid.boardHeight = boardHeight;
    safeGrid.layerCount = layerCount;
    safeGrid.mutex = &gridMutex;

    // Настройка пула потоков
    threadPool.setMaxThreadCount(QThread::idealThreadCount());

    // Исправляем проблему с диалоговыми окнами
    this->setAttribute(Qt::WA_DeleteOnClose);

    // Устанавливаем начальное количество слоев на 2
    layerCount = 2;
    ui->layerSpinBox->setValue(layerCount);

    // Создаем кастомную сцену
    scene = new CustomGraphicsScene(this);
    ui->graphicsView->setScene(scene);

    // Создаем радио-кнопки для активного слоя
    createLayerRadioButtons();

    // Подключаем сигнал от кастомной сцены
    connect(scene, &CustomGraphicsScene::cellClicked,
            this, &MainWindow::onCellClicked);
    // Подключаем радиокнопки методов трассировки
        connect(ui->singleThreadRadio, &QRadioButton::toggled, this, &MainWindow::onRoutingMethodChanged);
        connect(ui->multiThreadRadio, &QRadioButton::toggled, this, &MainWindow::onRoutingMethodChanged);
    connect(ui->helpBtn, &QPushButton::clicked, this, &MainWindow::onHelpClicked);
    // Подключение сигналов кнопок
    connect(ui->obstacleBtn, &QPushButton::clicked, this, &MainWindow::setModeObstacle);
    connect(ui->padBtn, &QPushButton::clicked, this, &MainWindow::setModePad);
    connect(ui->connectionBtn, &QPushButton::clicked, this, &MainWindow::setModeConnection);
    connect(ui->routeBtn, &QPushButton::clicked, this, &MainWindow::onRoute);
    connect(ui->clearBtn, &QPushButton::clicked, this, &MainWindow::onClearTraces);
    connect(ui->removeObstacleBtn, &QPushButton::clicked, this, &MainWindow::onRemoveObstacle);
    connect(ui->removePadBtn, &QPushButton::clicked, this, &MainWindow::onRemovePad);
    connect(ui->boardSizeBtn, &QPushButton::clicked, this, &MainWindow::onBoardSizeChanged);
    connect(ui->layerSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::onLayerCountChanged);
    connect(ui->autoFillBtn, &QPushButton::clicked, this, &MainWindow::onAutoFill);

    // Инициализация сетки - теперь scene уже создана
    initGrid();
    drawGrid();
}

MainWindow::~MainWindow()
{
    // Отменяем все ожидающие задачи
       threadPool.clear();

       // Ждем завершения всех потоков
       threadPool.waitForDone();

       // Удаляем watchers
       for (QFutureWatcher<QList<GridPoint>>* watcher : futureWatchers) {
           watcher->waitForFinished();
           delete watcher;
       }

       // Обнуляем safeGrid перед удалением grid
       safeGrid.grid = nullptr;

    // Удаляем радио-кнопки
    QList<QAbstractButton*> buttons = layerButtonGroup->buttons();
    for (QAbstractButton* btn : buttons) {
        layerButtonGroup->removeButton(btn);
        delete btn;
    }
    delete layerButtonGroup;

    // Очищаем сетку ПЕРЕД удалением UI
    clearGrid();

    delete ui;
}

// Добавьте реализацию недостающих методов:
void MainWindow::drawTraceLineMultiThreaded(const GridPoint& from, const GridPoint& to, int layer) {
    // Просто вызываем обычный метод
    drawTraceLine(from, to, layer);
}

int MainWindow::findLayerForPad(int x, int y) {
    for (int l = 0; l < layerCount; l++) {
        if (grid[l][y][x].type == CELL_PAD) {
            return l;
        }
    }
    return 0;
}

void MainWindow::placeTraceSafely(const QList<GridPoint>& path, int fromPadId, int toPadId) {
    QMutexLocker locker(&gridMutex);

    for (int i = 0; i < path.size(); i++) {
        const GridPoint& point = path[i];

        if (point.x < 0 || point.x >= boardWidth ||
            point.y < 0 || point.y >= boardHeight ||
            point.layer < 0 || point.layer >= layerCount) {
            continue;
        }

        GridCell& cell = grid[point.layer][point.y][point.x];

        if (cell.type == CELL_PAD) continue;

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
        cell.traceId = fromPadId;
    }

    for (Connection& conn : connections) {
        if (conn.fromPadId == fromPadId && conn.toPadId == toPadId) {
            conn.routed = true;
            conn.layer = path.first().layer;
            break;
        }
    }
}

void MainWindow::processMultiThreadedResults(const QList<RoutingResult>& results,
                                            const QList<Connection>& connectionsToRoute)
{
    int routedCount = 0;
    failedConnections.clear();

    qDebug() << "=== ОБРАБОТКА РЕЗУЛЬТАТОВ МНОГОПОТОЧНОЙ ТРАССИРОВКИ ===";
    qDebug() << "Получено результатов:" << results.size();

    // Сортируем результаты по качеству пути (сначала короткие)
    QList<RoutingResult> sortedResults = results;
    std::sort(sortedResults.begin(), sortedResults.end(),
              [](const RoutingResult& a, const RoutingResult& b) {
                  if (a.path.size() != b.path.size()) {
                      return a.path.size() < b.path.size();
                  }

                  // Если длина одинаковая, считаем количество переходов между слоями
                  int aTransitions = 0;
                  int bTransitions = 0;

                  if (!a.path.isEmpty()) {
                      int currentLayer = a.path[0].layer;
                      for (int i = 1; i < a.path.size(); i++) {
                          if (a.path[i].layer != currentLayer) {
                              aTransitions++;
                              currentLayer = a.path[i].layer;
                          }
                      }
                  }

                  if (!b.path.isEmpty()) {
                      int currentLayer = b.path[0].layer;
                      for (int i = 1; i < b.path.size(); i++) {
                          if (b.path[i].layer != currentLayer) {
                              bTransitions++;
                              currentLayer = b.path[i].layer;
                          }
                      }
                  }

                  return aTransitions < bTransitions;
              });

    // Последовательно размещаем трассы (чтобы избежать конфликтов)
    for (const RoutingResult& result : sortedResults) {
        if (!result.success) {
            qDebug() << "Соединение не найдено";
            failedConnections.append(result.fromPadId);
            continue;
        }

        // Находим соответствующее соединение в основном списке
        Connection* targetConn = nullptr;
        for (Connection& conn : connections) {
            if (conn.fromPadId == result.fromPadId && conn.toPadId == result.toPadId) {
                targetConn = &conn;
                break;
            }
        }

        if (!targetConn || targetConn->routed) {
            continue;
        }

        Pad* fromPad = getPadById(result.fromPadId);
        Pad* toPad = getPadById(result.toPadId);

        if (!fromPad || !toPad) {
            continue;
        }

        qDebug() << "Попытка разместить трассу для" << fromPad->name << "->" << toPad->name
                 << " (длина:" << result.path.size() << ")";

        // Проверяем возможность размещения с учетом уже размещенных трасс
        bool canPlace = true;
        {
            QMutexLocker locker(&gridMutex);

            for (int i = 0; i < result.path.size(); i++) {
                const GridPoint& point = result.path[i];

                // Пропускаем площадки
                if ((point.x == fromPad->x && point.y == fromPad->y) ||
                    (point.x == toPad->x && point.y == toPad->y)) {
                    continue;
                }

                if (point.x < 0 || point.x >= boardWidth ||
                    point.y < 0 || point.y >= boardHeight ||
                    point.layer < 0 || point.layer >= layerCount) {
                    canPlace = false;
                    break;
                }

                GridCell& cell = grid[point.layer][point.y][point.x];

                // Если ячейка уже занята другой трассой
                if ((cell.type == CELL_TRACE || cell.type == CELL_VIA) &&
                    cell.traceId != fromPad->id) {
                    canPlace = false;
                    break;
                }

                // Также проверяем препятствия
                if (cell.type == CELL_OBSTACLE) {
                    canPlace = false;
                    break;
                }
            }

            if (canPlace) {
                // Размещаем трассу
                for (int i = 0; i < result.path.size(); i++) {
                    const GridPoint& point = result.path[i];

                    // Пропускаем площадки
                    if ((point.x == fromPad->x && point.y == fromPad->y) ||
                        (point.x == toPad->x && point.y == toPad->y)) {
                        continue;
                    }

                    GridCell& cell = grid[point.layer][point.y][point.x];

                    // Определяем, является ли это VIA
                    bool isVia = false;
                    if (i > 0) {
                        const GridPoint& prev = result.path[i - 1];
                        if (prev.x == point.x && prev.y == point.y &&
                            prev.layer != point.layer) {
                            isVia = true;
                        }
                    }

                    if (isVia) {
                        cell.type = CELL_VIA;
                        cell.color = Qt::black; // VIA черного цвета
                    } else {
                        cell.type = CELL_TRACE;
                        cell.color = Qt::white;
                    }
                    cell.traceId = fromPad->id;
                }

                // Обновляем соединение
                targetConn->routed = true;
                targetConn->layer = result.path.first().layer;

                // Скрываем линию связи
                if (targetConn->visualLine) {
                    targetConn->visualLine->hide();
                }

                routedCount++;

                // Рисуем линии трасс
                for (int i = 0; i < result.path.size() - 1; i++) {
                    drawTraceLine(result.path[i], result.path[i + 1], result.path[i].layer);
                }

                qDebug() << "Трасса успешно размещена";
            } else {
                qDebug() << "Невозможно разместить трассу (конфликт)";
                failedConnections.append(fromPad->id);
            }
        }

        // Обновляем GUI между трассами
        drawGrid();
        QApplication::processEvents();
    }

    qDebug() << "=== ОБРАБОТКА ЗАВЕРШЕНА ===";
    qDebug() << "Успешно проложено:" << routedCount << "из" << connectionsToRoute.size();
    qDebug() << "Неудачные соединения:" << failedConnections.size();
}

void MainWindow::processRoutedPath(const QList<GridPoint>& path, Connection& conn, Pad* fromPad)
{
    if (path.isEmpty()) {
        return;
    }

    QMutexLocker locker(&gridMutex);

    for (int i = 0; i < path.size(); i++) {
        const GridPoint& point = path[i];

        if (point.x < 0 || point.x >= boardWidth ||
            point.y < 0 || point.y >= boardHeight ||
            point.layer < 0 || point.layer >= layerCount) {
            continue;
        }

        GridCell& cell = grid[point.layer][point.y][point.x];

        // Пропускаем площадки
        if (cell.type == CELL_PAD) {
            continue;
        }

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
        cell.traceId = fromPad->id;
    }

    conn.routed = true;
    conn.layer = path.first().layer;
}

// ДОБАВЬТЕ ЭТОТ МЕТОД (объявлен в заголовочном файле, но не реализован):
QList<GridPoint> MainWindow::routeConnection(Pad* fromPad, Pad* toPad)
{
    if (!fromPad || !toPad) {
        return QList<GridPoint>();
    }

    // Находим доступные слои для площадок
    int startLayer = 0;
    int endLayer = 0;

    for (int l = 0; l < layerCount; l++) {
        if (grid[l][fromPad->y][fromPad->x].type == CELL_PAD) {
            startLayer = l;
            break;
        }
    }

    for (int l = 0; l < layerCount; l++) {
        if (grid[l][toPad->y][toPad->x].type == CELL_PAD) {
            endLayer = l;
            break;
        }
    }

    GridPoint start(fromPad->x, fromPad->y, startLayer);
    GridPoint end(toPad->x, toPad->y, endLayer);

    QList<GridPoint> path;
    {
        QMutexLocker locker(&gridMutex);
        path = pathFinder.findPath(start, end, grid, boardWidth, boardHeight,
                                  layerCount, fromPad->id, toPad->id);
    }

    return path;
}

void MainWindow::onHelpClicked()
{
    HelpDialog helpDialog(this);
    helpDialog.exec();
}

// Реализация диалога для изменения размера платы
BoardSizeDialog::BoardSizeDialog(QWidget *parent, int currentWidth, int currentHeight)
    : QDialog(parent)
{
    setWindowTitle("Размер платы");
    setModal(true);

    // Создаем layout
    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Форма для ввода размеров
    QFormLayout *formLayout = new QFormLayout();

    widthSpinBox = new QSpinBox(this);
    widthSpinBox->setRange(5, 100);
    widthSpinBox->setValue(currentWidth);
    widthSpinBox->setSingleStep(1);
    widthSpinBox->setMinimumWidth(100);

    heightSpinBox = new QSpinBox(this);
    heightSpinBox->setRange(5, 100);
    heightSpinBox->setValue(currentHeight);
    heightSpinBox->setSingleStep(1);
    heightSpinBox->setMinimumWidth(100);

    formLayout->addRow("Ширина (клеток):", widthSpinBox);
    formLayout->addRow("Высота (клеток):", heightSpinBox);

    mainLayout->addLayout(formLayout);

    // Кнопки OK/Cancel
    QDialogButtonBox *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        Qt::Horizontal,
        this
    );

    mainLayout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Устанавливаем фиксированный размер
    setFixedSize(300, 150);
}

void MainWindow::onRoutingMethodChanged()
{
    if (ui->singleThreadRadio->isChecked()) {
        currentRoutingMethod = SINGLE_THREADED;
        ui->statusBar->showMessage("Выбран однопоточный метод трассировки");
    } else {
        currentRoutingMethod = MULTI_THREADED;
        ui->statusBar->showMessage("Выбран многопоточный метод трассировки");
    }
}

void MainWindow::createLayerRadioButtons()
{
    // Получаем группу из UI (она теперь напрямую доступна через ui)
    QGroupBox *layerGroupBox = ui->activeLayerGroupBox;

    if (!layerGroupBox) {
        qDebug() << "Ошибка: группа activeLayerGroupBox не найдена!";
        return;
    }

    // Очищаем предыдущие кнопки
    QList<QAbstractButton*> buttons = layerButtonGroup->buttons();
    for (QAbstractButton* btn : buttons) {
        layerButtonGroup->removeButton(btn);
        delete btn;
    }

    // Очищаем layout группы
    QLayout* layout = layerGroupBox->layout();
    if (layout) {
        QLayoutItem* item;
        while ((item = layout->takeAt(0)) != nullptr) {
            if (item->widget()) {
                delete item->widget();
            }
            delete item;
        }
    } else {
        // Создаем новый layout если его нет
        QVBoxLayout *layerLayout = new QVBoxLayout(layerGroupBox);
        layerLayout->setContentsMargins(10, 20, 10, 10);
        layerLayout->setSpacing(8);
        layout = layerLayout;
    }

    // Создаем радио-кнопки для каждого слоя
    for (int i = 0; i < 8; i++) {
        QRadioButton *radioBtn = new QRadioButton(QString("Слой %1").arg(i + 1), layerGroupBox);
        radioBtn->setMinimumHeight(30);
        radioBtn->setMinimumWidth(150);

        // Устанавливаем цвет текста в зависимости от доступности
        if (i < layerCount) {
            // Для доступных слоев - цвет слоя
            if (i < layerColors.size()) {
                QString colorName = layerColors[i].name();
                QString style = QString("QRadioButton { color: %1; font-weight: %2; }")
                    .arg(colorName)
                    .arg(i == currentLayer ? "bold" : "normal");
                radioBtn->setStyleSheet(style);
            }
            radioBtn->setEnabled(true);
        } else {
            // Для недоступных слоев - серый цвет и отключено
            radioBtn->setStyleSheet("QRadioButton { color: #A0A0A0; }");
            radioBtn->setEnabled(false);
        }

        layerButtonGroup->addButton(radioBtn, i);
        layout->addWidget(radioBtn);
    }

    QSpacerItem* spacer = new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding);
    layout->addItem(spacer);

    // Выбираем текущий слой, если он доступен
    if (currentLayer >= 0 && currentLayer < layerCount) {
        QAbstractButton* btn = layerButtonGroup->button(currentLayer);
        if (btn) {
            btn->setChecked(true);
        }
    } else {
        // Или первый доступный слой
        for (int i = 0; i < layerCount; i++) {
            QAbstractButton* btn = layerButtonGroup->button(i);
            if (btn && btn->isEnabled()) {
                btn->setChecked(true);
                currentLayer = i;
                break;
            }
        }
    }

    // Подключаем сигнал изменения выбора (если еще не подключен)
    if (!layerButtonGroup->signalsBlocked()) {
        connect(layerButtonGroup, QOverload<QAbstractButton *>::of(&QButtonGroup::buttonClicked),
                this, &MainWindow::onLayerRadioButtonClicked);
    }
}

void MainWindow::onLayerRadioButtonClicked()
{
    int newLayer = layerButtonGroup->checkedId();
    if (newLayer >= 0 && newLayer < layerCount && newLayer != currentLayer) {
        currentLayer = newLayer;

        // Обновляем существующие линии трасс
        updateTraceLinesForCurrentLayer();

        drawGrid(); // Обновляем отображение ячеек
        ui->statusBar->showMessage(QString("Текущий слой: %1").arg(currentLayer + 1));
    }
}

void MainWindow::updateTraceLinesForCurrentLayer()
{
    // Обновляем все линии трасс в списке
    for (TraceLineInfo& info : traceLinesInfo) {
        if (info.line) {
            int lineLayer = info.layer;
            QColor newColor = getLayerColor(lineLayer, lineLayer == currentLayer);
            int lineWidth = (lineLayer == currentLayer) ? 4 : 2;
            Qt::PenStyle lineStyle = (lineLayer == currentLayer) ? Qt::SolidLine : Qt::DashLine;
            qreal opacity = (lineLayer == currentLayer) ? 1.0 : 0.6;

            info.line->setPen(QPen(newColor, lineWidth, lineStyle, Qt::RoundCap));
            info.line->setOpacity(opacity);
        }
    }
}

void MainWindow::initGrid()
{
    // Очищаем сцену, если она уже содержит элементы
    if (scene->items().count() > 0) {
        scene->clear();
    }

    // Сохраняем старые размеры перед очисткой
    int oldLayerCount = layerCount;
    int oldBoardHeight = boardHeight;

    // Освобождаем старую память с использованием старых размеров
    if (grid) {
        for (int l = 0; l < oldLayerCount; l++) {
            if (grid[l]) {
                for (int y = 0; y < oldBoardHeight; y++) {
                    if (grid[l][y]) {
                        delete[] grid[l][y];
                    }
                }
                delete[] grid[l];
            }
        }
        delete[] grid;
        grid = nullptr;
    }

    if (cells) {
        for (int y = 0; y < oldBoardHeight; y++) {
            if (cells[y]) {
                delete[] cells[y];
            }
        }
        delete[] cells;
        cells = nullptr;
    }

    // Выделение памяти для 3D сетки с использованием новых размеров
    grid = new GridCell**[static_cast<size_t>(layerCount)];
    for (int l = 0; l < layerCount; l++) {
        grid[l] = new GridCell*[static_cast<size_t>(boardHeight)];
        for (int y = 0; y < boardHeight; y++) {
            grid[l][y] = new GridCell[static_cast<size_t>(boardWidth)];
            for (int x = 0; x < boardWidth; x++) {
                grid[l][y][x] = {CELL_EMPTY, l, -1, -1, Qt::white};
            }
        }
    }

    // Инициализация визуальных ячеек с использованием новых размеров
    cells = new QGraphicsRectItem**[static_cast<size_t>(boardHeight)];
    for (int y = 0; y < boardHeight; y++) {
        cells[y] = new QGraphicsRectItem*[static_cast<size_t>(boardWidth)];
        for (int x = 0; x < boardWidth; x++) {
            QGraphicsRectItem* rect = scene->addRect(
                x * gridSize, y * gridSize, gridSize, gridSize,
                QPen(Qt::black), QBrush(Qt::white)
            );
            rect->setData(0, x);
            rect->setData(1, y);

            // Убираем выделение
            rect->setFlag(QGraphicsItem::ItemIsFocusable, false);
            rect->setFlag(QGraphicsItem::ItemIsSelectable, false);

            cells[y][x] = rect;
        }
    }

    // Обновляем safeGrid
    safeGrid.grid = grid;
    safeGrid.boardWidth = boardWidth;
    safeGrid.boardHeight = boardHeight;
    safeGrid.layerCount = layerCount;
}

void MainWindow::clearGrid()
{
    // Очищаем все линии трасс
    traceLinesInfo.clear(); // Просто очищаем список, элементы уже удалены из сцены

    // Очищаем связи - только данные, визуальные элементы удаляются из сцены
    for (Connection& conn : connections) {
        conn.visualLine = nullptr; // Элементы будут удалены из сцены
    }

    // Освобождаем память сетки
    if (grid) {
        for (int l = 0; l < layerCount; l++) {
            if (grid[l]) {
                for (int y = 0; y < boardHeight; y++) {
                    if (grid[l][y]) {
                        delete[] grid[l][y];
                    }
                }
                delete[] grid[l];
            }
        }
        delete[] grid;
        grid = nullptr;
    }

    // Освобождаем указатели на визуальные ячейки
    if (cells) {
        for (int y = 0; y < boardHeight; y++) {
            if (cells[y]) {
                delete[] cells[y];
            }
        }
        delete[] cells;
        cells = nullptr;
    }

    // Очищаем данные (но оставляем nextPadId)
    pads.clear();
    connections.clear();
    selectedPadId = -1;

    // ВАЖНО: НЕ сбрасываем nextPadId здесь, так как он может понадобиться
    // nextPadId сбрасывается только в onAutoFill()
}

void MainWindow::drawGrid()
{
    if (!grid || !cells || !scene) return;

    for (int y = 0; y < boardHeight; y++) {
        for (int x = 0; x < boardWidth; x++) {
            if (cells[y][x]) {
                updateCellDisplay(x, y);
            }
        }
    }
}

void MainWindow::drawTraceLine(const GridPoint& from, const GridPoint& to, int layer)
{
    if (layer < 0 || layer >= layerCount) return;

    QColor traceColor = getLayerColor(layer, layer == currentLayer);

    // Вычисляем координаты центров ячеек
    qreal x1 = from.x * gridSize + gridSize / 2.0;
    qreal y1 = from.y * gridSize + gridSize / 2.0;
    qreal x2 = to.x * gridSize + gridSize / 2.0;
    qreal y2 = to.y * gridSize + gridSize / 2.0;

    // Определяем толщину и стиль линии
    int lineWidth = (layer == currentLayer) ? 4 : 2;
    Qt::PenStyle lineStyle = (layer == currentLayer) ? Qt::SolidLine : Qt::DashLine;
    qreal opacity = (layer == currentLayer) ? 1.0 : 0.6;

    // Рисуем линию
    QGraphicsLineItem* line = scene->addLine(
        x1, y1, x2, y2,
        QPen(traceColor, lineWidth, lineStyle, Qt::RoundCap)
    );

    // Если это переход между слоями, добавляем маркер VIA
    if (from.layer != to.layer) {
        qreal centerX = (x1 + x2) / 2.0;
        qreal centerY = (y1 + y2) / 2.0;

        // Проверяем, не находится ли точка VIA на площадке
        bool isOnPad = false;
        for (const Pad& pad : pads) {
            // Приблизительная проверка на совпадение координат
            qreal padCenterX = pad.x * gridSize + gridSize / 2.0;
            qreal padCenterY = pad.y * gridSize + gridSize / 2.0;
            qreal distance = QLineF(centerX, centerY, padCenterX, padCenterY).length();

            if (distance < gridSize / 4.0) { // Если VIA слишком близко к центру площадки
                isOnPad = true;
                break;
            }
        }

        // Рисуем VIA только если не на площадке
        if (!isOnPad) {
            // Увеличиваем размер виа-маркера с 6x6 до 10x10
            QGraphicsEllipseItem* viaMarker = scene->addEllipse(
                centerX - 5, centerY - 5, 10, 10,
                QPen(Qt::black, 3),  // Более толстая граница
                QBrush(Qt::black)
            );
            viaMarker->setZValue(10);  // Выше других элементов
        }
    }

    // Сохраняем информацию о линии
    TraceLineInfo info;
    info.line = line;
    info.layer = layer;
    traceLinesInfo.append(info);

    // Устанавливаем прозрачность для неактивных слоев
    if (layer != currentLayer) {
        line->setOpacity(opacity);
    }
}

void MainWindow::drawConnectionLine(int padId1, int padId2)
{
    Pad* pad1 = getPadById(padId1);
    Pad* pad2 = getPadById(padId2);

    if (!pad1 || !pad2) return;

    // Вычисляем координаты центров ячеек
    qreal x1 = pad1->x * gridSize + gridSize / 2.0;
    qreal y1 = pad1->y * gridSize + gridSize / 2.0;
    qreal x2 = pad2->x * gridSize + gridSize / 2.0;
    qreal y2 = pad2->y * gridSize + gridSize / 2.0;

    // Определяем стиль линии
    QPen pen;
    pen.setWidth(1);

    // Всегда используем черный пунктир для линий связей
    // Успешные трассы будут скрыты/удалены после трассировки
    pen.setColor(Qt::black);
    pen.setStyle(Qt::DashLine);

    // Рисуем линию между площадками
    QGraphicsLineItem* line = scene->addLine(x1, y1, x2, y2, pen);
    line->setZValue(5); // Средний уровень (ниже трасс, выше ячеек)

    // Сохраняем ссылку на линию в connection
    for (Connection& conn : connections) {
        if ((conn.fromPadId == padId1 && conn.toPadId == padId2) ||
            (conn.fromPadId == padId2 && conn.toPadId == padId1)) {
            if (conn.visualLine) {
                scene->removeItem(conn.visualLine);
                delete conn.visualLine;
            }
            conn.visualLine = line;
            break;
        }
    }
}

void MainWindow::updateConnectionLines()
{
    // Удаляем старые линии
    QList<QGraphicsItem*> items = scene->items();
    for (QGraphicsItem* item : items) {
        if (QGraphicsLineItem* lineItem = dynamic_cast<QGraphicsLineItem*>(item)) {
            // Проверяем, является ли это линией связи (а не трассой)
            bool isTraceLine = false;
            QPen pen = lineItem->pen();
            if (pen.width() >= 2 || pen.color() != Qt::black || pen.style() != Qt::DashLine) {
                isTraceLine = true;
            }

            if (!isTraceLine) {
                scene->removeItem(lineItem);
                delete lineItem;
            }
        }
    }

    // Обнуляем указатели в connections
    for (Connection& conn : connections) {
        conn.visualLine = nullptr;
    }

    // Рисуем новые линии связей
    for (const Connection& conn : connections) {
        drawConnectionLine(conn.fromPadId, conn.toPadId);
    }
}

void MainWindow::updateCellDisplay(int x, int y, int layer)
{
    if (x < 0 || x >= boardWidth || y < 0 || y >= boardHeight) return;
    if (layer == -1) layer = currentLayer;
    if (layer < 0 || layer >= layerCount) return;

    QGraphicsRectItem* rect = cells[y][x];
    GridCell& cell = grid[layer][y][x];

    QColor color = cell.color;
    QPen pen(Qt::black, 1);

    // Для трасс - ячейки должны оставаться пустыми (белыми)
    if (cell.type == CELL_TRACE) {
        color = Qt::white; // Ячейка остается белой
        pen = QPen(Qt::gray, 1); // Тонкая серая граница
    }
    // УБИРАЕМ специальное отображение для VIA - оставляем ячейки пустыми
    else if (cell.type == CELL_VIA) {
        color = Qt::white; // Ячейка ВИА тоже должна быть белой
        pen = QPen(Qt::gray, 1); // Тонкая серая граница
        // Визуальное отображение VIA теперь ТОЛЬКО через черные кружки в drawTraceLine()
    }
    // Для препятствий серый
    else if (cell.type == CELL_OBSTACLE) {
        color = Qt::darkGray;
        pen = QPen(Qt::darkGray, 2);
    }
    // Для площадок оставляем оригинальный цвет
    else if (cell.type == CELL_PAD) {
        pen = QPen(color.darker(), 2);
    }
    // Для пустых ячеек
    else {
        color = Qt::white;
        pen = QPen(Qt::gray, 1);
    }

    rect->setBrush(QBrush(color));
    rect->setPen(pen);

    // Сброс прозрачности
    rect->setOpacity(1.0);
}

void MainWindow::onCellClicked(int x, int y)
{
    // Проверяем границы
    if (x < 0 || x >= boardWidth || y < 0 || y >= boardHeight) {
        ui->statusBar->showMessage("Ошибка: клик вне границ платы");
        return;
    }

    switch (currentMode) {
    case MODE_OBSTACLE:
        // Проверяем, нет ли площадки в этой точке
        if (hasPadAt(x, y)) {
            QMessageBox::warning(this, "Ошибка",
                "Нельзя установить препятствие на контактной площадке");
            return;
        }

        grid[currentLayer][y][x].type = CELL_OBSTACLE;
        grid[currentLayer][y][x].color = Qt::darkGray;
        updateCellDisplay(x, y);
        ui->statusBar->showMessage(QString("Установлено препятствие в (%1, %2)").arg(x).arg(y));
        break;

    case MODE_PAD: {
        // Проверяем, нет ли уже площадки в этой точке
        if (hasPadAt(x, y)) {
            QMessageBox::warning(this, "Ошибка",
                "В этой точке уже есть контактная площадка");
            return;
        }

        // Проверяем, нет ли препятствия на текущем слое
        if (grid[currentLayer][y][x].type == CELL_OBSTACLE) {
            QMessageBox::warning(this, "Ошибка",
                "Нельзя установить площадку на препятствии");
            return;
        }

        // УБИРАЕМ ДИАЛОГОВОЕ ОКНО и используем автоматическое имя
        QString name = QString("PAD%1").arg(nextPadId);

        Pad pad;
        pad.id = nextPadId;
        pad.x = x;
        pad.y = y;
        pad.name = name;

        // Генерируем случайный цвет для площадки
        static int hue = 0;
        pad.color = QColor::fromHsv(hue, 180, 220);
        hue = (hue + 40) % 360;

        // Добавляем площадку в список
        pads.append(pad);

        // Установка на всех слоях
        for (int l = 0; l < layerCount; l++) {
            grid[l][y][x].type = CELL_PAD;
            grid[l][y][x].padId = pad.id;
            grid[l][y][x].color = pad.color;
            grid[l][y][x].traceId = -1;
        }

        nextPadId++; // Увеличиваем ID для следующей площадки

        updateCellDisplay(x, y);
        ui->statusBar->showMessage(
            QString("Добавлена площадка %1 в (%2, %3)")
            .arg(name).arg(x).arg(y));
        break;
    }

    case MODE_CONNECTION: {
        int padId = getPadAt(x, y);
        if (padId == -1) {
            QMessageBox::warning(this, "Ошибка",
                "Выберите контактную площадку");
            return;
        }

        if (selectedPadId == -1) {
            selectedPadId = padId;
            Pad* pad = getPadById(padId);
            ui->statusBar->showMessage(
                QString("Выбрана площадка %1. Выберите вторую площадку для соединения")
                .arg(pad ? pad->name : QString::number(padId)));
        } else if (selectedPadId != padId) {
            // Проверяем, нет ли уже такой связи
            bool connectionExists = false;
            for (const Connection& conn : connections) {
                if ((conn.fromPadId == selectedPadId && conn.toPadId == padId) ||
                    (conn.fromPadId == padId && conn.toPadId == selectedPadId)) {
                    connectionExists = true;
                    break;
                }
            }

            if (connectionExists) {
                QMessageBox::information(this, "Информация",
                    "Связь между этими площадками уже существует");
                selectedPadId = -1;
                return;
            }

            // Добавление связи
            Connection conn;
            conn.fromPadId = selectedPadId;
            conn.toPadId = padId;
            conn.routed = false;
            conn.layer = -1;
            conn.visualLine = nullptr;

            connections.append(conn);

            // Рисуем визуальную линию связи
            drawConnectionLine(selectedPadId, padId);

            // Обновление связей площадок
            Pad* pad1 = getPadById(selectedPadId);
            Pad* pad2 = getPadById(padId);

            if (pad1 && pad2) {
                if (!pad1->connections.contains(padId)) {
                    pad1->connections.append(padId);
                }
                if (!pad2->connections.contains(selectedPadId)) {
                    pad2->connections.append(selectedPadId);
                }

                ui->statusBar->showMessage(
                    QString("Создана связь между площадками %1 и %2")
                    .arg(pad1->name).arg(pad2->name));
            }

            selectedPadId = -1;
        }
        break;
    }

    default:
        // В режиме без инструмента показываем информацию о ячейке
        QString info = QString("Ячейка (%1, %2), Слой %3: ").arg(x).arg(y).arg(currentLayer + 1);

        CellType type = grid[currentLayer][y][x].type;
        switch (type) {
        case CELL_EMPTY: info += "Пусто"; break;
        case CELL_OBSTACLE: info += "Препятствие"; break;
        case CELL_PAD: {
            int padId = grid[currentLayer][y][x].padId;
            Pad* pad = getPadById(padId);
            info += QString("Площадка %1").arg(pad ? pad->name : "?");
            break;
        }
        case CELL_TRACE: info += "Трасса"; break;
        case CELL_VIA: info += "Переход"; break;
        }

        ui->statusBar->showMessage(info);
        break;
    }
}

void MainWindow::restoreFailedConnectionLines()
{
    // Удаляем только визуальные элементы, которые были изменены
    // (зеленые сплошные линии успешных трасс)
    QList<QGraphicsItem*> items = scene->items();
    for (QGraphicsItem* item : items) {
        if (QGraphicsLineItem* lineItem = dynamic_cast<QGraphicsLineItem*>(item)) {
            QPen pen = lineItem->pen();
            // Удаляем только зеленые сплошные линии (успешные трассы)
            if (pen.color() == Qt::green && pen.style() == Qt::SolidLine) {
                scene->removeItem(lineItem);
                delete lineItem;
            }
        }
    }

    // Восстанавливаем стандартное отображение для всех связей
    for (Connection& conn : connections) {
        if (!conn.routed) {
            // Для неудачных трасс создаем/обновляем черный пунктир
            if (conn.visualLine) {
                // Просто меняем стиль существующей линии
                conn.visualLine->setPen(QPen(Qt::black, 1, Qt::DashLine));
            } else {
                // Создаем новую линию если она была удалена
                drawConnectionLine(conn.fromPadId, conn.toPadId);
            }
        }
    }
}

void MainWindow::onRoute()
{
    if (connections.isEmpty()) {
        QMessageBox::information(this, "Информация", "Нет связей для трассировки");
        return;
    }

    // Проверяем, не выполняется ли уже трассировка
    if (multiThreadRoutingInProgress) {
        QMessageBox::warning(this, "Внимание", "Трассировка уже выполняется");
        return;
    }

    // Очищаем старые трассы
    onClearTraces();

    // Начинаем замер времени
    routeTimer.start();

    // Выполняем трассировку выбранным методом
    switch (currentRoutingMethod) {
    case SINGLE_THREADED:
        performSingleThreadedRouting();
        break;
    case MULTI_THREADED:
        performMultiThreadedRouting();
        break;
    }
}

void MainWindow::performSingleThreadedRouting()
{
    int routedCount = 0;
    failedConnections.clear();

    qDebug() << "=== ОДНОПОТОЧНАЯ ТРАССИРОВКА ===";
    qDebug() << "Количество слоев:" << layerCount;
    qDebug() << "Количество соединений:" << connections.size();

    // Сортируем соединения по сложности
    QList<Connection> sortedConnections = connections;
    std::sort(sortedConnections.begin(), sortedConnections.end(),
              [this](const Connection& a, const Connection& b) {
                  Pad* padA1 = getPadById(a.fromPadId);
                  Pad* padA2 = getPadById(a.toPadId);
                  Pad* padB1 = getPadById(b.fromPadId);
                  Pad* padB2 = getPadById(b.toPadId);

                  if (!padA1 || !padA2 || !padB1 || !padB2) return false;

                  int distA = abs(padA1->x - padA2->x) + abs(padA1->y - padA2->y);
                  int distB = abs(padB1->x - padB2->x) + abs(padB1->y - padB2->y);

                  return distA < distB;
              });

    // Трассируем каждое соединение последовательно
    for (int connIndex = 0; connIndex < sortedConnections.size(); connIndex++) {
        Connection& conn = sortedConnections[connIndex];
        Pad* fromPad = getPadById(conn.fromPadId);
        Pad* toPad = getPadById(conn.toPadId);

        if (!fromPad || !toPad) {
            qDebug() << "Пропускаем соединение: не найдены площадки";
            continue;
        }

        qDebug() << "Трассируем соединение" << connIndex << "(" << fromPad->name << "->" << toPad->name << ")";

        // ВАЖНОЕ ИСПРАВЛЕНИЕ: Выбираем оптимальные начальные и конечные слои
        GridPoint start(fromPad->x, fromPad->y, 0);
        GridPoint end(toPad->x, toPad->y, 0);

        // Пытаемся найти лучшие слои для начала и конца
        int bestStartLayer = 0;
        int bestEndLayer = 0;
        int bestStartCost = INT_MAX;
        int bestEndCost = INT_MAX;

        // Проверяем все слои для стартовой точки
        for (int layer = 0; layer < layerCount; layer++) {
            if (grid[layer][fromPad->y][fromPad->x].type == CELL_PAD) {
                // Проверяем, не занята ли эта точка на этом слое
                bool free = true;
                for (int l = 0; l < layerCount; l++) {
                    if (grid[l][fromPad->y][fromPad->x].type == CELL_OBSTACLE) {
                        free = false;
                        break;
                    }
                }
                if (free) {
                    // Оцениваем "стоимость" использования этого слоя
                    int cost = 0;
                    // Поощряем использование менее загруженных слоев
                    int traceCountOnLayer = 0;
                    for (int y = 0; y < boardHeight; y++) {
                        for (int x = 0; x < boardWidth; x++) {
                            if (grid[layer][y][x].type == CELL_TRACE) {
                                traceCountOnLayer++;
                            }
                        }
                    }
                    cost = traceCountOnLayer * 2; // Чем больше трасс на слое, тем дороже

                    if (cost < bestStartCost) {
                        bestStartCost = cost;
                        bestStartLayer = layer;
                    }
                }
            }
        }

        // Аналогично для конечной точки
        for (int layer = 0; layer < layerCount; layer++) {
            if (grid[layer][toPad->y][toPad->x].type == CELL_PAD) {
                bool free = true;
                for (int l = 0; l < layerCount; l++) {
                    if (grid[l][toPad->y][toPad->x].type == CELL_OBSTACLE) {
                        free = false;
                        break;
                    }
                }
                if (free) {
                    int cost = 0;
                    int traceCountOnLayer = 0;
                    for (int y = 0; y < boardHeight; y++) {
                        for (int x = 0; x < boardWidth; x++) {
                            if (grid[layer][y][x].type == CELL_TRACE) {
                                traceCountOnLayer++;
                            }
                        }
                    }
                    cost = traceCountOnLayer * 2;

                    if (cost < bestEndCost) {
                        bestEndCost = cost;
                        bestEndLayer = layer;
                    }
                }
            }
        }

        // Устанавливаем оптимальные слои
        start.layer = bestStartLayer;
        end.layer = bestEndLayer;

        qDebug() << "Используем слои: старт=" << start.layer << ", конец=" << end.layer;

        QList<GridPoint> path;
        {
            QMutexLocker locker(&gridMutex);
            path = pathFinder.findPath(start, end, grid, boardWidth, boardHeight,
                                      layerCount, fromPad->id, toPad->id);
        }

        if (!path.isEmpty()) {
            // Размещаем трассу
            QMutexLocker locker(&gridMutex);

            // Собираем статистику по слоям для этой трассы
            QMap<int, int> layerStats;
            for (const GridPoint& point : path) {
                layerStats[point.layer]++;
            }

            qDebug() << "Путь найден. Длина:" << path.size();
            qDebug() << "Использовано слоев:" << layerStats.size();
            for (auto it = layerStats.begin(); it != layerStats.end(); ++it) {
                qDebug() << "  Слой" << it.key() << ":" << it.value() << "точек";
            }

            for (const GridPoint& point : path) {
                if (point.x < 0 || point.x >= boardWidth ||
                    point.y < 0 || point.y >= boardHeight ||
                    point.layer < 0 || point.layer >= layerCount) {
                    continue;
                }

                GridCell& cell = grid[point.layer][point.y][point.x];
                if (cell.type != CELL_PAD) {
                    // Определяем, является ли это точкой перехода между слоями
                    bool isVia = false;
                    int prevIndex = path.indexOf(point) - 1;
                    if (prevIndex >= 0) {
                        const GridPoint& prevPoint = path[prevIndex];
                        if (prevPoint.x == point.x && prevPoint.y == point.y &&
                            prevPoint.layer != point.layer) {
                            isVia = true;
                        }
                    }

                    if (isVia) {
                        // Для VIA отмечаем на всех слоях
                        for (int l = 0; l < layerCount; l++) {
                            if (l == point.layer || l == path[prevIndex].layer) {
                                grid[l][point.y][point.x].type = CELL_VIA;
                                grid[l][point.y][point.x].traceId = fromPad->id;
                            }
                        }
                    } else {
                        cell.type = CELL_TRACE;
                        cell.traceId = fromPad->id;
                    }
                }
            }

            // Рисуем линии
            for (int i = 0; i < path.size() - 1; i++) {
                drawTraceLine(path[i], path[i + 1], path[i].layer);
            }

            // Обновляем соединение
            for (Connection& mainConn : connections) {
                if (mainConn.fromPadId == conn.fromPadId &&
                    mainConn.toPadId == conn.toPadId) {
                    mainConn.routed = true;
                    mainConn.layer = start.layer; // Сохраняем слой, на котором начата трассировка

                    // ВАЖНОЕ ИЗМЕНЕНИЕ: Не меняем стиль визуальной линии здесь
                    // Линия останется черным пунктиром до конца трассировки
                    if (mainConn.visualLine) {
                        // Временно скрываем успешную связь
                        mainConn.visualLine->hide();
                    }
                    break;
                }
            }

            routedCount++;
            qDebug() << "Соединение успешно проложено";
        } else {
            qDebug() << "Не удалось найти путь для соединения";
            failedConnections.append(fromPad->id);

            // Для неудачных трасс просто оставляем черный пунктир
            // Ничего не меняем в visualLine
        }

        drawGrid();
        QApplication::processEvents();
    }

    // В самом конце метода добавьте:
       qDebug() << "=== ТРАССИРОВКА ЗАВЕРШЕНА ===";
       qDebug() << "Успешно проложено:" << routedCount << "из" << connections.size();
       qDebug() << "Неудачные соединения:" << failedConnections.size();

       // Выводим статистику по слоям
       QMap<int, int> tracesPerLayer;
       for (int l = 0; l < layerCount; l++) {
           int traceCount = 0;
           for (int y = 0; y < boardHeight; y++) {
               for (int x = 0; x < boardWidth; x++) {
                   if (grid[l][y][x].type == CELL_TRACE || grid[l][y][x].type == CELL_VIA) {
                       traceCount++;
                   }
               }
           }
           tracesPerLayer[l] = traceCount;
       }

       qDebug() << "Распределение трасс по слоям:";
       for (int l = 0; l < layerCount; l++) {
           qDebug() << "  Слой" << l << ":" << tracesPerLayer[l] << "трасс";
       }

       // ПОКАЗЫВАЕМ ОКНО С РЕЗУЛЬТАТАМИ
       showRoutingResults(routedCount, connections.size(), failedConnections.size(), tracesPerLayer);
   }

void MainWindow::performMultiThreadedRouting()
{
    if (connections.isEmpty()) {
        QMessageBox::information(this, "Информация", "Нет связей для трассировки");
        return;
    }

    // Очищаем старые трассы
    onClearTraces();

    // Подготавливаем данные для трассировки
    QList<ConnectionRequest> requests = prepareConnectionRequests();

    if (requests.isEmpty()) {
        QMessageBox::information(this, "Информация", "Нет соединений для трассировки");
        return;
    }

    qDebug() << "Запуск многопоточной трассировки для" << requests.size() << "соединений";

    // Создаем и настраиваем роутер
    multiThreadRouter = new MultiThreadedRouter(
        grid, boardWidth, boardHeight, layerCount,
        requests,
        this
    );

    // Настраиваем количество потоков
    multiThreadRouter->setLayerThreadCount(2); // 2 потока на слой

    // Подключаем сигналы с использованием старого синтаксиса для надежности
    connect(multiThreadRouter, SIGNAL(progressChanged(int)),
            this, SLOT(onMultiThreadRoutingProgress(int)));
    connect(multiThreadRouter, SIGNAL(routingComplete()),
            this, SLOT(onMultiThreadRoutingComplete()));
    connect(multiThreadRouter, SIGNAL(errorOccurred(QString)),
            this, SLOT(onMultiThreadRoutingError(QString)));

    // Устанавливаем флаг
    multiThreadRoutingInProgress = true;
    ui->routeBtn->setEnabled(false);

    // Запускаем таймер и трассировку
    routeTimer.start();
    multiThreadRouter->startRouting();

    ui->statusBar->showMessage("Многопоточная трассировка запущена...");
}

void MainWindow::onMultiThreadRoutingProgress(int percent) {
    ui->statusBar->showMessage(QString("Трассировка: %1% завершено").arg(percent));
}

void MainWindow::onMultiThreadRoutingError(const QString& message) {
    QMessageBox::warning(this, "Ошибка многопоточной трассировки", message);

    if (multiThreadRouter) {
        delete multiThreadRouter;
        multiThreadRouter = nullptr;
    }
}

void MainWindow::onMultiThreadRoutingComplete()
{
    qint64 elapsedMs = routeTimer.elapsed();

    if (!multiThreadRouter) return;

    // Получаем результаты
    QList<RoutingResult> results = multiThreadRouter->getSuccessfulResults();
    QList<QPair<int, int>> failed = multiThreadRouter->getFailedConnections();

    // Применяем результаты
    int successfulTraces = 0;

    QMutexLocker locker(&gridMutex);

    for (const RoutingResult& result : results) {
        if (result.success && !result.path.isEmpty()) {
            // Находим соответствующее соединение
            for (Connection& conn : connections) {
                if (conn.fromPadId == result.fromPadId &&
                    conn.toPadId == result.toPadId && !conn.routed) {

                    // Проверяем возможность размещения
                    bool canPlace = true;
                    for (const GridPoint& point : result.path) {
                        if (point.x < 0 || point.x >= boardWidth ||
                            point.y < 0 || point.y >= boardHeight ||
                            point.layer < 0 || point.layer >= layerCount) {
                            canPlace = false;
                            break;
                        }

                        GridCell& cell = grid[point.layer][point.y][point.x];
                        Pad* fromPad = getPadById(result.fromPadId);

                        // Пропускаем начальную и конечную точки
                        if (fromPad &&
                           ((point.x == fromPad->x && point.y == fromPad->y) ||
                            (point.x == result.path.last().x && point.y == result.path.last().y))) {
                            continue;
                        }

                        if (cell.type == CELL_OBSTACLE ||
                           (cell.type == CELL_TRACE && cell.traceId != result.fromPadId) ||
                           (cell.type == CELL_VIA && cell.traceId != result.fromPadId)) {
                            canPlace = false;
                            break;
                        }
                    }

                    if (canPlace) {
                        // Размещаем трассу
                        for (int i = 0; i < result.path.size(); i++) {
                            const GridPoint& point = result.path[i];
                            GridCell& cell = grid[point.layer][point.y][point.x];

                            // Пропускаем площадки
                            if (cell.type == CELL_PAD) continue;

                            // Определяем, является ли это VIA
                            bool isVia = false;
                            if (i > 0) {
                                const GridPoint& prev = result.path[i - 1];
                                if (prev.x == point.x && prev.y == point.y &&
                                    prev.layer != point.layer) {
                                    isVia = true;
                                }
                            }

                            if (isVia) {
                                cell.type = CELL_VIA;
                            } else {
                                cell.type = CELL_TRACE;
                            }
                            cell.traceId = result.fromPadId;
                        }

                        // Обновляем соединение
                        conn.routed = true;
                        conn.layer = result.path.first().layer;

                        // Скрываем линию связи
                        if (conn.visualLine) {
                            conn.visualLine->hide();
                        }

                        // Рисуем линии трасс
                        for (int i = 0; i < result.path.size() - 1; i++) {
                            drawTraceLine(result.path[i], result.path[i + 1], result.path[i].layer);
                        }

                        successfulTraces++;
                        break;
                    }
                }
            }
        }
    }

    // Обновляем отображение
    drawGrid();

    // Восстанавливаем линии для неудачных соединений
    restoreFailedConnectionLines();

    // Показываем результаты
    QString message = QString("Многопоточная трассировка завершена за %1 мс\n")
                     .arg(elapsedMs);
    message += QString("Успешно: %1, Неудачно: %2")
               .arg(successfulTraces).arg(failed.size());

    ui->statusBar->showMessage(message);

    multiThreadRoutingInProgress = false;
    ui->routeBtn->setEnabled(true);

    // Очищаем роутер
    delete multiThreadRouter;
    multiThreadRouter = nullptr;

    QMap<int, int> tracesPerLayer;
       for (int l = 0; l < layerCount; l++) {
           int traceCount = 0;
           for (int y = 0; y < boardHeight; y++) {
               for (int x = 0; x < boardWidth; x++) {
                   if (grid[l][y][x].type == CELL_TRACE || grid[l][y][x].type == CELL_VIA) {
                       traceCount++;
                   }
               }
           }
           tracesPerLayer[l] = traceCount;
       }

      int totalConnections = results.size() + failed.size();
       // ПОКАЗЫВАЕМ ОКНО С РЕЗУЛЬТАТАМИ
      showRoutingResults(successfulTraces, totalConnections, failed.size(), tracesPerLayer);

       multiThreadRoutingInProgress = false;
       ui->routeBtn->setEnabled(true);

       // Очищаем роутер
       delete multiThreadRouter;
       multiThreadRouter = nullptr;
}

void MainWindow::showRoutingResults(int successCount, int totalCount, int failedCount,
                                   const QMap<int, int>& tracesPerLayer)
{
    QString message = QString("<h3>Результаты трассировки</h3>");
    message += QString("<p><b>Общая статистика:</b></p>");
    message += QString("<table border='1' cellpadding='5' style='border-collapse: collapse; width: 100%;'>");
    message += QString("<tr><th>Показатель</th><th>Значение</th></tr>");
    message += QString("<tr><td>Всего соединений</td><td align='center'><b>%1</b></td></tr>").arg(totalCount);

    if (successCount > 0) {
        message += QString("<tr><td>Успешно проложено</td><td align='center' style='color: green;'><b>%1</b></td></tr>").arg(successCount);
    }

    if (failedCount > 0) {
        message += QString("<tr><td>Не удалось проложить</td><td align='center' style='color: red;'><b>%1</b></td></tr>").arg(failedCount);
    }

    int elapsedMs = routeTimer.elapsed();
    message += QString("<tr><td>Время выполнения</td><td align='center'><b>%1 мс</b></td></tr>").arg(elapsedMs);
    message += QString("</table>");

    // Только общая статистика без распределения по слоям

    // Создаем диалоговое окно с увеличенными размерами
    QDialog resultsDialog(this);
    resultsDialog.setWindowTitle("Результаты трассировки");
    resultsDialog.setMinimumSize(500, 400);  // Уменьшенный размер, так как информации меньше
    resultsDialog.setMaximumSize(600, 450);

    // Устанавливаем стиль для лучшего отображения
    resultsDialog.setStyleSheet(
        "QDialog { background-color: white; }"
        "QTextBrowser { border: 1px solid #ccc; background-color: #f9f9f9; }"
        "QPushButton { padding: 5px 15px; font-weight: bold; }"
    );

    QVBoxLayout *layout = new QVBoxLayout(&resultsDialog);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(10);

    // Поле для отображения текста
    QTextBrowser *textBrowser = new QTextBrowser();
    textBrowser->setHtml(message);
    textBrowser->setOpenExternalLinks(false);
    textBrowser->setMinimumHeight(200);  // Уменьшенная высота
    textBrowser->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    // Панель кнопок
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(10);

    // Если есть неудачные трассы, добавляем кнопку для их просмотра
    if (failedCount > 0) {
        QPushButton *showFailedBtn = new QPushButton("Показать неудачные соединения");
        showFailedBtn->setStyleSheet("QPushButton { background-color: #ffcccc; }");
        connect(showFailedBtn, &QPushButton::clicked, this, &MainWindow::showFailedConnections);
        buttonLayout->addWidget(showFailedBtn);
    }

    buttonLayout->addStretch();

    QPushButton *okButton = new QPushButton("OK");
    okButton->setDefault(true);
    okButton->setStyleSheet("QPushButton { background-color: #4CAF50; color: white; }");
    connect(okButton, &QPushButton::clicked, &resultsDialog, &QDialog::accept);

    buttonLayout->addWidget(okButton);

    layout->addWidget(textBrowser);
    layout->addLayout(buttonLayout);

    // Показываем окно модально
    resultsDialog.exec();
}

int MainWindow::countLayerTransitions(const QList<GridPoint>& path)
{
    if (path.size() < 2) return 0;

    int transitions = 0;
    int currentLayer = path[0].layer;

    for (int i = 1; i < path.size(); i++) {
        if (path[i].layer != currentLayer) {
            transitions++;
            currentLayer = path[i].layer;
        }
    }

    return transitions;
}

int MainWindow::estimateConnectionComplexity(int x1, int y1, int x2, int y2)
{
    int complexity = 0;

    // Проверяем количество препятствий на прямой линии между точками
    int dx = x2 - x1;
    int dy = y2 - y1;
    int steps = qMax(abs(dx), abs(dy));

    if (steps > 0) {
        for (int i = 0; i <= steps; i++) {
            int x = x1 + dx * i / steps;
            int y = y1 + dy * i / steps;

            for (int l = 0; l < layerCount; l++) {
                if (grid[l][y][x].type == CELL_OBSTACLE) {
                    complexity++;
                    break;
                }
            }
        }
    }

    return complexity;
}

QList<GridPoint> MainWindow::adjustPathAroundConflict(const QList<GridPoint>& originalPath,
                                                     int conflictIndex,
                                                     int fromPadId, int toPadId)
{
    if (conflictIndex < 0 || conflictIndex >= originalPath.size()) {
        return originalPath;
    }

    const GridPoint& conflictPoint = originalPath[conflictIndex];
    Pad* fromPad = getPadById(fromPadId);
    Pad* toPad = getPadById(toPadId);

    if (!fromPad || !toPad) return originalPath;

    // Попробуем обойти конфликт через соседние ячейки
    QList<GridPoint> newPath;

    // Копируем часть до конфликта
    for (int i = 0; i < conflictIndex; i++) {
        newPath.append(originalPath[i]);
    }

    // Пробуем 4 направления для обхода
    const int dx[4] = {1, -1, 0, 0};
    const int dy[4] = {0, 0, 1, -1};

    bool foundBypass = false;

    for (int dir = 0; dir < 4; dir++) {
        int nx = conflictPoint.x + dx[dir];
        int ny = conflictPoint.y + dy[dir];

        if (nx >= 0 && nx < boardWidth && ny >= 0 && ny < boardHeight) {
            // Проверяем, свободна ли ячейка
            bool free = true;
            for (int l = 0; l < layerCount; l++) {
                GridCell& cell = grid[l][ny][nx];
                if ((cell.type == CELL_OBSTACLE) ||
                    ((cell.type == CELL_TRACE || cell.type == CELL_VIA) && cell.traceId != fromPadId)) {
                    free = false;
                    break;
                }
            }

            if (free) {
                // Добавляем точку обхода
                newPath.append(GridPoint(nx, ny, conflictPoint.layer));
                newPath.append(conflictPoint); // Возвращаемся к конфликтной точке
                foundBypass = true;
                break;
            }
        }
    }

    if (foundBypass) {
        // Копируем остаток пути
        for (int i = conflictIndex + 1; i < originalPath.size(); i++) {
            newPath.append(originalPath[i]);
        }
        return newPath;
    }

    // Если не удалось обойти, попробуем сменить слой
    for (int layer = 0; layer < layerCount; layer++) {
        if (layer != conflictPoint.layer) {
            // Проверяем, свободна ли ячейка на другом слое
            GridCell& cell = grid[layer][conflictPoint.y][conflictPoint.x];
            if (cell.type == CELL_EMPTY ||
                (cell.type == CELL_TRACE && cell.traceId == fromPadId) ||
                (cell.type == CELL_VIA && cell.traceId == fromPadId)) {

                // Создаем путь со сменой слоя
                newPath.clear();
                for (int i = 0; i < conflictIndex; i++) {
                    newPath.append(originalPath[i]);
                }

                // Добавляем переход на другой слой и обратно
                newPath.append(GridPoint(conflictPoint.x, conflictPoint.y, layer));
                newPath.append(GridPoint(conflictPoint.x, conflictPoint.y, conflictPoint.layer));

                // Копируем остаток пути
                for (int i = conflictIndex + 1; i < originalPath.size(); i++) {
                    newPath.append(originalPath[i]);
                }

                return newPath;
            }
        }
    }

    return originalPath; // Возвращаем оригинальный путь, если не удалось обойти
}

bool MainWindow::canPlacePathInTempGrid(const QList<GridPoint>& path,
                                       GridCell*** tempGrid,
                                       int fromPadId, int toPadId)
{
    for (const GridPoint& point : path) {
        if (point.x < 0 || point.x >= boardWidth ||
            point.y < 0 || point.y >= boardHeight ||
            point.layer < 0 || point.layer >= layerCount) {
            return false;
        }

        GridCell& cell = tempGrid[point.layer][point.y][point.x];

        // Пропускаем площадки этого соединения
        if ((cell.type == CELL_PAD && (cell.padId == fromPadId || cell.padId == toPadId))) {
            continue;
        }

        // Если ячейка занята (не пустая и не наша трасса)
        if (cell.type != CELL_EMPTY &&
            !(cell.type == CELL_TRACE && cell.traceId == fromPadId) &&
            !(cell.type == CELL_VIA && cell.traceId == fromPadId)) {
            return false;
        }
    }
    return true;
}

void MainWindow::placePathInTempGrid(const QList<GridPoint>& path,
                                    GridCell*** tempGrid,
                                    int traceId)
{
    for (int i = 0; i < path.size(); i++) {
        const GridPoint& point = path[i];
        GridCell& cell = tempGrid[point.layer][point.y][point.x];

        // Пропускаем площадки
        if (cell.type == CELL_PAD) continue;

        // Определяем VIA
        bool isVia = false;
        if (i > 0) {
            const GridPoint& prev = path[i-1];
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
        cell.color = Qt::white;
    }
}

void MainWindow::copyTempGridToMain(GridCell*** tempGrid)
{
    QMutexLocker locker(&gridMutex);

    for (int l = 0; l < layerCount; l++) {
        for (int y = 0; y < boardHeight; y++) {
            for (int x = 0; x < boardWidth; x++) {
                // Копируем только трассы и VIA
                if (tempGrid[l][y][x].type == CELL_TRACE ||
                    tempGrid[l][y][x].type == CELL_VIA) {
                    grid[l][y][x] = tempGrid[l][y][x];
                }
            }
        }
    }

    // Обновляем статус соединений и рисуем линии трасс
    for (Connection& conn : connections) {
        Pad* fromPad = getPadById(conn.fromPadId);
        Pad* toPad = getPadById(conn.toPadId);
        if (!fromPad || !toPad) continue;

        // Проверяем, проложена ли трасса
        bool routed = false;
        for (int l = 0; l < layerCount; l++) {
            if (grid[l][fromPad->y][fromPad->x].traceId == fromPad->id ||
                grid[l][toPad->y][toPad->x].traceId == fromPad->id) {
                routed = true;

                // Ищем путь и рисуем линии
                QList<GridPoint> path = findPath(GridPoint(fromPad->x, fromPad->y, l),
                                                 GridPoint(toPad->x, toPad->y, findLayerForPad(toPad->x, toPad->y)),
                                                 fromPad->id, toPad->id);
                if (!path.isEmpty()) {
                    for (int i = 0; i < path.size() - 1; i++) {
                        drawTraceLineMultiThreaded(path[i], path[i + 1], path[i].layer);
                    }
                }
                break;
            }
        }

        conn.routed = routed;
        if (conn.routed && conn.visualLine) {
            conn.visualLine->hide();
        }
    }
}


RoutingResult MainWindow::routeSingleConnectionAsync(int connIndex, const Connection& conn)
{
    Pad* fromPad = getPadById(conn.fromPadId);
    Pad* toPad = getPadById(conn.toPadId);

    RoutingResult result;
    result.connIndex = connIndex;
    result.fromPadId = conn.fromPadId;
    result.toPadId = conn.toPadId;
    result.success = false;

    if (!fromPad || !toPad) {
        return result;
    }

    // Блокируем доступ к сетке для чтения
    QMutexLocker locker(&gridMutex);

    // Определяем доступные слои для старта и конца
    int bestStartLayer = -1;
    int bestEndLayer = -1;

    // Ищем доступные слои для стартовой площадки
    for (int l = 0; l < layerCount; l++) {
        if (grid[l][fromPad->y][fromPad->x].type == CELL_PAD) {
            bestStartLayer = l;
            break;
        }
    }

    // Ищем доступные слои для конечной площадки
    for (int l = 0; l < layerCount; l++) {
        if (grid[l][toPad->y][toPad->x].type == CELL_PAD) {
            bestEndLayer = l;
            break;
        }
    }

    if (bestStartLayer == -1 || bestEndLayer == -1) {
        return result;
    }

    GridPoint start(fromPad->x, fromPad->y, bestStartLayer);
    GridPoint end(toPad->x, toPad->y, bestEndLayer);

    // Ищем путь
    result.path = pathFinder.findPath(start, end, grid, boardWidth, boardHeight,
                                     layerCount, fromPad->id, toPad->id);
    result.success = !result.path.isEmpty();

    return result;
}

void MainWindow::processRoutingResultsImproved(const QList<RoutingResult>& results,
                                              const QList<Connection>& connectionsToRoute)
{
    int routedCount = 0;

    // Сортируем результаты по качеству пути (сначала короткие, потом с меньшим количеством переходов)
    QList<RoutingResult> sortedResults = results;
    std::sort(sortedResults.begin(), sortedResults.end(),
              [](const RoutingResult& a, const RoutingResult& b) {
                  if (a.path.size() != b.path.size()) {
                      return a.path.size() < b.path.size();
                  }

                  // Считаем количество переходов между слоями
                  int aTransitions = 0;
                  if (!a.path.isEmpty()) {
                      int currentLayer = a.path[0].layer;
                      for (int i = 1; i < a.path.size(); i++) {
                          if (a.path[i].layer != currentLayer) {
                              aTransitions++;
                              currentLayer = a.path[i].layer;
                          }
                      }
                  }

                  int bTransitions = 0;
                  if (!b.path.isEmpty()) {
                      int currentLayer = b.path[0].layer;
                      for (int i = 1; i < b.path.size(); i++) {
                          if (b.path[i].layer != currentLayer) {
                              bTransitions++;
                              currentLayer = b.path[i].layer;
                          }
                      }
                  }

                  return aTransitions < bTransitions;
              });

    for (const RoutingResult& result : results) {
            if (!result.success) continue;

            Connection* conn = nullptr;
            for (Connection& c : connections) {
                if (c.fromPadId == result.fromPadId && c.toPadId == result.toPadId) {
                    conn = &c;
                    break;
                }
            }

        if (!conn || conn->routed) continue;

        // Более либеральная проверка возможности размещения
        bool canPlace = true;
        QList<GridPoint> adjustedPath = result.path;

        {
            QMutexLocker locker(&gridMutex);

            // Проверяем каждую точку пути
            for (int i = 0; i < adjustedPath.size(); i++) {
                const GridPoint& point = adjustedPath[i];

                if (point.x < 0 || point.x >= boardWidth ||
                    point.y < 0 || point.y >= boardHeight ||
                    point.layer < 0 || point.layer >= layerCount) {
                    canPlace = false;
                    break;
                }

                GridCell& cell = grid[point.layer][point.y][point.x];

                // Пропускаем начальную и конечную точки (площадки)
                Pad* fromPad = getPadById(conn->fromPadId);
                Pad* toPad = getPadById(conn->toPadId);

                if ((fromPad && point.x == fromPad->x && point.y == fromPad->y) ||
                    (toPad && point.x == toPad->x && point.y == toPad->y)) {
                    continue;
                }

                // Если ячейка уже занята другой трассой, пытаемся ее обойти
                if ((cell.type == CELL_TRACE || cell.type == CELL_VIA) &&
                    cell.traceId != conn->fromPadId) {

                    // Проверяем, можно ли использовать соседние ячейки
                    bool foundAlternative = false;

                    // Проверяем 4 направления
                    const int dx[4] = {1, -1, 0, 0};
                    const int dy[4] = {0, 0, 1, -1};

                    for (int dir = 0; dir < 4; dir++) {
                        int nx = point.x + dx[dir];
                        int ny = point.y + dy[dir];

                        if (nx >= 0 && nx < boardWidth && ny >= 0 && ny < boardHeight) {
                            GridCell& neighbor = grid[point.layer][ny][nx];

                            if (neighbor.type == CELL_EMPTY ||
                                (neighbor.type == CELL_TRACE && neighbor.traceId == conn->fromPadId) ||
                                (neighbor.type == CELL_VIA && neighbor.traceId == conn->fromPadId)) {

                                // Можем обойти через соседнюю ячейку
                                foundAlternative = true;

                                // Вставляем две дополнительные точки для обхода
                                QList<GridPoint> newPath;
                                newPath.reserve(adjustedPath.size() + 2);

                                // Копируем путь до текущей точки
                                for (int j = 0; j < i; j++) {
                                    newPath.append(adjustedPath[j]);
                                }

                                // Добавляем точку обхода
                                newPath.append(GridPoint(nx, ny, point.layer));

                                // Возвращаемся к следующей точке после текущей
                                if (i + 1 < adjustedPath.size()) {
                                    newPath.append(adjustedPath[i]);
                                    newPath.append(adjustedPath[i+1]);

                                    // Копируем остаток пути
                                    for (int j = i+2; j < adjustedPath.size(); j++) {
                                        newPath.append(adjustedPath[j]);
                                    }
                                } else {
                                    // Если это последняя точка, просто добавляем ее
                                    newPath.append(adjustedPath[i]);
                                }

                                adjustedPath = newPath;
                                break;
                            }
                        }
                    }

                    if (!foundAlternative) {
                        canPlace = false;
                        break;
                    }
                }
            }

            if (canPlace) {
                // Размещаем трассу
                for (int i = 0; i < adjustedPath.size(); i++) {
                    const GridPoint& point = adjustedPath[i];
                    GridCell& cell = grid[point.layer][point.y][point.x];

                    // Пропускаем площадки
                    if (cell.type == CELL_PAD) {
                        continue;
                    }

                    // Определяем, является ли это VIA
                    bool isVia = false;
                    if (i > 0) {
                        const GridPoint& prev = adjustedPath[i - 1];
                        if (prev.x == point.x && prev.y == point.y &&
                            prev.layer != point.layer) {
                            isVia = true;
                        }
                    }

                    if (isVia) {
                        // Для VIA помечаем на обоих слоях
                        for (int l = 0; l < layerCount; l++) {
                            if (l == point.layer || (i > 0 && l == adjustedPath[i-1].layer)) {
                                grid[l][point.y][point.x].type = CELL_VIA;
                                grid[l][point.y][point.x].traceId = conn->fromPadId;
                            }
                        }
                    } else {
                        cell.type = CELL_TRACE;
                        cell.traceId = conn->fromPadId;
                    }
                }

                // Обновляем соединение
                conn->routed = true;
                conn->layer = adjustedPath.first().layer;

                // Скрываем линию связи
                if (conn->visualLine) {
                    conn->visualLine->hide();
                }

                routedCount++;

                // Рисуем линии трасс
                for (int i = 0; i < adjustedPath.size() - 1; i++) {
                    drawTraceLine(adjustedPath[i], adjustedPath[i + 1], adjustedPath[i].layer);
                }

                qDebug() << "Успешно размещена трасса:"
                         << getPadById(conn->fromPadId)->name
                         << "->" << getPadById(conn->toPadId)->name
                         << "(длина:" << adjustedPath.size() << ")";
            } else {
                failedConnections.append(conn->fromPadId);
                qDebug() << "Не удалось разместить трассу для соединения"
                         << getPadById(conn->fromPadId)->name
                         << "->" << getPadById(conn->toPadId)->name;
            }
        }

        // Обновляем GUI между трассами
        drawGrid();
        QApplication::processEvents();
    }

    qDebug() << "Успешно проложено:" << routedCount << "трасс из" << connectionsToRoute.size();
}

QList<RoutingResult> MainWindow::routeConnectionsParallel(
    const QList<Connection>& connectionsToRoute)
{
    QList<RoutingResult> results;
    QMutex resultsMutex;

    // Создаем временную копию сетки для безопасного чтения
    GridCell*** tempGrid = createTempGrid();

    // Создаем список индексов для параллельной обработки
    QVector<int> indices;
    for (int i = 0; i < connectionsToRoute.size(); i++) {
        indices.append(i);
    }

    // Используем QtConcurrent::mapped с правильной лямбда-функцией
    auto routeFunction = [&](int connIndex) -> RoutingResult {
        const Connection& conn = connectionsToRoute[connIndex];
        Pad* fromPad = getPadById(conn.fromPadId);
        Pad* toPad = getPadById(conn.toPadId);

        RoutingResult result;
        result.connIndex = connIndex;
        result.success = false;

        if (!fromPad || !toPad) {
            return result;
        }

        // Определяем оптимальные начальный и конечный слои
        int bestStartLayer = findOptimalLayer(fromPad->x, fromPad->y);
        int bestEndLayer = findOptimalLayer(toPad->x, toPad->y);

        // Если найденные слои не содержат площадок, ищем доступные
        if (tempGrid[bestStartLayer][fromPad->y][fromPad->x].type != CELL_PAD) {
            for (int l = 0; l < layerCount; l++) {
                if (tempGrid[l][fromPad->y][fromPad->x].type == CELL_PAD) {
                    bestStartLayer = l;
                    break;
                }
            }
        }

        if (tempGrid[bestEndLayer][toPad->y][toPad->x].type != CELL_PAD) {
            for (int l = 0; l < layerCount; l++) {
                if (tempGrid[l][toPad->y][toPad->x].type == CELL_PAD) {
                    bestEndLayer = l;
                    break;
                }
            }
        }

        GridPoint start(fromPad->x, fromPad->y, bestStartLayer);
        GridPoint end(toPad->x, toPad->y, bestEndLayer);

        // Ищем путь во временной сетке
        QList<GridPoint> path = pathFinder.findPath(start, end, tempGrid,
                                                   boardWidth, boardHeight,
                                                   layerCount, fromPad->id, toPad->id);

        if (!path.isEmpty()) {
            result.path = path;
            result.success = true;
        }

        return result;
    };

    // Используем блокирующую версию для простоты
    QList<RoutingResult> tempResults;

    #pragma omp parallel for if(connectionsToRoute.size() > 1)
    for (int i = 0; i < indices.size(); i++) {
        RoutingResult result = routeFunction(i);
        #pragma omp critical
        {
            tempResults.append(result);
        }
    }

    // Сортируем результаты по индексу соединения
    std::sort(tempResults.begin(), tempResults.end(),
              [](const RoutingResult& a, const RoutingResult& b) {
                  return a.connIndex < b.connIndex;
              });

    results = tempResults;

    // Очищаем временную сетку
    freeTempGrid(tempGrid);

    return results;
}

void MainWindow::processRoutingResults(const QList<RoutingResult>& results)
{
    int routedCount = 0;

    // Сортируем результаты по качеству пути (сначала короткие)
    QList<RoutingResult> sortedResults = results;
    std::sort(sortedResults.begin(), sortedResults.end(),
              [](const RoutingResult& a, const RoutingResult& b) {
                  return a.path.size() < b.path.size();
              });

    for (const RoutingResult& result : sortedResults) {
        if (!result.success) continue;

        // Находим соответствующее соединение
        Connection* conn = nullptr;
        for (Connection& c : connections) {
            Pad* fromPad = getPadById(c.fromPadId);
            Pad* toPad = getPadById(c.toPadId);
            if (fromPad && toPad) {
                if ((fromPad->x == result.path.first().x &&
                     fromPad->y == result.path.first().y) &&
                    (toPad->x == result.path.last().x &&
                     toPad->y == result.path.last().y)) {
                    conn = &c;
                    break;
                }
            }
        }

        if (!conn || conn->routed) continue;

        // Проверяем, можно ли разместить путь без конфликтов
        bool canPlace = true;
        {
            QMutexLocker locker(&gridMutex);

            for (const GridPoint& point : result.path) {
                // Пропускаем начальную и конечную точки (площадки)
                Pad* fromPad = getPadById(conn->fromPadId);
                Pad* toPad = getPadById(conn->toPadId);

                if ((fromPad && point.x == fromPad->x && point.y == fromPad->y) ||
                    (toPad && point.x == toPad->x && point.y == toPad->y)) {
                    continue;
                }

                GridCell& cell = grid[point.layer][point.y][point.x];

                // Если ячейка уже занята другой трассой - конфликт
                if ((cell.type == CELL_TRACE || cell.type == CELL_VIA) &&
                    cell.traceId != conn->fromPadId) {
                    canPlace = false;
                    break;
                }
            }

            if (canPlace) {
                // Размещаем трассу
                for (int i = 0; i < result.path.size(); i++) {
                    const GridPoint& point = result.path[i];
                    GridCell& cell = grid[point.layer][point.y][point.x];

                    // Пропускаем площадки
                    if (cell.type == CELL_PAD) {
                        continue;
                    }

                    // Определяем, является ли это VIA
                    bool isVia = false;
                    if (i > 0) {
                        const GridPoint& prev = result.path[i - 1];
                        if (prev.x == point.x && prev.y == point.y &&
                            prev.layer != point.layer) {
                            isVia = true;
                        }
                    }

                    if (isVia) {
                        // Для VIA помечаем на обоих слоях
                        for (int l = 0; l < layerCount; l++) {
                            if (l == point.layer || (i > 0 && l == result.path[i-1].layer)) {
                                grid[l][point.y][point.x].type = CELL_VIA;
                                grid[l][point.y][point.x].traceId = conn->fromPadId;
                            }
                        }
                    } else {
                        cell.type = CELL_TRACE;
                        cell.traceId = conn->fromPadId;
                    }
                }

                // Обновляем соединение
                conn->routed = true;
                conn->layer = result.path.first().layer;

                // Скрываем линию связи
                if (conn->visualLine) {
                    conn->visualLine->hide();
                }

                routedCount++;

                // Рисуем линии трасс
                for (int i = 0; i < result.path.size() - 1; i++) {
                    drawTraceLine(result.path[i], result.path[i + 1], result.path[i].layer);
                }

                qDebug() << "Успешно размещена трасса:"
                         << getPadById(conn->fromPadId)->name
                         << "->" << getPadById(conn->toPadId)->name;
            } else {
                failedConnections.append(conn->fromPadId);
                qDebug() << "Конфликт при размещении трассы";
            }
        }

        // Обновляем GUI между трассами
        drawGrid();
        QApplication::processEvents();
    }

    qDebug() << "Успешно проложено:" << routedCount << "трасс";
}

void MainWindow::routeSingleConnection(Connection& conn)
{
    Pad* fromPad = getPadById(conn.fromPadId);
    Pad* toPad = getPadById(conn.toPadId);

    if (!fromPad || !toPad || conn.routed) {
        return;
    }

    // Находим доступные слои для площадок
    int startLayer = 0;
    int endLayer = 0;

    for (int l = 0; l < layerCount; l++) {
        if (grid[l][fromPad->y][fromPad->x].type == CELL_PAD) {
            startLayer = l;
            break;
        }
    }

    for (int l = 0; l < layerCount; l++) {
        if (grid[l][toPad->y][toPad->x].type == CELL_PAD) {
            endLayer = l;
            break;
        }
    }

    GridPoint start(fromPad->x, fromPad->y, startLayer);
    GridPoint end(toPad->x, toPad->y, endLayer);

    QList<GridPoint> path;
    {
        QMutexLocker locker(&gridMutex);
        path = pathFinder.findPath(start, end, grid,
                                  boardWidth, boardHeight,
                                  layerCount, fromPad->id, toPad->id);
    }

    if (!path.isEmpty()) {
        // Пытаемся разместить трассу с блокировкой
        QMutexLocker locker(&gridMutex);

        // Проверяем, не заняты ли ячейки другими трассами
        bool canPlace = true;
        for (const GridPoint& point : path) {
            GridCell& cell = grid[point.layer][point.y][point.x];

            // Пропускаем площадки
            if (cell.type == CELL_PAD) {
                continue;
            }

            // Проверяем, не занята ли ячейка чужой трассой
            if ((cell.type == CELL_TRACE || cell.type == CELL_VIA) &&
                cell.traceId != fromPad->id) {
                canPlace = false;
                break;
            }
        }

        if (canPlace) {
            // Размещаем трассу
            for (int i = 0; i < path.size(); i++) {
                const GridPoint& point = path[i];
                GridCell& cell = grid[point.layer][point.y][point.x];

                if (cell.type == CELL_PAD) {
                    continue;
                }

                // Определяем VIA
                bool isVia = false;
                if (i > 0) {
                    const GridPoint& prev = path[i-1];
                    if (prev.x == point.x && prev.y == point.y &&
                        prev.layer != point.layer) {
                        isVia = true;
                    }
                }

                if (isVia) {
                    cell.type = CELL_VIA;
                } else {
                    cell.type = CELL_TRACE;
                }
                cell.traceId = fromPad->id;
            }

            conn.routed = true;
            conn.layer = startLayer;

            // Отправляем сигнал для отрисовки в основном потоке
            QMetaObject::invokeMethod(this, [this, path]() {
                for (int i = 0; i < path.size() - 1; i++) {
                    drawTraceLine(path[i], path[i + 1], path[i].layer);
                }
            }, Qt::QueuedConnection);
        }
    }
}

int MainWindow::findOptimalLayer(int x, int y)
{
    // Ищем слой с наименьшим количеством трасс вокруг
    int bestLayer = 0;
    int bestScore = INT_MAX;

    for (int layer = 0; layer < layerCount; layer++) {
        if (grid[layer][y][x].type == CELL_PAD) {
            // Считаем количество трасс в радиусе 3 клеток
            int score = 0;
            for (int dy = -3; dy <= 3; dy++) {
                for (int dx = -3; dx <= 3; dx++) {
                    int nx = x + dx;
                    int ny = y + dy;

                    if (nx >= 0 && nx < boardWidth && ny >= 0 && ny < boardHeight) {
                        if (grid[layer][ny][nx].type == CELL_TRACE ||
                            grid[layer][ny][nx].type == CELL_VIA) {
                            score++;
                        }
                    }
                }
            }

            if (score < bestScore) {
                bestScore = score;
                bestLayer = layer;
            }
        }
    }

    return bestLayer;
}

// Вспомогательные функции
GridCell*** MainWindow::createTempGrid()
{
    GridCell*** tempGrid = new GridCell**[layerCount];
    for (int l = 0; l < layerCount; l++) {
        tempGrid[l] = new GridCell*[boardHeight];
        for (int y = 0; y < boardHeight; y++) {
            tempGrid[l][y] = new GridCell[boardWidth];
            for (int x = 0; x < boardWidth; x++) {
                tempGrid[l][y][x] = grid[l][y][x];
            }
        }
    }
    return tempGrid;
}

void MainWindow::freeTempGrid(GridCell*** tempGrid)
{
    if (!tempGrid) return;

    for (int l = 0; l < layerCount; l++) {
        for (int y = 0; y < boardHeight; y++) {
            delete[] tempGrid[l][y];
        }
        delete[] tempGrid[l];
    }
    delete[] tempGrid;
}

void MainWindow::onClearTraces()
{
    // Очищаем линии трасс
    for (TraceLineInfo& info : traceLinesInfo) {
        if (info.line) {
            scene->removeItem(info.line);
            delete info.line;
        }
    }
    traceLinesInfo.clear();

    // Также удаляем все виа-маркеры (черные кружки)
    QList<QGraphicsItem*> items = scene->items();
    for (QGraphicsItem* item : items) {
        if (QGraphicsEllipseItem* ellipse = dynamic_cast<QGraphicsEllipseItem*>(item)) {
            // Проверяем, является ли это виа-маркером
            QPen pen = ellipse->pen();
            if (pen.color() == Qt::black && ellipse->brush().color() == Qt::black) {
                scene->removeItem(ellipse);
                delete ellipse;
            }
        }
    }

    // Очищаем ячейки трасс и виа
    for (int l = 0; l < layerCount; l++) {
        for (int y = 0; y < boardHeight; y++) {
            for (int x = 0; x < boardWidth; x++) {
                if (grid[l][y][x].type == CELL_TRACE || grid[l][y][x].type == CELL_VIA) {
                    grid[l][y][x].type = CELL_EMPTY;
                    grid[l][y][x].traceId = -1;
                    grid[l][y][x].color = Qt::white;
                    updateCellDisplay(x, y, l);
                }
            }
        }
    }

    // Сбрасываем статус связей и восстанавливаем стандартное отображение
    for (Connection& conn : connections) {
        conn.routed = false;
        conn.layer = -1;

        // Восстанавливаем стандартный вид линии (черный пунктир)
        if (conn.visualLine) {
            conn.visualLine->setPen(QPen(Qt::black, 1, Qt::DashLine));
            conn.visualLine->setZValue(0); // Возвращаем стандартный уровень
        }
    }

    // Очищаем список неудачных трасс
    failedConnections.clear();

    // Восстанавливаем линии связей (если они были удалены)
    updateConnectionLines();

    drawGrid();
    ui->statusBar->showMessage("Все трассы и переходные отверстия удалены");
}

void MainWindow::onRemovePad()
{
    currentMode = MODE_NONE;

    if (pads.isEmpty()) {
        QMessageBox::information(this, "Информация", "Нет площадок для удаления");
        return;
    }

    QStringList padNames;
    for (const Pad& pad : pads) {
        padNames << QString("%1 (%2,%3)").arg(pad.name).arg(pad.x).arg(pad.y);
    }

    bool ok;
    QString selected = QInputDialog::getItem(
        this,
        "Удаление площадки",
        "Выберите площадку для удаления:",
        padNames,
        0,
        false,
        &ok
    );

    if (ok && !selected.isEmpty()) {
        int index = padNames.indexOf(selected);
        if (index != -1) {
            int padId = pads[index].id;

            // Удаление связей
            QList<Connection> newConnections;
            for (const Connection& conn : connections) {
                if (conn.fromPadId != padId && conn.toPadId != padId) {
                    newConnections.append(conn);
                } else {
                    // Удаляем визуальную линию
                    if (conn.visualLine) {
                        scene->removeItem(conn.visualLine);
                        delete conn.visualLine;
                    }
                }
            }
            connections = newConnections;

            // Удаление из сетки
            for (int l = 0; l < layerCount; l++) {
                for (int y = 0; y < boardHeight; y++) {
                    for (int x = 0; x < boardWidth; x++) {
                        if (grid[l][y][x].padId == padId) {
                            grid[l][y][x].type = CELL_EMPTY;
                            grid[l][y][x].padId = -1;
                            grid[l][y][x].color = Qt::white;
                        }
                    }
                }
            }

            pads.removeAt(index);
            drawGrid();
            updateConnectionLines(); // Обновляем линии связей
            ui->statusBar->showMessage("Площадка удалена");
        }
    }
}

void MainWindow::onLayerCountChanged(int count)
{
    // Сохраняем текущие данные
    QList<Pad> oldPads = pads;
    QList<Connection> oldConnections = connections;
    int oldNextPadId = nextPadId;

    // СОХРАНЯЕМ ПРЕПЯТСТВИЯ
    // Собираем информацию о всех препятствиях
    struct ObstacleInfo {
        int x;
        int y;
        int layer;
    };
    QList<ObstacleInfo> oldObstacles;

    for (int l = 0; l < layerCount; l++) {
        for (int y = 0; y < boardHeight; y++) {
            for (int x = 0; x < boardWidth; x++) {
                if (grid[l][y][x].type == CELL_OBSTACLE) {
                    ObstacleInfo obs;
                    obs.x = x;
                    obs.y = y;
                    obs.layer = l;
                    oldObstacles.append(obs);
                }
            }
        }
    }

    // Очищаем сетку
    clearGrid();

    // Устанавливаем новое количество слоев
    layerCount = qMin(count, 8);
    layerCount = qMax(layerCount, 1);

    ui->layerSpinBox->setValue(layerCount);

    // Обновляем радио-кнопки активного слоя
    createLayerRadioButtons();

    // Инициализируем новую сетку
    initGrid();

    // ВОССТАНАВЛИВАЕМ ПРЕПЯТСТВИЯ на соответствующих слоях
    // Если старый слой существует в новой конфигурации, восстанавливаем препятствие
    // Если слой был удален, препятствие теряется (или можно перенести на ближайший слой)
    for (const ObstacleInfo& obs : oldObstacles) {
        if (obs.layer < layerCount) {
            // Слой существует - восстанавливаем препятствие
            grid[obs.layer][obs.y][obs.x].type = CELL_OBSTACLE;
            grid[obs.layer][obs.y][obs.x].color = Qt::darkGray;
        } else {
            // Слой был удален - переносим препятствие на последний доступный слой
            int targetLayer = layerCount - 1;
            grid[targetLayer][obs.y][obs.x].type = CELL_OBSTACLE;
            grid[targetLayer][obs.y][obs.x].color = Qt::darkGray;
        }
    }

    // Восстанавливаем площадки на всех слоях
    for (const Pad& oldPad : oldPads) {
        if (oldPad.x < boardWidth && oldPad.y < boardHeight) {
            Pad pad = oldPad;

            // Устанавливаем на всех слоях
            for (int l = 0; l < layerCount; l++) {
                grid[l][pad.y][pad.x].type = CELL_PAD;
                grid[l][pad.y][pad.x].padId = pad.id;
                grid[l][pad.y][pad.x].color = pad.color;
                grid[l][pad.y][pad.x].traceId = -1;
            }

            pads.append(pad);
        }
    }

    // Восстанавливаем nextPadId
    nextPadId = oldNextPadId;

    // Восстанавливаем связи
    for (const Connection& oldConn : oldConnections) {
        if (getPadById(oldConn.fromPadId) && getPadById(oldConn.toPadId)) {
            connections.append(oldConn);
        }
    }

    // Обновляем отображение
    drawGrid();
    updateConnectionLines();

    // УБРАНО всплывающее окно, оставляем только сообщение в статусной строке
    QString message = QString("Установлено %1 слоев").arg(layerCount);

    // Если были потеряны препятствия из-за уменьшения слоев, сообщаем об этом
    int lostObstacles = 0;
    for (const ObstacleInfo& obs : oldObstacles) {
        if (obs.layer >= layerCount) {
            lostObstacles++;
        }
    }

    if (lostObstacles > 0) {
        message += QString(". %1 препятствий перемещено на другие слои").arg(lostObstacles);
    }

    ui->statusBar->showMessage(message);
}

void MainWindow::onBoardSizeChanged()
{
    BoardSizeDialog dialog(this, boardWidth, boardHeight);

    if (dialog.exec() == QDialog::Accepted) {
        int newWidth = dialog.getWidth();
        int newHeight = dialog.getHeight();

        if (newWidth != boardWidth || newHeight != boardHeight) {
            if (!pads.isEmpty()) {
                QMessageBox::StandardButton reply = QMessageBox::question(
                    this,
                    "Подтверждение",
                    "При изменении размера платы все данные (площадки, препятствия, трассы) будут удалены. Продолжить?",
                    QMessageBox::Yes | QMessageBox::No
                );

                if (reply == QMessageBox::No) {
                    return;
                }
            }

            // Сохраняем nextPadId перед очисткой
            int oldNextPadId = nextPadId;

            // Очищаем ВСЕ элементы сцены
            scene->clear();  // ОЧЕНЬ ВАЖНО: очищаем сцену полностью

            // Очищаем данные сетки
            clearGrid();

            // Важно: после scene->clear() нужно сбросить указатели
            cells = nullptr;
            grid = nullptr;

            // Устанавливаем новые размеры
            boardWidth = newWidth;
            boardHeight = newHeight;

            // Инициализируем новую сетку
            initGrid();

            // Восстанавливаем nextPadId
            nextPadId = oldNextPadId;

            drawGrid();

            ui->statusBar->showMessage(QString("Размер платы изменен: %1x%2").arg(boardWidth).arg(boardHeight));
        }
    }
}

void MainWindow::showFailedConnections()
{
    if (failedConnections.isEmpty()) {
        QMessageBox::information(this, "Трассировка",
            "Все соединения успешно проложены!");
        return;
    }

    QStringList failedList;
    QSet<int> uniqueFailed;

    // Собираем уникальные неудачные соединения
    for (int connId : failedConnections) {
        for (const Connection& conn : connections) {
            if ((conn.fromPadId == connId || conn.toPadId == connId) &&
                !conn.routed && !uniqueFailed.contains(conn.fromPadId)) {
                Pad* pad1 = getPadById(conn.fromPadId);
                Pad* pad2 = getPadById(conn.toPadId);
                if (pad1 && pad2) {
                    failedList << QString("%1 (%2,%3) → %4 (%5,%6)")
                        .arg(pad1->name).arg(pad1->x).arg(pad1->y)
                        .arg(pad2->name).arg(pad2->x).arg(pad2->y);
                    uniqueFailed.insert(conn.fromPadId);
                }
                break;
            }
        }
    }

    QString message = QString("<h3>Не удалось проложить %1 соединений:</h3>").arg(failedList.size());
    message += QString("<table border='1' cellpadding='5' style='border-collapse: collapse; width: 100%;'>");
    message += QString("<tr><th>№</th><th>Соединение</th><th>Координаты</th></tr>");

    for (int i = 0; i < failedList.size(); ++i) {
        message += QString("<tr><td align='center'>%1</td><td>%2</td><td align='center'>См. выше</td></tr>")
                   .arg(i + 1).arg(failedList[i]);
    }
    message += QString("</table>");

    // Создаем диалоговое окно с увеличенными размерами
    QDialog dialog(this);
    dialog.setWindowTitle("Неудачные соединения");
    dialog.setMinimumSize(600, 400);

    QVBoxLayout *layout = new QVBoxLayout(&dialog);

    QTextBrowser *textBrowser = new QTextBrowser();
    textBrowser->setHtml(message);
    textBrowser->setOpenExternalLinks(false);
    textBrowser->setMinimumHeight(250);

    QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);

    layout->addWidget(textBrowser);
    layout->addWidget(buttonBox);

    dialog.exec();
}

void MainWindow::onRemoveObstacle()
{
    currentMode = MODE_NONE;

    for (int y = 0; y < boardHeight; y++) {
        for (int x = 0; x < boardWidth; x++) {
            for (int l = 0; l < layerCount; l++) {
                if (grid[l][y][x].type == CELL_OBSTACLE) {
                    grid[l][y][x].type = CELL_EMPTY;
                    grid[l][y][x].color = Qt::white;
                }
            }
        }
    }

    drawGrid();
    ui->statusBar->showMessage("Все препятствия удалены");
}

void MainWindow::setModeObstacle()
{
    currentMode = MODE_OBSTACLE;
    ui->statusBar->showMessage("Режим: установка препятствий. Кликните на ячейку.");
}

void MainWindow::setModePad()
{
    currentMode = MODE_PAD;
    ui->statusBar->showMessage("Режим: установка контактных площадок. Кликните на ячейку.");
}

void MainWindow::setModeConnection()
{
    currentMode = MODE_CONNECTION;
    selectedPadId = -1;
    ui->statusBar->showMessage("Режим: создание связей. Выберите первую площадку.");
}

void MainWindow::onSetObstacle()
{
    setModeObstacle();
}

void MainWindow::onSetPad()
{
    setModePad();
}

void MainWindow::onSetConnection()
{
    setModeConnection();
}

// Исправьте объявления функций в конце mainwindow.cpp:
QList<GridPoint> MainWindow::findPath(const GridPoint& start, const GridPoint& end, int fromPadId, int toPadId)
{
    return pathFinder.findPath(start, end, grid, boardWidth, boardHeight, layerCount, fromPadId, toPadId);
}

void MainWindow::placeVia(int x, int y)
{
    // Проверяем, не находится ли точка на площадке
    if (hasPadAt(x, y)) {
        return; // Не размещаем VIA на площадке
    }

    for (int l = 0; l < layerCount; l++) {
        // Не ставим переход на препятствия или другие трассы
        if (grid[l][y][x].type == CELL_OBSTACLE ||
            grid[l][y][x].type == CELL_PAD ||
            grid[l][y][x].type == CELL_TRACE) {
            continue;
        }

        // Изменяем: устанавливаем как TRACE, а не VIA
        grid[l][y][x].type = CELL_TRACE;  // Было CELL_VIA
        grid[l][y][x].color = Qt::white;  // Было Qt::black
        updateCellDisplay(x, y, l);
    }
}

Pad* MainWindow::getPadById(int id)
{
    for (int i = 0; i < pads.size(); i++) {
        if (pads[i].id == id) {
            return &pads[i];
        }
    }
    return nullptr;
}

int MainWindow::getPadAt(int x, int y)
{
    // Просто ищем в списке площадок
    for (const Pad& pad : pads) {
        if (pad.x == x && pad.y == y) {
            return pad.id;
        }
    }
    return -1;
}

bool MainWindow::hasPadAt(int x, int y)
{
    // Простая проверка по списку площадок
    for (const Pad& pad : pads) {
        if (pad.x == x && pad.y == y) {
            return true;
        }
    }

    // Дополнительная проверка по сетке (на всякий случай)
    for (int l = 0; l < layerCount; l++) {
        if (grid[l][y][x].type == CELL_PAD) {
            return true;
        }
    }

    return false;
}

void MainWindow::updatePadConnections()
{
    // Очищаем существующие связи в площадках
    for (Pad& pad : pads) {
        pad.connections.clear();
    }

    // Обновляем связи на основе connections
    for (const Connection& conn : connections) {
        Pad* pad1 = getPadById(conn.fromPadId);
        Pad* pad2 = getPadById(conn.toPadId);

        if (pad1 && pad2) {
            if (!pad1->connections.contains(conn.toPadId)) {
                pad1->connections.append(conn.toPadId);
            }
            if (!pad2->connections.contains(conn.fromPadId)) {
                pad2->connections.append(conn.fromPadId);
            }
        }
    }
}

QColor MainWindow::getLayerColor(int layer, bool isActiveLayer)
{
    if (layer < 0 || layer >= layerColors.size()) {
        return Qt::white;
    }

    QColor baseColor = layerColors[layer];

    if (isActiveLayer) {
        // Яркий цвет для активного слоя
        return baseColor.lighter(150);
    } else {
        // Тусклый цвет для неактивных слоев
        QColor dimmed = baseColor.darker(200);
        dimmed.setAlpha(150); // Полупрозрачный
        return dimmed;
    }
}

void MainWindow::removeConnectionLines()
{
    // Удаляем все визуальные линии связей
    for (Connection& conn : connections) {
        if (conn.visualLine) {
            scene->removeItem(conn.visualLine);
            delete conn.visualLine;
            conn.visualLine = nullptr;
        }
    }
}

void MainWindow::onAutoFill()
{
    if (!pads.isEmpty() || !connections.isEmpty()) {
        QMessageBox::StandardButton reply = QMessageBox::question(
            this,
            "Авто-заполнение",
            "Все существующие данные будут удалены. Продолжить?",
            QMessageBox::Yes | QMessageBox::No
        );

        if (reply == QMessageBox::No) {
            return;
        }
    }

    // ПОЛНОСТЬЮ ОЧИЩАЕМ ВСЕ СУЩЕСТВУЮЩИЕ ДАННЫЕ

    // 1. Очищаем трассы
    onClearTraces();

    // 2. Удаляем все визуальные линии связей
    for (Connection& conn : connections) {
        if (conn.visualLine) {
            scene->removeItem(conn.visualLine);
            delete conn.visualLine;
            conn.visualLine = nullptr;
        }
    }

    // 3. Очищаем все площадки из сетки
    for (int l = 0; l < layerCount; l++) {
        for (int y = 0; y < boardHeight; y++) {
            for (int x = 0; x < boardWidth; x++) {
                if (grid[l][y][x].type == CELL_PAD) {
                    grid[l][y][x].type = CELL_EMPTY;
                    grid[l][y][x].padId = -1;
                    grid[l][y][x].color = Qt::white;
                }
            }
        }
    }

    // 4. Удаляем препятствия
    for (int l = 0; l < layerCount; l++) {
        for (int y = 0; y < boardHeight; y++) {
            for (int x = 0; x < boardWidth; x++) {
                if (grid[l][y][x].type == CELL_OBSTACLE) {
                    grid[l][y][x].type = CELL_EMPTY;
                    grid[l][y][x].color = Qt::white;
                }
            }
        }
    }

    // 5. Сбрасываем списки
    pads.clear();
    connections.clear();
    selectedPadId = -1;

    // 6. Сбрасываем nextPadId на 1 для нового автозаполнения
    nextPadId = 1;

    // 7. Обновляем отображение сетки
    drawGrid();

    // 8. Выполняем авто-заполнение с чистой платы
    autoFillBoard();

    // 9. Обновляем отображение
    drawGrid();
    updateConnectionLines();
    ui->statusBar->showMessage("Авто-заполнение завершено");
}

void MainWindow::autoFillBoard()
{
    qsrand(static_cast<uint>(QTime::currentTime().msec()));

    // ВАЖНО: Сбрасываем ID площадок при каждом новом автозаполнении
    if (pads.isEmpty()) {
        nextPadId = 1;
    }

    // Очищаем визуальные элементы, если они остались
    QList<QGraphicsItem*> allItems = scene->items();
    for (QGraphicsItem* item : allItems) {
        // Удаляем только линии связей (черные пунктиры)
        if (QGraphicsLineItem* lineItem = dynamic_cast<QGraphicsLineItem*>(item)) {
            QPen pen = lineItem->pen();
            if (pen.color() == Qt::black && pen.style() == Qt::DashLine && pen.width() == 1) {
                scene->removeItem(lineItem);
                delete lineItem;
            }
        }
    }

    int boardArea = boardWidth * boardHeight;

    // Уменьшаем плотность элементов для лучшей трассировки
    int maxElements = boardArea / 10;

    // 1. Добавляем случайные площадки
    int minPadCount = qMax(3, boardArea * 3 / 100);
    int maxPadCount = qMin(maxElements / 2, boardArea * 8 / 100);
    int padCount = minPadCount + qrand() % (maxPadCount - minPadCount + 1);
    padCount = qMin(padCount, boardArea / 8);

    qDebug() << "Добавляем" << padCount << "площадок";

    // Список для хранения ID созданных площадок
    QList<int> padIds;

    for (int i = 0; i < padCount; i++) {
        int attempts = 0;
        bool placed = false;

        while (!placed && attempts < 150) {
            int x = qrand() % boardWidth;
            int y = qrand() % boardHeight;

            // Проверяем, свободна ли ячейка
            bool free = true;
            for (int l = 0; l < layerCount; l++) {
                if (grid[l][y][x].type != CELL_EMPTY) {
                    free = false;
                    break;
                }
            }

            // Проверяем расстояние до других площадок
            if (free) {
                bool tooClose = false;
                for (const Pad& pad : pads) {
                    int dist = abs(pad.x - x) + abs(pad.y - y);
                    if (dist < 3) {
                        tooClose = true;
                        break;
                    }
                }

                if (!tooClose) {
                    // Создаем площадку
                    Pad pad;
                    pad.id = nextPadId;
                    pad.x = x;
                    pad.y = y;
                    pad.name = QString("AUTO%1").arg(nextPadId);

                    // Генерируем случайный цвет
                    static int hue = 0;
                    pad.color = QColor::fromHsv(hue, 180, 220);
                    hue = (hue + 40) % 360;

                    // Устанавливаем на всех слоях
                    for (int l = 0; l < layerCount; l++) {
                        grid[l][pad.y][pad.x].type = CELL_PAD;
                        grid[l][pad.y][pad.x].padId = pad.id;
                        grid[l][pad.y][pad.x].color = pad.color;
                        grid[l][pad.y][pad.x].traceId = -1;
                    }

                    pads.append(pad);
                    padIds.append(pad.id);
                    nextPadId++;
                    placed = true;
                }
            }
            attempts++;
        }
    }

    // 2. Добавляем случайные препятствия
    int minObstacleCount = qMax(1, boardArea * 2 / 100);
    int maxObstacleCount = qMin(maxElements - pads.size(), boardArea * 4 / 100);
    int obstacleCount = minObstacleCount + qrand() % (maxObstacleCount - minObstacleCount + 1);

    for (int i = 0; i < obstacleCount; i++) {
        int attempts = 0;
        bool placed = false;

        while (!placed && attempts < 100) {
            int type = qrand() % 4;
            int size;

            if (type == 0) {
                size = 1;
            } else {
                size = 2 + qrand() % 3;
            }

            int startX = qrand() % boardWidth;
            int startY = qrand() % boardHeight;

            QList<GridPoint> obstaclePoints = generateObstacle(size, type);
            bool canPlace = true;

            for (const GridPoint& point : obstaclePoints) {
                int x = startX + point.x;
                int y = startY + point.y;

                if (x < 0 || x >= boardWidth || y < 0 || y >= boardHeight) {
                    canPlace = false;
                    break;
                }

                if (hasPadAt(x, y)) {
                    canPlace = false;
                    break;
                }

                for (int l = 0; l < layerCount; l++) {
                    if (grid[l][y][x].type == CELL_OBSTACLE) {
                        canPlace = false;
                        break;
                    }
                }

                if (!canPlace) break;
            }

            if (canPlace) {
                int obstacleLayer = qrand() % layerCount;
                for (const GridPoint& point : obstaclePoints) {
                    int x = startX + point.x;
                    int y = startY + point.y;
                    grid[obstacleLayer][y][x].type = CELL_OBSTACLE;
                    grid[obstacleLayer][y][x].color = Qt::darkGray;
                }
                placed = true;
            }
            attempts++;
        }
    }

    // 3. ГАРАНТИРУЕМ, что каждая площадка имеет хотя бы одну связь
    connections.clear(); // Очищаем существующие связи

    if (pads.size() >= 2) {
        qDebug() << "Создаем связи для" << pads.size() << "площадок";

        // Шаг 1: Создаем минимальное связующее дерево (MST) с алгоритмом Прима
        // Это гарантирует, что все площадки будут связаны
        QSet<int> connectedSet;
        QList<QPair<int, int>> mstEdges; // Пары индексов площадок

        if (!pads.isEmpty()) {
            // Начинаем с первой площадки
            connectedSet.insert(0); // Индекс первой площадки в списке pads

            while (connectedSet.size() < pads.size()) {
                int minDistance = INT_MAX;
                int bestConnectedIdx = -1;
                int bestUnconnectedIdx = -1;

                // Ищем ближайшую несвязанную площадку к любой из связанных
                for (int connectedIdx : connectedSet) {
                    for (int j = 0; j < pads.size(); j++) {
                        if (!connectedSet.contains(j)) {
                            int dist = abs(pads[connectedIdx].x - pads[j].x) +
                                      abs(pads[connectedIdx].y - pads[j].y);
                            if (dist < minDistance) {
                                minDistance = dist;
                                bestConnectedIdx = connectedIdx;
                                bestUnconnectedIdx = j;
                            }
                        }
                    }
                }

                if (bestConnectedIdx != -1 && bestUnconnectedIdx != -1) {
                    // Добавляем ребро в MST
                    mstEdges.append(qMakePair(bestConnectedIdx, bestUnconnectedIdx));
                    connectedSet.insert(bestUnconnectedIdx);
                    qDebug() << "MST: связываем" << pads[bestConnectedIdx].name
                             << "с" << pads[bestUnconnectedIdx].name;
                } else {
                    break;
                }
            }
        }

        // Шаг 2: Добавляем все ребра MST в connections
        for (const auto& edge : mstEdges) {
            int i = edge.first;
            int j = edge.second;

            Connection conn;
            conn.fromPadId = pads[i].id;
            conn.toPadId = pads[j].id;
            conn.routed = false;
            conn.layer = -1;
            conn.visualLine = nullptr;

            connections.append(conn);

            // Обновляем связи в площадках
            if (!pads[i].connections.contains(pads[j].id)) {
                pads[i].connections.append(pads[j].id);
            }
            if (!pads[j].connections.contains(pads[i].id)) {
                pads[j].connections.append(pads[i].id);
            }
        }

        qDebug() << "Создано MST связей: " << mstEdges.size();

        // Шаг 3: Добавляем дополнительные случайные связи для разнообразия
        // Цель: примерно 2-3 связи на площадку в среднем
        int targetConnections = qMin(pads.size() * 3, pads.size() * (pads.size() - 1) / 2);
        int additionalNeeded = targetConnections - connections.size();

        if (additionalNeeded > 0) {
            int added = 0;
            int attempts = 0;
            const int MAX_ATTEMPTS = 200;

            while (added < additionalNeeded && attempts < MAX_ATTEMPTS) {
                int i = qrand() % pads.size();
                int j = qrand() % pads.size();

                if (i == j) continue;

                // Проверяем, нет ли уже такой связи
                bool exists = false;
                for (const Connection& conn : connections) {
                    if ((conn.fromPadId == pads[i].id && conn.toPadId == pads[j].id) ||
                        (conn.fromPadId == pads[j].id && conn.toPadId == pads[i].id)) {
                        exists = true;
                        break;
                    }
                }

                if (!exists) {
                    // Проверяем расстояние - не слишком ли далеко
                    int dist = abs(pads[i].x - pads[j].x) + abs(pads[i].y - pads[j].y);
                    // Максимальное расстояние: половина диагонали платы
                    int maxDist = (boardWidth + boardHeight) / 2;

                    if (dist <= maxDist) {
                        Connection conn;
                        conn.fromPadId = pads[i].id;
                        conn.toPadId = pads[j].id;
                        conn.routed = false;
                        conn.layer = -1;
                        conn.visualLine = nullptr;

                        connections.append(conn);

                        // Обновляем связи в площадках
                        if (!pads[i].connections.contains(pads[j].id)) {
                            pads[i].connections.append(pads[j].id);
                        }
                        if (!pads[j].connections.contains(pads[i].id)) {
                            pads[j].connections.append(pads[i].id);
                        }

                        added++;
                        qDebug() << "Доп. связь:" << pads[i].name << "-" << pads[j].name;
                    }
                }

                attempts++;
            }

            qDebug() << "Добавлено дополнительных связей: " << added;
        }

        // Шаг 4: Финальная проверка - гарантируем, что каждая площадка имеет хотя бы одну связь
        QMap<int, int> connectionCount; // ID площадки -> количество связей
        for (const Connection& conn : connections) {
            connectionCount[conn.fromPadId]++;
            connectionCount[conn.toPadId]++;
        }

        qDebug() << "Статистика связей по площадкам:";
        for (const Pad& pad : pads) {
            int count = connectionCount.value(pad.id, 0);
            qDebug() << "  " << pad.name << ":" << count << "связей";

            // Если площадка без связей (на всякий случай)
            if (count == 0) {
                qDebug() << "  ВНИМАНИЕ: Площадка" << pad.name << "не имеет связей!";

                // Ищем ближайшую площадку для связи
                int closestIdx = -1;
                int minDist = INT_MAX;

                for (int j = 0; j < pads.size(); j++) {
                    if (pad.id != pads[j].id) {
                        int dist = abs(pad.x - pads[j].x) + abs(pad.y - pads[j].y);
                        if (dist < minDist) {
                            minDist = dist;
                            closestIdx = j;
                        }
                    }
                }

                if (closestIdx != -1) {
                    Connection conn;
                    conn.fromPadId = pad.id;
                    conn.toPadId = pads[closestIdx].id;
                    conn.routed = false;
                    conn.layer = -1;
                    conn.visualLine = nullptr;

                    connections.append(conn);

                    // Обновляем связи в площадках
                    if (!pad.connections.contains(pads[closestIdx].id)) {
                        // Находим пад в списке и обновляем его connections
                        for (Pad& p : pads) {
                            if (p.id == pad.id) {
                                p.connections.append(pads[closestIdx].id);
                                break;
                            }
                        }
                    }
                    if (!pads[closestIdx].connections.contains(pad.id)) {
                        pads[closestIdx].connections.append(pad.id);
                    }

                    qDebug() << "  Добавлена связь для" << pad.name << "с" << pads[closestIdx].name;
                }
            }
        }

        // Обновляем визуальные линии связей
        updateConnectionLines();

        qDebug() << "Итого создано связей: " << connections.size();
        qDebug() << "Всего площадок: " << pads.size();
        qDebug() << "Среднее количество связей на площадку: "
                 << (pads.size() > 0 ? static_cast<double>(connections.size()) * 2 / pads.size() : 0);
    } else if (pads.size() == 1) {
        qDebug() << "Только одна площадка, связи не создаются";
    } else {
        qDebug() << "Нет площадок для создания связей";
    }
}

QList<GridPoint> MainWindow::generateObstacle(int size, int type)
{
    QList<GridPoint> points;

    switch (type) {
    case 0: // Горизонтальное
        for (int i = 0; i < size; i++) {
            points.append(GridPoint(i, 0));
        }
        break;

    case 1: // Вертикальное
        for (int i = 0; i < size; i++) {
            points.append(GridPoint(0, i));
        }
        break;

    case 2: // L-образное (минимум 3x3)
        size = qMax(size, 3);
        // Горизонтальная часть
        for (int i = 0; i < size; i++) {
            points.append(GridPoint(i, 0));
        }
        // Вертикальная часть
        for (int i = 1; i < size; i++) {
            points.append(GridPoint(0, i));
        }
        break;
    }

    return points;
}

QList<ConnectionRequest> MainWindow::prepareConnectionRequests()
{
    QList<ConnectionRequest> requests;

    for (const Connection& conn : connections) {
        if (conn.routed) continue;

        Pad* fromPad = getPadById(conn.fromPadId);
        Pad* toPad = getPadById(conn.toPadId);

        if (!fromPad || !toPad) continue;

        ConnectionRequest request;
        request.fromPadId = conn.fromPadId;
        request.toPadId = conn.toPadId;

        // Находим доступные слои для старта
        int startLayer = findLayerForPad(fromPad->x, fromPad->y);
        int endLayer = findLayerForPad(toPad->x, toPad->y);

        request.start = GridPoint(fromPad->x, fromPad->y, startLayer);
        request.end = GridPoint(toPad->x, toPad->y, endLayer);
        request.priority = abs(fromPad->x - toPad->x) + abs(fromPad->y - toPad->y);

        requests.append(request);
    }

    // Сортируем по приоритету (короткие пути первыми)
    std::sort(requests.begin(), requests.end(),
              [](const ConnectionRequest& a, const ConnectionRequest& b) {
                  return a.priority < b.priority;
              });

    return requests;
}

void MainWindow::applyRoutingResults(const QList<RoutingResult>& results) {
    QMutexLocker locker(&gridMutex);

    for (const RoutingResult& result : results) {
        if (!result.success || result.path.isEmpty()) {
            continue;
        }

        // Обновляем сетку
        for (int i = 0; i < result.path.size(); i++) {
            const GridPoint& point = result.path[i];

            if (point.x < 0 || point.x >= boardWidth ||
                point.y < 0 || point.y >= boardHeight ||
                point.layer < 0 || point.layer >= layerCount) {
                continue;
            }

            GridCell& cell = grid[point.layer][point.y][point.x];

            // Пропускаем площадки
            if (cell.type == CELL_PAD) {
                continue;
            }

            // Определяем VIA
            bool isVia = false;
            if (i > 0) {
                const GridPoint& prev = result.path[i - 1];
                if (prev.x == point.x && prev.y == point.y &&
                    prev.layer != point.layer) {
                    isVia = true;
                }
            }

            if (isVia) {
                cell.type = CELL_VIA;
                cell.color = Qt::black;
            } else {
                cell.type = CELL_TRACE;
                cell.color = Qt::white;
            }
            cell.traceId = result.fromPadId;
        }

        // Обновляем соединение
        for (Connection& conn : connections) {
            if (conn.fromPadId == result.fromPadId &&
                conn.toPadId == result.toPadId) {
                conn.routed = true;
                conn.layer = result.layerUsed;

                // Скрываем линию связи
                if (conn.visualLine) {
                    conn.visualLine->hide();
                }
                break;
            }
        }

        // Рисуем линии трасс
        for (int i = 0; i < result.path.size() - 1; i++) {
            drawTraceLine(result.path[i], result.path[i + 1], result.path[i].layer);
        }
    }
}
