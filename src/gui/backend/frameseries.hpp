// SPDX-License-Identifier: MIT
// FrameSeries: one line of a FrameTimeChart, x = seconds, y = milliseconds, x ascending.
// Producers (LiveSession, CaptureAnalysis) fill it from C++ and call notify() once per batch;
// the chart redraws on changed().
#pragma once

#include <QList>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QtQml/qqmlregistration.h>

class FrameSeries : public QObject
{
	Q_OBJECT
	QML_ELEMENT
	QML_UNCREATABLE("FrameSeries comes from LiveSession or CaptureAnalysis")
	Q_PROPERTY(int count READ count NOTIFY changed)
	Q_PROPERTY(double minX READ minX NOTIFY changed)
	Q_PROPERTY(double maxX READ maxX NOTIFY changed)
	Q_PROPERTY(double maxY READ maxY NOTIFY changed)
	Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)   // legend label

public:
	explicit FrameSeries(QObject *parent = nullptr);

	int count() const;
	double minX() const;   // NaN when empty
	double maxX() const;   // NaN when empty
	double maxY() const;   // NaN when empty
	QString name() const;
	void setName(const QString &name);

	const QList<QPointF> &points() const;

	// Producer side. append() expects x >= the last x (an older point is dropped). None of these
	// emit; call notify() after a batch.
	void append(double x, double y);
	void trimBefore(double x);            // drops points with x < the given x
	void setPoints(QList<QPointF> points);// sorts by x if needed
	void clear();
	void notify();                        // emits changed()

	// y of the last point with x <= the given x, NaN if none (tooltips).
	Q_INVOKABLE double valueAt(double x) const;

Q_SIGNALS:
	void changed();
	void nameChanged();

private:
	QList<QPointF> points_;
	QString name_;
	mutable double maxY_ = 0;
	mutable bool maxYDirty_ = true;
};
