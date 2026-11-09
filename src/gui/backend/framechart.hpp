// SPDX-License-Identifier: MIT
// FrameTimeChart: a fast line chart for frame times, drawn straight into the Qt Quick scene graph
// (QSGGeometryNode triangle strips, so lines have a real width on every RHI backend). Thousands of
// points redraw at display rate. With more points than pixel columns it keeps the min and max of
// each column, so single-frame spikes stay visible.
//
// Axis labels are drawn by QML from xTicks / yTicks and the mapping functions. API contract with
// the QML: the properties and invokables below.
#pragma once

#include <QColor>
#include <QList>
#include <QPointer>
#include <QQuickItem>
#include <QElapsedTimer>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <vector>

class FrameSeries;

class FrameTimeChart : public QQuickItem
{
	Q_OBJECT
	QML_ELEMENT
	// FrameSeries objects (a JS array of them from QML). Redraws when any of them changes.
	Q_PROPERTY(QList<QObject *> series READ series WRITE setSeries NOTIFY seriesChanged)
	// Per series, cycled. Use opaque colours: segments are separate overlapping quads, so a
	// translucent colour would show darker beads at every joint.
	Q_PROPERTY(QList<QColor> colors READ colors WRITE setColors NOTIFY colorsChanged)
	Q_PROPERTY(double lineWidth READ lineWidth WRITE setLineWidth NOTIFY lineWidthChanged) // device-independent px, default 1.5
	Q_PROPERTY(QColor gridColor READ gridColor WRITE setGridColor NOTIFY gridColorChanged)
	Q_PROPERTY(QColor referenceColor READ referenceColor WRITE setReferenceColor NOTIFY referenceColorChanged)

	// x range. follow = true: the right edge is the newest x of all series and the span is
	// windowSeconds (live view). follow = false: xMin..xMax if xMax > xMin, else all data.
	Q_PROPERTY(bool follow READ follow WRITE setFollow NOTIFY rangeInputsChanged)
	Q_PROPERTY(double windowSeconds READ windowSeconds WRITE setWindowSeconds NOTIFY rangeInputsChanged)
	Q_PROPERTY(double xMin READ xMin WRITE setXMin NOTIFY rangeInputsChanged)
	Q_PROPERTY(double xMax READ xMax WRITE setXMax NOTIFY rangeInputsChanged)
	// y range: 0..yMax ms. yMax = 0: automatic (a round number above the visible frames, ignoring
	// rare spikes; spikes above the top are drawn clipped at the top edge).
	Q_PROPERTY(double yMax READ yMax WRITE setYMax NOTIFY rangeInputsChanged)
	// Horizontal reference lines in ms (e.g. the refresh interval), drawn in referenceColor.
	Q_PROPERTY(QList<double> referenceLines READ referenceLines WRITE setReferenceLines NOTIFY referenceLinesChanged)

	// The range actually drawn, and round tick values inside it, for axis labels.
	Q_PROPERTY(double visibleXMin READ visibleXMin NOTIFY rangeChanged)
	Q_PROPERTY(double visibleXMax READ visibleXMax NOTIFY rangeChanged)
	Q_PROPERTY(double visibleYMax READ visibleYMax NOTIFY rangeChanged)
	Q_PROPERTY(QList<double> xTicks READ xTicks NOTIFY rangeChanged)
	Q_PROPERTY(QList<double> yTicks READ yTicks NOTIFY rangeChanged)

public:
	explicit FrameTimeChart(QQuickItem *parent = nullptr);
	~FrameTimeChart() override;

	QList<QObject *> series() const;
	void setSeries(const QList<QObject *> &series);
	QList<QColor> colors() const;
	void setColors(const QList<QColor> &colors);
	double lineWidth() const;
	void setLineWidth(double w);
	QColor gridColor() const;
	void setGridColor(const QColor &c);
	QColor referenceColor() const;
	void setReferenceColor(const QColor &c);
	bool follow() const;
	void setFollow(bool on);
	double windowSeconds() const;
	void setWindowSeconds(double s);
	double xMin() const;
	void setXMin(double v);
	double xMax() const;
	void setXMax(double v);
	double yMax() const;
	void setYMax(double v);
	QList<double> referenceLines() const;
	void setReferenceLines(const QList<double> &lines);

	double visibleXMin() const;
	double visibleXMax() const;
	double visibleYMax() const;
	QList<double> xTicks() const;
	QList<double> yTicks() const;

	// Mapping between data and item coordinates (for labels, tooltips and tests).
	Q_INVOKABLE double xToPixel(double x) const;
	Q_INVOKABLE double yToPixel(double ms) const;
	Q_INVOKABLE double pixelToX(double px) const;
	Q_INVOKABLE double pixelToY(double py) const;

	// Number of vertices of the last scene-graph update (tests: decimation keeps it bounded).
	int lastVertexCount() const;
	// Time the last updatePaintNode() took, in nanoseconds (tests print it for 1M points).
	qint64 lastPaintNanoseconds() const;
	// Test hook: runs updatePaintNode() without a scene graph render loop, always on the geometry
	// (GPU) path, even under the software backend (the only one on the offscreen platform), so tests
	// can rasterize the returned tree on the CPU. The caller owns the tree and passes it back in.
	QSGNode *buildSceneGraphForTest(QSGNode *old);

Q_SIGNALS:
	void seriesChanged();
	void colorsChanged();
	void lineWidthChanged();
	void gridColorChanged();
	void referenceColorChanged();
	void rangeInputsChanged();
	void referenceLinesChanged();
	void rangeChanged();

protected:
	QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
	void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
	void onSeriesChanged();
	void recomputeRange();
	double stabilizedTop(double needed);
	// Fills polyline_ with series s in item pixels (decimated, gaps marked); shared by both paths.
	void buildPolyline(const FrameSeries *s, double W, double H);
	// The Qt Quick software renderer cannot draw custom geometry: there the chart paints the same
	// lines with QPainter into an image node.
	QSGNode *updateSoftwareNode(QSGNode *oldNode, double W, double H);

	// Scratch buffers reused between frames (no per-frame allocation once warmed up).
	struct Pt { float x, y; bool brk; };   // pixel coordinates; brk = starts a new run (gap before it)
	std::vector<Pt> polyline_;
	std::vector<float> sample_;
	bool forceGeometry_ = false;     // buildSceneGraphForTest()

	QList<QPointer<FrameSeries>> series_;
	QList<QColor> colors_;
	double lineWidth_ = 1.5;
	QColor gridColor_ = QColor(128, 128, 128, 60);
	QColor referenceColor_ = QColor(128, 128, 128, 140);
	bool follow_ = true;
	double windowSeconds_ = 10;
	double xMin_ = 0, xMax_ = 0, yMax_ = 0;
	QList<double> referenceLines_;
	double visXMin_ = 0, visXMax_ = 1, visYMax_ = 50;
	QList<double> xTicks_, yTicks_;
	int lastVertexCount_ = 0;
	qint64 lastPaintNs_ = 0;
	double autoTop_ = 0;            // current automatic top; kept stable (see autoTop())
	bool shrinkPending_ = false;
	QElapsedTimer shrinkSince_;
	QTimer *shrinkTimer_ = nullptr;
};
