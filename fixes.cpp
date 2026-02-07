// fixes.cpp
#include "multi_trace.h"
#include <QMetaObject>

// Временное исправление для подключения сигналов
void MultiThreadedRouter::connectSignals() {
    for (LayerRoutingThread* thread : layerThreads) {
        if (thread) {
            // Используем QMetaObject для надежного подключения
            QMetaObject::Connection conn1 = connect(
                thread, SIGNAL(pathFound(RoutingResult)),
                this, SLOT(onPathFoundFromThread(RoutingResult)),
                Qt::QueuedConnection
            );
            
            QMetaObject::Connection conn2 = connect(
                thread, SIGNAL(routingFailed(int, int)),
                this, SLOT(onRoutingFailedFromThread(int, int)),
                Qt::QueuedConnection
            );
            
            if (!conn1 || !conn2) {
                qDebug() << "Ошибка подключения сигналов для потока" << thread->layerId;
            }
        }
    }
}