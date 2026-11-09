// SPDX-License-Identifier: MIT
#include "framechart.hpp"

#include "frameseries.hpp"

#include <QPainter>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QQuickWindow>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

// Hysteresis of the automatic y top: grow at once, shrink only when the needed value stayed
// below kShrinkBand x the current top for kShrinkDelayMs (live view only).
constexpr double kShrinkBand = 0.7;
constexpr int kShrinkDelayMs = 1500;
constexpr int kMaxYSamples = 16384;      // the percentile is estimated from at most this many values
constexpr double kMinGapSeconds = 0.5;   // lines break at gaps longer than this (or 10x the median spacing), unless the frame's own duration explains it
constexpr double kDashLen = 6, kDashGap = 4;

// Smallest 1/2/2.5/5 x 10^k that is >= v.
double niceAbove(double v)
{
	if (!(v > 0))
		return 1;
	const double e = std::floor(std::log10(v)) - 1;
	for (int k = 0; k < 4; ++k)
		for (double m : {1.0, 2.0, 2.5, 5.0})
		{
			const double c = m * std::pow(10.0, e + k);
			if (c >= v * (1 - 1e-12))
				return c;
		}
	return std::pow(10.0, e + 4);
}

// Round values inside [lo, hi]: the smallest 1/2/2.5/5 x 10^k step that gives at most 7 of them.
QList<double> niceTicks(double lo, double hi)
{
	QList<double> out;
	const double span = hi - lo;
	if (!(span > 0) || !std::isfinite(span))
		return out;
	const double e0 = std::floor(std::log10(span / 8));
	for (int k = 0; k < 3; ++k)
		for (double m : {1.0, 2.0, 2.5, 5.0})
		{
			const double step = m * std::pow(10.0, e0 + k);
			const double first = std::ceil(lo / step - 1e-9);
			const double last = std::floor(hi / step + 1e-9);
			if (last - first + 1 > 7)
				continue;
			for (double i = first; i <= last; i += 1)
				out << std::round(i * step * 1e9) / 1e9 + 0.0;   // strip float noise (0.30000000000000004)
			return out;
		}
	return out;
}

// Index range [lo, hi) of the points with xmin <= x <= xmax.
std::pair<qsizetype, qsizetype> visibleSlice(const QList<QPointF> &pts, double xmin, double xmax)
{
	const auto b = pts.cbegin();
	const auto lo = std::lower_bound(b, pts.cend(), xmin, [](const QPointF &p, double v) { return p.x() < v; });
	const auto hi = std::upper_bound(lo, pts.cend(), xmax, [](double v, const QPointF &p) { return v < p.x(); });
	return {lo - b, hi - b};
}

QSGGeometryNode *makeNode()
{
	auto *n = new QSGGeometryNode;
	auto *g = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0);
	g->setDrawingMode(QSGGeometry::DrawTriangles);
	n->setGeometry(g);
	n->setFlag(QSGNode::OwnsGeometry);
	auto *m = new QSGFlatColorMaterial;
	n->setMaterial(m);
	n->setFlag(QSGNode::OwnsMaterial);
	return n;
}

void setColor(QSGGeometryNode *n, const QColor &c)
{
	auto *m = static_cast<QSGFlatColorMaterial *>(n->material());
	if (m->color() != c)
	{
		m->setColor(c);
		n->markDirty(QSGNode::DirtyMaterial);
	}
}

// Copies the vertices into the node's geometry, reallocating only when the count changed.
void upload(QSGGeometryNode *n, const std::vector<QSGGeometry::Point2D> &v)
{
	QSGGeometry *g = n->geometry();
	if (g->vertexCount() != int(v.size()))
		g->allocate(int(v.size()));
	if (!v.empty())
		std::memcpy(g->vertexDataAsPoint2D(), v.data(), v.size() * sizeof(QSGGeometry::Point2D));
	n->markDirty(QSGNode::DirtyGeometry);
}

void addQuad(std::vector<QSGGeometry::Point2D> &v, float x0, float y0, float x1, float y1)
{
	QSGGeometry::Point2D a{x0, y0}, b{x1, y0}, c{x1, y1}, d{x0, y1};
	for (const auto &p : {a, b, c, a, c, d})
		v.push_back(p);
}

// Scene graph tree: root -> [grid, reference, series 0, series 1, ...].
constexpr int kFixedChildren = 2;

std::vector<QSGGeometry::Point2D> &scratchVertices()
{
	thread_local std::vector<QSGGeometry::Point2D> v;
	return v;
}

} // namespace

FrameTimeChart::FrameTimeChart(QQuickItem *parent) : QQuickItem(parent) { setFlag(ItemHasContents, true); }
FrameTimeChart::~FrameTimeChart() = default;

QList<QObject *> FrameTimeChart::series() const
{
	QList<QObject *> out;
	for (const auto &s : series_)
		if (s)
			out << s.data();
	return out;
}

void FrameTimeChart::setSeries(const QList<QObject *> &series)
{
	QList<QPointer<FrameSeries>> next;
	for (QObject *o : series)
		if (auto *s = qobject_cast<FrameSeries *>(o))
			if (!next.contains(s))
				next << s;
	if (next == series_)
		return;
	for (const auto &s : series_)
		if (s)
			disconnect(s, nullptr, this, nullptr);
	series_ = next;
	for (const auto &s : series_)
	{
		connect(s, &FrameSeries::changed, this, &FrameTimeChart::onSeriesChanged);
		connect(s, &QObject::destroyed, this, &FrameTimeChart::onSeriesChanged);
	}
	Q_EMIT seriesChanged();
	onSeriesChanged();
}

QList<QColor> FrameTimeChart::colors() const { return colors_; }
void FrameTimeChart::setColors(const QList<QColor> &colors)
{
	if (colors == colors_)
		return;
	colors_ = colors;
	Q_EMIT colorsChanged();
	update();
}

double FrameTimeChart::lineWidth() const { return lineWidth_; }
void FrameTimeChart::setLineWidth(double w)
{
	if (w == lineWidth_)
		return;
	lineWidth_ = w;
	Q_EMIT lineWidthChanged();
	update();
}

QColor FrameTimeChart::gridColor() const { return gridColor_; }
void FrameTimeChart::setGridColor(const QColor &c)
{
	if (c == gridColor_)
		return;
	gridColor_ = c;
	Q_EMIT gridColorChanged();
	update();
}

QColor FrameTimeChart::referenceColor() const { return referenceColor_; }
void FrameTimeChart::setReferenceColor(const QColor &c)
{
	if (c == referenceColor_)
		return;
	referenceColor_ = c;
	Q_EMIT referenceColorChanged();
	update();
}

bool FrameTimeChart::follow() const { return follow_; }
double FrameTimeChart::windowSeconds() const { return windowSeconds_; }
double FrameTimeChart::xMin() const { return xMin_; }
double FrameTimeChart::xMax() const { return xMax_; }
double FrameTimeChart::yMax() const { return yMax_; }

#define PML_RANGE_INPUT(setter, member, type) \
	void FrameTimeChart::setter(type v) \
	{ \
		if (v == member) \
			return; \
		member = v; \
		Q_EMIT rangeInputsChanged(); \
		recomputeRange(); \
		update(); \
	}
PML_RANGE_INPUT(setFollow, follow_, bool)
PML_RANGE_INPUT(setWindowSeconds, windowSeconds_, double)
PML_RANGE_INPUT(setXMin, xMin_, double)
PML_RANGE_INPUT(setXMax, xMax_, double)
PML_RANGE_INPUT(setYMax, yMax_, double)
#undef PML_RANGE_INPUT

QList<double> FrameTimeChart::referenceLines() const { return referenceLines_; }
void FrameTimeChart::setReferenceLines(const QList<double> &lines)
{
	if (lines == referenceLines_)
		return;
	referenceLines_ = lines;
	Q_EMIT referenceLinesChanged();
	update();
}

double FrameTimeChart::visibleXMin() const { return visXMin_; }
double FrameTimeChart::visibleXMax() const { return visXMax_; }
double FrameTimeChart::visibleYMax() const { return visYMax_; }
QList<double> FrameTimeChart::xTicks() const { return xTicks_; }
QList<double> FrameTimeChart::yTicks() const { return yTicks_; }

double FrameTimeChart::xToPixel(double x) const
{
	const double span = visXMax_ - visXMin_;
	return span > 0 ? (x - visXMin_) / span * width() : 0;
}
double FrameTimeChart::yToPixel(double ms) const
{
	return visYMax_ > 0 ? height() * (1 - ms / visYMax_) : height();
}
double FrameTimeChart::pixelToX(double px) const
{
	return width() > 0 ? visXMin_ + px / width() * (visXMax_ - visXMin_) : visXMin_;
}
double FrameTimeChart::pixelToY(double py) const
{
	return height() > 0 ? (1 - py / height()) * visYMax_ : 0;
}

int FrameTimeChart::lastVertexCount() const { return lastVertexCount_; }
qint64 FrameTimeChart::lastPaintNanoseconds() const { return lastPaintNs_; }

QSGNode *FrameTimeChart::buildSceneGraphForTest(QSGNode *old)
{
	forceGeometry_ = true;
	QSGNode *root = updatePaintNode(old, nullptr);
	forceGeometry_ = false;
	return root;
}

void FrameTimeChart::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
	QQuickItem::geometryChange(newGeometry, oldGeometry);
	update();
}

void FrameTimeChart::onSeriesChanged()
{
	recomputeRange();
	update();
}

// A y top that follows the data but does not jitter: grows at once; shrinks (live view) only
// after the needed value stayed clearly lower for a while; immediately for a static view.
double FrameTimeChart::stabilizedTop(double needed)
{
	if (autoTop_ <= 0 || needed > autoTop_)
	{
		autoTop_ = needed;
		shrinkPending_ = false;
	}
	else if (needed < autoTop_ * kShrinkBand)
	{
		if (!follow_)
		{
			autoTop_ = needed;
			shrinkPending_ = false;
		}
		else if (!shrinkPending_)
		{
			shrinkPending_ = true;
			shrinkSince_.start();
			if (!shrinkTimer_)
			{
				shrinkTimer_ = new QTimer(this);
				shrinkTimer_->setSingleShot(true);
				connect(shrinkTimer_, &QTimer::timeout, this, [this] { recomputeRange(); update(); });
			}
			shrinkTimer_->start(kShrinkDelayMs + 50);
		}
		else if (shrinkSince_.elapsed() >= kShrinkDelayMs)
		{
			autoTop_ = needed;
			shrinkPending_ = false;
		}
	}
	else
		shrinkPending_ = false;
	return autoTop_;
}

void FrameTimeChart::recomputeRange()
{
	// x range
	double x0 = 0, x1 = 1;
	double dataMin = std::numeric_limits<double>::infinity(), dataMax = -dataMin;
	for (const auto &s : series_)
		if (s && s->count() > 0)
		{
			dataMin = std::min(dataMin, s->minX());
			dataMax = std::max(dataMax, s->maxX());
		}
	const bool haveData = dataMin <= dataMax;
	if (follow_)
	{
		const double w = windowSeconds_ > 0 ? windowSeconds_ : 10;
		x1 = haveData ? dataMax : w;
		x0 = x1 - w;
	}
	else if (xMax_ > xMin_)
	{
		x0 = xMin_;
		x1 = xMax_;
	}
	else if (haveData)
	{
		x0 = dataMin;
		x1 = dataMax > dataMin ? dataMax : dataMin + 1;
	}

	// y range
	double top;
	if (yMax_ > 0)
	{
		top = yMax_;
		autoTop_ = 0;
		shrinkPending_ = false;
	}
	else
	{
		// Pool (a stride sample of) the visible y values of all series.
		qsizetype total = 0;
		for (const auto &s : series_)
			if (s)
			{
				const auto r = visibleSlice(s->points(), x0, x1);
				total += r.second - r.first;
			}
		sample_.clear();
		if (total > 0)
		{
			const qsizetype stride = std::max<qsizetype>(1, total / kMaxYSamples);
			sample_.reserve(size_t(total / stride + series_.size() + 1));
			for (const auto &s : series_)
				if (s)
				{
					const QList<QPointF> &pts = s->points();
					const auto r = visibleSlice(pts, x0, x1);
					for (qsizetype i = r.first; i < r.second; i += stride)
					{
						const double y = pts[i].y();
						if (std::isfinite(y) && y >= 0)
							sample_.push_back(float(y));
					}
				}
		}
		if (sample_.empty())
			top = autoTop_ > 0 ? autoTop_ : 50;
		else
		{
			const size_t n = sample_.size();
			const size_t mid = n / 2;
			std::nth_element(sample_.begin(), sample_.begin() + mid, sample_.end());
			const double median = sample_[mid];
			const size_t hiIdx = std::max(mid, size_t(0.995 * double(n - 1)));
			if (hiIdx > mid)
				std::nth_element(sample_.begin() + mid, sample_.begin() + hiIdx, sample_.end());
			const double p995 = sample_[hiIdx];
			top = stabilizedTop(niceAbove(std::max({p995 * 1.1, median * 1.5, 1.0})));
		}
	}

	const QList<double> xt = niceTicks(x0, x1);
	const QList<double> yt = niceTicks(0, top);
	const bool changed = x0 != visXMin_ || x1 != visXMax_ || top != visYMax_ || xt != xTicks_ || yt != yTicks_;
	visXMin_ = x0;
	visXMax_ = x1;
	visYMax_ = top;
	xTicks_ = xt;
	yTicks_ = yt;
	if (changed)
		Q_EMIT rangeChanged();
}

QSGNode *FrameTimeChart::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
	QElapsedTimer timer;
	timer.start();

	const double W = width(), H = height();
	if (!(W > 0) || !(H > 0))
	{
		delete oldNode;
		lastVertexCount_ = 0;
		return nullptr;
	}
	if (!forceGeometry_ && window() && window()->rendererInterface()->graphicsApi() == QSGRendererInterface::Software)
		return updateSoftwareNode(oldNode, W, H);
	const double dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;

	QSGNode *root = oldNode;
	QSGGeometryNode *grid, *ref;
	if (!root)
	{
		root = new QSGNode;
		root->appendChildNode(grid = makeNode());
		root->appendChildNode(ref = makeNode());
	}
	else
	{
		grid = static_cast<QSGGeometryNode *>(root->childAtIndex(0));
		ref = static_cast<QSGGeometryNode *>(root->childAtIndex(1));
	}
	int vertices = 0;
	auto &verts = scratchVertices();

	// Grid: 1 device px lines snapped to the pixel grid so they stay crisp.
	const double px = 1.0 / dpr;
	auto snap = [&](double y) { return std::floor(y * dpr) / dpr; };
	verts.clear();
	for (double t : yTicks_)
	{
		const double y = std::clamp(snap(yToPixel(t)), 0.0, std::max(0.0, H - px));
		addQuad(verts, 0, float(y), float(W), float(y + px));
	}
	upload(grid, verts);
	setColor(grid, gridColor_);
	vertices += int(verts.size());

	// Reference lines: dashed.
	verts.clear();
	for (double t : referenceLines_)
	{
		if (!(t >= 0 && t <= visYMax_))
			continue;
		const double y = std::clamp(snap(yToPixel(t)), 0.0, std::max(0.0, H - px));
		for (double x = 0; x < W; x += kDashLen + kDashGap)
			addQuad(verts, float(x), float(y), float(std::min(W, x + kDashLen)), float(y + px));
	}
	upload(ref, verts);
	setColor(ref, referenceColor_);
	vertices += int(verts.size());

	// Series nodes: add/remove at the end only.
	static const QList<QColor> defaults = {QColor(0x4d, 0xab, 0xf7), QColor(0xff, 0x92, 0x2b), QColor(0x51, 0xcf, 0x66),
	                                       QColor(0xf0, 0x65, 0x95), QColor(0xb1, 0x97, 0xfc)};
	const QList<QColor> &palette = colors_.isEmpty() ? defaults : colors_;
	const int nSeries = int(series_.size());
	while (root->childCount() - kFixedChildren > nSeries)
		delete root->lastChild();   // QSGNode destructor detaches it from the parent
	while (root->childCount() - kFixedChildren < nSeries)
		root->appendChildNode(makeNode());

	const float half = float(std::max(0.5, lineWidth_) / 2);

	for (int si = 0; si < nSeries; ++si)
	{
		auto *node = static_cast<QSGGeometryNode *>(root->childAtIndex(kFixedChildren + si));
		setColor(node, palette[si % palette.size()]);
		verts.clear();
		buildPolyline(series_[si], W, H);

		// Polyline -> triangles. One quad per segment, extended by half the width at both ends so
		// corners are filled without joins (robust for spikes); a run of one point is a dot.
		const size_t n = polyline_.size();
		for (size_t i = 0; i < n;)
		{
			size_t j = i + 1;
			while (j < n && !polyline_[j].brk)
				++j;
			if (j - i == 1)
				addQuad(verts, std::max(0.0f, polyline_[i].x - half), std::max(0.0f, polyline_[i].y - half),
				        std::min(float(W), polyline_[i].x + half), std::min(float(H), polyline_[i].y + half));
			else
				for (size_t k = i; k + 1 < j; ++k)
				{
					const Pt &a = polyline_[k], &b = polyline_[k + 1];
					const float dx = b.x - a.x, dy = b.y - a.y;
					const float len = std::sqrt(dx * dx + dy * dy);
					if (len < 1e-4f)
						continue;
					const float ux = dx / len * half, uy = dy / len * half;
					const float nx = -uy, ny = ux;
					const float ax = a.x - ux, ay = a.y - uy, bx = b.x + ux, by = b.y + uy;
					// Keep every vertex inside the item: no clip is set, so the line would otherwise
					// paint over neighbouring axis labels at the edges.
					auto cx = [&](float v) { return std::clamp(v, 0.0f, float(W)); };
					auto cy = [&](float v) { return std::clamp(v, 0.0f, float(H)); };
					const QSGGeometry::Point2D q[4] = {{cx(ax + nx), cy(ay + ny)}, {cx(ax - nx), cy(ay - ny)}, {cx(bx + nx), cy(by + ny)}, {cx(bx - nx), cy(by - ny)}};
					for (int idx : {0, 1, 2, 2, 1, 3})
						verts.push_back(q[idx]);
				}
			i = j;
		}
		upload(node, verts);
		vertices += int(verts.size());
	}

	lastVertexCount_ = vertices;
	lastPaintNs_ = timer.nsecsElapsed();
	return root;
}

void FrameTimeChart::buildPolyline(const FrameSeries *s, double W, double H)
{
	polyline_.clear();
	const double x0 = visXMin_, x1 = visXMax_;
	const double sx = (x1 > x0) ? W / (x1 - x0) : 0;
	const double sy = visYMax_ > 0 ? H / visYMax_ : 0;
	const int cols = std::max(1, int(std::ceil(W)));
	const double colSeconds = (x1 - x0) / cols;
	{
		if (s && s->count() > 0 && sx > 0)
		{
			const QList<QPointF> &pts = s->points();
			const auto [lo, hi] = visibleSlice(pts, x0, x1);

			// Gap threshold: kMinGapSeconds, 10x the median spacing (sparse series), and a couple of
			// pixel columns (a zoomed-out view has long spacing between decimated points).
			double gap = kMinGapSeconds;
			if (hi - lo > 2)
			{
				sample_.clear();
				const qsizetype stride = std::max<qsizetype>(1, (hi - lo) / 512);
				for (qsizetype i = lo; i + 1 < hi; i += stride)
					sample_.push_back(float(pts[i + 1].x() - pts[i].x()));
				std::nth_element(sample_.begin(), sample_.begin() + sample_.size() / 2, sample_.end());
				gap = std::max(gap, 10.0 * double(sample_[sample_.size() / 2]));
			}
			gap = std::max(gap, 2.5 * colSeconds);

			double lastX = 0;
			bool have = false;
			auto push = [&](double x, double y) {
				if (!std::isfinite(y))
					return;
				// A frame-time sample sits at the END of its frame, so a hitch of d ms is d/1000 s
				// away from the previous point: that distance is explained by the sample's own value
				// and must stay connected (a stall is the most important data here). Only gaps the
				// frame does not explain (no data, paused capture) break the line.
				const double dx = x - lastX;
				const bool brk = !have || (dx > gap && dx > y * 1.5e-3);
				lastX = x;
				have = true;
				polyline_.push_back({float((x - x0) * sx), float(std::clamp(H - y * sy, 0.0, H)), brk});
			};
			// Neighbours outside the range continue the line to the edge (interpolated), so the
			// line does not stop short and never leaves the item.
			auto edge = [&](const QPointF &a, const QPointF &b, double xe) {
				return a.y() + (b.y() - a.y()) * (xe - a.x()) / (b.x() - a.x());
			};
			const bool hasL = lo > 0, hasR = hi < pts.size();
			const QPointF *first = lo < hi ? &pts[lo] : (hasR ? &pts[hi] : nullptr);
			const QPointF *last = lo < hi ? &pts[hi - 1] : (hasL ? &pts[lo - 1] : nullptr);
			if (hasL && first && first->x() > pts[lo - 1].x() && first->x() - pts[lo - 1].x() <= gap)
				push(x0, edge(pts[lo - 1], *first, x0));

			if (hi - lo <= qsizetype(2) * cols)
			{
				for (qsizetype i = lo; i < hi; ++i)
					push(pts[i].x(), pts[i].y());
			}
			else
			{
				// Min and max of each pixel column, in time order: a one-frame spike stays visible.
				const double scale = cols / (x1 - x0);
				int cur = -1;
				qsizetype iMin = 0, iMax = 0;
				double yMin = 0, yMax = 0;
				auto flush = [&] {
					if (cur < 0)
						return;
					if (iMin == iMax)
						push(pts[iMin].x(), yMin);
					else if (iMin < iMax)
					{
						push(pts[iMin].x(), yMin);
						push(pts[iMax].x(), yMax);
					}
					else
					{
						push(pts[iMax].x(), yMax);
						push(pts[iMin].x(), yMin);
					}
				};
				for (qsizetype i = lo; i < hi; ++i)
				{
					const double y = pts[i].y();
					if (!std::isfinite(y))
						continue;
					const int col = std::min(cols - 1, int((pts[i].x() - x0) * scale));
					if (col != cur)
					{
						flush();
						cur = col;
						iMin = iMax = i;
						yMin = yMax = y;
					}
					else
					{
						if (y < yMin) { yMin = y; iMin = i; }
						if (y > yMax) { yMax = y; iMax = i; }
					}
				}
				flush();
			}
			if (hasR && last && pts[hi].x() > last->x() && pts[hi].x() - last->x() <= gap)
				push(x1, edge(*last, pts[hi], x1));
		}
	}
}

QSGNode *FrameTimeChart::updateSoftwareNode(QSGNode *oldNode, double W, double H)
{
	QElapsedTimer timer;
	timer.start();
	const double dpr = window()->effectiveDevicePixelRatio();
	QImage image(QSize(int(std::ceil(W * dpr)), int(std::ceil(H * dpr))), QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(dpr);
	image.fill(Qt::transparent);
	int drawn = 0;
	{
		QPainter p(&image);
		// Grid and dashed reference lines: 1 device px, snapped like the geometry path.
		const double px = 1.0 / dpr;
		auto snap = [&](double y) { return std::clamp(std::floor(yToPixel(y) * dpr) / dpr, 0.0, std::max(0.0, H - px)); };
		for (double t : yTicks_)
			p.fillRect(QRectF(0, snap(t), W, px), gridColor_);
		for (double t : referenceLines_)
		{
			if (!(t >= 0 && t <= visYMax_))
				continue;
			const double y = snap(t);
			for (double x = 0; x < W; x += kDashLen + kDashGap)
				p.fillRect(QRectF(x, y, std::min(W, x + kDashLen) - x, px), referenceColor_);
		}
		drawn += int(yTicks_.size() + referenceLines_.size());

		static const QList<QColor> defaults = {QColor(0x4d, 0xab, 0xf7), QColor(0xff, 0x92, 0x2b), QColor(0x51, 0xcf, 0x66),
		                                       QColor(0xf0, 0x65, 0x95), QColor(0xb1, 0x97, 0xfc)};
		const QList<QColor> &palette = colors_.isEmpty() ? defaults : colors_;
		p.setRenderHint(QPainter::Antialiasing);
		p.setClipRect(QRectF(0, 0, W, H));   // like the geometry path: never paint outside the item
		for (int si = 0; si < int(series_.size()); ++si)
		{
			buildPolyline(series_[si], W, H);
			QPen pen(palette[si % palette.size()], std::max(0.5, lineWidth_));
			pen.setCapStyle(Qt::RoundCap);
			pen.setJoinStyle(Qt::RoundJoin);
			p.setPen(pen);
			const size_t n = polyline_.size();
			QList<QPointF> run;
			for (size_t i = 0; i < n;)
			{
				size_t j = i + 1;
				while (j < n && !polyline_[j].brk)
					++j;
				run.clear();
				for (size_t k = i; k < j; ++k)
					run.append(QPointF(polyline_[k].x, polyline_[k].y));
				if (run.size() == 1)
					p.drawPoint(run.front());
				else
					p.drawPolyline(run.constData(), int(run.size()));
				i = j;
			}
			drawn += int(n);
		}
	}

	// The node is the image itself, with a new texture each frame. The node owns its texture, and
	// setTexture() deletes the previous one (checked on Qt 6.12; deleting it here as well was a
	// double free from the second frame on).
	auto *node = static_cast<QSGImageNode *>(oldNode);
	if (!node)
	{
		node = window()->createImageNode();
		node->setOwnsTexture(true);
		node->setFiltering(QSGTexture::Linear);
	}
	node->setTexture(window()->createTextureFromImage(image));
	node->setRect(QRectF(0, 0, W, H));
	node->markDirty(QSGNode::DirtyMaterial | QSGNode::DirtyGeometry);

	lastVertexCount_ = drawn;
	lastPaintNs_ = timer.nsecsElapsed();
	return node;
}
